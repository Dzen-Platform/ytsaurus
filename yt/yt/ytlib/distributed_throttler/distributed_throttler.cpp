#include "distributed_throttler.h"
#include "distributed_throttler_proxy.h"
#include "config.h"

#include <yt/yt/core/rpc/service_detail.h>

#include <yt/yt/core/concurrency/throughput_throttler.h>
#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/spinlock.h>

#include <yt/yt/core/misc/algorithm_helpers.h>
#include <yt/yt/core/misc/atomic_object.h>
#include <yt/yt/core/misc/historic_usage_aggregator.h>

#include <yt/yt/ytlib/discovery_client/discovery_client.h>
#include <yt/yt/ytlib/discovery_client/member_client.h>

#include <yt/yt/library/numeric/binary_search.h>

namespace NYT::NDistributedThrottler {

using namespace NRpc;
using namespace NDiscoveryClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const TString AddressAttributeKey = "address";
static const TString RealmIdAttributeKey = "realm_id";
static const TString LeaderIdAttributeKey = "leader_id";

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TWrappedThrottler)

class TWrappedThrottler
    : public IReconfigurableThroughputThrottler
{
public:
    TWrappedThrottler(
        TString throttlerId,
        TDistributedThrottlerConfigPtr config,
        TThroughputThrottlerConfigPtr throttlerConfig,
        TDuration throttleRpcTimeout)
        : Underlying_(CreateReconfigurableThroughputThrottler(throttlerConfig))
        , ThrottlerId_(std::move(throttlerId))
        , Config_(std::move(config))
        , ThrottlerConfig_(std::move(throttlerConfig))
        , ThrottleRpcTimeout_(throttleRpcTimeout)
    {
        HistoricUsageAggregator_.UpdateParameters(THistoricUsageAggregationParameters(
            EHistoricUsageAggregationMode::ExponentialMovingAverage,
            Config_.Load()->EmaAlpha
        ));
    }

    void SetDistributedThrottlerConfig(TDistributedThrottlerConfigPtr config)
    {
        HistoricUsageAggregator_.UpdateParameters(THistoricUsageAggregationParameters(
            EHistoricUsageAggregationMode::ExponentialMovingAverage,
            config->EmaAlpha
        ));
        Config_.Store(std::move(config));
    }

    double GetUsageRate()
    {
        return HistoricUsageAggregator_.GetHistoricUsage();
    }

    TThroughputThrottlerConfigPtr GetConfig()
    {
        return ThrottlerConfig_.Load();
    }

    virtual TFuture<void> Throttle(i64 count) override
    {
        auto config = Config_.Load();

        if (config->Mode == EDistributedThrottlerMode::Precise) {
            auto leaderChannel = LeaderChannel_.Load();
            // Either we are leader or we dont know the leader yet.
            if (!leaderChannel) {
                return Underlying_->Throttle(count);
            }

            TDistributedThrottlerProxy proxy(leaderChannel);

            auto req = proxy.Throttle();
            req->SetTimeout(ThrottleRpcTimeout_);
            req->set_throttler_id(ThrottlerId_);
            req->set_count(count);

            return req->Invoke().As<void>();
        }

        auto future = Underlying_->Throttle(count);
        future.Subscribe(BIND([=] (const TError& error) {
            if (error.IsOK()) {
                UpdateHistoricUsage(count);
            }
        }));
        return future;
    }

    virtual bool TryAcquire(i64 count) override
    {
        YT_VERIFY(Config_.Load()->Mode != EDistributedThrottlerMode::Precise);

        auto result = Underlying_->TryAcquire(count);
        if (result) {
            UpdateHistoricUsage(count);
        }
        return result;
    }

    virtual i64 TryAcquireAvailable(i64 count) override
    {
        YT_VERIFY(Config_.Load()->Mode != EDistributedThrottlerMode::Precise);

        auto result = Underlying_->TryAcquireAvailable(count);
        if (result > 0) {
            UpdateHistoricUsage(result);
        }
        return result;
    }

    virtual void Acquire(i64 count) override
    {
        YT_VERIFY(Config_.Load()->Mode != EDistributedThrottlerMode::Precise);

        UpdateHistoricUsage(count);
        Underlying_->Acquire(count);
    }

    virtual bool IsOverdraft() override
    {
        YT_VERIFY(Config_.Load()->Mode != EDistributedThrottlerMode::Precise);

        return Underlying_->IsOverdraft();
    }

    virtual i64 GetQueueTotalCount() const override
    {
        YT_VERIFY(Config_.Load()->Mode != EDistributedThrottlerMode::Precise);

        return Underlying_->GetQueueTotalCount();
    }

    virtual void Reconfigure(TThroughputThrottlerConfigPtr config) override
    {
        if (Config_.Load()->Mode == EDistributedThrottlerMode::Precise) {
            Underlying_->Reconfigure(std::move(config));
        } else {
            ThrottlerConfig_.Store(CloneYsonSerializable(std::move(config)));
        }
    }

    virtual void SetLimit(std::optional<double> limit) override
    {
        Underlying_->SetLimit(limit);
    }

    void SetLeaderChannel(IChannelPtr leaderChannel)
    {
        LeaderChannel_.Store(leaderChannel);
    }

private:
    const IReconfigurableThroughputThrottlerPtr Underlying_;
    const TString ThrottlerId_;

    TAtomicObject<TDistributedThrottlerConfigPtr> Config_;
    TAtomicObject<TThroughputThrottlerConfigPtr> ThrottlerConfig_;

    const TDuration ThrottleRpcTimeout_;

    TAtomicObject<IChannelPtr> LeaderChannel_;

    YT_DECLARE_SPINLOCK(TAdaptiveLock, HistoricUsageAggregatorLock_);
    THistoricUsageAggregator HistoricUsageAggregator_;

    void UpdateHistoricUsage(i64 count)
    {
        auto guard = Guard(HistoricUsageAggregatorLock_);
        HistoricUsageAggregator_.UpdateAt(TInstant::Now(), count);
    }
};

