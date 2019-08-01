#include "operation.h"
#include "operation_controller.h"
#include "exec_node.h"
#include "helpers.h"
#include "job.h"
#include "controller_agent.h"

#include <yt/ytlib/scheduler/helpers.h>
#include <yt/ytlib/scheduler/config.h>

#include <yt/ytlib/api/native/connection.h>
#include <yt/ytlib/api/native/client.h>

#include <yt/client/api/transaction.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/actions/cancelable_context.h>

namespace NYT::NScheduler {

using namespace NApi;
using namespace NTransactionClient;
using namespace NJobTrackerClient;
using namespace NObjectClient;
using namespace NRpc;
using namespace NYTree;
using namespace NYson;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TOperationEvent& event, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("time").Value(event.Time)
            .Item("state").Value(event.State)
        .EndMap();
}

void Deserialize(TOperationEvent& event, INodePtr node)
{
    auto mapNode = node->AsMap();
    event.Time = ConvertTo<TInstant>(mapNode->GetChild("time"));
    event.State = ConvertTo<EOperationState>(mapNode->GetChild("state"));
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(
    NControllerAgent::NProto::TControllerTransactionIds* transactionIdsProto,
    const TOperationTransactions& transactions)
{
    auto getId = [] (const NApi::ITransactionPtr& transaction) {
        return transaction ? transaction->GetId() : NTransactionClient::TTransactionId();
    };

    ToProto(transactionIdsProto->mutable_async_id(), getId(transactions.AsyncTransaction));
    ToProto(transactionIdsProto->mutable_input_id(), getId(transactions.InputTransaction));
    ToProto(transactionIdsProto->mutable_output_id(), getId(transactions.OutputTransaction));
    ToProto(transactionIdsProto->mutable_debug_id(), getId(transactions.DebugTransaction));
    ToProto(transactionIdsProto->mutable_output_completion_id(), getId(transactions.OutputCompletionTransaction));
    ToProto(transactionIdsProto->mutable_debug_completion_id(), getId(transactions.DebugCompletionTransaction));

    for (const auto& transaction : transactions.NestedInputTransactions) {
        ToProto(transactionIdsProto->add_nested_input_ids(), getId(transaction));
    }
}

void FromProto(
    TOperationTransactions* transactions,
    const NControllerAgent::NProto::TControllerTransactionIds& transactionIdsProto,
    std::function<NNative::IClientPtr(TCellTag)> getClient,
    TDuration pingPeriod)
{
    auto attachTransaction = [&] (TTransactionId transactionId) -> ITransactionPtr {
        if (!transactionId) {
            return nullptr;
        }

        auto client = getClient(CellTagFromId(transactionId));

        TTransactionAttachOptions options;
        options.Ping = true;
        options.PingAncestors = false;
        options.PingPeriod = pingPeriod;
        return client->AttachTransaction(transactionId, options);
    };

    transactions->AsyncTransaction = attachTransaction(FromProto<TTransactionId>(transactionIdsProto.async_id()));
    transactions->InputTransaction = attachTransaction(FromProto<TTransactionId>(transactionIdsProto.input_id()));
    transactions->OutputTransaction = attachTransaction(FromProto<TTransactionId>(transactionIdsProto.output_id()));
    transactions->DebugTransaction = attachTransaction(FromProto<TTransactionId>(transactionIdsProto.debug_id()));
    transactions->OutputCompletionTransaction = attachTransaction(FromProto<TTransactionId>(transactionIdsProto.output_completion_id()));
    transactions->DebugCompletionTransaction = attachTransaction(FromProto<TTransactionId>(transactionIdsProto.debug_completion_id()));

    auto nestedInputTransactionIds = FromProto<std::vector<TTransactionId>>(transactionIdsProto.nested_input_ids());
    for (auto transactionId : nestedInputTransactionIds) {
        transactions->NestedInputTransactions.push_back(attachTransaction(transactionId));
    }
}

////////////////////////////////////////////////////////////////////////////////

TOperation::TOperation(
    TOperationId id,
    EOperationType type,
    TMutationId mutationId,
    TTransactionId userTransactionId,
    TOperationSpecBasePtr spec,
    TYsonString specString,
    IMapNodePtr annotations,
    IMapNodePtr secureVault,
    TOperationRuntimeParametersPtr runtimeParams,
    NSecurityClient::TSerializableAccessControlList baseAcl,
    const TString& authenticatedUser,
    TInstant startTime,
    IInvokerPtr controlInvoker,
    const std::optional<TString>& alias,
    EOperationState state,
    const std::vector<TOperationEvent>& events,
    bool suspended,
    std::vector<TString> erasedTrees)
    : MutationId_(mutationId)
    , State_(state)
    , Suspended_(suspended)
    , UserTransactionId_(userTransactionId)
    , RuntimeData_(New<TOperationRuntimeData>())
    , SecureVault_(std::move(secureVault))
    , Events_(events)
    , Spec_(std::move(spec))
    , SuspiciousJobs_(NYson::TYsonString(TString(), NYson::EYsonType::MapFragment))
    , Alias_(alias)
    , Annotations_(std::move(annotations))
    , BaseAcl_(std::move(baseAcl))
    , Id_(id)
    , Type_(type)
    , StartTime_(startTime)
    , AuthenticatedUser_(authenticatedUser)
    , SpecString_(specString)
    , CodicilData_(MakeOperationCodicilString(Id_))
    , ControlInvoker_(std::move(controlInvoker))
    , RuntimeParameters_(std::move(runtimeParams))
    , ErasedTrees_(std::move(erasedTrees))
{
    YT_VERIFY(SpecString_);
    Restart();
}

EOperationType TOperation::GetType() const
{
    return Type_;
}

TOperationId TOperation::GetId() const
{
    return Id_;
}

TInstant TOperation::GetStartTime() const
{
    return StartTime_;
}

TString TOperation::GetAuthenticatedUser() const
{
    return AuthenticatedUser_;
}

const TYsonString& TOperation::GetSpecString() const
{
    return SpecString_;
}

TFuture<TOperationPtr> TOperation::GetStarted()
{
    return StartedPromise_.ToFuture().Apply(BIND([this_ = MakeStrong(this)] () -> TOperationPtr {
        return this_;
    }));
}

void TOperation::SetStarted(const TError& error)
{
    StartedPromise_.Set(error);
}

TFuture<void> TOperation::GetFinished()
{
    return FinishedPromise_;
}

void TOperation::SetFinished()
{
    FinishedPromise_.Set();
    Suspended_ = false;
    for (auto& pair : Alerts_) {
        NConcurrency::TDelayedExecutor::CancelAndClear(pair.second.ResetCookie);
    }
    Alerts_.clear();
}

bool TOperation::IsFinishedState() const
{
    return IsOperationFinished(State_);
}

bool TOperation::IsFinishingState() const
{
    return IsOperationFinishing(State_);
}

bool TOperation::IsSchedulable() const
{
    return State_ == EOperationState::Running && !Suspended_;
}

IOperationControllerStrategyHostPtr TOperation::GetControllerStrategyHost() const
{
    return Controller_;
}

TCodicilGuard TOperation::MakeCodicilGuard() const
{
    return TCodicilGuard(CodicilData_);
}

void TOperation::SetStateAndEnqueueEvent(EOperationState state)
{
    State_ = state;
    Events_.emplace_back(TOperationEvent({TInstant::Now(), state}));
    ShouldFlush_ = true;
}

void TOperation::SetSlotIndex(const TString& treeId, int value)
{
    TreeIdToSlotIndex_.emplace(treeId, value);
}

std::optional<int> TOperation::FindSlotIndex(const TString& treeId) const
{
    auto it = TreeIdToSlotIndex_.find(treeId);
    return it != TreeIdToSlotIndex_.end() ? std::make_optional(it->second) : std::nullopt;
}

int TOperation::GetSlotIndex(const TString& treeId) const
{
    auto slotIndex = FindSlotIndex(treeId);
    YT_VERIFY(slotIndex);
    return *slotIndex;
}

const THashMap<TString, int>& TOperation::GetSlotIndices() const
{
    return TreeIdToSlotIndex_;
}

TOperationRuntimeParametersPtr TOperation::GetRuntimeParameters() const
{
    return RuntimeParameters_;
}

bool TOperation::GetActivated() const
{
    return Activated_;
}

void TOperation::SetActivated(bool value)
{
    Activated_ = value;
};

void TOperation::SetRuntimeParameters(TOperationRuntimeParametersPtr parameters)
{
    if (parameters->Acl != RuntimeParameters_->Acl) {
        SetShouldFlushAcl(true);
    }
    SetShouldFlush(true);
    RuntimeParameters_ = std::move(parameters);
}

TYsonString TOperation::BuildAlertsString() const
{
    auto result = BuildYsonStringFluently()
        .DoMapFor(Alerts_, [&] (TFluentMap fluent, const auto& pair) {
            const auto& alertType = pair.first;
            const auto& alert = pair.second;

            fluent
                .Item(FormatEnum(alertType)).Value(alert.Error);
        });

    return result;
}

bool TOperation::HasAlert(EOperationAlertType alertType) const
{
    return Alerts_.find(alertType) != Alerts_.end();
}

void TOperation::SetAlert(EOperationAlertType alertType, const TError& error, std::optional<TDuration> timeout)
{
    auto& alert = Alerts_[alertType];

    if (alert.Error.Sanitize() == error.Sanitize()) {
        return;
    }

    alert.Error = error;
    NConcurrency::TDelayedExecutor::CancelAndClear(alert.ResetCookie);

    if (timeout) {
        auto resetCallback = BIND(&TOperation::ResetAlert, MakeStrong(this), alertType)
            .Via(CancelableInvoker_);

        alert.ResetCookie = NConcurrency::TDelayedExecutor::Submit(resetCallback, *timeout);
    }

    ShouldFlush_ = true;
}

void TOperation::ResetAlert(EOperationAlertType alertType)
{
    auto it = Alerts_.find(alertType);
    if (it == Alerts_.end()) {
        return;
    }
    NConcurrency::TDelayedExecutor::CancelAndClear(it->second.ResetCookie);
    Alerts_.erase(it);
    ShouldFlush_ = true;
}

const IInvokerPtr& TOperation::GetCancelableControlInvoker()
{
    return CancelableInvoker_;
}

void TOperation::Cancel()
{
    if (CancelableContext_) {
        CancelableContext_->Cancel();
    }
}

void TOperation::Restart()
{
    Cancel();
    CancelableContext_ = New<TCancelableContext>();
    CancelableInvoker_ = CancelableContext_->CreateInvoker(ControlInvoker_);
}

TYsonString TOperation::BuildResultString() const
{
    auto error = NYT::FromProto<TError>(Result_.error());
    return BuildYsonStringFluently()
        .BeginMap()
            .Item("error").Value(error)
        .EndMap();
}

void TOperation::SetAgent(const TControllerAgentPtr& agent)
{
    Agent_ = agent;
}

TControllerAgentPtr TOperation::GetAgentOrCancelFiber()
{
    auto agent = Agent_.Lock();
    if (!agent) {
        throw NConcurrency::TFiberCanceledException();
    }
    return agent;
}

TControllerAgentPtr TOperation::FindAgent()
{
    return Agent_.Lock();
}

TControllerAgentPtr TOperation::GetAgentOrThrow()
{
    auto agent = FindAgent();
    if (!agent) {
        THROW_ERROR_EXCEPTION("Operation %v is not assigned to any agent",
            Id_);
    }
    return agent;
}

void TOperation::SetErasedTrees(std::vector<TString> erasedTrees)
{
    ErasedTrees_ = std::move(erasedTrees);
}

const std::vector<TString>& TOperation::ErasedTrees() const
{
    return ErasedTrees_;
}

void TOperation::EraseTree(const TString& treeId)
{
    ErasedTrees_.push_back(treeId);
}

////////////////////////////////////////////////////////////////////////////////

int TOperationRuntimeData::GetPendingJobCount() const
{
    return PendingJobCount_.load();
}

void TOperationRuntimeData::SetPendingJobCount(int value)
{
    PendingJobCount_.store(value);
}

NScheduler::TJobResources TOperationRuntimeData::GetNeededResources()
{
    NConcurrency::TReaderGuard guard(NeededResourcesLock_);
    return NeededResources_;
}

void TOperationRuntimeData::SetNeededResources(const NScheduler::TJobResources& value)
{
    NConcurrency::TWriterGuard guard(NeededResourcesLock_);
    NeededResources_ = value;
}

TJobResourcesWithQuotaList TOperationRuntimeData::GetMinNeededJobResources() const
{
    NConcurrency::TReaderGuard guard(MinNeededResourcesJobLock_);
    return MinNeededJobResources_;
}

void TOperationRuntimeData::SetMinNeededJobResources(const TJobResourcesWithQuotaList& value)
{
    NConcurrency::TWriterGuard guard(MinNeededResourcesJobLock_);
    MinNeededJobResources_ = value;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler

