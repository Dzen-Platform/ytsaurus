#pragma once

#include "private.h"
#include "chunk_merger_traversal_info.h"
#include "chunk_replacer.h"
#include "job.h"
#include "job_controller.h"

#include <yt/yt/server/master/chunk_server/proto/chunk_merger.pb.h>

#include <yt/yt/server/master/cell_master/automaton.h>
#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/server/master/security_server/account.h>

#include <yt/yt/server/master/transaction_server/transaction.h>

#include <yt/yt/server/master/cypress_server/public.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>

#include <yt/yt/library/profiling/producer.h>

#include <queue>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

// Items of this enum are compared by <, greater values always
// override smaller ones.
DEFINE_ENUM(EMergeSessionResult,
    ((None)              (0))
    // Everything went OK, no need to reschedule merge.
    ((OK)                (1))
    // Some jobs failed, reschedule.
    ((TransientFailure)  (2))
    // Some jobs failed, but there was no chance to succeed (typically node is dead), no need to reschedule.
    ((PermanentFailure)  (3))
);

struct TMergeJobInfo
{
    TJobId JobId;
    int JobIndex;
    // TODO(shakurov): ephemeral ptr?
    NCypressClient::TObjectId NodeId;
    TChunkListId ParentChunkListId;
    TChunkListId RootChunkListId;

    std::vector<TChunkId> InputChunkIds;
    TChunkId OutputChunkId;

    NChunkClient::EChunkMergerMode MergeMode;
};

struct TChunkMergerSession
{
    THashMap<NCypressClient::TObjectId, THashSet<TJobId>> ChunkListIdToRunningJobs;
    THashMap<NCypressClient::TObjectId, std::vector<TMergeJobInfo>> ChunkListIdToCompletedJobs;
    EMergeSessionResult Result = EMergeSessionResult::None;

    TChunkMergerTraversalInfo TraversalInfo;
};

////////////////////////////////////////////////////////////////////////////////

class TMergeJob
    : public TJob
{
public:
    DEFINE_BYREF_RO_PROPERTY(TNodePtrWithIndexesList, TargetReplicas);

public:
    using TChunkVector = TCompactVector<TChunk*, 16>;
    TMergeJob(
        TJobId jobId,
        NNodeTrackerServer::TNode* node,
        NChunkClient::TChunkIdWithIndexes chunkIdWithIndexes,
        TChunkVector inputChunks,
        NChunkClient::NProto::TChunkMergerWriterOptions chunkMergerWriterOptions,
        TNodePtrWithIndexesList targetReplicas);

    void FillJobSpec(NCellMaster::TBootstrap* bootstrap, NJobTrackerClient::NProto::TJobSpec* jobSpec) const override;

private:
    const TChunkVector InputChunks_;
    const NChunkClient::NProto::TChunkMergerWriterOptions ChunkMergerWriterOptions_;

    static NNodeTrackerClient::NProto::TNodeResources GetResourceUsage(const TChunkVector& inputChunks);
};

DECLARE_REFCOUNTED_TYPE(TMergeJob)
DEFINE_REFCOUNTED_TYPE(TMergeJob)

////////////////////////////////////////////////////////////////////////////////

struct IMergeChunkVisitorHost
    : public virtual TRefCounted
{
    virtual void RegisterJobAwaitingChunkCreation(
        TJobId jobId,
        NChunkClient::EChunkMergerMode mode,
        int jobIndex,
        NCypressClient::TObjectId nodeId,
        TChunkListId parentChunkListId,
        std::vector<TChunkId> inputChunkIds) = 0;
    virtual void OnTraversalFinished(
        NCypressClient::TObjectId nodeId,
        EMergeSessionResult result,
        TChunkMergerTraversalInfo traversalInfo) = 0;
};

class TChunkMerger
    : public NCellMaster::TMasterAutomatonPart
    , public virtual ITypedJobController<TMergeJob>
    , public virtual IMergeChunkVisitorHost
{
public:
    explicit TChunkMerger(NCellMaster::TBootstrap* bootstrap);

    void Initialize();

    // TODO(shakurov): rename to "chunkOwnerId"
    void ScheduleMerge(NCypressClient::TObjectId nodeId);
    // TODO(shakurov): rename to "trunkChunkOwner"
    void ScheduleMerge(TChunkOwnerBase* chunkOwner);

    bool IsNodeBeingMerged(NCypressClient::TObjectId nodeId) const;

    void OnProfiling(NProfiling::TSensorBuffer* buffer) const;

    // IJobController implementation.
    void ScheduleJobs(IJobSchedulingContext* context) override;

    void OnJobWaiting(const TMergeJobPtr& job, IJobControllerCallbacks* callbacks) override;
    void OnJobRunning(const TMergeJobPtr& job, IJobControllerCallbacks* callbacks) override;

    void OnJobCompleted(const TMergeJobPtr& job) override;
    void OnJobAborted(const TMergeJobPtr& job) override;
    void OnJobFailed(const TMergeJobPtr& job) override;

private:
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

    NCellMaster::TBootstrap* const Bootstrap_;

    IChunkReplacerCallbacksPtr ChunkReplacerCallbacks_;

    NConcurrency::TPeriodicExecutorPtr ScheduleExecutor_;
    NConcurrency::TPeriodicExecutorPtr ChunkCreatorExecutor_;
    NConcurrency::TPeriodicExecutorPtr StartTransactionExecutor_;
    NConcurrency::TPeriodicExecutorPtr FinalizeSessionExecutor_;

    bool Enabled_ = false;

    // Persistent fields.
    TTransactionId TransactionId_;
    TTransactionId PreviousTransactionId_;
    THashSet<NCypressClient::TObjectId> NodesBeingMerged_;
    i64 ConfigVersion_ = 0;

    i64 ChunkReplacementSucceded_ = 0;
    i64 ChunkReplacementFailed_ = 0;
    i64 ChunkCountSaving_ = 0;

    TEnumIndexedVector<NChunkClient::EChunkMergerMode, i64> CompletedJobCountPerMode_;
    i64 AutoMergeFallbackJobCount_ = 0;

    THashMap<NCypressClient::TObjectId, TChunkMergerSession> RunningSessions_;


    // TODO(shakurov): ephemeral ptrs?
    using TNodeQueue = std::queue<NCypressClient::TObjectId>;
    // Per-account queue. All touched tables start here.
    THashMap<NObjectServer::TEphemeralObjectPtr<NSecurityServer::TAccount>, TNodeQueue> AccountToNodeQueue_;

    // After traversal, before creating chunks. We want to batch chunk creation,
    // so we do not create them right away.
    std::queue<TMergeJobInfo> JobsAwaitingChunkCreation_;

    // Chunk creation in progress. Stores i64 -> TMergeJobInfo to find the right TMergeJobInfo
    // after creating chunk.
    THashMap<TJobId, TMergeJobInfo> JobsUndergoingChunkCreation_;

    // After creating chunks, before scheduling (waiting for node heartbeat to schedule jobs).
    std::queue<TMergeJobInfo> JobsAwaitingNodeHeartbeat_;

    // Scheduled jobs (waiting for node heartbeat with job result).
    THashMap<TJobId, TMergeJobInfo> RunningJobs_;

    // Already merged nodes waiting to be erased from NodesBeingMerged_.
    struct TMergeSessionResult
    {
        NCypressClient::TObjectId NodeId;
        EMergeSessionResult Result;
        TChunkMergerTraversalInfo TraversalInfo;
    };
    std::queue<TMergeSessionResult> SessionsAwaitingFinalizaton_;

    void OnLeaderActive() override;
    void OnStopLeading() override;

    void RegisterSession(TChunkOwnerBase* chunkOwner);
    void RegisterSessionTransient(TChunkOwnerBase* chunkOwner);
    void FinalizeJob(
        TMergeJobInfo jobInfo,
        EMergeSessionResult result);

    void RegisterJobAwaitingChunkCreation(
        TJobId jobId,
        NChunkClient::EChunkMergerMode mode,
        int jobIndex,
        NCypressClient::TObjectId nodeId,
        TChunkListId parentChunkListId,
        std::vector<TChunkId> inputChunkIds) override;
    void OnTraversalFinished(
        NCypressClient::TObjectId nodeId,
        EMergeSessionResult result,
        TChunkMergerTraversalInfo traversalInfo) override;

    void ScheduleSessionFinalization(NCypressClient::TObjectId nodeId, EMergeSessionResult result);
    void FinalizeSessions();

    void FinalizeReplacement(
        NCypressClient::TObjectId nodeId,
        TChunkListId chunkListId,
        EMergeSessionResult result);

    void Clear() override;

    void ResetTransientState();

    bool IsMergeTransactionAlive() const;

    bool CanScheduleMerge(TChunkOwnerBase* chunkOwner) const;

    void StartMergeTransaction();

    void OnTransactionAborted(NTransactionServer::TTransaction* transaction);

    void ProcessTouchedNodes();

    void CreateChunks();

    bool TryScheduleMergeJob(
        IJobSchedulingContext* context,
        const TMergeJobInfo& jobInfo);

    void ScheduleReplaceChunks(
        NCypressClient::TObjectId nodeId,
        TChunkListId parentChunkListId,
        std::vector<TMergeJobInfo>* jobInfos);

    void OnJobFinished(const TMergeJobPtr& job);

    const TDynamicChunkMergerConfigPtr& GetDynamicConfig() const;
    void OnDynamicConfigChanged(NCellMaster::TDynamicClusterConfigPtr /*oldConfig*/ = nullptr);

    TChunkOwnerBase* FindChunkOwner(NCypressClient::TObjectId nodeId);

    void HydraCreateChunks(NProto::TReqCreateChunks* request);
    void HydraReplaceChunks(NProto::TReqReplaceChunks* request);
    void HydraStartMergeTransaction(NProto::TReqStartMergeTransaction* request);
    void HydraFinalizeChunkMergeSessions(NProto::TReqFinalizeChunkMergeSessions* request);

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);
};

DEFINE_REFCOUNTED_TYPE(TChunkMerger)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
