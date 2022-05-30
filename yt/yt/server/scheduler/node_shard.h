#pragma once

#include "private.h"
#include "scheduler.h"
#include "scheduler_strategy.h"

#include <yt/yt/server/lib/scheduler/scheduling_tag.h>
#include <yt/yt/server/lib/scheduler/structs.h>

#include <yt/yt/client/api/client.h>

#include <yt/yt/ytlib/chunk_client/chunk_service_proxy.h>

#include <yt/yt/ytlib/job_prober_client/job_prober_service_proxy.h>

#include <yt/yt/ytlib/job_tracker_client/helpers.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/ytlib/scheduler/job_resources_with_quota.h>

#include <yt/yt/core/concurrency/public.h>

#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/core/yson/public.h>

#include <yt/yt/core/ytree/public.h>

#include <yt/yt/core/misc/sync_expiring_cache.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

struct INodeShardHost
{
    virtual ~INodeShardHost() = default;

    virtual int GetNodeShardId(NNodeTrackerClient::TNodeId nodeId) const = 0;

    virtual TString FormatResources(const TJobResourcesWithQuota& resources) const = 0;
    virtual TString FormatResourceUsage(
        const TJobResources& usage,
        const TJobResources& limits,
        const NNodeTrackerClient::NProto::TDiskResources& diskResources) const = 0;
    virtual TString FormatHeartbeatResourceUsage(
        const TJobResources& usage,
        const TJobResources& limits,
        const NNodeTrackerClient::NProto::TDiskResources& diskResources) const = 0;

    virtual TFuture<void> RegisterOrUpdateNode(
        NNodeTrackerClient::TNodeId nodeId,
        const TString& nodeAddress,
        const TBooleanFormulaTags& tags) = 0;

    virtual void UnregisterNode(
        NNodeTrackerClient::TNodeId nodeId,
        const TString& nodeAddress) = 0;

    virtual const ISchedulerStrategyPtr& GetStrategy() const = 0;

    virtual TFuture<void> ValidateOperationAccess(
        const TString& user,
        TOperationId operationId,
        NYTree::EPermission permission) = 0;

    virtual TFuture<void> AttachJobContext(
        const NYTree::TYPath& path,
        NChunkClient::TChunkId chunkId,
        TOperationId operationId,
        TJobId jobId,
        const TString& user) = 0;

    virtual NJobProberClient::TJobProberServiceProxy CreateJobProberProxy(const NRpc::TAddressWithNetwork& addressWithNetwork) = 0;

    virtual int GetOperationArchiveVersion() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

struct TJobTimeStatisticsDelta
{
    void Reset()
    {
        CompletedJobTimeDelta = 0;
        FailedJobTimeDelta = 0;
        AbortedJobTimeDelta = 0;
    }

    TJobTimeStatisticsDelta& operator += (const TJobTimeStatisticsDelta& rhs)
    {
        CompletedJobTimeDelta += rhs.CompletedJobTimeDelta;
        FailedJobTimeDelta += rhs.FailedJobTimeDelta;
        AbortedJobTimeDelta += rhs.AbortedJobTimeDelta;
        return *this;
    }

