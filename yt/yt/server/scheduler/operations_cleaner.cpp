#include "private.h"
#include "operations_cleaner.h"
#include "operation.h"
#include "bootstrap.h"
#include "helpers.h"
#include "operation_alert_event.h"

#include <yt/yt/server/lib/scheduler/config.h>
#include <yt/yt/server/lib/scheduler/experiments.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/ytlib/controller_agent/helpers.h>

#include <yt/yt/client/api/rowset.h>
#include <yt/yt/client/api/transaction.h>
#include <yt/yt/client/api/operation_archive_schema.h>

#include <yt/yt/client/security_client/public.h>

#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/helpers.h>

#include <yt/yt/core/actions/cancelable_context.h>

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/nonblocking_batch.h>
#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/async_semaphore.h>

#include <yt/yt/core/misc/numeric_helpers.h>

#include <yt/yt/core/profiling/profiler.h>

#include <yt/yt/core/rpc/dispatcher.h>

#include <yt/yt/core/utilex/random.h>

#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/ypath_resolver.h>

namespace NYT::NScheduler {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NObjectClient;
using namespace NApi;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NProfiling;

static const NLogging::TLogger Logger("OperationsCleaner");

////////////////////////////////////////////////////////////////////////////////

struct TOrderedByIdTag
{ };

struct TOrderedByStartTimeTag
{ };

struct TOperationAliasesTag
{ };

////////////////////////////////////////////////////////////////////////////////

void TArchiveOperationRequest::InitializeFromOperation(const TOperationPtr& operation)
{
    Id = operation->GetId();
    StartTime = operation->GetStartTime();
    FinishTime = *operation->GetFinishTime();
    State = operation->GetState();
    AuthenticatedUser = operation->GetAuthenticatedUser();
    OperationType = operation->GetType();
    Spec = operation->GetSpecString();
    Result = operation->BuildResultString();
    Events = ConvertToYsonString(operation->Events());
    Alerts = operation->BuildAlertsString();
    BriefSpec = operation->BriefSpecString();
    RuntimeParameters = ConvertToYsonString(operation->GetRuntimeParameters(), EYsonFormat::Binary);
    Alias = operation->Alias();
    SlotIndexPerPoolTree = ConvertToYsonString(operation->GetSlotIndices(), EYsonFormat::Binary);
    TaskNames = ConvertToYsonString(operation->GetTaskNames(), EYsonFormat::Binary);
    ExperimentAssignments = ConvertToYsonString(operation->ExperimentAssignments(), EYsonFormat::Binary);
    ExperimentAssignmentNames = ConvertToYsonString(operation->GetExperimentAssignmentNames(), EYsonFormat::Binary);

    const auto& attributes = operation->ControllerAttributes();
    const auto& initializationAttributes = attributes.InitializeAttributes;
    if (initializationAttributes) {
        UnrecognizedSpec = initializationAttributes->UnrecognizedSpec;
        FullSpec = initializationAttributes->FullSpec;
    }
}

const std::vector<TString>& TArchiveOperationRequest::GetAttributeKeys()
{
    // Keep the stuff below synchronized with InitializeFromAttributes method.
    static const std::vector<TString> attributeKeys = {
        "key",
        "start_time",
        "finish_time",
        "state",
        "authenticated_user",
        "operation_type",
        "progress",
        "brief_progress",
        "spec",
        "brief_spec",
        "result",
        "events",
        "alerts",
        "full_spec",
        "unrecognized_spec",
        "runtime_parameters",
        "heavy_runtime_parameters",
        "alias",
        "slot_index_per_pool_tree",
        "task_names",
        "experiment_assignments",
        "controller_features",
    };

    return attributeKeys;
}

const std::vector<TString>& TArchiveOperationRequest::GetProgressAttributeKeys()
{
    static const std::vector<TString> attributeKeys = {
        "progress",
        "brief_progress",
    };

    return attributeKeys;
}

void TArchiveOperationRequest::InitializeFromAttributes(const IAttributeDictionary& attributes)
{
    Id = TOperationId::FromString(attributes.Get<TString>("key"));
    StartTime = attributes.Get<TInstant>("start_time");
    FinishTime = attributes.Get<TInstant>("finish_time");
    State = attributes.Get<EOperationState>("state");
    AuthenticatedUser = attributes.Get<TString>("authenticated_user");
    OperationType = attributes.Get<EOperationType>("operation_type");
    Progress = attributes.FindYson("progress");
    BriefProgress = attributes.FindYson("brief_progress");
    Spec = attributes.GetYson("spec");
    // In order to recover experiment assignment names, we must either
    // dig into assignment YSON representation or reconstruct assignment objects.
    // The latter seems more convenient. Also, do not forget that older operations
    // may miss assignment attribute at all.
    if (auto experimentAssignmentsYson = attributes.FindYson("experiment_assignments")) {
        ExperimentAssignments = experimentAssignmentsYson;
        auto experimentAssignments = ConvertTo<std::vector<TExperimentAssignmentPtr>>(experimentAssignmentsYson);
        std::vector<TString> experimentAssignmentNames;
        experimentAssignmentNames.reserve(experimentAssignments.size());
        for (const auto& experimentAssignment : experimentAssignments) {
            experimentAssignmentNames.emplace_back(experimentAssignment->GetName());
        }
        ExperimentAssignmentNames = ConvertToYsonString(experimentAssignmentNames, EYsonFormat::Binary);
    }

    BriefSpec = attributes.FindYson("brief_spec");
    Result = attributes.GetYson("result");
    Events = attributes.GetYson("events");
    Alerts = attributes.GetYson("alerts");
    FullSpec = attributes.FindYson("full_spec");
    UnrecognizedSpec = attributes.FindYson("unrecognized_spec");

    if (auto heavyRuntimeParameters = attributes.Find<IMapNodePtr>("heavy_runtime_parameters")) {
        auto runtimeParameters = attributes.Find<IMapNodePtr>("runtime_parameters");
        if (!runtimeParameters) {
            RuntimeParameters = ConvertToYsonString(heavyRuntimeParameters);
        } else {
            RuntimeParameters = ConvertToYsonString(PatchNode(runtimeParameters, heavyRuntimeParameters));
        }
    } else {
        RuntimeParameters = attributes.FindYson("runtime_parameters");
    }
    Alias = ConvertTo<TOperationSpecBasePtr>(Spec)->Alias;
    SlotIndexPerPoolTree = attributes.FindYson("slot_index_per_pool_tree");
    TaskNames = attributes.FindYson("task_names");
    ControllerFeatures = attributes.FindYson("controller_features");
}

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

std::vector<TString> GetPools(const IMapNodePtr& runtimeParameters)
{
    auto schedulingOptionsNode = runtimeParameters->FindChild("scheduling_options_per_pool_tree");
    if (!schedulingOptionsNode) {
        return {};
    }

    std::vector<TString> pools;
    for (const auto& [key, value] : schedulingOptionsNode->AsMap()->GetChildren()) {
        pools.push_back(value->AsMap()->GetChildOrThrow("pool")->GetValue<TString>());
    }

    return pools;
}

TString GetFilterFactors(const TArchiveOperationRequest& request)
{
    auto getOriginalPath = [] (const TString& path) -> TString {
        try {
            auto parsedPath = NYPath::TRichYPath::Parse(path);
            auto originalPath = parsedPath.Attributes().Find<TString>("original_path");
            if (originalPath) {
                return *originalPath;
            }
            return parsedPath.GetPath();
        } catch (const std::exception& ex) {
            return "";
        }
    };

    auto runtimeParametersMapNode = ConvertToNode(request.RuntimeParameters)->AsMap();
    auto specMapNode = ConvertToNode(request.Spec)->AsMap();

    std::vector<TString> parts;
    parts.push_back(ToString(request.Id));
    parts.push_back(request.AuthenticatedUser);
    parts.push_back(FormatEnum(request.State));
    parts.push_back(FormatEnum(request.OperationType));

    if (request.ExperimentAssignmentNames) {
        auto experimentAssignmentNames = ConvertTo<std::vector<TString>>(request.ExperimentAssignmentNames);
        parts.insert(parts.end(), experimentAssignmentNames.begin(), experimentAssignmentNames.end());
    }

    if (auto node = runtimeParametersMapNode->FindChild("annotations")) {
        parts.push_back(ConvertToYsonString(node, EYsonFormat::Text).ToString());
    }

    for (const auto& key : {"pool", "title"}) {
        auto node = specMapNode->FindChild(key);
        if (node && node->GetType() == ENodeType::String) {
            parts.push_back(node->GetValue<TString>());
        }
    }

    for (const auto& key : {"input_table_paths", "output_table_paths"}) {
        auto node = specMapNode->FindChild(key);
        if (node && node->GetType() == ENodeType::List) {
            auto child = node->AsList()->FindChild(0);
            if (child && child->GetType() == ENodeType::String) {
                auto path = getOriginalPath(child->GetValue<TString>());
                if (!path.empty()) {
                    parts.push_back(path);
                }
            }
        }
    }

    for (const auto& key : {"output_table_path", "table_path"}) {
        auto node = specMapNode->FindChild(key);
        if (node && node->GetType() == ENodeType::String) {
            auto path = getOriginalPath(node->AsString()->GetValue());
            if (!path.empty()) {
                parts.push_back(path);
            }
        }
    }

    if (request.RuntimeParameters) {
        auto runtimeParametersNode = ConvertToNode(request.RuntimeParameters)->AsMap();
        auto pools = GetPools(runtimeParametersNode);
        parts.insert(parts.end(), pools.begin(), pools.end());
    }

    auto result = JoinToString(parts.begin(), parts.end(), TStringBuf(" "));
    return to_lower(result);
}

bool HasFailedJobs(const TYsonString& briefProgress)
{
    YT_VERIFY(briefProgress);
    auto failedJobs = NYTree::TryGetInt64(briefProgress.AsStringBuf(), "/jobs/failed");
    return failedJobs && *failedJobs > 0;
}

// If progress has state field, we overwrite Archive with Cypress's progress only if operation is finished.
// Otherwise, let's think that information in Archive is the newest (in most cases it is true).
bool NeedProgressInRequest(const TYsonString& progress)
{
    YT_VERIFY(progress);
    auto stateString = NYTree::TryGetString(progress.AsStringBuf(), "/state");
    if (!stateString) {
        return false;
    }
    auto stateEnum = ParseEnum<NControllerAgent::EControllerState>(*stateString);
    return NControllerAgent::IsFinishedState(stateEnum);
}

TUnversionedRow BuildOrderedByIdTableRow(
    const TRowBufferPtr& rowBuffer,
    const TArchiveOperationRequest& request,
    const TOrderedByIdTableDescriptor::TIndex& index,
    int version)
{
    // All any and string values passed to MakeUnversioned* functions MUST be alive till
    // they are captured in row buffer (they are not owned by unversioned value or builder).
    auto state = FormatEnum(request.State);
    auto operationType = FormatEnum(request.OperationType);
    auto filterFactors = GetFilterFactors(request);

    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[0], index.IdHi));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[1], index.IdLo));
    builder.AddValue(MakeUnversionedStringValue(state, index.State));
    builder.AddValue(MakeUnversionedStringValue(request.AuthenticatedUser, index.AuthenticatedUser));
    builder.AddValue(MakeUnversionedStringValue(operationType, index.OperationType));
    if (request.Progress && NeedProgressInRequest(request.Progress)) {
        builder.AddValue(MakeUnversionedAnyValue(request.Progress.AsStringBuf(), index.Progress));
    }
    if (request.BriefProgress && NeedProgressInRequest(request.BriefProgress)) {
        builder.AddValue(MakeUnversionedAnyValue(request.BriefProgress.AsStringBuf(), index.BriefProgress));
    }
    builder.AddValue(MakeUnversionedAnyValue(request.Spec.AsStringBuf(), index.Spec));
    if (request.BriefSpec) {
        builder.AddValue(MakeUnversionedAnyValue(request.BriefSpec.AsStringBuf(), index.BriefSpec));
    }
    builder.AddValue(MakeUnversionedInt64Value(request.StartTime.MicroSeconds(), index.StartTime));
    builder.AddValue(MakeUnversionedInt64Value(request.FinishTime.MicroSeconds(), index.FinishTime));
    builder.AddValue(MakeUnversionedStringValue(filterFactors, index.FilterFactors));
    builder.AddValue(MakeUnversionedAnyValue(request.Result.AsStringBuf(), index.Result));
    builder.AddValue(MakeUnversionedAnyValue(request.Events.AsStringBuf(), index.Events));
    if (request.Alerts) {
        builder.AddValue(MakeUnversionedAnyValue(request.Alerts.AsStringBuf(), index.Alerts));
    }
    if (version >= 17) {
        if (request.UnrecognizedSpec) {
            builder.AddValue(MakeUnversionedAnyValue(request.UnrecognizedSpec.AsStringBuf(), index.UnrecognizedSpec));
        }
        if (request.FullSpec) {
            builder.AddValue(MakeUnversionedAnyValue(request.FullSpec.AsStringBuf(), index.FullSpec));
        }
    }

    if (version >= 22 && request.RuntimeParameters) {
        builder.AddValue(MakeUnversionedAnyValue(request.RuntimeParameters.AsStringBuf(), index.RuntimeParameters));
    }

    if (version >= 27 && request.SlotIndexPerPoolTree) {
        builder.AddValue(MakeUnversionedAnyValue(request.SlotIndexPerPoolTree.AsStringBuf(), index.SlotIndexPerPoolTree));
    }

    if (version >= 35 && request.TaskNames) {
        builder.AddValue(MakeUnversionedAnyValue(request.TaskNames.AsStringBuf(), index.TaskNames));
    }

    if (version >= 40 && request.ExperimentAssignments) {
        builder.AddValue(MakeUnversionedAnyValue(request.ExperimentAssignments.AsStringBuf(), index.ExperimentAssignments));
        builder.AddValue(MakeUnversionedAnyValue(request.ExperimentAssignmentNames.AsStringBuf(), index.ExperimentAssignmentNames));
    }

    if (version >= 42 && request.ControllerFeatures) {
        builder.AddValue(MakeUnversionedAnyValue(request.ControllerFeatures.AsStringBuf(), index.ControllerFeatures));
    }

    return rowBuffer->CaptureRow(builder.GetRow());
}

