#pragma once

#include "public.h"

#include "fair_share_tree_snapshot.h"

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

// Manages profiling data of fair share tree.
class TFairShareTreeProfileManager
    : public TRefCounted
{
public:
    TFairShareTreeProfileManager(
        const TString& treeId,
        bool sparsifyMetrics,
        const IInvokerPtr& profilingInvoker);

    // Thread affinity: Control thread.
    NProfiling::TProfiler GetProfiler() const;

    // Thread affinity: Control thread.
    void ProfileOperationUnregistration(const TSchedulerCompositeElement* pool, EOperationState state);

    // Thread affinity: Control thread.
    void RegisterPool(const TSchedulerCompositeElementPtr& element);
    void UnregisterPool(const TSchedulerCompositeElementPtr& element);

    // Thread affinity: Profiler thread.
    void ProfileElements(
        const TFairShareTreeSnapshotPtr& treeSnapshot,
        const THashMap<TOperationId, TResourceVolume>& operationIdToAccumulatedResourceUsage);

    // Thread affinity: Profiler thread.
    void ApplyJobMetricsDelta(
        const TFairShareTreeSnapshotPtr& treeSnapshot,
        const THashMap<TOperationId, TJobMetrics>& jobMetricsPerOperation);

    // Thread affinity: Profiler thread.
    void ApplyScheduledAndPreemptedResourcesDelta(
        const TFairShareTreeSnapshotPtr& treeSnapshot,
        const THashMap<std::optional<EJobSchedulingStage>, TOperationIdToJobResources>& operationIdWithStageToScheduledJobResourcesDeltas,
        const TEnumIndexedVector<EJobPreemptionReason, TOperationIdToJobResources>& operationIdWithReasonToPreemptedJobResourcesDeltas,
        const TEnumIndexedVector<EJobPreemptionReason, TOperationIdToJobResources>& operationIdWithReasonToPreemptedJobResourceTimeDeltas,
        const TEnumIndexedVector<EJobPreemptionReason, TOperationIdToJobResources>& operationIdWithReasonToImproperlyPreemptedJobResourcesDeltas);

private:
    const NProfiling::TProfiler Profiler_;
    const bool SparsifyMetrics_;
    const IInvokerPtr ProfilingInvoker_;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

    struct TUnregisterOperationCounters
    {
        TEnumIndexedVector<EOperationState, NProfiling::TCounter> FinishedCounters;
        NProfiling::TCounter BannedCounter;
    };
    THashMap<TString, TUnregisterOperationCounters> PoolToUnregisterOperationCounters_;

    struct TOperationUserProfilingTag
    {
        TString PoolId;
        TString UserName;
        std::optional<TString> CustomTag;

        bool operator == (const TOperationUserProfilingTag& other) const;
        bool operator != (const TOperationUserProfilingTag& other) const;
    };

    struct TOperationProfilingEntry
    {
        int SlotIndex;
        TString ParentPoolId;
        std::vector<TOperationUserProfilingTag> UserProfilingTags;

        NProfiling::TBufferedProducerPtr BufferedProducer;
    };

    struct TPoolProfilingEntry
    {
        TUnregisterOperationCounters UnregisterOperationCounters;

        // We postpone deletion to avoid ABA problem with pool deletion and immediate creation.
        std::optional<TInstant> RemoveTime;

        NProfiling::TBufferedProducerPtr BufferedProducer;
    };

    NProfiling::TGauge PoolCountGauge_;
    NProfiling::TGauge TotalElementCountGauge_;

    THashMap<TString, TJobMetrics> JobMetricsMap_;
    THashMap<std::optional<EJobSchedulingStage>, THashMap<TString, TJobResources>> ScheduledResourcesByStageMap_;
    TEnumIndexedVector<EJobPreemptionReason, THashMap<TString, TJobResources>> PreemptedResourcesByReasonMap_;
    TEnumIndexedVector<EJobPreemptionReason, THashMap<TString, TJobResources>> PreemptedResourceTimesByReasonMap_;
    TEnumIndexedVector<EJobPreemptionReason, THashMap<TString, TJobResources>> ImproperlyPreemptedResourcesByReasonMap_;

    THashMap<TOperationId, TOperationProfilingEntry> OperationIdToProfilingEntry_;

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, PoolNameToProfilingEntryLock_);
    THashMap<TString, TPoolProfilingEntry> PoolNameToProfilingEntry_;

    NProfiling::TBufferedProducerPtr DistributedResourcesBufferedProducer_;

    void RegisterPoolProfiler(const TString& poolName);

    void PrepareOperationProfilingEntries(const TFairShareTreeSnapshotPtr& treeSnapshot);

    void CleanupPoolProfilingEntries();

    void ProfileOperations(
        const TFairShareTreeSnapshotPtr& treeSnapshot,
        const THashMap<TOperationId, TResourceVolume>& operationIdToAccumulatedResourceUsage);
    void ProfilePools(const TFairShareTreeSnapshotPtr& treeSnapshot);

    void ProfilePool(
        const TSchedulerCompositeElement* element,
        const TFairShareStrategyTreeConfigPtr& treeConfig,
        const NProfiling::TBufferedProducerPtr& producer);

    void ProfileElement(
        NProfiling::ISensorWriter* writer,
        const TSchedulerElement* element,
        const TFairShareStrategyTreeConfigPtr& treeConfig);

    void ProfileDistributedResources(const TFairShareTreeSnapshotPtr& treeSnapshot);
};

DEFINE_REFCOUNTED_TYPE(TFairShareTreeProfileManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