DEFINE_REFCOUNTED_TYPE(TWrappedThrottler)

////////////////////////////////////////////////////////////////////////////////

struct TThrottlers
{
    YT_DECLARE_SPINLOCK(TReaderWriterSpinLock, Lock);
    THashMap<TString, TWeakPtr<TWrappedThrottler>> Throttlers;
};

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TDistributedThrottlerService)

class TDistributedThrottlerService
    : public TServiceBase
{
public:
    TDistributedThrottlerService(
        IServerPtr rpcServer,
        IInvokerPtr invoker,
        IDiscoveryClientPtr discoveryClient,
        TGroupId groupId,
        TDistributedThrottlerConfigPtr config,
        TRealmId realmId,
        TThrottlers* throttlers,
        NLogging::TLogger logger,
        int shardCount = 16)
        : TServiceBase(
            invoker,
            TDistributedThrottlerProxy::GetDescriptor(),
            logger,
            realmId)
        , RpcServer_(std::move(rpcServer))
        , DiscoveryClient_(std::move(discoveryClient))
        , GroupId_(std::move(groupId))
        , UpdatePeriodicExecutor_(New<TPeriodicExecutor>(
            std::move(invoker),
            BIND(&TDistributedThrottlerService::UpdateLimits, MakeWeak(this)),
            config->LimitUpdatePeriod))
        , Throttlers_(throttlers)
        , Logger(std::move(logger))
        , ShardCount_(shardCount)
        , Config_(std::move(config))
        , MemberShards_(ShardCount_)
        , ThrottlerShards_(ShardCount_)
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Heartbeat));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Throttle)
            .SetCancelable(true)
            .SetConcurrencyLimit(10000)
            .SetQueueSizeLimit(20000));
    }

    void Initialize()
    {
        RpcServer_->RegisterService(this);
        UpdatePeriodicExecutor_->Start();
    }

    void Finalize()
    {
        UpdatePeriodicExecutor_->Stop();
        RpcServer_->UnregisterService(this);
    }

    void Reconfigure(TDistributedThrottlerConfigPtr config)
    {
        auto oldConfig = Config_.Load();

        if (oldConfig->LimitUpdatePeriod != config->LimitUpdatePeriod) {
            UpdatePeriodicExecutor_->SetPeriod(config->LimitUpdatePeriod);
        }

        Config_.Store(config);
    }

    void SetTotalLimit(const TString& throttlerId, std::optional<double> limit)
    {
        auto* shard = GetThrottlerShard(throttlerId);

        auto guard = WriterGuard(shard->TotalLimitsLock);
        shard->ThrottlerIdToTotalLimit[throttlerId] = limit;
    }

    void UpdateUsageRate(const TMemberId& memberId, THashMap<TString, double> throttlerIdToUsageRate)
    {
        auto config = Config_.Load();

        std::vector<std::vector<TString>> throttlerIdsByShard(ShardCount_);
        for (const auto& [throttlerId, usageRate] : throttlerIdToUsageRate) {
            throttlerIdsByShard[GetShardIndex(throttlerId)].push_back(throttlerId);
        }

        auto now = TInstant::Now();
        for (int i = 0; i < ShardCount_; ++i) {
            if (throttlerIdsByShard[i].empty()) {
                continue;
            }

            auto& shard = ThrottlerShards_[i];
            auto guard = WriterGuard(shard.LastUpdateTimeLock);
            for (const auto& throttlerId : throttlerIdsByShard[i]) {
                shard.ThrottlerIdToLastUpdateTime[throttlerId] = now;
            }
        }

        {
            auto* shard = GetMemberShard(memberId);

            auto guard = WriterGuard(shard->UsageRatesLock);
            shard->MemberIdToUsageRate[memberId].swap(throttlerIdToUsageRate);
        }
    }

    THashMap<TString, std::optional<double>> GetMemberLimits(const TMemberId& memberId, const std::vector<TString>& throttlerIds)
    {
        auto config = Config_.Load();

        std::vector<std::vector<TString>> throttlerIdsByShard(ShardCount_);
        for (const auto& throttlerId : throttlerIds) {
            throttlerIdsByShard[GetShardIndex(throttlerId)].push_back(throttlerId);
        }

        THashMap<TString, std::optional<double>> result;
        for (int i = 0; i < ShardCount_; ++i) {
            if (throttlerIdsByShard[i].empty()) {
                continue;
            }

            auto& throttlerShard = ThrottlerShards_[i];
            auto totalLimitsGuard = ReaderGuard(throttlerShard.TotalLimitsLock);
            for (const auto& throttlerId : throttlerIdsByShard[i]) {
                auto totalLimitIt = throttlerShard.ThrottlerIdToTotalLimit.find(throttlerId);
                if (totalLimitIt == throttlerShard.ThrottlerIdToTotalLimit.end()) {
                    YT_LOG_WARNING("There is no total limit for throttler (ThrottlerId: %v)", throttlerId);
                    continue;
                }

                auto optionalTotalLimit = totalLimitIt->second;
                if (!optionalTotalLimit) {
                    YT_VERIFY(result.emplace(throttlerId, std::nullopt).second);
                }
            }

            auto fillLimits = [&] (const THashMap<TString, double>& throttlerIdToLimits) {
                for (const auto& throttlerId : throttlerIdsByShard[i]) {
                    if (result.contains(throttlerId)) {
                        continue;
                    }
                    auto limitIt = throttlerIdToLimits.find(throttlerId);
                    if (limitIt == throttlerIdToLimits.end()) {
                        YT_LOG_WARNING("There is no total limit for throttler (ThrottlerId: %v)", throttlerId);
                    } else {
                        YT_VERIFY(result.emplace(throttlerId, limitIt->second).second);
                    }
                }
            };

            if (config->Mode == EDistributedThrottlerMode::Uniform) {
                auto guard = ReaderGuard(throttlerShard.UniformLimitLock);
                fillLimits(throttlerShard.ThrottlerIdToUniformLimit);
            } else {
                auto* shard = GetMemberShard(memberId);

                auto guard = ReaderGuard(shard->LimitsLock);
                auto memberIt = shard->MemberIdToLimit.find(memberId);
                if (memberIt != shard->MemberIdToLimit.end()) {
                    fillLimits(memberIt->second);
                }
            }
        }

        return result;
    }