TUnversionedRow BuildOrderedByStartTimeTableRow(
    const TRowBufferPtr& rowBuffer,
    const TArchiveOperationRequest& request,
    const TOrderedByStartTimeTableDescriptor::TIndex& index,
    int version)
{
    // All any and string values passed to MakeUnversioned* functions MUST be alive till
    // they are captured in row buffer (they are not owned by unversioned value or builder).
    auto state = FormatEnum(request.State);
    auto operationType = FormatEnum(request.OperationType);
    auto filterFactors = GetFilterFactors(request);

    TYsonString pools;
    TYsonString acl;

    if (request.RuntimeParameters) {
        auto runtimeParametersNode = ConvertToNode(request.RuntimeParameters)->AsMap();
        pools = ConvertToYsonString(GetPools(runtimeParametersNode));
        if (auto aclNode = runtimeParametersNode->FindChild("acl")) {
            acl = ConvertToYsonString(aclNode);
        }
    }

    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedInt64Value(request.StartTime.MicroSeconds(), index.StartTime));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[0], index.IdHi));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[1], index.IdLo));
    builder.AddValue(MakeUnversionedStringValue(operationType, index.OperationType));
    builder.AddValue(MakeUnversionedStringValue(state, index.State));
    builder.AddValue(MakeUnversionedStringValue(request.AuthenticatedUser, index.AuthenticatedUser));
    builder.AddValue(MakeUnversionedStringValue(filterFactors, index.FilterFactors));

    if (version >= 24) {
        if (pools) {
            builder.AddValue(MakeUnversionedAnyValue(pools.AsStringBuf(), index.Pools));
        }
        if (request.BriefProgress) {
            builder.AddValue(MakeUnversionedBooleanValue(HasFailedJobs(request.BriefProgress), index.HasFailedJobs));
        }
    }

    if (version >= 30 && acl) {
        builder.AddValue(MakeUnversionedAnyValue(acl.AsStringBuf(), index.Acl));
    }

    return rowBuffer->CaptureRow(builder.GetRow());
}

TUnversionedRow BuildOperationAliasesTableRow(
    const TRowBufferPtr& rowBuffer,
    const TArchiveOperationRequest& request,
    const TOperationAliasesTableDescriptor::TIndex& index,
    int /* version */)
{
    // All any and string values passed to MakeUnversioned* functions MUST be alive till
    // they are captured in row buffer (they are not owned by unversioned value or builder).

    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedStringValue(*request.Alias, index.Alias));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[0], index.OperationIdHi));
    builder.AddValue(MakeUnversionedUint64Value(request.Id.Parts64[1], index.OperationIdLo));

    return rowBuffer->CaptureRow(builder.GetRow());
}

void DoSendOperationAlerts(
    NNative::IClientPtr client,
    std::deque<TOperationAlertEvent> eventsToSend,
    int maxAlertEventCountPerOperation)
{
    YT_LOG_DEBUG("Writing operation alert events to archive (EventCount: %v)", eventsToSend.size());

    TOrderedByIdTableDescriptor tableDescriptor;
    const auto& tableIndex = tableDescriptor.Index;
    auto columns = std::vector{tableIndex.IdHi, tableIndex.IdLo, tableIndex.AlertEvents};
    auto columnFilter = NTableClient::TColumnFilter(columns);

    THashSet<TOperationId> ids;
    for (const auto& event : eventsToSend) {
        ids.insert(*event.OperationId);
    }
    auto rowsetOrError = LookupOperationsInArchive(
        client,
        std::vector(ids.begin(), ids.end()),
        columnFilter);
    THROW_ERROR_EXCEPTION_IF_FAILED(rowsetOrError, "Failed to fetch operation alert events from archive");
    auto rowset = rowsetOrError.Value();

    auto idHiIndex = columnFilter.GetPosition(tableIndex.IdHi);
    auto idLoIndex = columnFilter.GetPosition(tableIndex.IdLo);
    auto alertEventsIndex = columnFilter.GetPosition(tableIndex.AlertEvents);

    THashMap<TOperationId, std::deque<TOperationAlertEvent>> idToAlertEvents;
    for (auto row : rowset->GetRows()) {
        if (!row) {
            continue;
        }
        auto operationId = TOperationId(
            FromUnversionedValue<ui64>(row[idHiIndex]),
            FromUnversionedValue<ui64>(row[idLoIndex]));

        auto eventsFromArchive = FromUnversionedValue<std::optional<TYsonStringBuf>>(row[alertEventsIndex]);            
        if (eventsFromArchive) {
            idToAlertEvents.emplace(
                operationId,
                ConvertTo<std::deque<TOperationAlertEvent>>(*eventsFromArchive));
        }
    }
    for (const auto& alertEvent : eventsToSend) {
        // Id can be absent in idToAlertEvents if row with such id is not created in archive yet.
        // In this case we want to create this row and initialize it with empty operation alert history.
        auto& operationAlertEvents = idToAlertEvents[*alertEvent.OperationId];
        operationAlertEvents.push_back(alertEvent);
        while (std::ssize(operationAlertEvents) > maxAlertEventCountPerOperation) {
            operationAlertEvents.pop_front();
        }
    }

    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> rows;
    rows.reserve(idToAlertEvents.size());

    for (const auto& [operationId, events] : idToAlertEvents) {
        TUnversionedRowBuilder builder;
        builder.AddValue(MakeUnversionedUint64Value(operationId.Parts64[0], tableIndex.IdHi));
        builder.AddValue(MakeUnversionedUint64Value(operationId.Parts64[1], tableIndex.IdLo));
        auto serializedEvents = ConvertToYsonString(events);
        builder.AddValue(MakeUnversionedAnyValue(serializedEvents.AsStringBuf(), tableIndex.AlertEvents));

        rows.push_back(rowBuffer->CaptureRow(builder.GetRow()));
    }

    auto transaction = WaitFor(client->StartTransaction(ETransactionType::Tablet, TTransactionStartOptions{}))
        .ValueOrThrow();
    transaction->WriteRows(
        GetOperationsArchiveOrderedByIdPath(),
        tableDescriptor.NameTable,
        MakeSharedRange(std::move(rows), std::move(rowBuffer)));

    WaitFor(transaction->Commit())
        .ThrowOnError();

    YT_LOG_DEBUG("Operation alert events written to archive (EventCount: %v)", eventsToSend.size());
}

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

class TOperationsCleaner::TImpl
    : public TRefCounted
{
public:
    DEFINE_SIGNAL(void(const std::vector<TArchiveOperationRequest>&), OperationsArchived);

    TImpl(
        TOperationsCleanerConfigPtr config,
        IOperationsCleanerHost* host,
        TBootstrap* bootstrap)
        : Config_(std::move(config))
        , Bootstrap_(bootstrap)
        , Host_(host)
        , RemoveBatcher_(New<TNonblockingBatch<TOperationId>>(
            Config_->RemoveBatchSize,
            Config_->RemoveBatchTimeout))
        , ArchiveBatcher_(New<TNonblockingBatch<TOperationId>>(
            Config_->ArchiveBatchSize,
            Config_->ArchiveBatchTimeout))
        , Client_(Bootstrap_->GetMasterClient()->GetNativeConnection()
            ->CreateNativeClient(TClientOptions::FromUser(NSecurityClient::OperationsCleanerUserName)))
    {
        Profiler.AddFuncGauge("/remove_pending", MakeStrong(this), [this] {
            return RemovePending_.load();
        });
        Profiler.AddFuncGauge("/archive_pending", MakeStrong(this), [this] {
            return ArchivePending_.load();
        });
        Profiler.AddFuncGauge("/submitted", MakeStrong(this), [this] {
            return Submitted_.load();
        });
        Profiler.AddFuncGauge("/alert_events/enqueued", MakeStrong(this), [this] {
            return EnqueuedAlertEvents_.load();
        });
    }

    void Start()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DoStart(/* fetchFinishedOperations */ false);
    }

    void Stop()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DoStop();
    }

    void UpdateConfig(const TOperationsCleanerConfigPtr& config)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        bool enable = Config_->Enable;
        bool enableOperationArchivation = Config_->EnableOperationArchivation;
        bool enableOperationAlertEventArchivation = Config_->EnableOperationAlertEventArchivation;
        Config_ = config;

        if (enable != Config_->Enable) {
            if (Config_->Enable) {
                DoStart(/* fetchFinishedOperations */ true);
            } else {
                DoStop();
            }
        }

        if (enableOperationArchivation != Config_->EnableOperationArchivation) {
            if (Config_->EnableOperationArchivation) {
                DoStartOperationArchivation();
            } else {
                DoStopOperationArchivation();
            }
        }

        if (enableOperationAlertEventArchivation != Config_->EnableOperationAlertEventArchivation) {
            if (Config_->EnableOperationAlertEventArchivation) {
                DoStartAlertEventArchivation();
            } else {
                DoStopAlertEventArchivation();
            }
        }

        CheckAndTruncateAlertEvents();
        if (OperationAlertEventSenderExecutor_) {
            OperationAlertEventSenderExecutor_->SetPeriod(Config_->OperationAlertEventSendPeriod);
        }

        ArchiveBatcher_->UpdateMaxBatchSize(Config_->ArchiveBatchSize);
        ArchiveBatcher_->UpdateBatchDuration(Config_->ArchiveBatchTimeout);

        RemoveBatcher_->UpdateMaxBatchSize(Config_->RemoveBatchSize);
        RemoveBatcher_->UpdateBatchDuration(Config_->RemoveBatchTimeout);

        YT_LOG_INFO("Operations cleaner config updated (Enable: %v, EnableOperationArchivation: %v, EnableOperationAlertEventArchivation: %v)",
            Config_->Enable,
            Config_->EnableOperationArchivation,
            Config_->EnableOperationAlertEventArchivation);
    }

    void SubmitForArchivation(TArchiveOperationRequest request)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!IsEnabled()) {
            return;
        }

        auto id = request.Id;

        // Can happen if scheduler reported operation and archiver was turned on and
        // fetched the same operation from Cypress.
        if (OperationMap_.find(id) != OperationMap_.end()) {
            return;
        }

        auto deadline = request.FinishTime + Config_->CleanDelay;

        ArchiveTimeToOperationIdMap_.emplace(deadline, id);
        YT_VERIFY(OperationMap_.emplace(id, std::move(request)).second);

        ++Submitted_;

        YT_LOG_DEBUG("Operation submitted for archivation (OperationId: %v, ArchivationStartTime: %v)",
            id,
            deadline);
    }

    void SubmitForRemoval(TRemoveOperationRequest request)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!IsEnabled()) {
            return;
        }

        EnqueueForRemoval(request.Id);
        YT_LOG_DEBUG("Operation submitted for removal (OperationId: %v)", request.Id);
    }

    void SetArchiveVersion(int version)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ArchiveVersion_ = version;
    }

    bool IsEnabled() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Enabled_;
    }

    void BuildOrchid(TFluentMap fluent) const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        fluent
            .Item("enable").Value(IsEnabled())
            .Item("enable_operation_archivation").Value(IsOperationArchivationEnabled())
            .Item("remove_pending").Value(RemovePending_.load())
            .Item("archive_pending").Value(ArchivePending_.load())
            .Item("submitted").Value(Submitted_.load());
    }

    void EnqueueOperationAlertEvent(
        TOperationId operationId,
        EOperationAlertType alertType,
        const TError& alert)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        OperationAlertEventQueue_.push_back({
            operationId,
            alertType,
            TInstant::Now(),
            alert});
        CheckAndTruncateAlertEvents();
    }

private:
    TOperationsCleanerConfigPtr Config_;
    TBootstrap* const Bootstrap_;
    IOperationsCleanerHost* const Host_;

    TPeriodicExecutorPtr AnalysisExecutor_;
    TPeriodicExecutorPtr OperationAlertEventSenderExecutor_;

    TCancelableContextPtr CancelableContext_;
    IInvokerPtr CancelableControlInvoker_;

    int ArchiveVersion_ = -1;

    bool Enabled_ = false;
    bool OperationArchivationEnabled_ = false;

    TDelayedExecutorCookie OperationArchivationStartCookie_;

    std::multimap<TInstant, TOperationId> ArchiveTimeToOperationIdMap_;
    THashMap<TOperationId, TArchiveOperationRequest> OperationMap_;

    TIntrusivePtr<TNonblockingBatch<TOperationId>> RemoveBatcher_;
    TIntrusivePtr<TNonblockingBatch<TOperationId>> ArchiveBatcher_;

    std::deque<TOperationAlertEvent> OperationAlertEventQueue_;
    TInstant LastOperationAlertEventSendTime_;

    NNative::IClientPtr Client_;

    TProfiler Profiler{"/operations_cleaner"};
    std::atomic<i64> RemovePending_{0};
    std::atomic<i64> ArchivePending_{0};
    std::atomic<i64> Submitted_{0};
    std::atomic<i64> EnqueuedAlertEvents_{0};

    TCounter ArchivedOperationCounter_ = Profiler.Counter("/archived");
    TCounter RemovedOperationCounter_ = Profiler.Counter("/removed");
    TCounter CommittedDataWeightCounter_ = Profiler.Counter("/committed_data_weight");
    TCounter ArchiveErrorCounter_ = Profiler.Counter("/archive_errors");
    TCounter RemoveOperationErrorCounter_ = Profiler.Counter("/remove_errors");
    TCounter ArchivedOperationAlertEventCounter_ = Profiler.Counter("/alert_events/archived");
    TEventTimer AnalyzeOperationsTimer_ = Profiler.Timer("/analyze_operations_time");
    TEventTimer OperationsRowsPreparationTimer_ = Profiler.Timer("/operations_rows_preparation_time");

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

private:
    IInvokerPtr GetInvoker() const
    {
        return CancelableControlInvoker_;
    }

    void ScheduleArchiveOperations()
    {
        GetInvoker()->Invoke(BIND(&TImpl::ArchiveOperations, MakeStrong(this)));
    }

    void DoStart(bool fetchFinishedOperations)
    {
        if (Config_->Enable && !Enabled_) {
            Enabled_ = true;

            YT_VERIFY(!CancelableContext_);
            CancelableContext_ = New<TCancelableContext>();
            CancelableControlInvoker_ = CancelableContext_->CreateInvoker(
                Bootstrap_->GetControlInvoker(EControlQueue::OperationsCleaner));

            AnalysisExecutor_ = New<TPeriodicExecutor>(
                CancelableControlInvoker_,
                BIND(&TImpl::OnAnalyzeOperations, MakeWeak(this)),
                Config_->AnalysisPeriod);

            AnalysisExecutor_->Start();

            GetInvoker()->Invoke(BIND(&TImpl::RemoveOperations, MakeStrong(this)));

            ScheduleArchiveOperations();
            DoStartOperationArchivation();
            DoStartAlertEventArchivation();

            // If operations cleaner was disabled during scheduler runtime and then
            // enabled then we should fetch all finished operation since scheduler did not
            // reported them.
            if (fetchFinishedOperations) {
                GetInvoker()->Invoke(BIND(&TImpl::FetchFinishedOperations, MakeStrong(this)));
            }

            YT_LOG_INFO("Operations cleaner started");
        }
    }

    void DoStartOperationArchivation()
    {
        if (Config_->Enable && Config_->EnableOperationArchivation && !OperationArchivationEnabled_) {
            OperationArchivationEnabled_ = true;
            TDelayedExecutor::CancelAndClear(OperationArchivationStartCookie_);
            Host_->SetSchedulerAlert(ESchedulerAlertType::OperationsArchivation, TError());

            YT_LOG_INFO("Operations archivation started");
        }
    }

    void DoStartAlertEventArchivation()
    {
        if (Config_->Enable && Config_->EnableOperationAlertEventArchivation && !OperationAlertEventSenderExecutor_) {
            OperationAlertEventSenderExecutor_ = New<TPeriodicExecutor>(
                CancelableControlInvoker_,
                BIND(&TImpl::SendOperationAlerts, MakeWeak(this)),
                Config_->OperationAlertEventSendPeriod);
            OperationAlertEventSenderExecutor_->Start();

            YT_LOG_INFO("Alert event archivation started");
        }
    }

    void DoStopOperationArchivation()
    {
        if (!OperationArchivationEnabled_) {
            return;
        }

        OperationArchivationEnabled_ = false;
        TDelayedExecutor::CancelAndClear(OperationArchivationStartCookie_);
        Host_->SetSchedulerAlert(ESchedulerAlertType::OperationsArchivation, TError());

        YT_LOG_INFO("Operations archivation stopped");
    }

    void DoStopAlertEventArchivation()
    {
        if (!OperationAlertEventSenderExecutor_) {
            return;
        }
        OperationAlertEventSenderExecutor_->Stop();
        OperationAlertEventSenderExecutor_.Reset();

        YT_LOG_INFO("Alert event archivation stopped");
    }

    void DoStop()
    {
        if (!Enabled_) {
            return;
        }

        Enabled_ = false;

        if (CancelableContext_) {
            CancelableContext_->Cancel(TError("Operation cleaner stopped"));
        }
        CancelableContext_.Reset();

        CancelableControlInvoker_ = nullptr;

        if (AnalysisExecutor_) {
            AnalysisExecutor_->Stop();
        }
        AnalysisExecutor_.Reset();

        TDelayedExecutor::CancelAndClear(OperationArchivationStartCookie_);

        DoStopOperationArchivation();
        DoStopAlertEventArchivation();

        ArchiveBatcher_->Drop();
        RemoveBatcher_->Drop();
        ArchiveTimeToOperationIdMap_.clear();
        OperationMap_.clear();
        ArchivePending_ = 0;
        RemovePending_ = 0;

        YT_LOG_INFO("Operations cleaner stopped");
    }

    void OnAnalyzeOperations()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        YT_LOG_INFO("Analyzing operations submitted for archivation (SubmittedOperationCount: %v)",
            ArchiveTimeToOperationIdMap_.size());

        if (ArchiveTimeToOperationIdMap_.empty()) {
            YT_LOG_INFO("No operations submitted for archivation");
            return;
        }

        auto now = TInstant::Now();

        int retainedCount = 0;
        int enqueuedForArchivationCount = 0;
        THashMap<TString, int> operationCountPerUser;

        auto canArchive = [&] (const auto& request) {
            if (retainedCount >= Config_->HardRetainedOperationCount) {
                return true;
            }

            if (now - request.FinishTime > Config_->MaxOperationAge) {
                return true;
            }

            if (!IsOperationWithUserJobs(request.OperationType) &&
                request.State == EOperationState::Completed)
            {
                return true;
            }

            if (operationCountPerUser[request.AuthenticatedUser] >= Config_->MaxOperationCountPerUser) {
                return true;
            }

            // TODO(asaitgalin): Consider only operations without stderrs?
            if (retainedCount >= Config_->SoftRetainedOperationCount &&
                request.State != EOperationState::Failed)
            {
                return true;
            }

            return false;
        };

        // Analyze operations with expired grace timeout, from newest to oldest.
        {
            TEventTimerGuard guard(AnalyzeOperationsTimer_);

            auto it = ArchiveTimeToOperationIdMap_.lower_bound(now);
            while (it != ArchiveTimeToOperationIdMap_.begin()) {
                --it;

                auto operationId = it->second;
                const auto& request = GetRequest(operationId);
                if (canArchive(request)) {
                    it = ArchiveTimeToOperationIdMap_.erase(it);
                    CleanOperation(operationId);
                    enqueuedForArchivationCount += 1;
                } else {
                    retainedCount += 1;
                    operationCountPerUser[request.AuthenticatedUser] += 1;
                }
            }
        }

        Submitted_.store(ArchiveTimeToOperationIdMap_.size());

        YT_LOG_INFO(
            "Finished analyzing operations submitted for archivation "
            "(RetainedCount: %v, EnqueuedForArchivationCount: %v)",
            retainedCount,
            enqueuedForArchivationCount);
    }

    void EnqueueForRemoval(TOperationId operationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_LOG_DEBUG("Operation enqueued for removal (OperationId: %v)", operationId);
        RemovePending_++;
        RemoveBatcher_->Enqueue(operationId);
    }

    void EnqueueForArchivation(TOperationId operationId)
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        YT_LOG_DEBUG("Operation enqueued for archivation (OperationId: %v)", operationId);
        ArchivePending_++;
        ArchiveBatcher_->Enqueue(operationId);
    }

    void CleanOperation(TOperationId operationId)
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        if (IsOperationArchivationEnabled()) {
            EnqueueForArchivation(operationId);
        } else {
            EnqueueForRemoval(operationId);
        }
    }

    void TryArchiveOperations(const std::vector<TOperationId>& operationIds)
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        int version = ArchiveVersion_;
        if (version == -1) {
            THROW_ERROR_EXCEPTION("Unknown operations archive version");
        }

        auto asyncTransaction = Client_->StartTransaction(
            ETransactionType::Tablet, TTransactionStartOptions{});
        auto transaction = WaitFor(asyncTransaction)
            .ValueOrThrow();

        YT_LOG_DEBUG(
            "Operations archivation transaction started (TransactionId: %v, OperationCount: %v)",
            transaction->GetId(),
            operationIds.size());

        i64 orderedByIdRowsDataWeight = 0;
        i64 orderedByStartTimeRowsDataWeight = 0;
        i64 operationAliasesRowsDataWeight = 0;

        THashSet<TOperationId> skippedOperationIds;

        auto isValueWeightViolated = [&] (TUnversionedRow row, TOperationId operationId, const TNameTablePtr nameTable) {
            for (auto value : row) {
                auto valueWeight = GetDataWeight(value);
                if (valueWeight > MaxStringValueLength) {
                    YT_LOG_WARNING(
                        "Operation row violates value data weight, archivation skipped"
                        "(OperationId: %v, Key: %v, Weight: %v, WeightLimit: %v)",
                        operationId,
                        nameTable->GetNameOrThrow(value.Id),
                        valueWeight,
                        MaxStringValueLength);
                    return true;
                }
            }
            return false;
        };

        {
            TEventTimerGuard guard(OperationsRowsPreparationTimer_);

            // ordered_by_id table rows
            {
                TOrderedByIdTableDescriptor desc;
                auto rowBuffer = New<TRowBuffer>(TOrderedByIdTag{});
                std::vector<TUnversionedRow> rows;
                rows.reserve(operationIds.size());

                for (auto operationId : operationIds) {
                    try {
                        const auto& request = GetRequest(operationId);
                        auto row = NDetail::BuildOrderedByIdTableRow(rowBuffer, request, desc.Index, version);

                        if (isValueWeightViolated(row, operationId, desc.NameTable)) {
                            skippedOperationIds.insert(operationId);
                            continue;
                        }

                        rows.push_back(row);
                        orderedByIdRowsDataWeight += GetDataWeight(row);
                    } catch (const std::exception& ex) {
                        THROW_ERROR_EXCEPTION("Failed to build row for operation %v", operationId)
                            << ex;
                    }
                }

                transaction->WriteRows(
                    GetOperationsArchiveOrderedByIdPath(),
                    desc.NameTable,
                    MakeSharedRange(std::move(rows), std::move(rowBuffer)));
            }

            // ordered_by_start_time rows
            {
                TOrderedByStartTimeTableDescriptor desc;
                auto rowBuffer = New<TRowBuffer>(TOrderedByStartTimeTag{});
                std::vector<TUnversionedRow> rows;
                rows.reserve(operationIds.size());

                for (auto operationId : operationIds) {
                    if (skippedOperationIds.contains(operationId)) {
                        continue;
                    }
                    try {
                        const auto& request = GetRequest(operationId);
                        auto row = NDetail::BuildOrderedByStartTimeTableRow(rowBuffer, request, desc.Index, version);
                        rows.push_back(row);
                        orderedByStartTimeRowsDataWeight += GetDataWeight(row);
                    } catch (const std::exception& ex) {
                        THROW_ERROR_EXCEPTION("Failed to build row for operation %v", operationId)
                            << ex;
                    }
                }

                transaction->WriteRows(
                    GetOperationsArchiveOrderedByStartTimePath(),
                    desc.NameTable,
                    MakeSharedRange(std::move(rows), std::move(rowBuffer)));
            }

            // operation_aliases rows
            if (ArchiveVersion_ >= 26) {
                TOperationAliasesTableDescriptor desc;
                auto rowBuffer = New<TRowBuffer>(TOperationAliasesTag{});
                std::vector<TUnversionedRow> rows;
                rows.reserve(operationIds.size());

                for (auto operationId : operationIds) {
                    if (skippedOperationIds.contains(operationId)) {
                        continue;
                    }

                    const auto& request = GetRequest(operationId);

                    if (request.Alias) {
                        auto row = NDetail::BuildOperationAliasesTableRow(rowBuffer, request, desc.Index, version);
                        rows.emplace_back(row);
                        operationAliasesRowsDataWeight += GetDataWeight(row);
                    }
                }

                transaction->WriteRows(
                    GetOperationsArchiveOperationAliasesPath(),
                    desc.NameTable,
                    MakeSharedRange(std::move(rows), std::move(rowBuffer)));
            }
        }

        i64 totalDataWeight = orderedByIdRowsDataWeight + orderedByStartTimeRowsDataWeight;

        YT_LOG_DEBUG(
            "Started committing archivation transaction (TransactionId: %v, OperationCount: %v, SkippedOperationCount: %v, "
            "OrderedByIdRowsDataWeight: %v, OrderedByStartTimeRowsDataWeight: %v, TotalDataWeight: %v)",
            transaction->GetId(),
            operationIds.size(),
            skippedOperationIds.size(),
            orderedByIdRowsDataWeight,
            orderedByStartTimeRowsDataWeight,
            totalDataWeight);

        WaitFor(transaction->Commit())
            .ThrowOnError();

        YT_LOG_DEBUG("Finished committing archivation transaction (TransactionId: %v)", transaction->GetId());

        YT_LOG_DEBUG("Operations archived (OperationIds: %v)", operationIds);

        CommittedDataWeightCounter_.Increment(totalDataWeight);
        ArchivedOperationCounter_.Increment(operationIds.size());
    }

    bool IsOperationArchivationEnabled() const
    {
        return IsEnabled() && OperationArchivationEnabled_;
    }

    void ArchiveOperations()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        auto batch = WaitFor(ArchiveBatcher_->DequeueBatch())
            .ValueOrThrow();

        if (!batch.empty()) {
            while (IsOperationArchivationEnabled()) {
                TError error;
                try {
                    TryArchiveOperations(batch);
                } catch (const std::exception& ex) {
                    int pendingCount = ArchivePending_.load();
                    error = TError("Failed to archive operations")
                        << TErrorAttribute("pending_count", pendingCount)
                        << ex;
                    YT_LOG_WARNING(error);
                    ArchiveErrorCounter_.Increment();
                }

                int pendingCount = ArchivePending_.load();
                if (pendingCount >= Config_->MinOperationCountEnqueuedForAlert) {
                    auto alertError = TError("Too many operations in archivation queue")
                        << TErrorAttribute("pending_count", pendingCount);
                    if (!error.IsOK()) {
                        alertError.MutableInnerErrors()->push_back(error);
                    }
                    Host_->SetSchedulerAlert(
                        ESchedulerAlertType::OperationsArchivation,
                        alertError);
                } else {
                    Host_->SetSchedulerAlert(
                        ESchedulerAlertType::OperationsArchivation,
                        TError());
                }

                if (error.IsOK()) {
                    break;
                }

                if (ArchivePending_ > Config_->MaxOperationCountEnqueuedForArchival) {
                    TemporarilyDisableArchivation();
                    break;
                } else {
                    auto sleepDelay = Config_->MinArchivationRetrySleepDelay +
                        RandomDuration(Config_->MaxArchivationRetrySleepDelay - Config_->MinArchivationRetrySleepDelay);
                    TDelayedExecutor::WaitForDuration(sleepDelay);
                }
            }

            ProcessCleanedOperation(batch);
            for (auto operationId : batch) {
                EnqueueForRemoval(operationId);
            }

            ArchivePending_ -= batch.size();
        }

        ScheduleArchiveOperations();
    }

    void DoRemoveOperations(std::vector<TOperationId> operationIds)
    {
        YT_LOG_DEBUG("Removing operations from Cypress (OperationCount: %v)", operationIds.size());

        std::vector<TOperationId> failedOperationIds;
        std::vector<TOperationId> removedOperationIds;
        std::vector<TOperationId> operationIdsToRemove;

        int lockedOperationCount = 0;
        int failedToRemoveOperationCount = 0;

        // Fetch lock_count attribute.
        {
            auto channel = Client_->GetMasterChannelOrThrow(
                EMasterChannelKind::Follower,
                PrimaryMasterCellTagSentinel);

            TObjectServiceProxy proxy(channel);
            auto batchReq = proxy.ExecuteBatch();

            for (auto operationId : operationIds) {
                auto req = TYPathProxy::Get(GetOperationPath(operationId) + "/@lock_count");
                batchReq->AddRequest(req, "get_lock_count");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());

            if (batchRspOrError.IsOK()) {
                const auto& batchRsp = batchRspOrError.Value();
                auto rsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_lock_count");
                YT_VERIFY(rsps.size() == operationIds.size());

                for (int index = 0; index < std::ssize(rsps); ++index) {
                    bool isLocked = false;
                    const auto rsp = rsps[index];
                    if (rsp.IsOK()) {
                        auto lockCountNode = ConvertToNode(TYsonString(rsp.Value()->value()));
                        if (lockCountNode->AsUint64()->GetValue() > 0) {
                            isLocked = true;
                        }
                    }

                    auto operationId = operationIds[index];
                    if (isLocked) {
                        failedOperationIds.push_back(operationId);
                        ++lockedOperationCount;
                    } else {
                        operationIdsToRemove.push_back(operationId);
                    }
                }
            } else {
                YT_LOG_WARNING(
                    batchRspOrError,
                    "Failed to get lock count for operations from Cypress (OperationCount: %v)",
                    operationIds.size());

                failedOperationIds = operationIds;

                failedToRemoveOperationCount = operationIds.size();
            }
        }

        // Perform actual remove.
        if (!operationIdsToRemove.empty()) {
            int subbatchSize = Config_->RemoveSubbatchSize;

            auto channel = Client_->GetMasterChannelOrThrow(
                EMasterChannelKind::Leader,
                PrimaryMasterCellTagSentinel);
            TObjectServiceProxy proxy(channel);

            std::vector<TFuture<TObjectServiceProxy::TRspExecuteBatchPtr>> responseFutures;

            int subbatchCount = DivCeil(static_cast<int>(operationIdsToRemove.size()), subbatchSize);

            std::vector<int> subbatchSizes;
            for (int subbatchIndex = 0; subbatchIndex < subbatchCount; ++subbatchIndex) {
                auto batchReq = proxy.ExecuteBatch();

                int startIndex = subbatchIndex * subbatchSize;
                int endIndex = std::min(static_cast<int>(operationIdsToRemove.size()), startIndex + subbatchSize);
                for (int index = startIndex; index < endIndex; ++index) {
                    auto req = TYPathProxy::Remove(GetOperationPath(operationIdsToRemove[index]));
                    req->set_recursive(true);
                    batchReq->AddRequest(req, "remove_operation");
                }

                responseFutures.push_back(batchReq->Invoke());
            }

            auto responseResultsOrError = WaitFor(AllSet(responseFutures));
            YT_VERIFY(responseResultsOrError.IsOK());
            const auto& responseResults = responseResultsOrError.Value();

            for (int subbatchIndex = 0; subbatchIndex < subbatchCount; ++subbatchIndex) {
                int startIndex = subbatchIndex * subbatchSize;
                int endIndex = std::min(static_cast<int>(operationIdsToRemove.size()), startIndex + subbatchSize);

                const auto& batchRspOrError = responseResults[subbatchIndex];
                if (batchRspOrError.IsOK()) {
                    const auto& batchRsp = batchRspOrError.Value();
                    auto rsps = batchRsp->GetResponses<TYPathProxy::TRspRemove>("remove_operation");
                    YT_VERIFY(std::ssize(rsps) == endIndex - startIndex);

                    for (int index = startIndex; index < endIndex; ++index) {
                        auto operationId = operationIdsToRemove[index];

                        auto rsp = rsps[index - startIndex];
                        if (rsp.IsOK()) {
                            removedOperationIds.push_back(operationId);
                        } else {
                            YT_LOG_DEBUG(
                                rsp,
                                "Failed to remove finished operation from Cypress (OperationId: %v)",
                                operationId);

                            failedOperationIds.push_back(operationId);

                            ++failedToRemoveOperationCount;
                        }
                    }
                } else {
                    YT_LOG_WARNING(
                        batchRspOrError,
                        "Failed to remove finished operations from Cypress (OperationCount: %v)",
                        endIndex - startIndex);

                    for (int index = startIndex; index < endIndex; ++index) {
                        failedOperationIds.push_back(operationIdsToRemove[index]);
                        ++failedToRemoveOperationCount;
                    }
                }
            }
        }

        YT_VERIFY(operationIds.size() == failedOperationIds.size() + removedOperationIds.size());
        int removedCount = removedOperationIds.size();

        RemovedOperationCounter_.Increment(removedOperationIds.size());
        RemoveOperationErrorCounter_.Increment(failedOperationIds.size());

        ProcessCleanedOperation(removedOperationIds);

        for (auto operationId : failedOperationIds) {
            RemoveBatcher_->Enqueue(operationId);
        }

        RemovePending_ -= removedCount;
        YT_LOG_DEBUG(
            "Successfully removed operations from Cypress (Count: %v, LockedCount: %v, FailedToRemoveCount: %)",
            removedCount,
            lockedOperationCount,
            failedToRemoveOperationCount);
    }

    void RemoveOperations()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        auto batch = WaitFor(RemoveBatcher_->DequeueBatch())
            .ValueOrThrow();

        if (!batch.empty()) {
            DoRemoveOperations(std::move(batch));
        }

        auto callback = BIND(&TImpl::RemoveOperations, MakeStrong(this))
            .Via(GetInvoker());

        TDelayedExecutor::Submit(callback, RandomDuration(Config_->MaxRemovalSleepDelay));
    }

    void TemporarilyDisableArchivation()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        DoStopOperationArchivation();

        auto enableCallback = BIND(&TImpl::DoStartOperationArchivation, MakeStrong(this))
            .Via(GetInvoker());

        OperationArchivationStartCookie_ = TDelayedExecutor::Submit(
            enableCallback,
            Config_->ArchivationEnableDelay);

        auto enableTime = TInstant::Now() + Config_->ArchivationEnableDelay;

        Host_->SetSchedulerAlert(
            ESchedulerAlertType::OperationsArchivation,
            TError("Max enqueued operations limit reached; archivation is temporarily disabled")
            << TErrorAttribute("enable_time", enableTime));

        YT_LOG_INFO("Archivation is temporarily disabled (EnableTime: %v)", enableTime);
    }

    void FetchFinishedOperations()
    {
        try {
            DoFetchFinishedOperations();
        } catch (const std::exception& ex) {
            // NOTE(asaitgalin): Maybe disconnect? What can we do here?
            YT_LOG_WARNING(ex, "Failed to fetch finished operations from Cypress");
        }
    }

    void FetchBriefProgressFromArchive(std::vector<TArchiveOperationRequest>& requests)
    {
        const TOrderedByIdTableDescriptor descriptor;
        std::vector<TOperationId> ids;
        ids.reserve(requests.size());
        for (const auto& req : requests) {
            ids.push_back(req.Id);
        }
        auto filter = TColumnFilter({descriptor.Index.BriefProgress});
        auto briefProgressIndex = filter.GetPosition(descriptor.Index.BriefProgress);
        auto timeout = Config_->FinishedOperationsArchiveLookupTimeout;
        auto rowsetOrError = LookupOperationsInArchive(Client_, ids, filter, timeout);
        if (!rowsetOrError.IsOK()) {
            YT_LOG_WARNING("Failed to fetch operation brief progress from archive (Error: %v)",
                rowsetOrError);
            return;
        }
        auto rows = rowsetOrError.Value()->GetRows();
        YT_VERIFY(rows.size() == requests.size());
        for (int i = 0; i < static_cast<int>(requests.size()); ++i) {
            if (!requests[i].BriefProgress && rows[i] && rows[i][briefProgressIndex].Type != EValueType::Null) {
                auto value = rows[i][briefProgressIndex];
                requests[i].BriefProgress = TYsonString(TString(value.Data.String, value.Length));
            }
        }
    }

    void DoFetchFinishedOperations()
    {
        YT_LOG_INFO("Fetching all finished operations from Cypress");

        auto createBatchRequest = BIND([this] {
            auto channel = Client_->GetMasterChannelOrThrow(
                EMasterChannelKind::Follower, PrimaryMasterCellTagSentinel);

            TObjectServiceProxy proxy(channel);
            return proxy.ExecuteBatch();
        });

        auto listOperationsResult = ListOperations(createBatchRequest);

        // Remove some operations.
        for (const auto& operation : listOperationsResult.OperationsToRemove) {
            SubmitForRemoval({operation});
        }

        auto operations = FetchOperationsFromCypressForCleaner(
            listOperationsResult.OperationsToArchive,
            createBatchRequest,
            Config_->ParseOperationAttributesBatchSize,
            Host_->GetBackgroundInvoker());

        // Controller agent reports brief_progress only to archive,
        // but it is necessary to fill ordered_by_start_time table,
        // so we request it here.
        FetchBriefProgressFromArchive(operations);

        // NB: needed for us to store the latest operation for each alias in operation_aliases archive table.
        std::sort(operations.begin(), operations.end(), [] (const auto& lhs, const auto& rhs) {
            return lhs.FinishTime < rhs.FinishTime;
        });

        for (auto& operation : operations) {
            SubmitForArchivation(std::move(operation));
        }

        YT_LOG_INFO("Fetched and processed all finished operations");
    }

    const TArchiveOperationRequest& GetRequest(TOperationId operationId) const
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());

        return GetOrCrash(OperationMap_, operationId);
    }

    void ProcessCleanedOperation(const std::vector<TOperationId>& cleanedOperationIds)
    {
        std::vector<TArchiveOperationRequest> archivedOperationRequests;
        archivedOperationRequests.reserve(cleanedOperationIds.size());
        for (const auto& operationId : cleanedOperationIds) {
            auto it = OperationMap_.find(operationId);
            if (it != OperationMap_.end()) {
                archivedOperationRequests.emplace_back(std::move(it->second));
                OperationMap_.erase(it);
            }
        }

        OperationsArchived_.Fire(archivedOperationRequests);
    }

    void SendOperationAlerts()
    {
        VERIFY_INVOKER_AFFINITY(GetInvoker());
        
        if (ArchiveVersion_ < 43 || OperationAlertEventQueue_.empty()) {
            Host_->SetSchedulerAlert(ESchedulerAlertType::OperationAlertArchivation, TError());
            return;
        }

        std::deque<TOperationAlertEvent> eventsToSend;
        eventsToSend.swap(OperationAlertEventQueue_);
        try {
            WaitFor(BIND([
                    client = Client_,
                    eventsToSend,
                    maxAlertEventCountPerOperation = Config_->MaxAlertEventCountPerOperation] () mutable {
                    NDetail::DoSendOperationAlerts(std::move(client), std::move(eventsToSend), maxAlertEventCountPerOperation);
                })
                .AsyncVia(Host_->GetBackgroundInvoker())
                .Run())
                .ThrowOnError();
            LastOperationAlertEventSendTime_ = TInstant::Now();
            Host_->SetSchedulerAlert(ESchedulerAlertType::OperationAlertArchivation, TError());
            ArchivedOperationAlertEventCounter_.Increment(eventsToSend.size());
        } catch (const std::exception& ex) {
            auto error = TError("Failed to write operation alert events to archive")
                << ex;
            YT_LOG_WARNING(error);
            if (TInstant::Now() - LastOperationAlertEventSendTime_ > Config_->OperationAlertSenderAlertThreshold) {
                Host_->SetSchedulerAlert(ESchedulerAlertType::OperationAlertArchivation, error);
            }

            while (!eventsToSend.empty() && std::ssize(OperationAlertEventQueue_) < Config_->MaxEnqueuedOperationAlertEventCount) {
                OperationAlertEventQueue_.emplace_front(std::move(eventsToSend.back()));
                eventsToSend.pop_back();
            }
        }
        EnqueuedAlertEvents_.store(std::ssize(OperationAlertEventQueue_));
    }

    void CheckAndTruncateAlertEvents()
    {
        while (std::ssize(OperationAlertEventQueue_) > Config_->MaxEnqueuedOperationAlertEventCount) {
            OperationAlertEventQueue_.pop_front();
        }
        EnqueuedAlertEvents_.store(std::ssize(OperationAlertEventQueue_));
    }
};

