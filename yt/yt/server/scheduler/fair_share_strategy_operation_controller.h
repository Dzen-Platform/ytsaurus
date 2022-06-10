#pragma once

#include "private.h"

#include <yt/yt/core/misc/atomic_object.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TFairShareStrategyOperationController
    : public TRefCounted
{
public:
    TFairShareStrategyOperationController(
        IOperationStrategyHost* operation,
        const TFairShareStrategyOperationControllerConfigPtr& config,
        int NodeShardCount);

    // TODO(eshcherbin): Use scheduling context instead of node shard ID.
    void DecreaseConcurrentScheduleJobCalls(int nodeShardId);
    void IncreaseConcurrentScheduleJobCalls(int nodeShardId);
    void IncreaseScheduleJobCallsSinceLastUpdate(int nodeShardId);

    TControllerEpoch GetEpoch() const;

    TCompositeNeededResources GetNeededResources() const;
    TJobResourcesWithQuotaList GetDetailedMinNeededJobResources() const;
    TJobResources GetAggregatedMinNeededJobResources() const;

    void UpdateMinNeededJobResources();

    void ComputeMaxConcurrentControllerScheduleJobCallsPerNodeShard();
    int GetMaxConcurrentControllerScheduleJobCallsPerNodeShard() const;
    void CheckMaxScheduleJobCallsOverdraft(int maxScheduleJobCalls, bool* isMaxScheduleJobCallsViolated) const;
    // TODO(eshcherbin): Remove unnecessary second argument.
    bool IsMaxConcurrentScheduleJobCallsPerNodeShardViolated(
        const ISchedulingContextPtr& schedulingContext,
        int maxConcurrentScheduleJobCallsPerNodeShard) const;
    bool HasRecentScheduleJobFailure(NProfiling::TCpuInstant now) const;

    TControllerScheduleJobResultPtr ScheduleJob(
        const ISchedulingContextPtr& schedulingContext,
        const TJobResources& availableResources,
        TDuration timeLimit,
        const TString& treeId,
        const TString& poolPath,
        const TFairShareStrategyTreeConfigPtr& treeConfig);

    void AbortJob(
        TJobId jobId,
        EAbortReason abortReason,
        TControllerEpoch jobEpoch);

    void OnScheduleJobFailed(
        NProfiling::TCpuInstant now,
        const TString& treeId,
        const TControllerScheduleJobResultPtr& scheduleJobResult);

    bool IsSaturatedInTentativeTree(
        NProfiling::TCpuInstant now,
        const TString& treeId,
        TDuration saturationDeactivationTimeout) const;

    void UpdateConfig(const TFairShareStrategyOperationControllerConfigPtr& config);
    TFairShareStrategyOperationControllerConfigPtr GetConfig();

private:
    const IOperationControllerStrategyHostPtr Controller_;
    const TOperationId OperationId_;

    const NLogging::TLogger Logger;

    NThreading::TReaderWriterSpinLock ConfigLock_;
    TAtomicObject<TFairShareStrategyOperationControllerConfigPtr> Config_;

    struct TStateShard
    {
        mutable std::atomic<int> ScheduleJobCallsSinceLastUpdate = 0;
        char Padding[64];
        int ConcurrentScheduleJobCalls = 0;
    };
    std::array<TStateShard, MaxNodeShardCount> StateShards_;

    const int NodeShardCount_;
    int MaxConcurrentControllerScheduleJobCallsPerNodeShard;
    mutable int ScheduleJobCallsOverdraft_ = 0;

    std::atomic<NProfiling::TCpuDuration> ScheduleJobControllerThrottlingBackoff_;
    std::atomic<NProfiling::TCpuInstant> ScheduleJobBackoffDeadline_ = ::Min<NProfiling::TCpuInstant>();

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, SaturatedTentativeTreesLock_);
    THashMap<TString, NProfiling::TCpuInstant> TentativeTreeIdToSaturationTime_;
};

DEFINE_REFCOUNTED_TYPE(TFairShareStrategyOperationController)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
