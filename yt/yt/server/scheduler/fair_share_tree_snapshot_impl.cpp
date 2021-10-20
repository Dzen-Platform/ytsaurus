#include "fair_share_tree_snapshot_impl.h"


namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////
TFairShareTreeSnapshotImpl::TFairShareTreeSnapshotImpl(
    TTreeSnapshotId id,
    TSchedulerRootElementPtr rootElement,
    TNonOwningOperationElementMap enabledOperationIdToElement,
    TNonOwningOperationElementMap disabledOperationIdToElement,
    TNonOwningPoolElementMap poolNameToElement,
    const TCachedJobPreemptionStatuses& cachedJobPreemptionStatuses,
    TFairShareStrategyTreeConfigPtr treeConfig,
    TFairShareStrategyOperationControllerConfigPtr controllerConfig,
    TTreeSchedulingSegmentsState schedulingSegmentsState)
    : Id_(id)
    , RootElement_(std::move(rootElement))
    , EnabledOperationMap_(std::move(enabledOperationIdToElement))
    , DisabledOperationMap_(std::move(disabledOperationIdToElement))
    , PoolMap_(std::move(poolNameToElement))
    , TreeConfig_(std::move(treeConfig))
    , ControllerConfig_(std::move(controllerConfig))
    , SchedulingSegmentsState_(std::move(schedulingSegmentsState))
    , CachedJobPreemptionStatuses_(cachedJobPreemptionStatuses)
{ }

TSchedulerPoolElement* TFairShareTreeSnapshotImpl::FindPool(const TString& poolName) const
{
    auto it = PoolMap_.find(poolName);
    return it != PoolMap_.end() ? it->second : nullptr;
}

TSchedulerOperationElement* TFairShareTreeSnapshotImpl::FindEnabledOperationElement(TOperationId operationId) const
{
    auto it = EnabledOperationMap_.find(operationId);
    return it != EnabledOperationMap_.end() ? it->second : nullptr;
}

TSchedulerOperationElement* TFairShareTreeSnapshotImpl::FindDisabledOperationElement(TOperationId operationId) const
{
    auto it = DisabledOperationMap_.find(operationId);
    return it != DisabledOperationMap_.end() ? it->second : nullptr;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
