#pragma once

#include "private.h"

#include "operation.h"

#include <yt/yt/ytlib/hive/cluster_directory.h>

#include <yt/yt/ytlib/chunk_client/helpers.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/core/yson/public.h>
#include <yt/yt/core/yson/forwarding_consumer.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

void BuildMinimalOperationAttributes(TOperationPtr operation, NYTree::TFluentMap fluent);
void BuildFullOperationAttributes(TOperationPtr operation, bool includeOperationId, NYTree::TFluentMap fluent);
void BuildMutableOperationAttributes(TOperationPtr operation, NYTree::TFluentMap fluent);

////////////////////////////////////////////////////////////////////////////////

TString MakeOperationCodicilString(TOperationId operationId);
TCodicilGuard MakeOperationCodicilGuard(TOperationId operationId);

////////////////////////////////////////////////////////////////////////////////

struct TListOperationsResult
{
    std::vector<std::pair<TOperationId, EOperationState>> OperationsToRevive;
    std::vector<TOperationId> OperationsToArchive;
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

void BuildSupportedFeatures(NYTree::TFluentMap fluent);

////////////////////////////////////////////////////////////////////////////////

TString GuessGpuType(const TString& treeId);

std::vector<std::pair<TInstant, TInstant>> SplitTimeIntervalByHours(TInstant startTime, TInstant finishTime);

////////////////////////////////////////////////////////////////////////////////

THashSet<int> GetDiskQuotaMedia(const TDiskQuota& diskQuota);

////////////////////////////////////////////////////////////////////////////////

class TYsonMapFragmentBatcher final
    : public NYson::TForwardingYsonConsumer
    , public NYson::IFlushableYsonConsumer
    , private TNonCopyable
{
public:
    TYsonMapFragmentBatcher(
        std::vector<NYson::TYsonString>* batchOutput,
        int maxBatchSize,
        NYson::EYsonFormat format = NYson::EYsonFormat::Binary);

    //! Flushes current batch if it's non-empty.
    void Flush() override;

protected:
    void OnMyKeyedItem(TStringBuf key) override;

private:
    std::vector<NYson::TYsonString>* const BatchOutput_;
    const int MaxBatchSize_;

    int BatchSize_ = 0;
    TStringStream BatchStream_;
    std::unique_ptr<NYson::IFlushableYsonConsumer> BatchWriter_;
};

////////////////////////////////////////////////////////////////////////////////

struct TSchedulerTreeAlertDescriptor
{
    ESchedulerAlertType Type;
    TString Message;
};

const std::vector<TSchedulerTreeAlertDescriptor>& GetSchedulerTreeAlertDescriptors();

bool IsSchedulerTreeAlertType(ESchedulerAlertType alertType);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
