#include "discovery_service.h"
#include "proxy_coordinator.h"
#include "config.h"
#include "public.h"
#include "private.h"
#include "bootstrap.h"

#include <yt/ytlib/api/native/client.h>
#include <yt/ytlib/api/native/connection.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/client/api/rpc_proxy/discovery_service_proxy.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/net/address.h>
#include <yt/core/net/local_address.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/helpers.h>

#include <yt/core/utilex/random.h>

#include <yt/core/rpc/service_detail.h>

#include <yt/build/build.h>

namespace NYT::NRpcProxy {

using namespace NApi;
using namespace NApi::NRpcProxy;
using namespace NYson;
using namespace NYTree;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NRpc;
using namespace NNet;
using namespace NNodeTrackerClient;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const TString ExpirationTimeAttributeName = "expiration_time";
static const TString VersionAttributeName = "version";
static const TString StartTimeAttributeName = "start_time";

////////////////////////////////////////////////////////////////////////////////

namespace {

const TServiceDescriptor& GetDescriptor()
{
    static const auto descriptor = TServiceDescriptor(DiscoveryServiceName)
        .SetProtocolVersion({0, 0});
    return descriptor;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TDiscoveryService
    : public TServiceBase
{
public:
    TDiscoveryService(
        TBootstrap* bootstrap)
        : TServiceBase(
            bootstrap->GetWorkerInvoker(),
            GetDescriptor(),
            RpcProxyLogger)
        , Bootstrap_(bootstrap)
        , Config_(bootstrap->GetConfig()->DiscoveryService)
        , Coordinator_(bootstrap->GetProxyCoordinator())
        , RootClient_(bootstrap->GetNativeClient())
        , ProxyPath_(RpcProxiesPath + "/" + BuildServiceAddress(
            GetLocalHostName(),
            Bootstrap_->GetConfig()->RpcPort))
        , AliveUpdateExecutor_(New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TDiscoveryService::OnPeriodicEvent, MakeWeak(this), &TDiscoveryService::UpdateLiveness),
            TPeriodicExecutorOptions::WithJitter(Config_->LivenessUpdatePeriod)))
        , ProxyUpdateExecutor_(New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TDiscoveryService::OnPeriodicEvent, MakeWeak(this), &TDiscoveryService::UpdateProxies),
            TPeriodicExecutorOptions::WithJitter(Config_->ProxyUpdatePeriod)))
    {
        Initialize();

        if (Bootstrap_->GetConfig()->GrpcServer) {
            const auto& addresses = Bootstrap_->GetConfig()->GrpcServer->Addresses;
            YT_VERIFY(addresses.size() == 1);

            int port;
            ParseServiceAddress(addresses[0]->Address, nullptr, &port);

            GrpcProxyPath_ = GrpcProxiesPath + "/" + BuildServiceAddress(GetLocalHostName(), port);
        }

        RegisterMethod(RPC_SERVICE_METHOD_DESC(DiscoverProxies));
    }

private:
    const TBootstrap* Bootstrap_;

    const TDiscoveryServiceConfigPtr Config_;
    const IProxyCoordinatorPtr Coordinator_;
    const NNative::IClientPtr RootClient_;
    const TString ProxyPath_;
    const TPeriodicExecutorPtr AliveUpdateExecutor_;
    const TPeriodicExecutorPtr ProxyUpdateExecutor_;

    std::optional<TString> GrpcProxyPath_;

    TInstant LastSuccessTimestamp_ = Now();

    TSpinLock ProxySpinLock_;

    struct TProxy
    {
        TString Address;
        TString Role;
    };

    std::vector<TProxy> AvailableProxies_;

    bool Initialized_ = false;


    void Initialize()
    {
        AliveUpdateExecutor_->Start();
        ProxyUpdateExecutor_->Start();
    }

    std::vector<TString> GetCypressPaths() const
    {
        std::vector<TString> paths = {ProxyPath_};
        if (GrpcProxyPath_) {
            paths.push_back(*GrpcProxyPath_);
        }
        return paths;
    }

    void CreateProxyNode()
    {
        auto channel = RootClient_->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        for (const auto& path : GetCypressPaths()) {
            {
                auto req = TCypressYPathProxy::Create(path);
                req->set_type(static_cast<int>(EObjectType::MapNode));
                req->set_recursive(true);
                req->set_ignore_existing(true);
                GenerateMutationId(req);
                batchReq->AddRequest(req);
            }
            {
                auto req = TYPathProxy::Set(path + "/@" + VersionAttributeName);
                req->set_value(ConvertToYsonString(GetVersion()).GetData());
                GenerateMutationId(req);
                batchReq->AddRequest(req);
            }
            {
                auto req = TYPathProxy::Set(path + "/@" + StartTimeAttributeName);
                req->set_value(ConvertToYsonString(TInstant::Now().ToString()).GetData());
                GenerateMutationId(req);
                batchReq->AddRequest(req);
            }
            {
                auto req = TCypressYPathProxy::Set(path + "/@annotations");
                req->set_value(ConvertToYsonString(Bootstrap_->GetConfig()->CypressAnnotations).GetData());
                GenerateMutationId(req);
                batchReq->AddRequest(req);
            }
            {
                auto req = TCypressYPathProxy::Create(path + "/orchid");
                req->set_ignore_existing(true);
                req->set_type(static_cast<int>(EObjectType::Orchid));
                auto attributes = CreateEphemeralAttributes();
                attributes->Set("remote_addresses", Bootstrap_->GetLocalAddresses());
                ToProto(req->mutable_node_attributes(), *attributes);
                batchReq->AddRequest(req);
            }
        }

        batchReq->SetTimeout(Config_->LivenessUpdatePeriod);
        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(
            GetCumulativeError(batchRspOrError),
            "Error creating proxy node %Qv",
            ProxyPath_);

        YT_LOG_INFO("Proxy node created (Path: %v)", ProxyPath_);
    }

    bool IsAvailable() const
    {
        return Now() - LastSuccessTimestamp_ < Config_->AvailabilityPeriod;
    }

    void OnPeriodicEvent(void (TDiscoveryService::*action)())
    {
        TDuration backoffDuration;
        while (true) {
            try {
                (this->*action)();
                return;
            } catch (const std::exception& ex) {
                backoffDuration = Min(backoffDuration + RandomDuration(Max(backoffDuration, Config_->LivenessUpdatePeriod)),
                    Config_->BackoffPeriod);
                YT_LOG_WARNING(ex, "Failed to perform update, backing off (Duration: %v)", backoffDuration);
                if (!IsAvailable() && Coordinator_->SetAvailableState(false)) {
                    Initialized_ = false;
                    YT_LOG_WARNING("Connectivity lost");
                }
                TDelayedExecutor::WaitForDuration(backoffDuration);
            }
        }
    }

    void UpdateLiveness()
    {
        if (!Initialized_) {
            CreateProxyNode();
            Initialized_ = true;
        }

        auto channel = RootClient_->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto& path : GetCypressPaths()) {
            auto req = TCypressYPathProxy::Create(path + "/" + AliveNodeName);
            req->set_type(static_cast<int>(EObjectType::MapNode));
            auto* attr = req->mutable_node_attributes()->add_attributes();
            attr->set_key(ExpirationTimeAttributeName);
            attr->set_value(ConvertToYsonString(TInstant::Now() + Config_->AvailabilityPeriod).GetData());
            req->set_force(true);
            batchReq->AddRequest(req);
        }

        batchReq->SetTimeout(Config_->LivenessUpdatePeriod);
        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error updating proxy liveness");

        LastSuccessTimestamp_ = Now();
        if (Coordinator_->SetAvailableState(true)) {
            YT_LOG_INFO("Connectivity restored");
        }
    }

    void UpdateProxies()
    {
        auto channel = RootClient_->GetMasterChannelOrThrow(EMasterChannelKind::Cache);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        {
            auto req = TYPathProxy::Get(ProxyPath_ + "/@");
            ToProto(
                req->mutable_attributes()->mutable_keys(),
                std::vector<TString>{
                    RoleAttributeName,
                    BannedAttributeName,
                    BanMessageAttributeName,
                });
            batchReq->AddRequest(req, "get_ban");
        }
        {
            auto req = TYPathProxy::Get(RpcProxiesPath);
            ToProto(
                req->mutable_attributes()->mutable_keys(),
                std::vector<TString>{
                    RoleAttributeName,
                    BannedAttributeName,
                    ConfigAttributeName,
                });

            auto* cachingHeaderExt = req->Header().MutableExtension(NYTree::NProto::TCachingHeaderExt::caching_header_ext);
            cachingHeaderExt->set_success_expiration_time(ToProto<i64>(Config_->ProxyUpdatePeriod));
            cachingHeaderExt->set_failure_expiration_time(ToProto<i64>(Config_->ProxyUpdatePeriod));

            auto* balancingHeaderExt = req->Header().MutableExtension(NRpc::NProto::TBalancingExt::balancing_ext);
            balancingHeaderExt->set_enable_stickness(true);
            balancingHeaderExt->set_sticky_group_size(1);

            batchReq->AddRequest(req, "get_proxies");
        }

        batchReq->SetTimeout(Config_->ProxyUpdatePeriod);
        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting states of proxies");
        const auto& batchRsp = batchRspOrError.Value();

        {
            auto rsp = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_ban").Value();
            auto attributes = ConvertToAttributes(TYsonString(rsp->value()));
            bool banned = attributes->Get(BannedAttributeName, false);
            bool changed = Coordinator_->SetBannedState(banned);
            if (changed) {
                if (banned) {
                    Coordinator_->SetBanMessage(attributes->Get(BanMessageAttributeName, TString()));
                }
                YT_LOG_INFO("Proxy has been %v (Path: %v)", banned ? "banned" : "unbanned", ProxyPath_);
            }
        }
        {
            std::vector<TProxy> proxies;

            auto rsp = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_proxies").Value();
            auto nodeResult = ConvertToNode(TYsonString(rsp->value()));

            if (auto dynamicConfig = nodeResult->Attributes().Find<TDynamicConfigPtr>(ConfigAttributeName)) {
                Coordinator_->SetDynamicConfig(dynamicConfig);
            }

            for (const auto& child : nodeResult->AsMap()->GetChildren()) {
                bool banned = child.second->Attributes().Get(BannedAttributeName, false);
                auto role = child.second->Attributes().Get<TString>(RoleAttributeName, DefaultProxyRole);
                bool alive = static_cast<bool>(child.second->AsMap()->FindChild(AliveNodeName));
                bool available = alive && !banned;
                if (available) {
                    proxies.push_back({child.first, role});
                }
            }
            YT_LOG_DEBUG("Updated proxy list (ProxyCount: %v)", proxies.size());

            {
                auto guard = Guard(ProxySpinLock_);
                AvailableProxies_ = std::move(proxies);
            }
        }
    }

    DECLARE_RPC_SERVICE_METHOD(NApi::NRpcProxy::NProto, DiscoverProxies)
    {
        Coordinator_->ValidateOperable();

        auto roleFilter = request->has_role() ? request->role() : DefaultProxyRole;

        context->SetRequestInfo("Role: %v", roleFilter);

        {
            auto guard = Guard(ProxySpinLock_);
            for (const auto& proxy : AvailableProxies_) {
                if (proxy.Role == roleFilter) {
                    *response->mutable_addresses()->Add() = proxy.Address;
                }
            }
        }

        context->SetResponseInfo("ProxyCount: %v", response->addresses_size());
        context->Reply();
    }
};

////////////////////////////////////////////////////////////////////////////////

IServicePtr CreateDiscoveryService(
    TBootstrap* bootstrap)
{
    return New<TDiscoveryService>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpcProxy
