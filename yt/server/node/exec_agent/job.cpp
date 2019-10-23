#include "job.h"
#include "private.h"
#include "slot.h"
#include "slot_manager.h"

#include <yt/server/lib/exec_agent/config.h>

#include <yt/server/node/cell_node/bootstrap.h>
#include <yt/server/node/cell_node/config.h>

#include <yt/server/lib/containers/public.h>

#include <yt/server/node/data_node/artifact.h>
#include <yt/server/node/data_node/chunk.h>
#include <yt/server/node/data_node/chunk_cache.h>
#include <yt/server/node/data_node/location.h>
#include <yt/server/node/data_node/master_connector.h>
#include <yt/server/node/data_node/volume_manager.h>

#include <yt/server/node/job_agent/job.h>
#include <yt/server/node/job_agent/gpu_manager.h>
#include <yt/server/node/job_agent/statistics_reporter.h>

#include <yt/server/lib/scheduler/config.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/ytlib/chunk_client/data_slice_descriptor.h>
#include <yt/ytlib/chunk_client/data_source.h>
#include <yt/ytlib/chunk_client/traffic_meter.h>
#include <yt/ytlib/chunk_client/dispatcher.h>

#include <yt/ytlib/job_prober_client/public.h>
#include <yt/ytlib/job_prober_client/job_probe.h>
#include <yt/ytlib/job_prober_client/job_prober_service_proxy.h>

#include <yt/ytlib/job_proxy/public.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/ytlib/job_tracker_client/statistics.h>

#include <yt/ytlib/node_tracker_client/node_directory_builder.h>

#include <yt/core/concurrency/thread_affinity.h>
#include <yt/core/concurrency/delayed_executor.h>

#include <yt/core/actions/cancelable_context.h>

#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/proc.h>
#include <yt/core/misc/statistics.h>

#include <yt/core/rpc/dispatcher.h>

namespace NYT::NExecAgent {

using namespace NRpc;
using namespace NJobProxy;
using namespace NYTree;
using namespace NYson;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTableClient;
using namespace NFileClient;
using namespace NCellNode;
using namespace NDataNode;
using namespace NCellNode;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJobAgent;
using namespace NJobProberClient;
using namespace NJobTrackerClient;
using namespace NJobTrackerClient::NProto;
using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NConcurrency;
using namespace NApi;
using namespace NCoreDump;

using NNodeTrackerClient::TNodeDirectory;
using NChunkClient::TDataSliceDescriptor;

////////////////////////////////////////////////////////////////////////////////

class TJob
    : public NJobAgent::IJob
{
public:
    DEFINE_SIGNAL(void(const TNodeResources&), ResourcesUpdated);
    DEFINE_SIGNAL(void(), PortsReleased);
    DEFINE_SIGNAL(void(), JobFinished);

public:
    TJob(
        TJobId jobId,
        TOperationId operationId,
        const TNodeResources& resourceUsage,
        TJobSpec&& jobSpec,
        NCellNode::TBootstrap* bootstrap)
        : Id_(jobId)
        , OperationId_(operationId)
        , Bootstrap_(bootstrap)
        , Config_(Bootstrap_->GetConfig()->ExecAgent)
        , Invoker_(Bootstrap_->GetControlInvoker())
        , StartTime_(TInstant::Now())
        , TrafficMeter_(New<TTrafficMeter>(
            Bootstrap_->GetMasterConnector()->GetLocalDescriptor().GetDataCenter()))
        , JobSpec_(std::move(jobSpec))
        , ResourceUsage_(resourceUsage)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        TrafficMeter_->Start();

        const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        AbortJobIfAccountLimitExceeded_ = schedulerJobSpecExt.abort_job_if_account_limit_exceeded();

        Logger.AddTag("JobId: %v, OperationId: %v, JobType: %v",
            Id_,
            OperationId_,
            GetType());

        JobEvents_.emplace_back(JobState_, JobPhase_);
        ReportStatistics(MakeDefaultJobStatistics());
    }

    virtual void Start() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (JobPhase_ != EJobPhase::Created) {
            YT_LOG_DEBUG("Cannot start job, unexpected job phase (JobState: %v, JobPhase: %v)",
                JobState_,
                JobPhase_);
            return;
        }

        GuardedAction([&] () {
            SetJobState(EJobState::Running);

            auto now = TInstant::Now();
            PrepareTime_ = now;

            YT_LOG_INFO("Starting job");

            InitializeArtifacts();

            i64 diskSpaceLimit = Config_->MinRequiredDiskSpace;

            const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
            if (schedulerJobSpecExt.has_user_job_spec()) {
                const auto& userJobSpec = schedulerJobSpecExt.user_job_spec();
                if (userJobSpec.has_disk_space_limit()) {
                    diskSpaceLimit = userJobSpec.disk_space_limit();
                }

                if (userJobSpec.has_prepare_time_limit()) {
                    auto prepareTimeLimit = FromProto<TDuration>(userJobSpec.prepare_time_limit());
                    TDelayedExecutor::Submit(
                        BIND(&TJob::OnJobPreparationTimeout, MakeWeak(this), prepareTimeLimit)
                            .Via(Invoker_),
                        prepareTimeLimit);
                }
            }

            if (!Config_->JobController->TestGpuResource) {
                for (int i = 0; i < GetResourceUsage().gpu(); ++i) {
                    GpuSlots_.emplace_back(Bootstrap_->GetGpuManager()->AcquireGpuSlot());

                    TGpuStatistics statistics;
                    statistics.LastUpdateTime = now;
                    GpuStatistics_.emplace_back(std::move(statistics));
                }

                if (schedulerJobSpecExt.has_user_job_spec()) {
                    const auto& userJobSpec = schedulerJobSpecExt.user_job_spec();
                    if (userJobSpec.has_cuda_toolkit_version()) {
                        Bootstrap_->GetGpuManager()->VerifyToolkitDriverVersion(userJobSpec.cuda_toolkit_version());
                    }
                }
            }

            auto slotManager = Bootstrap_->GetExecSlotManager();
            Slot_ = slotManager->AcquireSlot(diskSpaceLimit);

            SetJobPhase(EJobPhase::PreparingNodeDirectory);
            // This is a heavy part of preparation, offload it to compression invoker.
            BIND(&TJob::PrepareNodeDirectory, MakeWeak(this))
                .AsyncVia(NRpc::TDispatcher::Get()->GetCompressionPoolInvoker())
                .Run()
                .Subscribe(
                    BIND(&TJob::OnNodeDirectoryPrepared, MakeWeak(this))
                        .Via(Invoker_));
        });
    }

    virtual void Abort(const TError& error) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_LOG_INFO(error, "Job abort requested (Phase: %v)",
            JobPhase_);

        auto startAbortion = [&] () {
            SetJobStatePhase(EJobState::Aborting, EJobPhase::WaitingAbort);
            DoSetResult(error);
            TDelayedExecutor::Submit(BIND(&TJob::OnJobAbortionTimeout, MakeStrong(this))
                .Via(Invoker_), Config_->JobAbortionTimeout);
        };

        switch (JobPhase_) {
            case EJobPhase::Created:
            case EJobPhase::DownloadingArtifacts:
            case EJobPhase::Running:
                startAbortion();
                ArtifactsFuture_.Cancel();

                // Do the actual cleanup asynchronously.
                BIND(&TJob::Cleanup, MakeStrong(this))
                    .Via(Bootstrap_->GetControlInvoker())
                    .Run();

                break;

            case EJobPhase::PreparingNodeDirectory:
            case EJobPhase::PreparingSandboxDirectories:
            case EJobPhase::PreparingArtifacts:
            case EJobPhase::PreparingRootVolume:
            case EJobPhase::PreparingProxy:
                // Wait for the next event handler to complete the abortion.
                startAbortion();
                Slot_->CancelPreparation();
                break;

            default:
                YT_LOG_DEBUG("Cannot abort job (JobState: %v, JobPhase: %v)",
                    JobState_,
                    JobPhase_);
                break;
        }
    }

    virtual void OnJobPrepared() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        GuardedAction([&] {
            YT_LOG_INFO("Job prepared");

            ValidateJobPhase(EJobPhase::PreparingProxy);
            SetJobPhase(EJobPhase::Running);
        });
    }

    virtual void SetResult(const TJobResult& jobResult) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        GuardedAction([&] () {
            SetJobPhase(EJobPhase::FinalizingProxy);
            DoSetResult(jobResult);
        });
    }

    virtual TJobId GetId() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Id_;
    }

    virtual TOperationId GetOperationId() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return OperationId_;
    }

    virtual EJobType GetType() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return EJobType(JobSpec_.type());
    }

    virtual const TJobSpec& GetSpec() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return JobSpec_;
    }

    virtual int GetPortCount() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        if (schedulerJobSpecExt.has_user_job_spec()) {
            return schedulerJobSpecExt.user_job_spec().port_count();
        }

        return 0;
    }

    virtual void SetPorts(const std::vector<int>& ports) override
    {
        Ports_ = ports;
    }

    virtual EJobState GetState() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return JobState_;
    }

    virtual TInstant GetStartTime() const override
    {
        return StartTime_;
    }

    virtual std::optional<TDuration> GetPrepareDuration() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!PrepareTime_) {
            return std::nullopt;
        } else if (!ExecTime_) {
            return TInstant::Now() - *PrepareTime_;
        } else {
            return *ExecTime_ - *PrepareTime_;
        }
    }

    virtual std::optional<TDuration> GetPrepareRootFSDuration() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!StartPrepareVolumeTime_) {
            return std::nullopt;
        } else if (!FinishPrepareVolumeTime_) {
            return TInstant::Now() - *StartPrepareVolumeTime_;
        } else {
            return *FinishPrepareVolumeTime_ - *StartPrepareVolumeTime_;
        }
    }

    virtual std::optional<TDuration> GetDownloadDuration() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!PrepareTime_) {
            return std::nullopt;
        } else if (!CopyTime_) {
            return TInstant::Now() - *PrepareTime_;
        } else {
            return *CopyTime_ - *PrepareTime_;
        }
    }

    virtual std::optional<TDuration> GetExecDuration() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!ExecTime_) {
            return std::nullopt;
        } else if (!FinishTime_) {
            return TInstant::Now() - *ExecTime_;
        } else {
            return *FinishTime_ - *ExecTime_;
        }
    }

    virtual EJobPhase GetPhase() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return JobPhase_;
    }

    virtual TNodeResources GetResourceUsage() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return ResourceUsage_;
    }

    virtual std::vector<int> GetPorts() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Ports_;
    }

    virtual TJobResult GetResult() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return *JobResult_;
    }

    virtual double GetProgress() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Progress_;
    }

    virtual void SetResourceUsage(const TNodeResources& newUsage) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (JobPhase_ == EJobPhase::Running) {
            auto delta = newUsage - ResourceUsage_;
            ResourceUsage_ = newUsage;
            ResourcesUpdated_.Fire(delta);
        }
    }

    virtual void SetProgress(double progress) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (JobPhase_ == EJobPhase::Running) {
            Progress_ = progress;
        }
    }

    virtual ui64 GetStderrSize() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        return StderrSize_;
    }

    virtual void SetStderrSize(ui64 value) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (StderrSize_ != value) {
            StderrSize_ = value;
            ReportStatistics(MakeDefaultJobStatistics()
                .StderrSize(StderrSize_));
        }
    }

    virtual void SetStderr(const TString& value) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        Stderr_ = value;
    }

    virtual void SetFailContext(const TString& value) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        FailContext_ = value;
    }

    virtual void SetProfile(const TJobProfile& value) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        Profile_ = value;
    }

    virtual void SetCoreInfos(TCoreInfos value) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        CoreInfos_ = std::move(value);
    }

    virtual TYsonString GetStatistics() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Statistics_;
    }

    virtual TInstant GetStatisticsLastSendTime() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return StatisticsLastSendTime_;
    }

    virtual void ResetStatisticsLastSendTime() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        StatisticsLastSendTime_ = TInstant::Now();
    }

    virtual void SetStatistics(const TYsonString& statistics) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (JobPhase_ == EJobPhase::Running || JobPhase_ == EJobPhase::FinalizingProxy) {
            if (!GpuSlots_.empty()) {
                auto enrichedStatistics = EnrichStatisticsWithGpuInfo(statistics);
                Statistics_ = enrichedStatistics;
            } else {
                Statistics_ = statistics;
            }
            ReportStatistics(MakeDefaultJobStatistics()
                .Statistics(Statistics_));
        }
    }

    virtual std::vector<TChunkId> DumpInputContext() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        ValidateJobRunning();

        try {
            return GetJobProbeOrThrow()->DumpInputContext();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error requesting input contexts dump from job proxy")
                << ex;
        }
    }

    virtual TString GetStderr() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (Stderr_) {
            return *Stderr_;
        }

        ValidateJobRunning();

        try {
            return GetJobProbeOrThrow()->GetStderr();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error requesting stderr from job proxy")
                << ex;
        }
    }

    virtual std::optional<TString> GetFailContext() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return FailContext_;
    }

    std::optional<TJobProfile> GetProfile()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Profile_;
    }

    const TCoreInfos& GetCoreInfos()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return CoreInfos_;
    }

    virtual TYsonString StraceJob() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        try {
            return GetJobProbeOrThrow()->StraceJob();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error requesting strace dump from job proxy")
                << ex;
        }
    }

    virtual void SignalJob(const TString& signalName) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        ValidateJobRunning();

        Signaled_ = true;

        try {
            GetJobProbeOrThrow()->SignalJob(signalName);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error sending signal to job proxy")
                << ex;
        }
    }

    virtual TYsonString PollJobShell(const TYsonString& parameters) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        try {
            return GetJobProbeOrThrow()->PollJobShell(parameters);
        } catch (const TErrorException& ex) {
            // The following code changes error code for more user-friendly
            // diagnostics in interactive shell.
            if (ex.Error().FindMatching(NRpc::EErrorCode::TransportError)) {
                THROW_ERROR_EXCEPTION(NExecAgent::EErrorCode::JobProxyConnectionFailed,
                    "No connection to job proxy")
                    << ex;
            }
            THROW_ERROR_EXCEPTION("Error polling job shell")
                << ex;
        }
    }

    virtual void ReportStatistics(TJobStatistics&& statistics) override
    {
        Bootstrap_->GetStatisticsReporter()->ReportStatistics(
            std::move(statistics)
                .OperationId(GetOperationId())
                .JobId(GetId()));
    }

    virtual void ReportSpec() override
    {
        ReportStatistics(MakeDefaultJobStatistics()
            .Spec(JobSpec_));
    }

    virtual void ReportStderr() override
    {
        ReportStatistics(TJobStatistics()
            .Stderr(GetStderr()));
    }

    virtual void ReportFailContext() override
    {
        if (auto failContext = GetFailContext()) {
            ReportStatistics(TJobStatistics()
                .FailContext(*failContext));
        }
    }

    virtual void ReportProfile() override
    {
        if (auto profile = GetProfile()) {
            ReportStatistics(TJobStatistics()
                .Profile(*profile));
        }
    }

    virtual void Interrupt() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (JobPhase_ < EJobPhase::Running) {
            Abort(TError(NJobProxy::EErrorCode::JobNotPrepared, "Interrupting job that has not started yet"));
            return;
        } else if (JobPhase_ > EJobPhase::Running) {
            // We're done with this job, no need to interrupt.
            return;
        }

        try {
            GetJobProbeOrThrow()->Interrupt();
        } catch (const std::exception& ex) {
            auto error = TError("Error interrupting job on job proxy")
                << ex;

            if (error.FindMatching(NJobProxy::EErrorCode::JobNotPrepared)) {
                Abort(error);
            } else {
                THROW_ERROR error;
            }
        }
    }

    virtual void Fail() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        ValidateJobRunning();

        try {
            GetJobProbeOrThrow()->Fail();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error failing job on job proxy")
                    << ex;
        }
    }

    virtual bool GetStored() const override
    {
        return Stored_;
    }

    virtual void SetStored(bool value) override
    {
        Stored_ = value;
    }

private:
    const TJobId Id_;
    const TOperationId OperationId_;
    NCellNode::TBootstrap* const Bootstrap_;

    const TExecAgentConfigPtr Config_;
    const IInvokerPtr Invoker_;
    const TInstant StartTime_;
    const TTrafficMeterPtr TrafficMeter_;

    TJobSpec JobSpec_;

    bool AbortJobIfAccountLimitExceeded_;

    // Used to terminate artifacts downloading in case of cancelation.
    TFuture<void> ArtifactsFuture_ = VoidFuture;

    double Progress_ = 0.0;
    ui64 StderrSize_ = 0;

    std::optional<TString> Stderr_;
    std::optional<TString> FailContext_;
    std::optional<TJobProfile> Profile_;
    TCoreInfos CoreInfos_;

    TYsonString Statistics_ = TYsonString("{}");
    TInstant StatisticsLastSendTime_ = TInstant::Now();

    bool Signaled_ = false;

    std::optional<TJobResult> JobResult_;

    std::optional<TInstant> PrepareTime_;
    std::optional<TInstant> CopyTime_;
    std::optional<TInstant> StartPrepareVolumeTime_;
    std::optional<TInstant> FinishPrepareVolumeTime_;
    std::optional<TInstant> ExecTime_;
    std::optional<TInstant> FinishTime_;

    std::vector<TGpuManager::TGpuSlotPtr> GpuSlots_;
    std::vector<TGpuStatistics> GpuStatistics_;

    ISlotPtr Slot_;
    std::vector<TString> TmpfsPaths_;

    struct TArtifact
    {
        ESandboxKind SandboxKind;
        TString Name;
        bool Executable;
        bool BypassArtifactCache;
        TArtifactKey Key;
        NDataNode::IChunkPtr Chunk;
    };

    std::vector<TArtifact> Artifacts_;
    std::vector<TArtifactKey> LayerArtifactKeys_;

    IVolumePtr RootVolume_;

    TNodeResources ResourceUsage_;
    std::vector<int> Ports_;

    EJobState JobState_ = EJobState::Waiting;
    EJobPhase JobPhase_ = EJobPhase::Created;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    NLogging::TLogger Logger = ExecAgentLogger;

    TJobEvents JobEvents_;

    //! True if scheduler asked to store this job.
    bool Stored_ = false;

    TSpinLock JobProbeLock_;
    IJobProbePtr JobProbe_;

    // Helpers.

    template <class... U>
    void AddJobEvent(U&&... u)
    {
        JobEvents_.emplace_back(std::forward<U>(u)...);
        ReportStatistics(MakeDefaultJobStatistics());
    }

    void SetJobState(EJobState state)
    {
        JobState_ = state;
        AddJobEvent(state);
    }

    void SetJobPhase(EJobPhase phase)
    {
        JobPhase_ = phase;
        AddJobEvent(phase);
    }

    void SetJobStatePhase(EJobState state, EJobPhase phase)
    {
        JobState_ = state;
        JobPhase_ = phase;
        AddJobEvent(state, phase);
    }

    void ValidateJobRunning() const
    {
        if (JobPhase_ != EJobPhase::Running) {
            THROW_ERROR_EXCEPTION(NJobProberClient::EErrorCode::JobIsNotRunning, "Job %v is not running", Id_)
                << TErrorAttribute("job_state", JobState_)
                << TErrorAttribute("job_phase", JobPhase_);
        }
    }

    void DoSetResult(const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        TJobResult jobResult;
        ToProto(jobResult.mutable_error(), error);
        DoSetResult(std::move(jobResult));
    }

    void DoSetResult(TJobResult jobResult)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        if (JobResult_) {
            auto error = FromProto<TError>(JobResult_->error());
            if (!error.IsOK()) {
                return;
            }
        }

        {
            auto error = FromProto<TError>(jobResult.error());
            ToProto(jobResult.mutable_error(), error.Truncate());
        }

        JobResult_ = jobResult;
        FinishTime_ = TInstant::Now();
    }

    bool HandleFinishingPhase()
    {
        switch (JobPhase_) {
            case EJobPhase::WaitingAbort:
                Cleanup();
                return true;

            case EJobPhase::Cleanup:
            case EJobPhase::Finished:
                return true;

            case EJobPhase::Created:
                YT_VERIFY(JobState_ == EJobState::Waiting);
                return false;

            default:
                YT_VERIFY(JobState_ == EJobState::Running);
                return false;
        }
    }

    void ValidateJobPhase(EJobPhase expectedPhase) const
    {
        if (JobPhase_ != expectedPhase) {
            THROW_ERROR_EXCEPTION("Unexpected job phase")
                << TErrorAttribute("expected_phase", expectedPhase)
                << TErrorAttribute("actual_phase", JobPhase_);
        }
    }

    // Event handlers.
    void OnNodeDirectoryPrepared(const TError& error)
    {
        GuardedAction([&] {
            ValidateJobPhase(EJobPhase::PreparingNodeDirectory);
            THROW_ERROR_EXCEPTION_IF_FAILED(error,
                NExecAgent::EErrorCode::NodeDirectoryPreparationFailed,
                "Failed to prepare job node directory");

            SetJobPhase(EJobPhase::DownloadingArtifacts);
            auto artifactsFuture = DownloadArtifacts();
            artifactsFuture.Subscribe(
                BIND(&TJob::OnArtifactsDownloaded, MakeWeak(this))
                    .Via(Invoker_));
            ArtifactsFuture_ = artifactsFuture.As<void>();
        });
    }

    void OnArtifactsDownloaded(const TErrorOr<std::vector<NDataNode::IChunkPtr>>& errorOrArtifacts)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        GuardedAction([&] {
            ValidateJobPhase(EJobPhase::DownloadingArtifacts);
            THROW_ERROR_EXCEPTION_IF_FAILED(errorOrArtifacts, "Failed to download artifacts");

            YT_LOG_INFO("Artifacts downloaded");

            const auto& chunks = errorOrArtifacts.Value();
            for (size_t index = 0; index < Artifacts_.size(); ++index) {
                Artifacts_[index].Chunk = chunks[index];
            }

            CopyTime_ = TInstant::Now();
            SetJobPhase(EJobPhase::PreparingSandboxDirectories);
            BIND(&TJob::PrepareSandboxDirectories, MakeStrong(this))
                .AsyncVia(Invoker_)
                .Run()
                .Subscribe(BIND(
                    &TJob::OnSandboxDirectoriesPrepared,
                    MakeWeak(this))
                .Via(Invoker_));
        });
    }

    void OnSandboxDirectoriesPrepared(const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        GuardedAction([&] {
            ValidateJobPhase(EJobPhase::PreparingSandboxDirectories);
            THROW_ERROR_EXCEPTION_IF_FAILED(error, "Failed to prepare sandbox directories");

            SetJobPhase(EJobPhase::PreparingArtifacts);
            BIND(&TJob::PrepareArtifacts, MakeWeak(this))
                .AsyncVia(Invoker_)
                .Run()
                .Subscribe(BIND(
                    &TJob::OnArtifactsPrepared,
                    MakeWeak(this))
                .Via(Invoker_));
        });
    }

    void OnArtifactsPrepared(const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        GuardedAction([&] {
            ValidateJobPhase(EJobPhase::PreparingArtifacts);
            THROW_ERROR_EXCEPTION_IF_FAILED(error, "Failed to prepare artifacts");

            YT_LOG_INFO("Artifacts prepared");
            if (LayerArtifactKeys_.empty()) {
                RunJobProxy();
            } else {
                StartPrepareVolumeTime_ = TInstant::Now();
                SetJobPhase(EJobPhase::PreparingRootVolume);
                YT_LOG_INFO("Preparing root volume (LayerCount: %v)", LayerArtifactKeys_.size());
                Slot_->PrepareRootVolume(LayerArtifactKeys_)
                    .Subscribe(BIND(
                        &TJob::OnVolumePrepared,
                        MakeWeak(this))
                    .Via(Invoker_));
            }
        });
    }

    void OnVolumePrepared(const TErrorOr<IVolumePtr>& volumeOrError)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        FinishPrepareVolumeTime_ = TInstant::Now();

        GuardedAction([&] {
            ValidateJobPhase(EJobPhase::PreparingRootVolume);
            if (!volumeOrError.IsOK()) {
                THROW_ERROR TError(EErrorCode::RootVolumePreparationFailed, "Failed to prepare artifacts")
                    << volumeOrError;
            }

            RootVolume_ = volumeOrError.Value();

            const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
            if (schedulerJobSpecExt.has_user_job_spec()) {
                const auto& userJobSpec = schedulerJobSpecExt.user_job_spec();
                if (userJobSpec.enable_setup_commands()) {
                    SetJobPhase(EJobPhase::RunningSetupCommands);
                    YT_LOG_INFO("Running setup commands");

                    // Even though #RunSetupCommands returns future, we still need to pass it through invoker
                    // since Porto API is used and can cause context switch.
                    BIND(&TJob::RunSetupCommands, MakeStrong(this))
                        .AsyncVia(Invoker_)
                        .Run()
                        .Subscribe(BIND(
                            &TJob::OnSetupCommandsFinished,
                            MakeWeak(this))
                            .Via(Invoker_));
                    return;
                } else {
                    YT_LOG_INFO("Setup commands disabled, running job proxy");
                }
            }
            RunJobProxy();
        });
    }

    void OnSetupCommandsFinished(const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        GuardedAction([&] {
            ValidateJobPhase(EJobPhase::RunningSetupCommands);
            if (!error.IsOK()) {
                THROW_ERROR_EXCEPTION(EErrorCode::SetupCommandFailed, "Failed to run setup commands")
                    << error;
            }

            RunJobProxy();
        });
    }

    void RunJobProxy()
    {
        ExecTime_ = TInstant::Now();
        SetJobPhase(EJobPhase::PreparingProxy);
        InitializeJobProbe();

        BIND(
            &ISlot::RunJobProxy,
            Slot_,
            CreateConfig(),
            Id_,
            OperationId_)
        .AsyncVia(Invoker_)
        .Run()
        .Subscribe(BIND(
            &TJob::OnJobProxyFinished,
            MakeWeak(this))
        .Via(Invoker_));

        TDelayedExecutor::Submit(BIND(&TJob::OnJobProxyPreparationTimeout, MakeStrong(this))
           .Via(Invoker_), Config_->JobProxyPreparationTimeout);
    }

    void OnJobProxyPreparationTimeout()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        GuardedAction([&] {
            if (JobPhase_ == EJobPhase::PreparingProxy) {
                THROW_ERROR_EXCEPTION(
                    EErrorCode::JobProxyPreparationTimeout,
                    "Failed to prepare job proxy within timeout, aborting job");
            }
        });
    }

    void OnJobPreparationTimeout(TDuration prepareTimeLimit)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (JobPhase_ < EJobPhase::Running) {
            auto error = TError(
                EErrorCode::JobPreparationTimeout,
                "Failed to prepare job within timeout")
                << TErrorAttribute("prepare_time_limit", prepareTimeLimit)
                << TErrorAttribute("job_start_time", StartTime_);
            Abort(error);
        }
    }

    void OnJobAbortionTimeout()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (JobState_ == EJobState::Aborting) {
            auto error = TError("Failed to abort job %v within timeout", Id_)
                << TErrorAttribute("job_abortion_timeout", Config_->JobAbortionTimeout);
            Bootstrap_->GetExecSlotManager()->Disable(error);
        }
    }

    void OnJobProxyFinished(const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ResetJobProbe();

        if (HandleFinishingPhase()) {
            return;
        }

        YT_LOG_INFO("Job proxy finished");

        if (!error.IsOK()) {
            DoSetResult(TError(EErrorCode::JobProxyFailed, "Job proxy failed")
                << BuildJobProxyError(error));
        }

        Cleanup();
    }

    void GuardedAction(std::function<void()> action)
    {
        if (HandleFinishingPhase()) {
            return;
        }

        try {
            TForbidContextSwitchGuard contextSwitchGuard;
            action();
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Error preparing scheduler job");

            DoSetResult(ex);
            Cleanup();
        }
    }

    // Finalization.
    void Cleanup()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (JobPhase_ == EJobPhase::Cleanup || JobPhase_ == EJobPhase::Finished) {
            return;
        }

        YT_LOG_INFO("Cleaning up after scheduler job");

        FinishTime_ = TInstant::Now();
        SetJobPhase(EJobPhase::Cleanup);

        if (Slot_) {
            try {
                YT_LOG_DEBUG("Clean processes (SlotIndex: %v)", Slot_->GetSlotIndex());
                Slot_->CleanProcesses();
            } catch (const std::exception& ex) {
                // Errors during cleanup phase do not affect job outcome.
                YT_LOG_ERROR(ex, "Failed to clean processed (SlotIndex: %v)", Slot_->GetSlotIndex());
            }
        }

        // NodeDirectory can be really huge, we better offload its cleanup.
        // NB: do this after slot cleanup.
        WaitFor(BIND([this_ = MakeStrong(this), this] () {
            auto* schedulerJobSpecExt = JobSpec_.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
            schedulerJobSpecExt->clear_input_node_directory();
        })
            .AsyncVia(NRpc::TDispatcher::Get()->GetCompressionPoolInvoker())
            .Run())
            .ThrowOnError();

        YT_VERIFY(JobResult_);

        // Copy info from traffic meter to statistics.
        auto deserializedStatistics = ConvertTo<TStatistics>(Statistics_);
        FillTrafficStatistics(ExecAgentTrafficStatisticsPrefix, deserializedStatistics, TrafficMeter_);
        Statistics_ = ConvertToYsonString(deserializedStatistics);

        auto error = FromProto<TError>(JobResult_->error());

        if (!error.IsOK()) {
            // NB: it is required to report error that occurred in some place different
            // from OnJobFinished method.
            ReportStatistics(TJobStatistics().Error(error));
        }

        if (error.IsOK()) {
            SetJobState(EJobState::Completed);
        } else if (IsFatalError(error)) {
            error.Attributes().Set("fatal", true);
            ToProto(JobResult_->mutable_error(), error);
            SetJobState(EJobState::Failed);
        } else {
            auto abortReason = GetAbortReason(*JobResult_);
            if (abortReason) {
                error.Attributes().Set("abort_reason", abortReason);
                ToProto(JobResult_->mutable_error(), error);
                SetJobState(EJobState::Aborted);
            } else {
                SetJobState(EJobState::Failed);
            }
        }

        YT_LOG_INFO(error, "Setting final job state (JobState: %v)", GetState());
        JobFinished_.Fire();

        // Release resources.
        GpuSlots_.clear();
        GpuStatistics_.clear();

        auto oneUserSlotResources = ZeroNodeResources();
        oneUserSlotResources.set_user_slots(1);

        auto resourceDelta = ZeroNodeResources() - ResourceUsage_ + oneUserSlotResources;
        ResourceUsage_ = ZeroNodeResources();
        ResourcesUpdated_.Fire(resourceDelta);
        PortsReleased_.Fire();

        if (Slot_) {
            try {
                YT_LOG_DEBUG("Clean sandbox (SlotIndex: %v)", Slot_->GetSlotIndex());
                Slot_->CleanSandbox();
            } catch (const std::exception& ex) {
                // Errors during cleanup phase do not affect job outcome.
                YT_LOG_ERROR(ex, "Failed to clean sandbox (SlotIndex: %v)", Slot_->GetSlotIndex());
            }
            Bootstrap_->GetExecSlotManager()->ReleaseSlot(Slot_->GetSlotIndex());
        }

        ResourcesUpdated_.Fire(-oneUserSlotResources);

        SetJobPhase(EJobPhase::Finished);

        YT_LOG_INFO("Job finalized (JobState: %v)", GetState());

        Bootstrap_->GetExecSlotManager()->OnJobFinished(GetState());
    }

    // Preparation.
    void PrepareNodeDirectory()
    {
        auto* schedulerJobSpecExt = JobSpec_.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);

        if (schedulerJobSpecExt->has_input_node_directory()) {
            YT_LOG_INFO("Node directory is provided by scheduler");
            return;
        }

        YT_LOG_INFO("Started preparing node directory");

        const auto& nodeDirectory = Bootstrap_->GetNodeDirectory();

        for (int attempt = 1;; ++attempt) {
            if (JobPhase_ != EJobPhase::PreparingNodeDirectory) {
                break;
            }

            std::optional<TNodeId> unresolvedNodeId;

            auto validateNodeIds = [&] (
                const ::google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>& chunkSpecs,
                const TNodeDirectoryPtr& nodeDirectory)
            {
                for (const auto& chunkSpec : chunkSpecs) {
                    auto replicas = FromProto<TChunkReplicaList>(chunkSpec.replicas());
                    for (auto replica : replicas) {
                        auto nodeId = replica.GetNodeId();
                        const auto* descriptor = nodeDirectory->FindDescriptor(nodeId);
                        if (!descriptor) {
                            unresolvedNodeId = nodeId;
                            return;
                        }
                    }
                }
            };

            auto validateTableSpecs = [&] (const ::google::protobuf::RepeatedPtrField<TTableInputSpec>& tableSpecs) {
                for (const auto& tableSpec : tableSpecs) {
                    validateNodeIds(tableSpec.chunk_specs(), nodeDirectory);
                }
            };

            validateTableSpecs(schedulerJobSpecExt->input_table_specs());
            validateTableSpecs(schedulerJobSpecExt->foreign_input_table_specs());

            // NB: No need to add these descriptors to the input node directory.
            for (const auto& artifact : Artifacts_) {
                validateNodeIds(artifact.Key.chunk_specs(), nodeDirectory);
            }

            for (const auto& artifactKey : LayerArtifactKeys_) {
                validateNodeIds(artifactKey.chunk_specs(), nodeDirectory);
            }

            if (!unresolvedNodeId) {
                break;
            }

            if (attempt >= Config_->NodeDirectoryPrepareRetryCount) {
                YT_LOG_WARNING("Some node ids were not resolved, skipping corresponding replicas (UnresolvedNodeId: %v)",
                    *unresolvedNodeId);
                break;
            }

            YT_LOG_INFO("Unresolved node id found in job spec; backing off and retrying (NodeId: %v, Attempt: %v)",
                *unresolvedNodeId,
                attempt);
            TDelayedExecutor::WaitForDuration(Config_->NodeDirectoryPrepareBackoffTime);
        }

        nodeDirectory->DumpTo(schedulerJobSpecExt->mutable_input_node_directory());

        YT_LOG_INFO("Finished preparing node directory");
    }

    TJobProxyConfigPtr CreateConfig()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto proxyConfig = Bootstrap_->BuildJobProxyConfig();
        proxyConfig->BusServer = Slot_->GetBusServerConfig();
        proxyConfig->TmpfsPaths = TmpfsPaths_;
        proxyConfig->SlotIndex = Slot_->GetSlotIndex();
        if (RootVolume_) {
            proxyConfig->RootPath = RootVolume_->GetPath();
            proxyConfig->Binds = Config_->RootFSBinds;
        }

        for (const auto& slot : GpuSlots_) {
            proxyConfig->GpuDevices.push_back(slot->GetDeviceName());
        }

        return proxyConfig;
    }

    void PrepareSandboxDirectories()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_LOG_INFO("Started preparing sandbox directories");

        TUserSandboxOptions options;

        const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);

        if (schedulerJobSpecExt.has_user_job_spec()) {
            const auto& userJobSpec = schedulerJobSpecExt.user_job_spec();
            for (const auto& tmpfsVolumeProto : userJobSpec.tmpfs_volumes()) {
                TTmpfsVolume tmpfsVolume;
                tmpfsVolume.Size = tmpfsVolumeProto.size();
                tmpfsVolume.Path = tmpfsVolumeProto.path();
                options.TmpfsVolumes.push_back(tmpfsVolume);
            }

            // COMPAT(ignat).
            if (options.TmpfsVolumes.empty() && userJobSpec.has_tmpfs_path()) {
                TTmpfsVolume tmpfsVolume;
                tmpfsVolume.Size = userJobSpec.tmpfs_size();
                tmpfsVolume.Path = userJobSpec.tmpfs_path();
                options.TmpfsVolumes.push_back(tmpfsVolume);
            }

            if (userJobSpec.has_inode_limit()) {
                options.InodeLimit = userJobSpec.inode_limit();
            }

            if (userJobSpec.has_disk_space_limit()) {
                options.DiskSpaceLimit = userJobSpec.disk_space_limit();
            }
        }

        TmpfsPaths_ = WaitFor(Slot_->CreateSandboxDirectories(options))
            .ValueOrThrow();

        YT_LOG_INFO("Finished preparing sandbox directories");
    }

    // Build artifacts.
    void InitializeArtifacts()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);

        if (schedulerJobSpecExt.has_user_job_spec()) {
            const auto& userJobSpec = schedulerJobSpecExt.user_job_spec();
            for (const auto& descriptor : userJobSpec.files()) {
                Artifacts_.push_back(TArtifact{
                    ESandboxKind::User,
                    descriptor.file_name(),
                    descriptor.executable(),
                    descriptor.bypass_artifact_cache(),
                    TArtifactKey(descriptor),
                    nullptr});
            }

            bool needGpu = GetResourceUsage().gpu() > 0 || Config_->JobController->TestGpuLayers;

            if (needGpu && userJobSpec.enable_gpu_layers()) {
                if (userJobSpec.layers().empty()) {
                    THROW_ERROR_EXCEPTION(EErrorCode::GpuJobWithoutLayers,
                        "No layers specified for GPU job; at least a base layer is required to use GPU");
                }

                for (auto&& layerKey : Bootstrap_->GetGpuManager()->GetToppingLayers()) {
                    LayerArtifactKeys_.push_back(std::move(layerKey));
                }
            }

            for (const auto& descriptor : userJobSpec.layers()) {
                LayerArtifactKeys_.push_back(TArtifactKey(descriptor));
            }
        }

        if (schedulerJobSpecExt.has_input_query_spec()) {
            const auto& querySpec = schedulerJobSpecExt.input_query_spec();
            for (const auto& function : querySpec.external_functions()) {
                TArtifactKey key;
                key.mutable_data_source()->set_type(static_cast<int>(EDataSourceType::File));

                for (const auto& chunkSpec : function.chunk_specs()) {
                    *key.add_chunk_specs() = chunkSpec;
                }

                Artifacts_.push_back(TArtifact{
                    ESandboxKind::Udf,
                    function.name(),
                    false,
                    false,
                    key,
                    nullptr});
            }
        }
    }

    TArtifactDownloadOptions MakeArtifactDownloadOptions()
    {
        TArtifactDownloadOptions options;
        options.NodeDirectory = Bootstrap_->GetNodeDirectory();
        options.TrafficMeter = TrafficMeter_;
        return options;
    }

    // Start async artifacts download.
    TFuture<std::vector<NDataNode::IChunkPtr>> DownloadArtifacts()
    {
        const auto& chunkCache = Bootstrap_->GetChunkCache();

        std::vector<TFuture<IChunkPtr>> asyncChunks;
        for (const auto& artifact : Artifacts_) {
            if (artifact.BypassArtifactCache) {
                asyncChunks.push_back(MakeFuture<IChunkPtr>(nullptr));
                continue;
            }

            YT_LOG_INFO("Downloading user file (FileName: %v, SandboxKind: %v)",
                artifact.Name,
                artifact.SandboxKind);

            auto downloadOptions = MakeArtifactDownloadOptions();
            auto asyncChunk = chunkCache->DownloadArtifact(artifact.Key, downloadOptions)
                .Apply(BIND([=, fileName = artifact.Name, this_ = MakeStrong(this)] (const TErrorOr<IChunkPtr>& chunkOrError) {
                    THROW_ERROR_EXCEPTION_IF_FAILED(chunkOrError,
                        EErrorCode::ArtifactDownloadFailed,
                        "Failed to prepare user file %Qv",
                        fileName);

                    const auto& chunk = chunkOrError.Value();
                    YT_LOG_INFO("Artifact chunk ready (FileName: %v, LocationId: %v, ChunkId: %v)",
                        fileName,
                        chunk->GetLocation()->GetId(),
                        chunk->GetId());
                    return chunk;
                }));

            asyncChunks.push_back(asyncChunk);
        }

        return Combine(asyncChunks);
    }

    // Put files to sandbox.
    TFuture<void> PrepareArtifact(const TArtifact& artifact)
    {
        if (artifact.BypassArtifactCache) {
            YT_LOG_INFO("Downloading artifact with cache bypass (FileName: %v, Executable: %v, SandboxKind: %v)",
                artifact.Name,
                artifact.Executable,
                artifact.SandboxKind);

            const auto& chunkCache = Bootstrap_->GetChunkCache();
            auto downloadOptions = MakeArtifactDownloadOptions();
            auto producer = chunkCache->MakeArtifactDownloadProducer(artifact.Key, downloadOptions);

            return Slot_->MakeFile(
                artifact.SandboxKind,
                producer,
                artifact.Name,
                artifact.Executable);
        } else {
            YT_VERIFY(artifact.Chunk);

            const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
            bool copyFiles = schedulerJobSpecExt.has_user_job_spec() && schedulerJobSpecExt.user_job_spec().copy_files();
            if (copyFiles) {
                YT_LOG_INFO("Copying artifact (FileName: %v, Executable: %v, SandboxKind: %v)",
                    artifact.Name,
                    artifact.Executable,
                    artifact.SandboxKind);

                return Slot_->MakeCopy(
                    artifact.SandboxKind,
                    artifact.Chunk->GetFileName(),
                    artifact.Name,
                    artifact.Executable);
            } else {
                YT_LOG_INFO("Making symlink for artifact (FileName: %v, Executable: %v, SandboxKind: %v)",
                    artifact.Name,
                    artifact.Executable,
                    artifact.SandboxKind);

                return Slot_->MakeLink(
                    artifact.SandboxKind,
                    artifact.Chunk->GetFileName(),
                    artifact.Name,
                    artifact.Executable);
            }
        }
    }

    void PrepareArtifacts()
    {
        YT_LOG_INFO("Started preparing artifacts");

        for (const auto& artifact : Artifacts_) {
            // Artifact preparation is uncancelable, so we check for an early exit.
            if (JobPhase_ != EJobPhase::PreparingArtifacts) {
                return;
            }

            WaitFor(PrepareArtifact(artifact))
                .ThrowOnError();
        }

        // When all artifacts are prepared we can finally change permission for sandbox which will
        // take away write access from the current user (see slot_location.cpp for details).
        const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        if (schedulerJobSpecExt.has_user_job_spec()) {
            YT_LOG_INFO("Setting sandbox permissions");
            WaitFor(Slot_->FinalizePreparation())
                .ThrowOnError();
        }

        YT_LOG_INFO("Finished preparing artifacts");
    }

    TFuture<void> RunSetupCommands()
    {
        auto commands = GetSetupCommands();
        if (commands.empty()) {
            return VoidFuture;
        }

        YT_LOG_INFO("Running setup commands");
        return Slot_->RunSetupCommands(Id_, commands, MakeWritableRootFS(), Config_->JobController->SetupCommandUser);
    }

    // Analyse results.
    static TError BuildJobProxyError(const TError& spawnError)
    {
        if (spawnError.IsOK()) {
            return TError();
        }

        auto jobProxyError = TError(EErrorCode::JobProxyFailed, "Job proxy failed")
            << spawnError;

        if (spawnError.GetCode() == EProcessErrorCode::NonZeroExitCode) {
            // Try to translate the numeric exit code into some human readable reason.
            auto reason = EJobProxyExitCode(spawnError.Attributes().Get<int>("exit_code"));
            const auto& validReasons = TEnumTraits<EJobProxyExitCode>::GetDomainValues();
            if (std::find(validReasons.begin(), validReasons.end(), reason) != validReasons.end()) {
                jobProxyError.Attributes().Set("reason", reason);
            }
        }

        return jobProxyError;
    }

    std::optional<EAbortReason> GetAbortReason(const TJobResult& jobResult)
    {
        auto resultError = FromProto<TError>(jobResult.error());

        if (jobResult.HasExtension(TSchedulerJobResultExt::scheduler_job_result_ext)) {
            const auto& schedulerResultExt = jobResult.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

            if (!resultError.FindMatching(NNet::EErrorCode::ResolveTimedOut) &&
                !resultError.FindMatching(NChunkClient::EErrorCode::BandwidthThrottlingFailed) &&
                schedulerResultExt.failed_chunk_ids_size() > 0)
            {
                return EAbortReason::FailedChunks;
            }
        }

        // This is most probably user error, still we don't want to make it fatal.
        if (resultError.FindMatching(NDataNode::EErrorCode::LayerUnpackingFailed)) {
            return std::nullopt;
        }

        auto abortReason = resultError.Attributes().Find<EAbortReason>("abort_reason");
        if (abortReason) {
            return *abortReason;
        }

        if (AbortJobIfAccountLimitExceeded_ &&
            resultError.FindMatching(NSecurityClient::EErrorCode::AccountLimitExceeded))
        {
            return EAbortReason::AccountLimitExceeded;
        }

        if (resultError.FindMatching(NExecAgent::EErrorCode::ResourceOverdraft)) {
            return EAbortReason::ResourceOverdraft;
        }

        if (resultError.FindMatching(NExecAgent::EErrorCode::WaitingJobTimeout)) {
            return EAbortReason::WaitingTimeout;
        }

        if (resultError.FindMatching(NExecAgent::EErrorCode::AbortByScheduler) ||
            resultError.FindMatching(NJobProxy::EErrorCode::JobNotPrepared))
        {
            return EAbortReason::Scheduler;
        }

        if (resultError.FindMatching(NChunkClient::EErrorCode::AllTargetNodesFailed) ||
            resultError.FindMatching(NChunkClient::EErrorCode::BandwidthThrottlingFailed) ||
            resultError.FindMatching(NChunkClient::EErrorCode::MasterCommunicationFailed) ||
            resultError.FindMatching(NChunkClient::EErrorCode::MasterNotConnected) ||
            resultError.FindMatching(NChunkClient::EErrorCode::ReaderTimeout) ||
            resultError.FindMatching(NExecAgent::EErrorCode::ConfigCreationFailed) ||
            resultError.FindMatching(NExecAgent::EErrorCode::SlotNotFound) ||
            resultError.FindMatching(NExecAgent::EErrorCode::JobEnvironmentDisabled) ||
            resultError.FindMatching(NExecAgent::EErrorCode::ArtifactCopyingFailed) ||
            resultError.FindMatching(NExecAgent::EErrorCode::ArtifactDownloadFailed) ||
            resultError.FindMatching(NExecAgent::EErrorCode::NodeDirectoryPreparationFailed) ||
            resultError.FindMatching(NExecAgent::EErrorCode::SlotLocationDisabled) ||
            resultError.FindMatching(NExecAgent::EErrorCode::RootVolumePreparationFailed) ||
            resultError.FindMatching(NExecAgent::EErrorCode::NotEnoughDiskSpace) ||
            resultError.FindMatching(NJobProxy::EErrorCode::MemoryCheckFailed) ||
            resultError.FindMatching(NContainers::EErrorCode::FailedToStartContainer) ||
            resultError.FindMatching(EProcessErrorCode::CannotResolveBinary) ||
            resultError.FindMatching(NNet::EErrorCode::ResolveTimedOut) ||
            resultError.FindMatching(NExecAgent::EErrorCode::JobProxyPreparationTimeout) ||
            resultError.FindMatching(NExecAgent::EErrorCode::JobPreparationTimeout) ||
            resultError.FindMatching(NExecAgent::EErrorCode::GpuLayerNotFetched))
        {
            return EAbortReason::Other;
        }

        if (auto jobProxyFailedError = resultError.FindMatching(NExecAgent::EErrorCode::JobProxyFailed)) {
            if (auto processError = resultError.FindMatching(EProcessErrorCode::NonZeroExitCode))
            {
                auto exitCode = NExecAgent::EJobProxyExitCode(processError->Attributes().Get<int>("exit_code"));
                if (exitCode == EJobProxyExitCode::HeartbeatFailed ||
                    exitCode == EJobProxyExitCode::ResultReportFailed ||
                    exitCode == EJobProxyExitCode::ResourcesUpdateFailed ||
                    exitCode == EJobProxyExitCode::GetJobSpecFailed ||
                    exitCode == EJobProxyExitCode::InvalidSpecVersion ||
                    exitCode == EJobProxyExitCode::PortoManagmentFailed)
                {
                    return EAbortReason::Other;
                }
                if (exitCode == EJobProxyExitCode::ResourceOverdraft) {
                    return EAbortReason::ResourceOverdraft;
                }
            }
        }

        if (Signaled_) {
            return EAbortReason::UserRequest;
        }

        return std::nullopt;
    }

    bool IsFatalError(const TError& error)
    {
        return
            error.FindMatching(NTableClient::EErrorCode::SortOrderViolation) ||
            error.FindMatching(NSecurityClient::EErrorCode::AuthenticationError) ||
            error.FindMatching(NSecurityClient::EErrorCode::AuthorizationError) ||
            (
                error.FindMatching(NSecurityClient::EErrorCode::AccountLimitExceeded) &&
                !AbortJobIfAccountLimitExceeded_
            ) ||
            error.FindMatching(NSecurityClient::EErrorCode::NoSuchAccount) ||
            error.FindMatching(NNodeTrackerClient::EErrorCode::NoSuchNetwork) ||
            error.FindMatching(NTableClient::EErrorCode::InvalidDoubleValue) ||
            error.FindMatching(NTableClient::EErrorCode::IncomparableType) ||
            error.FindMatching(NTableClient::EErrorCode::UnhashableType) ||
            error.FindMatching(NTableClient::EErrorCode::CorruptedNameTable) ||
            error.FindMatching(NTableClient::EErrorCode::RowWeightLimitExceeded) ||
            error.FindMatching(NTableClient::EErrorCode::InvalidColumnFilter) ||
            error.FindMatching(NTableClient::EErrorCode::InvalidColumnRenaming) ||
            error.FindMatching(EErrorCode::SetupCommandFailed) ||
            error.FindMatching(EErrorCode::GpuJobWithoutLayers);
    }

    TYsonString EnrichStatisticsWithGpuInfo(const TYsonString& statisticsYson)
    {
        auto statistics = ConvertTo<TStatistics>(statisticsYson);

        i64 totalUtilizationGpu = 0;
        i64 totalUtilizationMemory = 0;
        i64 totalLoad = 0;
        i64 totalMaxMemoryUsed = 0;

        auto gpuInfoMap = Bootstrap_->GetGpuManager()->GetGpuInfoMap();
        for (int index = 0; index < GpuSlots_.size(); ++index) {
            const auto& slot = GpuSlots_[index];
            auto& slotStatistics = GpuStatistics_[index];

            TGpuInfo gpuInfo;
            {
                auto it = gpuInfoMap.find(slot->GetDeviceNumber());
                if (it == gpuInfoMap.end()) {
                    continue;
                }
                gpuInfo = it->second;
            }

            slotStatistics.CumulativeUtilizationGpu +=
                (gpuInfo.UpdateTime - slotStatistics.LastUpdateTime).MilliSeconds() *
                gpuInfo.UtilizationGpuRate;
            slotStatistics.CumulativeUtilizationMemory +=
                (gpuInfo.UpdateTime - slotStatistics.LastUpdateTime).MilliSeconds() *
                gpuInfo.UtilizationMemoryRate;
            if (gpuInfo.UtilizationGpuRate > 0) {
                slotStatistics.CumulativeLoad += (gpuInfo.UpdateTime - slotStatistics.LastUpdateTime).MilliSeconds();
            }
            slotStatistics.MaxMemoryUsed = std::max(slotStatistics.MaxMemoryUsed, gpuInfo.MemoryUsed);
            slotStatistics.LastUpdateTime = gpuInfo.UpdateTime;

            totalUtilizationGpu += slotStatistics.CumulativeUtilizationGpu;
            totalUtilizationMemory += slotStatistics.CumulativeUtilizationMemory;
            totalLoad += slotStatistics.CumulativeLoad;
            totalMaxMemoryUsed += slotStatistics.MaxMemoryUsed;
        }

        statistics.AddSample("/user_job/gpu/utilization_gpu", totalUtilizationGpu);
        statistics.AddSample("/user_job/gpu/utilization_memory", totalUtilizationMemory);
        statistics.AddSample("/user_job/gpu/load", totalLoad);
        statistics.AddSample("/user_job/gpu/memory_used", totalMaxMemoryUsed);

        return ConvertToYsonString(statistics);
    }

    std::vector<TShellCommandConfigPtr> GetSetupCommands()
    {
        std::vector<TShellCommandConfigPtr> result;

        auto addIfPresent = [&] (const std::optional<TShellCommandConfigPtr>& command) {
            if (command) {
                result.push_back(*command);
            }
        };

        addIfPresent(Config_->JobController->JobSetupCommand);
        addIfPresent(Config_->JobController->GpuManager->JobSetupCommand);

        return result;
    }

    NContainers::TRootFS MakeWritableRootFS()
    {
        YT_VERIFY(RootVolume_);
        NContainers::TRootFS rootFS;

        rootFS.RootPath = RootVolume_->GetPath();
        rootFS.IsRootReadOnly = false;
        rootFS.Binds.reserve(Config_->RootFSBinds.size());

        for (const auto& bind : Config_->RootFSBinds) {
            rootFS.Binds.push_back({bind->ExternalPath, bind->InternalPath, bind->ReadOnly});
        }

        return rootFS;
    }

    TJobStatistics MakeDefaultJobStatistics()
    {
        auto statistics = TJobStatistics()
            .Type(GetType())
            .State(GetState())
            .StartTime(GetStartTime())
            .SpecVersion(0) // TODO: fill correct spec version.
            .Events(JobEvents_)
            .CoreInfos(CoreInfos_);
        if (FinishTime_) {
            statistics.SetFinishTime(*FinishTime_);
        }
        return statistics;
    }


    void InitializeJobProbe()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto probe = CreateJobProbe(Slot_->GetBusClientConfig(), Id_);
        {
            auto guard = Guard(JobProbeLock_);
            std::swap(JobProbe_, probe);
        }
    }

    void ResetJobProbe()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        IJobProbePtr probe;
        {
            auto guard = Guard(JobProbeLock_);
            std::swap(JobProbe_, probe);
        }
    }

    IJobProbePtr GetJobProbeOrThrow()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto guard = Guard(JobProbeLock_);
        if (!JobProbe_) {
            THROW_ERROR_EXCEPTION("Job probe is not available");
        }
        return JobProbe_;
    }
};

////////////////////////////////////////////////////////////////////////////////

NJobAgent::IJobPtr CreateUserJob(
    TJobId jobId,
    TOperationId operationId,
    const TNodeResources& resourceUsage,
    TJobSpec&& jobSpec,
    NCellNode::TBootstrap* bootstrap)
{
    return New<TJob>(
        jobId,
        operationId,
        resourceUsage,
        std::move(jobSpec),
        bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecAgent
