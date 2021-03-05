#include "cypress_integration.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>

#include <yt/yt/server/master/cypress_server/node.h>
#include <yt/yt/server/master/cypress_server/virtual.h>

#include <yt/yt/server/master/cell_master/bootstrap.h>

#include <yt/yt/ytlib/orchid/orchid_service_proxy.h>
#include <yt/yt/ytlib/orchid/private.h>

#include <yt/yt/ytlib/node_tracker_client/channel.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/core/rpc/bus/channel.h>
#include <yt/yt/core/rpc/caching_channel_factory.h>
#include <yt/yt/core/rpc/retrying_channel.h>
#include <yt/yt/core/rpc/balancing_channel.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NOrchid {

using namespace NRpc;
using namespace NYT::NBus;
using namespace NYTree;
using namespace NYson;
using namespace NHydra;
using namespace NCypressServer;
using namespace NObjectServer;
using namespace NCellMaster;
using namespace NOrchid::NProto;
using namespace NNodeTrackerClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = OrchidLogger;

////////////////////////////////////////////////////////////////////////////////

class TOrchidYPathService
    : public IYPathService
{
public:
    TOrchidYPathService(INodeChannelFactoryPtr channelFactory, INodePtr owningProxy)
        : ChannelFactory_(std::move(channelFactory))
        , OwningNode_(std::move(owningProxy))
    { }

    virtual TResolveResult Resolve(const TYPath& path, const IServiceContextPtr& /*context*/) override
    {
        return TResolveResultHere{path};
    }

    virtual void Invoke(const IServiceContextPtr& context) override
    {
        if (IsRequestMutating(context->RequestHeader())) {
            THROW_ERROR_EXCEPTION("Orchid nodes are read-only");
        }

        auto manifest = LoadManifest();

        auto channel = CreateChannel(manifest);

        TOrchidServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(manifest->Timeout);

        auto path = GetRedirectPath(manifest, GetRequestTargetYPath(context->RequestHeader()));
        const auto& method = context->GetMethod();

        auto requestMessage = context->GetRequestMessage();
        NRpc::NProto::TRequestHeader requestHeader;
        if (!ParseRequestHeader(requestMessage, &requestHeader)) {
            context->Reply(TError("Error parsing request header"));
            return;
        }

        SetRequestTargetYPath(&requestHeader, path);

        auto innerRequestMessage = SetRequestHeader(requestMessage, requestHeader);

        auto outerRequest = proxy.Execute();
        outerRequest->SetMultiplexingBand(EMultiplexingBand::Heavy);
        outerRequest->Attachments() = innerRequestMessage.ToVector();

        YT_LOG_DEBUG("Sending request to remote Orchid (Path: %v, Method: %v, RequestId: %v)",
            path,
            method,
            outerRequest->GetRequestId());

        outerRequest->Invoke().Subscribe(BIND(
            &TOrchidYPathService::OnResponse,
            context,
            manifest,
            path,
            method));
    }

    virtual void DoWriteAttributesFragment(
        IAsyncYsonConsumer* /*consumer*/,
        const std::optional<std::vector<TString>>& /*attributeKeys*/,
        bool /*stable*/) override
    {
        YT_ABORT();
    }

    virtual bool ShouldHideAttributes() override
    {
        YT_ABORT();
    }

private:
    const INodeChannelFactoryPtr ChannelFactory_;
    const INodePtr OwningNode_;


    TOrchidManifestPtr LoadManifest()
    {
        auto manifest = New<TOrchidManifest>();
        auto manifestNode = ConvertToNode(OwningNode_->Attributes());
        try {
            manifest->Load(manifestNode);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing Orchid manifest")
                << ex;
        }
        return manifest;
    }

    IChannelPtr CreateChannel(const TOrchidManifestPtr& manifest)
    {
        switch (manifest->RemoteAddresses->GetType()) {
            case ENodeType::Map:
                return CreateRetryingChannel(
                    manifest,
                    ChannelFactory_->CreateChannel(ConvertTo<TAddressMap>(manifest->RemoteAddresses)));

            case ENodeType::List: {
                auto channelConfig = New<TBalancingChannelConfig>();
                channelConfig->Addresses = ConvertTo<std::vector<TString>>(manifest->RemoteAddresses);
                auto endpointDescription = TString("Orchid@");
                auto endpointAttributes = ConvertToAttributes(BuildYsonStringFluently()
                    .BeginMap()
                        .Item("orchid").Value(true)
                    .EndMap());
                return CreateRetryingChannel(
                    manifest,
                    CreateBalancingChannel(
                        std::move(channelConfig),
                        ChannelFactory_,
                        std::move(endpointDescription),
                        std::move(endpointAttributes)));
            }

            default:
                YT_ABORT();
        }
    }

    static void OnResponse(
        const IServiceContextPtr& context,
        const TOrchidManifestPtr& manifest,
        const TYPath& path,
        const TString& method,
        const TOrchidServiceProxy::TErrorOrRspExecutePtr& rspOrError)
    {
        if (rspOrError.IsOK()) {
            YT_LOG_DEBUG("Orchid request succeeded");
            const auto& rsp = rspOrError.Value();
            auto innerResponseMessage = TSharedRefArray(rsp->Attachments(), TSharedRefArray::TMoveParts{});
            context->Reply(std::move(innerResponseMessage));
        } else {
            context->Reply(TError("Error executing Orchid request")
                << TErrorAttribute("path", path)
                << TErrorAttribute("method", method)
                << TErrorAttribute("remote_addresses", manifest->RemoteAddresses)
                << TErrorAttribute("remote_root", manifest->RemoteRoot)
                << rspOrError);
        }
    }

    static TString GetRedirectPath(const TOrchidManifestPtr& manifest, const TYPath& path)
    {
        return manifest->RemoteRoot + path;
    }
};

INodeTypeHandlerPtr CreateOrchidTypeHandler(NCellMaster::TBootstrap* bootstrap)
{
    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::Orchid,
        BIND([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TOrchidYPathService>(bootstrap->GetNodeChannelFactory(), owningNode);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NOrchid
