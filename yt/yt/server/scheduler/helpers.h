#pragma once

#include "private.h"

#include "operation.h"

#include <yt/yt/ytlib/hive/cluster_directory.h>

#include <yt/yt/ytlib/chunk_client/helpers.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/core/yson/public.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

void BuildMinimalOperationAttributes(TOperationPtr operation, NYTree::TFluentMap fluent);
void BuildFullOperationAttributes(TOperationPtr operation, bool includeOperationId, NYTree::TFluentMap fluent);
void BuildMutableOperationAttributes(TOperationPtr operation, NYTree::TFluentMap fluent);

////////////////////////////////////////////////////////////////////////////////

EAbortReason GetAbortReason(const TError& resultError);
TJobStatus JobStatusFromError(const TError& error);

////////////////////////////////////////////////////////////////////////////////

TString MakeOperationCodicilString(TOperationId operationId);
TCodicilGuard MakeOperationCodicilGuard(TOperationId operationId);

////////////////////////////////////////////////////////////////////////////////

struct TListOperationsResult
{
    std::vector<std::pair<TOperationId, EOperationState>> OperationsToRevive;
    std::vector<TOperationId> OperationsToArchive;
    std::vector<TOperationId> OperationsToRemove;
};

TListOperationsResult ListOperations(
    TCallback<NObjectClient::TObjectServiceProxy::TReqExecuteBatchPtr()> createBatchRequest);

////////////////////////////////////////////////////////////////////////////////

TJobResources ComputeAvailableResources(
    const TJobResources& resourceLimits,
    const TJobResources& resourceUsage,
    const TJobResources& resourceDiscount);

////////////////////////////////////////////////////////////////////////////////

TOperationFairShareTreeRuntimeParametersPtr GetSchedulingOptionsPerPoolTree(IOperationStrategyHost* operation, const TString& treeId);

////////////////////////////////////////////////////////////////////////////////

TString GuessGpuType(const TString& treeId);

std::vector<std::pair<TInstant, TInstant>> SplitTimeIntervalByHours(TInstant startTime, TInstant finishTime);

////////////////////////////////////////////////////////////////////////////////

THashSet<int> GetDiskQuotaMedia(const TDiskQuota& diskQuota);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler

