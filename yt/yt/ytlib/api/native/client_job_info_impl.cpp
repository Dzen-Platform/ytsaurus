#include "client_impl.h"

#include "connection.h"
#include "private.h"

#include <yt/yt/client/api/file_reader.h>
#include <yt/yt/client/api/operation_archive_schema.h>
#include <yt/yt/client/api/rowset.h>

#include <yt/yt/client/job_tracker_client/helpers.h>

#include <yt/yt/client/query_client/query_builder.h>

#include <yt/yt/client/table_client/helpers.h>
#include <yt/yt/client/table_client/name_table.h>

#include <yt/yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_options.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/data_slice_descriptor.h>
#include <yt/yt/ytlib/chunk_client/data_source.h>
#include <yt/yt/ytlib/chunk_client/helpers.h>
#include <yt/yt/ytlib/chunk_client/job_spec_extensions.h>

#include <yt/yt/ytlib/controller_agent/helpers.h>

#include <yt/yt/ytlib/job_proxy/job_spec_helper.h>
#include <yt/yt/ytlib/job_proxy/helpers.h>
#include <yt/yt/ytlib/job_proxy/user_job_read_controller.h>

#include <yt/yt/ytlib/job_prober_client/job_shell_descriptor_cache.h>

#include <yt/yt/ytlib/node_tracker_client/channel.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/core/compression/codec.h>

#include <yt/yt/core/concurrency/async_stream.h>
#include <yt/yt/core/concurrency/async_stream_pipe.h>
#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/throughput_throttler.h>

#include <yt/yt/core/ytree/ypath_resolver.h>

#include <util/string/join.h>

namespace NYT::NApi::NNative {

using namespace NConcurrency;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;
using namespace NControllerAgent;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NFileClient;
using namespace NRpc;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NSecurityClient;
using namespace NQueryClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NScheduler;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;

using NChunkClient::TChunkReaderStatistics;
using NChunkClient::TLegacyReadLimit;
using NChunkClient::TLegacyReadRange;
using NChunkClient::TDataSliceDescriptor;
using NNodeTrackerClient::TNodeDescriptor;

////////////////////////////////////////////////////////////////////////////////

static const THashSet<TString> DefaultListJobsAttributes = {
    "job_id",
    "type",
    "state",
    "start_time",
    "finish_time",
    "address",
    "has_spec",
    "progress",
    "stderr_size",
    "fail_context_size",
    "error",
    "brief_statistics",
    "job_competition_id",
    "has_competitors",
    "task_name",
    "pool",
    "pool_tree",
    "monitoring_descriptor",
};

static const auto DefaultGetJobAttributes = [] {
    auto attributes = DefaultListJobsAttributes;
    attributes.insert("operation_id");
    attributes.insert("statistics");
    attributes.insert("events");
    attributes.insert("exec_attributes");
    return attributes;
}();

static const auto SupportedJobAttributes = DefaultGetJobAttributes;

////////////////////////////////////////////////////////////////////////////////

class TJobInputReader
    : public NConcurrency::IAsyncZeroCopyInputStream
{
public:
    TJobInputReader(NJobProxy::IUserJobReadControllerPtr userJobReadController, IInvokerPtr invoker)
        : Invoker_(std::move(invoker))
        , UserJobReadController_(std::move(userJobReadController))
        , AsyncStreamPipe_(New<TAsyncStreamPipe>())
    { }

    ~TJobInputReader()
    {
        if (TransferResultFuture_) {
            TransferResultFuture_.Cancel(TError("Reader destroyed"));
        }
    }

    void Open()
    {
        auto transferClosure = UserJobReadController_->PrepareJobInputTransfer(AsyncStreamPipe_);
        TransferResultFuture_ = BIND(transferClosure)
            .AsyncVia(Invoker_)
            .Run();

        TransferResultFuture_.Subscribe(BIND([pipe = AsyncStreamPipe_] (const TError& error) {
            if (!error.IsOK()) {
                pipe->Abort(TError("Failed to get job input") << error);
            }
        }));
    }

    virtual TFuture<TSharedRef> Read() override
    {
        return AsyncStreamPipe_->Read();
    }

private:
    const IInvokerPtr Invoker_;
    const NJobProxy::IUserJobReadControllerPtr UserJobReadController_;
    const NConcurrency::TAsyncStreamPipePtr AsyncStreamPipe_;

    TFuture<void> TransferResultFuture_;
};

DECLARE_REFCOUNTED_CLASS(TJobInputReader)
DEFINE_REFCOUNTED_TYPE(TJobInputReader)

////////////////////////////////////////////////////////////////////////////////

static TUnversionedOwningRow CreateJobKey(TJobId jobId, const TNameTablePtr& nameTable)
{
    TOwningRowBuilder keyBuilder(2);

    keyBuilder.AddValue(MakeUnversionedUint64Value(jobId.Parts64[0], nameTable->GetIdOrRegisterName("job_id_hi")));
    keyBuilder.AddValue(MakeUnversionedUint64Value(jobId.Parts64[1], nameTable->GetIdOrRegisterName("job_id_lo")));

    return keyBuilder.FinishRow();
}

static TYPath GetControllerAgentOrchidRunningJobsPath(TStringBuf controllerAgentAddress, TOperationId operationId)
{
    return GetControllerAgentOrchidOperationPath(controllerAgentAddress, operationId) + "/running_jobs";
}

static TYPath GetControllerAgentOrchidRetainedFinishedJobsPath(TStringBuf controllerAgentAddress, TOperationId operationId)
{
    return GetControllerAgentOrchidOperationPath(controllerAgentAddress, operationId) + "/retained_finished_jobs";
}

////////////////////////////////////////////////////////////////////////////////

static void ValidateJobSpecVersion(
    TJobId jobId,
    const NYT::NJobTrackerClient::NProto::TJobSpec& jobSpec)
{
    if (!jobSpec.has_version() || jobSpec.version() != GetJobSpecVersion()) {
        THROW_ERROR_EXCEPTION("Job spec found in operation archive is of unsupported version")
            << TErrorAttribute("job_id", jobId)
            << TErrorAttribute("found_version", jobSpec.version())
            << TErrorAttribute("supported_version", GetJobSpecVersion());
    }
}

static bool IsNoSuchJobOrOperationError(const TError& error)
{
    return
        error.FindMatching(NScheduler::EErrorCode::NoSuchJob) ||
        error.FindMatching(NScheduler::EErrorCode::NoSuchOperation);
}

// Get job node descriptor from scheduler and check that user has |requiredPermissions|
// for accessing the corresponding operation.
TErrorOr<TNodeDescriptor> TClient::TryGetJobNodeDescriptor(
    TJobId jobId,
    EPermissionSet requiredPermissions)
{
    TJobProberServiceProxy proxy(GetSchedulerChannel());
    auto req = proxy.GetJobNode();
    req->SetUser(Options_.GetAuthenticatedUser());
    ToProto(req->mutable_job_id(), jobId);
    req->set_required_permissions(static_cast<ui32>(requiredPermissions));

    auto rspOrError = WaitFor(req->Invoke());
    if (rspOrError.IsOK()) {
        TNodeDescriptor nodeDescriptor;
        FromProto(&nodeDescriptor, rspOrError.Value()->node_descriptor());
        return nodeDescriptor;
    } else {
        return static_cast<TError>(rspOrError);
    }
}

TErrorOr<IChannelPtr> TClient::TryCreateChannelToJobNode(
    TOperationId operationId,
    TJobId jobId,
    EPermissionSet requiredPermissions)
{
    auto jobNodeDescriptorOrError = TryGetJobNodeDescriptor(jobId, requiredPermissions);
    if (jobNodeDescriptorOrError.IsOK()) {
        return ChannelFactory_->CreateChannel(jobNodeDescriptorOrError.ValueOrThrow());
    }

    YT_LOG_DEBUG(
        jobNodeDescriptorOrError,
        "Failed to get job node descriptor from scheduler (OperationId: %v, JobId: %v)",
        operationId,
        jobId);

    if (!IsNoSuchJobOrOperationError(jobNodeDescriptorOrError)) {
        THROW_ERROR_EXCEPTION("Failed to get job node descriptor from scheduler")
            << jobNodeDescriptorOrError;
    }

    try {
        ValidateOperationAccess(operationId, jobId, requiredPermissions);

        TGetJobOptions options;
        options.Attributes = {TString("address")};
        // TODO(ignat): support structured return value in GetJob.
        auto jobYsonString = WaitFor(GetJob(operationId, jobId, options))
            .ValueOrThrow();
        auto address = ConvertToNode(jobYsonString)->AsMap()->GetChildOrThrow("address")->GetValue<TString>();
        return ChannelFactory_->CreateChannel(address);
    } catch (const TErrorException& ex) {
        YT_LOG_DEBUG(ex, "Failed to create node channel to job using address from archive (OperationId: %v, JobId: %v)",
            operationId,
            jobId);
        return ex.Error();
    }
}

TErrorOr<NJobTrackerClient::NProto::TJobSpec> TClient::TryFetchJobSpecFromJobNode(
    TJobId jobId,
    NRpc::IChannelPtr nodeChannel)
{
    NJobProberClient::TJobProberServiceProxy jobProberServiceProxy(std::move(nodeChannel));
    jobProberServiceProxy.SetDefaultTimeout(Connection_->GetConfig()->JobProberRpcTimeout);

    auto req = jobProberServiceProxy.GetSpec();
    ToProto(req->mutable_job_id(), jobId);

    auto rspOrError = WaitFor(req->Invoke());
    if (!rspOrError.IsOK()) {
        return TError("Failed to get job spec from job node")
            << std::move(rspOrError)
            << TErrorAttribute("job_id", jobId);
    }

    const auto& rsp = rspOrError.Value();
    const auto& spec = rsp->spec();
    ValidateJobSpecVersion(jobId, spec);
    return spec;
}

TErrorOr<NJobTrackerClient::NProto::TJobSpec> TClient::TryFetchJobSpecFromJobNode(
    TJobId jobId,
    EPermissionSet requiredPermissions)
{
    if (auto operationId = TryGetOperationId(jobId)) {
        auto nodeChannelOrError = TryCreateChannelToJobNode(operationId, jobId, requiredPermissions);
        if (nodeChannelOrError.IsOK()) {
            return TryFetchJobSpecFromJobNode(jobId, nodeChannelOrError.ValueOrThrow());
        }
        YT_LOG_DEBUG(
            nodeChannelOrError,
            "Failed to create channel to job node using archive info (OperationId: %v, JobId: %v)",
            operationId,
            jobId);
    }
    auto jobNodeDescriptorOrError = TryGetJobNodeDescriptor(jobId, requiredPermissions);
    if (!jobNodeDescriptorOrError.IsOK()) {
        return TError(std::move(jobNodeDescriptorOrError));
    }
    const auto& nodeDescriptor = jobNodeDescriptorOrError.Value();
    auto nodeChannel = ChannelFactory_->CreateChannel(nodeDescriptor);
    return TryFetchJobSpecFromJobNode(jobId, nodeChannel);
}

NJobTrackerClient::NProto::TJobSpec TClient::FetchJobSpecFromArchive(TJobId jobId)
{
    auto nameTable = New<TNameTable>();

    TLookupRowsOptions lookupOptions;
    lookupOptions.ColumnFilter = NTableClient::TColumnFilter({nameTable->RegisterName("spec")});
    lookupOptions.KeepMissingRows = true;

    auto owningKey = CreateJobKey(jobId, nameTable);

    std::vector<TUnversionedRow> keys;
    keys.push_back(owningKey);

    auto lookupResult = WaitFor(LookupRows(
        GetOperationsArchiveJobSpecsPath(),
        nameTable,
        MakeSharedRange(keys, owningKey),
        lookupOptions));

    if (!lookupResult.IsOK()) {
        THROW_ERROR_EXCEPTION(lookupResult)
            .Wrap("Lookup job spec in operation archive failed")
            << TErrorAttribute("job_id", jobId);
    }

    auto rows = lookupResult.Value()->GetRows();
    YT_VERIFY(!rows.Empty());

    if (!rows[0]) {
        THROW_ERROR_EXCEPTION("Missing job spec in job archive table")
            << TErrorAttribute("job_id", jobId);
    }

    auto value = rows[0][0];

    if (value.Type != EValueType::String) {
        THROW_ERROR_EXCEPTION("Found job spec has unexpected value type")
            << TErrorAttribute("job_id", jobId)
            << TErrorAttribute("value_type", value.Type);
    }

    NJobTrackerClient::NProto::TJobSpec jobSpec;
    bool ok = jobSpec.ParseFromArray(value.Data.String, value.Length);
    if (!ok) {
        THROW_ERROR_EXCEPTION("Cannot parse job spec")
            << TErrorAttribute("job_id", jobId);
    }

    ValidateJobSpecVersion(jobId, jobSpec);

    return jobSpec;
}

TOperationId TClient::TryGetOperationId(
    TJobId jobId)
{
    TOperationIdTableDescriptor table;

    auto owningKey = CreateJobKey(jobId, table.NameTable);
    std::vector<TUnversionedRow> keys = {owningKey};

    TLookupRowsOptions lookupOptions;
    lookupOptions.KeepMissingRows = true;

    auto rowsetOrError = WaitFor(LookupRows(
        GetOperationsArchiveOperationIdsPath(),
        table.NameTable,
        MakeSharedRange(std::move(keys), std::move(owningKey)),
        lookupOptions));

    if (!rowsetOrError.IsOK()) {
        if (rowsetOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
            return {};
        }
        rowsetOrError.ThrowOnError();
    }

    auto rowset = rowsetOrError.ValueOrThrow();
    auto rows = rowset->GetRows();
    YT_VERIFY(!rows.Empty());
    if (!rows[0]) {
        return {};
    }

    auto row = rows[0];
    auto operationIdHiIndex = rowset->GetSchema().GetColumnIndexOrThrow("operation_id_hi");
    auto operationIdLoIndex = rowset->GetSchema().GetColumnIndexOrThrow("operation_id_lo");
    auto operationIdHi = row[operationIdHiIndex];
    auto operationIdLo = row[operationIdLoIndex];
    YT_VERIFY(operationIdHi.Type == EValueType::Uint64);
    YT_VERIFY(operationIdLo.Type == EValueType::Uint64);
    return TOperationId(FromUnversionedValue<ui64>(operationIdHi), FromUnversionedValue<ui64>(operationIdLo));
}

void TClient::ValidateOperationAccess(
    TOperationId operationId,
    TJobId jobId,
    EPermissionSet permissions)
{
    TGetOperationOptions getOperationOptions;
    getOperationOptions.Attributes = {TString("runtime_parameters")};
    auto operationOrError = WaitFor(GetOperation(operationId, getOperationOptions));

    TSerializableAccessControlList acl;
    if (operationOrError.IsOK()) {
        auto operation = std::move(operationOrError).Value();
        auto aclYson = TryGetAny(operation.RuntimeParameters.AsStringBuf(), "/acl");
        if (aclYson) {
            acl = ConvertTo<TSerializableAccessControlList>(TYsonStringBuf(*aclYson));
        } else {
            // We check against an empty ACL to allow only "superusers" and "root" access.
            YT_LOG_WARNING(
                "Failed to get ACL from operation attributes; "
                "validating against empty ACL (OperationId: %v, JobId: %v)",
                operationId,
                jobId);
        }
    } else {
        // We check against an empty ACL to allow only "superusers" and "root" access.
        YT_LOG_WARNING(
            operationOrError,
            "Failed to get operation to validate access; "
            "validating against empty ACL (OperationId: %v, JobId: %v)",
            operationId,
            jobId);
    }

    NScheduler::ValidateOperationAccess(
        /* user */ std::nullopt,
        operationId,
        jobId,
        permissions,
        acl,
        this,
        Logger);
}

void TClient::ValidateOperationAccess(
    TJobId jobId,
    const NJobTrackerClient::NProto::TJobSpec& jobSpec,
    EPermissionSet permissions)
{
    const auto extensionId = NScheduler::NProto::TSchedulerJobSpecExt::scheduler_job_spec_ext;
    TSerializableAccessControlList acl;
    if (jobSpec.HasExtension(extensionId) && jobSpec.GetExtension(extensionId).has_acl()) {
        TYsonString aclYson(jobSpec.GetExtension(extensionId).acl());
        acl = ConvertTo<TSerializableAccessControlList>(aclYson);
    } else {
        // We check against an empty ACL to allow only "superusers" and "root" access.
        YT_LOG_WARNING(
            "Job spec has no sheduler_job_spec_ext or the extension has no ACL; "
            "validating against empty ACL (JobId: %v)",
            jobId);
    }

    NScheduler::ValidateOperationAccess(
        /* user */ std::nullopt,
        TOperationId(),
        jobId,
        permissions,
        acl,
        this,
        Logger);
}

NJobTrackerClient::NProto::TJobSpec TClient::FetchJobSpec(
    NScheduler::TJobId jobId,
    NYTree::EPermissionSet requiredPermissions)
{
    auto jobSpecFromProxyOrError = TryFetchJobSpecFromJobNode(jobId, requiredPermissions);
    if (!jobSpecFromProxyOrError.IsOK() && !IsNoSuchJobOrOperationError(jobSpecFromProxyOrError)) {
        THROW_ERROR jobSpecFromProxyOrError;
    }

    if (jobSpecFromProxyOrError.IsOK()) {
        return std::move(jobSpecFromProxyOrError).Value();
    }

    YT_LOG_DEBUG(jobSpecFromProxyOrError, "Failed to fetch job spec from job node (JobId: %v)",
        jobId);

    auto jobSpec = FetchJobSpecFromArchive(jobId);

    auto operationId = TryGetOperationId(jobId);
    if (operationId) {
        ValidateOperationAccess(operationId, jobId, requiredPermissions);
    } else {
        ValidateOperationAccess(jobId, jobSpec, requiredPermissions);
    }


    return jobSpec;
}

////////////////////////////////////////////////////////////////////////////////

void TClient::DoDumpJobContext(
    TJobId jobId,
    const TYPath& path,
    const TDumpJobContextOptions& /*options*/)
{
    auto req = JobProberProxy_->DumpInputContext();
    ToProto(req->mutable_job_id(), jobId);
    ToProto(req->mutable_path(), path);

    WaitFor(req->Invoke())
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

IAsyncZeroCopyInputStreamPtr TClient::DoGetJobInput(
    TJobId jobId,
    const TGetJobInputOptions& /*options*/)
{
    auto jobSpec = FetchJobSpec(jobId, EPermissionSet(EPermission::Read));

    auto* schedulerJobSpecExt = jobSpec.MutableExtension(NScheduler::NProto::TSchedulerJobSpecExt::scheduler_job_spec_ext);

    auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
    auto locateChunks = BIND([=] {
        std::vector<TChunkSpec*> chunkSpecList;
        for (auto& tableSpec : *schedulerJobSpecExt->mutable_input_table_specs()) {
            for (auto& chunkSpec : *tableSpec.mutable_chunk_specs()) {
                chunkSpecList.push_back(&chunkSpec);
            }
        }

        for (auto& tableSpec : *schedulerJobSpecExt->mutable_foreign_input_table_specs()) {
            for (auto& chunkSpec : *tableSpec.mutable_chunk_specs()) {
                chunkSpecList.push_back(&chunkSpec);
            }
        }

        LocateChunks(
            MakeStrong(this),
            New<TMultiChunkReaderConfig>()->MaxChunksPerLocateRequest,
            chunkSpecList,
            nodeDirectory,
            Logger);
        nodeDirectory->DumpTo(schedulerJobSpecExt->mutable_input_node_directory());
    });

    auto locateChunksResult = WaitFor(locateChunks
        .AsyncVia(GetConnection()->GetInvoker())
        .Run());

    if (!locateChunksResult.IsOK()) {
        THROW_ERROR_EXCEPTION("Failed to locate chunks used in job input")
            << TErrorAttribute("job_id", jobId);
    }

    auto jobSpecHelper = NJobProxy::CreateJobSpecHelper(jobSpec);

    auto userJobReadController = CreateUserJobReadController(
        jobSpecHelper,
        MakeStrong(this),
        GetConnection()->GetInvoker(),
        TNodeDescriptor(),
        /*onNetworkRelease*/ BIND([] { }),
        /*udfDirectory*/ {},
        /*chunkReadOptions*/ {},
        GetNullBlockCache(),
        /*chunkMetaCache*/ nullptr,
        /*trafficMeter*/ nullptr,
        /*bandwidthThrottler*/ NConcurrency::GetUnlimitedThrottler(),
        /*rpsThrottler*/ NConcurrency::GetUnlimitedThrottler());

    auto jobInputReader = New<TJobInputReader>(std::move(userJobReadController), GetConnection()->GetInvoker());
    jobInputReader->Open();
    return jobInputReader;
}

////////////////////////////////////////////////////////////////////////////////

TYsonString TClient::DoGetJobInputPaths(
    TJobId jobId,
    const TGetJobInputPathsOptions& /*options*/)
{
    auto jobSpec = FetchJobSpec(jobId, EPermissionSet(EPermissionSet::Read));

    auto schedulerJobSpecExt = jobSpec.GetExtension(NScheduler::NProto::TSchedulerJobSpecExt::scheduler_job_spec_ext);

    auto optionalDataSourceDirectoryExt = FindProtoExtension<TDataSourceDirectoryExt>(schedulerJobSpecExt.extensions());
    if (!optionalDataSourceDirectoryExt) {
        THROW_ERROR_EXCEPTION("Cannot build job input paths; job is either too old or has intermediate input")
            << TErrorAttribute("job_id", jobId);
    }

    const auto& dataSourceDirectoryExt = *optionalDataSourceDirectoryExt;
    auto dataSourceDirectory = FromProto<TDataSourceDirectoryPtr>(dataSourceDirectoryExt);

    for (const auto& dataSource : dataSourceDirectory->DataSources()) {
        if (!dataSource.GetPath()) {
            THROW_ERROR_EXCEPTION("Cannot build job input paths; job has intermediate input")
               << TErrorAttribute("job_id", jobId);
        }
    }

    std::vector<std::vector<TDataSliceDescriptor>> slicesByTable(dataSourceDirectory->DataSources().size());
    for (const auto& inputSpec : schedulerJobSpecExt.input_table_specs()) {
        auto dataSliceDescriptors = NJobProxy::UnpackDataSliceDescriptors(inputSpec);
        for (const auto& slice : dataSliceDescriptors) {
            slicesByTable[slice.GetDataSourceIndex()].push_back(slice);
        }
    }

    for (const auto& inputSpec : schedulerJobSpecExt.foreign_input_table_specs()) {
        auto dataSliceDescriptors = NJobProxy::UnpackDataSliceDescriptors(inputSpec);
        for (const auto& slice : dataSliceDescriptors) {
            slicesByTable[slice.GetDataSourceIndex()].push_back(slice);
        }
    }

    auto compareAbsoluteReadLimits = [] (const TLegacyReadLimit& lhs, const TLegacyReadLimit& rhs) -> bool {
        YT_VERIFY(lhs.HasRowIndex() == rhs.HasRowIndex());

        if (lhs.HasRowIndex() && lhs.GetRowIndex() != rhs.GetRowIndex()) {
            return lhs.GetRowIndex() < rhs.GetRowIndex();
        }

        if (lhs.HasLegacyKey() && rhs.HasLegacyKey()) {
            return lhs.GetLegacyKey() < rhs.GetLegacyKey();
        } else if (lhs.HasLegacyKey()) {
            // rhs is less
            return false;
        } else if (rhs.HasLegacyKey()) {
            // lhs is less
            return true;
        } else {
            // These read limits are effectively equal.
            return false;
        }
    };

    auto canMergeSlices = [] (const TDataSliceDescriptor& lhs, const TDataSliceDescriptor& rhs, bool versioned) {
        if (lhs.GetRangeIndex() != rhs.GetRangeIndex()) {
            return false;
        }

        auto lhsUpperLimit = GetAbsoluteUpperReadLimit(lhs, versioned);
        auto rhsLowerLimit = GetAbsoluteLowerReadLimit(rhs, versioned);

        YT_VERIFY(lhsUpperLimit.HasRowIndex() == rhsLowerLimit.HasRowIndex());
        if (lhsUpperLimit.HasRowIndex() && lhsUpperLimit.GetRowIndex() < rhsLowerLimit.GetRowIndex()) {
            return false;
        }

        if (lhsUpperLimit.HasLegacyKey() != rhsLowerLimit.HasLegacyKey()) {
            return false;
        }

        if (lhsUpperLimit.HasLegacyKey() && lhsUpperLimit.GetLegacyKey() < rhsLowerLimit.GetLegacyKey()) {
            return false;
        }

        return true;
    };

    std::vector<std::vector<std::pair<TDataSliceDescriptor, TDataSliceDescriptor>>> rangesByTable(dataSourceDirectory->DataSources().size());
    for (int tableIndex = 0; tableIndex < std::ssize(dataSourceDirectory->DataSources()); ++tableIndex) {
        bool versioned = dataSourceDirectory->DataSources()[tableIndex].GetType() == EDataSourceType::VersionedTable;
        auto& tableSlices = slicesByTable[tableIndex];
        std::sort(
            tableSlices.begin(),
            tableSlices.end(),
            [&] (const TDataSliceDescriptor& lhs, const TDataSliceDescriptor& rhs) {
                if (lhs.GetRangeIndex() != rhs.GetRangeIndex()) {
                    return lhs.GetRangeIndex() < rhs.GetRangeIndex();
                }

                auto lhsLowerLimit = GetAbsoluteLowerReadLimit(lhs, versioned);
                auto rhsLowerLimit = GetAbsoluteLowerReadLimit(rhs, versioned);

                return compareAbsoluteReadLimits(lhsLowerLimit, rhsLowerLimit);
            });

        int firstSlice = 0;
        while (firstSlice < static_cast<int>(tableSlices.size())) {
            int lastSlice = firstSlice + 1;
            while (lastSlice < static_cast<int>(tableSlices.size())) {
                if (!canMergeSlices(tableSlices[lastSlice - 1], tableSlices[lastSlice], versioned)) {
                    break;
                }
                ++lastSlice;
            }
            rangesByTable[tableIndex].emplace_back(
                tableSlices[firstSlice],
                tableSlices[lastSlice - 1]);

            firstSlice = lastSlice;
        }
    }

    auto buildSliceLimit = [](const TLegacyReadLimit& limit, TFluentAny fluent) {
        fluent.BeginMap()
              .DoIf(limit.HasRowIndex(), [&] (TFluentMap fluent) {
                  fluent
                      .Item("row_index").Value(limit.GetRowIndex());
              })
              .DoIf(limit.HasLegacyKey(), [&] (TFluentMap fluent) {
                  fluent
                      .Item("key").Value(limit.GetLegacyKey());
              })
              .EndMap();
    };

    return BuildYsonStringFluently(EYsonFormat::Pretty)
        .DoListFor(rangesByTable, [&] (TFluentList fluent, const std::vector<std::pair<TDataSliceDescriptor, TDataSliceDescriptor>>& tableRanges) {
            fluent
                .DoIf(!tableRanges.empty(), [&] (TFluentList fluent) {
                    int dataSourceIndex = tableRanges[0].first.GetDataSourceIndex();
                    const auto& dataSource =  dataSourceDirectory->DataSources()[dataSourceIndex];
                    bool versioned = dataSource.GetType() == EDataSourceType::VersionedTable;
                    fluent
                        .Item()
                            .BeginAttributes()
                        .DoIf(dataSource.GetForeign(), [&] (TFluentMap fluent) {
                            fluent
                                .Item("foreign").Value(true);
                        })
                        .Item("ranges")
                        .DoListFor(tableRanges, [&] (TFluentList fluent, const std::pair<TDataSliceDescriptor, TDataSliceDescriptor>& range) {
                            fluent
                                .Item()
                                .BeginMap()
                                .Item("lower_limit").Do(BIND(
                                    buildSliceLimit,
                                    GetAbsoluteLowerReadLimit(range.first, versioned)))
                                .Item("upper_limit").Do(BIND(
                                    buildSliceLimit,
                                    GetAbsoluteUpperReadLimit(range.second, versioned)))
                                .EndMap();
                        })
                        .EndAttributes()
                        .Value(dataSource.GetPath());
                });
        });
}

////////////////////////////////////////////////////////////////////////////////

TYsonString TClient::DoGetJobSpec(
    TJobId jobId,
    const TGetJobSpecOptions& options)
{
    auto jobSpec = FetchJobSpec(jobId, EPermissionSet(EPermissionSet::Read));
    auto* schedulerJobSpecExt = jobSpec.MutableExtension(NScheduler::NProto::TSchedulerJobSpecExt::scheduler_job_spec_ext);

    if (options.OmitNodeDirectory) {
        schedulerJobSpecExt->clear_input_node_directory();
    }

    if (options.OmitInputTableSpecs) {
        schedulerJobSpecExt->clear_input_table_specs();
        schedulerJobSpecExt->clear_foreign_input_table_specs();
    }

    if (options.OmitOutputTableSpecs) {
        schedulerJobSpecExt->clear_output_table_specs();
    }

    TString jobSpecYsonBytes;
    TStringOutput jobSpecYsonBytesOutput(jobSpecYsonBytes);
    TYsonWriter jobSpecYsonWriter(&jobSpecYsonBytesOutput);

    TProtobufParserOptions parserOptions{
        .SkipUnknownFields = true,
    };
    WriteProtobufMessage(&jobSpecYsonWriter, jobSpec, parserOptions);

    auto jobSpecNode = ConvertToNode(TYsonString(jobSpecYsonBytes));

    return ConvertToYsonString(jobSpecNode);
}

////////////////////////////////////////////////////////////////////////////////

template <typename TFun>
auto RetryJobIsNotRunning(TOperationId operationId, TJobId jobId, TFun invokeRequest, NLogging::TLogger Logger)
{
    constexpr int RetryCount = 10;
    constexpr TDuration RetryBackoff = TDuration::MilliSeconds(100);

    auto needRetry = [] (auto rspOrError) {
        auto jobIsNotRunning = rspOrError.FindMatching(NJobProberClient::EErrorCode::JobIsNotRunning);
        if (!jobIsNotRunning) {
            return false;
        }
        auto jobState = jobIsNotRunning->Attributes().template Find<EJobState>("job_state");
        return jobState && *jobState == EJobState::Running;
    };

    auto rspOrError = invokeRequest();
    for (int retry = 0; needRetry(rspOrError) && retry < RetryCount; ++retry) {
        YT_LOG_DEBUG("Job state is \"running\" but job phase is not, retrying "
            "(OperationId: %v, JobId: %v, Retry: %d, RetryCount: %d, RetryBackoff: %v, Error: %v)",
            operationId,
            jobId,
            retry,
            RetryCount,
            RetryBackoff,
            rspOrError);
        TDelayedExecutor::WaitForDuration(RetryBackoff);
        rspOrError = invokeRequest();
    }
    return rspOrError;
}

TSharedRef TClient::DoGetJobStderrFromNode(
    TOperationId operationId,
    TJobId jobId)
{
    auto nodeChannelOrError = TryCreateChannelToJobNode(operationId, jobId, EPermissionSet(EPermission::Read));
    if (!nodeChannelOrError.IsOK()) {
        return TSharedRef();
    }
    auto nodeChannel = std::move(nodeChannelOrError).Value();

    NJobProberClient::TJobProberServiceProxy jobProberServiceProxy(std::move(nodeChannel));
    jobProberServiceProxy.SetDefaultTimeout(Connection_->GetConfig()->JobProberRpcTimeout);

    auto rspOrError = RetryJobIsNotRunning(
        operationId,
        jobId,
        [&] {
            auto req = jobProberServiceProxy.GetStderr();
            req->SetMultiplexingBand(EMultiplexingBand::Heavy);
            ToProto(req->mutable_job_id(), jobId);
            return WaitFor(req->Invoke());
        },
        Logger);

    if (!rspOrError.IsOK()) {
        if (IsNoSuchJobOrOperationError(rspOrError) ||
            rspOrError.FindMatching(NJobProberClient::EErrorCode::JobIsNotRunning))
        {
            return TSharedRef();
        }
        THROW_ERROR_EXCEPTION("Failed to get job stderr from job proxy")
            << TErrorAttribute("operation_id", operationId)
            << TErrorAttribute("job_id", jobId)
            << std::move(rspOrError);
    }
    auto rsp = rspOrError.Value();
    return TSharedRef::FromString(rsp->stderr_data());
}

TSharedRef TClient::DoGetJobStderrFromCypress(
    TOperationId operationId,
    TJobId jobId)
{
    auto createFileReader = [&] (const NYPath::TYPath& path) {
        return WaitFor(static_cast<IClientBase*>(this)->CreateFileReader(path));
    };

    try {
        auto fileReader = createFileReader(NScheduler::GetStderrPath(operationId, jobId))
            .ValueOrThrow();

        std::vector<TSharedRef> blocks;
        while (true) {
            auto block = WaitFor(fileReader->Read())
                .ValueOrThrow();

            if (!block) {
                break;
            }

            blocks.push_back(std::move(block));
        }

        i64 size = GetByteSize(blocks);
        YT_VERIFY(size);
        auto stderrFile = TSharedMutableRef::Allocate(size);
        auto memoryOutput = TMemoryOutput(stderrFile.Begin(), size);

        for (const auto& block : blocks) {
            memoryOutput.Write(block.Begin(), block.Size());
        }

        return stderrFile;
    } catch (const TErrorException& exception) {
        auto matchedError = exception.Error().FindMatching(NYTree::EErrorCode::ResolveError);

        if (!matchedError) {
            THROW_ERROR_EXCEPTION("Failed to get job stderr from Cypress")
                << TErrorAttribute("operation_id", operationId)
                << TErrorAttribute("job_id", jobId)
                << exception.Error();
        }
    }

    return TSharedRef();
}

TSharedRef TClient::DoGetJobStderrFromArchive(
    TOperationId operationId,
    TJobId jobId)
{
    ValidateOperationAccess(operationId, jobId, EPermissionSet(EPermission::Read));

    try {
        TJobStderrTableDescriptor tableDescriptor;

        auto rowBuffer = New<TRowBuffer>();

        std::vector<TUnversionedRow> keys;
        auto key = rowBuffer->AllocateUnversioned(4);
        key[0] = MakeUnversionedUint64Value(operationId.Parts64[0], tableDescriptor.Index.OperationIdHi);
        key[1] = MakeUnversionedUint64Value(operationId.Parts64[1], tableDescriptor.Index.OperationIdLo);
        key[2] = MakeUnversionedUint64Value(jobId.Parts64[0], tableDescriptor.Index.JobIdHi);
        key[3] = MakeUnversionedUint64Value(jobId.Parts64[1], tableDescriptor.Index.JobIdLo);
        keys.push_back(key);

        TLookupRowsOptions lookupOptions;
        lookupOptions.ColumnFilter = NTableClient::TColumnFilter({tableDescriptor.Index.Stderr});
        lookupOptions.KeepMissingRows = true;

        auto rowset = WaitFor(LookupRows(
            GetOperationsArchiveJobStderrsPath(),
            tableDescriptor.NameTable,
            MakeSharedRange(keys, rowBuffer),
            lookupOptions))
            .ValueOrThrow();

        auto rows = rowset->GetRows();
        YT_VERIFY(!rows.Empty());

        if (rows[0]) {
            auto value = rows[0][0];

            YT_VERIFY(value.Type == EValueType::String);
            return TSharedRef::MakeCopy<char>(TRef(value.Data.String, value.Length));
        }
    } catch (const TErrorException& exception) {
        auto matchedError = exception.Error().FindMatching(NYTree::EErrorCode::ResolveError);

        if (!matchedError) {
            THROW_ERROR_EXCEPTION("Failed to get job stderr from archive")
                << TErrorAttribute("operation_id", operationId)
                << TErrorAttribute("job_id", jobId)
                << exception.Error();
        }
    }

    return TSharedRef();
}

TSharedRef TClient::DoGetJobStderr(
    const TOperationIdOrAlias& operationIdOrAlias,
    TJobId jobId,
    const TGetJobStderrOptions& options)
{
    auto timeout = options.Timeout.value_or(Connection_->GetConfig()->DefaultGetOperationTimeout);
    auto deadline = timeout.ToDeadLine();

    TOperationId operationId;
    Visit(operationIdOrAlias.Payload,
        [&] (const TOperationId& id) {
            operationId = id;
        },
        [&] (const TString& alias) {
            operationId = ResolveOperationAlias(alias, options, deadline);
        });

    auto stderrRef = DoGetJobStderrFromNode(operationId, jobId);
    if (stderrRef) {
        return stderrRef;
    }

    stderrRef = DoGetJobStderrFromCypress(operationId, jobId);
    if (stderrRef) {
        return stderrRef;
    }

    stderrRef = DoGetJobStderrFromArchive(operationId, jobId);
    if (stderrRef) {
        return stderrRef;
    }

    THROW_ERROR_EXCEPTION(NScheduler::EErrorCode::NoSuchJob, "Job stderr is not found")
        << TErrorAttribute("operation_id", operationId)
        << TErrorAttribute("job_id", jobId);
}

////////////////////////////////////////////////////////////////////////////////

TSharedRef TClient::DoGetJobFailContextFromArchive(
    TOperationId operationId,
    TJobId jobId)
{
    ValidateOperationAccess(operationId, jobId, EPermissionSet(EPermission::Read));

    try {
        TJobFailContextTableDescriptor tableDescriptor;

        auto rowBuffer = New<TRowBuffer>();

        std::vector<TUnversionedRow> keys;
        auto key = rowBuffer->AllocateUnversioned(4);
        key[0] = MakeUnversionedUint64Value(operationId.Parts64[0], tableDescriptor.Index.OperationIdHi);
        key[1] = MakeUnversionedUint64Value(operationId.Parts64[1], tableDescriptor.Index.OperationIdLo);
        key[2] = MakeUnversionedUint64Value(jobId.Parts64[0], tableDescriptor.Index.JobIdHi);
        key[3] = MakeUnversionedUint64Value(jobId.Parts64[1], tableDescriptor.Index.JobIdLo);
        keys.push_back(key);

        TLookupRowsOptions lookupOptions;
        lookupOptions.ColumnFilter = NTableClient::TColumnFilter({tableDescriptor.Index.FailContext});
        lookupOptions.KeepMissingRows = true;

        auto rowset = WaitFor(LookupRows(
            GetOperationsArchiveJobFailContextsPath(),
            tableDescriptor.NameTable,
            MakeSharedRange(keys, rowBuffer),
            lookupOptions))
            .ValueOrThrow();

        auto rows = rowset->GetRows();
        YT_VERIFY(!rows.Empty());

        if (rows[0]) {
            auto value = rows[0][0];

            YT_VERIFY(value.Type == EValueType::String);
            return TSharedRef::MakeCopy<char>(TRef(value.Data.String, value.Length));
        }
    } catch (const TErrorException& exception) {
        auto matchedError = exception.Error().FindMatching(NYTree::EErrorCode::ResolveError);

        if (!matchedError) {
            THROW_ERROR_EXCEPTION("Failed to get job fail_context from archive")
                << TErrorAttribute("operation_id", operationId)
                << TErrorAttribute("job_id", jobId)
                << exception.Error();
        }
    }

    return TSharedRef();
}

TSharedRef TClient::DoGetJobFailContextFromCypress(
    TOperationId operationId,
    TJobId jobId)
{
    auto createFileReader = [&] (const NYPath::TYPath& path) {
        return WaitFor(static_cast<IClientBase*>(this)->CreateFileReader(path));
    };

    try {
        auto fileReader = createFileReader(NScheduler::GetFailContextPath(operationId, jobId))
            .ValueOrThrow();

        std::vector<TSharedRef> blocks;
        while (true) {
            auto block = WaitFor(fileReader->Read())
                .ValueOrThrow();

            if (!block) {
                break;
            }

            blocks.push_back(std::move(block));
        }

        i64 size = GetByteSize(blocks);
        YT_VERIFY(size);
        auto failContextFile = TSharedMutableRef::Allocate(size);
        auto memoryOutput = TMemoryOutput(failContextFile.Begin(), size);

        for (const auto& block : blocks) {
            memoryOutput.Write(block.Begin(), block.Size());
        }

        return failContextFile;
    } catch (const TErrorException& exception) {
        auto matchedError = exception.Error().FindMatching(NYTree::EErrorCode::ResolveError);

        if (!matchedError) {
            THROW_ERROR_EXCEPTION("Failed to get job fail context from Cypress")
                << TErrorAttribute("operation_id", operationId)
                << TErrorAttribute("job_id", jobId)
                << exception.Error();
        }
    }

    return TSharedRef();
}

TSharedRef TClient::DoGetJobFailContext(
    const TOperationIdOrAlias& operationIdOrAlias,
    TJobId jobId,
    const TGetJobFailContextOptions& options)
{
    auto timeout = options.Timeout.value_or(Connection_->GetConfig()->DefaultGetOperationTimeout);
    auto deadline = timeout.ToDeadLine();

    TOperationId operationId;
    Visit(operationIdOrAlias.Payload,
        [&] (const TOperationId& id) {
            operationId = id;
        },
        [&] (const TString& alias) {
            operationId = ResolveOperationAlias(alias, options, deadline);
        });

    if (auto failContextRef = DoGetJobFailContextFromCypress(operationId, jobId)) {
        return failContextRef;
    }
    if (auto failContextRef = DoGetJobFailContextFromArchive(operationId, jobId)) {
        return failContextRef;
    }
    THROW_ERROR_EXCEPTION(
        NScheduler::EErrorCode::NoSuchJob,
        "Job fail context is not found")
        << TErrorAttribute("operation_id", operationId)
        << TErrorAttribute("job_id", jobId);
}

////////////////////////////////////////////////////////////////////////////////

static void ValidateNonNull(
    const TUnversionedValue& value,
    TStringBuf name,
    TOperationId operationId,
    TJobId jobId = {})
{
    if (Y_UNLIKELY(value.Type == EValueType::Null)) {
        auto error = TError("Unexpected null value in column %Qv in job archive", name)
            << TErrorAttribute("operation_id", operationId);
        if (jobId) {
            error = error << TErrorAttribute("job_id", jobId);
        }
        THROW_ERROR error;
    }
}

static TQueryBuilder GetListJobsQueryBuilder(
    TOperationId operationId,
    const TListJobsOptions& options)
{
    NQueryClient::TQueryBuilder builder;
    builder.SetSource(GetOperationsArchiveJobsPath());

    builder.AddWhereConjunct(Format(
        "(operation_id_hi, operation_id_lo) = (%vu, %vu)",
        operationId.Parts64[0],
        operationId.Parts64[1]));

    builder.AddWhereConjunct(Format(
        R""(job_state IN ("aborted", "failed", "completed", "lost") )""
        "OR (NOT is_null(update_time) AND update_time >= %v)",
        (TInstant::Now() - options.RunningJobsLookbehindPeriod).MicroSeconds()));

    if (options.Address) {
        builder.AddWhereConjunct(Format("address = %Qv", *options.Address));
    }

    return builder;
}

// Get statistics for jobs.
TFuture<TListJobsStatistics> TClient::ListJobsStatisticsFromArchiveAsync(
    TOperationId operationId,
    TInstant deadline,
    const TListJobsOptions& options)
{
    auto builder = GetListJobsQueryBuilder(operationId, options);

    auto jobTypeIndex = builder.AddSelectExpression("type", "job_type");
    auto jobStateIndex = builder.AddSelectExpression("if(is_null(state), transient_state, state)", "job_state");
    auto countIndex = builder.AddSelectExpression("sum(1)", "count");

    builder.AddGroupByExpression("job_type");
    builder.AddGroupByExpression("job_state");

    TSelectRowsOptions selectRowsOptions;
    selectRowsOptions.Timestamp = AsyncLastCommittedTimestamp;
    selectRowsOptions.Timeout = deadline - Now();
    selectRowsOptions.InputRowLimit = std::numeric_limits<i64>::max();
    selectRowsOptions.MemoryLimitPerNode = 100_MB;

    return SelectRows(builder.Build(), selectRowsOptions).Apply(BIND([=] (const TSelectRowsResult& result) {
        TListJobsStatistics statistics;
        for (auto row : result.Rowset->GetRows()) {
            ValidateNonNull(row[jobTypeIndex], "type", operationId);
            auto jobType = ParseEnum<EJobType>(FromUnversionedValue<TStringBuf>(row[jobTypeIndex]));
            ValidateNonNull(row[jobStateIndex], "state", operationId);
            auto jobState = ParseEnum<EJobState>(FromUnversionedValue<TStringBuf>(row[jobStateIndex]));
            auto count = FromUnversionedValue<i64>(row[countIndex]);

            statistics.TypeCounts[jobType] += count;
            if (options.Type && *options.Type != jobType) {
                continue;
            }

            statistics.StateCounts[jobState] += count;
            if (options.State && *options.State != jobState) {
                continue;
            }
        }
        return statistics;
    }));
}

static std::vector<TJob> ParseJobsFromArchiveResponse(
    TOperationId operationId,
    const IUnversionedRowsetPtr& rowset,
    bool needFullStatistics)
{
    const auto& schema = rowset->GetSchema();

    auto findColumnIndex = [&] (auto ...names) -> std::optional<int> {
        for (auto name : {names...,}) {
            auto column = schema.FindColumn(name);
            if (column) {
                return schema.GetColumnIndex(*column);
            }
        }
        return {};
    };

    auto jobIdHiIndex = findColumnIndex("job_id_hi");
    auto jobIdLoIndex = findColumnIndex("job_id_lo");
    auto operationIdHiIndex = findColumnIndex("operation_id_hi");
    auto typeIndex = findColumnIndex("job_type", "type");
    auto stateIndex = findColumnIndex("job_state", "transient_state");
    auto startTimeIndex = findColumnIndex("start_time");
    auto finishTimeIndex = findColumnIndex("finish_time");
    auto addressIndex = findColumnIndex("address");
    auto errorIndex = findColumnIndex("error");
    auto statisticsIndex = findColumnIndex("statistics");
    auto eventsIndex = findColumnIndex("events");
    auto briefStatisticsIndex = findColumnIndex("brief_statistics");
    auto statisticsLz4Index = findColumnIndex("statistics_lz4");
    auto stderrSizeIndex = findColumnIndex("stderr_size");
    auto hasSpecIndex = findColumnIndex("has_spec");
    auto failContextSizeIndex = findColumnIndex("fail_context_size");
    auto jobCompetitionIdIndex = findColumnIndex("job_competition_id");
    auto hasCompetitorsIndex = findColumnIndex("has_competitors");
    auto execAttributesIndex = findColumnIndex("exec_attributes");
    auto taskNameIndex = findColumnIndex("task_name");
    auto coreInfosIndex = findColumnIndex("core_infos");
    auto poolTreeIndex = findColumnIndex("pool_tree");
    auto monitoringDescriptorIndex = findColumnIndex("monitoring_descriptor");

    std::vector<TJob> jobs;
    auto rows = rowset->GetRows();
    jobs.reserve(rows.Size());
    for (auto row : rows) {
        auto& job = jobs.emplace_back();

        if (jobIdHiIndex) {
            YT_VERIFY(jobIdLoIndex);
            ValidateNonNull(row[*jobIdHiIndex], "job_id_hi", operationId);
            ValidateNonNull(row[*jobIdLoIndex], "job_id_lo", operationId);
            job.Id = TJobId(FromUnversionedValue<ui64>(row[*jobIdHiIndex]), FromUnversionedValue<ui64>(row[*jobIdLoIndex]));
        }

        if (operationIdHiIndex) {
            job.OperationId = operationId;
        }

        if (typeIndex) {
            ValidateNonNull(row[*typeIndex], "type", operationId, job.Id);
            job.Type = ParseEnum<EJobType>(FromUnversionedValue<TStringBuf>(row[*typeIndex]));
        }

        if (stateIndex) {
            ValidateNonNull(row[*stateIndex], "state", operationId, job.Id);
            job.ArchiveState = ParseEnum<EJobState>(FromUnversionedValue<TStringBuf>(row[*stateIndex]));
        }

        if (startTimeIndex) {
            if (row[*startTimeIndex].Type != EValueType::Null) {
                job.StartTime = TInstant::MicroSeconds(FromUnversionedValue<i64>(row[*startTimeIndex]));
            } else {
                // This field previously was non-optional.
                job.StartTime.emplace();
            }
        }

        if (finishTimeIndex && row[*finishTimeIndex].Type != EValueType::Null) {
            job.FinishTime = TInstant::MicroSeconds(FromUnversionedValue<i64>(row[*finishTimeIndex]));
        }

        if (addressIndex) {
            if (row[*addressIndex].Type != EValueType::Null) {
                job.Address = FromUnversionedValue<TString>(row[*addressIndex]);
            } else {
                // This field previously was non-optional.
                job.Address.emplace();
            }
        }

        if (stderrSizeIndex && row[*stderrSizeIndex].Type != EValueType::Null) {
            job.StderrSize = FromUnversionedValue<ui64>(row[*stderrSizeIndex]);
        }

        if (failContextSizeIndex && row[*failContextSizeIndex].Type != EValueType::Null) {
            job.FailContextSize = FromUnversionedValue<ui64>(row[*failContextSizeIndex]);
        }

        if (jobCompetitionIdIndex && row[*jobCompetitionIdIndex].Type != EValueType::Null) {
            job.JobCompetitionId = FromUnversionedValue<TGuid>(row[*jobCompetitionIdIndex]);
        }

        if (hasCompetitorsIndex) {
            if (row[*hasCompetitorsIndex].Type != EValueType::Null) {
                job.HasCompetitors = FromUnversionedValue<bool>(row[*hasCompetitorsIndex]);
            } else {
                job.HasCompetitors = false;
            }
        }

        if (hasSpecIndex) {
            if (row[*hasSpecIndex].Type != EValueType::Null) {
                job.HasSpec = FromUnversionedValue<bool>(row[*hasSpecIndex]);
            } else {
                // This field previously was non-optional.
                job.HasSpec = false;
            }
        }

        if (errorIndex && row[*errorIndex].Type != EValueType::Null) {
            job.Error = FromUnversionedValue<TYsonString>(row[*errorIndex]);
        }

        if (coreInfosIndex && row[*coreInfosIndex].Type != EValueType::Null) {
            job.CoreInfos = FromUnversionedValue<TYsonString>(row[*coreInfosIndex]);
        }

        if (briefStatisticsIndex && row[*briefStatisticsIndex].Type != EValueType::Null) {
            job.BriefStatistics = FromUnversionedValue<TYsonString>(row[*briefStatisticsIndex]);
        }

        if ((needFullStatistics || !job.BriefStatistics) &&
            statisticsIndex && row[*statisticsIndex].Type != EValueType::Null)
        {
            auto statisticsYson = FromUnversionedValue<TYsonStringBuf>(row[*statisticsIndex]);
            if (needFullStatistics) {
                job.Statistics = TYsonString(statisticsYson);
            }
            auto statistics = ConvertToNode(statisticsYson);
            job.BriefStatistics = BuildBriefStatistics(statistics);
        }

        if ((needFullStatistics || !job.BriefStatistics) &&
            statisticsLz4Index && row[*statisticsLz4Index].Type != EValueType::Null)
        {
            auto statisticsLz4 = FromUnversionedValue<TStringBuf>(row[*statisticsLz4Index]);
            auto codec = NCompression::GetCodec(NCompression::ECodec::Lz4);
            auto decompressed = codec->Decompress(TSharedRef(statisticsLz4.data(), statisticsLz4.size(), nullptr));
            auto statisticsYson = TYsonStringBuf(TStringBuf(decompressed.Begin(), decompressed.Size()));
            if (needFullStatistics) {
                job.Statistics = TYsonString(statisticsYson);
            }
            auto statistics = ConvertToNode(statisticsYson);
            job.BriefStatistics = BuildBriefStatistics(statistics);
        }

        if (eventsIndex && row[*eventsIndex].Type != EValueType::Null) {
            job.Events = FromUnversionedValue<TYsonString>(row[*eventsIndex]);
        }

        if (execAttributesIndex && row[*execAttributesIndex].Type != EValueType::Null) {
            job.ExecAttributes = FromUnversionedValue<TYsonString>(row[*execAttributesIndex]);
        }

        if (taskNameIndex && row[*taskNameIndex].Type != EValueType::Null) {
            job.TaskName = FromUnversionedValue<TString>(row[*taskNameIndex]);
        }

        if (poolTreeIndex && row[*poolTreeIndex].Type != EValueType::Null) {
            job.PoolTree = FromUnversionedValue<TString>(row[*poolTreeIndex]);
        }

        if (monitoringDescriptorIndex && row[*monitoringDescriptorIndex].Type != EValueType::Null) {
            job.MonitoringDescriptor = FromUnversionedValue<TString>(row[*monitoringDescriptorIndex]);
        }

        // We intentionally mark stderr as missing if job has no spec since
        // it is impossible to check permissions without spec.
        if (job.GetState() && NJobTrackerClient::IsJobFinished(*job.GetState()) && !job.HasSpec) {
            job.StderrSize = std::nullopt;
        }
    }
    return jobs;
}

TFuture<std::vector<TJob>> TClient::DoListJobsFromArchiveAsync(
    TOperationId operationId,
    TInstant deadline,
    const TListJobsOptions& options)
{
    auto builder = GetListJobsQueryBuilder(operationId, options);

    builder.SetLimit(options.Limit + options.Offset);

    builder.AddSelectExpression("job_id_hi");
    builder.AddSelectExpression("job_id_lo");
    builder.AddSelectExpression("type", "job_type");
    builder.AddSelectExpression("if(is_null(state), transient_state, state)", "job_state");
    builder.AddSelectExpression("start_time");
    builder.AddSelectExpression("finish_time");
    builder.AddSelectExpression("address");
    builder.AddSelectExpression("error");
    builder.AddSelectExpression("statistics");
    builder.AddSelectExpression("statistics_lz4");
    builder.AddSelectExpression("stderr_size");
    builder.AddSelectExpression("has_spec");
    builder.AddSelectExpression("fail_context_size");
    builder.AddSelectExpression("job_competition_id");
    builder.AddSelectExpression("has_competitors");
    builder.AddSelectExpression("exec_attributes");
    builder.AddSelectExpression("task_name");
    builder.AddSelectExpression("pool_tree");
    builder.AddSelectExpression("monitoring_descriptor");
    if (constexpr int requiredVersion = 31; DoGetOperationsArchiveVersion() >= requiredVersion) {
        builder.AddSelectExpression("core_infos");
    }

    if (options.WithStderr) {
        if (*options.WithStderr) {
            builder.AddWhereConjunct("stderr_size != 0 AND NOT is_null(stderr_size)");
        } else {
            builder.AddWhereConjunct("stderr_size = 0 OR is_null(stderr_size)");
        }
    }

    if (options.WithSpec) {
        if (*options.WithSpec) {
            builder.AddWhereConjunct("has_spec");
        } else {
            builder.AddWhereConjunct("NOT has_spec OR is_null(has_spec)");
        }
    }

    if (options.WithFailContext) {
        if (*options.WithFailContext) {
            builder.AddWhereConjunct("fail_context_size != 0 AND NOT is_null(fail_context_size)");
        } else {
            builder.AddWhereConjunct("fail_context_size = 0 OR is_null(fail_context_size)");
        }
    }

    if (options.Type) {
        builder.AddWhereConjunct(Format("job_type = %Qv", FormatEnum(*options.Type)));
    }

    if (options.State) {
        builder.AddWhereConjunct(Format("job_state = %Qv", FormatEnum(*options.State)));
    }

    if (options.JobCompetitionId) {
        builder.AddWhereConjunct(Format("job_competition_id = %Qv", options.JobCompetitionId));
    }

    if (options.WithCompetitors) {
        if (*options.WithCompetitors) {
            builder.AddWhereConjunct("has_competitors");
        } else {
            builder.AddWhereConjunct("is_null(has_competitors) OR NOT has_competitors");
        }
    }

    if (options.TaskName) {
        builder.AddWhereConjunct(Format("task_name = %Qv", *options.TaskName));
    }

    if (options.SortField != EJobSortField::None) {
        auto orderByDirection = [&] {
            switch (options.SortOrder) {
                case EJobSortDirection::Ascending:
                    return EOrderByDirection::Ascending;
                case EJobSortDirection::Descending:
                    return EOrderByDirection::Descending;
            }
            YT_ABORT();
        }();
        auto orderByFieldExpressions = [&] () -> std::vector<TString> {
            switch (options.SortField) {
                case EJobSortField::Type:
                    return {"job_type"};
                case EJobSortField::State:
                    return {"job_state"};
                case EJobSortField::StartTime:
                    return {"start_time"};
                case EJobSortField::FinishTime:
                    return {"finish_time"};
                case EJobSortField::Address:
                    return {"address"};
                case EJobSortField::Duration:
                    return {Format("if(is_null(finish_time), %v, finish_time) - start_time", TInstant::Now().MicroSeconds())};
                case EJobSortField::Id:
                case EJobSortField::None:
                    // We sort by id anyway.
                    return {};
                case EJobSortField::Progress:
                    // XXX: progress is not present in archive table.
                    return {};
            }
            YT_ABORT();
        }();
        orderByFieldExpressions.push_back("format_guid(job_id_hi, job_id_lo)");
        builder.AddOrderByExpression(JoinSeq(",", orderByFieldExpressions), orderByDirection);
    }

    TSelectRowsOptions selectRowsOptions;
    selectRowsOptions.Timestamp = AsyncLastCommittedTimestamp;
    selectRowsOptions.Timeout = deadline - Now();
    selectRowsOptions.InputRowLimit = std::numeric_limits<i64>::max();
    selectRowsOptions.MemoryLimitPerNode = 100_MB;

    return SelectRows(builder.Build(), selectRowsOptions).Apply(BIND([operationId] (const TSelectRowsResult& result) {
        return ParseJobsFromArchiveResponse(operationId, result.Rowset, /* needFullStatistics */ false);
    }));
}

static void ParseJobsFromControllerAgentResponse(
    TOperationId operationId,
    const std::vector<std::pair<TString, INodePtr>>& jobNodes,
    const std::function<bool(const INodePtr&)>& filter,
    const THashSet<TString>& attributes,
    std::vector<TJob>* jobs)
{
    auto needJobId = attributes.contains("job_id");
    auto needOperationId = attributes.contains("operation_id");
    auto needType = attributes.contains("type");
    auto needState = attributes.contains("state");
    auto needStartTime = attributes.contains("start_time");
    auto needFinishTime = attributes.contains("finish_time");
    auto needAddress = attributes.contains("address");
    auto needHasSpec = attributes.contains("has_spec");
    auto needProgress = attributes.contains("progress");
    auto needStderrSize = attributes.contains("stderr_size");
    auto needBriefStatistics = attributes.contains("brief_statistics");
    auto needJobCompetitionId = attributes.contains("job_competition_id");
    auto needHasCompetitors = attributes.contains("has_competitors");
    auto needError = attributes.contains("error");
    auto needTaskName = attributes.contains("task_name");

    for (const auto& [jobIdString, jobNode] : jobNodes) {
        if (!filter(jobNode)) {
            continue;
        }

        const auto& jobMapNode = jobNode->AsMap();
        auto& job = jobs->emplace_back();
        if (needJobId) {
            job.Id = TJobId::FromString(jobIdString);
        }
        if (needOperationId) {
            job.OperationId = operationId;
        }
        if (needType) {
            job.Type = ConvertTo<EJobType>(jobMapNode->GetChildOrThrow("job_type"));
        }
        if (needState) {
            job.ControllerAgentState = ConvertTo<EJobState>(jobMapNode->GetChildOrThrow("state"));
        }
        if (needStartTime) {
            job.StartTime = ConvertTo<TInstant>(jobMapNode->GetChildOrThrow("start_time")->GetValue<TString>());
        }
        if (needFinishTime) {
            if (auto child = jobMapNode->FindChild("finish_time")) {
                job.FinishTime = ConvertTo<TInstant>(child->GetValue<TString>());
            }
        }
        if (needAddress) {
            job.Address = jobMapNode->GetChildOrThrow("address")->GetValue<TString>();
        }
        if (needHasSpec) {
            job.HasSpec = true;
        }
        if (needProgress) {
            job.Progress = jobMapNode->GetChildOrThrow("progress")->GetValue<double>();
        }

        auto stderrSize = jobMapNode->GetChildOrThrow("stderr_size")->GetValue<i64>();
        if (stderrSize > 0 && needStderrSize) {
            job.StderrSize = stderrSize;
        }

        if (needBriefStatistics) {
            job.BriefStatistics = ConvertToYsonString(jobMapNode->GetChildOrThrow("brief_statistics"));
        }
        if (needJobCompetitionId) {
            //COMPAT(renadeen): can remove this check when 19.8 will be on all clusters
            if (auto child = jobMapNode->FindChild("job_competition_id")) {
                job.JobCompetitionId = ConvertTo<TJobId>(child);
            }
        }
        if (needHasCompetitors) {
            //COMPAT(renadeen): can remove this check when 19.8 will be on all clusters
            if (auto child = jobMapNode->FindChild("has_competitors")) {
                job.HasCompetitors = ConvertTo<bool>(child);
            }
        }
        if (needError) {
            if (auto child = jobMapNode->FindChild("error")) {
                job.Error = ConvertToYsonString(ConvertTo<TError>(child));
            }
        }
        if (needTaskName) {
            if (auto child = jobMapNode->FindChild("task_name")) {
                job.TaskName = ConvertTo<TString>(child);
            }
        }
    }
}

static void ParseJobsFromControllerAgentResponse(
    TOperationId operationId,
    const TObjectServiceProxy::TRspExecuteBatchPtr& batchRsp,
    const TString& key,
    const THashSet<TString>& attributes,
    const TListJobsOptions& options,
    std::vector<TJob>* jobs,
    int* totalCount,
    NLogging::TLogger Logger)
{
    auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>(key);
    if (rspOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
        return;
    }
    if (!rspOrError.IsOK()) {
        THROW_ERROR_EXCEPTION("Cannot get %Qv from controller agent", key)
            << rspOrError;
    }

    auto rsp = rspOrError.Value();
    auto items = ConvertToNode(NYson::TYsonString(rsp->value()))->AsMap();
    *totalCount += items->GetChildren().size();

    YT_LOG_DEBUG("Received %v jobs from controller agent (Count: %v)", key, items->GetChildren().size());

    auto filter = [&] (const INodePtr& jobNode) -> bool {
        const auto& jobMap = jobNode->AsMap();
        auto address = jobMap->GetChildOrThrow("address")->GetValue<TString>();
        auto type = ConvertTo<EJobType>(jobMap->GetChildOrThrow("job_type"));
        auto state = ConvertTo<EJobState>(jobMap->GetChildOrThrow("state"));
        auto stderrSize = jobMap->GetChildOrThrow("stderr_size")->GetValue<i64>();
        auto failContextSizeNode = jobMap->FindChild("fail_context_size");
        auto failContextSize = failContextSizeNode
            ? failContextSizeNode->GetValue<i64>()
            : 0;
        auto jobCompetitionIdNode = jobMap->FindChild("job_competition_id");
        auto jobCompetitionId = jobCompetitionIdNode  //COMPAT(renadeen): can remove this check when 19.8 will be on all clusters
            ? ConvertTo<TJobId>(jobCompetitionIdNode)
            : TJobId();
        auto hasCompetitorsNode = jobMap->FindChild("has_competitors");
        auto hasCompetitors = hasCompetitorsNode  //COMPAT(renadeen): can remove this check when 19.8 will be on all clusters
            ? ConvertTo<bool>(hasCompetitorsNode)
            : false;
        auto taskNameNode = jobMap->FindChild("task_name");
        auto taskName = taskNameNode
            ? ConvertTo<TString>(taskNameNode)
            : "";
        return
            (!options.Address || options.Address == address) &&
            (!options.Type || options.Type == type) &&
            (!options.State || options.State == state) &&
            (!options.WithStderr || *options.WithStderr == (stderrSize > 0)) &&
            (!options.WithFailContext || *options.WithFailContext == (failContextSize > 0)) &&
            (!options.JobCompetitionId || options.JobCompetitionId == jobCompetitionId) &&
            (!options.WithCompetitors || options.WithCompetitors == hasCompetitors) &&
            (!options.TaskName || options.TaskName == taskName);
    };

    ParseJobsFromControllerAgentResponse(
        operationId,
        items->GetChildren(),
        filter,
        attributes,
        jobs);
}

TFuture<TClient::TListJobsFromControllerAgentResult> TClient::DoListJobsFromControllerAgentAsync(
    TOperationId operationId,
    const std::optional<TString>& controllerAgentAddress,
    TInstant deadline,
    const TListJobsOptions& options)
{
    if (!controllerAgentAddress) {
        return MakeFuture(TListJobsFromControllerAgentResult{});
    }

    TObjectServiceProxy proxy(GetMasterChannelOrThrow(EMasterChannelKind::Follower));
    proxy.SetDefaultTimeout(deadline - Now());
    auto batchReq = proxy.ExecuteBatch();

    batchReq->AddRequest(
        TYPathProxy::Get(GetControllerAgentOrchidRunningJobsPath(*controllerAgentAddress, operationId)),
        "running_jobs");

    batchReq->AddRequest(
        TYPathProxy::Get(GetControllerAgentOrchidRetainedFinishedJobsPath(*controllerAgentAddress, operationId)),
        "retained_finished_jobs");

    return batchReq->Invoke().Apply(
        BIND([operationId, options, this, this_ = MakeStrong(this)] (const TObjectServiceProxy::TRspExecuteBatchPtr& batchRsp) {
            TListJobsFromControllerAgentResult result;
            ParseJobsFromControllerAgentResponse(
                operationId,
                batchRsp,
                "running_jobs",
                DefaultListJobsAttributes,
                options,
                &result.InProgressJobs,
                &result.TotalInProgressJobCount,
                Logger);
            ParseJobsFromControllerAgentResponse(
                operationId,
                batchRsp,
                "retained_finished_jobs",
                DefaultListJobsAttributes,
                options,
                &result.FinishedJobs,
                &result.TotalFinishedJobCount,
                Logger);
            return result;
        }));
}

using TJobComparator = std::function<bool(const TJob&, const TJob&)>;

static TJobComparator GetJobsComparator(
    EJobSortField sortField,
    EJobSortDirection sortOrder)
{
    auto makeLessBy = [sortOrder] (auto key) -> TJobComparator {
        switch (sortOrder) {
            case EJobSortDirection::Ascending:
                return [=] (const TJob& lhs, const TJob& rhs) {
                    auto lhsKey = key(lhs);
                    auto rhsKey = key(rhs);
                    return lhsKey < rhsKey || (lhsKey == rhsKey && lhs.Id < rhs.Id);
                };
            case EJobSortDirection::Descending:
                return [=] (const TJob& lhs, const TJob& rhs) {
                    auto lhsKey = key(lhs);
                    auto rhsKey = key(rhs);
                    return rhsKey < lhsKey || (rhsKey == lhsKey && rhs.Id < lhs.Id);
                };
        }
        YT_ABORT();
    };

    auto makeLessByField = [&] (auto TJob::* field) {
        return makeLessBy([field] (const TJob& job) {
            return job.*field;
        });
    };

    switch (sortField) {
        case EJobSortField::Type:
            return makeLessBy([] (const TJob& job) -> std::optional<TString> {
                if (auto type = job.Type) {
                    return FormatEnum(*type);
                } else {
                    return std::nullopt;
                }
            });
        case EJobSortField::State:
            return makeLessBy([] (const TJob& job) -> std::optional<TString> {
                if (auto state = job.GetState()) {
                    return FormatEnum(*state);
                } else {
                    return std::nullopt;
                }
            });
        case EJobSortField::StartTime:
            return makeLessByField(&TJob::StartTime);
        case EJobSortField::FinishTime:
            return makeLessByField(&TJob::FinishTime);
        case EJobSortField::Address:
            return makeLessByField(&TJob::Address);
        case EJobSortField::Progress:
            return makeLessByField(&TJob::Progress);
        case EJobSortField::None:
            return makeLessByField(&TJob::Id);
        case EJobSortField::Id:
            return makeLessBy([] (const TJob& job) {
                return ToString(job.Id);
            });
        case EJobSortField::Duration:
            return makeLessBy([now = TInstant::Now()] (const TJob& job) -> std::optional<TDuration> {
                if (job.StartTime) {
                    return (job.FinishTime ? *job.FinishTime : now) - *job.StartTime;
                } else {
                    return std::nullopt;
                }
            });
    }
    YT_ABORT();
}

static void MergeJobs(TJob&& controllerAgentJob, TJob* archiveJob)
{
    if (auto archiveState = archiveJob->GetState(); archiveState && IsJobFinished(*archiveState)) {
        // Archive job is most recent, it will not change anymore.
        return;
    }

    auto mergeNullableField = [&] (auto TJob::* field) {
        if (controllerAgentJob.*field) {
            archiveJob->*field = std::move(controllerAgentJob.*field);
        }
    };

    mergeNullableField(&TJob::Type);
    mergeNullableField(&TJob::ControllerAgentState);
    mergeNullableField(&TJob::ArchiveState);
    mergeNullableField(&TJob::Progress);
    mergeNullableField(&TJob::StartTime);
    mergeNullableField(&TJob::FinishTime);
    mergeNullableField(&TJob::Address);
    mergeNullableField(&TJob::Progress);
    mergeNullableField(&TJob::Error);
    mergeNullableField(&TJob::BriefStatistics);
    mergeNullableField(&TJob::InputPaths);
    mergeNullableField(&TJob::CoreInfos);
    mergeNullableField(&TJob::JobCompetitionId);
    mergeNullableField(&TJob::HasCompetitors);
    mergeNullableField(&TJob::ExecAttributes);
    mergeNullableField(&TJob::TaskName);
    mergeNullableField(&TJob::PoolTree);
    if (controllerAgentJob.StderrSize && archiveJob->StderrSize.value_or(0) < controllerAgentJob.StderrSize) {
        archiveJob->StderrSize = controllerAgentJob.StderrSize;
    }
}

static void UpdateJobsAndAddMissing(std::vector<std::vector<TJob>>&& controllerAgentJobs, std::vector<TJob>* archiveJobs)
{
    THashMap<TJobId, TJob*> jobIdToArchiveJob;
    for (auto& job : *archiveJobs) {
        jobIdToArchiveJob.emplace(job.Id, &job);
    }
    std::vector<TJob> newJobs;
    for (auto& jobs : controllerAgentJobs) {
        for (auto& job : jobs) {
            if (auto it = jobIdToArchiveJob.find(job.Id); it != jobIdToArchiveJob.end()) {
                MergeJobs(std::move(job), it->second);
            } else {
                newJobs.push_back(std::move(job));
            }
        }
    }
    archiveJobs->insert(
        archiveJobs->end(),
        std::make_move_iterator(newJobs.begin()),
        std::make_move_iterator(newJobs.end()));
}

static bool IsJobStale(std::optional<EJobState> controllerAgentState, std::optional<EJobState> archiveState)
{
    return !controllerAgentState && archiveState && IsJobInProgress(*archiveState);
}

static TError TryFillJobPools(
    const IClientPtr& client,
    TOperationId operationId,
    TMutableRange<TJob> jobs,
    NLogging::TLogger Logger)
{
    TGetOperationOptions getOperationOptions;
    getOperationOptions.Attributes = {TString("runtime_parameters")};

    auto operationOrError = WaitFor(client->GetOperation(operationId, getOperationOptions));
    if (!operationOrError.IsOK()) {
        YT_LOG_DEBUG(operationOrError, "Failed to fetch operation to extract pools (OperationId: %v)",
            operationId);
        return operationOrError;
    }

    auto path = "/scheduling_options_per_pool_tree";
    auto schedulingOptionsPerPoolTreeYson = TryGetAny(operationOrError.Value().RuntimeParameters.AsStringBuf(), path);
    if (!schedulingOptionsPerPoolTreeYson) {
        YT_LOG_DEBUG("Operation runtime_parameters miss scheduling_options_per_pool_tree (OperationId: %v)",
            operationId);
        return TError("Operation %v runtime_parameters miss scheduling_options_per_pool_tree",
            operationId);
    }

    auto schedulingOptionPerPoolTree = ConvertTo<THashMap<TString, INodePtr>>(
        TYsonStringBuf(*schedulingOptionsPerPoolTreeYson));

    for (auto& job : jobs) {
        if (!job.PoolTree) {
            return TError(Format("Pool tree is missing in job %v", job.Id));
        }
        auto optionsIt = schedulingOptionPerPoolTree.find(*job.PoolTree);
        if (optionsIt == schedulingOptionPerPoolTree.end()) {
            return TError(Format("Pool tree %Qv is not found in scheduling_options_per_pool_tree", *job.PoolTree));
        }
        const auto& optionsNode = optionsIt->second;
        auto poolNode = optionsNode->AsMap()->FindChild("pool");
        if (!poolNode) {
            return TError(Format("%Qv field is missing in scheduling_options_per_pool_tree for tree %Qv", "pool", *job.PoolTree));
        }
        job.Pool = ConvertTo<TString>(poolNode);
    }

    return TError();
}

TListJobsResult TClient::DoListJobs(
    const TOperationIdOrAlias& operationIdOrAlias,
    const TListJobsOptions& options)
{
    auto timeout = options.Timeout.value_or(Connection_->GetConfig()->DefaultListJobsTimeout);
    auto deadline = timeout.ToDeadLine();

    TOperationId operationId;
    Visit(operationIdOrAlias.Payload,
        [&] (const TOperationId& id) {
            operationId = id;
        },
        [&] (const TString& alias) {
            operationId = ResolveOperationAlias(alias, options, deadline);
        });

    // Issue the requests in parallel.
    TFuture<std::vector<TJob>> archiveResultFuture;
    TFuture<TListJobsStatistics> statisticsFuture;
    if (DoesOperationsArchiveExist()) {
        archiveResultFuture = DoListJobsFromArchiveAsync(
            operationId,
            deadline,
            options);
        statisticsFuture = ListJobsStatisticsFromArchiveAsync(operationId, deadline, options);
    }

    auto controllerAgentAddress = FindControllerAgentAddressFromCypress(
        operationId,
        GetMasterChannelOrThrow(EMasterChannelKind::Follower));
    auto controllerAgentResultFuture = DoListJobsFromControllerAgentAsync(
        operationId,
        controllerAgentAddress,
        deadline,
        options);

    // Wait for results and extract them.
    TListJobsResult result;
    TListJobsFromControllerAgentResult controllerAgentResult;
    auto controllerAgentResultOrError = WaitFor(controllerAgentResultFuture);
    if (controllerAgentResultOrError.IsOK()) {
        controllerAgentResult = std::move(controllerAgentResultOrError.Value());
        result.ControllerAgentJobCount =
            controllerAgentResult.TotalFinishedJobCount + controllerAgentResult.TotalInProgressJobCount;
    } else if (controllerAgentResultOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
        // No such operation in the controller agent.
        result.ControllerAgentJobCount = 0;
    } else {
        result.Errors.push_back(std::move(controllerAgentResultOrError));
    }

    std::vector<TJob> archiveResult;
    if (archiveResultFuture) {
        auto archiveResultOrError = WaitFor(archiveResultFuture);
        if (archiveResultOrError.IsOK()) {
            archiveResult = std::move(archiveResultOrError.Value());
        } else {
            result.Errors.push_back(TError(
                EErrorCode::JobArchiveUnavailable,
                "Job archive is unavailable")
                << archiveResultOrError);
        }
    }

    // Combine the results if necessary.
    if (!controllerAgentAddress) {
        result.Jobs = std::move(archiveResult);
    } else {
        UpdateJobsAndAddMissing(
            {std::move(controllerAgentResult.InProgressJobs), std::move(controllerAgentResult.FinishedJobs)},
            &archiveResult);
        result.Jobs = std::move(archiveResult);
        auto jobComparator = GetJobsComparator(options.SortField, options.SortOrder);
        std::sort(result.Jobs.begin(), result.Jobs.end(), jobComparator);
    }

    // Take the correct range [offset, offset + limit).
    auto beginIt = std::min(result.Jobs.end(), result.Jobs.begin() + options.Offset);
    auto endIt = std::min(result.Jobs.end(), beginIt + options.Limit);
    result.Jobs = std::vector<TJob>(std::make_move_iterator(beginIt), std::make_move_iterator(endIt));

    // Extract statistics if available.
    if (statisticsFuture) {
        auto statisticsOrError = WaitFor(statisticsFuture);
        if (!statisticsOrError.IsOK()) {
            result.Errors.push_back(TError(
                EErrorCode::JobArchiveUnavailable,
                "Failed to fetch statistics from job archive")
                << statisticsOrError);
        } else {
            result.Statistics = std::move(statisticsOrError).Value();
            result.ArchiveJobCount = 0;
            for (auto count : result.Statistics.TypeCounts) {
                *result.ArchiveJobCount += count;
            }
        }
    }

    // Compute pools.
    auto error = TryFillJobPools(this, operationId, TMutableRange(result.Jobs), Logger);
    if (!error.IsOK()) {
        YT_LOG_DEBUG(error, "Failed to fill job pools (OperationId: %v)",
            operationId);
    }

    // Compute job staleness.
    for (auto& job : result.Jobs) {
        job.IsStale = IsJobStale(job.ControllerAgentState, job.ArchiveState);
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

static std::vector<TString> MakeJobArchiveAttributes(const THashSet<TString>& attributes)
{
    std::vector<TString> result;
    // Plus 2 as operation_id and job_id are split into hi and lo.
    result.reserve(attributes.size() + 2);
    for (const auto& attribute : attributes) {
        if (!SupportedJobAttributes.contains(attribute)) {
            THROW_ERROR_EXCEPTION(
                NApi::EErrorCode::NoSuchAttribute,
                "Job attribute %Qv is not supported",
                attribute)
                << TErrorAttribute("attribute_name", attribute);
        }
        if (attribute == "operation_id" || attribute == "job_id") {
            result.push_back(attribute + "_hi");
            result.push_back(attribute + "_lo");
        } else if (attribute == "state") {
            result.emplace_back("state");
            result.emplace_back("transient_state");
        } else if (attribute == "statistics") {
            result.emplace_back("statistics");
            result.emplace_back("statistics_lz4");
        } else if (attribute == "progress" || attribute == "pool") {
            // Progress and pool are missing from job archive.
        } else {
            result.push_back(attribute);
        }
    }
    return result;
}

std::optional<TJob> TClient::DoGetJobFromArchive(
    TOperationId operationId,
    TJobId jobId,
    TInstant deadline,
    const THashSet<TString>& attributes)
{
    TJobTableDescriptor table;
    auto rowBuffer = New<TRowBuffer>();

    std::vector<TUnversionedRow> keys;
    auto key = rowBuffer->AllocateUnversioned(4);
    key[0] = MakeUnversionedUint64Value(operationId.Parts64[0], table.Index.OperationIdHi);
    key[1] = MakeUnversionedUint64Value(operationId.Parts64[1], table.Index.OperationIdLo);
    key[2] = MakeUnversionedUint64Value(jobId.Parts64[0], table.Index.JobIdHi);
    key[3] = MakeUnversionedUint64Value(jobId.Parts64[1], table.Index.JobIdLo);
    keys.push_back(key);


    std::vector<int> columnIndexes;
    auto fields = MakeJobArchiveAttributes(attributes);
    for (const auto& field : fields) {
        columnIndexes.push_back(table.NameTable->GetIdOrThrow(field));
    }

    TLookupRowsOptions lookupOptions;
    lookupOptions.ColumnFilter = NTableClient::TColumnFilter(columnIndexes);
    lookupOptions.KeepMissingRows = true;
    lookupOptions.Timeout = deadline - Now();

    auto rowset = WaitFor(LookupRows(
        GetOperationsArchiveJobsPath(),
        table.NameTable,
        MakeSharedRange(std::move(keys), std::move(rowBuffer)),
        lookupOptions))
        .ValueOrThrow();

    auto rows = rowset->GetRows();
    YT_VERIFY(!rows.Empty());
    if (!rows[0]) {
        return {};
    }

    auto jobs = ParseJobsFromArchiveResponse(operationId, rowset, /* needFullStatistics */ true);
    YT_VERIFY(!jobs.empty());
    return std::move(jobs.front());
}

std::optional<TJob> TClient::DoGetJobFromControllerAgent(
    TOperationId operationId,
    TJobId jobId,
    TInstant deadline,
    const THashSet<TString>& attributes)
{
    auto controllerAgentAddress = FindControllerAgentAddressFromCypress(
        operationId,
        GetMasterChannelOrThrow(EMasterChannelKind::Follower));
    if (!controllerAgentAddress) {
        return {};
    }

    TObjectServiceProxy proxy(GetMasterChannelOrThrow(EMasterChannelKind::Follower));
    proxy.SetDefaultTimeout(deadline - Now());
    auto batchReq = proxy.ExecuteBatch();

    auto runningJobPath =
        GetControllerAgentOrchidRunningJobsPath(*controllerAgentAddress, operationId) + "/" + ToString(jobId);
    batchReq->AddRequest(TYPathProxy::Get(runningJobPath));

    auto finishedJobPath =
        GetControllerAgentOrchidRetainedFinishedJobsPath(*controllerAgentAddress, operationId) + "/" + ToString(jobId);
    batchReq->AddRequest(TYPathProxy::Get(finishedJobPath));

    auto batchRspOrError = WaitFor(batchReq->Invoke());

    if (!batchRspOrError.IsOK()) {
        THROW_ERROR_EXCEPTION("Cannot get jobs from controller agent")
            << batchRspOrError;
    }

    for (const auto& rspOrError : batchRspOrError.Value()->GetResponses<TYPathProxy::TRspGet>()) {
        if (rspOrError.IsOK()) {
            std::vector<TJob> jobs;
            ParseJobsFromControllerAgentResponse(
                operationId,
                {{ToString(jobId), ConvertToNode(TYsonString(rspOrError.Value()->value()))}},
                [] (const INodePtr&) {
                    return true;
                },
                attributes,
                &jobs);
            YT_VERIFY(jobs.size() == 1);
            return jobs[0];
        } else if (!rspOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
            THROW_ERROR_EXCEPTION("Cannot get jobs from controller agent")
                << rspOrError;
        }
    }

    return {};
}

TYsonString TClient::DoGetJob(
    const TOperationIdOrAlias& operationIdOrAlias,
    TJobId jobId,
    const TGetJobOptions& options)
{
    auto timeout = options.Timeout.value_or(Connection_->GetConfig()->DefaultGetJobTimeout);
    auto deadline = timeout.ToDeadLine();

    TOperationId operationId;
    Visit(operationIdOrAlias.Payload,
        [&] (const TOperationId& id) {
            operationId = id;
        },
        [&] (const TString& alias) {
            operationId = ResolveOperationAlias(alias, options, deadline);
        });

    const auto& attributes = options.Attributes.value_or(DefaultGetJobAttributes);

    auto controllerAgentJob = DoGetJobFromControllerAgent(operationId, jobId, deadline, attributes);
    auto archiveJob = DoGetJobFromArchive(operationId, jobId, deadline, attributes);

    TJob job;
    if (archiveJob && controllerAgentJob) {
        job = std::move(*archiveJob);
        MergeJobs(std::move(*controllerAgentJob), &job);
    } else if (archiveJob) {
        job = std::move(*archiveJob);
    } else if (controllerAgentJob) {
        job = std::move(*controllerAgentJob);
    } else {
        THROW_ERROR_EXCEPTION(
            EErrorCode::NoSuchJob,
            "Job %v or operation %v not found neither in archive nor in controller agent",
            jobId,
            operationId);
    }

    job.IsStale = IsJobStale(job.ControllerAgentState, job.ArchiveState);

    if (attributes.contains("pool")) {
        auto error = TryFillJobPools(this, operationId, TMutableRange(&job, 1), Logger);
        if (!error.IsOK()) {
            YT_LOG_DEBUG(error, "Failed to fill job pools (OperationId: %v, JobId: %v)",
                operationId,
                jobId);
        }
    }

    return BuildYsonStringFluently()
        .Do([&] (TFluentAny fluent) {
            Serialize(job, fluent.GetConsumer(), "job_id");
        });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
