#pragma once

#include "private.h"
#include "operation.h"
#include "config.h"
#include "scheduler_strategy_host.h"

#include <yt/yt/server/lib/scheduler/config.h>

#include <yt/yt/server/scheduler/job.h>
#include <yt/yt/server/scheduler/operation.h>

#include <fstream>

namespace NYT::NSchedulerSimulator {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EEventType,
    (Heartbeat)
    (JobFinished)
);

struct TNodeShardEvent
{
    EEventType Type;
    TInstant Time;
    NNodeTrackerClient::TNodeId NodeId;
    NScheduler::TJobPtr Job;
    NScheduler::TExecNodePtr JobNode;
    bool ScheduledOutOfBand;

    static TNodeShardEvent Heartbeat(TInstant time, NNodeTrackerClient::TNodeId nodeId, bool scheduledOutOfBand);

    static TNodeShardEvent JobFinished(
        TInstant time,
        const NScheduler::TJobPtr& job,
        const NScheduler::TExecNodePtr& execNode,
        NNodeTrackerClient::TNodeId nodeId);

private:
    TNodeShardEvent(EEventType type, TInstant time);
};

bool operator<(const TNodeShardEvent& lhs, const TNodeShardEvent& rhs);


struct TOperationStatistics
{
    int JobCount = 0;
    int PreemptedJobCount = 0;
    TDuration JobMaxDuration;
    TDuration JobsTotalDuration;
    TDuration PreemptedJobsTotalDuration;

    // These fields are not accumulative. They are set exactly once when the operation is finished.
    TDuration StartTime;
    TDuration FinishTime;
    TDuration RealDuration;
    NScheduler::EOperationType OperationType;
    TString OperationState;
    bool InTimeframe = false;
};

class TSharedOperationStatistics
{
public:
    explicit TSharedOperationStatistics(std::vector<TOperationDescription> operations);

    void OnJobStarted(NScheduler::TOperationId operationId, TDuration duration);

    void OnJobPreempted(NScheduler::TOperationId operationId, TDuration duration);

    void OnJobFinished(NScheduler::TOperationId operationId, TDuration duration);

    void OnOperationStarted(NScheduler::TOperationId operationId);

    TOperationStatistics OnOperationFinished(
        NScheduler::TOperationId operationId,
        TDuration startTime,
        TDuration finishTime);

    const TOperationDescription& GetOperationDescription(NScheduler::TOperationId operationId) const;

private:
    struct TOperationStatisticsWithLock final
    {
        TOperationStatistics Value;
        YT_DECLARE_SPINLOCK(TAdaptiveLock, Lock);
    };

    using TOperationDescriptionMap = THashMap<NScheduler::TOperationId, TOperationDescription>;
    using TOperationStatisticsMap = THashMap<NScheduler::TOperationId, TIntrusivePtr<TOperationStatisticsWithLock>>;

    static TOperationDescriptionMap CreateOperationDescriptionMap(std::vector<TOperationDescription> operations);
    static TOperationStatisticsMap CreateOperationsStorageMap(const TOperationDescriptionMap& operationDescriptions);

    const TOperationDescriptionMap IdToOperationDescription_;
    const TOperationStatisticsMap IdToOperationStorage_;
};


class TSharedEventQueue
{
public:
    TSharedEventQueue(
        const std::vector<NScheduler::TExecNodePtr>& execNodes,
        int heartbeatPeriod,
        TInstant earliestTime,
        int nodeShardCount,
        TDuration maxAllowedOutrunning);

    void InsertNodeShardEvent(int workerId, TNodeShardEvent event);

    std::optional<TNodeShardEvent> PopNodeShardEvent(int workerId);

    void WaitForStrugglingNodeShards(TInstant timeBarrier);
    void UpdateControlThreadTime(TInstant time);

    void OnNodeShardSimulationFinished(int workerId);

private:
    const std::vector<TMutable<std::multiset<TNodeShardEvent>>> NodeShardEvents_;

    std::atomic<TInstant> ControlThreadTime_;
    const std::vector<TMutable<std::atomic<TInstant>>> NodeShardClocks_;

    const TDuration MaxAllowedOutrunning_;
};


class TSharedJobAndOperationCounter
{
public:
    explicit TSharedJobAndOperationCounter(int totalOperationCount);

    void OnJobStarted();

    void OnJobPreempted();

    void OnJobFinished();

    void OnOperationStarted();

    void OnOperationFinished();

    int GetRunningJobCount() const;

    int GetStartedOperationCount() const;

    int GetFinishedOperationCount() const;

    int GetTotalOperationCount() const;

    bool HasUnfinishedOperations() const;

private:
    std::atomic<int> RunningJobCount_;
    std::atomic<int> StartedOperationCount_;
    std::atomic<int> FinishedOperationCount_;
    const int TotalOperationCount_;
};

class IOperationStatisticsOutput
{
public:
    virtual void PrintEntry(NScheduler::TOperationId id, TOperationStatistics stats) = 0;

    virtual ~IOperationStatisticsOutput() = default;

protected:
    IOperationStatisticsOutput() = default;
};

class TSharedOperationStatisticsOutput
    : public IOperationStatisticsOutput
{
public:
    explicit TSharedOperationStatisticsOutput(const TString& filename);

    void PrintEntry(NScheduler::TOperationId id, TOperationStatistics stats) override;

private:
    std::ofstream OutputStream_;
    bool HeaderPrinted_ = false;
    YT_DECLARE_SPINLOCK(TAdaptiveLock, Lock_);
};


using TSharedRunningOperationsMap = TLockProtectedMap<NScheduler::TOperationId, NSchedulerSimulator::TOperationPtr>;


class TSharedSchedulerStrategy
{
public:
    TSharedSchedulerStrategy(
        const NScheduler::ISchedulerStrategyPtr& schedulerStrategy,
        TSchedulerStrategyHost& strategyHost,
        const IInvokerPtr& controlThreadInvoker);

    void ScheduleJobs(const NScheduler::ISchedulingContextPtr& schedulingContext);

    void PreemptJob(const NScheduler::TJobPtr& job);

    void ProcessJobUpdates(
        const std::vector<NScheduler::TJobUpdate>& jobUpdates,
        std::vector<std::pair<NScheduler::TOperationId, NScheduler::TJobId>>* successfullyUpdatedJobs,
        std::vector<NScheduler::TJobId>* jobsToAbort);

    void UnregisterOperation(NScheduler::IOperationStrategyHost* operation);

private:
    NScheduler::ISchedulerStrategyPtr SchedulerStrategy_;
    TSchedulerStrategyHost& StrategyHost_;
    IInvokerPtr ControlThreadInvoker_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSchedulerSimulator
