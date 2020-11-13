#pragma once

#include "lock_manager.h"
#include "object_detail.h"
#include "partition.h"
#include "public.h"
#include "sorted_dynamic_comparer.h"
#include "cached_row.h"

#include <yt/ytlib/chunk_client/public.h>

#include <yt/client/table_client/schema.h>
#include <yt/client/table_client/unversioned_row.h>

#include <yt/ytlib/table_client/tablet_snapshot.h>
#include <yt/ytlib/table_client/versioned_chunk_reader.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/ytlib/query_client/public.h>

#include <yt/core/actions/cancelable_context.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/ref_tracked.h>
#include <yt/core/misc/atomic_object.h>
#include <yt/core/misc/slab_allocator.h>
#include <yt/core/misc/concurrent_cache.h>

#include <atomic>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

struct TDeleteListFlusher
{
    ~TDeleteListFlusher();
};

struct TRowCache
    : public TRefCounted
    , public TDeleteListFlusher
{
    TSlabAllocator Allocator;
    TConcurrentCache<TCachedRow> Cache;

    TRowCache(size_t elementCount, IMemoryUsageTrackerPtr memoryTracker);
};

DEFINE_REFCOUNTED_TYPE(TRowCache)

////////////////////////////////////////////////////////////////////////////////

//! Cf. TRuntimeTabletData.
struct TRuntimeTableReplicaData
    : public TRefCounted
{
    std::atomic<ETableReplicaMode> Mode = {ETableReplicaMode::Async};
    std::atomic<i64> CurrentReplicationRowIndex = {0};
    std::atomic<TTimestamp> CurrentReplicationTimestamp = {NullTimestamp};
    std::atomic<TTimestamp> LastReplicationTimestamp = {NullTimestamp};
    std::atomic<i64> PreparedReplicationRowIndex = {-1};
    std::atomic<bool> PreserveTimestamps = {true};
    std::atomic<NTransactionClient::EAtomicity> Atomicity = {NTransactionClient::EAtomicity::Full};
    TAtomicObject<TError> Error;

    void Populate(NTabletClient::NProto::TTableReplicaStatistics* statistics) const;
    void MergeFrom(const NTabletClient::NProto::TTableReplicaStatistics& statistics);
};

DEFINE_REFCOUNTED_TYPE(TRuntimeTableReplicaData)

////////////////////////////////////////////////////////////////////////////////

struct TReplicaCounters
{
    TReplicaCounters() = default;
    explicit TReplicaCounters(const NProfiling::TTagIdList& list);

    NProfiling::TShardedAggregateGauge LagRowCount;
    NProfiling::TShardedAggregateGauge LagTime;
    NProfiling::TShardedAggregateGauge ReplicationTransactionStartTime;
    NProfiling::TShardedAggregateGauge ReplicationTransactionCommitTime;
    NProfiling::TShardedAggregateGauge ReplicationRowsReadTime;
    NProfiling::TShardedAggregateGauge ReplicationRowsWriteTime;
    NProfiling::TShardedAggregateGauge ReplicationBatchRowCount;
    NProfiling::TShardedAggregateGauge ReplicationBatchDataWeight;
    NProfiling::TShardedMonotonicCounter ReplicationRowCount;
    NProfiling::TShardedMonotonicCounter ReplicationDataWeight;
    NProfiling::TShardedMonotonicCounter ReplicationErrorCount;

    const NProfiling::TTagIdList Tags;
};

extern TReplicaCounters NullReplicaCounters;

////////////////////////////////////////////////////////////////////////////////

struct TTableReplicaSnapshot
    : public TRefCounted
{
    NTransactionClient::TTimestamp StartReplicationTimestamp;
    TRuntimeTableReplicaDataPtr RuntimeData;
    TReplicaCounters* Counters = &NullReplicaCounters;
};

DEFINE_REFCOUNTED_TYPE(TTableReplicaSnapshot)

////////////////////////////////////////////////////////////////////////////////

//! All fields must be atomic since they're being accessed both
//! from the writer and from readers concurrently.
struct TRuntimeTabletData
    : public TRefCounted
{
    std::atomic<i64> TotalRowCount = {0};
    std::atomic<i64> TrimmedRowCount = {0};
    std::atomic<TTimestamp> LastCommitTimestamp = {NullTimestamp};
    std::atomic<TTimestamp> LastWriteTimestamp = {NullTimestamp};
    std::atomic<TTimestamp> UnflushedTimestamp = {MinTimestamp};
    std::atomic<TInstant> ModificationTime = {NProfiling::GetInstant()};
    std::atomic<TInstant> AccessTime = {TInstant::Zero()};
    TEnumIndexedVector<ETabletDynamicMemoryType, std::atomic<i64>> DynamicMemoryUsagePerType;
    TEnumIndexedVector<NTabletClient::ETabletBackgroundActivity, TAtomicObject<TError>> Errors;
};

DEFINE_REFCOUNTED_TYPE(TRuntimeTabletData)

////////////////////////////////////////////////////////////////////////////////

struct TTabletSnapshot
    : public NTableClient::TTabletSnapshot
{
    NHydra::TCellId CellId;
    NHydra::IHydraManagerPtr HydraManager;
    NTabletClient::TTabletId TabletId;
    TString LoggingId;
    NYPath::TYPath TablePath;
    TTableMountConfigPtr Config;
    TTabletChunkWriterConfigPtr WriterConfig;
    TTabletWriterOptionsPtr WriterOptions;
    TLegacyOwningKey PivotKey;
    TLegacyOwningKey NextPivotKey;
    NTableClient::TTableSchemaPtr PhysicalSchema;
    NTableClient::TTableSchemaPtr QuerySchema;
    NTableClient::TSchemaData PhysicalSchemaData;
    NTableClient::TSchemaData KeysSchemaData;
    NTransactionClient::EAtomicity Atomicity;
    NTabletClient::TTableReplicaId UpstreamReplicaId;
    int HashTableSize = 0;
    int OverlappingStoreCount = 0;
    int EdenOverlappingStoreCount = 0;
    int CriticalPartitionCount = 0;
    NTransactionClient::TTimestamp RetainedTimestamp = NTransactionClient::MinTimestamp;

    TPartitionSnapshotPtr Eden;

    using TPartitionList = std::vector<TPartitionSnapshotPtr>;
    using TPartitionListIterator = TPartitionList::iterator;
    TPartitionList PartitionList;

    std::vector<IOrderedStorePtr> OrderedStores;

    std::vector<TWeakPtr<ISortedStore>> LockedStores;

    std::vector<TDynamicStoreId> PreallocatedDynamicStoreIds;

    int StoreCount = 0;
    int PreloadPendingStoreCount = 0;
    int PreloadCompletedStoreCount = 0;
    int PreloadFailedStoreCount = 0;

    TSortedDynamicRowKeyComparer RowKeyComparer;

    TTabletPerformanceCountersPtr PerformanceCounters;

    NQueryClient::TColumnEvaluatorPtr ColumnEvaluator;

    TRuntimeTabletDataPtr TabletRuntimeData;
    TRuntimeTabletCellDataPtr TabletCellRuntimeData;

    THashMap<TTableReplicaId, TTableReplicaSnapshotPtr> Replicas;

    NProfiling::TTagIdList ProfilerTags;
    NProfiling::TTagIdList DiskProfilerTags;

    NConcurrency::IReconfigurableThroughputThrottlerPtr FlushThrottler;
    NConcurrency::IReconfigurableThroughputThrottlerPtr CompactionThrottler;
    NConcurrency::IReconfigurableThroughputThrottlerPtr PartitioningThrottler;

    TLockManagerPtr LockManager;
    TLockManagerEpoch LockManagerEpoch;
    TRowCachePtr RowCache;

    //! Returns a range of partitions intersecting with the range |[lowerBound, upperBound)|.
    std::pair<TPartitionListIterator, TPartitionListIterator> GetIntersectingPartitions(
        const TLegacyKey& lowerBound,
        const TLegacyKey& upperBound);

    //! Returns a partition possibly containing a given #key or
    //! |nullptr| is there's none.
    TPartitionSnapshotPtr FindContainingPartition(TLegacyKey key);

    //! For sorted tablets only.
    //! This includes both regular and locked Eden stores.
    std::vector<ISortedStorePtr> GetEdenStores();

    //! Returns true if |id| corresponds to a preallocated dynamic store
    //! which has not been created yet.
    bool IsPreallocatedDynamicStoreId(TDynamicStoreId storeId) const;

    //! Returns a dynamic store with given |storeId| or |nullptr| if there is none.
    IDynamicStorePtr FindDynamicStore(TDynamicStoreId storeId) const;

    //! Returns a dynamic store with given |storeId| or throws if there is none.
    IDynamicStorePtr GetDynamicStoreOrThrow(TDynamicStoreId storeId) const;

    TTableReplicaSnapshotPtr FindReplicaSnapshot(TTableReplicaId replicaId);

    void ValidateCellId(NElection::TCellId cellId);
    void ValidateMountRevision(NHydra::TRevision mountRevision);
    bool IsProfilingEnabled() const;
    void WaitOnLocks(TTimestamp timestamp) const;
};

DEFINE_REFCOUNTED_TYPE(TTabletSnapshot)

////////////////////////////////////////////////////////////////////////////////

void ValidateTabletRetainedTimestamp(const TTabletSnapshotPtr& tabletSnapshot, TTimestamp timestamp);

////////////////////////////////////////////////////////////////////////////////

struct TTabletPerformanceCounters
    : public TChunkReaderPerformanceCounters
{
    std::atomic<i64> DynamicRowReadCount = {0};
    std::atomic<i64> DynamicRowReadDataWeightCount = {0};
    std::atomic<i64> DynamicRowLookupCount = {0};
    std::atomic<i64> DynamicRowLookupDataWeightCount = {0};
    std::atomic<i64> DynamicRowWriteCount = {0};
    std::atomic<i64> DynamicRowWriteDataWeightCount = {0};
    std::atomic<i64> DynamicRowDeleteCount = {0};
    std::atomic<i64> UnmergedRowReadCount = {0};
    std::atomic<i64> MergedRowReadCount = {0};
    std::atomic<i64> CompactionDataWeightCount = {0};
    std::atomic<i64> PartitioningDataWeightCount = {0};
    std::atomic<i64> LookupErrorCount = {0};
    std::atomic<i64> WriteErrorCount = {0};
};

DEFINE_REFCOUNTED_TYPE(TTabletPerformanceCounters)

////////////////////////////////////////////////////////////////////////////////

struct TTabletCounters
{
    TTabletCounters(const NProfiling::TTagIdList& list);

    NProfiling::TShardedAggregateGauge OverlappingStoreCount;
    NProfiling::TShardedAggregateGauge EdenStoreCount;
};

////////////////////////////////////////////////////////////////////////////////

struct ITabletContext
{
    virtual ~ITabletContext() = default;

    virtual NObjectClient::TCellId GetCellId() = 0;
    virtual const TString& GetTabletCellBundleName() = 0;
    virtual NHydra::EPeerState GetAutomatonState() = 0;
    virtual NQueryClient::TColumnEvaluatorCachePtr GetColumnEvaluatorCache() = 0;
    virtual NObjectClient::TObjectId GenerateId(NObjectClient::EObjectType type) = 0;
    virtual IStorePtr CreateStore(
        TTablet* tablet,
        EStoreType type,
        TStoreId storeId,
        const NTabletNode::NProto::TAddStoreDescriptor* descriptor) = 0;
    virtual TTransactionManagerPtr GetTransactionManager() = 0;
    virtual NRpc::IServerPtr GetLocalRpcServer() = 0;
    virtual NNodeTrackerClient::TNodeDescriptor GetLocalDescriptor() = 0;