private:
    const IServerPtr RpcServer_;
    const IDiscoveryClientPtr DiscoveryClient_;
    const TString GroupId_;
    const TPeriodicExecutorPtr UpdatePeriodicExecutor_;
    TThrottlers* const Throttlers_;
    const NLogging::TLogger Logger;
    const int ShardCount_;

    TAtomicObject<TDistributedThrottlerConfigPtr> Config_;

    struct TMemberShard
    {
        YT_DECLARE_SPINLOCK(TReaderWriterSpinLock, LimitsLock);
        THashMap<TMemberId, THashMap<TString, double>> MemberIdToLimit;

        YT_DECLARE_SPINLOCK(TReaderWriterSpinLock, UsageRatesLock);
        THashMap<TMemberId, THashMap<TString, double>> MemberIdToUsageRate;
    };
    std::vector<TMemberShard> MemberShards_;

    struct TThrottlerShard
    {
        YT_DECLARE_SPINLOCK(TReaderWriterSpinLock, TotalLimitsLock);
        THashMap<TString, std::optional<double>> ThrottlerIdToTotalLimit;

        YT_DECLARE_SPINLOCK(TReaderWriterSpinLock, UniformLimitLock);
        THashMap<TString, double> ThrottlerIdToUniformLimit;

        YT_DECLARE_SPINLOCK(TReaderWriterSpinLock, LastUpdateTimeLock);
        THashMap<TString, TInstant> ThrottlerIdToLastUpdateTime;
    };
    std::vector<TThrottlerShard> ThrottlerShards_;

    DECLARE_RPC_SERVICE_METHOD(NDistributedThrottler::NProto, Heartbeat)
    {
        auto config = Config_.Load();

        if (config->Mode == EDistributedThrottlerMode::Precise) {
            THROW_ERROR_EXCEPTION(
                NDistributedThrottler::EErrorCode::UnexpectedThrottlerMode,
                "Cannot handle heartbeat request in %v mode",
                config->Mode);
        }

        const auto& memberId = request->member_id();

        context->SetRequestInfo("MemberId: %v, ThrottlerCount: %v",
            memberId,
            request->throttlers().size());

        THashMap<TString, double> throttlerIdToUsageRate;
        for (const auto& throttler : request->throttlers()) {
            const auto& throttlerId = throttler.id();
            auto usageRate = throttler.usage_rate();
            YT_VERIFY(throttlerIdToUsageRate.emplace(throttlerId, usageRate).second);
        }

        auto limits = GetMemberLimits(memberId, GetKeys(throttlerIdToUsageRate));
        for (const auto& [throttlerId, limit] : limits) {
            auto* result = response->add_throttlers();
            result->set_id(throttlerId);
            if (limit) {
                result->set_limit(*limit);
            }
        }
        UpdateUsageRate(memberId, std::move(throttlerIdToUsageRate));

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NDistributedThrottler::NProto, Throttle)
    {
        auto config = Config_.Load();

        if (config->Mode != EDistributedThrottlerMode::Precise) {
            THROW_ERROR_EXCEPTION(
                NDistributedThrottler::EErrorCode::UnexpectedThrottlerMode,
                "Cannot handle throttle request in %v mode",
                config->Mode);
        }

        const auto& throttlerId = request->throttler_id();
        auto count = request->count();

        context->SetRequestInfo("ThrottlerId: %v, Count: %v",
            throttlerId,
            count);

        Throttle(throttlerId, count).Subscribe(BIND([=] (const TError& error) {
            context->Reply(error);
        }));
    }

    IReconfigurableThroughputThrottlerPtr FindThrottler(const TString& throttlerId)
    {
        auto guard = ReaderGuard(Throttlers_->Lock);

        auto it = Throttlers_->Throttlers.find(throttlerId);
        if (it == Throttlers_->Throttlers.end()) {
            return nullptr;
        }
        return it->second.Lock();
    }

    TFuture<void> Throttle(const TString& throttlerId, i64 count)
    {
        auto throttler = FindThrottler(throttlerId);
        if (!throttler) {
            return MakeFuture(TError(NDistributedThrottler::EErrorCode::NoSuchThrottler, "No such throttler %Qv", throttlerId));
        }

        return throttler->Throttle(count);
    }

    int GetShardIndex(const TMemberId& memberId)
    {
        return THash<TMemberId>()(memberId) % ShardCount_;
    }

    TMemberShard* GetMemberShard(const TMemberId& memberId)
    {
        return &MemberShards_[GetShardIndex(memberId)];
    }

    TThrottlerShard* GetThrottlerShard(const TMemberId& memberId)
    {
        return &ThrottlerShards_[GetShardIndex(memberId)];
    }

    void UpdateUniformLimitDistribution()
    {
        auto countRspOrError = WaitFor(DiscoveryClient_->GetGroupMeta(GroupId_));
        if (!countRspOrError.IsOK()) {
            YT_LOG_WARNING(countRspOrError, "Error updating throttler limits");
            return;
        }

        auto totalCount = countRspOrError.Value().MemberCount;
        if (totalCount == 0) {
            YT_LOG_WARNING("No members in current group");
            return;
        }

        for (auto& shard : ThrottlerShards_) {
            THashMap<TString, double> throttlerIdToUniformLimit;
            {
                auto guard = ReaderGuard(shard.TotalLimitsLock);
                for (const auto& [throttlerId, optionalTotalLimit] : shard.ThrottlerIdToTotalLimit) {
                    if (!optionalTotalLimit) {
                        continue;
                    }

                    auto uniformLimit = std::max<double>(1, *optionalTotalLimit / totalCount);
                    YT_VERIFY(throttlerIdToUniformLimit.emplace(throttlerId, uniformLimit).second);
                    YT_LOG_TRACE("Uniform distribution limit updated (ThrottlerId: %v, UniformLimit: %v)",
                        throttlerId,
                        uniformLimit);
                }
            }

            {
                auto guard = WriterGuard(shard.UniformLimitLock);
                shard.ThrottlerIdToUniformLimit.swap(throttlerIdToUniformLimit);
            }
        }
    }

    void UpdateLimits()
    {
        ForgetDeadThrottlers();

        auto config = Config_.Load();

        if (config->Mode == EDistributedThrottlerMode::Precise) {
            return;
        }

        if (config->Mode == EDistributedThrottlerMode::Uniform) {
            UpdateUniformLimitDistribution();
            return;
        }

        std::vector<THashMap<TMemberId, THashMap<TString, double>>> memberIdToLimit(ShardCount_);
        for (auto& throttlerShard : ThrottlerShards_) {
            THashMap<TString, std::optional<double>> throttlerIdToTotalLimit;
            {
                auto guard = ReaderGuard(throttlerShard.TotalLimitsLock);
                throttlerIdToTotalLimit = throttlerShard.ThrottlerIdToTotalLimit;
            }

            THashMap<TString, double> throttlerIdToTotalUsage;
            THashMap<TString, THashMap<TString, double>> throttlerIdToUsageRates;
            int memberCount = 0;

            for (auto& shard : MemberShards_) {
                auto guard = ReaderGuard(shard.UsageRatesLock);
                memberCount += shard.MemberIdToUsageRate.size();
                for (const auto& [memberId, throttlers] : shard.MemberIdToUsageRate) {
                    for (const auto& [throttlerId, totalLimit] : throttlerIdToTotalLimit) {
                        auto throttlerIt = throttlers.find(throttlerId);
                        if (throttlerIt == throttlers.end()) {
                            YT_LOG_INFO("Member doesn't know about throttler (MemberId: %v, ThrottlerId: %v)",
                                memberId,
                                throttlerId);
                            continue;
                        }
                        auto usageRate = throttlerIt->second;
                        throttlerIdToTotalUsage[throttlerId] += usageRate;
                        throttlerIdToUsageRates[throttlerId].emplace(memberId, usageRate);
                    }
                }
            }

            for (const auto& [throttlerId, totalUsageRate] : throttlerIdToTotalUsage) {
                auto optionalTotalLimit = GetOrCrash(throttlerIdToTotalLimit, throttlerId);
                if (!optionalTotalLimit) {
                    continue;
                }
                auto totalLimit = *optionalTotalLimit;

                auto defaultLimit = FloatingPointInverseLowerBound(0, totalLimit, [&, &throttlerId = throttlerId](double value) {
                    double total = 0;
                    for (const auto& [memberId, usageRate] : throttlerIdToUsageRates[throttlerId]) {
                        total += Min(value, usageRate);
                    }
                    return total <= totalLimit;
                });

                auto extraLimit = (config->ExtraLimitRatio * totalLimit + Max<double>(0, totalLimit - totalUsageRate)) / memberCount;

                for (const auto& [memberId, usageRate] : GetOrCrash(throttlerIdToUsageRates, throttlerId)) {
                    auto newLimit = Min(usageRate, defaultLimit) + extraLimit;
                    YT_LOG_TRACE(
                        "Updating throttler limit (MemberId: %v, ThrottlerId: %v, UsageRate: %v, NewLimit: %v, ExtraLimit: %v)",
                        memberId,
                        throttlerId,
                        usageRate,
                        newLimit,
                        extraLimit);
                    YT_VERIFY(memberIdToLimit[GetShardIndex(memberId)][memberId].emplace(throttlerId, newLimit).second);
                }
            }
        }

        {
            for (int i = 0; i < ShardCount_; ++i) {
                auto& shard = MemberShards_[i];
                auto guard = WriterGuard(shard.LimitsLock);
                shard.MemberIdToLimit.swap(memberIdToLimit[i]);
            }
        }
    }

    void ForgetDeadThrottlers()
    {
        auto config = Config_.Load();

        for (auto& throttlerShard : ThrottlerShards_) {
            std::vector<TString> deadThrottlersIds;

            {
                auto now = TInstant::Now();
                auto guard = ReaderGuard(throttlerShard.LastUpdateTimeLock);
                for (const auto& [throttlerId, lastUpdateTime] : throttlerShard.ThrottlerIdToLastUpdateTime) {
                    if (lastUpdateTime + config->ThrottlerExpirationTime < now) {
                        deadThrottlersIds.push_back(throttlerId);
                    }
                }
            }

            if (deadThrottlersIds.empty()) {
                continue;
            }

            {
                auto guard = WriterGuard(throttlerShard.TotalLimitsLock);
                for (const auto& deadThrottlerId : deadThrottlersIds) {
                    throttlerShard.ThrottlerIdToTotalLimit.erase(deadThrottlerId);
                }
            }

            {
                auto guard = WriterGuard(throttlerShard.UniformLimitLock);
                for (const auto& deadThrottlerId : deadThrottlersIds) {
                    throttlerShard.ThrottlerIdToUniformLimit.erase(deadThrottlerId);
                }
            }

            for (auto& memberShard : MemberShards_) {
                auto guard = WriterGuard(memberShard.LimitsLock);
                for (auto& [memberId, throttlerIdToLimit] : memberShard.MemberIdToLimit) {
                    for (const auto& deadThrottlerId : deadThrottlersIds) {
                        throttlerIdToLimit.erase(deadThrottlerId);
                    }
                }
            }

            for (auto& memberShard : MemberShards_) {
                auto guard = WriterGuard(memberShard.UsageRatesLock);
                for (auto& [memberId, throttlerIdToUsageRate] : memberShard.MemberIdToUsageRate) {
                    for (const auto& deadThrottlerId : deadThrottlersIds) {
                        throttlerIdToUsageRate.erase(deadThrottlerId);
                    }
                }
            }

            {
                auto guard = WriterGuard(throttlerShard.LastUpdateTimeLock);
                for (const auto& deadThrottlerId : deadThrottlersIds) {
                    throttlerShard.ThrottlerIdToLastUpdateTime.erase(deadThrottlerId);
                }
            }
        }
    }
};