////////////////////////////////////////////////////////////////////////////////

TOperationsCleaner::TOperationsCleaner(
    TOperationsCleanerConfigPtr config,
    IOperationsCleanerHost* host,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(std::move(config), host, bootstrap))
{ }

TOperationsCleaner::~TOperationsCleaner()
{ }

void TOperationsCleaner::Start()
{
    Impl_->Start();
}

void TOperationsCleaner::Stop()
{
    Impl_->Stop();
}

void TOperationsCleaner::SubmitForArchivation(TArchiveOperationRequest request)
{
    Impl_->SubmitForArchivation(std::move(request));
}

void TOperationsCleaner::SubmitForRemoval(TRemoveOperationRequest request)
{
    Impl_->SubmitForRemoval(std::move(request));
}

void TOperationsCleaner::UpdateConfig(const TOperationsCleanerConfigPtr& config)
{
    Impl_->UpdateConfig(config);
}

void TOperationsCleaner::SetArchiveVersion(int version)
{
    Impl_->SetArchiveVersion(version);
}

bool TOperationsCleaner::IsEnabled() const
{
    return Impl_->IsEnabled();
}

void TOperationsCleaner::BuildOrchid(TFluentMap fluent) const
{
    Impl_->BuildOrchid(fluent);
}

void TOperationsCleaner::EnqueueOperationAlertEvent(
    TOperationId operationId,
    EOperationAlertType alertType,
    const TError& alert)
{
    Impl_->EnqueueOperationAlertEvent(operationId, alertType, alert);
}

DELEGATE_SIGNAL(TOperationsCleaner, void(const std::vector<TArchiveOperationRequest>& requests), OperationsArchived, *Impl_);

////////////////////////////////////////////////////////////////////////////////

struct TOperationDataToParse final
{
    TYsonString AttrbutesYson;
    TOperationId OperationId;
};

std::vector<TArchiveOperationRequest> FetchOperationsFromCypressForCleaner(
    const std::vector<TOperationId>& operationIds,
    TCallback<TObjectServiceProxy::TReqExecuteBatchPtr()> createBatchRequest,
    const int parseOperationAttributesBatchSize,
    const IInvokerPtr& invoker)
{
    using NYT::ToProto;

    YT_LOG_INFO("Fetching operations attributes for cleaner (OperationCount: %v)", operationIds.size());

    std::vector<TArchiveOperationRequest> result;

    auto batchReq = createBatchRequest();

    for (auto operationId : operationIds) {
        auto req = TYPathProxy::Get(GetOperationPath(operationId) + "/@");
        ToProto(req->mutable_attributes()->mutable_keys(), TArchiveOperationRequest::GetAttributeKeys());
        batchReq->AddRequest(req, "get_op_attributes");
    }

    auto rspOrError = WaitFor(batchReq->Invoke());
    auto error = GetCumulativeError(rspOrError);
    THROW_ERROR_EXCEPTION_IF_FAILED(error, "Error requesting operations attributes for archivation");

    auto rsps = rspOrError.Value()->GetResponses<TYPathProxy::TRspGet>("get_op_attributes");
    YT_VERIFY(operationIds.size() == rsps.size());

    {
        const auto processBatch = BIND([parseOperationAttributesBatchSize] (
            const std::vector<TOperationDataToParse>& operationDataToParseBatch)
        {
            std::vector<TArchiveOperationRequest> result;
            result.reserve(parseOperationAttributesBatchSize);

            for (const auto& operationDataToParse : operationDataToParseBatch) {
                IAttributeDictionaryPtr attributes;
                TOperationId operationId;
                try {
                    attributes = ConvertToAttributes(operationDataToParse.AttrbutesYson);
                    operationId = TOperationId::FromString(attributes->Get<TString>("key"));
                    YT_VERIFY(operationId == operationDataToParse.OperationId);
                } catch (const std::exception& ex) {
                    THROW_ERROR_EXCEPTION("Error parsing operation attributes")
                        << TErrorAttribute("operation_id", operationDataToParse.OperationId)
                        << ex;
                }

                try {
                    TArchiveOperationRequest req;
                    req.InitializeFromAttributes(*attributes);
                    result.push_back(std::move(req));
                } catch (const std::exception& ex) {
                    THROW_ERROR_EXCEPTION("Error initializing operation archivation request")
                        << TErrorAttribute("operation_id", operationId)
                        << TErrorAttribute("attributes", ConvertToYsonString(*attributes, EYsonFormat::Text))
                        << ex;
                }
            }

            return result;
        });

        const int operationCount{static_cast<int>(operationIds.size())};
        std::vector<TFuture<std::vector<TArchiveOperationRequest>>> futures;
        futures.reserve(RoundUp(operationCount, parseOperationAttributesBatchSize));

        for (int startIndex = 0; startIndex < operationCount; startIndex += parseOperationAttributesBatchSize) {
            std::vector<TOperationDataToParse> operationDataToParseBatch;
            operationDataToParseBatch.reserve(parseOperationAttributesBatchSize);

            for (int index = startIndex; index < std::min(operationCount, startIndex + parseOperationAttributesBatchSize); ++index) {
                operationDataToParseBatch.push_back({TYsonString(rsps[index].Value()->value()), operationIds[index]});
            }

            futures.push_back(processBatch
                .AsyncVia(invoker)
                .Run(std::move(operationDataToParseBatch))
            );
        }

        YT_LOG_INFO("Operations attributes for cleaner fetch started");
        auto operationRequestsArray = WaitFor(AllSucceeded(futures)).ValueOrThrow();

        result.reserve(operationCount);
        for (auto& operationRequests : operationRequestsArray) {
            for (auto& operationRequest : operationRequests) {
                result.push_back(std::move(operationRequest));
            }
        }
    }

    YT_LOG_INFO("Operations attributes for cleaner fetched");

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
