#include "job_proxy.h"
#include "config.h"
#include "job_prober_service.h"
#include "merge_job.h"
#include "partition_job.h"
#include "partition_sort_job.h"
#include "remote_copy_job.h"
#include "simple_sort_job.h"
#include "sorted_merge_job.h"
#include "user_job.h"
#include "user_job_io.h"
#include "user_job_synchronizer.h"

#include <yt/server/containers/public.h>

#include <yt/server/exec_agent/config.h>
#include <yt/server/exec_agent/supervisor_service.pb.h>

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/native_connection.h>

#include <yt/ytlib/cgroup/cgroup.h>

#include <yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/ytlib/chunk_client/config.h>
#include <yt/ytlib/chunk_client/data_statistics.h>

#include <yt/ytlib/job_proxy/job_spec_helper.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>
#include <yt/ytlib/node_tracker_client/helpers.h>

#include <yt/ytlib/scheduler/public.h>

#include <yt/core/bus/tcp_client.h>
#include <yt/core/bus/tcp_server.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/lfalloc_helpers.h>
#include <yt/core/misc/proc.h>
#include <yt/core/misc/ref_counted_tracker.h>

#include <yt/core/rpc/bus_channel.h>
#include <yt/core/rpc/helpers.h>
#include <yt/core/rpc/server.h>
#include <yt/core/rpc/bus_server.h>

#include <yt/core/ytree/public.h>

#include <util/system/fs.h>
#include <util/system/execpath.h>

#include <util/folder/dirut.h>

namespace NYT {
namespace NJobProxy {

using namespace NScheduler;
using namespace NExecAgent;
using namespace NExecAgent::NProto;
using namespace NBus;
using namespace NRpc;
using namespace NApi;
using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJobProberClient;
using namespace NJobTrackerClient::NProto;
using namespace NConcurrency;
using namespace NCGroup;
using namespace NYTree;
using namespace NYson;
using namespace NContainers;

using NJobTrackerClient::TStatistics;

const TString SlotBindPath("/slot");

////////////////////////////////////////////////////////////////////////////////

TJobProxy::TJobProxy(
    TJobProxyConfigPtr config,
    const TOperationId& operationId,
    const TJobId& jobId)
    : Config_(std::move(config))
    , OperationId_(operationId)
    , JobId_(jobId)
    , JobThread_(New<TActionQueue>("JobMain"))
    , ControlThread_(New<TActionQueue>("Control"))
    , Logger(JobProxyLogger)
{
    Logger.AddTag("OperationId: %v, JobId: %v",
        OperationId_,
        JobId_);
}

TString TJobProxy::GetPreparationPath() const
{
    return NFs::CurrentWorkingDirectory();
}

TString TJobProxy::GetSlotPath() const
{
    return Config_->RootPath && !Config_->TestRootFS
       ? SlotBindPath
       : NFs::CurrentWorkingDirectory();
}

std::vector<NChunkClient::TChunkId> TJobProxy::DumpInputContext()
{
    return Job_->DumpInputContext();
}

TString TJobProxy::GetStderr()
{
    return Job_->GetStderr();
}

TYsonString TJobProxy::StraceJob()
{
    return Job_->StraceJob();
}

void TJobProxy::SignalJob(const TString& signalName)
{
    Job_->SignalJob(signalName);
}

TYsonString TJobProxy::PollJobShell(const TYsonString& parameters)
{
    return Job_->PollJobShell(parameters);
}

void TJobProxy::Interrupt()
{
    Job_->Interrupt();
}

void TJobProxy::Fail()
{
    Job_->Fail();
}

IServerPtr TJobProxy::GetRpcServer() const
{
    return RpcServer_;
}

void TJobProxy::ValidateJobId(const TJobId& jobId)
{
    if (JobId_ != jobId) {
        THROW_ERROR_EXCEPTION("Job id mismatch: expected %v, got %v",
            JobId_,
            jobId);
    }

    if (!Job_) {
        THROW_ERROR_EXCEPTION("Job has not started yet");
    }
}

void TJobProxy::SendHeartbeat()
{
    auto req = SupervisorProxy_->OnJobProgress();
    ToProto(req->mutable_job_id(), JobId_);
    req->set_progress(Job_->GetProgress());
    req->set_statistics(ConvertToYsonString(GetStatistics()).GetData());
    req->set_stderr_size(Job_->GetStderrSize());

    req->Invoke().Subscribe(BIND(&TJobProxy::OnHeartbeatResponse, MakeWeak(this)));

    LOG_DEBUG("Supervisor heartbeat sent");
}

void TJobProxy::OnHeartbeatResponse(const TError& error)
{
    if (!error.IsOK()) {
        // NB: user process is not killed here.
        // Good user processes are supposed to die themselves
        // when io pipes are closed.
        // Bad processes will die at container shutdown.
        LOG_ERROR(error, "Error sending heartbeat to supervisor");
        Exit(EJobProxyExitCode::HeartbeatFailed);
    }

    LOG_DEBUG("Successfully reported heartbeat to supervisor");
}

void TJobProxy::RetrieveJobSpec()
{
    LOG_INFO("Requesting job spec");

    auto req = SupervisorProxy_->GetJobSpec();
    ToProto(req->mutable_job_id(), JobId_);

    auto rspOrError = req->Invoke().Get();
    if (!rspOrError.IsOK()) {
        LOG_ERROR(rspOrError, "Failed to get job spec");
        Exit(EJobProxyExitCode::GetJobSpecFailed);
    }

    const auto& rsp = rspOrError.Value();

    if (rsp->job_spec().version() != GetJobSpecVersion()) {
        LOG_WARNING("Invalid job spec version (Expected: %v, Actual: %v)",
            GetJobSpecVersion(),
            rsp->job_spec().version());
        Exit(EJobProxyExitCode::InvalidSpecVersion);
    }

    JobSpecHelper_ = CreateJobSpecHelper(rsp->job_spec());
    const auto& resourceUsage = rsp->resource_usage();

    LOG_INFO("Job spec received (JobType: %v, ResourceLimits: {Cpu: %v, Memory: %v, Network: %v})\n%v",
        NScheduler::EJobType(rsp->job_spec().type()),
        resourceUsage.cpu(),
        resourceUsage.memory(),
        resourceUsage.network(),
        rsp->job_spec().DebugString());

    JobProxyMemoryReserve_ = resourceUsage.memory();
    CpuLimit_ = resourceUsage.cpu();
    NetworkUsage_ = resourceUsage.network();

    // We never report to node less memory usage, than was initially reserved.
    TotalMaxMemoryUsage_ = JobProxyMemoryReserve_ - Config_->AheadMemoryReserve;
    ApprovedMemoryReserve_ = JobProxyMemoryReserve_;

    std::vector<TString> annotations{
        Format("OperationId: %v", OperationId_),
        Format("JobId: %v", JobId_),
        Format("JobType: %v", GetJobSpecHelper()->GetJobType()),
    };

    for (auto* descriptor : {
        &GetJobSpecHelper()->GetJobIOConfig()->TableReader->WorkloadDescriptor,
        &GetJobSpecHelper()->GetJobIOConfig()->TableWriter->WorkloadDescriptor,
        &GetJobSpecHelper()->GetJobIOConfig()->ErrorFileWriter->WorkloadDescriptor
    })
    {
        descriptor->Annotations.insert(
            descriptor->Annotations.end(),
            annotations.begin(),
            annotations.end());
    }
}

void TJobProxy::Run()
{
    auto startTime = Now();
    auto resultOrError = BIND(&TJobProxy::DoRun, Unretained(this))
        .AsyncVia(JobThread_->GetInvoker())
        .Run()
        .Get();
    auto finishTime = Now();

    TJobResult result;
    if (!resultOrError.IsOK()) {
        LOG_ERROR(resultOrError, "Job failed");
        ToProto(result.mutable_error(), resultOrError);
    } else {
        result = resultOrError.Value();
    }

    // Reliably terminate all async calls before reporting result.
    if (HeartbeatExecutor_) {
        WaitFor(HeartbeatExecutor_->Stop())
            .ThrowOnError();
    }

    if (MemoryWatchdogExecutor_) {
        WaitFor(MemoryWatchdogExecutor_->Stop())
            .ThrowOnError();
    }

    RpcServer_->Stop()
        .WithTimeout(RpcServerShutdownTimeout)
        .Get();

    if (Job_) {
        auto failedChunkIds = Job_->GetFailedChunkIds();
        LOG_INFO("Found %v failed chunks", static_cast<int>(failedChunkIds.size()));

        // For erasure chunks, replace part id with whole chunk id.
        auto* schedulerResultExt = result.MutableExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
        for (const auto& chunkId : failedChunkIds) {
            auto actualChunkId = IsErasureChunkPartId(chunkId)
                ? ErasureChunkIdFromPartId(chunkId)
                : chunkId;
            ToProto(schedulerResultExt->add_failed_chunk_ids(), actualChunkId);
        }

        auto interruptDescriptor = Job_->GetInterruptDescriptor();

        if (!interruptDescriptor.UnreadDataSliceDescriptors.empty()) {
            if (!interruptDescriptor.ReadDataSliceDescriptors.empty()) {
                ToProto(
                    schedulerResultExt->mutable_unread_chunk_specs(),
                    schedulerResultExt->mutable_chunk_spec_count_per_unread_data_slice(),
                    interruptDescriptor.UnreadDataSliceDescriptors);
                ToProto(
                    schedulerResultExt->mutable_read_chunk_specs(),
                    schedulerResultExt->mutable_chunk_spec_count_per_read_data_slice(),
                    interruptDescriptor.ReadDataSliceDescriptors);

                LOG_DEBUG(
                    "Found interrupt descriptor (UnreadDescriptorCount: %v, ReadDescriptorCount: %v, SchedulerResultExt: %v)",
                    interruptDescriptor.UnreadDataSliceDescriptors.size(),
                    interruptDescriptor.ReadDataSliceDescriptors.size(),
                    schedulerResultExt->ShortDebugString());
            } else {
                if (result.error().code() == 0) {
                    // It is tempting to check /data/input/row_count statistics to be equal to zero.
                    // Surprisingly we could still have read some foreign rows, but since we didn't read primary rows
                    // we made no progress. So let's chunk data slice count at least.

                    auto getPrimaryDataSliceCount = [&] () {
                        int result = 0;
                        for (const auto& inputTableSpec : JobSpecHelper_->GetSchedulerJobSpecExt().input_table_specs()) {
                            result += inputTableSpec.chunk_spec_count_per_data_slice_size();
                        }
                        return result;
                    };

                    YCHECK(getPrimaryDataSliceCount() == interruptDescriptor.UnreadDataSliceDescriptors.size());

                    ToProto(
                        result.mutable_error(),
                        TError(EErrorCode::JobNotPrepared, "Job did not read anything"));
                }
            }
        }
    }

    auto statistics = ConvertToYsonString(GetStatistics());

    EnsureStderrResult(&result);

    ReportResult(result, statistics, startTime, finishTime);
}

IJobPtr TJobProxy::CreateBuiltinJob()
{
    auto jobType = GetJobSpecHelper()->GetJobType();
    switch (jobType) {
        case NScheduler::EJobType::OrderedMerge:
            return CreateOrderedMergeJob(this);

        case NScheduler::EJobType::UnorderedMerge:
            return CreateUnorderedMergeJob(this);

        case NScheduler::EJobType::SortedMerge:
            return CreateSortedMergeJob(this);

        case NScheduler::EJobType::FinalSort:
        case NScheduler::EJobType::IntermediateSort:
            return CreatePartitionSortJob(this);

        case NScheduler::EJobType::SimpleSort:
            return CreateSimpleSortJob(this);

        case NScheduler::EJobType::Partition:
            return CreatePartitionJob(this);

        case NScheduler::EJobType::RemoteCopy:
            return CreateRemoteCopyJob(this);

        default:
            Y_UNREACHABLE();
    }
}

TJobResult TJobProxy::DoRun()
{
    try {
        // Use everything.

        auto createRootFS = [&] () -> TNullable<TRootFS> {
            if (!Config_->RootPath) {
                LOG_DEBUG("Job is not using custom root fs");
                return Null;
            }

            if (Config_->TestRootFS) {
                LOG_DEBUG("Job is running in testing root fs mode");
                return Null;
            }

            LOG_DEBUG("Job is using custom root fs (Path: %v)", Config_->RootPath);

            TRootFS rootFS;
            rootFS.RootPath = *Config_->RootPath;
            rootFS.Binds.emplace_back(TBind {NFs::CurrentWorkingDirectory(), SlotBindPath, false});

            return rootFS;
        };

        ResourceController = CreateResourceController(Config_->JobEnvironment, createRootFS());

        LocalDescriptor_ = NNodeTrackerClient::TNodeDescriptor(Config_->Addresses, Config_->Rack, Config_->DataCenter);

        RpcServer_ = CreateBusServer(CreateTcpBusServer(Config_->BusServer));
        RpcServer_->RegisterService(CreateJobProberService(this));
        RpcServer_->Start();

        auto supervisorClient = CreateTcpBusClient(Config_->SupervisorConnection);
        auto supervisorChannel = CreateBusChannel(supervisorClient);

        SupervisorProxy_.reset(new TSupervisorServiceProxy(supervisorChannel));
        SupervisorProxy_->SetDefaultTimeout(Config_->SupervisorRpcTimeout);

        auto clusterConnection = CreateNativeConnection(Config_->ClusterConnection);

        Client_ = clusterConnection->CreateNativeClient(TClientOptions(NSecurityClient::JobUserName));

        RetrieveJobSpec();
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Failed to prepare job proxy");
        Exit(EJobProxyExitCode::JobProxyPrepareFailed);
    }

    const auto& schedulerJobSpecExt = GetJobSpecHelper()->GetSchedulerJobSpecExt();
    NLFAlloc::SetBufferSize(schedulerJobSpecExt.lfalloc_buffer_size());
    JobProxyMemoryOvercommitLimit_ =
        schedulerJobSpecExt.has_job_proxy_memory_overcommit_limit() ?
        MakeNullable(schedulerJobSpecExt.job_proxy_memory_overcommit_limit()) :
        Null;

    RefCountedTrackerLogPeriod_ = FromProto<TDuration>(schedulerJobSpecExt.job_proxy_ref_counted_tracker_log_period());

    if (ResourceController) {
        ResourceController->SetCpuShare(CpuLimit_);
    }

    InputNodeDirectory_ = New<NNodeTrackerClient::TNodeDirectory>();
    InputNodeDirectory_->MergeFrom(schedulerJobSpecExt.input_node_directory());

    HeartbeatExecutor_ = New<TPeriodicExecutor>(
        JobThread_->GetInvoker(),
        BIND(&TJobProxy::SendHeartbeat, MakeWeak(this)),
        Config_->HeartbeatPeriod);

    auto jobEnvironmentConfig = ConvertTo<TJobEnvironmentConfigPtr>(Config_->JobEnvironment);
    MemoryWatchdogExecutor_ = New<TPeriodicExecutor>(
        JobThread_->GetInvoker(),
        BIND(&TJobProxy::CheckMemoryUsage, MakeWeak(this)),
        jobEnvironmentConfig->MemoryWatchdogPeriod);

    if (schedulerJobSpecExt.has_user_job_spec()) {
        auto& userJobSpec = schedulerJobSpecExt.user_job_spec();
        JobProxyMemoryReserve_ -= userJobSpec.memory_reserve();
        LOG_DEBUG("Adjusting job proxy memory limit (JobProxyMemoryReserve: %v, UserJobMemoryReserve: %v)",
            JobProxyMemoryReserve_,
            userJobSpec.memory_reserve());
        Job_ = CreateUserJob(
            this,
            userJobSpec,
            JobId_,
            std::make_unique<TUserJobIO>(this));
    } else {
        Job_ = CreateBuiltinJob();
    }

    Job_->Initialize();

    MemoryWatchdogExecutor_->Start();
    HeartbeatExecutor_->Start();

    return Job_->Run();
}

void TJobProxy::ReportResult(
    const TJobResult& result,
    const TYsonString& statistics,
    TInstant startTime,
    TInstant finishTime)
{
    if (!SupervisorProxy_) {
        LOG_ERROR("Supervisor channel is not available");
        Exit(EJobProxyExitCode::ResultReportFailed);
    }

    auto req = SupervisorProxy_->OnJobFinished();
    ToProto(req->mutable_job_id(), JobId_);
    *req->mutable_result() = result;
    req->set_statistics(statistics.GetData());
    req->set_start_time(ToProto<i64>(startTime));
    req->set_finish_time(ToProto<i64>(finishTime));

    auto rspOrError = req->Invoke().Get();
    if (!rspOrError.IsOK()) {
        LOG_ERROR(rspOrError, "Failed to report job result");
        Exit(EJobProxyExitCode::ResultReportFailed);
    }
}

TStatistics TJobProxy::GetStatistics() const
{
    auto statistics = Job_ ? Job_->GetStatistics() : TStatistics();

    if (ResourceController) {
        try {
            auto cpuStatistics = ResourceController->GetCpuStatistics();
            statistics.AddSample("/job_proxy/cpu", cpuStatistics);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Unable to get cpu statistics from resource controller");
        }

        try {
            auto blockIOStatistics = ResourceController->GetBlockIOStatistics();
            statistics.AddSample("/job_proxy/block_io", blockIOStatistics);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Unable to get block IO statistics from resource controller");
        }
    }

    if (JobProxyMaxMemoryUsage_ > 0) {
        statistics.AddSample("/job_proxy/max_memory", JobProxyMaxMemoryUsage_);
    }

    if (JobProxyMemoryReserve_ > 0) {
        statistics.AddSample("/job_proxy/memory_reserve", JobProxyMemoryReserve_);
    }

    statistics.SetTimestamp(TInstant::Now());

    return statistics;
}

IResourceControllerPtr TJobProxy::GetResourceController() const
{
    return ResourceController;
}

TJobProxyConfigPtr TJobProxy::GetConfig() const
{
    return Config_;
}

const TOperationId& TJobProxy::GetOperationId() const
{
    return OperationId_;
}

const TJobId& TJobProxy::GetJobId() const
{
    return JobId_;
}

const IJobSpecHelperPtr& TJobProxy::GetJobSpecHelper() const
{
    YCHECK(JobSpecHelper_);
    return JobSpecHelper_;
}

void TJobProxy::UpdateResourceUsage(i64 memoryReserve)
{
    // Fire-and-forget.
    auto req = SupervisorProxy_->UpdateResourceUsage();
    ToProto(req->mutable_job_id(), JobId_);
    auto* resourceUsage = req->mutable_resource_usage();
    resourceUsage->set_cpu(CpuLimit_);
    resourceUsage->set_network(NetworkUsage_);
    resourceUsage->set_memory(memoryReserve);
    req->Invoke().Subscribe(BIND(&TJobProxy::OnResourcesUpdated, MakeWeak(this), memoryReserve));
}

void TJobProxy::SetUserJobMemoryUsage(i64 memoryUsage)
{
    UserJobCurrentMemoryUsage_ = memoryUsage;
}

void TJobProxy::OnResourcesUpdated(i64 memoryReserve, const TError& error)
{
    if (!error.IsOK()) {
        LOG_ERROR(error, "Failed to update resource usage");
        Exit(EJobProxyExitCode::ResourcesUpdateFailed);
    }

    if (ApprovedMemoryReserve_ < memoryReserve) {
        LOG_DEBUG("Successfully updated resource usage (MemoryReserve: %v)", memoryReserve);
        ApprovedMemoryReserve_ = memoryReserve;
    }
}

void TJobProxy::ReleaseNetwork()
{
    LOG_DEBUG("Releasing network");
    NetworkUsage_ = 0;
    UpdateResourceUsage(ApprovedMemoryReserve_);
}

void TJobProxy::OnPrepared()
{
    LOG_DEBUG("Job prepared");

    auto req = SupervisorProxy_->OnJobPrepared();
    ToProto(req->mutable_job_id(), JobId_);
    req->Invoke();
}

NApi::INativeClientPtr TJobProxy::GetClient() const
{
    return Client_;
}

IBlockCachePtr TJobProxy::GetBlockCache() const
{
    return GetNullBlockCache();
}

TNodeDirectoryPtr TJobProxy::GetInputNodeDirectory() const
{
    return InputNodeDirectory_;
}

const NNodeTrackerClient::TNodeDescriptor& TJobProxy::LocalDescriptor() const
{
    return LocalDescriptor_;
}

void TJobProxy::CheckMemoryUsage()
{
    i64 jobProxyMemoryUsage = GetProcessRss();
    JobProxyMaxMemoryUsage_ = std::max(JobProxyMaxMemoryUsage_.load(), jobProxyMemoryUsage);

    LOG_DEBUG("Job proxy memory check (JobProxyMemoryUsage: %v, JobProxyMaxMemoryUsage: %v, JobProxyMemoryReserve: %v, UserJobCurrentMemoryUsage: %v)",
        jobProxyMemoryUsage,
        JobProxyMaxMemoryUsage_.load(),
        JobProxyMemoryReserve_,
        UserJobCurrentMemoryUsage_.load());

    LOG_DEBUG("LFAlloc counters (LargeBlocks: %v, SmallBlocks: %v, System: %v, Used: %v, Mmapped: %v)",
        NLFAlloc::GetCurrentLargeBlocks(),
        NLFAlloc::GetCurrentSmallBlocks(),
        NLFAlloc::GetCurrentSystem(),
        NLFAlloc::GetCurrentUsed(),
        NLFAlloc::GetCurrentMmapped());

    if (JobProxyMaxMemoryUsage_.load() > JobProxyMemoryReserve_) {
        if (TInstant::Now() - LastRefCountedTrackerLogTime_ > RefCountedTrackerLogPeriod_) {
            LOG_WARNING("Job proxy used more memory than estimated "
                "(JobProxyMaxMemoryUsage: %v, JobProxyMemoryReserve: %v, RefCountedTracker: %v)",
                JobProxyMaxMemoryUsage_.load(),
                JobProxyMemoryReserve_,
                TRefCountedTracker::Get()->GetDebugInfo(2 /* sortByColumn */));
            LastRefCountedTrackerLogTime_ = TInstant::Now();
        }
    }

    if (JobProxyMemoryOvercommitLimit_ && jobProxyMemoryUsage > JobProxyMemoryReserve_ + *JobProxyMemoryOvercommitLimit_) {
        LOG_FATAL("Job proxy exceeded the memory overcommit limit "
            "(JobProxyMemoryUsage: %v, JobProxyMemoryReserve: %v, MemoryOvercommitLimit: %v, RefCountedTracker: %v)",
            jobProxyMemoryUsage,
            JobProxyMemoryReserve_,
            JobProxyMemoryOvercommitLimit_,
            TRefCountedTracker::Get()->GetDebugInfo(2 /* sortByColumn */));
    }

    i64 totalMemoryUsage = UserJobCurrentMemoryUsage_ + jobProxyMemoryUsage;

    if (TotalMaxMemoryUsage_ < totalMemoryUsage) {
        LOG_DEBUG("Total memory usage increased (OldTotalMaxMemoryUsage: %v, NewTotalMaxMemoryUsage: %v)",
            TotalMaxMemoryUsage_,
            totalMemoryUsage);
        TotalMaxMemoryUsage_ = totalMemoryUsage;
        if (TotalMaxMemoryUsage_ > ApprovedMemoryReserve_) {
            LOG_ERROR("Total memory usage exceeded the limit approved by the node "
                "(TotalMaxMemoryUsage: %v, ApprovedMemoryReserve: %v, AheadMemoryReserve: %v)",
                TotalMaxMemoryUsage_,
                ApprovedMemoryReserve_.load(),
                Config_->AheadMemoryReserve);
            // TODO(psushin): first improve memory estimates with data weights.
            // Exit(EJobProxyExitCode::ResourceOverdraft);
        }
    }
    i64 memoryReserve = TotalMaxMemoryUsage_ + Config_->AheadMemoryReserve;
    if (ApprovedMemoryReserve_ < memoryReserve) {
        LOG_DEBUG("Asking node for resource usage update (MemoryReserve: %v)", memoryReserve);
        UpdateResourceUsage(memoryReserve);
    }
}

void TJobProxy::EnsureStderrResult(TJobResult* jobResult)
{
    const auto& schedulerJobSpecExt = GetJobSpecHelper()->GetSchedulerJobSpecExt();
    const auto& userJobSpec = schedulerJobSpecExt.user_job_spec();

    auto* schedulerJobResultExt = jobResult->MutableExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    // If we were provided with stderr_table_spec we are expected to write stderr and provide some results.
    if (userJobSpec.has_stderr_table_spec() && !schedulerJobResultExt->has_stderr_table_boundary_keys()) {
        // If error occurred during user job initialization, stderr blob table writer may not have been created at all.
        LOG_WARNING("Stderr table boundary keys are absent");
        auto* stderrBoundaryKeys = schedulerJobResultExt->mutable_stderr_table_boundary_keys();
        stderrBoundaryKeys->set_sorted(true);
    }
}

void TJobProxy::Exit(EJobProxyExitCode exitCode)
{
    if (Job_) {
        Job_->Cleanup();
    }

    NLogging::TLogManager::Get()->Shutdown();
    _exit(static_cast<int>(exitCode));
}

NLogging::TLogger TJobProxy::GetLogger() const
{
    return Logger;
}

IInvokerPtr TJobProxy::GetControlInvoker() const
{
    return ControlThread_->GetInvoker();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
