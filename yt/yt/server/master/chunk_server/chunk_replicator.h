#pragma once

#include "private.h"
#include "chunk.h"
#include "job_controller.h"

#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/server/lib/misc/max_min_balancer.h>

#include <yt/yt/server/master/incumbent_server/incumbent.h>

#include <yt/yt/server/master/node_tracker_server/data_center.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>

#include <yt/yt/ytlib/node_tracker_client/proto/node_tracker_service.pb.h>

#include <yt/yt/core/concurrency/public.h>

#include <yt/yt/library/erasure/impl/public.h>

#include <yt/yt/library/profiling/producer.h>

#include <yt/yt/core/misc/error.h>
#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/property.h>

#include <yt/yt/core/profiling/timing.h>

#include <functional>
#include <deque>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunkReplicator
    : public IJobController
    , public NIncumbentServer::TIncumbentBase
{
public:
    TChunkReplicator(
        TChunkManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap,
        TChunkPlacementPtr chunkPlacement,
        IJobRegistryPtr jobRegistry);

    ~TChunkReplicator();

    void OnEpochStarted();
    void OnEpochFinished();

    void OnLeadingStarted();
    void OnLeadingFinished();

    void OnIncumbencyStarted(int shardIndex) override;
    void OnIncumbencyFinished(int shardIndex) override;

    NIncumbentClient::EIncumbentType GetType() const override;

    void OnNodeDisposed(TNode* node);
    void OnNodeUnregistered(TNode* node);

    // 'On all of the media' chunk states. E.g. LostChunks contain chunks that
    // have been lost on all of the media.
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, LostChunks);
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, LostVitalChunks);
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, DataMissingChunks);
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, ParityMissingChunks);
    DEFINE_BYREF_RO_PROPERTY(TOldestPartMissingChunkSet, OldestPartMissingChunks);
    // Medium-wise unsafely placed chunks: all replicas are on transient media
    // (and requisitions of these chunks demand otherwise).
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, PrecariousChunks);
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, PrecariousVitalChunks);

    // 'On any medium'. E.g. UnderreplicatedChunks contain chunks that are
    // underreplicated on at least one medium.
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, UnderreplicatedChunks);
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, OverreplicatedChunks);
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, QuorumMissingChunks);
    // Rack-wise unsafely placed chunks.
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, UnsafelyPlacedChunks);
    DEFINE_BYREF_RO_PROPERTY(TShardedChunkSet, InconsistentlyPlacedChunks);

    void OnChunkDestroyed(TChunk* chunk);
    void OnReplicaRemoved(
        TChunkLocation* chunkLocation,
        TChunkPtrWithReplicaIndex chunkWithIndexes,
        ERemoveReplicaReason reason);

    void ScheduleChunkRefresh(TChunk* chunk);
    void ScheduleNodeRefresh(TNode* node);
    void ScheduleGlobalChunkRefresh();

    void ScheduleRequisitionUpdate(TChunk* chunk);
    void ScheduleRequisitionUpdate(TChunkList* chunkList);

    void TouchChunk(TChunk* chunk);

    TMediumMap<EChunkStatus> ComputeChunkStatuses(TChunk* chunk);
    ECrossMediumChunkStatus ComputeCrossMediumChunkStatus(TChunk* chunk);

    bool IsReplicatorEnabled();
    bool IsSealerEnabled();
    bool IsRefreshEnabled();
    bool IsRequisitionUpdateEnabled();

    //! Returns true if chunk replicator is processing
    //! shard |chunk| belongs to and false otherwise.
    bool ShouldProcessChunk(TChunk* chunk);
    bool ShouldProcessChunk(TChunkId chunkId);

    TJobEpoch GetJobEpoch(TChunk* chunk) const;

    bool IsDurabilityRequired(TChunk* chunk) const;

    void OnProfiling(NProfiling::TSensorBuffer* buffer);

    // IJobController implementation.
    void ScheduleJobs(EJobType jobType, IJobSchedulingContext* context) override;

    void OnJobWaiting(const TJobPtr& job, IJobControllerCallbacks* callbacks) override;
    void OnJobRunning(const TJobPtr& job, IJobControllerCallbacks* callbacks) override;

    void OnJobCompleted(const TJobPtr& job) override;
    void OnJobAborted(const TJobPtr& job) override;
    void OnJobFailed(const TJobPtr& job) override;

private:
    struct TPerMediumChunkStatistics
    {
        TPerMediumChunkStatistics();

        EChunkStatus Status;

        //! Number of active replicas, per each replica index.
        int ReplicaCount[ChunkReplicaIndexBound];

        //! Number of decommissioned replicas, per each replica index.
        int DecommissionedReplicaCount[ChunkReplicaIndexBound];

        //! Indexes of replicas whose replication is advised.
        TCompactVector<int, TypicalReplicaCount> ReplicationIndexes;

        //! Decommissioned replicas whose removal is advised.
        // NB: there's no actual need to have medium index in context of this
        // per-medium class. This is just for convenience.
        TChunkLocationPtrWithReplicaIndexList DecommissionedRemovalReplicas;
         //! Indexes of replicas whose removal is advised for balancing.
        TCompactVector<int, TypicalReplicaCount> BalancingRemovalIndexes;

        //! Any replica that violates failure domain placement.
        TChunkLocationPtrWithReplicaInfo UnsafelyPlacedReplica;

        //! Missing chunk replicas for CRP-enabled chunks.
        TNodePtrWithReplicaAndMediumIndexList MissingReplicas;
    };

    struct TChunkStatistics
    {
        TMediumMap<TPerMediumChunkStatistics> PerMediumStatistics;
        ECrossMediumChunkStatus Status = ECrossMediumChunkStatus::None;
    };

    // This is for a simple optimization: updating adjacent chunks in the
    // requisition update queue is likely to produce identical results.
    struct TChunkRequisitionCache
    {
        TChunk::TParents LastChunkParents;
        std::optional<TChunkRequisition> LastChunkUpdatedRequisition;
        std::optional<TChunkRequisition> LastErasureChunkUpdatedRequisition;
    };

    TChunkRequisitionCache ChunkRequisitionCache_;

    TEphemeralRequisitionRegistry TmpRequisitionRegistry_;

    TInstant LastPerNodeProfilingTime_;

    const TChunkManagerConfigPtr Config_;
    NCellMaster::TBootstrap* const Bootstrap_;
    const TChunkPlacementPtr ChunkPlacement_;
    const IJobRegistryPtr JobRegistry_;

    std::array<TJobEpoch, ChunkShardCount> JobEpochs_;

    NConcurrency::TPeriodicExecutorPtr RefreshExecutor_;
    const std::unique_ptr<TChunkScanner> BlobRefreshScanner_;
    const std::unique_ptr<TChunkScanner> JournalRefreshScanner_;

    NConcurrency::TPeriodicExecutorPtr RequisitionUpdateExecutor_;
    const std::unique_ptr<TChunkScanner> BlobRequisitionUpdateScanner_;
    const std::unique_ptr<TChunkScanner> JournalRequisitionUpdateScanner_;

    NConcurrency::TPeriodicExecutorPtr FinishedRequisitionTraverseFlushExecutor_;

    // Contains the chunk list ids for which requisition update traversals
    // have finished. These confirmations are batched and then flushed.
    std::vector<TChunkListId> ChunkListIdsWithFinishedRequisitionTraverse_;

    //! A queue of chunks to be repaired on each medium.
    //! Replica index is always GenericChunkReplicaIndex.
    //! Medium index designates the medium where the chunk is missing some of
    //! its parts. It's always equal to the index of its queue.
    //! In each queue, a single chunk may only appear once.
    // NB: Queues are not modified when a replicator shard is disabled, so one should
    // take care of such a chunks.
    std::array<TChunkRepairQueue, MaxMediumCount> MissingPartChunkRepairQueues_ = {};
    std::array<TChunkRepairQueue, MaxMediumCount> DecommissionedPartChunkRepairQueues_ = {};
    TDecayingMaxMinBalancer<int, double> MissingPartChunkRepairQueueBalancer_;
    TDecayingMaxMinBalancer<int, double> DecommissionedPartChunkRepairQueueBalancer_;

    NConcurrency::TPeriodicExecutorPtr EnabledCheckExecutor_;

    std::vector<TChunkId> ChunkIdsPendingEndorsementRegistration_;

    TEnumIndexedVector<EJobType, i64> MisscheduledJobs_;

    std::optional<bool> Enabled_;

    std::bitset<ChunkShardCount> RefreshRunning_;
    bool RequisitionUpdateRunning_ = false;

    TInstant LastActiveShardSetUpdateTime_ = TInstant::Zero();

    void ScheduleReplicationJobs(IJobSchedulingContext* context);
    void ScheduleRemovalJobs(IJobSchedulingContext* context);
    void ScheduleRepairJobs(IJobSchedulingContext* context);

    bool TryScheduleReplicationJob(
        IJobSchedulingContext* context,
        TChunkPtrWithReplicaAndMediumIndex chunkWithIndex,
        TMedium* targetMedium,
        TNodeId targetNodeId);
    bool TryScheduleBalancingJob(
        IJobSchedulingContext* context,
        TChunkPtrWithReplicaAndMediumIndex chunkWithIndex,
        double maxFillCoeff);
    bool TryScheduleRemovalJob(
        IJobSchedulingContext* context,
        const NChunkClient::TChunkIdWithIndexes& chunkIdWithIndex,
        TRealChunkLocation* location);
    bool TryScheduleRepairJob(
        IJobSchedulingContext* context,
        EChunkRepairQueue repairQueue,
        TChunkPtrWithReplicaAndMediumIndex chunkWithIndexes);

    void OnRefresh();
    void RefreshChunk(TChunk* chunk);

    void ResetChunkStatus(TChunk* chunk);
    void RemoveChunkFromQueuesOnRefresh(TChunk* chunk);
    void RemoveChunkFromQueuesOnDestroy(TChunk* chunk);

    void MaybeRememberPartMissingChunk(TChunk* chunk);

    TChunkStatistics ComputeChunkStatistics(const TChunk* chunk);

    TChunkStatistics ComputeRegularChunkStatistics(const TChunk* chunk);
    void ComputeRegularChunkStatisticsForMedium(
        TPerMediumChunkStatistics& result,
        const TChunk* chunk,
        TReplicationPolicy replicationPolicy,
        int replicaCount,
        int decommissionedReplicaCount,
        const TChunkLocationPtrWithReplicaIndexList& decommissionedReplicas,
        bool hasSealedReplica,
        bool totallySealed,
        TChunkLocationPtrWithReplicaInfo unsafelyPlacedReplica,
        TChunkLocationPtrWithReplicaIndex inconsistentlyPlacedReplica,
        const TNodePtrWithReplicaAndMediumIndexList& missingReplicas);

    void ComputeRegularChunkStatisticsCrossMedia(
        TChunkStatistics& result,
        const TChunk* chunk,
        int totalReplicaCount,
        int totalDecommissionedReplicaCount,
        bool hasSealedReplicas,
        bool precarious,
        bool allMediaTransient,
        const TCompactVector<int, MaxMediumCount>& mediaOnWhichLost,
        bool hasMediumOnWhichPresent,
        bool hasMediumOnWhichUnderreplicated,
        bool hasMediumOnWhichSealedMissing);

    TChunkStatistics ComputeErasureChunkStatistics(const TChunk* chunk);
    void ComputeErasureChunkStatisticsForMedium(
        TPerMediumChunkStatistics& result,
        NErasure::ICodec* codec,
        TReplicationPolicy replicationPolicy,
        const std::array<TChunkLocationList, ChunkReplicaIndexBound>& decommissionedReplicas,
        TChunkLocationPtrWithReplicaInfo unsafelyPlacedSealedReplica,
        NErasure::TPartIndexSet& erasedIndexes,
        bool totallySealed);
    void ComputeErasureChunkStatisticsCrossMedia(
        TChunkStatistics& result,
        const TChunk* chunk,
        NErasure::ICodec* codec,
        bool allMediaTransient,
        bool allMediaDataPartsOnly,
        const TMediumMap<NErasure::TPartIndexSet>& mediumToErasedIndexes,
        const TMediumSet& activeMedia,
        const NErasure::TPartIndexSet& replicaIndexes,
        bool totallySealed);

    bool IsReplicaDecommissioned(TChunkLocation* replica);

    //! Same as corresponding #TChunk method but
    //!   - replication factors are capped by medium-specific bounds;
    //!   - additional entries may be introduced if the chunk has replicas
    //!     stored on a medium it's not supposed to have replicas on.
    TChunkReplication GetChunkAggregatedReplication(const TChunk* chunk) const;

    //! Same as corresponding #TChunk method but the result is capped by the medium-specific bound.
    int GetChunkAggregatedReplicationFactor(const TChunk* chunk, int mediumIndex);

    void OnRequisitionUpdate();
    void ComputeChunkRequisitionUpdate(TChunk* chunk, NProto::TReqUpdateChunkRequisition* request);

    void ClearChunkRequisitionCache();
    bool CanServeRequisitionFromCache(const TChunk* chunk);
    TChunkRequisition GetRequisitionFromCache(const TChunk* chunk);
    void CacheRequisition(const TChunk* chunk, const TChunkRequisition& requisition);

    //! Computes the actual requisition the chunk must have.
    TChunkRequisition ComputeChunkRequisition(const TChunk* chunk);

    void ConfirmChunkListRequisitionTraverseFinished(TChunkList* chunkList);
    void OnFinishedRequisitionTraverseFlush();

    //! Follows upward parent links.
    //! Stops when some owning nodes are discovered or parents become ambiguous.
    TChunkList* FollowParentLinks(TChunkList* chunkList);

    void AddToChunkRepairQueue(TChunkPtrWithMediumIndex chunkWithIndexes, EChunkRepairQueue queue);
    void TouchChunkInRepairQueues(TChunk* chunk);
    void RemoveFromChunkRepairQueues(TChunk* chunkWithIndexes);

    void FlushEndorsementQueue();

    void OnCheckEnabled();
    void OnCheckEnabledPrimary();
    void OnCheckEnabledSecondary();

    void TryRescheduleChunkRemoval(const TJobPtr& unsucceededJob);

    TChunkRequisitionRegistry* GetChunkRequisitionRegistry() const;

    const std::unique_ptr<TChunkScanner>& GetChunkRefreshScanner(TChunk* chunk) const;
    const std::unique_ptr<TChunkScanner>& GetChunkRequisitionUpdateScanner(TChunk* chunk) const;

    TChunkRepairQueue& ChunkRepairQueue(int mediumIndex, EChunkRepairQueue queue);
    std::array<TChunkRepairQueue, MaxMediumCount>& ChunkRepairQueues(EChunkRepairQueue queue);
    TDecayingMaxMinBalancer<int, double>& ChunkRepairQueueBalancer(EChunkRepairQueue queue);

    TMediumMap<TNodeList> GetChunkConsistentPlacementNodes(const TChunk* chunk);

    void RemoveFromChunkReplicationQueues(
        TNode* node,
        TChunkPtrWithReplicaAndMediumIndex chunkWithIndexes);
    void RemoveChunkFromPullReplicationSet(const TJobPtr& job);
    void UnrefChunkBeingPulled(
        TNodeId nodeId,
        TChunkId chunkId,
        int mediumIndex);

    const TDynamicChunkManagerConfigPtr& GetDynamicConfig() const;
    void OnDynamicConfigChanged(NCellMaster::TDynamicClusterConfigPtr /*oldConfig*/);
    bool IsConsistentChunkPlacementEnabled() const;
    bool UsePullReplication(TChunk* chunk) const;

    void StartRefresh(int shardIndex);
    void StopRefresh(int shardIndex);

    void StartRequisitionUpdate();
    void StopRequisitionUpdate();
};

DEFINE_REFCOUNTED_TYPE(TChunkReplicator)

////////////////////////////////////////////////////////////////////////////////

TRefreshEpoch GetRefreshEpoch(int shardIndex);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