    ui64 CompletedJobTimeDelta = 0;
    ui64 FailedJobTimeDelta = 0;
    ui64 AbortedJobTimeDelta = 0;
};

////////////////////////////////////////////////////////////////////////////////

struct TSetNodeSchedulingSegmentOptions
{
    NNodeTrackerClient::TNodeId NodeId = NNodeTrackerClient::InvalidNodeId;
    ESchedulingSegment Segment = ESchedulingSegment::Default;
};

using TSetNodeSchedulingSegmentOptionsList = std::vector<TSetNodeSchedulingSegmentOptions>;

////////////////////////////////////////////////////////////////////////////////

struct TNodeShardMasterHandshakeResult
{
    TPersistentSchedulingSegmentsStatePtr InitialSchedulingSegmentsState;
    TInstant SchedulingSegmentInitializationDeadline;
    std::vector<TOperationId> OperationIds;
};

////////////////////////////////////////////////////////////////////////////////

class TNodeShard
    : public TRefCounted
{
public:
    TNodeShard(
        int id,
        TSchedulerConfigPtr config,
        INodeShardHost* host,
        TBootstrap* bootstrap);

    int GetId() const;
    const IInvokerPtr& GetInvoker() const;

    void UpdateConfig(const TSchedulerConfigPtr& config);

    IInvokerPtr OnMasterConnected(const TNodeShardMasterHandshakeResult& result);
    void OnMasterDisconnected();

    void RegisterOperation(
        TOperationId operationId,
        TControllerEpoch controllerEpoch,
        const IOperationControllerPtr& controller,
        bool jobsReady);
    void StartOperationRevival(TOperationId operationId, TControllerEpoch newControllerEpoch);
    void FinishOperationRevival(TOperationId operationId, const std::vector<TJobPtr>& jobs);
    void ResetOperationRevival(TOperationId operationId);
    void UnregisterOperation(TOperationId operationId);

    void ProcessHeartbeat(const TScheduler::TCtxNodeHeartbeatPtr& context);

    void UnregisterAndRemoveNodeById(NNodeTrackerClient::TNodeId nodeId);
    void AbortJobsAtNode(NNodeTrackerClient::TNodeId nodeId, EAbortReason reason);


    TRefCountedExecNodeDescriptorMapPtr GetExecNodeDescriptors();
    void UpdateExecNodeDescriptors();

    void RemoveMissingNodes(const std::vector<TString>& nodeAddresses);
    std::vector<TError> HandleNodesAttributes(const std::vector<std::pair<TString, NYTree::INodePtr>>& nodeMaps);

    void AbortOperationJobs(TOperationId operationId, const TError& abortReason, bool controllerTerminated);
    void ResumeOperationJobs(TOperationId operationId);

    NNodeTrackerClient::TNodeDescriptor GetJobNode(TJobId jobId);

    void DumpJobInputContext(TJobId jobId, const NYTree::TYPath& path, const TString& user);
    void AbandonJob(TJobId jobId);
    void AbortJobByUserRequest(TJobId jobId, std::optional<TDuration> interruptTimeout, const TString& user);

    void AbortJob(TJobId jobId, const TError& error);
    void AbortJobs(const std::vector<TJobId>& jobIds, const TError& error);
    void InterruptJob(TJobId jobId, EInterruptReason reason);
    void FailJob(TJobId jobId);
    void ReleaseJob(TJobId jobId, NJobTrackerClient::TReleaseJobFlags releaseFlags);

    void BuildNodesYson(NYTree::TFluentMap fluent);

    TOperationId FindOperationIdByJobId(TJobId job, bool considerFinished);

    TJobResources GetResourceLimits(const TSchedulingTagFilter& filter) const;
    TJobResources GetResourceUsage(const TSchedulingTagFilter& filter) const;

    int GetActiveJobCount() const;
    int GetExecNodeCount() const;
    int GetTotalNodeCount() const;
    int GetSubmitToStrategyJobCount() const;

    TFuture<TControllerScheduleJobResultPtr> BeginScheduleJob(
        TIncarnationId incarnationId,
        TOperationId operationId,
        TJobId jobId);
    void EndScheduleJob(
        const NProto::TScheduleJobResponse& response);
    void RemoveOutdatedScheduleJobEntries();

    int ExtractJobReporterWriteFailuresCount();
    int GetJobReporterQueueIsTooLargeNodeCount();

    void SetSchedulingSegmentsForNodes(const TSetNodeSchedulingSegmentOptionsList& nodesWithNewSegments);

    TControllerEpoch GetOperationControllerEpoch(TOperationId operationId);
    TControllerEpoch GetJobControllerEpoch(TJobId jobId);

    bool IsOperationControllerTerminated(TOperationId operationId) const noexcept;
    bool IsOperationRegistered(TOperationId operationId) const noexcept;
    bool AreNewJobsForbiddenForOperation(TOperationId operationId) const noexcept;

    std::vector<TString> GetNodeAddressesWithUnsupportedInterruption() const;

private:
    const int Id_;
    TSchedulerConfigPtr Config_;
    INodeShardHost* const Host_;
    TBootstrap* const Bootstrap_;

    const NConcurrency::TActionQueuePtr ActionQueue_;
    const NConcurrency::TPeriodicExecutorPtr CachedExecNodeDescriptorsRefresher_;

    struct TResourceStatistics
    {
        TJobResources Usage;
        TJobResources Limits;
    };
    const TIntrusivePtr<TSyncExpiringCache<TSchedulingTagFilter, TResourceStatistics>> CachedResourceStatisticsByTags_;

    const NLogging::TLogger Logger;

    bool Connected_ = false;

    TCancelableContextPtr CancelableContext_;
    IInvokerPtr CancelableInvoker_;

    int ConcurrentHeartbeatCount_ = 0;

    bool HasOngoingNodesAttributesUpdate_ = false;

    std::atomic<int> ActiveJobCount_ = {0};

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, ResourcesLock_);
    TJobResources TotalResourceUsage_;

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, CachedExecNodeDescriptorsLock_);
    TRefCountedExecNodeDescriptorMapPtr CachedExecNodeDescriptors_ = New<TRefCountedExecNodeDescriptorMap>();

    THashMap<NNodeTrackerClient::TNodeId, TExecNodePtr> IdToNode_;
    // Exec node is the node that is online and has user slots.
    std::atomic<int> ExecNodeCount_ = {0};
    std::atomic<int> TotalNodeCount_ = {0};

    std::atomic<int> JobReporterWriteFailuresCount_ = {0};
    std::atomic<int> JobReporterQueueIsTooLargeNodeCount_ = {0};

    TAllocationCounter AllocationCounter_;

    NProfiling::TCounter HardConcurrentHeartbeatLimitReachedCounter_;
    NProfiling::TCounter SoftConcurrentHeartbeatLimitReachedCounter_;
    NProfiling::TCounter HeartbeatWithScheduleJobsCounter_;
    NProfiling::TCounter HeartbeatJobCount_;
    NProfiling::TCounter HeartbeatStatisticBytes_;
    NProfiling::TCounter HeartbeatJobResultBytes_;
    NProfiling::TCounter HeartbeatProtoMessageBytes_;
    NProfiling::TCounter HeartbeatCount_;

    THashMap<TJobId, TJobUpdate> JobsToSubmitToStrategy_;
    std::atomic<int> SubmitToStrategyJobCount_;

    struct TScheduleJobEntry
    {
        TOperationId OperationId;
        TIncarnationId IncarnationId;
        TPromise<TControllerScheduleJobResultPtr> Promise;
        THashMultiMap<TOperationId, THashMap<TJobId, TScheduleJobEntry>::iterator>::iterator OperationIdToJobIdsIterator;
        NProfiling::TCpuInstant StartTime;
    };
    // NB: It is important to use THash* instead of std::unordered_* since we rely on
    // iterators not to be invalidated.
    THashMap<TJobId, TScheduleJobEntry> JobIdToScheduleEntry_;
    THashMultiMap<TOperationId, THashMap<TJobId, TScheduleJobEntry>::iterator> OperationIdToJobIterators_;

    NConcurrency::TPeriodicExecutorPtr RemoveOutdatedScheduleJobEntryExecutor_;

    NConcurrency::TPeriodicExecutorPtr SubmitJobsToStrategyExecutor_;

    using TShardEpoch = ui64;

    struct TOperationState
    {
        TOperationState(
            IOperationControllerPtr controller,
            bool jobsReady,
            TShardEpoch shardEpoch,
            TControllerEpoch controllerEpoch)
            : Controller(std::move(controller))
            , JobsReady(jobsReady)
            , ShardEpoch(shardEpoch)
            , ControllerEpoch(controllerEpoch)
        { }

        THashMap<TJobId, TJobPtr> Jobs;
        THashSet<TJobId> JobsToSubmitToStrategy;
        THashSet<TJobId> RecentlyFinishedJobIds;
        //! Used only to avoid multiple log messages per job about 'operation is not ready'.
        THashSet<TJobId> OperationUnreadyLoggedJobIds;
        IOperationControllerPtr Controller;
        bool ControllerTerminated = false;
        //! Raised to prevent races between suspension and scheduler strategy scheduling new jobs.
        bool ForbidNewJobs = false;
        //! Flag showing that we already know about all jobs of this operation
        //! and it is OK to abort unknown jobs that claim to be a part of this operation.
        bool JobsReady = false;
        //! Prevents leaking #AbortUnconfirmedJobs between different incarnations of the same operation.
        TShardEpoch ShardEpoch;
        TControllerEpoch ControllerEpoch;
    };

    THashMap<TOperationId, TOperationState> IdToOpertionState_;
    THashSet<TOperationId> WaitingForRegisterOperationIds_;
    TShardEpoch CurrentEpoch_ = 0;

    TPersistentSchedulingSegmentsStatePtr InitialSchedulingSegmentsState_;
    TInstant SchedulingSegmentInitializationDeadline_;

    void ValidateConnected();

    void DoCleanup();

    void DoProcessHeartbeat(const TScheduler::TCtxNodeHeartbeatPtr& context);

    TResourceStatistics CalculateResourceStatistics(const TSchedulingTagFilter& filter);

    TExecNodePtr GetOrRegisterNode(
        NNodeTrackerClient::TNodeId nodeId,
        const NNodeTrackerClient::TNodeDescriptor& descriptor,
        ENodeState state);
    TExecNodePtr RegisterNode(
        NNodeTrackerClient::TNodeId nodeId,
        const NNodeTrackerClient::TNodeDescriptor& descriptor,
        ENodeState state);
    void UnregisterNode(const TExecNodePtr& node);
    void DoUnregisterNode(const TExecNodePtr& node);
    void OnNodeHeartbeatLeaseExpired(NNodeTrackerClient::TNodeId nodeId);
    void OnNodeRegistrationLeaseExpired(NNodeTrackerClient::TNodeId nodeId);
    // NB: 'node' passed by value since we want to own it after remove.
    void RemoveNode(TExecNodePtr node);

    void SetNodeSchedulingSegment(const TExecNodePtr& node, ESchedulingSegment segment);

    void AbortAllJobsAtNode(const TExecNodePtr& node, EAbortReason reason);
    void AbortUnconfirmedJobs(
        TOperationId operationId,
        TShardEpoch shardEpoch,
        const std::vector<TJobPtr>& jobs);

    void ProcessHeartbeatJobs(
        const TExecNodePtr& node,
        NJobTrackerClient::NProto::TReqHeartbeat* request,
        NJobTrackerClient::NProto::TRspHeartbeat* response,
        std::vector<TJobPtr>* runningJobs,
        bool* hasWaitingJobs);

    TJobPtr ProcessJobHeartbeat(
        const TExecNodePtr& node,
        const THashSet<TJobId>& recentlyFinishedJobIdsToRemove,
        NJobTrackerClient::NProto::TRspHeartbeat* response,
        TJobStatus* jobStatus);

    void UpdateRunningJobStatistics(const TExecNodePtr& node, const std::vector<TJobPtr>& runningJobs, TInstant now);

    using TAllocationStateToJobList = TEnumIndexedVector<EAllocationState, std::vector<TJobPtr>>;
    void LogOngoingJobsAt(TInstant now, const TExecNodePtr& node, const TAllocationStateToJobList& ongoingJobsByAllocationState) const;

    void SubtractNodeResources(const TExecNodePtr& node);
    void AddNodeResources(const TExecNodePtr& node);
    void UpdateNodeResources(
        const TExecNodePtr& node,
        const TJobResources& limits,
        const TJobResources& usage,
        const NNodeTrackerClient::NProto::TDiskResources& diskResources);

    void BeginNodeHeartbeatProcessing(const TExecNodePtr& node);
    void EndNodeHeartbeatProcessing(const TExecNodePtr& node);

    void SubmitJobsToStrategy();

    void ProcessScheduledAndPreemptedJobs(
        const ISchedulingContextPtr& schedulingContext,
        const TScheduler::TCtxNodeHeartbeatPtr& rpcContext);

    void OnJobFinished(const TJobPtr& job, TJobStatus* status);
    void DoAbortJob(const TJobPtr& job, TJobStatus* status);
    void OnJobRunning(const TJobPtr& job, TJobStatus* status);
    void DoAbandonJob(const TJobPtr& job);

    void UpdateProfilingCounter(const TJobPtr& job, int value);

    void SetAllocationState(const TJobPtr& job, EAllocationState state);

    void RegisterJob(const TJobPtr& job);
    void UnregisterJob(const TJobPtr& job, bool enableLogging = true);

    void SetJobWaitingForConfirmation(const TJobPtr& job);
    void ResetJobWaitingForConfirmation(const TJobPtr& job);

    void AddRecentlyFinishedJob(const TJobPtr& job);
    void RemoveRecentlyFinishedJob(TJobId jobId);

    void SetOperationJobsReleaseDeadline(TOperationState* operationState);

    void ProcessPreemptedJob(NJobTrackerClient::NProto::TRspHeartbeat* response, const TJobPtr& job, TDuration interruptTimeout);
    void PreemptJob(const TJobPtr& job, NProfiling::TCpuDuration interruptTimeout);
    NJobTrackerClient::TJobToAbort BuildPreemptedJobAbortAttributes(const TJobPtr& job) const;

    void SendPreemptedJobToNode(
        NJobTrackerClient::NProto::TRspHeartbeat* response,
        const TJobPtr& job,
        TDuration interruptTimeout,
        bool isJobInterruptible) const;

    void DoInterruptJob(
        const TJobPtr& job,
        EInterruptReason reason,
        NProfiling::TCpuDuration interruptTimeout = 0,
        const std::optional<TString>& interruptUser = std::nullopt);

    TExecNodePtr FindNodeByJob(TJobId jobId);

    TJobPtr FindJob(TJobId jobId, const TExecNodePtr& node);
    TJobPtr FindJob(TJobId jobId);
    TJobPtr GetJobOrThrow(TJobId jobId);

    NJobProberClient::TJobProberServiceProxy CreateJobProberProxy(const TJobPtr& job);

    TOperationState* FindOperationState(TOperationId operationId) noexcept;
    const TOperationState* FindOperationState(TOperationId operationId) const noexcept;
    TOperationState& GetOperationState(TOperationId operationId) noexcept;
    const TOperationState& GetOperationState(TOperationId operationId) const noexcept;

    void BuildNodeYson(const TExecNodePtr& node, NYTree::TFluentMap consumer);

    void UpdateNodeState(
        const TExecNodePtr& execNode,
        NNodeTrackerClient::ENodeState newState,
        ENodeState newSchedulerState,
        const TError& error = TError());

    void RemoveOperationScheduleJobEntries(TOperationId operationId);

    void SetFinishedState(const TJobPtr& job);
};

DEFINE_REFCOUNTED_TYPE(TNodeShard)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
