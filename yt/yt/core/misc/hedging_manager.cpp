#include "hedging_manager.h"
#include "atomic_ptr.h"
#include "config.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TAdaptiveHedgingManager
    : public IHedgingManager
{
public:
    TAdaptiveHedgingManager(
        TAdaptiveHedgingManagerConfigPtr config,
        const NProfiling::TProfiler& profiler)
        : Config_(std::move(config))
        , HedgingStatistics_(New<THedgingStatistics>(
            Config_->MaxHedgingDelay))
        , PrimaryRequestCount_(profiler.Counter("/primary_request_count"))
        , BackupAttemptCount_(profiler.Counter("/backup_attempt_count"))
        , BackupRequestCount_(profiler.Counter("/backup_request_count"))
        , HedgingDelay_(profiler.TimeGauge("/hedging_delay"))
    {
        YT_VERIFY(Config_->MaxBackupRequestRatio);
    }

    TDuration OnPrimaryRequestsStarted(int requestCount) override
    {
        if (Config_->MaxBackupRequestRatio == 1.) {
            return TDuration::Zero();
        }

        auto statistics = AcquireHedgingStatistics();
        statistics->PrimaryRequestCount.fetch_add(requestCount, std::memory_order_relaxed);

        return statistics->HedgingDelay;
    }

    bool OnHedgingDelayPassed(int attemptCount) override
    {
        if (Config_->MaxBackupRequestRatio == 1.) {
            return true;
        }

        auto statistics = AcquireHedgingStatistics();

        double previousStatisticsWeight = 1. - (GetInstant() - statistics->StartInstant) / Config_->TickPeriod;
        previousStatisticsWeight = std::max(0., std::min(1., previousStatisticsWeight));

        auto backupRequestCount = statistics->BackupRequestCount.load(std::memory_order_relaxed);
        auto primaryRequestCount = statistics->PrimaryRequestCount.load(std::memory_order_relaxed);

        if (auto previousStatistics = statistics->PreviousStatistics.Acquire()) {
            backupRequestCount += previousStatisticsWeight *
                previousStatistics->BackupRequestCount.load(std::memory_order_relaxed);
            primaryRequestCount += previousStatisticsWeight *
                previousStatistics->PrimaryRequestCount.load(std::memory_order_relaxed);
        }

        statistics->BackupAttemptCount.fetch_add(attemptCount, std::memory_order_relaxed);

        bool hedgingApproved = !IsBackupRequestLimitExceeded(primaryRequestCount, backupRequestCount);
        if (hedgingApproved) {
            statistics->BackupRequestCount.fetch_add(attemptCount, std::memory_order_relaxed);
        }

        return hedgingApproved;
    }

private:
    const TAdaptiveHedgingManagerConfigPtr Config_;

    struct THedgingStatistics;
    using THedgingStatisticsPtr = TIntrusivePtr<THedgingStatistics>;

    struct THedgingStatistics final
    {
        static constexpr bool EnableHazard = true;

        explicit THedgingStatistics(
            TDuration hedgingDelay,
            THedgingStatisticsPtr previousStatistics = nullptr)
            : StartInstant(GetInstant())
            , HedgingDelay(hedgingDelay)
            , PreviousStatistics(std::move(previousStatistics))
        { }

        const TInstant StartInstant;
        const TDuration HedgingDelay;

        std::atomic<i64> PrimaryRequestCount = 0;
        std::atomic<i64> BackupAttemptCount = 0;
        std::atomic<i64> BackupRequestCount = 0;

        TAtomicPtr<THedgingStatistics> PreviousStatistics;
    };

    TAtomicPtr<THedgingStatistics> HedgingStatistics_;

    NProfiling::TCounter PrimaryRequestCount_;
    NProfiling::TCounter BackupAttemptCount_;
    NProfiling::TCounter BackupRequestCount_;
    NProfiling::TTimeGauge HedgingDelay_;


    THedgingStatisticsPtr TrySwitchStatisticsAndTuneHedgingDelay(
        const THedgingStatisticsPtr& currentStatistics)
    {
        auto newHedgingDelay = currentStatistics->HedgingDelay;
        auto primaryRequestCount = currentStatistics->PrimaryRequestCount.load(std::memory_order_relaxed);
        auto backupAttemptCount = currentStatistics->BackupAttemptCount.load(std::memory_order_relaxed);
        if (IsBackupRequestLimitExceeded(primaryRequestCount, backupAttemptCount)) {
            newHedgingDelay *= Config_->HedgingDelayTuneFactor;
        } else {
            newHedgingDelay /= Config_->HedgingDelayTuneFactor;
        }
        newHedgingDelay = std::max(Config_->MinHedgingDelay, std::min(Config_->MaxHedgingDelay, newHedgingDelay));

        auto newStatistics = New<THedgingStatistics>(newHedgingDelay, currentStatistics);

        if (!HedgingStatistics_.SwapIfCompare(currentStatistics, newStatistics)) {
            return HedgingStatistics_.Acquire();
        }

        // NB: Skip profiling in case of very low RPS.
        if (newStatistics->StartInstant - currentStatistics->StartInstant <= 2 * Config_->TickPeriod) {
            PrimaryRequestCount_.Increment(currentStatistics->PrimaryRequestCount.load(std::memory_order_relaxed));
            BackupAttemptCount_.Increment(currentStatistics->BackupAttemptCount.load(std::memory_order_relaxed));
            BackupRequestCount_.Increment(currentStatistics->BackupRequestCount.load(std::memory_order_relaxed));
            HedgingDelay_.Update(currentStatistics->HedgingDelay);
        }

        currentStatistics->PreviousStatistics.Release();

        return newStatistics;
    }

    THedgingStatisticsPtr AcquireHedgingStatistics()
    {
        auto statistics = HedgingStatistics_.Acquire();

        if (GetInstant() - statistics->StartInstant <= Config_->TickPeriod) {
            return statistics;
        }

        return TrySwitchStatisticsAndTuneHedgingDelay(statistics);
    }

    bool IsBackupRequestLimitExceeded(i64 primaryRequestCount, i64 backupRequestCount) const
    {
        return backupRequestCount >= static_cast<i64>(std::ceil(primaryRequestCount * *Config_->MaxBackupRequestRatio));
    }
};

////////////////////////////////////////////////////////////////////////////////

IHedgingManagerPtr CreateAdaptiveHedgingManager(
    TAdaptiveHedgingManagerConfigPtr config,
    const NProfiling::TProfiler& profiler)
{
    return New<TAdaptiveHedgingManager>(std::move(config), profiler);
}

////////////////////////////////////////////////////////////////////////////////

class TSimpleHedgingManager
    : public IHedgingManager
{
public:
    explicit TSimpleHedgingManager(TDuration hedgingDelay)
        : HedgingDelay_(hedgingDelay)
    { }

    TDuration OnPrimaryRequestsStarted(int /*requestCount*/) override
    {
        return HedgingDelay_;
    }

    bool OnHedgingDelayPassed(int /*attemptCount*/) override
    {
        return true;
    }

private:
    const TDuration HedgingDelay_;
};

////////////////////////////////////////////////////////////////////////////////

IHedgingManagerPtr CreateSimpleHedgingManager(TDuration hedgingDelay)
{
    return New<TSimpleHedgingManager>(hedgingDelay);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
