#pragma once

#include "private.h"

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TFairShareStrategyOperationController
    : public TIntrinsicRefCounted
{
public:
    explicit TFairShareStrategyOperationController(IOperationStrategyHost* operation);

    void DecreaseConcurrentScheduleJobCalls();
    void IncreaseConcurrentScheduleJobCalls();

    void SetLastScheduleJobFailTime(NProfiling::TCpuInstant now);

    TJobResourcesWithQuotaList GetDetailedMinNeededJobResources() const;
    TJobResources GetAggregatedMinNeededJobResources() const;
    void UpdateMinNeededJobResources();

    bool IsBlocked(
        NProfiling::TCpuInstant now,
        int maxConcurrentScheduleJobCalls,
        TDuration scheduleJobFailBackoffTime) const;

    TControllerScheduleJobResultPtr ScheduleJob(
        const ISchedulingContextPtr& schedulingContext,
        const TJobResources& availableResources,
        TDuration timeLimit,
        const TString& treeId);

    void AbortJob(
        TJobId jobId,
        EAbortReason abortReason);

    int GetPendingJobCount() const;
    TJobResources GetNeededResources() const;

    void OnTentativeTreeScheduleJobFailed(NProfiling::TCpuInstant now, const TString& treeId);
    bool IsSaturatedInTentativeTree(NProfiling::TCpuInstant now, const TString& treeId, TDuration saturationDeactivationTimeout) const;

private:
    const IOperationControllerStrategyHostPtr Controller_;
    const TOperationId OperationId_;

    const NLogging::TLogger Logger;

    mutable std::atomic<bool> Blocked_ = {false};
    std::atomic<int> ConcurrentScheduleJobCalls_ = {0};
    std::atomic<NProfiling::TCpuInstant> LastScheduleJobFailTime_ = ::Min<NProfiling::TCpuInstant>();

    NConcurrency::TReaderWriterSpinLock SaturatedTentativeTreesLock_;
    THashMap<TString, NProfiling::TCpuInstant> TentativeTreeIdToSaturationTime_;
};

DEFINE_REFCOUNTED_TYPE(TFairShareStrategyOperationController)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
