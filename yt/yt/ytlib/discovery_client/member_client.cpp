#include "member_client.h"
#include "helpers.h"
#include "private.h"
#include "request_session.h"

#include <yt/yt/core/actions/future.h>

#include <yt/yt/core/ytree/attributes.h>
#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/core/rpc/caching_channel_factory.h>

#include <yt/yt/core/concurrency/periodic_executor.h>

#include <library/cpp/yt/threading/rw_spin_lock.h>

namespace NYT::NDiscoveryClient {

using namespace NConcurrency;
using namespace NRpc;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TMemberAttributeDictionary
    : public NYTree::IAttributeDictionary
{
public:
    explicit TMemberAttributeDictionary(IAttributeDictionaryPtr underlying)
        : Underlying_(std::move(underlying))
    { }

    std::vector<TString> ListKeys() const override
    {
        return Underlying_->ListKeys();
    }

    std::vector<TKeyValuePair> ListPairs() const override
    {
        return Underlying_->ListPairs();
    }

    NYson::TYsonString FindYson(TStringBuf key) const override
    {
        return Underlying_->FindYson(key);
    }

    void SetYson(const TString& key, const NYson::TYsonString& value) override
    {
        if (IsMemberSystemAttribute(key)) {
            THROW_ERROR_EXCEPTION("Cannot set system attribute %Qv", key);
        }
        Underlying_->SetYson(key, value);
    }

    bool Remove(const TString& key) override
    {
        return Underlying_->Remove(key);
    }

private:
    const IAttributeDictionaryPtr Underlying_;
};

IAttributeDictionaryPtr CreateMemberAttributes(IAttributeDictionaryPtr underlying)
{
    return New<TMemberAttributeDictionary>(std::move(underlying));
}

////////////////////////////////////////////////////////////////////////////////

class TMemberClient
    : public IMemberClient
{
public:
    TMemberClient(
        TMemberClientConfigPtr config,
        IChannelFactoryPtr channelFactory,
        IInvokerPtr invoker,
        TMemberId memberId,
        TGroupId groupId)
        : Id_(std::move(memberId))
        , GroupId_(std::move(groupId))
        , PeriodicExecutor_(New<TPeriodicExecutor>(
            std::move(invoker),
            BIND(&TMemberClient::OnHeartbeat, MakeWeak(this)),
            config->HeartbeatPeriod))
        , ChannelFactory_(std::move(channelFactory))
        , Logger(DiscoveryClientLogger.WithTag("GroupId: %v, MemberId: %v",
            GroupId_,
            Id_))
        , AddressPool_(New<TServerAddressPool>(
            Logger,
            config))
        , Config_(std::move(config))
        , Attributes_(CreateMemberAttributes(CreateEphemeralAttributes()))
        , ThreadSafeAttributes_(CreateThreadSafeAttributes(Attributes_.Get()))
    { }

    NYTree::IAttributeDictionary* GetAttributes() override
    {
        return ThreadSafeAttributes_.Get();
    }

    i64 GetPriority() override
    {
        return Priority_.load();
    }

    void SetPriority(i64 value) override
    {
        Priority_ = value;
    }

    TFuture<void> Start() override
    {
        YT_LOG_INFO("Starting member client");
        PeriodicExecutor_->Start();
        return FirstSuccessPromise_;
    }

    TFuture<void> Stop() override
    {
        YT_LOG_INFO("Stopping member client");
        return PeriodicExecutor_->Stop();
    }

    void Reconfigure(TMemberClientConfigPtr config) override
    {
        auto guard = WriterGuard(Lock_);

        if (config->HeartbeatPeriod != Config_->HeartbeatPeriod) {
            PeriodicExecutor_->SetPeriod(config->HeartbeatPeriod);
        }

        Config_ = std::move(config);
        AddressPool_->SetConfig(Config_);
    }

private:
    const TMemberId Id_;
    const TGroupId GroupId_;
    const NConcurrency::TPeriodicExecutorPtr PeriodicExecutor_;
    const IChannelFactoryPtr ChannelFactory_;
    const NLogging::TLogger Logger;
    const TServerAddressPoolPtr AddressPool_;

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, Lock_);
    TMemberClientConfigPtr Config_;

    std::atomic<i64> Priority_ = std::numeric_limits<i64>::max();
    i64 Revision_ = 0;

    NYTree::IAttributeDictionaryPtr Attributes_;
    NYTree::IAttributeDictionaryPtr ThreadSafeAttributes_;
    TInstant LastAttributesUpdateTime_;

    const TPromise<void> FirstSuccessPromise_ = NewPromise<void>();

    void OnHeartbeat()
    {
        YT_LOG_DEBUG("Started sending heartbeat (Revision: %v)", Revision_);

        ++Revision_;

        NYTree::IAttributeDictionaryPtr attributes;
        auto now = TInstant::Now();
        THeartbeatSessionPtr session;
        {
            auto guard = ReaderGuard(Lock_);
            if (now - LastAttributesUpdateTime_ > Config_->AttributeUpdatePeriod) {
                attributes = ThreadSafeAttributes_->Clone();
            }

            session = New<THeartbeatSession>(
                AddressPool_,
                Config_,
                ChannelFactory_,
                Logger,
                GroupId_,
                Id_,
                Priority_,
                Revision_,
                std::move(attributes));
        }
        auto rspOrError = WaitFor(session->Run());
        if (!rspOrError.IsOK()) {
            YT_LOG_DEBUG(rspOrError, "Error reporting heartbeat (Revision: %v)", Revision_);

            if (rspOrError.FindMatching(NDiscoveryClient::EErrorCode::InvalidGroupId) ||
                rspOrError.FindMatching(NDiscoveryClient::EErrorCode::InvalidMemberId))
            {
                FirstSuccessPromise_.TrySet(rspOrError);
            } else if (!FirstSuccessPromise_.IsSet() && Revision_ > Config_->MaxFailedHeartbeatsOnStartup) {
                FirstSuccessPromise_.TrySet(
                    TError("Error reporting heartbeat %v times on startup", Config_->MaxFailedHeartbeatsOnStartup)
                        << rspOrError);
            }
        } else {
            YT_LOG_DEBUG("Successfully reported heartbeat (Revision: %v)", Revision_);
            if (attributes) {
                LastAttributesUpdateTime_ = now;
            }
            FirstSuccessPromise_.TrySet();
        }
    }
};

IMemberClientPtr CreateMemberClient(
    TMemberClientConfigPtr config,
    IChannelFactoryPtr channelFactory,
    IInvokerPtr invoker,
    TString memberId,
    TString groupId)
{
    return New<TMemberClient>(
        std::move(config),
        std::move(channelFactory),
        std::move(invoker),
        std::move(memberId),
        std::move(groupId));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDiscoveryClient
