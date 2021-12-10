#pragma once

#include "public.h"
#include "chunk_requisition.h"
#include "chunk_replica.h"
#include "chunk_tree.h"

#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/server/master/security_server/cluster_resources.h>

#include <yt/yt/server/lib/chunk_server/immutable_chunk_meta.h>

#include <yt/yt/ytlib/chunk_client/proto/chunk_info.pb.h>
#include <yt/yt/ytlib/chunk_client/proto/chunk_service.pb.h>

#include <yt/yt_proto/yt/client/chunk_client/proto/chunk_meta.pb.h>

#include <yt/yt/library/erasure/public.h>

#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/format.h>
#include <yt/yt/core/misc/property.h>
#include <yt/yt/core/misc/ref_tracked.h>
#include <yt/yt/core/misc/compact_flat_map.h>
#include <yt/yt/core/misc/compact_vector.h>

#include <library/cpp/yt/containers/intrusive_linked_list.h>

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
    using TMediumToRepairQueueIterator = TCompactFlatMap<int, TChunkRepairQueueIterator, 2>;

    using TJobSet = TCompactVector<TJobPtr, 1>;

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

    //! Set of jobs that are currently scheduled for this chunk.
    TJobSet Jobs;

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
    DEFINE_BYREF_RW_PROPERTY(TImmutableChunkMetaPtr, ChunkMeta);

    // This map is typically small, e.g. has the size of 1.
    using TParents = TCompactFlatMap<TChunkTree*, int, TypicalChunkParentCount>;
    DEFINE_BYREF_RO_PROPERTY(TParents, Parents);

    // Limits the lifetime of staged chunks. Useful for cleaning up abandoned staged chunks.
    DEFINE_BYVAL_RW_PROPERTY(TInstant, ExpirationTime);
    DEFINE_BYVAL_RW_PROPERTY(std::optional<TChunkExpirationMapIterator>, ExpirationIterator);
    DEFINE_BYVAL_RW_PROPERTY(TConsistentReplicaPlacementHash, ConsistentReplicaPlacementHash, NullConsistentReplicaPlacementHash);

    DEFINE_BYVAL_RW_PROPERTY(NNodeTrackerServer::TNode*, NodeWithEndorsement);

    DEFINE_BYVAL_RW_PROPERTY(i64, DiskSpace);

    //! Some TMiscExt fields extracted for effective access.
    DEFINE_BYVAL_RO_PROPERTY(i64, RowCount);
    DEFINE_BYVAL_RO_PROPERTY(i64, CompressedDataSize);
    DEFINE_BYVAL_RO_PROPERTY(i64, UncompressedDataSize);
    DEFINE_BYVAL_RO_PROPERTY(i64, DataWeight);
    DEFINE_BYVAL_RO_PROPERTY(i64, MaxBlockSize);
    DEFINE_BYVAL_RO_PROPERTY(NCompression::ECodec, CompressionCodec);

    DEFINE_BYVAL_RW_PROPERTY(NErasure::ECodec, ErasureCodec);

    //! Indicates that the list of replicas has changed and endorsement
    //! for ally replicas announcement should be registered.
    DEFINE_BYVAL_RW_PROPERTY(bool, EndorsementRequired);

    DEFINE_BYVAL_RW_PROPERTY(i8, ReadQuorum);
    DEFINE_BYVAL_RW_PROPERTY(i8, WriteQuorum);

public:
    explicit TChunk(TChunkId id);

    TChunkDynamicData* GetDynamicData() const;

    TChunkTreeStatistics GetStatistics() const;

    //! Get disk size of a single part of the chunk.
    /*!
     *  For a non-erasure chunk, simply returns its size
     *  (same as GetDiskSpace()).
     *  For an erasure chunk, returns that size divided by the number of parts
     *  used by the codec.
     */
    i64 GetPartDiskSpace() const;

    TString GetLowercaseObjectName() const override;
    TString GetCapitalizedObjectName() const override;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    void AddParent(TChunkTree* parent);
    void RemoveParent(TChunkTree* parent);
    int GetParentCount() const;
    bool HasParents() const;

    using TCachedReplicas = THashSet<TNodePtrWithIndexes>;
    const TCachedReplicas& CachedReplicas() const;

    TRange<TNodePtrWithIndexes> StoredReplicas() const;

    //! For non-erasure chunks, contains a FIFO queue of seen replicas; its tail position is kept in #CurrentLastSeenReplicaIndex_.
    //! For erasure chunks, this array is directly addressed by replica indexes; at most one replica is kept per part.
    TRange<TNodeId> LastSeenReplicas() const;

    void AddReplica(TNodePtrWithIndexes replica, const TMedium* medium, bool approved);
    void RemoveReplica(TNodePtrWithIndexes replica, const TMedium* medium, bool approved);
    TNodePtrWithIndexesList GetReplicas(std::optional<int> maxCachedReplicas = std::nullopt) const;

    void ApproveReplica(TNodePtrWithIndexes replica);
    int GetApprovedReplicaCount() const;

    // COMPAT(ifsmirnov)
    void SetApprovedReplicaCount(int count);

    void Confirm(
        const NChunkClient::NProto::TChunkInfo& chunkInfo,
        const NChunkClient::NProto::TChunkMeta& chunkMeta);

    bool GetMovable() const;
    void SetMovable(bool value);

    bool GetOverlayed() const;
    void SetOverlayed(bool value);

    void SetRowCount(i64 rowCount);

    bool IsConfirmed() const;

    bool GetScanFlag(EChunkScanKind kind) const;
    void SetScanFlag(EChunkScanKind kind);
    void ClearScanFlag(EChunkScanKind kind);
    TChunk* GetNextScannedChunk() const;

    std::optional<NProfiling::TCpuInstant> GetPartLossTime() const;
    void SetPartLossTime(NProfiling::TCpuInstant partLossTime);
    void ResetPartLossTime();

    TChunkRepairQueueIterator GetRepairQueueIterator(int mediumIndex, EChunkRepairQueue queue) const;
    void SetRepairQueueIterator(int mediumIndex, EChunkRepairQueue queue, TChunkRepairQueueIterator value);

    const TChunkDynamicData::TJobSet& GetJobs() const;

    bool HasJobs() const;
    void AddJob(TJobPtr job);
    void RemoveJob(const TJobPtr& job);

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

    //! Returns the number of physical replicas the chunk should be replicated to.
    //! Unlike similar methods, non-committed owners always contribute to this value.
    int GetAggregatedPhysicalReplicationFactor(const TChunkRequisitionRegistry* registry) const;

    //! Returns the number of physical replicas on particular medium. This equals to:
    //!   - RF for regular chunks,
    //!   - total part count for erasure chunks (or data part if dataPartsOnly is set).
    int GetPhysicalReplicationFactor(int mediumIndex, const TChunkRequisitionRegistry* registry) const;

    i64 GetReplicaLagLimit() const;
    void SetReplicaLagLimit(i64 value);

    std::optional<i64> GetFirstOverlayedRowIndex() const;
    void SetFirstOverlayedRowIndex(std::optional<i64> value);

    //! Returns |true| iff this is an erasure chunk.
    bool IsErasure() const;

    //! Returns |true| iff this is a journal chunk.
    bool IsJournal() const;

    //! Returns |true| iff this is a blob chunk.
    bool IsBlob() const;

    //! Returns |true| iff the chunk can be read immediately, i.e. without repair.
    /*!
     *  For regular (non-erasure) chunk this is equivalent to the existence of any replica.
     *  For erasure chunks this is equivalent to the existence of replicas for all data parts.
     */
    bool IsAvailable() const;

    //! Returns |true| iff this is a sealed journal chunk.
    //! For blob chunks always returns |true|.
    bool IsSealed() const;

    void SetSealed(bool value);

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

    //! Extracts chunk type from meta.
    NChunkClient::EChunkType GetChunkType() const;

    //! Extracts chunk format from meta.
    NChunkClient::EChunkFormat GetChunkFormat() const;

    bool HasConsistentReplicaPlacementHash() const;

private:
    //! -1 stands for std::nullopt for non-overlayed chunks.
    i64 FirstOverlayedRowIndex_ = -1;

    //! Per-cell data, indexed by cell index; cf. TMulticellManager::GetRegisteredMasterCellIndex.
    std::unique_ptr<TChunkExportDataList> ExportDataList_;

    TChunkRequisitionIndex AggregatedRequisitionIndex_;
    TChunkRequisitionIndex LocalRequisitionIndex_;

    //! Ceil(log_2 x), where x is an upper bound
    //! for the difference between length of any
    //! two replicas of a journal chunk.
    ui8 LogReplicaLagLimit_ = 0;

    struct
    {
        bool Movable : 1;
        bool Overlayed : 1;
        bool Sealed : 1;
    } Flags_ = {};

    //! The number of non-empty entries in #ExportDataList_.
    //! If zero, #ExportDataList_ is null.
    ui8 ExportCounter_ = 0;

    struct TReplicasDataBase
        : public TPoolAllocator::TObjectBase
    {
        //! This set is usually empty. Keeping a holder is very space efficient.
        std::unique_ptr<TCachedReplicas> CachedReplicas;

        //! Number of approved replicas among stored.
        int ApprovedReplicaCount = 0;

        //! Indicates the position in LastSeenReplicas to be written next.
        int CurrentLastSeenReplicaIndex = 0;

        virtual ~TReplicasDataBase() = default;

        virtual void Initialize() = 0;

        virtual TRange<TNodePtrWithIndexes> GetStoredReplicas() const = 0;
        virtual TMutableRange<TNodePtrWithIndexes> MutableStoredReplicas() = 0;
        virtual void AddStoredReplica(TNodePtrWithIndexes replica) = 0;
        virtual void RemoveStoredReplica(int replicaIndex) = 0;

        //! Null entries are InvalidNodeId.
        virtual TRange<TNodeId> GetLastSeenReplicas() const = 0;
        virtual TMutableRange<TNodeId> MutableLastSeenReplicas() = 0;

        virtual void Load(NCellMaster::TLoadContext& context, bool isErasure) = 0;
        virtual void Save(NCellMaster::TSaveContext& context) const = 0;
    };

    template <size_t TypicalStoredReplicaCount, size_t LastSeenReplicaCount>
    struct TReplicasData
        : public TReplicasDataBase
    {
        TCompactVector<TNodePtrWithIndexes, TypicalStoredReplicaCount> StoredReplicas;

        std::array<TNodeId, LastSeenReplicaCount> LastSeenReplicas;

        void Initialize() override;

        TRange<TNodeId> GetLastSeenReplicas() const override;
        TMutableRange<TNodeId> MutableLastSeenReplicas() override;

        //! Null entries are InvalidNodeId.
        TRange<TNodePtrWithIndexes> GetStoredReplicas() const override;
        TMutableRange<TNodePtrWithIndexes> MutableStoredReplicas() override;
        void AddStoredReplica(TNodePtrWithIndexes replica) override;
        void RemoveStoredReplica(int replicaIndex) override;

        void Load(NCellMaster::TLoadContext& context, bool isErasure) override;
        void Save(NCellMaster::TSaveContext& context) const override;
    };

    constexpr static int RegularChunkTypicalReplicaCount = 5;
    constexpr static int RegularChunkLastSeenReplicaCount = 5;
    using TRegularChunkReplicasData = TReplicasData<RegularChunkTypicalReplicaCount, RegularChunkLastSeenReplicaCount>;

    constexpr static int ErasureChunkTypicalReplicaCount = 24;
    constexpr static int ErasureChunkLastSeenReplicaCount = 16;
    static_assert(ErasureChunkLastSeenReplicaCount >= ::NErasure::MaxTotalPartCount, "ErasureChunkLastSeenReplicaCount < NErasure::MaxTotalPartCount");
    using TErasureChunkReplicasData = TReplicasData<ErasureChunkTypicalReplicaCount, ErasureChunkLastSeenReplicaCount>;

    // COMPAT(gritukan)
    constexpr static int OldLastSeenReplicaCount = 16;

    //! This additional indirection helps to save up some space since
    //! no replicas are being maintained for foreign chunks.
    //! It also separates relatively mutable data from static one,
    //! which helps to avoid excessive CoW during snapshot construction.
    std::unique_ptr<TReplicasDataBase> ReplicasData_;

    TChunkRequisition ComputeAggregatedRequisition(const TChunkRequisitionRegistry* registry);
    TChunkDynamicData::TMediumToRepairQueueIterator* SelectRepairQueueIteratorMap(EChunkRepairQueue queue) const;

    const TReplicasDataBase& ReplicasData() const;
    TReplicasDataBase* MutableReplicasData();

    void UpdateAggregatedRequisitionIndex(
        TChunkRequisitionRegistry* registry,
        const NObjectServer::TObjectManagerPtr& objectManager);

    void MaybeResetObsoleteEpochData();

    void OnMiscExtUpdated(const NChunkClient::NProto::TMiscExt& miscExt);

    static const TCachedReplicas EmptyCachedReplicas;

    using TEmptyChunkReplicasData = TReplicasData<0, 0>;
    static const TEmptyChunkReplicasData EmptyChunkReplicasData;
};

DEFINE_MASTER_OBJECT_TYPE(TChunk)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer

Y_DECLARE_PODTYPE(NYT::NChunkServer::TChunkExportDataList);

#define CHUNK_INL_H_
#include "chunk-inl.h"
#undef CHUNK_INL_H_
