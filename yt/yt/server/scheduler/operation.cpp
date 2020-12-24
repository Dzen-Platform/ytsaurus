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
using namespace NSecurityClient;
using namespace NRpc;
using namespace NYTree;
using namespace NYson;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TOperationEvent& event, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("time").Value(event.Time)
            .Item("state").Value(event.State)
            .Item("attributes").Value(event.Attributes)
        .EndMap();
}

void Deserialize(TOperationEvent& event, INodePtr node)
{
    auto mapNode = node->AsMap();
    event.Time = ConvertTo<TInstant>(mapNode->GetChildOrThrow("time"));
    event.State = ConvertTo<EOperationState>(mapNode->GetChildOrThrow("state"));
    event.Attributes = ConvertTo<THashMap<TString, TString>>(mapNode->GetChildOrThrow("attributes"));
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
    THashMap<TTransactionId, ITransactionPtr> transactionIdToTransaction;
    auto attachTransaction = [&] (TTransactionId transactionId) -> ITransactionPtr {
        if (!transactionId) {
            return nullptr;
        }

        auto it = transactionIdToTransaction.find(transactionId);
        if (it == transactionIdToTransaction.end()) {
            auto client = getClient(CellTagFromId(transactionId));

            TTransactionAttachOptions options;
            options.Ping = true;
            options.PingAncestors = false;
            options.PingPeriod = pingPeriod;

            auto transaction = client->AttachTransaction(transactionId, options);
            YT_VERIFY(transactionIdToTransaction.emplace(transactionId, transaction).second);
            return transaction;
        } else {
            return it->second;
        }
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
    THashMap<TString, TStrategyOperationSpecPtr> customSpecPerTree,
    TYsonString specString,
    IMapNodePtr secureVault,
    TOperationRuntimeParametersPtr runtimeParameters,
    NSecurityClient::TSerializableAccessControlList baseAcl,
    const TString& authenticatedUser,
    TInstant startTime,
    IInvokerPtr controlInvoker,
    const std::optional<TString>& alias,
    bool isScheduledInSingleTree,
    EOperationState state,
    const std::vector<TOperationEvent>& events,
    bool suspended,
    const std::optional<TJobResources>& initialAggregatedMinNeededResources)
    : MutationId_(mutationId)
    , Suspended_(suspended)
    , UserTransactionId_(userTransactionId)
    , ControllerData_(New<TOperationControllerData>())
    , SecureVault_(std::move(secureVault))
    , Events_(events)
    , Spec_(std::move(spec))
    , SuspiciousJobs_(NYson::TYsonString(TString(), NYson::EYsonType::MapFragment))
    , Alias_(alias)
    , BaseAcl_(std::move(baseAcl))
    , InitialAggregatedMinNeededResources_(initialAggregatedMinNeededResources)
    , Id_(id)
    , Type_(type)
    , StartTime_(startTime)
    , AuthenticatedUser_(authenticatedUser)
    , SpecString_(specString)
    , CustomSpecPerTree_(std::move(customSpecPerTree))
    , CodicilData_(MakeOperationCodicilString(Id_))
    , ControlInvoker_(std::move(controlInvoker))
    , State_(state)
    , RuntimeParameters_(std::move(runtimeParameters))
    , IsScheduledInSingleTree_(isScheduledInSingleTree)
{
    // COMPAT(gritukan)
    auto annotations = Spec_->Annotations;
    auto description = Spec_->Description;
    if (description) {
        if (!annotations) {
            annotations = GetEphemeralNodeFactory()->CreateMap();
        }
        annotations->AddChild("description", description);
    }

    if (annotations && !RuntimeParameters_->Annotations) {
        RuntimeParameters_->Annotations = annotations;
    }

    YT_VERIFY(SpecString_);
    Restart(TError()); // error is fake
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

TStrategyOperationSpecPtr TOperation::GetStrategySpec() const
{
    return Spec_;
}
    
TStrategyOperationSpecPtr TOperation::GetStrategySpecForTree(const TString& treeId) const
{
    auto it = CustomSpecPerTree_.find(treeId);
    if (it != CustomSpecPerTree_.end()) {
        return it->second;
    } else {
        return Spec_;
    }
}

const TYsonString& TOperation::GetSpecString() const
{
    return SpecString_;
}

std::vector<TString> TOperation::GetTaskNames() const
{
    if (Type_ != EOperationType::Vanilla) {
        return {};
    }

    TVanillaOperationSpecPtr vanillaOperationSpec;
    try {
        vanillaOperationSpec = ConvertTo<TVanillaOperationSpecPtr>(SpecString_);
    } catch (const std::exception& ex) {
        // We can try to archive operation with invalid spec (for example with empty task name).
        // Scheduler should not crash in this case.
        return {};
    }

    std::vector<TString> taskNames;
    taskNames.reserve(vanillaOperationSpec->Tasks.size());
    for (const auto& [taskName, taskSpec] : vanillaOperationSpec->Tasks) {
        taskNames.push_back(taskName);
    }

    return taskNames;
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
    for (auto& [_, alert] : Alerts_) {
        NConcurrency::TDelayedExecutor::CancelAndClear(alert.ResetCookie);
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

std::optional<EUnschedulableReason> TOperation::CheckUnschedulable() const
{
    if (State_ != EOperationState::Running) {
        return EUnschedulableReason::IsNotRunning;
    }

    if (Suspended_) {
        return EUnschedulableReason::Suspended;
    }

    if (Controller_->GetPendingJobCount() == 0) {
        return EUnschedulableReason::NoPendingJobs;
    }

    return std::nullopt;
}

IOperationControllerStrategyHostPtr TOperation::GetControllerStrategyHost() const
{
    return Controller_;
}

TCodicilGuard TOperation::MakeCodicilGuard() const
{
    return TCodicilGuard(CodicilData_);
}

EOperationState TOperation::GetState() const
{
    return State_;
}

void TOperation::SetStateAndEnqueueEvent(
    EOperationState state,
    const THashMap<TString, TString>& attributes)
{
    State_ = state;
    Events_.emplace_back(TOperationEvent({TInstant::Now(), state, attributes}));
    ShouldFlush_ = true;
}

void TOperation::SetSlotIndex(const TString& treeId, int value)
{
    auto result = TreeIdToSlotIndex_.emplace(treeId, value);
    if (!result.second) {
        result.first->second = value;
    }
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

bool TOperation::IsRunningInStrategy() const
{
    return RunningInStrategy_;
}

void TOperation::SetRunningInStrategy()
{
    RunningInStrategy_= true;
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
            const auto& [alertType, alert] = pair;

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

void TOperation::Cancel(const TError& error)
{
    if (CancelableContext_) {
        CancelableContext_->Cancel(error);
    }
}

void TOperation::Restart(const TError& error)
{
    Cancel(error);
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

bool TOperation::IsScheduledInSingleTree() const
{
    return IsScheduledInSingleTree_;
}

void TOperation::EraseTrees(const std::vector<TString>& treeIds)
{
    if (!treeIds.empty()) {
        ShouldFlush_ = true;
    }
    for (const auto& treeId : treeIds) {
        RuntimeParameters_->ErasedTrees.push_back(treeId);
        YT_VERIFY(RuntimeParameters_->SchedulingOptionsPerPoolTree.erase(treeId));
    }
}

////////////////////////////////////////////////////////////////////////////////

int TOperationControllerData::GetPendingJobCount() const
{
    return PendingJobCount_.load();
}

void TOperationControllerData::SetPendingJobCount(int value)
{
    PendingJobCount_.store(value);
}

NScheduler::TJobResources TOperationControllerData::GetNeededResources()
{
    auto guard = ReaderGuard(NeededResourcesLock_);
    return NeededResources_;
}

void TOperationControllerData::SetNeededResources(const NScheduler::TJobResources& value)
{
    auto guard = WriterGuard(NeededResourcesLock_);
    NeededResources_ = value;
}

TJobResourcesWithQuotaList TOperationControllerData::GetMinNeededJobResources() const
{
    auto guard = ReaderGuard(MinNeededResourcesJobLock_);
    return MinNeededJobResources_;
}

void TOperationControllerData::SetMinNeededJobResources(const TJobResourcesWithQuotaList& value)
{
    auto guard = WriterGuard(MinNeededResourcesJobLock_);
    MinNeededJobResources_ = value;
}

////////////////////////////////////////////////////////////////////////////////

TParseOperationSpecResult ParseSpec(TYsonString specString, INodePtr specTemplate, std::optional<TOperationId> operationId)
{
    VERIFY_THREAD_AFFINITY_ANY();

    IMapNodePtr specNode;
    try {
        specNode = ConvertToNode(specString)->AsMap();
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing operation spec string")
            << ex;
    }

    if (specTemplate) {
        specNode = PatchNode(specTemplate, specNode)->AsMap();
    }

    if (operationId) { // Revive case
        try {
            if (auto aclNode = specNode->FindChild("acl")) {
                ConvertTo<TSerializableAccessControlList>(aclNode);
            }
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Failed to parse operation ACL from spec, removing it (OperationId: %v)",
                *operationId);
            specNode->RemoveChild("acl");
        }
    }

    TParseOperationSpecResult result;
    try {
        result.Spec = ConvertTo<TOperationSpecBasePtr>(specNode);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing operation spec")
            << ex;
    }

    specNode->RemoveChild("secure_vault");
    result.SpecNode = specNode;
    result.SpecString = ConvertToYsonString(specNode);
    
    auto strategySpec = static_cast<TStrategyOperationSpecPtr>(result.Spec);
    for (const auto& [treeId, optionPerPoolTree] : strategySpec->SchedulingOptionsPerPoolTree) {
        result.CustomSpecPerTree.emplace(
            treeId,
            UpdateYsonSerializable(strategySpec, ConvertToNode(optionPerPoolTree)));
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler

