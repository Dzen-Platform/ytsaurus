#pragma once

#include "job.h"
#include "chunk_cache.h"
#include "controller_agent_connector.h"
#include "gpu_manager.h"
#include "public.h"
#include "volume_manager.h"

#include <yt/yt/server/node/exec_node/chunk_cache.h>

#include <yt/yt/server/node/job_agent/job_resource_manager.h>

#include <yt/yt/server/lib/containers/public.h>

#include <yt/yt/server/lib/exec_node/public.h>

#include <yt/yt/server/lib/job_agent/job_report.h>
#include <yt/yt/server/lib/job_agent/public.h>

#include <yt/yt/server/lib/job_proxy/public.h>

#include <yt/yt/client/api/client.h>

#include <yt/yt/ytlib/job_prober_client/public.h>

#include <yt/yt/ytlib/job_proxy/public.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/yson/string.h>

namespace NYT::NExecNode
{

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EGpuCheckType,
    (Preliminary)
    (Extra)
)

////////////////////////////////////////////////////////////////////////////////

class TJob
    : public NJobAgent::TResourceHolder
    , public TRefCounted
{
public:
    DEFINE_SIGNAL(void(const NNodeTrackerClient::NProto::TNodeResources&), ResourcesUpdated);
    DEFINE_SIGNAL(void(), JobPrepared);
    DEFINE_SIGNAL(void(), JobFinished);

public:
    TJob(
        TJobId jobId,
        TOperationId operationId,
        const NNodeTrackerClient::NProto::TNodeResources& resourceUsage,
        NJobTrackerClient::NProto::TJobSpec&& jobSpec,
        IBootstrap* bootstrap,
        TControllerAgentDescriptor agentDescriptor);

    ~TJob();

    void Start();
    bool IsStarted() const;

    NJobAgent::TResourceHolder* AsResourceHolder();

    void Abort(const TError& error);

    void OnJobProxySpawned();

    void PrepareArtifact(
        const TString& artifactName,
        const TString& pipePath);

    void OnArtifactPreparationFailed(
        const TString& artifactName,
        const TString& artifactPath,
        const TError& error);

    void OnArtifactsPrepared();

    void OnJobPrepared();

    void SetResult(const NJobTrackerClient::NProto::TJobResult& jobResult);

    TJobId GetId() const;

    TOperationId GetOperationId() const;

    const TControllerAgentDescriptor& GetControllerAgentDescriptor() const;

    void UpdateControllerAgentDescriptor(TControllerAgentDescriptor agentInfo);

    EJobType GetType() const;

    const NJobTrackerClient::NProto::TJobSpec& GetSpec() const;

    bool IsUrgent() const;

    const std::vector<int>& GetPorts() const;

    EJobState GetState() const;

    TInstant GetStartTime() const;

    NJobAgent::TTimeStatistics GetTimeStatistics() const;

    EJobPhase GetPhase() const;

    int GetSlotIndex() const;

    const NNodeTrackerClient::NProto::TNodeResources& GetResourceUsage() const;
    bool IsGpuRequested() const;

    const TError& GetJobError() const;
    NJobTrackerClient::NProto::TJobResult GetResult() const;

    double GetProgress() const;

    void SetResourceUsage(const NNodeTrackerClient::NProto::TNodeResources& newUsage);
    
    bool ResourceUsageOverdrafted() const;

    void SetProgress(double progress);

    i64 GetStderrSize() const;

    void SetStderrSize(i64 value);

    void SetStderr(const TString& value);

    void SetFailContext(const TString& value);

    void SetProfile(const NJobAgent::TJobProfile& value);

    void SetCoreInfos(NCoreDump::TCoreInfos value);

    const NJobAgent::TChunkCacheStatistics& GetChunkCacheStatistics() const;

    NYson::TYsonString GetStatistics() const;

    TInstant GetStatisticsLastSendTime() const;

    void ResetStatisticsLastSendTime();

    void SetStatistics(const NYson::TYsonString& statisticsYson);

    void BuildOrchid(NYTree::TFluentMap fluent) const;

    std::vector<NChunkClient::TChunkId> DumpInputContext();

    std::optional<TString> GetStderr();

    std::optional<TString> GetFailContext();

    std::optional<NJobAgent::TJobProfile> GetProfile();

    const NCoreDump::TCoreInfos& GetCoreInfos();

    NApi::TPollJobShellResponse PollJobShell(
        const NJobProberClient::TJobShellDescriptor& jobShellDescriptor,
        const NYson::TYsonString& parameters);

    void HandleJobReport(NJobAgent::TNodeJobReport&& jobReport);

    void ReportSpec();

    void ReportStderr();

    void ReportFailContext();

    void ReportProfile();

    void GuardedInterrupt(
        TDuration timeout,
        NScheduler::EInterruptReason interruptionReason,
        const std::optional<TString>& preemptionReason);

    void GuardedFail();

    bool GetStored() const;

    void SetStored(bool value);

    void OnJobProxyCompleted() noexcept;

    bool IsJobProxyCompleted() const noexcept;

    bool IsInterruptible() const noexcept;

    void OnJobInterruptionTimeout();

    const TControllerAgentConnectorPool::TControllerAgentConnectorPtr& GetControllerAgentConnector() const noexcept;

    void Interrupt(
        TDuration timeout,
        NScheduler::EInterruptReason interruptionReason,
        const std::optional<TString>& preemptionReason);

    void Fail();

    NScheduler::EInterruptReason GetInterruptionReason() const noexcept;

private:
    DECLARE_THREAD_AFFINITY_SLOT(JobThread);

    const TJobId Id_;
    const TOperationId OperationId_;
    IBootstrap* const Bootstrap_;

    TControllerAgentDescriptor ControllerAgentDescriptor_;
    TControllerAgentConnectorPool::TControllerAgentConnectorPtr ControllerAgentConnector_;

    const TExecNodeConfigPtr Config_;
    const TExecNodeDynamicConfigPtr DynamicConfig_;
    const IInvokerPtr Invoker_;
    const TInstant StartTime_;
    const NChunkClient::TTrafficMeterPtr TrafficMeter_;

    NJobTrackerClient::NProto::TJobSpec JobSpec_;
    const NScheduler::NProto::TSchedulerJobSpecExt* const SchedulerJobSpecExt_;
    const NScheduler::NProto::TUserJobSpec* const UserJobSpec_;
    const NJobProxy::TJobTestingOptionsPtr JobTestingOptions_;

    const bool Interruptible_;
    const bool AbortJobIfAccountLimitExceeded_;

    THashMap<TString, TUserJobSensorPtr> SupportedMonitoringSensors_;

    // Used to terminate artifacts downloading in case of cancelation.
    TFuture<void> ArtifactsFuture_ = VoidFuture;

    double Progress_ = 0.0;
    i64 StderrSize_ = 0;

    std::optional<TString> Stderr_;
    std::optional<TString> FailContext_;
    std::optional<NJobAgent::TJobProfile> Profile_;
    NCoreDump::TCoreInfos CoreInfos_;

    NConcurrency::TDelayedExecutorCookie InterruptionTimeoutCookie_;

    NYson::TYsonString StatisticsYson_ = NYson::TYsonString(TStringBuf("{}"));
    TInstant StatisticsLastSendTime_ = TInstant::Now();

    NProfiling::TBufferedProducerPtr UserJobSensorProducer_;

    NJobAgent::TExecAttributes ExecAttributes_;

    std::optional<TError> Error_;
    std::optional<NScheduler::NProto::TSchedulerJobResultExt> JobResultExtension_;

    std::optional<TInstant> PrepareTime_;
    std::optional<TInstant> CopyTime_;
    std::optional<TInstant> StartPrepareVolumeTime_;
    std::optional<TInstant> FinishPrepareVolumeTime_;
    std::optional<TInstant> ExecTime_;
    std::optional<TInstant> FinishTime_;

    std::optional<TInstant> PreliminaryGpuCheckStartTime_;
    std::optional<TInstant> PreliminaryGpuCheckFinishTime_;

    std::optional<TInstant> ExtraGpuCheckStartTime_;
    std::optional<TInstant> ExtraGpuCheckFinishTime_;

    std::vector<TGpuManager::TGpuSlotPtr> GpuSlots_;
    std::vector<TGpuStatistics> GpuStatistics_;

    i64 MaxDiskUsage_ = 0;

    int SetupCommandsCount_ = 0;

    std::optional<ui32> NetworkProjectId_;

    ISlotPtr Slot_;
    std::vector<TString> TmpfsPaths_;

    struct TArtifact
    {
        ESandboxKind SandboxKind;
        TString Name;
        bool Executable;
        bool BypassArtifactCache;
        bool CopyFile;
        NDataNode::TArtifactKey Key;
        NDataNode::IChunkPtr Chunk;
    };

    std::vector<TArtifact> Artifacts_;
    std::vector<NDataNode::TArtifactKey> LayerArtifactKeys_;

    //! Artifact name -> index of the artifact in #Artifacts_ list.
    THashMap<TString, int> UserArtifactNameToIndex_;

    IVolumePtr RootVolume_;

    bool IsGpuRequested_;
    double RequestedCpu_;
    i64 RequestedMemory_;

    EJobState JobState_ = EJobState::Waiting;
    EJobPhase JobPhase_ = EJobPhase::Created;

    NJobAgent::TJobEvents JobEvents_;

    NScheduler::EInterruptReason InterruptionReason_ = NScheduler::EInterruptReason::None;

    //! True if scheduler asked to store this job.
    bool Stored_ = false;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, JobProbeLock_);
    NJobProberClient::IJobProbePtr JobProbe_;

    std::vector<std::pair<TString, NNet::TIP6Address>> ResolvedNodeAddresses_;

    // Artifact statistics.
    NJobAgent::TChunkCacheStatistics ChunkCacheStatistics_;

    std::vector<TFuture<void>> ArtifactPrepareFutures_;

    bool JobProxyCompleted_ = false;

    bool Started_ = false;

    // IO statistics.
    i64 BytesRead_ = 0;
    i64 BytesWritten_ = 0;
    i64 IORequestsRead_ = 0;
    i64 IORequestsWritten_ = 0;

    // Tracing.
    NTracing::TTraceContextPtr TraceContext_;
    NTracing::TTraceContextFinishGuard FinishGuard_;

    // Helpers.

    template <class... U>
    void AddJobEvent(U&&... u)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        JobEvents_.emplace_back(std::forward<U>(u)...);
        HandleJobReport(MakeDefaultJobReport()
            .Events(JobEvents_));
    }

    void SetJobState(EJobState state);

    void SetJobPhase(EJobPhase phase);

    void SetJobStatePhase(EJobState state, EJobPhase phase);

    void ValidateJobRunning() const;

    void StartUserJobMonitoring();

    void DoSetResult(const TError& error);

    void DoSetResult(NJobTrackerClient::NProto::TJobResult jobResult);

    bool HandleFinishingPhase();

    void ValidateJobPhase(EJobPhase expectedPhase) const;

    // Event handlers.
    void OnNodeDirectoryPrepared(const TError& error);

    void OnArtifactsDownloaded(const TErrorOr<std::vector<NDataNode::IChunkPtr>>& errorOrArtifacts);

    void OnSandboxDirectoriesPrepared(const TError& error);

    void OnVolumePrepared(const TErrorOr<IVolumePtr>& volumeOrError);

    void OnSetupCommandsFinished(const TError& error);

    TFuture<void> RunGpuCheckCommand(
        const TString& gpuCheckBinaryPath,
        std::vector<TString> gpuCheckBinaryArgs,
        EGpuCheckType gpuCheckType);

    void OnGpuCheckCommandFinished(const TError& error);

    void OnExtraGpuCheckCommandFinished(const TError& error);

    void RunJobProxy();

    void OnJobProxyPreparationTimeout();

    void OnJobPreparationTimeout(TDuration prepareTimeLimit, bool fatal);

    void OnJobAbortionTimeout();

    void OnJobProxyFinished(const TError& error);

    void GuardedAction(std::function<void()> action);

    // Finalization.
    void Cleanup();

    // Preparation.
    void PrepareNodeDirectory();

    NJobProxy::TJobProxyConfigPtr CreateConfig();

    void PrepareSandboxDirectories();

    // Build artifacts.
    void InitializeArtifacts();

    TArtifactDownloadOptions MakeArtifactDownloadOptions() const;

    // Start async artifacts download.
    TFuture<std::vector<NDataNode::IChunkPtr>> DownloadArtifacts();

    TFuture<void> RunSetupCommands();

    // Analyse results.
    static TError BuildJobProxyError(const TError& spawnError);

    std::optional<NScheduler::EAbortReason> GetAbortReason();

    bool IsFatalError(const TError& error);

    void EnrichStatisticsWithGpuInfo(TStatistics* statistics);
    void EnrichStatisticsWithDiskInfo(TStatistics* statistics);
    void EnrichStatisticsWithArtifactsInfo(TStatistics* statistics);

    void UpdateIOStatistics(const TStatistics& statistics);

    void UpdateArtifactStatistics(i64 compressedDataSize, bool cacheHit);

    std::vector<NJobAgent::TShellCommandConfigPtr> GetSetupCommands();

    NContainers::TRootFS MakeWritableRootFS();

    NJobAgent::TNodeJobReport MakeDefaultJobReport();

    void InitializeJobProbe();

    void ResetJobProbe();

    NJobProberClient::IJobProbePtr GetJobProbeOrThrow();

    static bool ShouldCleanSandboxes();

    bool NeedGpuLayers();

    bool NeedGpu();

    void ProfileSensor(const TUserJobSensorPtr& sensor, NProfiling::ISensorWriter* writer, double value);
    void ProfileSensor(const TString& sensorName, NProfiling::ISensorWriter* writer, double value);

    void CollectSensorsFromStatistics(NProfiling::ISensorWriter* writer);
    void CollectSensorsFromGpuInfo(NProfiling::ISensorWriter* writer);

    TFuture<TSharedRef> DumpSensors();

    void OnResourcesAcquired() override;
};

DEFINE_REFCOUNTED_TYPE(TJob)

////////////////////////////////////////////////////////////////////////////////

TJobPtr CreateJob(
    NJobTrackerClient::TJobId jobId,
    NJobTrackerClient::TOperationId operationId,
    const NNodeTrackerClient::NProto::TNodeResources& resourceUsage,
    NJobTrackerClient::NProto::TJobSpec&& jobSpec,
    IBootstrap* bootstrap,
    TControllerAgentDescriptor agentDescriptor);

////////////////////////////////////////////////////////////////////////////////

void FillSchedulerJobStatus(NJobTrackerClient::NProto::TJobStatus* jobStatus, const TJobPtr& schedulerJob);

////////////////////////////////////////////////////////////////////////////////

using TJobFactory = TCallback<TJobPtr(
    NJobTrackerClient::TJobId jobid,
    NJobTrackerClient::TOperationId operationId,
    const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
    NJobTrackerClient::NProto::TJobSpec&& jobSpec,
    const NExecNode::TControllerAgentDescriptor& agentInfo)>;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecNode
