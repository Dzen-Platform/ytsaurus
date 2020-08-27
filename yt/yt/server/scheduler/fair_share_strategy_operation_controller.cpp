#include "fair_share_strategy_operation_controller.h"

#include "operation_controller.h"

#include <yt/server/lib/scheduler/config.h>

namespace NYT::NScheduler {

using namespace NConcurrency;
using namespace NProfiling;
using namespace NControllerAgent;

////////////////////////////////////////////////////////////////////////////////

TFairShareStrategyOperationController::TFairShareStrategyOperationController(
    IOperationStrategyHost* operation,
    const TFairShareStrategyOperationControllerConfigPtr& config)
    : Controller_(operation->GetControllerStrategyHost())
    , OperationId_(operation->GetId())
    , Logger(NLogging::TLogger(SchedulerLogger)
        .AddTag("OperationId: %v", OperationId_))
    , Config_(config)
    , ScheduleJobControllerThrottlingBackoff_(
        DurationToCpuDuration(Config_->ControllerThrottling->ScheduleJobStartBackoffTime))
{
    YT_VERIFY(Controller_);
}

void TFairShareStrategyOperationController::IncreaseConcurrentScheduleJobCalls(int nodeShardId)
{
    auto& shard = StateShards_[nodeShardId];
    ++shard.ConcurrentScheduleJobCalls;
}

void TFairShareStrategyOperationController::DecreaseConcurrentScheduleJobCalls(int nodeShardId)
{
    auto& shard = StateShards_[nodeShardId];
    --shard.ConcurrentScheduleJobCalls;
}

void TFairShareStrategyOperationController::IncreaseScheduleJobCallsSinceLastUpdate(int nodeShardId)
{
    auto& shard = StateShards_[nodeShardId];
    ++shard.ScheduleJobCallsSinceLastUpdate;
}

TJobResourcesWithQuotaList TFairShareStrategyOperationController::GetDetailedMinNeededJobResources() const
{
    return Controller_->GetMinNeededJobResources();
}

TJobResources TFairShareStrategyOperationController::GetAggregatedMinNeededJobResources() const
{
    // Min needed resources must be less than total needed resources of operation. See YT-9363.
    auto result = GetNeededResources();
    for (const auto& jobResources : GetDetailedMinNeededJobResources()) {
        result = Min(result, jobResources.ToJobResources());
    }
    return result;
}

void TFairShareStrategyOperationController::UpdateMinNeededJobResources()
{
    Controller_->UpdateMinNeededJobResources();
}

void TFairShareStrategyOperationController::CheckMaxScheduleJobCallsOverdraft(
    int maxScheduleJobCalls,
    bool* isMaxScheduleJobCallsViolated) const
{
    for (auto& shard : StateShards_) {
        ScheduleJobCallsOverdraft_ += shard.ScheduleJobCallsSinceLastUpdate.exchange(0);
    }
    ScheduleJobCallsOverdraft_ = std::max(0, ScheduleJobCallsOverdraft_ - maxScheduleJobCalls);

    *isMaxScheduleJobCallsViolated = ScheduleJobCallsOverdraft_ > 0;
}

bool TFairShareStrategyOperationController::IsMaxConcurrentScheduleJobCallsPerNodeShardViolated(
    const ISchedulingContextPtr& schedulingContext,
    int maxConcurrentScheduleJobCallsPerNodeShard) const
{
    auto& shard = StateShards_[schedulingContext->GetNodeShardId()];
    return shard.ConcurrentScheduleJobCalls >= maxConcurrentScheduleJobCallsPerNodeShard;
}

bool TFairShareStrategyOperationController::HasRecentScheduleJobFailure(TCpuInstant now) const
{
    return ScheduleJobBackoffDeadline_ > now;
}

void TFairShareStrategyOperationController::AbortJob(TJobId jobId, EAbortReason abortReason)
{
    Controller_->OnNonscheduledJobAborted(jobId, abortReason);
}

TControllerScheduleJobResultPtr TFairShareStrategyOperationController::ScheduleJob(
    const ISchedulingContextPtr& context,
    const TJobResources& availableResources,
    TDuration timeLimit,
    const TString& treeId)
{
    auto scheduleJobResultFuture = Controller_->ScheduleJob(context, availableResources, treeId);

    auto scheduleJobResultFutureWithTimeout = scheduleJobResultFuture
        .ToUncancelable()
        .WithTimeout(timeLimit);

    auto scheduleJobResultWithTimeoutOrError = WaitFor(scheduleJobResultFutureWithTimeout);

    if (!scheduleJobResultWithTimeoutOrError.IsOK()) {
        auto scheduleJobResult = New<TControllerScheduleJobResult>();
        if (scheduleJobResultWithTimeoutOrError.GetCode() == NYT::EErrorCode::Timeout) {
            scheduleJobResult->RecordFail(EScheduleJobFailReason::Timeout);
            // If ScheduleJob was not canceled we need to abort created job.
            scheduleJobResultFuture.Subscribe(
                BIND([this, this_ = MakeStrong(this)] (const TErrorOr<TControllerScheduleJobResultPtr>& scheduleJobResultOrError) {
                    if (!scheduleJobResultOrError.IsOK()) {
                        return;
                    }

                    const auto& scheduleJobResult = scheduleJobResultOrError.Value();
                    if (scheduleJobResult->StartDescriptor) {
                        auto jobId = scheduleJobResult->StartDescriptor->Id;
                        YT_LOG_WARNING("Aborting late job (JobId: %v)",
                            jobId);
                        AbortJob(jobId, EAbortReason::SchedulingTimeout);
                    }
            }));
        }
        return scheduleJobResult;
    }

    return scheduleJobResultWithTimeoutOrError.Value();
}

void TFairShareStrategyOperationController::OnScheduleJobFailed(
    TCpuInstant now,
    const TString& treeId,
    const TControllerScheduleJobResultPtr& scheduleJobResult)
{
    auto config = GetConfig();

    TCpuInstant backoffDeadline = 0;
    if (scheduleJobResult->Failed[EScheduleJobFailReason::ControllerThrottling] > 0) {
        auto value = ScheduleJobControllerThrottlingBackoff_.load();
        backoffDeadline = now + value;

        {
            auto newValue = std::min(
                DurationToCpuDuration(config->ControllerThrottling->ScheduleJobMaxBackoffTime),
                TCpuDuration(value * config->ControllerThrottling->ScheduleJobBackoffMultiplier));
            // Nobody cares if some of concurrent updates fail.
            ScheduleJobControllerThrottlingBackoff_.compare_exchange_weak(value, newValue);
        }

    } else {
        ScheduleJobControllerThrottlingBackoff_.store(
            DurationToCpuDuration(config->ControllerThrottling->ScheduleJobStartBackoffTime));

        if (scheduleJobResult->IsBackoffNeeded()) {
            backoffDeadline = now + DurationToCpuDuration(config->ScheduleJobFailBackoffTime);
        }
    }

    if (backoffDeadline > 0) {
        YT_LOG_DEBUG("Failed to schedule job, backing off (Duration: %v, Reasons: %v)",
            backoffDeadline - now,
            scheduleJobResult->Failed);
        ScheduleJobBackoffDeadline_.store(backoffDeadline);
    }

    if (scheduleJobResult->Failed[EScheduleJobFailReason::TentativeTreeDeclined] > 0) {
        TWriterGuard guard(SaturatedTentativeTreesLock_);
        TentativeTreeIdToSaturationTime_[treeId] = now;
    }
}

int TFairShareStrategyOperationController::GetPendingJobCount() const
{
    return Controller_->GetPendingJobCount();
}

TJobResources TFairShareStrategyOperationController::GetNeededResources() const
{
    return Controller_->GetNeededResources();
}

bool TFairShareStrategyOperationController::IsSaturatedInTentativeTree(TCpuInstant now, const TString& treeId, TDuration saturationDeactivationTimeout) const
{
    TReaderGuard guard(SaturatedTentativeTreesLock_);

    auto it = TentativeTreeIdToSaturationTime_.find(treeId);
    if (it == TentativeTreeIdToSaturationTime_.end()) {
        return false;
    }

    auto saturationTime = it->second;
    return saturationTime + DurationToCpuDuration(saturationDeactivationTimeout) > now;
}

void TFairShareStrategyOperationController::UpdateConfig(const TFairShareStrategyOperationControllerConfigPtr& config)
{
    TWriterGuard guard(ConfigLock_);

    Config_ = config;
}

TFairShareStrategyOperationControllerConfigPtr TFairShareStrategyOperationController::GetConfig()
{
    TReaderGuard guard(ConfigLock_);

    return Config_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