    virtual NNodeTrackerClient::TNodeMemoryTrackerPtr GetMemoryUsageTracker() = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TTableReplicaInfo
{
public:
    DEFINE_BYVAL_RW_PROPERTY(TTablet*, Tablet);
    DEFINE_BYVAL_RO_PROPERTY(TTableReplicaId, Id);
    DEFINE_BYVAL_RW_PROPERTY(TString, ClusterName);
    DEFINE_BYVAL_RW_PROPERTY(NYPath::TYPath, ReplicaPath);
    DEFINE_BYVAL_RW_PROPERTY(TTimestamp, StartReplicationTimestamp, NullTimestamp);
    DEFINE_BYVAL_RW_PROPERTY(TTransactionId, PreparedReplicationTransactionId);

    DEFINE_BYVAL_RW_PROPERTY(ETableReplicaState, State, ETableReplicaState::None);

    DEFINE_BYVAL_RW_PROPERTY(TTableReplicatorPtr, Replicator);
    DEFINE_BYVAL_RW_PROPERTY(TReplicaCounters*, Counters, &NullReplicaCounters);

public:
    TTableReplicaInfo() = default;
    TTableReplicaInfo(
        TTablet* tablet,
        TTableReplicaId id);

    void Save(TSaveContext& context) const;
    void Load(TLoadContext& context);

    ETableReplicaMode GetMode() const;
    void SetMode(ETableReplicaMode value);

    NTransactionClient::EAtomicity GetAtomicity() const;
    void SetAtomicity(NTransactionClient::EAtomicity value);

    bool GetPreserveTimestamps() const;
    void SetPreserveTimestamps(bool value);

    i64 GetCurrentReplicationRowIndex() const;
    void SetCurrentReplicationRowIndex(i64 value);

    TTimestamp GetCurrentReplicationTimestamp() const;
    void SetCurrentReplicationTimestamp(TTimestamp value);

    i64 GetPreparedReplicationRowIndex() const;
    void SetPreparedReplicationRowIndex(i64 value);

    TError GetError() const;
    void SetError(TError error);

    TTableReplicaSnapshotPtr BuildSnapshot() const;

    void PopulateStatistics(NTabletClient::NProto::TTableReplicaStatistics* statistics) const;
    void MergeFromStatistics(const NTabletClient::NProto::TTableReplicaStatistics& statistics);

private:
    const TRuntimeTableReplicaDataPtr RuntimeData_ = New<TRuntimeTableReplicaData>();

};

////////////////////////////////////////////////////////////////////////////////

class TTablet
    : public TObjectBase
    , public TRefTracked<TTablet>
{
public:
    DEFINE_BYVAL_RO_PROPERTY(NHydra::TRevision, MountRevision);
    DEFINE_BYVAL_RO_PROPERTY(NObjectClient::TObjectId, TableId);
    DEFINE_BYVAL_RO_PROPERTY(NYPath::TYPath, TablePath);

    DEFINE_BYVAL_RO_PROPERTY(NTableClient::TTableSchemaPtr, TableSchema);
    DEFINE_BYVAL_RO_PROPERTY(NTableClient::TTableSchemaPtr, PhysicalSchema);

    DEFINE_BYREF_RO_PROPERTY(NTableClient::TSchemaData, PhysicalSchemaData);
    DEFINE_BYREF_RO_PROPERTY(NTableClient::TSchemaData, KeysSchemaData);

    DEFINE_BYREF_RO_PROPERTY(std::vector<int>, ColumnIndexToLockIndex);
    DEFINE_BYREF_RO_PROPERTY(std::vector<TString>, LockIndexToName);

    DEFINE_BYVAL_RO_PROPERTY(TLegacyOwningKey, PivotKey);
    DEFINE_BYVAL_RO_PROPERTY(TLegacyOwningKey, NextPivotKey);

    DEFINE_BYVAL_RW_PROPERTY(ETabletState, State);

    DEFINE_BYVAL_RO_PROPERTY(TCancelableContextPtr, CancelableContext);

    // NB: Avoid keeping IStorePtr to simplify store removal.
    DEFINE_BYREF_RW_PROPERTY(std::deque<TStoreId>, PreloadStoreIds);

    DEFINE_BYVAL_RO_PROPERTY(NTransactionClient::EAtomicity, Atomicity);
    DEFINE_BYVAL_RO_PROPERTY(NTransactionClient::ECommitOrdering, CommitOrdering);
    DEFINE_BYVAL_RO_PROPERTY(NTabletClient::TTableReplicaId, UpstreamReplicaId);

    DEFINE_BYVAL_RO_PROPERTY(int, HashTableSize);
    DEFINE_BYVAL_RO_PROPERTY(int, LookupCacheSize);

    DEFINE_BYVAL_RO_PROPERTY(int, OverlappingStoreCount);
    DEFINE_BYVAL_RO_PROPERTY(int, EdenOverlappingStoreCount);
    DEFINE_BYVAL_RO_PROPERTY(int, CriticalPartitionCount);

    DEFINE_BYVAL_RW_PROPERTY(IDynamicStorePtr, ActiveStore);

    using TReplicaMap = THashMap<TTableReplicaId, TTableReplicaInfo>;
    DEFINE_BYREF_RW_PROPERTY(TReplicaMap, Replicas);

    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::TTimestamp, RetainedTimestamp);

    DEFINE_BYVAL_RO_PROPERTY(NConcurrency::TAsyncSemaphorePtr, StoresUpdateCommitSemaphore);

    DEFINE_BYVAL_RO_PROPERTY(NProfiling::TTagIdList, ProfilerTags);
    DEFINE_BYVAL_RO_PROPERTY(NProfiling::TTagIdList, DiskProfilerTags);

    DEFINE_BYREF_RO_PROPERTY(TTabletPerformanceCountersPtr, PerformanceCounters, New<TTabletPerformanceCounters>());
    DEFINE_BYREF_RO_PROPERTY(TRuntimeTabletDataPtr, RuntimeData, New<TRuntimeTabletData>());

    DEFINE_BYREF_RO_PROPERTY(std::deque<TDynamicStoreId>, DynamicStoreIdPool);
    DEFINE_BYVAL_RW_PROPERTY(bool, DynamicStoreIdRequested);

    DEFINE_BYVAL_RW_PROPERTY(NConcurrency::IThroughputThrottlerPtr, TabletStoresUpdateThrottler);

public:
    TTablet(
        TTabletId tabletId,
        ITabletContext* context);
    TTablet(
        TTableMountConfigPtr config,
        TTabletChunkReaderConfigPtr readerConfig,
        TTabletChunkWriterConfigPtr writerConfig,
        TTabletWriterOptionsPtr writerOptions,
        TTabletId tabletId,
        NHydra::TRevision mountRevision,
        NObjectClient::TObjectId tableId,
        const NYPath::TYPath& path,
        ITabletContext* context,
        NTableClient::TTableSchemaPtr schema,
        TLegacyOwningKey pivotKey,
        TLegacyOwningKey nextPivotKey,
        NTransactionClient::EAtomicity atomicity,
        NTransactionClient::ECommitOrdering commitOrdering,
        NTabletClient::TTableReplicaId upstreamReplicaId,
        TTimestamp retainedTimestamp);

    ETabletState GetPersistentState() const;

    const TTableMountConfigPtr& GetConfig() const;
    void SetConfig(TTableMountConfigPtr config);

    const TTabletChunkReaderConfigPtr& GetReaderConfig() const;
    void SetReaderConfig(TTabletChunkReaderConfigPtr config);

    const TTabletChunkWriterConfigPtr& GetWriterConfig() const;
    void SetWriterConfig(TTabletChunkWriterConfigPtr config);

    const TTabletWriterOptionsPtr& GetWriterOptions() const;
    void SetWriterOptions(TTabletWriterOptionsPtr options);

    const IStoreManagerPtr& GetStoreManager() const;
    void SetStoreManager(IStoreManagerPtr storeManager);

    const TLockManagerPtr& GetLockManager() const;

    using TPartitionList = std::vector<std::unique_ptr<TPartition>>;
    const TPartitionList& PartitionList() const;
    TPartition* GetEden() const;
    void CreateInitialPartition();
    TPartition* FindPartition(TPartitionId partitionId);
    TPartition* GetPartition(TPartitionId partitionId);
    void MergePartitions(int firstIndex, int lastIndex);
    void SplitPartition(int index, const std::vector<TLegacyOwningKey>& pivotKeys);
    //! Finds a partition fully containing the range |[minKey, maxKey]|.
    //! Returns the Eden if no such partition exists.
    TPartition* GetContainingPartition(const TLegacyOwningKey& minKey, const TLegacyOwningKey& maxKey);

    const THashMap<TStoreId, IStorePtr>& StoreIdMap() const;
    const std::map<i64, IOrderedStorePtr>& StoreRowIndexMap() const;
    void AddStore(IStorePtr store);
    void RemoveStore(IStorePtr store);
    IStorePtr FindStore(TStoreId id);
    IStorePtr GetStore(TStoreId id);
    IStorePtr GetStoreOrThrow(TStoreId id);

    TTableReplicaInfo* FindReplicaInfo(TTableReplicaId id);
    TTableReplicaInfo* GetReplicaInfoOrThrow(TTableReplicaId id);

    void Save(TSaveContext& context) const;
    void Load(TLoadContext& context);

    TCallback<void(TSaveContext&)> AsyncSave();
    void AsyncLoad(TLoadContext& context);

    bool IsPhysicallySorted() const;
    bool IsPhysicallyOrdered() const;
    bool IsReplicated() const;

    int GetColumnLockCount() const;

    // Only applicable to ordered tablets.
    i64 GetTotalRowCount() const;
    void UpdateTotalRowCount();

    // Only applicable to ordered tablets.
    i64 GetTrimmedRowCount() const;
    void SetTrimmedRowCount(i64 value);

    TTimestamp GetLastCommitTimestamp() const;
    void UpdateLastCommitTimestamp(TTimestamp value);

    TTimestamp GetLastWriteTimestamp() const;
    void UpdateLastWriteTimestamp(TTimestamp value);

    TTimestamp GetUnflushedTimestamp() const;

    void StartEpoch(TTabletSlotPtr slot);
    void StopEpoch();
    IInvokerPtr GetEpochAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const;

    TTabletSnapshotPtr BuildSnapshot(
        TTabletSlotPtr slot,
        std::optional<TLockManagerEpoch> epoch = std::nullopt) const;

    const TSortedDynamicRowKeyComparer& GetRowKeyComparer() const;

    void ValidateMountRevision(NHydra::TRevision mountRevision);

    void UpdateUnflushedTimestamp() const;

    i64 Lock();
    i64 Unlock();
    i64 GetTabletLockCount() const;

    void FillProfilerTags(TCellId cellId);
    void UpdateReplicaCounters();
    bool IsProfilingEnabled() const;

    void ReconfigureThrottlers();

    const TString& GetLoggingId() const;

    std::optional<TString> GetPoolTagByMemoryCategory(NNodeTrackerClient::EMemoryCategory category) const;

    int GetEdenStoreCount() const;

    void PushDynamicStoreIdToPool(TDynamicStoreId storeId);
    TDynamicStoreId PopDynamicStoreIdFromPool();
    void ClearDynamicStoreIdPool();

    NTabletNode::NProto::TMountHint GetMountHint() const;

    void ThrottleTabletStoresUpdate(
        const TTabletSlotPtr& slot,
        const NLogging::TLogger& Logger) const;

private:
    TTableMountConfigPtr Config_;
    TTabletChunkReaderConfigPtr ReaderConfig_;
    TTabletChunkWriterConfigPtr WriterConfig_;
    TTabletWriterOptionsPtr WriterOptions_;

    TString LoggingId_;

    IStoreManagerPtr StoreManager_;

    TEnumIndexedVector<EAutomatonThreadQueue, IInvokerPtr> EpochAutomatonInvokers_;

    std::unique_ptr<TPartition> Eden_;

    TPartitionList PartitionList_;
    THashMap<TPartitionId, TPartition*> PartitionMap_;

    THashMap<TStoreId, IStorePtr> StoreIdMap_;
    std::map<i64, IOrderedStorePtr> StoreRowIndexMap_;

    TSortedDynamicRowKeyComparer RowKeyComparer_;

    ITabletContext* const Context_;

    NQueryClient::TColumnEvaluatorPtr ColumnEvaluator_;

    TRowCachePtr RowCache_;


    i64 TabletLockCount_ = 0;

    TTabletCounters* ProfilerCounters_ = nullptr;

    TLockManagerPtr LockManager_;

    NLogging::TLogger Logger;

    NConcurrency::IReconfigurableThroughputThrottlerPtr FlushThrottler_;
    NConcurrency::IReconfigurableThroughputThrottlerPtr CompactionThrottler_;
    NConcurrency::IReconfigurableThroughputThrottlerPtr PartitioningThrottler_;

    void Initialize();

    TPartition* GetContainingPartition(const ISortedStorePtr& store);

    void UpdateOverlappingStoreCount();
    int ComputeEdenOverlappingStoreCount() const;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
