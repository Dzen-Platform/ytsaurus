#pragma once

#include "private.h"

#include <yt/yt/server/lib/scheduler/config.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

struct TPoolsUpdateResult
{
    TError Error;
    bool Updated;
};

////////////////////////////////////////////////////////////////////////////////

struct TTreeSchedulingSegmentsState
{
    ESegmentedSchedulingMode Mode = ESegmentedSchedulingMode::Disabled;
    ESchedulingSegmentModuleType ModuleType = ESchedulingSegmentModuleType::DataCenter;
    TDuration UnsatisfiedSegmentsRebalancingTimeout;
    bool ValidateInfinibandClusterTags;

    std::optional<EJobResourceType> KeyResource;
    double TotalKeyResourceAmount = 0.0;

    TSchedulingSegmentModuleList Modules;
    THashSet<TString> InfinibandClusters;
    TSegmentToResourceAmount FairResourceAmountPerSegment;
    TSegmentToFairShare FairSharePerSegment;
};

////////////////////////////////////////////////////////////////////////////////

struct TOperationIdWithSchedulingSegmentModule
{
    TOperationId OperationId;
    TSchedulingSegmentModule Module;
};

using TOperationIdWithSchedulingSegmentModuleList = std::vector<TOperationIdWithSchedulingSegmentModule>;

////////////////////////////////////////////////////////////////////////////////

struct ISchedulerTree
    : public virtual TRefCounted
{
    virtual TFairShareStrategyTreeConfigPtr GetConfig() const = 0;
    virtual bool UpdateConfig(const TFairShareStrategyTreeConfigPtr& config) = 0;
    virtual void UpdateControllerConfig(const TFairShareStrategyOperationControllerConfigPtr& config) = 0;

    virtual const TSchedulingTagFilter& GetNodesFilter() const = 0;

    virtual TFuture<std::pair<IFairShareTreeSnapshotPtr, TError>> OnFairShareUpdateAt(TInstant now) = 0;
    virtual void FinishFairShareUpdate() = 0;

    virtual bool HasOperation(TOperationId operationId) const = 0;
    virtual bool HasRunningOperation(TOperationId operationId) const = 0;
    virtual int GetOperationCount() const = 0;

    virtual void RegisterOperation(
        const TFairShareStrategyOperationStatePtr& state,
        const TStrategyOperationSpecPtr& spec,
        const TOperationFairShareTreeRuntimeParametersPtr& runtimeParameters) = 0;
    virtual void UnregisterOperation(const TFairShareStrategyOperationStatePtr& state) = 0;

    virtual void EnableOperation(const TFairShareStrategyOperationStatePtr& state) = 0;
    virtual void DisableOperation(const TFairShareStrategyOperationStatePtr& state) = 0;

    virtual void ChangeOperationPool(
        TOperationId operationId,
        const TFairShareStrategyOperationStatePtr& state,
        const TPoolName& newPool) = 0;

    virtual void UpdateOperationRuntimeParameters(
        TOperationId operationId,
        const TOperationFairShareTreeRuntimeParametersPtr& runtimeParameters) = 0;

    virtual void RegisterJobsFromRevivedOperation(TOperationId operationId, const std::vector<TJobPtr>& jobs) = 0;

    virtual TError CheckOperationIsHung(
        TOperationId operationId,
        TDuration safeTimeout,
        int minScheduleJobCallAttempts,
        const THashSet<EDeactivationReason>& deactivationReasons,
        TDuration limitingAncestorSafeTimeout) = 0;

    virtual void ProcessActivatableOperations() = 0;
    virtual void TryRunAllPendingOperations() = 0;

    virtual TPoolName CreatePoolName(const std::optional<TString>& poolFromSpec, const TString& user) const = 0;

    virtual TPoolsUpdateResult UpdatePools(const NYTree::INodePtr& poolsNode, bool forceUpdate) = 0;
    virtual TError ValidateUserToDefaultPoolMap(const THashMap<TString, TString>& userToDefaultPoolMap) = 0;

    virtual void ValidatePoolLimits(const IOperationStrategyHost* operation, const TPoolName& poolName) const = 0;
    virtual void ValidatePoolLimitsOnPoolChange(const IOperationStrategyHost* operation, const TPoolName& newPoolName) const = 0;
    virtual TFuture<void> ValidateOperationPoolsCanBeUsed(const IOperationStrategyHost* operation, const TPoolName& poolName) const = 0;

    virtual TPersistentTreeStatePtr BuildPersistentTreeState() const = 0;
    virtual void InitPersistentTreeState(const TPersistentTreeStatePtr& persistentTreeState) = 0;

    virtual ESchedulingSegment InitOperationSchedulingSegment(TOperationId operationId) = 0;
    virtual TTreeSchedulingSegmentsState GetSchedulingSegmentsState() const = 0;
    virtual TOperationIdWithSchedulingSegmentModuleList GetOperationSchedulingSegmentModuleUpdates() const = 0;

    virtual void BuildOperationAttributes(TOperationId operationId, NYTree::TFluentMap fluent) const = 0;
    virtual void BuildOperationProgress(TOperationId operationId, NYTree::TFluentMap fluent) const = 0;
    virtual void BuildBriefOperationProgress(TOperationId operationId, NYTree::TFluentMap fluent) const = 0;

    virtual void BuildStaticPoolsInformation(NYTree::TFluentAny fluent) const = 0;
    virtual void BuildUserToEphemeralPoolsInDefaultPool(NYTree::TFluentAny fluent) const = 0;

    virtual void BuildFairShareInfo(NYTree::TFluentMap fluent) const = 0;

    virtual void ActualizeEphemeralPoolParents(const THashMap<TString, TString>& userToDefaultPoolMap) = 0;

    virtual NYTree::IYPathServicePtr GetOrchidService() const = 0;

    //! Raised when operation considered running in tree.
    DECLARE_INTERFACE_SIGNAL(void(TOperationId), OperationRunning);
};

DEFINE_REFCOUNTED_TYPE(ISchedulerTree)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
