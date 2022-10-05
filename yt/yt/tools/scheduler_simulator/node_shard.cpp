#include "event_log.h"
#include "node_shard.h"
#include "operation_controller.h"

#include <yt/yt/ytlib/chunk_client/medium_directory.h>

#include <yt/yt/ytlib/scheduler/job_resources_helpers.h>

#include <yt/yt/core/concurrency/scheduler.h>

#include <yt/yt/core/yson/public.h>


namespace NYT::NSchedulerSimulator {

using namespace NScheduler;
using namespace NLogging;
using namespace NControllerAgent;
using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NNodeTrackerClient;

static const auto& Logger = SchedulerSimulatorLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

std::unique_ptr<TCompletedJobSummary> BuildCompletedJobSummary(const TJobPtr& job)
{
    TCompletedJobSummary jobSummary;
    jobSummary.Id = job->GetId();
    jobSummary.State = EJobState::Completed;
    jobSummary.FinishTime = TInstant::Now();

    return std::make_unique<TCompletedJobSummary>(std::move(jobSummary));
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

DEFINE_REFCOUNTED_TYPE(TSimulatorNodeShard)

TSimulatorNodeShard::TSimulatorNodeShard(
    const IInvokerPtr& commonNodeShardInvoker,
    TSchedulerStrategyHost* strategyHost,
    TSharedEventQueue* events,
    TSharedSchedulerStrategy* schedulingStrategy,
    TSharedOperationStatistics* operationStatistics,
    IOperationStatisticsOutput* operationStatisticsOutput,
    TSharedRunningOperationsMap* runningOperationsMap,
    TSharedJobAndOperationCounter* jobAndOperationCounter,
    const TSchedulerSimulatorConfigPtr& config,
    const TSchedulerConfigPtr& schedulerConfig,
    TInstant earliestTime,
    int shardId)
    : Events_(events)
    , StrategyHost_(strategyHost)
    , SchedulingStrategy_(schedulingStrategy)
    , OperationStatistics_(operationStatistics)
    , OperationStatisticsOutput_(operationStatisticsOutput)
    , RunningOperationsMap_(runningOperationsMap)
    , JobAndOperationCounter_(jobAndOperationCounter)
    , Config_(config)
    , SchedulerConfig_(schedulerConfig)
    , EarliestTime_(earliestTime)
    , ShardId_(shardId)
    , Invoker_(CreateSerializedInvoker(commonNodeShardInvoker))
    , Logger(NSchedulerSimulator::Logger.WithTag("ShardId: %v", shardId))
    , MediumDirectory_(CreateDefaultMediumDirectory())
{
    if (Config_->RemoteEventLog) {
        RemoteEventLogWriter_ = CreateRemoteEventLogWriter(Config_->RemoteEventLog, Invoker_);
        RemoteEventLogConsumer_ = RemoteEventLogWriter_->CreateConsumer();
    }
}

const IInvokerPtr& TSimulatorNodeShard::GetInvoker() const
{
    return Invoker_;
}

TFuture<void> TSimulatorNodeShard::AsyncRun()
{
    return BIND(&TSimulatorNodeShard::Run, MakeStrong(this))
        .AsyncVia(GetInvoker())
        .Run();
}

void TSimulatorNodeShard::RegisterNode(const NScheduler::TExecNodePtr& node)
{
    VERIFY_INVOKER_AFFINITY(GetInvoker());

    IdToNode_.emplace(node->GetId(), node);
}

void TSimulatorNodeShard::BuildNodesYson(TFluentMap fluent)
{
    VERIFY_INVOKER_AFFINITY(GetInvoker());

    for (const auto& [id, node] : IdToNode_) {
        BuildNodeYson(node, fluent);
    }
}

void TSimulatorNodeShard::Run()
{
    VERIFY_INVOKER_AFFINITY(GetInvoker());

    while (JobAndOperationCounter_->HasUnfinishedOperations()) {
        RunOnce();
        Yield();
    }

    Events_->OnNodeShardSimulationFinished(ShardId_);

    if (RemoteEventLogWriter_) {
        WaitFor(RemoteEventLogWriter_->Close())
            .ThrowOnError();
    }
}

void TSimulatorNodeShard::RunOnce()
{
    VERIFY_INVOKER_AFFINITY(GetInvoker());

    auto eventNullable = Events_->PopNodeShardEvent(ShardId_);
    if (!eventNullable) {
        return;
    }

    auto event = *eventNullable;
    switch (event.Type) {
        case EEventType::Heartbeat: {
            OnHeartbeat(event);
            break;
        }

        case EEventType::JobFinished: {
            OnJobFinished(event);
            break;
        }
    }
}

void TSimulatorNodeShard::OnHeartbeat(const TNodeShardEvent& event)
{
    const auto& node = IdToNode_[event.NodeId];

    YT_LOG_DEBUG(
        "Heartbeat started "
        "(VirtualTimestamp: %v, NodeId: %v, NodeAddress: %v, "
        "ResourceUsage: %v, JobCount: %v)",
        event.Time,
        event.NodeId,
        node->GetDefaultAddress(),
        StrategyHost_->FormatResourceUsage(
            TJobResources(node->GetResourceUsage()),
            TJobResources(node->GetResourceLimits()),
            node->GetDiskResources()),
        node->Jobs().size());

    // Prepare scheduling context.
    const auto& jobsSet = node->Jobs();
    std::vector<TJobPtr> nodeJobs(jobsSet.begin(), jobsSet.end());
    // NB(eshcherbin): We usually create a lot of simulator node shards running over a small thread pool to
    // introduce artificial contention. Thus we need to reduce the shard id to the range [0, MaxNodeShardCount).
    auto context = New<TSchedulingContext>(ShardId_ % MaxNodeShardCount, SchedulerConfig_, node, nodeJobs, MediumDirectory_);
    context->SetNow(NProfiling::InstantToCpuInstant(event.Time));

    SchedulingStrategy_->ProcessSchedulingHeartbeat(context, /*skipScheduleJobs*/ false);

    node->SetResourceUsage(context->ResourceUsage());

    // Create events for all started jobs.
    for (const auto& job : context->StartedJobs()) {
        const auto& duration = GetOrCrash(context->GetStartedJobsDurations(), job->GetId());

        // Notify scheduler.
        job->SetAllocationState(EAllocationState::Running);

        YT_LOG_DEBUG("Job started (VirtualTimestamp: %v, JobId: %v, OperationId: %v, FinishTime: %v, NodeId: %v)",
            event.Time,
            job->GetId(),
            job->GetOperationId(),
            event.Time + duration,
            event.NodeId);

        // Schedule new event.
        auto jobFinishedEvent = TNodeShardEvent::JobFinished(
            event.Time + duration,
            job,
            node,
            event.NodeId);
        Events_->InsertNodeShardEvent(ShardId_, jobFinishedEvent);

        // Update stats.
        OperationStatistics_->OnJobStarted(job->GetOperationId(), duration);

        YT_VERIFY(node->Jobs().insert(job).second);
        JobAndOperationCounter_->OnJobStarted();
    }

    // Process all preempted jobs.
    for (const auto& preemptedJob : context->PreemptedJobs()) {
        auto& job = preemptedJob.Job;
        auto duration = event.Time - job->GetStartTime();

        PreemptJob(job, Config_->EnableFullEventLog);
        auto operation = RunningOperationsMap_->Get(job->GetOperationId());
        auto controller = operation->GetControllerStrategyHost();
        controller->OnNonscheduledJobAborted(job->GetId(), EAbortReason::Preemption, TControllerEpoch{});

        // Update stats
        OperationStatistics_->OnJobPreempted(job->GetOperationId(), duration);

        JobAndOperationCounter_->OnJobPreempted();
    }

    if (!event.ScheduledOutOfBand) {
        auto nextHeartbeat = event;
        nextHeartbeat.Time += TDuration::MilliSeconds(Config_->HeartbeatPeriod);
        Events_->InsertNodeShardEvent(ShardId_, nextHeartbeat);
    }

    const auto& statistics = context->GetSchedulingStatistics();
    YT_LOG_DEBUG(
        "Heartbeat finished "
        "(VirtualTimestamp: %v, NodeId: %v, NodeAddress: %v, "
        "StartedJobs: %v, PreemptedJobs: %v, "
        "JobsScheduledDuringPreemption: %v, UnconditionallyPreemptibleJobCount: %v, UnconditionalDiscount: %v, "
        "TotalConditionalJobCount: %v, MaxConditionalJobCountPerPool: %v, MaxConditionalDiscount: %v, "
        "ControllerScheduleJobCount: %v, ScheduleJobAttemptCountPerStage: %v, "
        "OperationCountByPreemptionPriority: %v)",
        event.Time,
        event.NodeId,
        node->GetDefaultAddress(),
        context->StartedJobs().size(),
        context->PreemptedJobs().size(),
        statistics.ScheduledDuringPreemption,
        statistics.UnconditionallyPreemptibleJobCount,
        FormatResources(statistics.UnconditionalResourceUsageDiscount),
        statistics.TotalConditionallyPreemptibleJobCount,
        statistics.MaxConditionallyPreemptibleJobCountInPool,
        FormatResources(statistics.MaxConditionalResourceUsageDiscount),
        statistics.ControllerScheduleJobCount,
        statistics.ScheduleJobAttemptCountPerStage,
        statistics.OperationCountByPreemptionPriority);

}

void TSimulatorNodeShard::OnJobFinished(const TNodeShardEvent& event)
{
    auto job = event.Job;

    // When job is aborted by scheduler, events list is not updated, so aborted
    // job will still have corresponding JobFinished event that should be ignored.
    if (job->GetAllocationState() != EAllocationState::Running) {
        return;
    }

    YT_VERIFY(job->GetNode()->Jobs().erase(job) == 1);

    YT_LOG_DEBUG(
        "Job finished (VirtualTimestamp: %v, JobId: %v, OperationId: %v, NodeId: %v)",
        event.Time,
        job->GetId(),
        job->GetOperationId(),
        event.NodeId);

    JobAndOperationCounter_->OnJobFinished();

    job->SetAllocationState(EAllocationState::Finished);

    if (Config_->EnableFullEventLog) {
        LogFinishedJobFluently(ELogEventType::JobCompleted, job);
    }

    auto jobSummary = BuildCompletedJobSummary(job);

    // Notify scheduler.
    auto operation = RunningOperationsMap_->Get(job->GetOperationId());
    auto operationController = operation->GetController();
    operationController->OnJobCompleted(std::move(jobSummary));
    if (operationController->IsOperationCompleted()) {
        operation->SetState(EOperationState::Completed);
    }

    std::vector<TJobUpdate> jobUpdates({TJobUpdate{
        EJobUpdateStatus::Finished,
        job->GetOperationId(),
        job->GetId(),
        job->GetTreeId(),
        TJobResources(),
        /*jobDataCenter*/ std::nullopt,
        /*jobInfinibandCluster*/ std::nullopt,
    }});

    {
        std::vector<std::pair<TJobId, TOperationId>> jobsToRemove;
        std::vector<TJobId> jobsToAbort;
        SchedulingStrategy_->ProcessJobUpdates(
            jobUpdates,
            &jobsToRemove,
            &jobsToAbort);
        YT_VERIFY(jobsToRemove.size() == 1);
        YT_VERIFY(jobsToAbort.empty());
    }

    // Schedule out of band heartbeat.
    Events_->InsertNodeShardEvent(ShardId_, TNodeShardEvent::Heartbeat(event.Time, event.NodeId, true));

    // Update statistics.
    OperationStatistics_->OnJobFinished(operation->GetId(), event.Time - job->GetStartTime());

    const auto& node = IdToNode_[event.NodeId];
    YT_VERIFY(node == event.JobNode);
    node->SetResourceUsage(node->GetResourceUsage() - job->ResourceUsage());

    if (operation->GetState() == EOperationState::Completed && operation->SetCompleting()) {
        // Notify scheduler.
        SchedulingStrategy_->UnregisterOperation(operation.Get());

        RunningOperationsMap_->Erase(operation->GetId());

        JobAndOperationCounter_->OnOperationFinished();

        YT_LOG_INFO("Operation finished (VirtualTimestamp: %v, OperationId: %v)", event.Time, operation->GetId());

        const auto& id = operation->GetId();
        auto stats = OperationStatistics_->OnOperationFinished(
            id,
            operation->GetStartTime() - EarliestTime_,
            event.Time - EarliestTime_);
        OperationStatisticsOutput_->PrintEntry(id, std::move(stats));
    }
}

void TSimulatorNodeShard::BuildNodeYson(const TExecNodePtr& node, TFluentMap fluent)
{
    fluent
        .Item(node->GetDefaultAddress()).BeginMap()
            .Do([&] (TFluentMap fluent) {
                node->BuildAttributes(fluent);
            })
        .EndMap();
}

void TSimulatorNodeShard::PreemptJob(const NScheduler::TJobPtr& job, bool shouldLogEvent)
{
    SchedulingStrategy_->PreemptJob(job);

    if (shouldLogEvent) {
        auto fluent = LogFinishedJobFluently(ELogEventType::JobAborted, job);
        if (auto preemptedFor = job->GetPreemptedFor()) {
            fluent
                .Item("preempted_for").Value(preemptedFor);
        }
    }
}

NYson::IYsonConsumer* TSimulatorNodeShard::GetEventLogConsumer()
{
    YT_VERIFY(RemoteEventLogConsumer_);
    return RemoteEventLogConsumer_.get();
}

const NLogging::TLogger* TSimulatorNodeShard::GetEventLogger()
{
    return nullptr;
}

NEventLog::TFluentLogEvent TSimulatorNodeShard::LogFinishedJobFluently(ELogEventType eventType, const TJobPtr& job)
{
    YT_LOG_INFO("Logging job event");

    return LogEventFluently(StrategyHost_->GetEventLogger(), eventType)
        .Item("job_id").Value(job->GetId())
        .Item("operation_id").Value(job->GetOperationId())
        .Item("start_time").Value(job->GetStartTime())
        .Item("resource_limits").Value(job->ResourceLimits());
}

int GetNodeShardId(TNodeId nodeId, int nodeShardCount)
{
    return THash<TNodeId>()(nodeId) % nodeShardCount;
}

} // namespace NYT::NSchedulerSimulator