DEFINE_REFCOUNTED_TYPE(TDistributedThrottlerService)

////////////////////////////////////////////////////////////////////////////////

class TDistributedThrottlerFactory
    : public IDistributedThrottlerFactory
{
public:
    TDistributedThrottlerFactory(
        TDistributedThrottlerConfigPtr config,
        IChannelFactoryPtr channelFactory,
        IInvokerPtr invoker,
        TGroupId groupId,
        TMemberId memberId,
        IServerPtr rpcServer,
        TString address,
        const NLogging::TLogger& logger)
        : ChannelFactory_(std::move(channelFactory))
        , GroupId_(std::move(groupId))
        , MemberId_(std::move(memberId))
        , MemberClient_(CreateMemberClient(
            config->MemberClient,
            ChannelFactory_,
            invoker,
            MemberId_,
            GroupId_))
        , DiscoveryClient_(CreateDiscoveryClient(
            config->DiscoveryClient,
            ChannelFactory_))
        , UpdateLimitsExecutor_(New<TPeriodicExecutor>(
            invoker,
            BIND(&TDistributedThrottlerFactory::UpdateLimits, MakeWeak(this)),
            config->LimitUpdatePeriod))
        , UpdateLeaderExecutor_(New<TPeriodicExecutor>(
            invoker,
            BIND(&TDistributedThrottlerFactory::UpdateLeader, MakeWeak(this)),
            config->LeaderUpdatePeriod))
        , RealmId_(TGuid::Create())
        , Logger(logger.WithTag("SelfMemberId: %v, GroupId: %v, RealmId: %v",
            MemberId_,
            GroupId_,
            RealmId_))
        , Config_(std::move(config))
        , DistributedThrottlerService_(New<TDistributedThrottlerService>(
            std::move(rpcServer),
            std::move(invoker),
            DiscoveryClient_,
            GroupId_,
            Config_.Load(),
            RealmId_,
            &Throttlers_,
            Logger))
    {
        auto* attributes = MemberClient_->GetAttributes();
        attributes->Set(RealmIdAttributeKey, RealmId_);
        attributes->Set(AddressAttributeKey, address);

        MemberClient_->SetPriority(TInstant::Now().Seconds());
    }

    virtual IReconfigurableThroughputThrottlerPtr GetOrCreateThrottler(
        const TString& throttlerId,
        TThroughputThrottlerConfigPtr throttlerConfig,
        TDuration throttleRpcTimeout) override
    {
        auto findThrottler = [&] (const TString& throttlerId) -> IReconfigurableThroughputThrottlerPtr {
            auto it = Throttlers_.Throttlers.find(throttlerId);
            if (it == Throttlers_.Throttlers.end()) {
                return nullptr;
            }
            auto throttler = it->second.Lock();
            if (!throttler) {
                return nullptr;
            }
            throttler->Reconfigure(std::move(throttlerConfig));
            return throttler;
        };

        {
            auto guard = ReaderGuard(Throttlers_.Lock);
            if (auto throttler = findThrottler(throttlerId)) {
                return throttler;
            }
        }

        {
            auto guard = WriterGuard(Throttlers_.Lock);
            if (auto throttler = findThrottler(throttlerId)) {
                return throttler;
            }

            DistributedThrottlerService_->SetTotalLimit(throttlerId, throttlerConfig->Limit);

            auto wrappedThrottler = New<TWrappedThrottler>(
                throttlerId,
                Config_.Load(),
                std::move(throttlerConfig),
                throttleRpcTimeout);
            {
                auto readerGuard = ReaderGuard(Lock_);
                // NB: Could be null.
                wrappedThrottler->SetLeaderChannel(LeaderChannel_);
            }
            Throttlers_.Throttlers[throttlerId] = wrappedThrottler;

            YT_LOG_DEBUG("Distributed throttler created (ThrottlerId: %v)", throttlerId);
            return wrappedThrottler;
        }
    }

    virtual void Reconfigure(TDistributedThrottlerConfigPtr config) override
    {
        MemberClient_->Reconfigure(config->MemberClient);
        DiscoveryClient_->Reconfigure(config->DiscoveryClient);

        auto oldConfig = Config_.Load();

        if (oldConfig->LimitUpdatePeriod != config->LimitUpdatePeriod) {
            UpdateLimitsExecutor_->SetPeriod(config->LimitUpdatePeriod);
        }
        if (oldConfig->LeaderUpdatePeriod != config->LeaderUpdatePeriod) {
            UpdateLeaderExecutor_->SetPeriod(config->LimitUpdatePeriod);
        }

        DistributedThrottlerService_->Reconfigure(config);

        {
            auto guard = ReaderGuard(Throttlers_.Lock);
            for (const auto& [throttlerId, weakThrottler] : Throttlers_.Throttlers) {
                auto throttler = weakThrottler.Lock();
                if (!throttler) {
                    continue;
                }
                throttler->SetDistributedThrottlerConfig(config);
            }
        }

        Config_.Store(std::move(config));
    }

    virtual void Start() override
    {
        MemberClient_->Start();

        UpdateLimitsExecutor_->Start();
        UpdateLeaderExecutor_->Start();
    }

    virtual void Stop() override
    {
        MemberClient_->Stop();

        UpdateLimitsExecutor_->Stop();

        UpdateLeaderExecutor_->Stop()
            .Subscribe(
                BIND([=, this_ = MakeStrong(this)] (const TError&) {
                    std::optional<TMemberId> oldLeaderId;
                    {
                        auto guard = WriterGuard(Lock_);
                        oldLeaderId = LeaderId_;
                        LeaderId_.reset();
                        LeaderChannel_.Reset();
                    }

                    if (oldLeaderId == MemberId_) {
                        DistributedThrottlerService_->Finalize();
                    }
                }));
    }

private:
    const IChannelFactoryPtr ChannelFactory_;
    const TGroupId GroupId_;
    const TMemberId MemberId_;
    const IMemberClientPtr MemberClient_;
    const IDiscoveryClientPtr DiscoveryClient_;
    const TPeriodicExecutorPtr UpdateLimitsExecutor_;
    const TPeriodicExecutorPtr UpdateLeaderExecutor_;
    const TRealmId RealmId_;

    const NLogging::TLogger Logger;
    TAtomicObject<TDistributedThrottlerConfigPtr> Config_;
    const TDistributedThrottlerServicePtr DistributedThrottlerService_;

    TThrottlers Throttlers_;

    YT_DECLARE_SPINLOCK(TReaderWriterSpinLock, Lock_);
    std::optional<TMemberId> LeaderId_;
    IChannelPtr LeaderChannel_;

    void UpdateLimits()
    {
        auto config = Config_.Load();
        if (config->Mode == EDistributedThrottlerMode::Precise) {
            return;
        }

        IChannelPtr leaderChannel;
        TMemberId leaderId;
        {
            auto guard = ReaderGuard(Lock_);
            if (!LeaderId_) {
                YT_LOG_DEBUG("Unable to update throttler limit: no active leader");
                UpdateLeaderExecutor_->ScheduleOutOfBand();
                return;
            }
            leaderId = *LeaderId_;
            leaderChannel = LeaderChannel_;
        }

        THashMap<TString, TWrappedThrottlerPtr> throttlers;
        std::vector<TString> deadThrottlerIds;
        {
            auto guard = ReaderGuard(Throttlers_.Lock);
            for (const auto& [throttlerId, throttler] : Throttlers_.Throttlers) {
                if (auto throttlerPtr = throttler.Lock()) {
                    YT_VERIFY(throttlers.emplace(throttlerId, throttlerPtr).second);
                } else {
                    deadThrottlerIds.push_back(throttlerId);
                }
            }
        }

        if (!deadThrottlerIds.empty()) {
            auto guard = WriterGuard(Throttlers_.Lock);
            for (const auto& throttlerId : deadThrottlerIds) {
                Throttlers_.Throttlers.erase(throttlerId);
            }
        }

        if (leaderId == MemberId_) {
            UpdateLimitsAtLeader(std::move(throttlers));
        } else {
            UpdateLimitsAtFollower(std::move(leaderId), std::move(leaderChannel), std::move(throttlers));
        }
    }

    void UpdateLimitsAtLeader(THashMap<TString, TWrappedThrottlerPtr> throttlers)
    {
        THashMap<TString, double> throttlerIdToUsageRate;
        for (const auto& [throttlerId, throttler] : throttlers) {
            auto config = throttler->GetConfig();
            DistributedThrottlerService_->SetTotalLimit(throttlerId, config->Limit);

            auto usageRate = throttler->GetUsageRate();
            YT_VERIFY(throttlerIdToUsageRate.emplace(throttlerId, usageRate).second);
        }

        auto limits = DistributedThrottlerService_->GetMemberLimits(MemberId_, GetKeys(throttlerIdToUsageRate));
        for (const auto& [throttlerId, limit] : limits) {
            const auto& throttler = GetOrCrash(throttlers, throttlerId);
            throttler->SetLimit(limit);
            YT_LOG_TRACE("Throttler limit updated (ThrottlerId: %v, Limit: %v)",
                throttlerId,
                limit);
        }
        DistributedThrottlerService_->UpdateUsageRate(MemberId_, std::move(throttlerIdToUsageRate));
    }

    void UpdateLimitsAtFollower(
        TString leaderId,
        IChannelPtr leaderChannel,
        THashMap<TString, TWrappedThrottlerPtr> throttlers)
    {
        auto config = Config_.Load();

        TDistributedThrottlerProxy proxy(std::move(leaderChannel));

        auto req = proxy.Heartbeat();
        req->SetTimeout(config->ControlRpcTimeout);
        req->set_member_id(MemberId_);

        for (const auto& [throttlerId, throttler] : throttlers) {
            auto* protoThrottler = req->add_throttlers();
            protoThrottler->set_id(throttlerId);
            protoThrottler->set_usage_rate(throttler->GetUsageRate());
        }

        req->Invoke().Subscribe(
            BIND([
                =,
                this_ = MakeStrong(this),
                throttlers = std::move(throttlers)
            ] (const TErrorOr<TDistributedThrottlerProxy::TRspHeartbeatPtr>& rspOrError) {
                if (!rspOrError.IsOK()) {
                    YT_LOG_WARNING(rspOrError, "Failed updating throttler limit (LeaderId: %v)",
                        leaderId);
                    return;
                }

                const auto& rsp = rspOrError.Value();
                for (const auto& rspThrottler : rsp->throttlers()) {
                    auto limit = rspThrottler.has_limit() ? std::make_optional(rspThrottler.limit()) : std::nullopt;
                    const auto& throttlerId = rspThrottler.id();
                    const auto& throttler = GetOrCrash(throttlers, throttlerId);
                    throttler->SetLimit(limit);
                    YT_LOG_TRACE("Throttler limit updated (LeaderId: %v, ThrottlerId: %v, Limit: %v)",
                        leaderId,
                        throttlerId,
                        limit);
                }
            }));
    }

    void UpdateLeader()
    {
        TListMembersOptions options;
        options.Limit = 1;
        options.AttributeKeys = {AddressAttributeKey, RealmIdAttributeKey};

        auto rspFuture = DiscoveryClient_->ListMembers(GroupId_, options);
        auto rspOrError = WaitForUnique(rspFuture);
        if (!rspOrError.IsOK()) {
            YT_LOG_WARNING(rspOrError, "Error updating leader");
            return;
        }

        const auto& members = rspOrError.Value();
        if (members.empty()) {
            return;
        }

        const auto& leader = members[0];
        auto optionalAddress = leader.Attributes->Find<TString>(AddressAttributeKey);
        if (!optionalAddress) {
            YT_LOG_WARNING("Leader does not have '%v' attribute (LeaderId: %v)",
                AddressAttributeKey,
                leader.Id);
            return;
        }

        auto optionalRealmId = leader.Attributes->Find<TRealmId>(RealmIdAttributeKey);
        if (!optionalRealmId) {
            YT_LOG_WARNING("Leader does not have '%v' attribute (LeaderId: %v)",
                RealmIdAttributeKey,
                leader.Id);
            return;
        }

        const auto& leaderId = members[0].Id;
        std::optional<TMemberId> oldLeaderId;
        IChannelPtr leaderChannel;
        {
            auto guard = WriterGuard(Lock_);
            if (LeaderId_ == leaderId) {
                return;
            }
            YT_LOG_INFO("Leader changed (OldLeaderId: %v, NewLeaderId: %v)",
                LeaderId_,
                leaderId);
            {
                auto* attributes = MemberClient_->GetAttributes();
                attributes->Set(LeaderIdAttributeKey, leaderId);
            }
            oldLeaderId = LeaderId_;
            LeaderId_ = leaderId;
            LeaderChannel_ = leaderId == MemberId_
                ? nullptr
                : CreateRealmChannel(ChannelFactory_->CreateChannel(*optionalAddress), *optionalRealmId);
            leaderChannel = LeaderChannel_;
        }

        if (Config_.Load()->Mode == EDistributedThrottlerMode::Precise) {
            auto guard = ReaderGuard(Throttlers_.Lock);
            for (const auto& [throttlerId, weakThrottler] : Throttlers_.Throttlers) {
                auto throttler = weakThrottler.Lock();
                if (!throttler) {
                    continue;
                }
                throttler->SetLeaderChannel(leaderChannel);
            }
        }

        if (oldLeaderId == MemberId_) {
            DistributedThrottlerService_->Finalize();
        }

        if (leaderId == MemberId_) {
            DistributedThrottlerService_->Initialize();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

IDistributedThrottlerFactoryPtr CreateDistributedThrottlerFactory(
    TDistributedThrottlerConfigPtr config,
    IChannelFactoryPtr channelFactory,
    IInvokerPtr invoker,
    TGroupId groupId,
    TMemberId memberId,
    IServerPtr rpcServer,
    TString address,
    NLogging::TLogger logger)
{
    return New<TDistributedThrottlerFactory>(
        CloneYsonSerializable(std::move(config)),
        std::move(channelFactory),
        std::move(invoker),
        std::move(groupId),
        std::move(memberId),
        std::move(rpcServer),
        std::move(address),
        std::move(logger));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDistributedThrottler
