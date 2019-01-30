#include "shared_data.h"
#include "node_shard.h"

#include <yt/server/scheduler/fair_share_strategy.h>

#include <random>


namespace NYT::NSchedulerSimulator {

////////////////////////////////////////////////////////////////////////////////

using namespace NScheduler;
using namespace NConcurrency;
using namespace NYTree;
using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

TNodeShardEvent::TNodeShardEvent(EEventType type, TInstant time)
    : Type(type)
    , Time(time)
    , OperationId(TGuid())
    , NodeId(0)
    , Job(nullptr)
{ }

TNodeShardEvent TNodeShardEvent::Heartbeat(TInstant time, TNodeId nodeId, bool scheduledOutOfBand)
{
    TNodeShardEvent event(EEventType::Heartbeat, time);
    event.NodeId = nodeId;
    event.ScheduledOutOfBand = scheduledOutOfBand;
    return event;
}

TNodeShardEvent TNodeShardEvent::JobFinished(
    TInstant time,
    const TJobPtr& job,
    const TExecNodePtr& execNode,
    TNodeId nodeId)
{
    TNodeShardEvent event(EEventType::JobFinished, time);
    event.Job = job;
    event.JobNode = execNode;
    event.NodeId = nodeId;
    return event;
}

bool operator<(const TNodeShardEvent& lhs, const TNodeShardEvent& rhs)
{
    return lhs.Time < rhs.Time;
}

////////////////////////////////////////////////////////////////////////////////

namespace {

THashMap<TOperationId, TOperationDescription> CreateOperationDescriptionByIdMap(
    const std::vector<TOperationDescription>& operations)
{
    THashMap<NScheduler::TOperationId, TOperationDescription> operationDescriptionById;
    for (const auto& operation : operations) {
        operationDescriptionById[operation.Id] = operation;
    }
    return operationDescriptionById;
}

THashMap<TOperationId, TMutable<TOperationStatistics>> CreateOperationsStorage(
    const THashMap<TOperationId, TOperationDescription>& operationDescriptionById)
{
    THashMap<TOperationId, TMutable<TOperationStatistics>> operationStorage;

    for (const auto& pair : operationDescriptionById) {
        auto operationId = pair.first;
        operationStorage.emplace(operationId, TOperationStatistics());
    }

    return operationStorage;
}

} // namespace


TSharedOperationStatistics::TSharedOperationStatistics(const std::vector<TOperationDescription>& operations)
    : OperationDescriptionById_(CreateOperationDescriptionByIdMap(operations))
    , OperationStorage_(CreateOperationsStorage(OperationDescriptionById_))
{ }

void TSharedOperationStatistics::OnJobStarted(TOperationId operationId, TDuration duration)
{
    auto& stats = OperationStorage_.at(operationId);
    {
        auto guard = Guard(stats->Lock);
        ++stats->JobCount;
        stats->JobMaxDuration = std::max(stats->JobMaxDuration, duration);
    }
}

void TSharedOperationStatistics::OnJobPreempted(TOperationId operationId, TDuration duration)
{
    auto& stats = OperationStorage_.at(operationId);
    {
        auto guard = Guard(stats->Lock);
        --stats->JobCount;
        ++stats->PreemptedJobCount;
        stats->JobsTotalDuration += duration;
        stats->PreemptedJobsTotalDuration += duration;
    }
}

void TSharedOperationStatistics::OnJobFinished(TOperationId operationId, TDuration duration)
{
    auto& stats = OperationStorage_.at(operationId);
    {
        auto guard = Guard(stats->Lock);
        stats->JobsTotalDuration += duration;
    }
}

void TSharedOperationStatistics::OnOperationStarted(TOperationId operationId)
{
    // Nothing to do.
}

TOperationStatistics TSharedOperationStatistics::OnOperationFinished(
    TOperationId operationId,
    TDuration startTime,
    TDuration finishTime)
{
    auto& stats = OperationStorage_.at(operationId);
    {
        auto guard = Guard(stats->Lock);

        stats->StartTime = startTime;
        stats->FinishTime = finishTime;

        auto it = OperationDescriptionById_.find(operationId);
        YCHECK(it != OperationDescriptionById_.end());

        stats->RealDuration = it->second.Duration;
        stats->OperationType = it->second.Type;
        stats->OperationState = it->second.State;
        stats->InTimeframe = it->second.InTimeframe;

        return std::move(*stats);
    }
}

const TOperationDescription& TSharedOperationStatistics::GetOperationDescription(TOperationId operationId) const
{
    // No synchronization needed.
    auto it = OperationDescriptionById_.find(operationId);
    YCHECK(it != OperationDescriptionById_.end());
    return it->second;
}

////////////////////////////////////////////////////////////////////////////////

TSharedEventQueue::TSharedEventQueue(
    const std::vector<TExecNodePtr>& execNodes,
    int heartbeatPeriod,
    TInstant earliestTime,
    int nodeShardCount,
    TDuration maxAllowedOutrunning)
    : NodeShardEvents_(nodeShardCount)
    , ControlThreadTime_(earliestTime)
    , NodeShardClocks_(nodeShardCount)
    , MaxAllowedOutrunning_(maxAllowedOutrunning)
{
    for (int shardId = 0; shardId < nodeShardCount; ++shardId) {
        NodeShardClocks_[shardId]->store(earliestTime);
    }

    auto heartbeatStartTime = earliestTime - TDuration::MilliSeconds(heartbeatPeriod);
    std::mt19937 random_generator;
    std::uniform_int_distribution<int> distribution(0, heartbeatPeriod - 1);

    for (const auto& execNode : execNodes) {
        const int nodeShardId = GetNodeShardId(execNode->GetId(), nodeShardCount);

        const auto heartbeatStartDelay = TDuration::MilliSeconds(distribution(random_generator));
        auto heartbeat = TNodeShardEvent::Heartbeat(
            heartbeatStartTime + heartbeatStartDelay,
            execNode->GetId(),
            false);
        InsertNodeShardEvent(nodeShardId, heartbeat);
    }
}

void TSharedEventQueue::InsertNodeShardEvent(int workerId, TNodeShardEvent event)
{
    NodeShardEvents_[workerId]->insert(event);
}

std::optional<TNodeShardEvent> TSharedEventQueue::PopNodeShardEvent(int workerId)
{
    auto& localEventsSet = NodeShardEvents_[workerId];
    if (localEventsSet->empty()) {
        NodeShardClocks_[workerId]->store(ControlThreadTime_.load() + MaxAllowedOutrunning_);
        return std::nullopt;
    }
    auto beginIt = localEventsSet->begin();
    auto event = *beginIt;

    NodeShardClocks_[workerId]->store(event.Time);
    if (event.Time > ControlThreadTime_.load() + MaxAllowedOutrunning_) {
        return std::nullopt;
    }

    localEventsSet->erase(beginIt);
    return event;
}

void TSharedEventQueue::WaitForStrugglingNodeShards(TInstant timeBarrier)
{
    for (auto& nodeShardClock : NodeShardClocks_) {
        // Actively waiting.
        while (nodeShardClock->load() < timeBarrier) {
            Yield();
        }
    }
}

void TSharedEventQueue::UpdateControlThreadTime(TInstant time)
{
    ControlThreadTime_.store(time);
}

void TSharedEventQueue::OnNodeShardSimulationFinished(int workerId)
{
    NodeShardClocks_[workerId]->store(TInstant::Max());
}

////////////////////////////////////////////////////////////////////////////////

TSharedJobAndOperationCounter::TSharedJobAndOperationCounter(int totalOperationCount)
    : RunningJobCount_(0)
    , StartedOperationCount_(0)
    , FinishedOperationCount_(0)
    , TotalOperationCount_(totalOperationCount)
{ }

void TSharedJobAndOperationCounter::OnJobStarted()
{
    ++RunningJobCount_;
}

void TSharedJobAndOperationCounter::OnJobPreempted()
{
    --RunningJobCount_;
}

void TSharedJobAndOperationCounter::OnJobFinished()
{
    --RunningJobCount_;
}

void TSharedJobAndOperationCounter::OnOperationStarted()
{
    ++StartedOperationCount_;
}

void TSharedJobAndOperationCounter::OnOperationFinished()
{
    ++FinishedOperationCount_;
}

int TSharedJobAndOperationCounter::GetRunningJobCount() const
{
    return RunningJobCount_.load();
}

int TSharedJobAndOperationCounter::GetStartedOperationCount() const
{
    return StartedOperationCount_.load();
}

int TSharedJobAndOperationCounter::GetFinishedOperationCount() const
{
    return FinishedOperationCount_.load();
}

int TSharedJobAndOperationCounter::GetTotalOperationCount() const
{
    return TotalOperationCount_;
}

bool TSharedJobAndOperationCounter::HasUnfinishedOperations() const
{
    return FinishedOperationCount_ < TotalOperationCount_;
}

////////////////////////////////////////////////////////////////////////////////

TSharedOperationStatisticsOutput::TSharedOperationStatisticsOutput(const TString& filename)
    : OutputStream_(filename)
{ }

void TSharedOperationStatisticsOutput::PrintEntry(TOperationId id, TOperationStatistics stats)
{
    auto outputGuard = Guard(Lock_);

    if (!HeaderPrinted_) {
        OutputStream_
            << "id"
            << "," << "job_count"
            << "," << "preempted_job_count"
            << "," << "start_time"
            << "," << "finish_time"
            << "," << "real_duration"
            << "," << "jobs_total_duration"
            << "," << "job_max_duration"
            << "," << "preempted_jobs_total_duration"
            << "," << "operation_type"
            << "," << "operation_state"
            << "," << "in_timeframe"
            << std::endl;

        HeaderPrinted_ = true;
    }

    OutputStream_
        << ToString(id)
        << "," << stats.JobCount
        << "," << stats.PreemptedJobCount
        << "," << stats.StartTime.ToString()
        << "," << stats.FinishTime.ToString()
        << "," << stats.RealDuration.ToString()
        << "," << stats.JobsTotalDuration.ToString()
        << "," << stats.JobMaxDuration.ToString()
        << "," << stats.PreemptedJobsTotalDuration.ToString()
        << "," << ToString(stats.OperationType)
        << "," << stats.OperationState
        << "," << stats.InTimeframe
        << std::endl;
}

////////////////////////////////////////////////////////////////////////////////

TSharedSchedulerStrategy::TSharedSchedulerStrategy(
    const ISchedulerStrategyPtr& schedulerStrategy,
    TSchedulerStrategyHost& strategyHost,
    const IInvokerPtr& controlThreadInvoker)
    : SchedulerStrategy_(schedulerStrategy)
    , StrategyHost_(strategyHost)
    , ControlThreadInvoker_(controlThreadInvoker)
{ }

void TSharedSchedulerStrategy::ScheduleJobs(const ISchedulingContextPtr& schedulingContext)
{
    WaitFor(SchedulerStrategy_->ScheduleJobs(schedulingContext))
        .ThrowOnError();
}

void TSharedSchedulerStrategy::PreemptJob(const TJobPtr& job, bool shouldLogEvent)
{
    StrategyHost_.PreemptJob(job, shouldLogEvent);
}

void TSharedSchedulerStrategy::ProcessJobUpdates(
    const std::vector<TJobUpdate>& jobUpdates,
    std::vector<std::pair<TOperationId, TJobId>>* successfullyUpdatedJobs,
    std::vector<TJobId>* jobsToAbort)
{
    int snapshotRevision;
    SchedulerStrategy_->ProcessJobUpdates(jobUpdates, successfullyUpdatedJobs, jobsToAbort, &snapshotRevision);
}

void TSharedSchedulerStrategy::UnregisterOperation(NYT::NScheduler::IOperationStrategyHost* operation)
{
    WaitFor(
        BIND(&ISchedulerStrategy::UnregisterOperation, SchedulerStrategy_, operation)
            .AsyncVia(ControlThreadInvoker_)
            .Run())
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSchedulerSimulator
