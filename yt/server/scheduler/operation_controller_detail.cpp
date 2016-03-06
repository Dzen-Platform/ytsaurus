#include "operation_controller_detail.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "chunk_pool.h"
#include "helpers.h"
#include "master_connector.h"

#include <yt/ytlib/chunk_client/chunk_list_ypath_proxy.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_scraper.h>
#include <yt/ytlib/chunk_client/chunk_slice.h>
#include <yt/ytlib/chunk_client/data_statistics.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/node_tracker_client/node_directory_builder.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/query_client/plan_fragment.h>
#include <yt/ytlib/query_client/query_preparer.h>
#include <yt/ytlib/query_client/udf_descriptor.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_ypath_proxy.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/fs.h>

#include <functional>

namespace NYT {
namespace NScheduler {

using namespace NNodeTrackerClient;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NFormats;
using namespace NJobProxy;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NNodeTrackerClient::NProto;
using namespace NConcurrency;
using namespace NApi;
using namespace NRpc;
using namespace NTableClient;
using namespace NQueryClient;

using NTableClient::NProto::TBoundaryKeysExt;
using NTableClient::NProto::TOldBoundaryKeysExt;

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TUserTableBase::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Path);
    Persist(context, ObjectId);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TLivePreviewTableBase::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, LivePreviewTableId);
    Persist(context, LivePreviewChunkListId);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TInputTable::Persist(TPersistenceContext& context)
{
    TUserTableBase::Persist(context);

    using NYT::Persist;
    Persist(context, ChunkCount);
    Persist(context, Chunks);
    Persist(context, KeyColumns);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TJobBoundaryKeys::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, MinKey);
    Persist(context, MaxKey);
    Persist(context, ChunkTreeKey);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TOutputTable::Persist(TPersistenceContext& context)
{
    TUserTableBase::Persist(context);
    TLivePreviewTableBase::Persist(context);

    using NYT::Persist;
    Persist(context, AppendRequested);
    Persist(context, UpdateMode);
    Persist(context, LockMode);
    Persist(context, Options);
    Persist(context, KeyColumns);
    Persist(context, OutputChunkListId);
    // NB: Scheduler snapshots need not be stable.
    Persist<
        TMultiMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, OutputChunkTreeIds);
    Persist(context, BoundaryKeys);
    Persist(context, EffectiveAcl);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TIntermediateTable::Persist(TPersistenceContext& context)
{
    TLivePreviewTableBase::Persist(context);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TUserFile::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Path);
    Persist(context, Stage);
    Persist(context, FileName);
    Persist(context, FetchResponse);
    Persist(context, Type);
    Persist(context, Executable);
    Persist(context, Format);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TCompletedJob::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, IsLost);
    Persist(context, JobId);
    Persist(context, SourceTask);
    Persist(context, OutputCookie);
    Persist(context, DestinationPool);
    Persist(context, InputCookie);
    Persist(context, Address);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TJoblet::Persist(TPersistenceContext& context)
{
    // NB: Every joblet is aborted after snapshot is loaded.
    // Here we only serialize a subset of members required for ReinstallJob to work
    // properly.
    using NYT::Persist;
    Persist(context, Task);
    Persist(context, InputStripeList);
    Persist(context, OutputCookie);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TTaskGroup::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, MinNeededResources);
    // NB: Scheduler snapshots need not be stable.
    Persist<
        TSetSerializer<
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, NonLocalTasks);
    Persist<
        TMultiMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, CandidateTasks);
    Persist<
        TMultiMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, DelayedTasks);
    Persist<
        TMapSerializer<
            TDefaultSerializer,
            TSetSerializer<
                TDefaultSerializer,
                TUnsortedTag
            >,
            TUnsortedTag
        >
    >(context, LocalTasks);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TStripeDescriptor::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Stripe);
    Persist(context, Cookie);
    Persist(context, Task);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TInputChunkDescriptor::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, InputStripes);
    Persist(context, ChunkSpecs);
    Persist(context, State);
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TTask::TTask()
    : CachedPendingJobCount(-1)
    , CachedTotalJobCount(-1)
    , LastDemandSanityCheckTime(TInstant::Zero())
    , CompletedFired(false)
    , Logger(OperationLogger)
{ }

TOperationControllerBase::TTask::TTask(TOperationControllerBase* controller)
    : Controller(controller)
    , CachedPendingJobCount(0)
    , CachedTotalJobCount(0)
    , LastDemandSanityCheckTime(TInstant::Zero())
    , CompletedFired(false)
    , Logger(OperationLogger)
{ }

void TOperationControllerBase::TTask::Initialize()
{
    Logger = Controller->Logger;
    Logger.AddTag("Task: %v", GetId());
}

int TOperationControllerBase::TTask::GetPendingJobCount() const
{
    return GetChunkPoolOutput()->GetPendingJobCount();
}

int TOperationControllerBase::TTask::GetPendingJobCountDelta()
{
    int oldValue = CachedPendingJobCount;
    int newValue = GetPendingJobCount();
    CachedPendingJobCount = newValue;
    return newValue - oldValue;
}

int TOperationControllerBase::TTask::GetTotalJobCount() const
{
    return GetChunkPoolOutput()->GetTotalJobCount();
}

int TOperationControllerBase::TTask::GetTotalJobCountDelta()
{
    int oldValue = CachedTotalJobCount;
    int newValue = GetTotalJobCount();
    CachedTotalJobCount = newValue;
    return newValue - oldValue;
}

TNodeResources TOperationControllerBase::TTask::GetTotalNeededResourcesDelta()
{
    auto oldValue = CachedTotalNeededResources;
    auto newValue = GetTotalNeededResources();
    CachedTotalNeededResources = newValue;
    newValue -= oldValue;
    return newValue;
}

TNodeResources TOperationControllerBase::TTask::GetTotalNeededResources() const
{
    i64 count = GetPendingJobCount();
    // NB: Don't call GetMinNeededResources if there are no pending jobs.
    return count == 0 ? ZeroNodeResources() : GetMinNeededResources() * count;
}

i64 TOperationControllerBase::TTask::GetLocality(const Stroka& address) const
{
    return GetChunkPoolOutput()->GetLocality(address);
}

bool TOperationControllerBase::TTask::HasInputLocality() const
{
    return true;
}

void TOperationControllerBase::TTask::AddInput(TChunkStripePtr stripe)
{
    Controller->RegisterInputStripe(stripe, this);
    if (HasInputLocality()) {
        Controller->AddTaskLocalityHint(this, stripe);
    }
    AddPendingHint();
}

void TOperationControllerBase::TTask::AddInput(const std::vector<TChunkStripePtr>& stripes)
{
    for (auto stripe : stripes) {
        if (stripe) {
            AddInput(stripe);
        }
    }
}

void TOperationControllerBase::TTask::FinishInput()
{
    LOG_DEBUG("Task input finished");

    GetChunkPoolInput()->Finish();
    AddPendingHint();
    CheckCompleted();
}

void TOperationControllerBase::TTask::CheckCompleted()
{
    if (!CompletedFired && IsCompleted()) {
        CompletedFired = true;
        OnTaskCompleted();
    }
}

TJobId TOperationControllerBase::TTask::ScheduleJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    int chunkListCount = GetChunkListCountPerJob();
    int jobIndex = Controller->JobIndexGenerator.Next();
    auto joblet = New<TJoblet>(this, jobIndex);

    const auto& nodeResourceLimits = context->ResourceLimits();
    const auto& address = context->GetAddress();
    auto* chunkPoolOutput = GetChunkPoolOutput();
    joblet->OutputCookie = chunkPoolOutput->Extract(address);
    if (joblet->OutputCookie == IChunkPoolOutput::NullCookie) {
        LOG_DEBUG("Job input is empty");
        return NullJobId;
    }

    joblet->InputStripeList = chunkPoolOutput->GetStripeList(joblet->OutputCookie);
    joblet->MemoryReserveEnabled = IsMemoryReserveEnabled();

    auto neededResources = GetNeededResources(joblet);

    // Check the usage against the limits. This is the last chance to give up.
    if (!Dominates(jobLimits, neededResources)) {
        LOG_DEBUG("Job actual resource demand is not met (Limits: {%v}, Demand: {%v})",
            FormatResources(jobLimits),
            FormatResources(neededResources));
        CheckResourceDemandSanity(nodeResourceLimits, neededResources);
        chunkPoolOutput->Aborted(joblet->OutputCookie);
        // Seems like cached min needed resources are too optimistic.
        ResetCachedMinNeededResources();
        return NullJobId;
    }

    auto jobType = GetJobType();

    // Async part.
    auto controller = MakeStrong(Controller); // hold the controller
    auto jobSpecBuilder = BIND([=, this_ = MakeStrong(this)] (TJobSpec* jobSpec) {
        BuildJobSpec(joblet, jobSpec);
        controller->CustomizeJobSpec(joblet, jobSpec);

        auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_enable_job_proxy_memory_control(controller->Spec->EnableJobProxyMemoryControl);
        schedulerJobSpecExt->set_enable_sort_verification(controller->Spec->EnableSortVerification);

        // Adjust sizes if approximation flag is set.
        if (joblet->InputStripeList->IsApproximate) {
            schedulerJobSpecExt->set_input_uncompressed_data_size(static_cast<i64>(
                schedulerJobSpecExt->input_uncompressed_data_size() *
                ApproximateSizesBoostFactor));
            schedulerJobSpecExt->set_input_row_count(static_cast<i64>(
                schedulerJobSpecExt->input_row_count() *
                ApproximateSizesBoostFactor));
        }

        if (schedulerJobSpecExt->input_uncompressed_data_size() > Controller->Spec->MaxDataSizePerJob) {
            Controller->OnOperationFailed(TError(
                "Maximum allowed data size per job violated: %v > %v",
                schedulerJobSpecExt->input_uncompressed_data_size(),
                Controller->Spec->MaxDataSizePerJob));
        }
    });

    auto restarted = LostJobCookieMap.find(joblet->OutputCookie) != LostJobCookieMap.end();
    joblet->JobId = context->StartJob(
        Controller->Operation,
        jobType,
        neededResources,
        restarted,
        jobSpecBuilder);

    joblet->JobType = jobType;
    joblet->Address = context->GetAddress();

    LOG_INFO(
        "Job scheduled (JobId: %v, OperationId: %v, JobType: %v, Address: %v, JobIndex: %v, ChunkCount: %v (%v local), "
        "Approximate: %v, DataSize: %v (%v local), RowCount: %v, Restarted: %v, ResourceLimits: {%v})",
        joblet->JobId,
        Controller->OperationId,
        jobType,
        joblet->Address,
        jobIndex,
        joblet->InputStripeList->TotalChunkCount,
        joblet->InputStripeList->LocalChunkCount,
        joblet->InputStripeList->IsApproximate,
        joblet->InputStripeList->TotalDataSize,
        joblet->InputStripeList->LocalDataSize,
        joblet->InputStripeList->TotalRowCount,
        restarted,
        FormatResources(neededResources));

    // Prepare chunk lists.
    for (int index = 0; index < chunkListCount; ++index) {
        auto id = Controller->ExtractChunkList();
        joblet->ChunkListIds.push_back(id);
    }

    // Sync part.
    PrepareJoblet(joblet);
    Controller->CustomizeJoblet(joblet);

    Controller->RegisterJoblet(joblet);

    OnJobStarted(joblet);

    return joblet->JobId;
}

bool TOperationControllerBase::TTask::IsPending() const
{
    return GetChunkPoolOutput()->GetPendingJobCount() > 0;
}

bool TOperationControllerBase::TTask::IsCompleted() const
{
    return IsActive() && GetChunkPoolOutput()->IsCompleted();
}

bool TOperationControllerBase::TTask::IsActive() const
{
    return true;
}

i64 TOperationControllerBase::TTask::GetTotalDataSize() const
{
    return GetChunkPoolOutput()->GetTotalDataSize();
}

i64 TOperationControllerBase::TTask::GetCompletedDataSize() const
{
    return GetChunkPoolOutput()->GetCompletedDataSize();
}

i64 TOperationControllerBase::TTask::GetPendingDataSize() const
{
    return GetChunkPoolOutput()->GetPendingDataSize();
}

void TOperationControllerBase::TTask::Persist(TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, DelayedTime_);

    Persist(context, Controller);

    Persist(context, CachedPendingJobCount);
    Persist(context, CachedTotalJobCount);

    Persist(context, CachedTotalNeededResources);
    Persist(context, CachedMinNeededResources);

    Persist(context, LastDemandSanityCheckTime);

    Persist(context, CompletedFired);

    Persist(context, LostJobCookieMap);
}

void TOperationControllerBase::TTask::PrepareJoblet(TJobletPtr /* joblet */)
{ }

void TOperationControllerBase::TTask::OnJobStarted(TJobletPtr /* joblet */)
{ }

void TOperationControllerBase::TTask::OnJobCompleted(TJobletPtr joblet, const TCompletedJobSummary& jobSummary)
{
    if (!jobSummary.Abandoned) {
        const auto& statistics = jobSummary.Statistics;
        auto outputStatisticsMap = GetOutputDataStatistics(statistics);
        for (int index = 0; index < static_cast<int>(joblet->ChunkListIds.size()); ++index) {
            YCHECK(outputStatisticsMap.find(index) != outputStatisticsMap.end());
            auto outputStatistics = outputStatisticsMap[index];
            if (outputStatistics.chunk_count() == 0) {
                Controller->ChunkListPool->Reinstall(joblet->ChunkListIds[index]);
                joblet->ChunkListIds[index] = NullChunkListId;
            }
        }

        auto inputStatistics = GetTotalInputDataStatistics(statistics);
        auto outputStatistics = GetTotalOutputDataStatistics(statistics);
        if (Controller->IsRowCountPreserved()) {
            if (inputStatistics.row_count() != outputStatistics.row_count()) {
                Controller->OnOperationFailed(TError(
                    "Input/output row count mismatch in completed job: %v != %v",
                    inputStatistics.row_count(),
                    outputStatistics.row_count())
                    << TErrorAttribute("task", GetId()));
            }
        }
    } else {
        for (int index = 0; index < static_cast<int>(joblet->ChunkListIds.size()); ++index) {
            Controller->ChunkListPool->Reinstall(joblet->ChunkListIds[index]);
            joblet->ChunkListIds[index] = NullChunkListId;
        }
    }
    GetChunkPoolOutput()->Completed(joblet->OutputCookie);
}

void TOperationControllerBase::TTask::ReinstallJob(TJobletPtr joblet, EJobReinstallReason reason)
{
    Controller->ChunkListPool->Release(joblet->ChunkListIds);

    auto* chunkPoolOutput = GetChunkPoolOutput();

    auto list =
        HasInputLocality()
        ? chunkPoolOutput->GetStripeList(joblet->OutputCookie)
        : nullptr;

    switch (reason) {
        case EJobReinstallReason::Failed:
            chunkPoolOutput->Failed(joblet->OutputCookie);
            break;
        case EJobReinstallReason::Aborted:
            chunkPoolOutput->Aborted(joblet->OutputCookie);
            break;
        default:
            YUNREACHABLE();
    }

    if (HasInputLocality()) {
        for (const auto& stripe : list->Stripes) {
            Controller->AddTaskLocalityHint(this, stripe);
        }
    }

    AddPendingHint();
}

void TOperationControllerBase::TTask::OnJobFailed(TJobletPtr joblet, const TFailedJobSummary& /* jobSummary */)
{
    ReinstallJob(joblet, EJobReinstallReason::Failed);
}

void TOperationControllerBase::TTask::OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& /* jobSummary */)
{
    ReinstallJob(joblet, EJobReinstallReason::Aborted);
}

void TOperationControllerBase::TTask::OnJobLost(TCompletedJobPtr completedJob)
{
    YCHECK(LostJobCookieMap.insert(std::make_pair(
        completedJob->OutputCookie,
        completedJob->InputCookie)).second);
}

void TOperationControllerBase::TTask::OnTaskCompleted()
{
    LOG_DEBUG("Task completed");
}

void TOperationControllerBase::TTask::DoCheckResourceDemandSanity(
    const TNodeResources& neededResources)
{
    auto nodes = Controller->Host->GetExecNodes();
    if (nodes.size() < Controller->Config->SafeOnlineNodeCount)
        return;

    for (auto node : nodes) {
        if (node->CanSchedule(Controller->Operation->GetSchedulingTag()) &&
            Dominates(node->ResourceLimits(), neededResources))
        {
            return;
        }
    }

    // It seems nobody can satisfy the demand.
    Controller->OnOperationFailed(
        TError("No online node can satisfy the resource demand")
            << TErrorAttribute("task", GetId())
            << TErrorAttribute("needed_resources", neededResources));
}

void TOperationControllerBase::TTask::CheckResourceDemandSanity(
    const TNodeResources& neededResources)
{
    // Run sanity check to see if any node can provide enough resources.
    // Don't run these checks too often to avoid jeopardizing performance.
    auto now = TInstant::Now();
    if (now < LastDemandSanityCheckTime + Controller->Config->ResourceDemandSanityCheckPeriod)
        return;
    LastDemandSanityCheckTime = now;

    // Schedule check in control thread.
    Controller->GetCancelableControlInvoker()->Invoke(BIND(
        &TTask::DoCheckResourceDemandSanity,
        MakeWeak(this),
        neededResources));
}

void TOperationControllerBase::TTask::CheckResourceDemandSanity(
    const TNodeResources& nodeResourceLimits,
    const TNodeResources& neededResources)
{
    // The task is requesting more than some node is willing to provide it.
    // Maybe it's OK and we should wait for some time.
    // Or maybe it's not and the task is requesting something no one is able to provide.

    // First check if this very node has enough resources (including those currently
    // allocated by other jobs).
    if (Dominates(nodeResourceLimits, neededResources))
        return;

    CheckResourceDemandSanity(neededResources);
}

void TOperationControllerBase::TTask::AddPendingHint()
{
    Controller->AddTaskPendingHint(this);
}

void TOperationControllerBase::TTask::AddLocalityHint(const Stroka& address)
{
    Controller->AddTaskLocalityHint(this, address);
}

void TOperationControllerBase::TTask::AddSequentialInputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    TNodeDirectoryBuilder directoryBuilder(Controller->NodeDirectory, schedulerJobSpecExt->mutable_node_directory());
    auto* inputSpec = schedulerJobSpecExt->add_input_specs();
    auto list = joblet->InputStripeList;
    for (const auto& stripe : list->Stripes) {
        AddChunksToInputSpec(&directoryBuilder, inputSpec, stripe, list->PartitionTag);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::AddParallelInputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    TNodeDirectoryBuilder directoryBuilder(Controller->NodeDirectory, schedulerJobSpecExt->mutable_node_directory());
    auto list = joblet->InputStripeList;
    for (const auto& stripe : list->Stripes) {
        auto* inputSpec = schedulerJobSpecExt->add_input_specs();
        AddChunksToInputSpec(&directoryBuilder, inputSpec, stripe, list->PartitionTag);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::AddChunksToInputSpec(
    TNodeDirectoryBuilder* directoryBuilder,
    TTableInputSpec* inputSpec,
    TChunkStripePtr stripe,
    TNullable<int> partitionTag)
{
    for (const auto& chunkSlice : stripe->ChunkSlices) {
        auto* chunkSpec = inputSpec->add_chunks();
        ToProto(chunkSpec, chunkSlice);
        for (ui32 protoReplica : chunkSlice->GetChunkSpec()->replicas()) {
            auto replica = FromProto<TChunkReplica>(protoReplica);
            directoryBuilder->Add(replica);
        }
        if (partitionTag) {
            chunkSpec->set_partition_tag(partitionTag.Get());
        }
    }
}

void TOperationControllerBase::TTask::UpdateInputSpecTotals(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto list = joblet->InputStripeList;
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    schedulerJobSpecExt->set_input_uncompressed_data_size(
        schedulerJobSpecExt->input_uncompressed_data_size() +
        list->TotalDataSize);
    schedulerJobSpecExt->set_input_row_count(
        schedulerJobSpecExt->input_row_count() +
        list->TotalRowCount);
}

void TOperationControllerBase::TTask::AddFinalOutputSpecs(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    YCHECK(joblet->ChunkListIds.size() == Controller->OutputTables.size());
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    for (int index = 0; index < static_cast<int>(Controller->OutputTables.size()); ++index) {
        const auto& table = Controller->OutputTables[index];
        auto* outputSpec = schedulerJobSpecExt->add_output_specs();
        outputSpec->set_table_writer_options(ConvertToYsonString(table.Options).Data());
        if (!table.KeyColumns.empty()) {
            ToProto(outputSpec->mutable_key_columns(), table.KeyColumns);
        }
        ToProto(outputSpec->mutable_chunk_list_id(), joblet->ChunkListIds[index]);
    }
}

void TOperationControllerBase::TTask::AddIntermediateOutputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet,
    TKeyColumns keyColumns)
{
    YCHECK(joblet->ChunkListIds.size() == 1);
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    auto* outputSpec = schedulerJobSpecExt->add_output_specs();
    auto options = New<TTableWriterOptions>();
    options->Account = Controller->Spec->IntermediateDataAccount;
    options->ChunksVital = false;
    options->ChunksMovable = false;
    options->ReplicationFactor = 1;
    options->CompressionCodec = Controller->Spec->IntermediateCompressionCodec;
    outputSpec->set_table_writer_options(ConvertToYsonString(options).Data());

    if (!keyColumns.empty()) {
        ToProto(outputSpec->mutable_key_columns(), keyColumns);
    }
    ToProto(outputSpec->mutable_chunk_list_id(), joblet->ChunkListIds[0]);
}

void TOperationControllerBase::TTask::ResetCachedMinNeededResources()
{
    CachedMinNeededResources.Reset();
}

const TNodeResources& TOperationControllerBase::TTask::GetMinNeededResources() const
{
    if (!CachedMinNeededResources) {
        YCHECK(GetPendingJobCount() > 0);
        CachedMinNeededResources = GetMinNeededResourcesHeavy();
    }
    return *CachedMinNeededResources;
}

TNodeResources TOperationControllerBase::TTask::GetNeededResources(TJobletPtr /* joblet */) const
{
    return GetMinNeededResources();
}

void TOperationControllerBase::TTask::RegisterIntermediate(
    TJobletPtr joblet,
    TChunkStripePtr stripe,
    TTaskPtr destinationTask)
{
    RegisterIntermediate(joblet, stripe, destinationTask->GetChunkPoolInput());

    if (destinationTask->HasInputLocality()) {
        Controller->AddTaskLocalityHint(destinationTask, stripe);
    }
    destinationTask->AddPendingHint();
}

void TOperationControllerBase::TTask::RegisterIntermediate(
    TJobletPtr joblet,
    TChunkStripePtr stripe,
    IChunkPoolInput* destinationPool)
{
    IChunkPoolInput::TCookie inputCookie;

    auto lostIt = LostJobCookieMap.find(joblet->OutputCookie);
    if (lostIt == LostJobCookieMap.end()) {
        inputCookie = destinationPool->Add(stripe);
    } else {
        inputCookie = lostIt->second;
        destinationPool->Resume(inputCookie, stripe);
        LostJobCookieMap.erase(lostIt);
    }

    // Store recovery info.
    auto completedJob = New<TCompletedJob>(
        joblet->JobId,
        this,
        joblet->OutputCookie,
        destinationPool,
        inputCookie,
        joblet->Address);

    Controller->RegisterIntermediate(
        joblet,
        completedJob,
        stripe);
}

TChunkStripePtr TOperationControllerBase::TTask::BuildIntermediateChunkStripe(
    google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>* chunkSpecs)
{
    auto stripe = New<TChunkStripe>();
    for (auto& chunkSpec : *chunkSpecs) {
        auto chunkSlice = CreateChunkSlice(New<TRefCountedChunkSpec>(std::move(chunkSpec)));
        stripe->ChunkSlices.push_back(chunkSlice);
    }
    return stripe;
}

void TOperationControllerBase::TTask::RegisterOutput(
    TJobletPtr joblet,
    int key,
    const TCompletedJobSummary& jobSummary)
{
    Controller->RegisterOutput(joblet, key, jobSummary);
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TOperationControllerBase(
    TSchedulerConfigPtr config,
    TOperationSpecBasePtr spec,
    IOperationHost* host,
    TOperation* operation)
    : Config(config)
    , Host(host)
    , Operation(operation)
    , OperationId(Operation->GetId())
    , AuthenticatedMasterClient(CreateClient())
    , AuthenticatedInputMasterClient(AuthenticatedMasterClient)
    , AuthenticatedOutputMasterClient(AuthenticatedMasterClient)
    , Logger(OperationLogger)
    , CancelableContext(New<TCancelableContext>())
    , CancelableControlInvoker(CancelableContext->CreateInvoker(Host->GetControlInvoker()))
    , CancelableBackgroundInvoker(CancelableContext->CreateInvoker(Host->GetBackgroundInvoker()))
    , Prepared(false)
    , Running(false)
    , TotalEstimatedInputChunkCount(0)
    , TotalEstimatedInputDataSize(0)
    , TotalEstimatedInputRowCount(0)
    , TotalEstimatedInputValueCount(0)
    , TotalEstimatedCompressedDataSize(0)
    , ChunkLocatedCallCount(0)
    , UnavailableInputChunkCount(0)
    , JobCounter(0)
    , AsyncSchedulerTransactionId(NullTransactionId)
    , SyncSchedulerTransactionId(NullTransactionId)
    , InputTransactionId(NullTransactionId)
    , OutputTransactionId(NullTransactionId)
    , Spec(spec)
    , CachedPendingJobCount(0)
    , CachedNeededResources(ZeroNodeResources())
{
    Logger.AddTag("OperationId: %v", operation->GetId());
}

void TOperationControllerBase::Initialize()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Initializing operation (Title: %v)",
        Spec->Title ? ~(*Spec->Title) : "<Null>");

    NodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();

    for (const auto& path : GetInputTablePaths()) {
        TInputTable table;
        table.Path = path;
        InputTables.push_back(table);
    }

    for (const auto& path : GetOutputTablePaths()) {
        TOutputTable table;
        table.Path = path;

        if (path.GetAppend()) {
            table.AppendRequested = true;
            table.UpdateMode = EUpdateMode::Append;
            table.LockMode = ELockMode::Shared;
        }

        table.KeyColumns = path.Attributes().Get<TKeyColumns>("sorted_by", TKeyColumns());
        if (!table.KeyColumns.empty()) {
            if (!IsSortedOutputSupported()) {
                THROW_ERROR_EXCEPTION("Sorted outputs are not supported");
            }
            table.UpdateMode = EUpdateMode::Overwrite;
            table.LockMode = ELockMode::Exclusive;
        }

        OutputTables.push_back(table);
    }

    if (InputTables.size() > Config->MaxInputTableCount) {
        THROW_ERROR_EXCEPTION(
            "Too many input tables: maximum allowed %v, actual %v",
            Config->MaxInputTableCount,
            InputTables.size());
    }

    DoInitialize();

    LOG_INFO("Operation initialized");
}

void TOperationControllerBase::Essentiate()
{
    Operation->SetMaxStderrCount(Spec->MaxStderrCount.Get(Config->MaxStderrCount));
    Operation->SetSchedulingTag(Spec->SchedulingTag);

    InitializeTransactions();
}

void TOperationControllerBase::DoInitialize()
{ }

TFuture<void> TOperationControllerBase::Prepare()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    return BIND(&TThis::DoPrepare, MakeStrong(this))
        .AsyncVia(CancelableBackgroundInvoker)
        .Run()
        .Apply(BIND([=, this_ = MakeStrong(this)] () {
            Prepared = true;
            Running = true;
        }).AsyncVia(CancelableControlInvoker));
}

void TOperationControllerBase::DoPrepare()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    InitChunkListPool();

    GetInputObjectIds();
    GetOutputObjectIds();

    ValidateFileTypes();

    RequestInputObjects();
    RequestOutputObjects();
    RequestFileObjects();

    CreateLivePreviewTables();

    PrepareLivePreviewTablesForUpdate();

    CollectTotals();

    CustomPrepare();

    if (InputChunkMap.empty()) {
        // Possible reasons:
        // - All input chunks are unavailable && Strategy == Skip
        // - Merge decided to passthrough all input chunks
        // - Anything else?
        LOG_INFO("No jobs needed");
        OnOperationCompleted();
        return;
    }

    SuspendUnavailableInputStripes();

    AddAllTaskPendingHints();

    // Input chunk scraper initialization should be the last step to avoid races,
    // because input chunk scraper works in control thread.
    InitInputChunkScraper();
}

void TOperationControllerBase::SaveSnapshot(TOutputStream* output)
{
    DoSaveSnapshot(output);
}

void TOperationControllerBase::DoSaveSnapshot(TOutputStream* output)
{
    TSaveContext context;
    context.SetOutput(output);

    Save(context, this);
}

TFuture<void> TOperationControllerBase::Revive()
{
    return BIND(&TOperationControllerBase::DoRevive, MakeStrong(this))
        .AsyncVia(CancelableBackgroundInvoker)
        .Run()
        .Apply(BIND([=, this_ = MakeStrong(this)] () {
            ReinstallLivePreview();
            Prepared = true;
            Running = true;
        }).AsyncVia(CancelableControlInvoker));
}

void TOperationControllerBase::DoRevive()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    InitChunkListPool();

    DoLoadSnapshot();

    PrepareLivePreviewTablesForUpdate();

    AbortAllJoblets();

    AddAllTaskPendingHints();

    // Input chunk scraper initialization should be the last step to avoid races.
    InitInputChunkScraper();
}

void TOperationControllerBase::InitializeTransactions()
{
    StartAsyncSchedulerTransaction();
    if (Operation->GetCleanStart()) {
        StartSyncSchedulerTransaction();
        StartInputTransaction(SyncSchedulerTransactionId);
        StartOutputTransaction(SyncSchedulerTransactionId);
    } else {
        InputTransactionId = Operation->GetInputTransaction()->GetId();
        OutputTransactionId = Operation->GetOutputTransaction()->GetId();
    }
    Operation->SetHasActiveTransactions(true);
}

TTransactionId TOperationControllerBase::StartTransaction(
    const Stroka& transactionName,
    IClientPtr client,
    const TNullable<TTransactionId>& parentTransactionId = Null)
{
    LOG_INFO("Starting %v transaction", transactionName);

    auto channel = client->GetMasterChannel(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    {
        auto req = TMasterYPathProxy::CreateObjects();
        if (parentTransactionId) {
            ToProto(req->mutable_transaction_id(), parentTransactionId.Get());
        }
        req->set_type(static_cast<int>(EObjectType::Transaction));

        auto* reqExt = req->MutableExtension(
            NTransactionClient::NProto::TReqStartTransactionExt::create_transaction_ext);
        reqExt->set_timeout(Config->OperationTransactionTimeout.MilliSeconds());

        auto attributes = CreateEphemeralAttributes();
        attributes->Set("title", Format("Scheduler %v for operation %v", transactionName, OperationId));
        attributes->Set("operation_id", OperationId);
        ToProto(req->mutable_object_attributes(), *attributes);

        GenerateMutationId(req);
        batchReq->AddRequest(req, Format("start_%v_tx", transactionName));
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        GetCumulativeError(batchRspOrError),
        "Error starting %v transaction",
        transactionName);

    if (Operation->GetState() != EOperationState::Initializing &&
        Operation->GetState() != EOperationState::Reviving)
        throw TFiberCanceledException();

    const auto& batchRsp = batchRspOrError.Value();
    auto rspOrError = batchRsp->GetResponse<TMasterYPathProxy::TRspCreateObjects>(
        Format("start_%v_tx", transactionName));
    THROW_ERROR_EXCEPTION_IF_FAILED(
        rspOrError,
        "Error starting %v transaction",
        transactionName);

    const auto& rsp = rspOrError.Value();
    return FromProto<TTransactionId>(rsp->object_ids(0));
}

void TOperationControllerBase::StartSyncSchedulerTransaction()
{
    TNullable<TTransactionId> userTransactionId;
    if (Operation->GetUserTransaction()) {
        userTransactionId = Operation->GetUserTransaction()->GetId();
    }
    SyncSchedulerTransactionId =
        StartTransaction("sync", AuthenticatedMasterClient, userTransactionId);
    auto transactionManager = Host->GetMasterClient()->GetTransactionManager();
    Operation->SetSyncSchedulerTransaction(transactionManager->Attach(SyncSchedulerTransactionId));

    LOG_INFO("Scheduler sync transaction started (SyncTransactionId: %v)",
        SyncSchedulerTransactionId);
}

void TOperationControllerBase::StartAsyncSchedulerTransaction()
{
    AsyncSchedulerTransactionId = StartTransaction("async", AuthenticatedMasterClient);
    auto transactionManager = AuthenticatedMasterClient->GetTransactionManager();
    Operation->SetAsyncSchedulerTransaction(transactionManager->Attach(AsyncSchedulerTransactionId));

    LOG_INFO("Scheduler async transaction started (AsyncTranasctionId: %v)",
        AsyncSchedulerTransactionId);
}

void TOperationControllerBase::StartInputTransaction(TTransactionId parentTransactionId)
{
    InputTransactionId =
        StartTransaction("input", AuthenticatedInputMasterClient, parentTransactionId);
    auto transactionManager = AuthenticatedInputMasterClient->GetTransactionManager();
    Operation->SetInputTransaction(transactionManager->Attach(InputTransactionId));

    LOG_INFO("Input transaction started (InputTransactionId: %v)",
        InputTransactionId);
}

void TOperationControllerBase::StartOutputTransaction(TTransactionId parentTransactionId)
{
    OutputTransactionId =
        StartTransaction("output", AuthenticatedOutputMasterClient, parentTransactionId);
    auto transactionManager = AuthenticatedOutputMasterClient->GetTransactionManager();
    Operation->SetOutputTransaction(transactionManager->Attach(OutputTransactionId));

    LOG_INFO("Output transaction started (OutputTransactionId: %v)",
        OutputTransactionId);
}

void TOperationControllerBase::InitChunkListPool()
{
    ChunkListPool = New<TChunkListPool>(
        Config,
        Host->GetMasterClient()->GetMasterChannel(EMasterChannelKind::Leader),
        CancelableControlInvoker,
        Operation->GetId(),
        OutputTransactionId);
}

void TOperationControllerBase::InitInputChunkScraper()
{
    yhash_set<TChunkId> chunkIds;
    for (const auto& pair : InputChunkMap) {
        chunkIds.insert(pair.first);
    }

    YCHECK(!InputChunkScraper);
    InputChunkScraper = New<TChunkScraper>(
        Config,
        CancelableBackgroundInvoker,
        Host->GetChunkLocationThrottler(),
        AuthenticatedInputMasterClient->GetMasterChannel(EMasterChannelKind::Leader),
        NodeDirectory,
        std::move(chunkIds),
        BIND(&TThis::OnInputChunkLocated, MakeWeak(this))
            .Via(CancelableControlInvoker),
        Logger
    );

    if (UnavailableInputChunkCount > 0) {
        LOG_INFO("Waiting for %v unavailable input chunks", UnavailableInputChunkCount);
        InputChunkScraper->Start();
    }
}

void TOperationControllerBase::SuspendUnavailableInputStripes()
{
    YCHECK(UnavailableInputChunkCount == 0);

    for (const auto& pair : InputChunkMap) {
        const auto& chunkDescriptor = pair.second;
        if (chunkDescriptor.State == EInputChunkState::Waiting) {
            LOG_TRACE("Input chunk is unavailable (ChunkId: %v)", pair.first);
            for (const auto& inputStripe : chunkDescriptor.InputStripes) {
                if (inputStripe.Stripe->WaitingChunkCount == 0) {
                    inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);
                }
                ++inputStripe.Stripe->WaitingChunkCount;
            }
            ++UnavailableInputChunkCount;
        }
    }
}

void TOperationControllerBase::ReinstallLivePreview()
{
    auto masterConnector = Host->GetMasterConnector();

    if (IsOutputLivePreviewSupported()) {
        for (const auto& table : OutputTables) {
            std::vector<TChunkTreeId> childrenIds;
            childrenIds.reserve(table.OutputChunkTreeIds.size());
            for (const auto& pair : table.OutputChunkTreeIds) {
                childrenIds.push_back(pair.second);
            }
            masterConnector->AttachToLivePreview(
                Operation,
                table.LivePreviewChunkListId,
                childrenIds);
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        std::vector<TChunkTreeId> childrenIds;
        childrenIds.reserve(ChunkOriginMap.size());
        for (const auto& pair : ChunkOriginMap) {
            if (!pair.second->IsLost) {
                childrenIds.push_back(pair.first);
            }
        }
        masterConnector->AttachToLivePreview(
            Operation,
            IntermediateTable.LivePreviewChunkListId,
            childrenIds);
    }
}

void TOperationControllerBase::AbortAllJoblets()
{
    for (const auto& pair : JobletMap) {
        auto joblet = pair.second;
        JobCounter.Aborted(1, EAbortReason::Scheduler);
        joblet->Task->OnJobAborted(joblet, TAbortedJobSummary(pair.first, EAbortReason::Scheduler));
    }
    JobletMap.clear();
}

void TOperationControllerBase::DoLoadSnapshot()
{
    LOG_INFO("Started loading snapshot");

    auto snapshot = Operation->Snapshot();
    TMemoryInput input(snapshot.Begin(), snapshot.Size());

    TLoadContext context;
    context.SetInput(&input);

    NPhoenix::TSerializer::InplaceLoad(context, this);

    LOG_INFO("Finished loading snapshot");
}

TFuture<void> TOperationControllerBase::Commit()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    return BIND(&TThis::DoCommit, MakeStrong(this))
        .AsyncVia(CancelableBackgroundInvoker)
        .Run();
}

void TOperationControllerBase::DoCommit()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    CommitResults();
}

void TOperationControllerBase::CommitResults()
{
    LOG_INFO("Committing results");

    auto channel = AuthenticatedOutputMasterClient->GetMasterChannel(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (auto& table : OutputTables) {
        auto path = FromObjectId(table.ObjectId);
        // Split large outputs into separate requests.
        {
            TChunkListYPathProxy::TReqAttachPtr req;
            int reqSize = 0;
            auto flushReq = [&] () {
                if (req) {
                    batchReq->AddRequest(req, "attach_out");
                    reqSize = 0;
                    req.Reset();
                }
            };

            auto addChunkTree = [&] (const TChunkTreeId& chunkTreeId) {
                if (!req) {
                    req = TChunkListYPathProxy::Attach(FromObjectId(table.OutputChunkListId));
                    GenerateMutationId(req);
                }
                ToProto(req->add_children_ids(), chunkTreeId);
                ++reqSize;
                if (reqSize >= Config->MaxChildrenPerAttachRequest) {
                    flushReq();
                }
            };

            if (!table.KeyColumns.empty() && IsSortedOutputSupported()) {
                // Sorted output generated by user operation requires rearranging.
                LOG_DEBUG("Sorting %v boundary key pairs", table.BoundaryKeys.size());
                std::sort(
                    table.BoundaryKeys.begin(),
                    table.BoundaryKeys.end(),
                    [=] (const TJobBoundaryKeys& lhs, const TJobBoundaryKeys& rhs) -> bool {
                        auto keysResult = CompareRows(lhs.MinKey, rhs.MinKey);
                        if (keysResult != 0) {
                            return keysResult < 0;
                        }
                        return lhs.MaxKey < rhs.MaxKey;
                });

                for (auto current = table.BoundaryKeys.begin(); current != table.BoundaryKeys.end(); ++current) {
                    auto next = current + 1;
                    if (next != table.BoundaryKeys.end() && next->MinKey < current->MaxKey) {
                        auto error = TError("Output table %v is not sorted: job outputs have overlapping key ranges [MinKey %v, MaxKey: %v]",
                            table.Path.GetPath(),
                            next->MinKey,
                            current->MaxKey);

                        LOG_DEBUG(error);
                        THROW_ERROR error;
                    }

                    auto pair = table.OutputChunkTreeIds.equal_range(current->ChunkTreeKey);
                    auto it = pair.first;
                    if (it != pair.second) {
                        // Chunk tree may be absent if no data was written in the job.
                        addChunkTree(it->second);
                        // In user operations each ChunkTreeKey corresponds to a single OutputChunkTreeId.
                        // Let's check it.
                        YCHECK(++it == pair.second);
                    }
                }
            } else {
                for (const auto& pair : table.OutputChunkTreeIds) {
                    addChunkTree(pair.second);
                }
            }

            flushReq();
        }

        if (!table.KeyColumns.empty()) {
            LOG_INFO("Table %v will be marked as sorted by [%v]",
                table.Path.GetPath(),
                JoinToString(table.KeyColumns));
            auto req = TTableYPathProxy::SetSorted(path);
            ToProto(req->mutable_key_columns(), table.KeyColumns);
            SetTransactionId(req, OutputTransactionId);
            GenerateMutationId(req);
            batchReq->AddRequest(req, "set_out_sorted");
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error committing results");

    LOG_INFO("Results committed");
}

void TOperationControllerBase::OnJobRunning(const TJobId& /* jobId */, const TJobStatus& /* status */)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
}

void TOperationControllerBase::OnJobStarted(const TJobId& jobId)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto joblet = GetJoblet(jobId);
    auto address = joblet->Address;
    LogEventFluently(ELogEventType::JobStarted)
        .Item("job_id").Value(joblet->JobId)
        .Item("resource_limits").Value(joblet->ResourceLimits)
        .Item("node_address").Value(address)
        .Item("job_type").Value(joblet->JobType)
        .Item("total_data_size").Value(joblet->InputStripeList->TotalDataSize)
        .Item("local_data_size").Value(joblet->InputStripeList->LocalDataSize)
        .Item("scheduling_locality").Value(joblet->Task->GetLocality(address));

    JobCounter.Start(1);
}

void TOperationControllerBase::OnJobCompleted(const TCompletedJobSummary& jobSummary)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    const auto& jobId = jobSummary.Id;
    const auto& result = jobSummary.Result;

    JobCounter.Completed(1);
    UpdateJobStatistics(jobSummary);

    const auto& schedulerResultEx = result->GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    // Populate node directory by adding additional nodes returned from the job.
    NodeDirectory->MergeFrom(schedulerResultEx.node_directory());

    auto joblet = GetJoblet(jobId);
    joblet->Task->OnJobCompleted(joblet, jobSummary);

    RemoveJoblet(jobId);

    UpdateTask(joblet->Task);

    if (IsCompleted()) {
        OnOperationCompleted();
    }
}

void TOperationControllerBase::OnJobFailed(const TFailedJobSummary& jobSummary)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    const auto& jobId = jobSummary.Id;
    const auto& result = jobSummary.Result;

    auto error = FromProto<TError>(result->error());

    JobCounter.Failed(1);
    UpdateJobStatistics(jobSummary);

    auto joblet = GetJoblet(jobId);
    joblet->Task->OnJobFailed(joblet, jobSummary);

    RemoveJoblet(jobId);

    if (error.Attributes().Get<bool>("fatal", false)) {
        OnOperationFailed(error);
        return;
    }

    int failedJobCount = JobCounter.GetFailed();
    int maxFailedJobCount = Spec->MaxFailedJobCount.Get(Config->MaxFailedJobCount);
    if (failedJobCount >= maxFailedJobCount) {
        OnOperationFailed(TError("Failed jobs limit exceeded")
            << TErrorAttribute("max_failed_job_count", maxFailedJobCount));
        return;
    }
}

void TOperationControllerBase::OnJobAborted(const TAbortedJobSummary& jobSummary)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    const auto& jobId = jobSummary.Id;
    auto abortReason = jobSummary.AbortReason;

    JobCounter.Aborted(1, abortReason);
    UpdateJobStatistics(jobSummary);

    auto joblet = GetJoblet(jobId);
    joblet->Task->OnJobAborted(joblet, jobSummary);


    RemoveJoblet(jobId);

    if (abortReason == EAbortReason::FailedChunks) {
        const auto& result = jobSummary.Result;
        const auto& schedulerResultExt = result->GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
        for (const auto& chunkId : schedulerResultExt.failed_chunk_ids()) {
            OnChunkFailed(FromProto<TChunkId>(chunkId));
        }
    }
}

void TOperationControllerBase::OnChunkFailed(const TChunkId& chunkId)
{
    auto it = InputChunkMap.find(chunkId);
    if (it == InputChunkMap.end()) {
        LOG_WARNING("Intermediate chunk %v has failed", chunkId);
        OnIntermediateChunkUnavailable(chunkId);
    } else {
        LOG_WARNING("Input chunk %v has failed", chunkId);
        OnInputChunkUnavailable(chunkId, it->second);
    }
}

void TOperationControllerBase::OnInputChunkLocated(const TChunkId& chunkId, const TChunkReplicaList& replicas)
{
    auto it = InputChunkMap.find(chunkId);
    YCHECK(it != InputChunkMap.end());

    auto& descriptor = it->second;
    YCHECK(!descriptor.ChunkSpecs.empty());
    auto& chunkSpec = descriptor.ChunkSpecs.front();
    auto codecId = NErasure::ECodec(chunkSpec->erasure_codec());

    if (IsUnavailable(replicas, codecId, NeedsAllChunkParts())) {
        OnInputChunkUnavailable(chunkId, descriptor);
    } else {
        OnInputChunkAvailable(chunkId, descriptor, replicas);
    }
}

void TOperationControllerBase::OnInputChunkAvailable(const TChunkId& chunkId, TInputChunkDescriptor& descriptor, const TChunkReplicaList& replicas)
{
    if (descriptor.State != EInputChunkState::Waiting)
        return;

    LOG_TRACE("Input chunk is available (ChunkId: %v)", chunkId);

    --UnavailableInputChunkCount;
    YCHECK(UnavailableInputChunkCount >= 0);

    if (UnavailableInputChunkCount == 0) {
        InputChunkScraper->Stop();
    }

    // Update replicas in place for all input chunks with current chunkId.
    for (auto& chunkSpec : descriptor.ChunkSpecs) {
        chunkSpec->mutable_replicas()->Clear();
        ToProto(chunkSpec->mutable_replicas(), replicas);
    }

    descriptor.State = EInputChunkState::Active;

    for (const auto& inputStripe : descriptor.InputStripes) {
        --inputStripe.Stripe->WaitingChunkCount;
        if (inputStripe.Stripe->WaitingChunkCount > 0)
            continue;

        auto task = inputStripe.Task;
        task->GetChunkPoolInput()->Resume(inputStripe.Cookie, inputStripe.Stripe);
        if (task->HasInputLocality()) {
            AddTaskLocalityHint(task, inputStripe.Stripe);
        }
        AddTaskPendingHint(task);
    }
}

void TOperationControllerBase::OnInputChunkUnavailable(const TChunkId& chunkId, TInputChunkDescriptor& descriptor)
{
    if (descriptor.State != EInputChunkState::Active)
        return;

    ++ChunkLocatedCallCount;
    if (ChunkLocatedCallCount >= Config->MaxChunksPerScratch) {
        ChunkLocatedCallCount = 0;
        LOG_DEBUG("Located another batch of chunks (Count: %v, UnavailableInputChunkCount: %v)",
            Config->MaxChunksPerScratch,
            UnavailableInputChunkCount);
    }

    LOG_TRACE("Input chunk is unavailable (ChunkId: %v)", chunkId);

    ++UnavailableInputChunkCount;

    switch (Spec->UnavailableChunkTactics) {
        case EUnavailableChunkAction::Fail:
            OnOperationFailed(TError("Input chunk %v is unavailable",
                chunkId));
            break;

        case EUnavailableChunkAction::Skip: {
            descriptor.State = EInputChunkState::Skipped;
            for (const auto& inputStripe : descriptor.InputStripes) {
                inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);

                // Remove given chunk from the stripe list.
                SmallVector<TChunkSlicePtr, 1> slices;
                std::swap(inputStripe.Stripe->ChunkSlices, slices);

                std::copy_if(
                    slices.begin(),
                    slices.end(),
                    inputStripe.Stripe->ChunkSlices.begin(),
                    [&] (TChunkSlicePtr slice) {
                        return chunkId != FromProto<TChunkId>(slice->GetChunkSpec()->chunk_id());
                    });

                // Reinstall patched stripe.
                inputStripe.Task->GetChunkPoolInput()->Resume(inputStripe.Cookie, inputStripe.Stripe);
                AddTaskPendingHint(inputStripe.Task);
            }
            InputChunkScraper->Start();
            break;
        }

        case EUnavailableChunkAction::Wait: {
            descriptor.State = EInputChunkState::Waiting;
            for (const auto& inputStripe : descriptor.InputStripes) {
                if (inputStripe.Stripe->WaitingChunkCount == 0) {
                    inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);
                }
                ++inputStripe.Stripe->WaitingChunkCount;
            }
            InputChunkScraper->Start();
            break;
        }

        default:
            YUNREACHABLE();
    }
}

void TOperationControllerBase::OnIntermediateChunkUnavailable(const TChunkId& chunkId)
{
    auto it = ChunkOriginMap.find(chunkId);
    YCHECK(it != ChunkOriginMap.end());
    auto completedJob = it->second;
    if (completedJob->IsLost)
        return;

    LOG_INFO("Job is lost (Address: %v, JobId: %v, SourceTask: %v, OutputCookie: %v, InputCookie: %v)",
        completedJob->Address,
        completedJob->JobId,
        completedJob->SourceTask->GetId(),
        completedJob->OutputCookie,
        completedJob->InputCookie);

    JobCounter.Lost(1);
    completedJob->IsLost = true;
    completedJob->DestinationPool->Suspend(completedJob->InputCookie);
    completedJob->SourceTask->GetChunkPoolOutput()->Lost(completedJob->OutputCookie);
    completedJob->SourceTask->OnJobLost(completedJob);
    AddTaskPendingHint(completedJob->SourceTask);
}

bool TOperationControllerBase::IsOutputLivePreviewSupported() const
{
    return false;
}

bool TOperationControllerBase::IsIntermediateLivePreviewSupported() const
{
    return false;
}

void TOperationControllerBase::Abort()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Aborting operation");

    Running = false;

    CancelableContext->Cancel();

    LOG_INFO("Operation aborted");
}

void TOperationControllerBase::CheckTimeLimit()
{
    auto timeLimit = Config->OperationTimeLimit;
    if (Spec->TimeLimit) {
        timeLimit = Spec->TimeLimit;
    }

    if (timeLimit) {
        if (TInstant::Now() - Operation->GetStartTime() > timeLimit.Get()) {
            OnOperationFailed(TError("Operation is running for too long, aborted")
                << TErrorAttribute("time_limit", timeLimit.Get()));
        }
    }
}

TJobId TOperationControllerBase::ScheduleJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (!Running ||
        Operation->GetState() != EOperationState::Running ||
        Operation->GetSuspended())
    {
        LOG_TRACE("Operation is not running, scheduling request ignored");
        return NullJobId;
    }

    if (GetPendingJobCount() == 0) {
        LOG_TRACE("No pending jobs left, scheduling request ignored");
        return NullJobId;
    }

    auto jobId = DoScheduleJob(context, jobLimits);
    if (!jobId) {
        return NullJobId;
    }

    OnJobStarted(jobId);

    return jobId;
}

void TOperationControllerBase::CustomizeJoblet(TJobletPtr /* joblet */)
{ }

void TOperationControllerBase::CustomizeJobSpec(TJobletPtr /* joblet */, TJobSpec* /* jobSpec */)
{ }

void TOperationControllerBase::RegisterTask(TTaskPtr task)
{
    Tasks.push_back(std::move(task));
}

void TOperationControllerBase::RegisterTaskGroup(TTaskGroupPtr group)
{
    TaskGroups.push_back(std::move(group));
}

void TOperationControllerBase::UpdateTask(TTaskPtr task)
{
    int oldPendingJobCount = CachedPendingJobCount;
    int newPendingJobCount = CachedPendingJobCount + task->GetPendingJobCountDelta();
    CachedPendingJobCount = newPendingJobCount;

    int oldTotalJobCount = JobCounter.GetTotal();
    JobCounter.Increment(task->GetTotalJobCountDelta());
    int newTotalJobCount = JobCounter.GetTotal();

    CachedNeededResources += task->GetTotalNeededResourcesDelta();

    LOG_DEBUG_IF(
        newPendingJobCount != oldPendingJobCount || newTotalJobCount != oldTotalJobCount,
        "Task updated (Task: %v, PendingJobCount: %v -> %v, TotalJobCount: %v -> %v, NeededResources: {%v})",
        task->GetId(),
        oldPendingJobCount,
        newPendingJobCount,
        oldTotalJobCount,
        newTotalJobCount,
        FormatResources(CachedNeededResources));

    i64 outputTablesTimesJobsCount = OutputTables.size() * newTotalJobCount;
    if (outputTablesTimesJobsCount > Config->MaxOutputTablesTimesJobsCount) {
        OnOperationFailed(TError(
                "Maximum allowed number of output tables times job count violated: %v > %v",
                outputTablesTimesJobsCount,
                Config->MaxOutputTablesTimesJobsCount)
            << TErrorAttribute("output_table_count", OutputTables.size())
            << TErrorAttribute("job_count", newTotalJobCount));
    }

    task->CheckCompleted();
}

void TOperationControllerBase::UpdateAllTasks()
{
    for (auto& task: Tasks) {
        task->ResetCachedMinNeededResources();
        UpdateTask(task);
    }
}

void TOperationControllerBase::MoveTaskToCandidates(
    TTaskPtr task,
    std::multimap<i64, TTaskPtr>& candidateTasks)
{
    const auto& neededResources = task->GetMinNeededResources();
    task->CheckResourceDemandSanity(neededResources);
    i64 minMemory = neededResources.memory();
    candidateTasks.insert(std::make_pair(minMemory, task));
    LOG_DEBUG("Task moved to candidates (Task: %v, MinMemory: %v)",
        task->GetId(),
        minMemory / (1024 * 1024));

}

void TOperationControllerBase::AddTaskPendingHint(TTaskPtr task)
{
    if (task->GetPendingJobCount() > 0) {
        auto group = task->GetGroup();
        if (group->NonLocalTasks.insert(task).second) {
            LOG_DEBUG("Task pending hint added (Task: %v)", task->GetId());
            MoveTaskToCandidates(task, group->CandidateTasks);
        }
    }
    UpdateTask(task);
}

void TOperationControllerBase::AddAllTaskPendingHints()
{
    for (auto task : Tasks) {
        AddTaskPendingHint(task);
    }
}

void TOperationControllerBase::DoAddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    auto group = task->GetGroup();
    if (group->LocalTasks[address].insert(task).second) {
        LOG_TRACE("Task locality hint added (Task: %v, Address: %v)",
            task->GetId(),
            address);
    }
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    DoAddTaskLocalityHint(task, address);
    UpdateTask(task);
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, TChunkStripePtr stripe)
{
    for (const auto& chunkSlice : stripe->ChunkSlices) {
        for (ui32 protoReplica : chunkSlice->GetChunkSpec()->replicas()) {
            auto replica = FromProto<NChunkClient::TChunkReplica>(protoReplica);

            if (chunkSlice->GetLocality(replica.GetIndex()) > 0) {
                const auto& descriptor = NodeDirectory->GetDescriptor(replica);
                DoAddTaskLocalityHint(task, descriptor.GetDefaultAddress());
            }
        }
    }
    UpdateTask(task);
}

void TOperationControllerBase::ResetTaskLocalityDelays()
{
    LOG_DEBUG("Task locality delays are reset");
    for (auto group : TaskGroups) {
        for (const auto& pair : group->DelayedTasks) {
            auto task = pair.second;
            if (task->GetPendingJobCount() > 0) {
                MoveTaskToCandidates(task, group->CandidateTasks);
            }
        }
        group->DelayedTasks.clear();
    }
}

bool TOperationControllerBase::CheckJobLimits(
    TTaskPtr task,
    const TNodeResources& jobLimits,
    const TNodeResources& nodeResourceLimits)
{
    auto neededResources = task->GetMinNeededResources();
    if (Dominates(jobLimits, neededResources)) {
        return true;
    }
    task->CheckResourceDemandSanity(nodeResourceLimits, neededResources);
    return false;
}

TJobId TOperationControllerBase::DoScheduleJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    auto localJobId = DoScheduleLocalJob(context, jobLimits);
    if (localJobId) {
        return localJobId;
    }

    auto nonLocalJobId = DoScheduleNonLocalJob(context, jobLimits);
    if (nonLocalJobId) {
        return nonLocalJobId;
    }

    return NullJobId;
}

TJobId TOperationControllerBase::DoScheduleLocalJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    const auto& nodeResourceLimits = context->ResourceLimits();
    const auto& address = context->GetAddress();

    for (auto group : TaskGroups) {
        if (!Dominates(jobLimits, group->MinNeededResources)) {
            continue;
        }

        auto localTasksIt = group->LocalTasks.find(address);
        if (localTasksIt == group->LocalTasks.end()) {
            continue;
        }

        i64 bestLocality = 0;
        TTaskPtr bestTask = nullptr;

        auto& localTasks = localTasksIt->second;
        auto it = localTasks.begin();
        while (it != localTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            // Make sure that the task have positive locality.
            // Remove pending hint if not.
            i64 locality = task->GetLocality(address);
            if (locality <= 0) {
                localTasks.erase(jt);
                LOG_TRACE("Task locality hint removed (Task: %v, Address: %v)",
                    task->GetId(),
                    address);
                continue;
            }

            if (locality <= bestLocality) {
                continue;
            }

            if (task->GetPendingJobCount() == 0) {
                UpdateTask(task);
                continue;
            }

            if (!CheckJobLimits(task, jobLimits, nodeResourceLimits)) {
                continue;
            }

            bestLocality = locality;
            bestTask = task;
        }

        if (!Running) {
            return NullJobId;
        }

        if (bestTask) {
            LOG_DEBUG(
                "Attempting to schedule a local job (Task: %v, Address: %v, Locality: %v, JobLimits: {%v}, "
                "PendingDataSize: %v, PendingJobCount: %v)",
                bestTask->GetId(),
                address,
                bestLocality,
                FormatResources(jobLimits),
                bestTask->GetPendingDataSize(),
                bestTask->GetPendingJobCount());

            if (!HasEnoughChunkLists(bestTask->GetChunkListCountPerJob())) {
                LOG_DEBUG("Job chunk list demand is not met");
                return NullJobId;
            }

            auto jobId = bestTask->ScheduleJob(context, jobLimits);
            if (jobId) {
                UpdateTask(bestTask);
                return jobId;
            }
        }
    }
    return NullJobId;
}

TJobId TOperationControllerBase::DoScheduleNonLocalJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    auto now = context->GetNow();
    const auto& nodeResourceLimits = context->ResourceLimits();
    const auto& address = context->GetAddress();

    for (auto group : TaskGroups) {
        if (!Dominates(jobLimits, group->MinNeededResources)) {
            continue;
        }

        auto& nonLocalTasks = group->NonLocalTasks;
        auto& candidateTasks = group->CandidateTasks;
        auto& delayedTasks = group->DelayedTasks;

        // Move tasks from delayed to candidates.
        while (!delayedTasks.empty()) {
            auto it = delayedTasks.begin();
            auto deadline = it->first;
            if (now < deadline) {
                break;
            }
            auto task = it->second;
            delayedTasks.erase(it);
            if (task->GetPendingJobCount() == 0) {
                LOG_DEBUG("Task pending hint removed (Task: %v)",
                    task->GetId());
                YCHECK(nonLocalTasks.erase(task) == 1);
                UpdateTask(task);
            } else {
                LOG_DEBUG("Task delay deadline reached (Task: %v)", task->GetId());
                MoveTaskToCandidates(task, candidateTasks);
            }
        }

        // Consider candidates in the order of increasing memory demand.
        {
            int processedTaskCount = 0;
            auto it = candidateTasks.begin();
            while (it != candidateTasks.end()) {
                ++processedTaskCount;
                auto task = it->second;

                // Make sure that the task is ready to launch jobs.
                // Remove pending hint if not.
                if (task->GetPendingJobCount() == 0) {
                    LOG_DEBUG("Task pending hint removed (Task: %v)", task->GetId());
                    candidateTasks.erase(it++);
                    YCHECK(nonLocalTasks.erase(task) == 1);
                    UpdateTask(task);
                    continue;
                }

                // Check min memory demand for early exit.
                if (task->GetMinNeededResources().memory() > jobLimits.memory()) {
                    break;
                }

                if (!CheckJobLimits(task, jobLimits, nodeResourceLimits)) {
                    ++it;
                    continue;
                }

                if (!task->GetDelayedTime()) {
                    task->SetDelayedTime(now);
                }

                auto deadline = task->GetDelayedTime().Get() + task->GetLocalityTimeout();
                if (deadline > now) {
                    LOG_DEBUG("Task delayed (Task: %v, Deadline: %v)",
                        task->GetId(),
                        deadline);
                    delayedTasks.insert(std::make_pair(deadline, task));
                    candidateTasks.erase(it++);
                    continue;
                }

                if (!Running) {
                    return NullJobId;
                }

                LOG_DEBUG(
                    "Attempting to schedule a non-local job (Task: %v, Address: %v, JobLimits: {%v}, "
                    "PendingDataSize: %v, PendingJobCount: %v)",
                    task->GetId(),
                    address,
                    FormatResources(jobLimits),
                    task->GetPendingDataSize(),
                    task->GetPendingJobCount());

                if (!HasEnoughChunkLists(task->GetChunkListCountPerJob())) {
                    LOG_DEBUG("Job chunk list demand is not met");
                    return NullJobId;
                }

                auto jobId = task->ScheduleJob(context, jobLimits);
                if (jobId) {
                    UpdateTask(task);
                    LOG_DEBUG("Processed %v tasks", processedTaskCount);
                    return jobId;
                }

                // If task failed to schedule job, its min resources might have been updated.
                auto minMemory = task->GetMinNeededResources().memory();
                if (it->first == minMemory) {
                    ++it;
                } else {
                    it = candidateTasks.erase(it);
                    candidateTasks.insert(std::make_pair(minMemory, task));
                }
            }

            LOG_DEBUG("Processed %v tasks", processedTaskCount);
        }
    }
    return NullJobId;
}

TCancelableContextPtr TOperationControllerBase::GetCancelableContext() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableContext;
}

IInvokerPtr TOperationControllerBase::GetCancelableControlInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableControlInvoker;
}

IInvokerPtr TOperationControllerBase::GetCancelableBackgroundInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableBackgroundInvoker;
}

int TOperationControllerBase::GetPendingJobCount() const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    // Avoid accessing the state while not prepared.
    if (!Prepared) {
        return 0;
    }

    // NB: For suspended operations we still report proper pending job count
    // but zero demand.
    if (Operation->GetState() != EOperationState::Running) {
        return 0;
    }

    return CachedPendingJobCount;
}

int TOperationControllerBase::GetTotalJobCount() const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    // Avoid accessing the state while not prepared.
    if (!Prepared) {
        return 0;
    }

    return JobCounter.GetTotal();
}

TNodeResources TOperationControllerBase::GetNeededResources() const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (Operation->GetState() != EOperationState::Running) {
        return ZeroNodeResources();
    }

    return CachedNeededResources;
}

void TOperationControllerBase::OnOperationCompleted()
{
    VERIFY_THREAD_AFFINITY_ANY();

    CancelableControlInvoker->Invoke(BIND(&TThis::DoOperationCompleted, MakeStrong(this)));
}

void TOperationControllerBase::DoOperationCompleted()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Operation completed");

    Running = false;

    Host->OnOperationCompleted(Operation);
}

void TOperationControllerBase::OnOperationFailed(const TError& error)
{
    VERIFY_THREAD_AFFINITY_ANY();

    CancelableControlInvoker->Invoke(BIND(&TThis::DoOperationFailed, MakeStrong(this), error));
}

void TOperationControllerBase::DoOperationFailed(const TError& error)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    Running = false;

    Host->OnOperationFailed(Operation, error);
}

void TOperationControllerBase::CreateLivePreviewTables()
{
    // NB: use root credentials.
    auto channel = Host->GetMasterClient()->GetMasterChannel(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    auto addRequest = [&] (
            const Stroka& path,
            int replicationFactor,
            const Stroka& key,
            const TYsonString& acl)
    {
        {
            auto req = TCypressYPathProxy::Create(path);

            req->set_type(static_cast<int>(EObjectType::Table));
            req->set_ignore_existing(true);

            auto attributes = CreateEphemeralAttributes();
            attributes->Set("replication_factor", replicationFactor);

            ToProto(req->mutable_node_attributes(), *attributes);

            batchReq->AddRequest(req, key);
        }

        {
            auto req = TYPathProxy::Set(path + "/@acl");
            req->set_value(acl.Data());

            batchReq->AddRequest(req, key);
        }

        {
            auto req = TYPathProxy::Set(path + "/@inherit_acl");
            req->set_value(ConvertToYsonString(false).Data());

            batchReq->AddRequest(req, key);
        }
    };

    if (IsOutputLivePreviewSupported()) {
        LOG_INFO("Creating output tables for live preview");

        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            const auto& table = OutputTables[index];
            auto path = GetLivePreviewOutputPath(OperationId, index);
            addRequest(path, table.Options->ReplicationFactor, "create_output", OutputTables[index].EffectiveAcl);
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        LOG_INFO("Creating intermediate table for live preview");

        auto path = GetLivePreviewIntermediatePath(OperationId);
        addRequest(path, 1, "create_intermediate", ConvertToYsonString(Spec->IntermediateDataAcl));
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error creating live preview tables");
    const auto& batchRsp = batchRspOrError.Value();

    auto handleResponse = [&] (TLivePreviewTableBase& table, TCypressYPathProxy::TRspCreatePtr rsp) {
        table.LivePreviewTableId = FromProto<NCypressClient::TNodeId>(rsp->node_id());
    };

    if (IsOutputLivePreviewSupported()) {
        auto rspsOrError = batchRsp->GetResponses<TCypressYPathProxy::TRspCreate>("create_output");
        YCHECK(rspsOrError.size() == 3 * OutputTables.size());
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            handleResponse(OutputTables[index], rspsOrError[3 * index].Value());
        }

        LOG_INFO("Output live preview tables created");
    }

    if (IsIntermediateLivePreviewSupported()) {
        auto rspsOrError = batchRsp->GetResponses<TCypressYPathProxy::TRspCreate>("create_intermediate");
        handleResponse(IntermediateTable, rspsOrError[0].Value());

        LOG_INFO("Intermediate live preview table created");
    }
}

void TOperationControllerBase::PrepareLivePreviewTablesForUpdate()
{
    // NB: use root credentials.
    auto channel = Host->GetMasterClient()->GetMasterChannel(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    auto addRequest = [&] (const TLivePreviewTableBase& table, const Stroka& key) {
        auto req = TTableYPathProxy::PrepareForUpdate(FromObjectId(table.LivePreviewTableId));
        req->set_update_mode(static_cast<int>(EUpdateMode::Overwrite));
        req->set_lock_mode(static_cast<int>(ELockMode::Exclusive));
        SetTransactionId(req, AsyncSchedulerTransactionId);
        batchReq->AddRequest(req, key);
    };

    if (IsOutputLivePreviewSupported()) {
        LOG_INFO("Preparing live preview output tables for update");

        for (const auto& table : OutputTables) {
            addRequest(table, "prepare_output");
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        LOG_INFO("Preparing live preview intermediate table for update");

        addRequest(IntermediateTable, "prepare_intermediate");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error preparing live preview tables for update");
    const auto& batchRsp = batchRspOrError.Value();

    auto handleResponse = [&] (TLivePreviewTableBase& table, TTableYPathProxy::TRspPrepareForUpdatePtr rsp) {
        table.LivePreviewChunkListId = FromProto<NCypressClient::TNodeId>(rsp->chunk_list_id());
    };

    if (IsOutputLivePreviewSupported()) {
        auto rspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspPrepareForUpdate>("prepare_output");
        YCHECK(rspsOrError.size() == OutputTables.size());
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            handleResponse(OutputTables[index], rspsOrError[index].Value());
        }

        LOG_INFO("Output live preview tables prepared for update");
    }

    if (IsIntermediateLivePreviewSupported()) {
        auto rspOrError = batchRsp->GetResponse<TTableYPathProxy::TRspPrepareForUpdate>("prepare_intermediate");
        handleResponse(IntermediateTable, rspOrError.Value());

        LOG_INFO("Intermediate live preview table prepared for update");
    }
}

void TOperationControllerBase::GetInputObjectIds()
{
    LOG_INFO("Getting input object ids");

    auto channel = AuthenticatedInputMasterClient->GetMasterChannel(EMasterChannelKind::LeaderOrFollower);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& table : InputTables) {
        auto req = TObjectYPathProxy::GetBasicAttributes(table.Path.GetPath());
        SetTransactionId(req, InputTransactionId);
        batchReq->AddRequest(req, "get_in_id");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting ids for input objects");
    const auto& batchRsp = batchRspOrError.Value();

    {
        auto rspsOrError = batchRsp->GetResponses<TObjectYPathProxy::TRspGetBasicAttributes>("get_in_id");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            {
                const auto& rspOrError = rspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting id for input table %v",
                    table.Path.GetPath());
                const auto& rsp = rspOrError.Value();
                table.ObjectId = FromProto<TObjectId>(rsp->id());
                auto type = EObjectType(rsp->type());
                if (type != EObjectType::Table) {
                    THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv, actual %Qlv",
                        table.Path.GetPath(),
                        EObjectType::Table,
                        type);
                }
            }
        }
    }

    LOG_INFO("Input object ids received");
}

void TOperationControllerBase::GetOutputObjectIds()
{
    LOG_INFO("Getting output object ids");

    auto channel = AuthenticatedOutputMasterClient->GetMasterChannel(EMasterChannelKind::LeaderOrFollower);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& table : OutputTables) {
        auto req = TObjectYPathProxy::GetBasicAttributes(table.Path.GetPath());
        SetTransactionId(req, OutputTransactionId);
        batchReq->AddRequest(req, "get_out_id");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting ids for output objects");
    const auto& batchRsp = batchRspOrError.Value();

    {
        auto rspsOrError = batchRsp->GetResponses<TObjectYPathProxy::TRspGetBasicAttributes>("get_out_id");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            {
                const auto& rspOrError = rspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting id for output table %v",
                    table.Path.GetPath());
                const auto& rsp = rspOrError.Value();
                table.ObjectId = FromProto<TObjectId>(rsp->id());
                auto type = EObjectType(rsp->type());
                if (type != EObjectType::Table) {
                    THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv, actual %Qlv",
                        table.Path.GetPath(),
                        EObjectType::Table,
                        type);
                }
            }
        }
    }

    LOG_INFO("Output object ids received");
}

void TOperationControllerBase::ValidateFileTypes()
{
    LOG_INFO("Getting file object types");

    auto channel = AuthenticatedOutputMasterClient->GetMasterChannel(EMasterChannelKind::LeaderOrFollower);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& pair : GetFilePaths()) {
        const auto& path = pair.first;
        auto req = TObjectYPathProxy::Get(path.GetPath() + "/@type");
        SetTransactionId(req, InputTransactionId);
        batchReq->AddRequest(req, "get_file_types");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting file object types");
    const auto& batchRsp = batchRspOrError.Value();

    auto paths = GetFilePaths();
    auto rspsOrError = batchRsp->GetResponses<TObjectYPathProxy::TRspGet>("get_file_types");
    for (int index = 0; index < static_cast<int>(paths.size()); ++index) {
        const auto& richPath = paths[index].first;
        const auto& path = richPath.GetPath();
        auto stage = paths[index].second;
        const auto& rspOrError = rspsOrError[index];
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting type for file %v",
            path);

        const auto& rsp = rspOrError.Value();
        auto type = ConvertTo<EObjectType>(TYsonString(rsp->value()));
        TUserFile* file;
        switch (type) {
            case EObjectType::File:
            case EObjectType::Table:
                Files.push_back(TUserFile());
                file = &Files.back();
                break;
            default:
                THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv or %Qlv, actual %Qlv",
                    path,
                    EObjectType::File,
                    EObjectType::Table,
                    type);
        }
        file->Type = type;
        file->Stage = stage;
        file->Path = richPath;
    }

    LOG_INFO("File types received");
}

void TOperationControllerBase::FetchInputTables()
{
    auto channel = AuthenticatedInputMasterClient->GetMasterChannel(EMasterChannelKind::LeaderOrFollower);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (int tableIndex = 0; tableIndex < static_cast<int>(InputTables.size()); ++tableIndex) {
        const auto& table = InputTables[tableIndex];

        for (const auto& range : table.Path.GetRanges()) {
            for (i64 index = 0; index * Config->MaxChunksPerFetch < table.ChunkCount; ++index) {
                auto adjustedRange = range;
                auto chunkCountLowerLimit = index * Config->MaxChunksPerFetch;
                if (adjustedRange.LowerLimit().HasChunkIndex()) {
                    chunkCountLowerLimit = std::max(chunkCountLowerLimit, adjustedRange.LowerLimit().GetChunkIndex());
                }
                adjustedRange.LowerLimit().SetChunkIndex(chunkCountLowerLimit);

                auto chunkCountUpperLimit = (index + 1) * Config->MaxChunksPerFetch;
                if (adjustedRange.UpperLimit().HasChunkIndex()) {
                    chunkCountUpperLimit = std::min(chunkCountUpperLimit, adjustedRange.UpperLimit().GetChunkIndex());
                }
                adjustedRange.UpperLimit().SetChunkIndex(chunkCountUpperLimit);

                auto req = TTableYPathProxy::Fetch(FromObjectId(table.ObjectId));
                InitializeFetchRequest(req.Get(), table.Path);
                ToProto(req->mutable_ranges(), std::vector<TReadRange>({adjustedRange}));
                req->set_fetch_all_meta_extensions(false);
                req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                if (IsBoundaryKeysFetchEnabled()) {
                    req->add_extension_tags(TProtoExtensionTag<TBoundaryKeysExt>::Value);
                    req->add_extension_tags(TProtoExtensionTag<TOldBoundaryKeysExt>::Value);
                }
                req->set_fetch_parity_replicas(IsParityReplicasFetchEnabled());
                SetTransactionId(req, InputTransactionId);
                batchReq->AddRequest(req, Format("fetch_input_table_%v", tableIndex));
            }
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error fetching input tables");
    const auto& batchRsp = batchRspOrError.Value();

    for (int tableIndex = 0; tableIndex < static_cast<int>(InputTables.size()); ++tableIndex) {
        auto& table = InputTables[tableIndex];

        if (table.Path.GetRanges().empty()) {
            continue;
        }

        auto rspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspFetch>(Format("fetch_input_table_%v", tableIndex));
        for (const auto& rspOrError : rspsOrError) {
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error fetching input table %v", table.Path.GetPath());
            const auto& rsp = rspOrError.Value();
            NodeDirectory->MergeFrom(rsp->node_directory());
            for (const auto& chunk : rsp->chunks()) {
                auto chunkSpec = New<TRefCountedChunkSpec>(std::move(chunk));
                chunkSpec->set_table_index(tableIndex);
                table.Chunks.push_back(chunkSpec);
            }
        }
        LOG_INFO("Input table fetched (Path: %v, ChunkCount: %v)",
            table.Path.GetPath(),
            table.Chunks.size());
    }
}

void TOperationControllerBase::RequestInputObjects()
{
    LOG_INFO("Requesting input objects");

    auto channel = AuthenticatedInputMasterClient->GetMasterChannel(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& table : InputTables) {
        auto path = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(path);
            req->set_mode(static_cast<int>(ELockMode::Snapshot));
            SetTransactionId(req, InputTransactionId);
            GenerateMutationId(req);
            batchReq->AddRequest(req, "lock_in");
        }
        {
            auto req = TYPathProxy::Get(path);
            TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
            attributeFilter.Keys.push_back("dynamic");
            attributeFilter.Keys.push_back("sorted");
            attributeFilter.Keys.push_back("sorted_by");
            attributeFilter.Keys.push_back("chunk_count");
            ToProto(req->mutable_attribute_filter(), attributeFilter);
            SetTransactionId(req, InputTransactionId);
            batchReq->AddRequest(req, "get_in_attributes");
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error requesting input objects");
    const auto& batchRsp = batchRspOrError.Value();

    {
        auto lockInRspsOrError = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_in");
        auto getInAttributesRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_in_attributes");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            auto path = table.Path.GetPath();
            {
                const auto& rspOrError = lockInRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error locking input table %v", path);

                LOG_INFO("Input table locked (Path: %v)", path);
            }
            {
                const auto& rspOrError = getInAttributesRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting attributes for input table %v",
                    path);

                const auto& rsp = rspOrError.Value();
                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();

                if (attributes.Get<bool>("dynamic")) {
                    THROW_ERROR_EXCEPTION("Expected a static table, but got dynamic")
                        << TErrorAttribute("input_table", table.Path.GetPath());
                }

                if (attributes.Get<bool>("sorted")) {
                    table.KeyColumns = attributes.Get<TKeyColumns>("sorted_by");
                    LOG_INFO("Input table is sorted (Path: %v, KeyColumns: [%v])",
                        path,
                        JoinToString(table.KeyColumns));
                } else {
                    LOG_INFO("Input table is not sorted (Path: %v)",
                        path);
                }

                table.ChunkCount = attributes.Get<int>("chunk_count");
            }
        }
    }

    FetchInputTables();

    LOG_INFO("Input object recieved");
}


void TOperationControllerBase::RequestOutputObjects()
{
    LOG_INFO("Requesting output objects");

    auto channel = AuthenticatedOutputMasterClient->GetMasterChannel(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& table : OutputTables) {
        auto path = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(path);
            req->set_mode(static_cast<int>(table.LockMode));
            GenerateMutationId(req);
            SetTransactionId(req, OutputTransactionId);
            batchReq->AddRequest(req, "lock_out");
        }
        {
            auto req = TYPathProxy::Get(path);
            TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
            attributeFilter.Keys.push_back("channels");
            attributeFilter.Keys.push_back("compression_codec");
            attributeFilter.Keys.push_back("erasure_codec");
            attributeFilter.Keys.push_back("row_count");
            attributeFilter.Keys.push_back("replication_factor");
            attributeFilter.Keys.push_back("account");
            attributeFilter.Keys.push_back("vital");
            attributeFilter.Keys.push_back("effective_acl");
            ToProto(req->mutable_attribute_filter(), attributeFilter);
            SetTransactionId(req, OutputTransactionId);
            batchReq->AddRequest(req, "get_out_attributes");
        }
        {
            auto req = TTableYPathProxy::PrepareForUpdate(path);
            SetTransactionId(req, OutputTransactionId);
            GenerateMutationId(req);
            req->set_update_mode(static_cast<int>(table.UpdateMode));
            req->set_lock_mode(static_cast<int>(table.LockMode));
            batchReq->AddRequest(req, "prepare_for_update");
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error requesting output objects");
    const auto& batchRsp = batchRspOrError.Value();

    {
        auto lockOutRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_out");
        auto getOutAttributesRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_out_attributes");
        auto prepareForUpdateRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspPrepareForUpdate>("prepare_for_update");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            auto path = table.Path.GetPath();
            {
                const auto& rspOrError = lockOutRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error locking output table %v",
                    path);

                LOG_INFO("Output table %v locked",
                    path);
            }
            {
                const auto& rspOrError = getOutAttributesRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting attributes for output table %v",
                    path);

                const auto& rsp = rspOrError.Value();
                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();

                Deserialize(
                    table.Options->Channels,
                    ConvertToNode(attributes.GetYson("channels")));

                i64 initialRowCount = attributes.Get<i64>("row_count");
                if (initialRowCount > 0 &&
                    table.AppendRequested &&
                    table.UpdateMode == EUpdateMode::Overwrite)
                {
                    THROW_ERROR_EXCEPTION("Can't append sorted data to non-empty output table %v",
                        table.Path.GetPath());
                }
                table.Options->CompressionCodec = attributes.Get<NCompression::ECodec>("compression_codec");
                table.Options->ErasureCodec = attributes.Get<NErasure::ECodec>("erasure_codec", NErasure::ECodec::None);
                table.Options->ReplicationFactor = attributes.Get<int>("replication_factor");
                table.Options->Account = attributes.Get<Stroka>("account");
                table.Options->ChunksVital = attributes.Get<bool>("vital");
                table.EffectiveAcl = attributes.GetYson("effective_acl");

                LOG_INFO("Output table attributes received (Path: %v, Options: %v)",
                    path,
                    ConvertToYsonString(table.Options, EYsonFormat::Text).Data());
            }
            {
                const auto& rspOrError = prepareForUpdateRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error preparing output table %v for update",
                    path);

                const auto& rsp = rspOrError.Value();
                table.OutputChunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());
                LOG_INFO("Output table prepared for update (Path: %v, ChunkListId: %v)",
                    path,
                    table.OutputChunkListId);
            }
        }
    }

    LOG_INFO("Output objects recieved");
}

void TOperationControllerBase::FetchFileObjects(std::vector<TUserFile>* files)
{
    auto channel = AuthenticatedOutputMasterClient->GetMasterChannel(EMasterChannelKind::LeaderOrFollower);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& file : *files) {
        auto path = file.Path.GetPath();
        auto req = TFileYPathProxy::Fetch(path);
        ToProto(req->mutable_ranges(), std::vector<TReadRange>({TReadRange()}));
        switch (file.Type) {
            case EObjectType::Table:
                req->set_fetch_all_meta_extensions(true);
                InitializeFetchRequest(req.Get(), file.Path);
                break;
            case EObjectType::File:
                req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                break;
            default:
                YUNREACHABLE();
        }
        SetTransactionId(req, InputTransactionId);
        batchReq->AddRequest(req, "fetch_files");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error requesting file objects");
    const auto& batchRsp = batchRspOrError.Value();

    auto fetchFileRsps = batchRsp->GetResponses<TFileYPathProxy::TRspFetch>("fetch_files");
    for (int index = 0; index < static_cast<int>(files->size()); ++index) {
        auto& file = (*files)[index];
        const auto& rspOrError = fetchFileRsps[index];
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error fetching user file %v", file.Path.GetPath());

        const auto& rsp = rspOrError.Value();
        file.FetchResponse.Swap(rsp.Get());

        if (file.Type == EObjectType::Table) {
            NodeDirectory->MergeFrom(rsp->node_directory());
        }

        LOG_INFO("User file fetched (Path: %v)", file.Path.GetPath());
    }
}

void TOperationControllerBase::DoRequestFileObjects(
    std::vector<TUserFile>* files,
    std::function<void(TAttributeFilter&)> updateAttributeFilter,
    std::function<void(const TUserFile&, const IAttributeDictionary&)> onFileObject)
{
    auto channel = AuthenticatedOutputMasterClient->GetMasterChannel(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& file : *files) {
        auto path = file.Path.GetPath();
        {
            auto req = TCypressYPathProxy::Lock(path);
            req->set_mode(static_cast<int>(ELockMode::Snapshot));
            GenerateMutationId(req);
            SetTransactionId(req, InputTransactionId);
            batchReq->AddRequest(req, "lock_file");
        }
        {
            auto req = TYPathProxy::GetKey(path);
            SetTransactionId(req, InputTransactionId);
            batchReq->AddRequest(req, "get_file_name");
        }
        {
            auto req = TYPathProxy::Get(path);
            SetTransactionId(req, InputTransactionId);
            TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
            if (file.Type == EObjectType::File) {
                attributeFilter.Keys.push_back("executable");
                attributeFilter.Keys.push_back("file_name");
            }
            attributeFilter.Keys.push_back("chunk_count");
            attributeFilter.Keys.push_back("uncompressed_data_size");
            if (updateAttributeFilter) {
                updateAttributeFilter(attributeFilter);
            }
            ToProto(req->mutable_attribute_filter(), attributeFilter);
            batchReq->AddRequest(req, "get_file_attributes");
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error requesting file objects");
    const auto& batchRsp = batchRspOrError.Value();

    TEnumIndexedVector<yhash_set<Stroka>, EOperationStage> userFileNames;
    auto validateUserFileName = [&] (const TUserFile& userFile) {
        // TODO(babenko): more sanity checks?
        auto path = userFile.Path.GetPath();
        const auto& fileName = userFile.FileName;
        if (fileName.empty()) {
            THROW_ERROR_EXCEPTION("Empty user file name for %v",
                path);
        }
        if (!userFileNames[userFile.Stage].insert(fileName).second) {
            THROW_ERROR_EXCEPTION("Duplicate user file name %Qv for %v",
                fileName,
                path);
        }
    };

    {
        auto lockFileRspsOrError = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_file");
        auto getFileNameRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_file_name");
        auto getFileAttributesRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_file_attributes");
        for (int index = 0; index < static_cast<int>(files->size()); ++index) {
            auto& file = (*files)[index];
            auto path = file.Path.GetPath();

            Stroka fileName;
            bool executable = false;

            {
                const auto& rspOrError = lockFileRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error locking user file %v",
                    path);

                LOG_INFO("User file locked (Path: %v)",
                    path);
            }
            {
                const auto& rspOrError = getFileNameRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    rspOrError,
                    "Error getting file name for user file %v",
                    path);
                const auto& rsp = rspOrError.Value();

                fileName = ConvertTo<Stroka>(TYsonString(rsp->value()));
            }
            {
                const auto& rspOrError = getFileAttributesRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting attributes for user file %v",
                    path);

                const auto& rsp = rspOrError.Value();
                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();

                fileName = attributes.Get<Stroka>("file_name", fileName);
                // NB: Getting format from node attributes for table files is not supported.
                if (file.Type == EObjectType::File) {
                    executable = attributes.Get<bool>("executable", executable);
                }

                i64 fileSize = attributes.Get<i64>("uncompressed_data_size");
                if (fileSize > Config->MaxFileSize) {
                    THROW_ERROR_EXCEPTION(
                        "User file %v exceeds size limit: %v > %v",
                        path,
                        fileSize,
                        Config->MaxFileSize);
                }

                i64 chunkCount = attributes.Get<i64>("chunk_count");
                if (chunkCount > Config->MaxChunksPerFetch) {
                    THROW_ERROR_EXCEPTION(
                        "User file %v exceeds chunk count limit: %v > %v",
                        path,
                        chunkCount,
                        Config->MaxChunksPerFetch);
                }

                file.FileName = file.Path.Attributes().Get<Stroka>("file_name", fileName);
                if (file.Type == EObjectType::File) {
                    file.Executable = file.Path.Attributes().Get<bool>("executable", executable);
                } else {
                    file.Format = file.Path.Attributes().GetYson("format");
                }

                if (onFileObject) {
                    onFileObject(file, attributes);
                }

                validateUserFileName(file);

                LOG_INFO("User file attributes received (Path: %v)", path);
            }
        }
    }
}

void TOperationControllerBase::RequestFileObjects()
{
    LOG_INFO("Requesting file objects");

    DoRequestFileObjects(&Files);
    FetchFileObjects(&Files);

    LOG_INFO("File objects received");
}

void TOperationControllerBase::InitQuerySpec(
    NProto::TSchedulerJobSpecExt* schedulerJobSpecExt,
    const Stroka& queryString,
    const TTableSchema& schema)
{
    auto* querySpec = schedulerJobSpecExt->mutable_input_query_spec();
    auto parsedQueryInfo = PrepareJobQueryAst(queryString);
    auto registry = CreateBuiltinFunctionRegistry();
    auto externalFunctions = GetExternalFunctions(parsedQueryInfo, registry);
    bool fetchUdfs = externalFunctions.size() > 0;

    std::vector<TUserFile> udfFiles;
    std::vector<TUdfDescriptorPtr> descriptors;

    if (fetchUdfs) {
        if (!Config->UdfRegistryPath) {
            THROW_ERROR_EXCEPTION("External UDF registry is not configured");
        }

        LOG_INFO("Requesting UDF descriptors for: [%v]", JoinToString(externalFunctions));

        for (const auto& function : externalFunctions) {
            udfFiles.emplace_back();
            udfFiles.back().Path = GetUdfDescriptorPath(Config->UdfRegistryPath.Get(), function);
            udfFiles.back().Type = EObjectType::File;
        }

        DoRequestFileObjects(
            &udfFiles,
            [] (TAttributeFilter& attributeFilter) {
                attributeFilter.Keys.push_back(FunctionDescriptorAttribute);
                attributeFilter.Keys.push_back(AggregateDescriptorAttribute);
            },
            [&] (const TUserFile& file, const IAttributeDictionary& attributes) {
                auto descriptor = New<TUdfDescriptor>();
                descriptor->Name = file.FileName;
                descriptor->FunctionDescriptor = attributes.Find<TCypressFunctionDescriptorPtr>(FunctionDescriptorAttribute);
                descriptor->AggregateDescriptor = attributes.Find<TCypressAggregateDescriptorPtr>(AggregateDescriptorAttribute);
                descriptors.push_back(std::move(descriptor));
            });

        registry = CreateJobFunctionRegistry(descriptors, Null, std::move(registry));
    }

    auto query = PrepareJobQuery(queryString, std::move(parsedQueryInfo), schema, registry);

    if (fetchUdfs) {
        FetchFileObjects(&udfFiles);

        LOG_INFO("UDF descriptors received");
    }

    ToProto(querySpec->mutable_query(), query);

    for (const auto& descriptor : descriptors) {
        auto* protoDescriptor = querySpec->add_udf_descriptors();
        ToProto(protoDescriptor, ConvertToYsonString(descriptor).Data());
    }

    auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
    for (const auto& file : udfFiles) {
        auto *descriptor = querySpec->add_udf_files();
        descriptor->set_type(static_cast<int>(file.Type));
        descriptor->set_file_name(file.FileName);
        nodeDirectory->MergeFrom(file.FetchResponse.node_directory());
        descriptor->mutable_chunks()->MergeFrom(file.FetchResponse.chunks());
    }
    nodeDirectory->DumpTo(querySpec->mutable_node_directory());
}

void TOperationControllerBase::CollectTotals()
{
    for (const auto& table : InputTables) {
        for (const auto& chunkSpec : table.Chunks) {
            if (IsUnavailable(*chunkSpec, NeedsAllChunkParts())) {
                auto chunkId = FromProto<TChunkId>(chunkSpec->chunk_id());
                switch (Spec->UnavailableChunkStrategy) {
                    case EUnavailableChunkAction::Fail:
                        THROW_ERROR_EXCEPTION("Input chunk %v is unavailable",
                            chunkId);

                    case EUnavailableChunkAction::Skip:
                        LOG_TRACE("Skipping unavailable chunk (ChunkId: %v)",
                            chunkId);
                        continue;

                    case EUnavailableChunkAction::Wait:
                        // Do nothing.
                        break;

                    default:
                        YUNREACHABLE();
                }
            }
            i64 chunkDataSize;
            i64 chunkRowCount;
            i64 chunkValueCount;
            i64 chunkCompressedDataSize;
            NChunkClient::GetStatistics(*chunkSpec, &chunkDataSize, &chunkRowCount, &chunkValueCount, &chunkCompressedDataSize);

            TotalEstimatedInputDataSize += chunkDataSize;
            TotalEstimatedInputRowCount += chunkRowCount;
            TotalEstimatedInputValueCount += chunkValueCount;
            TotalEstimatedCompressedDataSize += chunkCompressedDataSize;
            ++TotalEstimatedInputChunkCount;
        }
    }

    LOG_INFO("Estimated input totals collected (ChunkCount: %v, DataSize: %v, RowCount: %v, ValueCount: %v, CompressedDataSize: %v)",
        TotalEstimatedInputChunkCount,
        TotalEstimatedInputDataSize,
        TotalEstimatedInputRowCount,
        TotalEstimatedInputValueCount,
        TotalEstimatedCompressedDataSize);
}

void TOperationControllerBase::CustomPrepare()
{ }

// NB: must preserve order of chunks in the input tables, no shuffling.
std::vector<TRefCountedChunkSpecPtr> TOperationControllerBase::CollectInputChunks() const
{
    std::vector<TRefCountedChunkSpecPtr> result;
    for (const auto& table : InputTables) {
        for (const auto& chunkSpec : table.Chunks) {
            if (IsUnavailable(*chunkSpec, NeedsAllChunkParts())) {
                switch (Spec->UnavailableChunkStrategy) {
                    case EUnavailableChunkAction::Skip:
                        continue;

                    case EUnavailableChunkAction::Wait:
                        // Do nothing.
                        break;

                    default:
                        YUNREACHABLE();
                }
            }
            result.push_back(chunkSpec);
        }
    }
    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::SliceChunks(
    const std::vector<TRefCountedChunkSpecPtr>& chunkSpecs,
    i64 maxSliceDataSize,
    int* jobCount)
{
    std::vector<TChunkStripePtr> result;
    auto appendStripes = [&] (std::vector<TChunkSlicePtr> slices) {
        for (const auto& slice : slices) {
            result.push_back(New<TChunkStripe>(slice));
        }
    };

    // TODO(ignat): we slice on two parts even if TotalEstimatedInputDataSize very small.
    i64 sliceDataSize = std::min(maxSliceDataSize, (i64)std::max(Config->SliceDataSizeMultiplier * TotalEstimatedInputDataSize / *jobCount, 1.0));

    for (const auto& chunkSpec : chunkSpecs) {
        int oldSize = result.size();

        bool hasNontrivialLimits = !IsCompleteChunk(*chunkSpec);

        auto codecId = NErasure::ECodec(chunkSpec->erasure_codec());
        if (hasNontrivialLimits || codecId == NErasure::ECodec::None) {
            auto slices = SliceChunkByRowIndexes(chunkSpec, sliceDataSize);
            appendStripes(slices);
        } else {
            for (const auto& slice : CreateErasureChunkSlices(chunkSpec, codecId)) {
                auto slices = slice->SliceEvenly(sliceDataSize);
                appendStripes(slices);
            }
        }

        LOG_TRACE("Slicing chunk (ChunkId: %v, SliceCount: %v)",
            FromProto<TChunkId>(chunkSpec->chunk_id()),
            result.size() - oldSize);
    }

    *jobCount = std::min(*jobCount, static_cast<int>(result.size()));
    if (!result.empty()) {
        *jobCount = std::max(*jobCount, 1 + (static_cast<int>(result.size()) - 1) / Config->MaxChunkStripesPerJob);
    }

    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::SliceInputChunks(
        i64 maxSliceDataSize,
        int* jobCount)
{
    return SliceChunks(CollectInputChunks(), maxSliceDataSize, jobCount);
}

TKeyColumns TOperationControllerBase::CheckInputTablesSorted(const TKeyColumns& keyColumns)
{
    YCHECK(!InputTables.empty());

    for (const auto& table : InputTables) {
        if (table.KeyColumns.empty()) {
            THROW_ERROR_EXCEPTION("Input table %v is not sorted",
                table.Path.GetPath());
        }
    }

    if (!keyColumns.empty()) {
        for (const auto& table : InputTables) {
            if (!CheckKeyColumnsCompatible(table.KeyColumns, keyColumns)) {
                THROW_ERROR_EXCEPTION("Input table %v is sorted by columns [%v] that are not compatible with the requested columns [%v]",
                    table.Path.GetPath(),
                    JoinToString(table.KeyColumns),
                    JoinToString(keyColumns));
            }
        }
        return keyColumns;
    } else {
        const auto& referenceTable = InputTables[0];
        for (const auto& table : InputTables) {
            if (table.KeyColumns != referenceTable.KeyColumns) {
                THROW_ERROR_EXCEPTION("Key columns do not match: input table %v is sorted by columns [%v] while input table %v is sorted by columns [%v]",
                    table.Path.GetPath(),
                    JoinToString(table.KeyColumns),
                    referenceTable.Path.GetPath(),
                    JoinToString(referenceTable.KeyColumns));
            }
        }
        return referenceTable.KeyColumns;
    }
}

bool TOperationControllerBase::CheckKeyColumnsCompatible(
    const std::vector<Stroka>& fullColumns,
    const std::vector<Stroka>& prefixColumns)
{
    if (fullColumns.size() < prefixColumns.size()) {
        return false;
    }

    for (int index = 0; index < static_cast<int>(prefixColumns.size()); ++index) {
        if (fullColumns[index] != prefixColumns[index]) {
            return false;
        }
    }

    return true;
}

//! Returns longest common prefix of input table keys.
TKeyColumns TOperationControllerBase::GetCommonInputKeyPrefix()
{
    auto commonKey = InputTables[0].KeyColumns;
    for (const auto& table : InputTables) {
        if (table.KeyColumns.size() < commonKey.size()) {
            commonKey.erase(commonKey.begin() + table.KeyColumns.size(), commonKey.end());
        }

        int i = 0;
        for (; i < static_cast<int>(commonKey.size()); ++i) {
            if (commonKey[i] != table.KeyColumns[i]) {
                break;
            }
        }
        commonKey.erase(commonKey.begin() + i, commonKey.end());
    }
    return commonKey;
}

bool TOperationControllerBase::IsSortedOutputSupported() const
{
    return false;
}

bool TOperationControllerBase::IsParityReplicasFetchEnabled() const
{
    return false;
}

bool TOperationControllerBase::IsBoundaryKeysFetchEnabled() const
{
    return false;
}

void TOperationControllerBase::UpdateAllTasksIfNeeded(const TProgressCounter& jobCounter)
{
    if (jobCounter.GetAborted(EAbortReason::ResourceOverdraft) == Config->MaxMemoryReserveAbortJobCount) {
        UpdateAllTasks();
    }
}

bool TOperationControllerBase::IsMemoryReserveEnabled(const TProgressCounter& jobCounter) const
{
    return jobCounter.GetAborted(EAbortReason::ResourceOverdraft) < Config->MaxMemoryReserveAbortJobCount;
}

i64 TOperationControllerBase::GetMemoryReserve(bool memoryReserveEnabled, TUserJobSpecPtr userJobSpec) const
{
    i64 size = 0;
    if (memoryReserveEnabled) {
        size += static_cast<i64>(userJobSpec->MemoryLimit * userJobSpec->MemoryReserveFactor);
    } else {
        size += userJobSpec->MemoryLimit;
    }

    if (userJobSpec->TmpfsSize) {
        size += *userJobSpec->TmpfsSize;
    }
    return size;
}

void TOperationControllerBase::RegisterOutput(
    const TChunkTreeId& chunkTreeId,
    int key,
    int tableIndex,
    TOutputTable& table)
{
    if (chunkTreeId == NullChunkTreeId) {
        return;
    }

    table.OutputChunkTreeIds.insert(std::make_pair(key, chunkTreeId));

    if (IsOutputLivePreviewSupported()) {
        auto masterConnector = Host->GetMasterConnector();
        masterConnector->AttachToLivePreview(
            Operation,
            table.LivePreviewChunkListId,
            chunkTreeId);
    }

    LOG_DEBUG("Output chunk tree registered (Table: %v, ChunkTreeId: %v, Key: %v)",
        tableIndex,
        chunkTreeId,
        key);
}

void TOperationControllerBase::RegisterBoundaryKeys(
    const TBoundaryKeysExt& boundaryKeys,
    int key,
    TOutputTable* outputTable)
{
    TJobBoundaryKeys jobBoundaryKeys;
    FromProto(&jobBoundaryKeys.MinKey, boundaryKeys.min());
    FromProto(&jobBoundaryKeys.MaxKey, boundaryKeys.max());
    jobBoundaryKeys.ChunkTreeKey = key;
    outputTable->BoundaryKeys.push_back(jobBoundaryKeys);
}

void TOperationControllerBase::RegisterOutput(
    TRefCountedChunkSpecPtr chunkSpec,
    int key,
    int tableIndex)
{
    auto& table = OutputTables[tableIndex];

    if (!table.KeyColumns.empty() && IsSortedOutputSupported()) {
        auto boundaryKeys = GetProtoExtension<TBoundaryKeysExt>(chunkSpec->chunk_meta().extensions());
        RegisterBoundaryKeys(boundaryKeys, key, &table);
    }

    RegisterOutput(FromProto<TChunkId>(chunkSpec->chunk_id()), key, tableIndex, table);
}

void TOperationControllerBase::RegisterOutput(
    TJobletPtr joblet,
    int key,
    const TCompletedJobSummary& jobSummary)
{
    const auto* userJobResult = FindUserJobResult(jobSummary.Result);

    for (int tableIndex = 0; tableIndex < static_cast<int>(OutputTables.size()); ++tableIndex) {
        auto& table = OutputTables[tableIndex];
        RegisterOutput(joblet->ChunkListIds[tableIndex], key, tableIndex, table);

        if (!table.KeyColumns.empty() && IsSortedOutputSupported()) {
            YCHECK(userJobResult);
            const auto& boundaryKeys = userJobResult->output_boundary_keys(tableIndex);
            RegisterBoundaryKeys(boundaryKeys, key, &table);
        }
    }
}

void TOperationControllerBase::RegisterInputStripe(TChunkStripePtr stripe, TTaskPtr task)
{
    yhash_set<TChunkId> visitedChunks;

    TStripeDescriptor stripeDescriptor;
    stripeDescriptor.Stripe = stripe;
    stripeDescriptor.Task = task;
    stripeDescriptor.Cookie = task->GetChunkPoolInput()->Add(stripe);

    for (const auto& slice : stripe->ChunkSlices) {
        auto chunkSpec = slice->GetChunkSpec();
        auto chunkId = FromProto<TChunkId>(chunkSpec->chunk_id());

        auto pair = InputChunkMap.insert(std::make_pair(chunkId, TInputChunkDescriptor()));
        auto& chunkDescriptor = pair.first->second;

        if (InputChunkSpecs.insert(chunkSpec).second) {
            chunkDescriptor.ChunkSpecs.push_back(chunkSpec);
        }

        if (IsUnavailable(*chunkSpec, NeedsAllChunkParts())) {
            chunkDescriptor.State = EInputChunkState::Waiting;
        }

        if (visitedChunks.insert(chunkId).second) {
            chunkDescriptor.InputStripes.push_back(stripeDescriptor);
        }
    }
}

void TOperationControllerBase::RegisterIntermediate(
    TJobletPtr joblet,
    TCompletedJobPtr completedJob,
    TChunkStripePtr stripe)
{
    for (const auto& chunkSlice : stripe->ChunkSlices) {
        auto chunkId = FromProto<TChunkId>(chunkSlice->GetChunkSpec()->chunk_id());
        YCHECK(ChunkOriginMap.insert(std::make_pair(chunkId, completedJob)).second);

        if (IsIntermediateLivePreviewSupported()) {
            auto masterConnector = Host->GetMasterConnector();
            masterConnector->AttachToLivePreview(
                Operation,
                IntermediateTable.LivePreviewChunkListId,
                chunkId);
        }
    }
}

bool TOperationControllerBase::HasEnoughChunkLists(int requestedCount)
{
    return ChunkListPool->HasEnough(requestedCount);
}

TChunkListId TOperationControllerBase::ExtractChunkList()
{
    return ChunkListPool->Extract();
}

void TOperationControllerBase::RegisterJoblet(TJobletPtr joblet)
{
    YCHECK(JobletMap.insert(std::make_pair(joblet->JobId, joblet)).second);
}

TOperationControllerBase::TJobletPtr TOperationControllerBase::GetJoblet(const TJobId& jobId)
{
    auto it = JobletMap.find(jobId);
    YCHECK(it != JobletMap.end());
    return it->second;
}

void TOperationControllerBase::RemoveJoblet(const TJobId& jobId)
{
    YCHECK(JobletMap.erase(jobId) == 1);
}

void TOperationControllerBase::BuildProgress(IYsonConsumer* consumer) const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    BuildYsonMapFluently(consumer)
        .Item("jobs").Value(JobCounter)
        .Item("ready_job_count").Value(GetPendingJobCount())
        .Item("job_statistics").Value(JobStatistics)
        .Item("estimated_input_statistics").BeginMap()
            .Item("chunk_count").Value(TotalEstimatedInputChunkCount)
            .Item("uncompressed_data_size").Value(TotalEstimatedInputDataSize)
            .Item("compressed_data_size").Value(TotalEstimatedCompressedDataSize)
            .Item("row_count").Value(TotalEstimatedInputRowCount)
            .Item("unavailable_chunk_count").Value(UnavailableInputChunkCount)
        .EndMap()
        .Item("live_preview").BeginMap()
            .Item("output_supported").Value(IsOutputLivePreviewSupported())
            .Item("intermediate_supported").Value(IsIntermediateLivePreviewSupported())
        .EndMap();
}

void TOperationControllerBase::BuildBriefProgress(IYsonConsumer* consumer) const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    BuildYsonMapFluently(consumer)
        .Item("jobs").Value(JobCounter);
}

void TOperationControllerBase::BuildResult(IYsonConsumer* consumer) const
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto error = FromProto<TError>(Operation->Result().error());
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("error").Value(error)
        .EndMap();
}

void TOperationControllerBase::UpdateJobStatistics(const TJobSummary& jobSummary)
{
    auto statistics = jobSummary.Statistics;
    LOG_INFO("Job data statistics (JobId: %v, Input: {%v}, Output: {%v})",
        jobSummary.Id,
        GetTotalInputDataStatistics(statistics),
        GetTotalOutputDataStatistics(statistics));

    statistics.AddSuffixToNames(jobSummary.StatisticsSuffix);
    JobStatistics.Update(statistics);
}

void TOperationControllerBase::BuildBriefSpec(IYsonConsumer* consumer) const
{
    BuildYsonMapFluently(consumer)
        .DoIf(Spec->Title.HasValue(), [&] (TFluentMap fluent) {
            fluent
                .Item("title").Value(*Spec->Title);
        })
        .Item("input_table_paths").ListLimited(GetInputTablePaths(), 1)
        .Item("output_table_paths").ListLimited(GetOutputTablePaths(), 1);
}

std::vector<TOperationControllerBase::TPathWithStage> TOperationControllerBase::GetFilePaths() const
{
    return std::vector<TPathWithStage>();
}

bool TOperationControllerBase::NeedsAllChunkParts() const
{
    return false;
}

bool TOperationControllerBase::IsRowCountPreserved() const
{
    return false;
}

int TOperationControllerBase::SuggestJobCount(
    i64 totalDataSize,
    i64 dataSizePerJob,
    TNullable<int> configJobCount,
    int maxJobCount) const
{
    i64 suggestionBySize = (totalDataSize + dataSizePerJob - 1) / dataSizePerJob;
    i64 jobCount = configJobCount.Get(suggestionBySize);
    return static_cast<int>(Clamp(jobCount, 1, maxJobCount));
}

void TOperationControllerBase::InitUserJobSpecTemplate(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TUserJobSpecPtr config,
    const std::vector<TUserFile>& files)
{
    jobSpec->set_shell_command(config->Command);
    jobSpec->set_memory_limit(config->MemoryLimit);
    jobSpec->set_include_memory_mapped_files(config->IncludeMemoryMappedFiles);
    jobSpec->set_iops_threshold(config->IopsThreshold);
    jobSpec->set_use_yamr_descriptors(config->UseYamrDescriptors);
    jobSpec->set_check_input_fully_consumed(config->CheckInputFullyConsumed);
    jobSpec->set_max_stderr_size(config->MaxStderrSize);
    jobSpec->set_enable_core_dump(config->EnableCoreDump);
    jobSpec->set_custom_statistics_count_limit(config->CustomStatisticsCountLimit);

    if (config->TmpfsSize) {
        jobSpec->set_tmpfs_size(*config->TmpfsSize);
    }

    if (Config->UserJobBlkioWeight) {
        jobSpec->set_blkio_weight(*Config->UserJobBlkioWeight);
    }

    {
        // Set input and output format.
        TFormat inputFormat(EFormatType::Yson);
        TFormat outputFormat(EFormatType::Yson);

        if (config->Format) {
            inputFormat = outputFormat = config->Format.Get();
        }

        if (config->InputFormat) {
            inputFormat = config->InputFormat.Get();
        }

        if (config->OutputFormat) {
            outputFormat = config->OutputFormat.Get();
        }

        jobSpec->set_input_format(ConvertToYsonString(inputFormat).Data());
        jobSpec->set_output_format(ConvertToYsonString(outputFormat).Data());
    }

    auto fillEnvironment = [&] (yhash_map<Stroka, Stroka>& env) {
        for (const auto& pair : env) {
            jobSpec->add_environment(Format("%v=%v", pair.first, pair.second));
        }
    };

    // Global environment.
    fillEnvironment(Config->Environment);

    // Local environment.
    fillEnvironment(config->Environment);

    jobSpec->add_environment(Format("YT_OPERATION_ID=%v", OperationId));

    auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
    auto registerChunks = [&] (
        const NChunkClient::NProto::TRspFetch& response,
        google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>* chunkSpecs)
    {
        nodeDirectory->MergeFrom(response.node_directory());
        chunkSpecs->MergeFrom(response.chunks());
    };

    for (const auto& file : files) {
        auto *descriptor = jobSpec->add_files();
        descriptor->set_type(static_cast<int>(file.Type));
        descriptor->set_file_name(file.FileName);
        registerChunks(file.FetchResponse, descriptor->mutable_chunks());
        switch (file.Type) {
            case EObjectType::File:
                descriptor->set_executable(file.Executable);
                break;
            case EObjectType::Table:
                descriptor->set_format(file.Format.Data());
                break;
            default:
                YUNREACHABLE();
        }
    }

    nodeDirectory->DumpTo(jobSpec->mutable_node_directory());
}

void TOperationControllerBase::InitUserJobSpec(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TJobletPtr joblet,
    i64 memoryReserve)
{
    ToProto(jobSpec->mutable_async_scheduler_transaction_id(), AsyncSchedulerTransactionId);

    jobSpec->set_memory_reserve(memoryReserve);

    jobSpec->add_environment(Format("YT_JOB_INDEX=%v", joblet->JobIndex));
    jobSpec->add_environment(Format("YT_JOB_ID=%v", joblet->JobId));
    if (joblet->StartRowIndex >= 0) {
        jobSpec->add_environment(Format("YT_START_ROW_INDEX=%v", joblet->StartRowIndex));
    }
}

i64 TOperationControllerBase::GetFinalOutputIOMemorySize(TJobIOConfigPtr ioConfig) const
{
    i64 result = 0;
    for (const auto& outputTable : OutputTables) {
        if (outputTable.Options->ErasureCodec == NErasure::ECodec::None) {
            i64 maxBufferSize = std::max(
                ioConfig->TableWriter->MaxRowWeight,
                ioConfig->TableWriter->MaxBufferSize);
            result += GetOutputWindowMemorySize(ioConfig) + maxBufferSize;
        } else {
            auto* codec = NErasure::GetCodec(outputTable.Options->ErasureCodec);
            double replicationFactor = (double) codec->GetTotalPartCount() / codec->GetDataPartCount();
            result += static_cast<i64>(ioConfig->TableWriter->DesiredChunkSize * replicationFactor);
        }
    }
    return result;
}

i64 TOperationControllerBase::GetFinalIOMemorySize(
    TJobIOConfigPtr ioConfig,
    const TChunkStripeStatisticsVector& stripeStatistics) const
{
    i64 result = 0;
    for (const auto& stat : stripeStatistics) {
        result += GetInputIOMemorySize(ioConfig, stat);
    }
    result += GetFinalOutputIOMemorySize(ioConfig);
    return result;
}

void TOperationControllerBase::InitIntermediateInputConfig(TJobIOConfigPtr config)
{
    // Disable master requests.
    config->TableReader->AllowFetchingSeedsFromMaster = false;
}

void TOperationControllerBase::InitIntermediateOutputConfig(TJobIOConfigPtr config)
{
    // Don't replicate intermediate output.
    config->TableWriter->UploadReplicationFactor = 1;
    config->TableWriter->MinUploadReplicationFactor = 1;

    // Cache blocks on nodes.
    config->TableWriter->PopulateCache = true;

    // Don't sync intermediate chunks.
    config->TableWriter->SyncOnClose = false;
}

void TOperationControllerBase::ValidateKey(const TOwningKey& key)
{
    for (int i = 0; i < key.GetCount(); ++i) {
        ValidateKeyValue(key[i]);
    }
}

void TOperationControllerBase::InitFinalOutputConfig(TJobIOConfigPtr /* config */)
{ }

TFluentLogEvent TOperationControllerBase::LogEventFluently(ELogEventType eventType)
{
    return Host->LogEventFluently(eventType)
        .Item("operation_id").Value(OperationId);
}

IClientPtr TOperationControllerBase::CreateClient()
{
    TClientOptions options;
    options.User = Operation->GetAuthenticatedUser();
    return Host
        ->GetMasterClient()
        ->GetConnection()
        ->CreateClient(options);
}

const NProto::TUserJobResult* TOperationControllerBase::FindUserJobResult(const TRefCountedJobResultPtr& result)
{
    const auto& schedulerJobResultExt = result->GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    if (schedulerJobResultExt.has_user_job_result()) {
        return &schedulerJobResultExt.user_job_result();
    }
    return nullptr;
}

void TOperationControllerBase::ValidateUserFileCount(TUserJobSpecPtr spec, const Stroka& operation)
{
    if (spec && spec->FilePaths.size() > Config->MaxUserFileCount) {
        THROW_ERROR_EXCEPTION("Too many user files in %v: maximum allowed %v, actual %v",
            operation,
            Config->MaxUserFileCount,
            spec->FilePaths.size());
    }
}

void TOperationControllerBase::Persist(TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, TotalEstimatedInputChunkCount);
    Persist(context, TotalEstimatedInputDataSize);
    Persist(context, TotalEstimatedInputRowCount);
    Persist(context, TotalEstimatedInputValueCount);
    Persist(context, TotalEstimatedCompressedDataSize);

    Persist(context, UnavailableInputChunkCount);

    Persist(context, JobCounter);

    Persist(context, NodeDirectory);

    Persist(context, InputTables);

    Persist(context, OutputTables);

    Persist(context, IntermediateTable);

    Persist(context, Files);

    Persist(context, Tasks);

    Persist(context, TaskGroups);

    Persist(context, InputChunkMap);

    Persist(context, CachedPendingJobCount);

    Persist(context, CachedNeededResources);

    Persist(context, ChunkOriginMap);

    Persist(context, JobletMap);

    Persist(context, JobIndexGenerator);

    Persist(context, JobStatistics);

    // NB: Scheduler snapshots need not be stable.
    Persist<
        TSetSerializer<
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, InputChunkSpecs);

    if (context.IsLoad()) {
        for (auto task : Tasks) {
            task->Initialize();
        }
    }
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

