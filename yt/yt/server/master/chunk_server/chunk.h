#pragma once

#include "public.h"
#include "chunk_requisition.h"
#include "chunk_replica.h"
#include "chunk_tree.h"

#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/server/master/security_server/cluster_resources.h>

#include <yt/yt/ytlib/chunk_client/proto/chunk_info.pb.h>
#include <yt/yt/ytlib/chunk_client/proto/chunk_service.pb.h>

#include <yt/yt_proto/yt/client/chunk_client/proto/chunk_meta.pb.h>

#include <yt/yt/library/erasure/public.h>

#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/format.h>
#include <yt/yt/core/misc/property.h>
#include <yt/yt/core/misc/ref_tracked.h>
#include <yt/yt/core/misc/small_flat_map.h>
#include <yt/yt/core/misc/small_vector.h>
#include <yt/yt/core/misc/intrusive_linked_list.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

struct TChunkExportData
{
    ui32 RefCounter = 0 ;
    TChunkRequisitionIndex ChunkRequisitionIndex = EmptyChunkRequisitionIndex;
};

static_assert(sizeof(TChunkExportData) == 8, "sizeof(TChunkExportData) != 8");

using TChunkExportDataList = std::array<TChunkExportData, NObjectClient::MaxSecondaryMasterCells>;

////////////////////////////////////////////////////////////////////////////////

struct TChunkDynamicData
    : public NObjectServer::TObjectDynamicData
{
    using TMediumToRepairQueueIterator = TSmallFlatMap<int, TChunkRepairQueueIterator, 2>;

    //! The time since this chunk needs repairing.
    NProfiling::TCpuInstant EpochPartLossTime = {};

    //! Indicates that certain background scans were scheduled for this chunk.
    EChunkScanKind EpochScanFlags = EChunkScanKind::None;

    //! Indicates for which epoch #EpochScanFlags and #EpochPartLossTime are valid.
    NObjectServer::TEpoch Epoch = 0;

    //! For each medium, contains a valid iterator for those chunks belonging to the repair queue
    //! and null (default iterator value) for others.
    TMediumToRepairQueueIterator MissingPartRepairQueueIterators;
    TMediumToRepairQueueIterator DecommissionedPartRepairQueueIterators;

    //! The job that is currently scheduled for this chunk (at most one).
    TJobPtr Job;

    //! All blob chunks are linked via this node, as are all journal
    //! chunks. (The two lists are separate.)
    TIntrusiveLinkedListNode<TChunk> LinkedListNode;
};

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EChunkRepairQueue,
    ((Missing)           (0))
    ((Decommissioned)    (1))
);

////////////////////////////////////////////////////////////////////////////////

class TChunk
    : public TChunkTree
    , public TRefTracked<TChunk>
{
public:
    DEFINE_BYREF_RW_PROPERTY(NChunkClient::NProto::TChunkMeta, ChunkMeta);
    DEFINE_BYREF_RW_PROPERTY(NChunkClient::NProto::TChunkInfo, ChunkInfo);
    DEFINE_BYREF_RW_PROPERTY(NChunkClient::NProto::TMiscExt, MiscExt);

    // This map is typically small, e.g. has the size of 1.
    using TParents = TSmallFlatMap<TChunkTree*, int, TypicalChunkParentCount>;
    DEFINE_BYREF_RO_PROPERTY(TParents, Parents);

    // Limits the lifetime of staged chunks. Useful for cleaning up abandoned staged chunks.
    DEFINE_BYVAL_RW_PROPERTY(TInstant, ExpirationTime);
    DEFINE_BYVAL_RW_PROPERTY(std::optional<TChunkExpirationMapIterator>, ExpirationIterator);

public:
    explicit TChunk(TChunkId id);

    TChunkDynamicData* GetDynamicData() const;

    TChunkTreeStatistics GetStatistics() const;

    //! Get disk size of a single part of the chunk.
    /*!
     *  For a non-erasure chunk, simply returns its size
     *  (same as ChunkInfo().disk_space()).
     *  For an erasure chunk, returns that size divided by the number of parts
     *  used by the codec.
     */
    i64 GetPartDiskSpace() const;

    virtual TString GetLowercaseObjectName() const override;
    virtual TString GetCapitalizedObjectName() const override;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    void AddParent(TChunkTree* parent);
    void RemoveParent(TChunkTree* parent);
    int GetParentCount() const;
    bool HasParents() const;

    using TCachedReplicas = THashSet<TNodePtrWithIndexes>;
    const TCachedReplicas& CachedReplicas() const;

    using TStoredReplicas = TNodePtrWithIndexesList;
    const TStoredReplicas& StoredReplicas() const;

    using TLastSeenReplicas = std::array<TNodeId, LastSeenReplicaCount>;
    //! For non-erasure chunks, contains a FIFO queue of seen replicas; its tail position is kept in #CurrentLastSeenReplicaIndex_.
    //! For erasure chunks, this array is directly addressed by replica indexes; at most one replica is kept per part.
    const TLastSeenReplicas& LastSeenReplicas() const;

    void AddReplica(TNodePtrWithIndexes replica, const TMedium* medium);
    void RemoveReplica(TNodePtrWithIndexes replica, const TMedium* medium);
    TNodePtrWithIndexesList GetReplicas() const;

    void ApproveReplica(TNodePtrWithIndexes replica);

    void Confirm(
        NChunkClient::NProto::TChunkInfo* chunkInfo,
        NChunkClient::NProto::TChunkMeta* chunkMeta);

    bool GetMovable() const;
    void SetMovable(bool value);

    bool GetOverlayed() const;
    void SetOverlayed(bool value);

    bool IsConfirmed() const;

    bool GetScanFlag(EChunkScanKind kind, NObjectServer::TEpoch epoch) const;
    void SetScanFlag(EChunkScanKind kind, NObjectServer::TEpoch epoch);
    void ClearScanFlag(EChunkScanKind kind, NObjectServer::TEpoch epoch);
    TChunk* GetNextScannedChunk() const;

    std::optional<NProfiling::TCpuInstant> GetPartLossTime(NObjectServer::TEpoch epoch) const;
    void SetPartLossTime(NProfiling::TCpuInstant partLossTime, NObjectServer::TEpoch epoch);
    void ResetPartLossTime(NObjectServer::TEpoch epoch);

    TChunkRepairQueueIterator GetRepairQueueIterator(int mediumIndex, EChunkRepairQueue queue) const;
    void SetRepairQueueIterator(int mediumIndex, EChunkRepairQueue queue, TChunkRepairQueueIterator value);

    bool IsJobScheduled() const;
    TJobPtr GetJob() const;
    void SetJob(TJobPtr job);

    //! Refs all (local, external and aggregated) requisitions this chunk uses.
    //! Supposed to be called soon after the chunk is constructed or loaded.
    void RefUsedRequisitions(TChunkRequisitionRegistry* registry) const;

    //! A reciprocal to the above. Called at chunk destruction.
    void UnrefUsedRequisitions(
        TChunkRequisitionRegistry* registry,
        const NObjectServer::TObjectManagerPtr& objectManager) const;

    TChunkRequisitionIndex GetLocalRequisitionIndex() const;
    void SetLocalRequisitionIndex(
        TChunkRequisitionIndex requisitionIndex,
        TChunkRequisitionRegistry* registry,
        const NObjectServer::TObjectManagerPtr& objectManager);

    //! Prerequisite: IsExportedToCell(cellIndex).
    TChunkRequisitionIndex GetExternalRequisitionIndex(int cellIndex) const;
    //! Prerequisite: IsExportedToCell(cellIndex).
    void SetExternalRequisitionIndex(
        int cellIndex,
        TChunkRequisitionIndex requisitionIndex,
        TChunkRequisitionRegistry* registry,
        const NObjectServer::TObjectManagerPtr& objectManager);

    //! Returns chunk's requisition aggregated from local and external values.
    //! If aggregating them would result in an empty requisition, returns the most
    //! recent non-empty aggregated requisition.
    //! For semantics of aggregation, see #TChunkRequisition::operator|=().
    const TChunkRequisition& GetAggregatedRequisition(const TChunkRequisitionRegistry* registry) const;

    //! Returns chunk's replication aggregated from local and external values.
    //! For semantics of aggregation, see #TChunkReplication::operator|=().
    /*!
     *  NB: by default only COMMITTED OWNERS affect this. If the chunk has no
     *  committed owners, then non-committed ones are taken into account.
     *
     *  If there're no owners at all, the returned value is the most recent
     *  non-empty aggregated replication.
     */
    const TChunkReplication& GetAggregatedReplication(const TChunkRequisitionRegistry* registry) const;

    //! Returns the replication factor for the specified medium aggregated from
    //! the local and the external values. See #GetAggregatedReplication().
    int GetAggregatedReplicationFactor(int mediumIndex, const TChunkRequisitionRegistry* registry) const;

    int GetReadQuorum() const;
    void SetReadQuorum(int value);

    int GetWriteQuorum() const;
    void SetWriteQuorum(int value);

    NErasure::ECodec GetErasureCodec() const;
    void SetErasureCodec(NErasure::ECodec value);

    //! Returns |true| iff this is an erasure chunk.
    bool IsErasure() const;

    //! Returns |true| iff this is a journal chunk.
    bool IsJournal() const;

    //! Returns |true| iff the chunk can be read immediately, i.e. without repair.
    /*!
     *  For regular (non-erasure) chunk this is equivalent to the existence of any replica.
     *  For erasure chunks this is equivalent to the existence of replicas for all data parts.
     */
    bool IsAvailable() const;

    //! Returns |true| iff this is a sealed journal chunk.
    //! For blob chunks always returns |true|.
    bool IsSealed() const;

    //! Returns the number of rows in a sealed chunk.
    i64 GetPhysicalSealedRowCount() const;

    //! Marks the chunk as sealed, i.e. sets its ultimate row count, data size etc.
    void Seal(const NChunkClient::NProto::TChunkSealInfo& info);

    //! For journal chunks, returns true iff the chunk is sealed.
    //! For blob chunks, return true iff the chunk is confirmed.
    bool IsDiskSizeFinal() const;

    //! Returns the maximum number of replicas that can be stored in the same
    //! rack without violating the availability guarantees.
    /*!
     *  As #GetAggregatedReplication(), takes into account only committed owners of
     *  this chunk, if there're any. Otherwise falls back to all owners.
     *
     *  \param replicationFactorOverride An override for replication factor;
     *  used when one wants to upload fewer replicas but still guarantee placement safety.
     */
    int GetMaxReplicasPerRack(
        int mediumIndex,
        std::optional<int> replicationFactorOverride,
        const TChunkRequisitionRegistry* registry) const;

    //! Returns the export data w.r.t. to a cell with a given #index.
    /*!
     *  It's ok to call this even if !IsExportedToCell(cellIndex).
     *
     *  \see #TMulticellManager::GetRegisteredMasterCellIndex
     */
    TChunkExportData GetExportData(int cellIndex) const;

    //! Same as GetExportData(cellIndex).RefCounter != 0.
    bool IsExportedToCell(int cellIndex) const;

    int ExportCounter() const;

    //! Increments export ref counter.
    void Export(int cellIndex, TChunkRequisitionRegistry* registry);

    //! Decrements export ref counter.
    void Unexport(
        int cellIndex,
        int importRefCounter,
        TChunkRequisitionRegistry* registry,
        const NObjectServer::TObjectManagerPtr& objectManager);

    i64 GetMasterMemoryUsage() const;

private:
    ui8 ReadQuorum_ = 0;
    ui8 WriteQuorum_ = 0;
    NErasure::ECodec ErasureCodec_ = NErasure::ECodec::None;

    struct
    {
        bool Movable : 1;
        bool Overlayed : 1;
    } Flags_ = {};

    TChunkRequisitionIndex AggregatedRequisitionIndex_;
    TChunkRequisitionIndex LocalRequisitionIndex_;

    //! The number of non-empty entries in #ExportDataList_.
    //! If zero, #ExportDataList_ is null.
    ui8 ExportCounter_ = 0;

    //! Per-cell data, indexed by cell index; cf. TMulticellManager::GetRegisteredMasterCellIndex.
    std::unique_ptr<TChunkExportDataList> ExportDataList_;

    struct TReplicasData
    {
        //! This set is usually empty. Keeping a holder is very space efficient.
        std::unique_ptr<TCachedReplicas> CachedReplicas;

        //! Just all the stored replicas.
        TStoredReplicas StoredReplicas;

        //! Null entries are InvalidNodeId.
        TLastSeenReplicas LastSeenReplicas;
        //! Indicates the position in LastSeenReplicas to be written next.
        int CurrentLastSeenReplicaIndex = 0;
    };

    //! This additional indirection helps to save up some space since
    //! no replicas are being maintained for foreign chunks.
    //! It also separates relatively mutable data from static one,
    //! which helps to avoid excessive CoW during snapshot construction.
    std::unique_ptr<TReplicasData> ReplicasData_;

    TChunkRequisition ComputeAggregatedRequisition(const TChunkRequisitionRegistry* registry);
    TChunkDynamicData::TMediumToRepairQueueIterator* SelectRepairQueueIteratorMap(EChunkRepairQueue queue) const;

    const TReplicasData& ReplicasData() const;
    TReplicasData* MutableReplicasData();

    void UpdateAggregatedRequisitionIndex(
        TChunkRequisitionRegistry* registry,
        const NObjectServer::TObjectManagerPtr& objectManager);

    void MaybeResetObsoleteEpochData(NObjectServer::TEpoch epoch);

    static const TCachedReplicas EmptyCachedReplicas;
    static const TReplicasData EmptyReplicasData;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer

Y_DECLARE_PODTYPE(NYT::NChunkServer::TChunkExportDataList);

#define CHUNK_INL_H_
#include "chunk-inl.h"
#undef CHUNK_INL_H_
