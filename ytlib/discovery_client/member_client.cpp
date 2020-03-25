#include "member_client.h"
#include "private.h"
#include "request_session.h"

#include <yt/core/actions/future.h>

#include <yt/core/ytree/attributes.h>
#include <yt/core/ytree/fluent.h>

#include <yt/core/rpc/caching_channel_factory.h>

#include <yt/core/concurrency/rw_spinlock.h>
#include <yt/core/concurrency/periodic_executor.h>

namespace NYT::NDiscoveryClient {

using namespace NConcurrency;
using namespace NRpc;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TMemberClient::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TMemberClientConfigPtr config,
        IChannelFactoryPtr channelFactory,
        IInvokerPtr invoker,
        TMemberId memberId,
        TGroupId groupId)
        : Config_(std::move(config))
        , Id_(std::move(memberId))
        , GroupId_(std::move(groupId))
        , PeriodicExecutor_(New<TPeriodicExecutor>(
            invoker,
            BIND(&TImpl::OnHeartbeat, MakeWeak(this)),
            Config_->HeartbeatPeriod))
        , ChannelFactory_(CreateCachingChannelFactory(std::move(channelFactory)))
        , Logger(NLogging::TLogger(DiscoveryClientLogger)
            .AddTag("MemberId: %v", Id_))
        , AddressPool_(New<TServerAddressPool>(
            Config_->ServerBanTimeout,
            Logger,
            Config_->ServerAddresses))
        , Attributes_(CreateEphemeralAttributes())
        , ThreadSafeAttributes_(CreateThreadSafeAttributes(Attributes_.get()))
    { }

    NYTree::IAttributeDictionary* GetAttributes()
    {
        return ThreadSafeAttributes_.get();
    }

    i64 GetPriority()
    {
        return Priority_.load();
    }

    void SetPriority(i64 value)
    {
        Priority_ = value;
    }

    void Start()
    {
        YT_LOG_INFO("Starting member client (MemberId: %v)", Id_);
        PeriodicExecutor_->Start();
    }

    void Stop()
    {
        YT_LOG_INFO("Stopping member client (MemberId: %v)", Id_);
        PeriodicExecutor_->Stop();
    }

private:
    const TMemberClientConfigPtr Config_;
    const TMemberId Id_;
    const TGroupId GroupId_;
    const NConcurrency::TPeriodicExecutorPtr PeriodicExecutor_;
    const IChannelFactoryPtr ChannelFactory_;
    const NLogging::TLogger Logger;
    const TServerAddressPoolPtr AddressPool_;

    std::atomic<i64> Priority_;
    i64 Revision_ = 0;

    std::unique_ptr<NYTree::IAttributeDictionary> Attributes_;
    std::unique_ptr<NYTree::IAttributeDictionary> ThreadSafeAttributes_;
    TInstant LastAttributesUpdateTime_;

    void OnHeartbeat()
    {
        ++Revision_;

        std::unique_ptr<NYTree::IAttributeDictionary> attributes;
        auto now = TInstant::Now();
        if (now - LastAttributesUpdateTime_ > Config_->AttributeUpdatePeriod) {
            attributes = ThreadSafeAttributes_->Clone();
        }

        YT_LOG_DEBUG("Started sending heartbeat (Revision: %v)",
            Revision_,
            Id_);

        auto session = New<THeartbeatSession>(
            AddressPool_,
            Config_,
            ChannelFactory_,
            Logger,
            GroupId_,
            Id_,
            Priority_,
            Revision_,
            std::move(attributes));
        auto rspOrError = WaitFor(session->Run());
        if (!rspOrError.IsOK()) {
            YT_LOG_DEBUG(rspOrError, "Error reporting heartbeat (Revision: %v)",
                Revision_,
                Id_);
            return;
        }
        YT_LOG_DEBUG("Successfully reported heartbeat (Revision: %v)",
            Revision_,
            Id_);
        if (attributes) {
            LastAttributesUpdateTime_ = now;
        }
    }
};

TMemberClient::TMemberClient(
    TMemberClientConfigPtr config,
    IChannelFactoryPtr channelFactory,
    IInvokerPtr invoker,
    TString memberId,
    TString groupId)
    : Impl_(New<TImpl>(
        std::move(config),
        std::move(channelFactory),
        std::move(invoker),
        std::move(memberId),
        std::move(groupId)))
{ }

TMemberClient::~TMemberClient() = default;

NYTree::IAttributeDictionary* TMemberClient::GetAttributes()
{
    return Impl_->GetAttributes();
}

i64 TMemberClient::GetPriority()
{
    return Impl_->GetPriority();
}

void TMemberClient::SetPriority(i64 value)
{
    Impl_->SetPriority(value);
}

void TMemberClient::Start()
{
    Impl_->Start();
}

void TMemberClient::Stop()
{
    Impl_->Stop();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDiscoveryClient
