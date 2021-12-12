#include "automaton.h"
#include "bootstrap.h"
#include "hint_manager.h"
#include "in_memory_manager.h"
#include "private.h"
#include "store_detail.h"
#include "store_manager.h"
#include "tablet.h"
#include "tablet_profiling.h"
#include "hunk_chunk.h"
#include "versioned_chunk_meta_manager.h"

#include <yt/yt/server/lib/tablet_node/proto/tablet_manager.pb.h>

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/server/node/data_node/chunk_registry.h>
#include <yt/yt/server/node/data_node/chunk.h>
#include <yt/yt/server/node/data_node/chunk_block_manager.h>
#include <yt/yt/server/node/data_node/chunk_registry.h>
#include <yt/yt/server/node/data_node/local_chunk_reader.h>

#include <yt/yt/server/node/cluster_node/config.h>

#include <yt/yt/client/table_client/row_buffer.h>

#include <yt/yt/client/transaction_client/helpers.h>

#include <yt/yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/yt/ytlib/chunk_client/replication_reader.h>
#include <yt/yt_proto/yt/client/chunk_client/proto/chunk_meta.pb.h>
#include <yt/yt/ytlib/chunk_client/helpers.h>
#include <yt/yt/ytlib/chunk_client/block_cache.h>
#include <yt/yt/ytlib/chunk_client/remote_chunk_reader.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/ytlib/table_client/chunk_state.h>
#include <yt/yt/ytlib/table_client/lookup_reader.h>

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/core/concurrency/delayed_executor.h>

namespace NYT::NTabletNode {

using namespace NApi;
using namespace NChunkClient;
using namespace NConcurrency;
using namespace NClusterNode;
using namespace NDataNode;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTransactionClient;
using namespace NYTree;
using namespace NYson;

using NChunkClient::NProto::TChunkMeta;
using NChunkClient::NProto::TChunkSpec;
using NChunkClient::NProto::TMiscExt;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

static const auto ChunkReaderEvictionTimeout = TDuration::Seconds(15);

////////////////////////////////////////////////////////////////////////////////

TStoreBase::TStoreBase(
    TTabletManagerConfigPtr config,
    TStoreId id,
    TTablet* tablet)
    : Config_(std::move(config))
    , StoreId_(id)
    , Tablet_(tablet)
    , PerformanceCounters_(Tablet_->PerformanceCounters())
    , RuntimeData_(Tablet_->RuntimeData())
    , TabletId_(Tablet_->GetId())
    , TablePath_(Tablet_->GetTablePath())
    , Schema_(Tablet_->GetPhysicalSchema())
    , KeyColumnCount_(Tablet_->GetPhysicalSchema()->GetKeyColumnCount())
    , SchemaColumnCount_(Tablet_->GetPhysicalSchema()->GetColumnCount())
    , ColumnLockCount_(Tablet_->GetColumnLockCount())
    , LockIndexToName_(Tablet_->LockIndexToName())
    , ColumnIndexToLockIndex_(Tablet_->ColumnIndexToLockIndex())
    , Logger(TabletNodeLogger.WithTag("StoreId: %v, TabletId: %v",
        StoreId_,
        TabletId_))
{
    UpdateTabletDynamicMemoryUsage(+1);
}

TStoreBase::~TStoreBase()
{
    YT_LOG_DEBUG("Store destroyed");
    UpdateTabletDynamicMemoryUsage(-1);
}

void TStoreBase::Initialize()
{ }

void TStoreBase::SetMemoryTracker(NClusterNode::TNodeMemoryTrackerPtr memoryTracker)
{
    YT_VERIFY(!MemoryTracker_);
    MemoryTracker_ = std::move(memoryTracker);
    DynamicMemoryTrackerGuard_ = TMemoryUsageTrackerGuard::Acquire(
        MemoryTracker_->WithCategory(
            GetMemoryCategory(),
            Tablet_->GetPoolTagByMemoryCategory(GetMemoryCategory())),
        DynamicMemoryUsage_,
        MemoryUsageGranularity);
}

TStoreId TStoreBase::GetId() const
{
    return StoreId_;
}

TTablet* TStoreBase::GetTablet() const
{
    return Tablet_;
}

bool TStoreBase::IsEmpty() const
{
    return GetRowCount() == 0;
}

EStoreState TStoreBase::GetStoreState() const
{
    return StoreState_;
}

void TStoreBase::SetStoreState(EStoreState state)
{
    UpdateTabletDynamicMemoryUsage(-1);
    StoreState_ = state;
    UpdateTabletDynamicMemoryUsage(+1);
}

i64 TStoreBase::GetDynamicMemoryUsage() const
{
    return DynamicMemoryUsage_;
}

TLegacyOwningKey TStoreBase::RowToKey(TUnversionedRow row) const
{
    return NTableClient::RowToKey(*Schema_, row);
}

TLegacyOwningKey TStoreBase::RowToKey(TSortedDynamicRow row) const
{
    return NTabletNode::RowToKey(*Schema_, row);
}

void TStoreBase::Save(TSaveContext& context) const
{
    using NYT::Save;
    Save(context, StoreState_);
}

void TStoreBase::Load(TLoadContext& context)
{
    using NYT::Load;
    // NB: Beware of overloads!
    TStoreBase::SetStoreState(Load<EStoreState>(context));
}

void TStoreBase::BuildOrchidYson(TFluentMap fluent)
{
    fluent
        .Item("store_state").Value(StoreState_)
        .Item("min_timestamp").Value(GetMinTimestamp())
        .Item("max_timestamp").Value(GetMaxTimestamp());
}

void TStoreBase::SetDynamicMemoryUsage(i64 value)
{
    RuntimeData_->DynamicMemoryUsagePerType[DynamicMemoryTypeFromState(StoreState_)] +=
        (value - DynamicMemoryUsage_);
    DynamicMemoryUsage_ = value;
    if (DynamicMemoryTrackerGuard_) {
        DynamicMemoryTrackerGuard_.SetSize(value);
    }
}

ETabletDynamicMemoryType TStoreBase::DynamicMemoryTypeFromState(EStoreState state)
{
    switch (state) {
        case EStoreState::ActiveDynamic:
            return ETabletDynamicMemoryType::Active;

        case EStoreState::PassiveDynamic:
            return ETabletDynamicMemoryType::Passive;

        case EStoreState::Removed:
            return ETabletDynamicMemoryType::Backing;

        default:
            return ETabletDynamicMemoryType::Other;
    }
}

void TStoreBase::UpdateTabletDynamicMemoryUsage(i64 multiplier)
{
    RuntimeData_->DynamicMemoryUsagePerType[DynamicMemoryTypeFromState(StoreState_)] +=
        DynamicMemoryUsage_ * multiplier;
}

////////////////////////////////////////////////////////////////////////////////

struct TDynamicStoreBufferTag
{ };

TDynamicStoreBase::TDynamicStoreBase(
    TTabletManagerConfigPtr config,
    TStoreId id,
    TTablet* tablet)
    : TStoreBase(std::move(config), id, tablet)
    , Atomicity_(Tablet_->GetAtomicity())
    , RowBuffer_(New<TRowBuffer>(
        TDynamicStoreBufferTag(),
        Config_->PoolChunkSize))
{
    SetStoreState(EStoreState::ActiveDynamic);
}

EMemoryCategory TDynamicStoreBase::GetMemoryCategory() const
{
    return EMemoryCategory::TabletDynamic;
}

i64 TDynamicStoreBase::GetLockCount() const
{
    return StoreLockCount_;
}

i64 TDynamicStoreBase::Lock()
{
    YT_ASSERT(Atomicity_ == EAtomicity::Full);

    auto result = ++StoreLockCount_;
    YT_LOG_TRACE("Store locked (Count: %v)",
        result);
    return result;
}

i64 TDynamicStoreBase::Unlock()
{
    YT_ASSERT(Atomicity_ == EAtomicity::Full);
    YT_ASSERT(StoreLockCount_ > 0);

    auto result = --StoreLockCount_;
    YT_LOG_TRACE("Store unlocked (Count: %v)",
        result);
    return result;
}

TTimestamp TDynamicStoreBase::GetMinTimestamp() const
{
    return MinTimestamp_;
}

TTimestamp TDynamicStoreBase::GetMaxTimestamp() const
{
    return MaxTimestamp_;
}

void TDynamicStoreBase::SetStoreState(EStoreState state)
{
    if (StoreState_ == EStoreState::ActiveDynamic && state == EStoreState::PassiveDynamic) {
        OnSetPassive();
    }
    TStoreBase::SetStoreState(state);
}

i64 TDynamicStoreBase::GetCompressedDataSize() const
{
    return GetPoolCapacity();
}

i64 TDynamicStoreBase::GetUncompressedDataSize() const
{
    return GetPoolCapacity();
}

EStoreFlushState TDynamicStoreBase::GetFlushState() const
{
    return FlushState_;
}

void TDynamicStoreBase::SetFlushState(EStoreFlushState state)
{
    FlushState_ = state;
}

i64 TDynamicStoreBase::GetValueCount() const
{
    return StoreValueCount_;
}

i64 TDynamicStoreBase::GetPoolSize() const
{
    return RowBuffer_->GetSize();
}

i64 TDynamicStoreBase::GetPoolCapacity() const
{
    return RowBuffer_->GetCapacity();
}

void TDynamicStoreBase::BuildOrchidYson(TFluentMap fluent)
{
    TStoreBase::BuildOrchidYson(fluent);

    fluent
        .Item("flush_state").Value(GetFlushState())
        .Item("row_count").Value(GetRowCount())
        .Item("lock_count").Value(GetLockCount())
        .Item("value_count").Value(GetValueCount())
        .Item("pool_size").Value(GetPoolSize())
        .Item("pool_capacity").Value(GetPoolCapacity())
        .Item("last_flush_attempt_time").Value(GetLastFlushAttemptTimestamp());
}

bool TDynamicStoreBase::IsDynamic() const
{
    return true;
}

IDynamicStorePtr TDynamicStoreBase::AsDynamic()
{
    return this;
}

TInstant TDynamicStoreBase::GetLastFlushAttemptTimestamp() const
{
    return LastFlushAttemptTimestamp_;
}

void TDynamicStoreBase::UpdateFlushAttemptTimestamp()
{
    LastFlushAttemptTimestamp_ = Now();
}

void TDynamicStoreBase::UpdateTimestampRange(TTimestamp commitTimestamp)
{
    // NB: Don't update min/max timestamps for passive stores since
    // others are relying on these values to remain constant.
    // See, e.g., TSortedStoreManager::MaxTimestampToStore_.
    if (StoreState_ == EStoreState::ActiveDynamic) {
        MinTimestamp_ = std::min(MinTimestamp_, commitTimestamp);
        MaxTimestamp_ = std::max(MaxTimestamp_, commitTimestamp);
    }
}

////////////////////////////////////////////////////////////////////////////////

class TPreloadedBlockCache
    : public IBlockCache
{
public:
    TPreloadedBlockCache(
        TIntrusivePtr<TChunkStoreBase> owner,
        TInMemoryChunkDataPtr chunkData,
        TChunkId chunkId,
        IBlockCachePtr underlyingCache)
        : Owner_(owner)
        , ChunkData_(chunkData)
        , ChunkId_(chunkId)
        , UnderlyingCache_(std::move(underlyingCache))
    { }

    void PutBlock(
        const TBlockId& id,
        EBlockType type,
        const TBlock& data) override
    {
        UnderlyingCache_->PutBlock(id, type, data);
    }

    TCachedBlock FindBlock(
        const TBlockId& id,
        EBlockType type) override
    {
        if (Any(type & GetSupportedBlockTypes())) {
            YT_ASSERT(id.ChunkId == ChunkId_);
            int blockIndex = id.BlockIndex - ChunkData_->StartBlockIndex;
            YT_ASSERT(blockIndex >= 0 && blockIndex < std::ssize(ChunkData_->Blocks));
            return TCachedBlock(ChunkData_->Blocks[blockIndex]);
        } else {
            return UnderlyingCache_->FindBlock(id, type);
        }
    }

    std::unique_ptr<ICachedBlockCookie> GetBlockCookie(
        const TBlockId& id,
        EBlockType type) override
    {
        if (Any(type & GetSupportedBlockTypes())) {
            YT_ASSERT(id.ChunkId == ChunkId_);
            int blockIndex = id.BlockIndex - ChunkData_->StartBlockIndex;
            YT_ASSERT(blockIndex >= 0 && blockIndex < std::ssize(ChunkData_->Blocks));
            return CreatePresetCachedBlockCookie(TCachedBlock(ChunkData_->Blocks[blockIndex]));
        } else {
            return UnderlyingCache_->GetBlockCookie(id, type);
        }
    }

    EBlockType GetSupportedBlockTypes() const override
    {
        return MapInMemoryModeToBlockType(ChunkData_->InMemoryMode);
    }

private:
    const TWeakPtr<TChunkStoreBase> Owner_;
    const TInMemoryChunkDataPtr ChunkData_;
    const TChunkId ChunkId_;
    const IBlockCachePtr UnderlyingCache_;
};

DEFINE_REFCOUNTED_TYPE(TPreloadedBlockCache)

////////////////////////////////////////////////////////////////////////////////

TChunkStoreBase::TChunkStoreBase(
    IBootstrap* bootstrap,
    TTabletManagerConfigPtr config,
    TStoreId id,
    TChunkId chunkId,
    TTimestamp chunkTimestamp,
    TTablet* tablet,
    const NTabletNode::NProto::TAddStoreDescriptor* addStoreDescriptor,
    IBlockCachePtr blockCache,
    IVersionedChunkMetaManagerPtr chunkMetaManager,
    IChunkRegistryPtr chunkRegistry,
    IChunkBlockManagerPtr chunkBlockManager,
    NNative::IClientPtr client,
    const TNodeDescriptor& localDescriptor)
    : TStoreBase(std::move(config), id, tablet)
    , Bootstrap_(bootstrap)
    , BlockCache_(std::move(blockCache))
    , ChunkMetaManager_(std::move(chunkMetaManager))
    , ChunkRegistry_(std::move(chunkRegistry))
    , ChunkBlockManager_(std::move(chunkBlockManager))
    , Client_(std::move(client))
    , LocalDescriptor_(localDescriptor)
    , ChunkMeta_(New<TRefCountedChunkMeta>())
    , ChunkId_(chunkId)
    , ChunkTimestamp_(chunkTimestamp)
    , ReaderConfig_(Tablet_->GetSettings().StoreReaderConfig)
{
    /* If store is over chunk, chunkId == storeId.
     * If store is over chunk view, chunkId and storeId are different,
     * because storeId represents the id of the chunk view.
     *
     * ChunkId is null during recovery of a sorted chunk store because it is is not known
     * at the moment of store creation and will be loaded separately.
     * Consider TTabletManagerImpl::DoCreateStore, TSortedChunkStore::Load.
     */
    YT_VERIFY(
        !ChunkId_ ||
        TypeFromId(ChunkId_) == EObjectType::Chunk ||
        TypeFromId(ChunkId_) == EObjectType::ErasureChunk);

    YT_VERIFY(TypeFromId(StoreId_) == EObjectType::ChunkView || StoreId_ == ChunkId_ || !ChunkId_);

    YT_VERIFY(ChunkMetaManager_);

    SetStoreState(EStoreState::Persistent);

    if (addStoreDescriptor) {
        ChunkMeta_->CopyFrom(addStoreDescriptor->chunk_meta());
    }
}

void TChunkStoreBase::Initialize()
{
    TStoreBase::Initialize();

    SetInMemoryMode(Tablet_->GetSettings().MountConfig->InMemoryMode);
    MiscExt_ = GetProtoExtension<TMiscExt>(ChunkMeta_->extensions());

    if (auto optionalHunkChunkRefsExt = FindProtoExtension<NTableClient::NProto::THunkChunkRefsExt>(ChunkMeta_->extensions())) {
        HunkChunkRefs_.reserve(optionalHunkChunkRefsExt->refs_size());
        for (const auto& ref : optionalHunkChunkRefsExt->refs()) {
            HunkChunkRefs_.push_back({
                .HunkChunk = Tablet_->GetHunkChunk(FromProto<TChunkId>(ref.chunk_id())),
                .HunkCount = ref.hunk_count(),
                .TotalHunkLength = ref.total_hunk_length()
            });
        }
    }
}

const TChunkMeta& TChunkStoreBase::GetChunkMeta() const
{
    return *ChunkMeta_;
}

i64 TChunkStoreBase::GetCompressedDataSize() const
{
    return MiscExt_.compressed_data_size();
}

i64 TChunkStoreBase::GetUncompressedDataSize() const
{
    return MiscExt_.uncompressed_data_size();
}

i64 TChunkStoreBase::GetRowCount() const
{
    return MiscExt_.row_count();
}

TTimestamp TChunkStoreBase::GetMinTimestamp() const
{
    return ChunkTimestamp_ != NullTimestamp ? ChunkTimestamp_ : MiscExt_.min_timestamp();
}

TTimestamp TChunkStoreBase::GetMaxTimestamp() const
{
    return ChunkTimestamp_ != NullTimestamp ? ChunkTimestamp_ : MiscExt_.max_timestamp();
}

void TChunkStoreBase::Save(TSaveContext& context) const
{
    using NYT::Save;
    Save(context, ChunkTimestamp_);
}

void TChunkStoreBase::Load(TLoadContext& context)
{
    using NYT::Load;

    Load(context, ChunkTimestamp_);
}

TCallback<void(TSaveContext&)> TChunkStoreBase::AsyncSave()
{
    return BIND([chunkMeta = ChunkMeta_] (TSaveContext& context) {
        using NYT::Save;
        Save(context, *chunkMeta);
    });
}

void TChunkStoreBase::AsyncLoad(TLoadContext& context)
{
    using NYT::Load;
    Load(context, *ChunkMeta_);
}

void TChunkStoreBase::BuildOrchidYson(TFluentMap fluent)
{
    TStoreBase::BuildOrchidYson(fluent);

    auto backingStore = GetBackingStore();
    fluent
        .Item("preload_state").Value(GetPreloadState())
        .Item("compaction_state").Value(GetCompactionState())
        .Item("compressed_data_size").Value(GetCompressedDataSize())
        .Item("uncompressed_data_size").Value(GetUncompressedDataSize())
        .Item("row_count").Value(GetRowCount())
        .Item("creation_time").Value(GetCreationTime())
        .Item("last_compaction_timestamp").Value(GetLastCompactionTimestamp())
        .DoIf(backingStore.operator bool(), [&] (auto fluent) {
            fluent
                .Item("backing_store").DoMap([&] (auto fluent) {
                    fluent
                        .Item(ToString(backingStore->GetId()))
                        .DoMap(BIND(&IStore::BuildOrchidYson, backingStore));
                });
        })
        .Item("hunk_chunk_refs").Value(HunkChunkRefs_);
}

IDynamicStorePtr TChunkStoreBase::GetBackingStore()
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto guard = ReaderGuard(ReaderLock_);
    return BackingStore_;
}

void TChunkStoreBase::SetBackingStore(IDynamicStorePtr store)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto guard = WriterGuard(ReaderLock_);
    std::swap(store, BackingStore_);
}

bool TChunkStoreBase::HasBackingStore() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto guard = ReaderGuard(ReaderLock_);
    return BackingStore_.operator bool();
}

EStorePreloadState TChunkStoreBase::GetPreloadState() const
{
    return PreloadState_;
}

void TChunkStoreBase::SetPreloadState(EStorePreloadState state)
{
    YT_LOG_INFO("Set preload state (Current: %v, New: %v)", PreloadState_, state);
    PreloadState_ = state;
}

TFuture<void> TChunkStoreBase::GetPreloadFuture() const
{
    return PreloadFuture_;
}

void TChunkStoreBase::SetPreloadFuture(TFuture<void> future)
{
    PreloadFuture_ = std::move(future);
}

EStoreCompactionState TChunkStoreBase::GetCompactionState() const
{
    return CompactionState_;
}

void TChunkStoreBase::SetCompactionState(EStoreCompactionState state)
{
    CompactionState_ = state;
}

bool TChunkStoreBase::IsChunk() const
{
    return true;
}

IChunkStorePtr TChunkStoreBase::AsChunk()
{
    return this;
}

IChunkStore::TReaders TChunkStoreBase::GetReaders(
    std::optional<EWorkloadCategory> workloadCategory)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto hasValidCachedRemoteReaderAdapter = [&] {
        if (!CachedReaders_) {
            return false;
        }
        if (CachedReadersLocal_) {
            return false;
        }
        if (!CachedRemoteReaderAdapters_.contains(workloadCategory)) {
            return false;
        }
        return true;
    };

    auto hasValidCachedLocalReader = [&] {
        if (!ReaderConfig_->PreferLocalReplicas) {
            return false;
        }
        if (!CachedReaders_) {
            return false;
        }
        if (!CachedReadersLocal_) {
            return false;
        }
        auto chunk = CachedWeakChunk_.Lock();
        if (!IsLocalChunkValid(chunk)) {
            return false;
        }
        return true;
    };

    auto setCachedReaders = [&] (bool local, IChunkReaderPtr chunkReader) {
        CachedReadersLocal_ = local;

        CachedReaders_.ChunkReader = std::move(chunkReader);
        CachedReaders_.LookupReader = dynamic_cast<ILookupReader*>(CachedReaders_.ChunkReader.Get());
    };

    auto createLocalReaders = [&] (const IChunkPtr& chunk) {
        CachedRemoteReaderAdapters_.clear();

        CachedWeakChunk_ = chunk;
        setCachedReaders(
            true,
            CreateLocalChunkReader(
                ReaderConfig_,
                chunk,
                ChunkBlockManager_,
                DoGetBlockCache(),
                /*blockMetaCache*/ nullptr));

        YT_LOG_DEBUG("Local chunk reader created and cached");
    };

    auto createRemoteReaders = [&] {
        TChunkSpec chunkSpec;
        ToProto(chunkSpec.mutable_chunk_id(), ChunkId_);
        chunkSpec.set_erasure_codec(MiscExt_.erasure_codec());
        *chunkSpec.mutable_chunk_meta() = *ChunkMeta_;
        CachedWeakChunk_.Reset();
        auto nodeStatusDirectory = Bootstrap_
            ? Bootstrap_->GetHintManager()
            : nullptr;

        // NB: Bandwidth throttler will be set in createRemoteReaderAdapter.
        setCachedReaders(
            false,
            CreateRemoteReader(
                chunkSpec,
                ReaderConfig_,
                New<TRemoteReaderOptions>(),
                Client_,
                Client_->GetNativeConnection()->GetNodeDirectory(),
                LocalDescriptor_,
                /*localNodeId*/ std::nullopt,
                DoGetBlockCache(),
                /*chunkMetaCache*/ nullptr,
                /*trafficMeter*/ nullptr,
                std::move(nodeStatusDirectory),
                /*bandwidthThrottler*/ GetUnlimitedThrottler(),
                /*rpsThrottler*/ GetUnlimitedThrottler()));

        YT_LOG_DEBUG("Remote chunk reader created and cached");
    };

    auto createRemoteReaderAdapter = [&] {
        auto bandwidthThrottler = workloadCategory
            ? Bootstrap_->GetInThrottler(*workloadCategory)
            : GetUnlimitedThrottler();

        TReaders readers;
        readers.ChunkReader = CreateRemoteReaderThrottlingAdapter(
            ChunkId_,
            CachedReaders_.ChunkReader,
            std::move(bandwidthThrottler),
            /*rpsThrottler*/ GetUnlimitedThrottler());
        readers.LookupReader = dynamic_cast<ILookupReader*>(readers.ChunkReader.Get());

        CachedRemoteReaderAdapters_.emplace(
            workloadCategory,
            std::move(readers));
    };

    auto now = NProfiling::GetCpuInstant();
    auto createCachedReader = [&] {
        ChunkReaderEvictionDeadline_ = now + NProfiling::DurationToCpuDuration(ChunkReaderEvictionTimeout);

        if (hasValidCachedLocalReader()) {
            return;
        }

        if (CachedReadersLocal_) {
            CachedReadersLocal_ = false;
            CachedReaders_.Reset();
            CachedWeakChunk_.Reset();
            YT_LOG_DEBUG("Cached local chunk reader is no longer valid");
        }

        if (ChunkRegistry_ && ReaderConfig_->PreferLocalReplicas) {
            auto chunk = ChunkRegistry_->FindChunk(ChunkId_);
            if (IsLocalChunkValid(chunk)) {
                createLocalReaders(chunk);
                return;
            }
        }

        if (!CachedReaders_) {
            createRemoteReaders();
        }
        if (!CachedRemoteReaderAdapters_.contains(workloadCategory)) {
            createRemoteReaderAdapter();
        }
    };

    auto makeResult = [&] {
        if (CachedReadersLocal_) {
            return CachedReaders_;
        } else {
            return GetOrCrash(CachedRemoteReaderAdapters_, workloadCategory);
        }
    };

    // Fast lane.
    {
        auto guard = ReaderGuard(ReaderLock_);
        if (now < ChunkReaderEvictionDeadline_ && (hasValidCachedLocalReader() || hasValidCachedRemoteReaderAdapter())) {
            return makeResult();
        }
    }

    // Slow lane.
    {
        auto guard = WriterGuard(ReaderLock_);
        createCachedReader();
        return makeResult();
    }
}

TTabletStoreReaderConfigPtr TChunkStoreBase::GetReaderConfig()
{
    auto guard = ReaderGuard(ReaderLock_);

    return ReaderConfig_;
}

void TChunkStoreBase::InvalidateCachedReaders(const TTableSettings& settings)
{
    {
        auto guard = WriterGuard(VersionedChunkMetaLock_);
        auto oldCachedWeakVersionedChunkMeta = std::move(CachedWeakVersionedChunkMeta_);
        // Prevent destroying oldCachedWeakVersionedChunkMeta under spinlock.
        guard.Release();
    }

    auto guard = WriterGuard(ReaderLock_);

    DoInvalidateCachedReaders();

    ReaderConfig_ = settings.StoreReaderConfig;
}

void TChunkStoreBase::DoInvalidateCachedReaders()
{
    CachedReaders_.Reset();
    CachedRemoteReaderAdapters_.clear();
    CachedReadersLocal_ = false;
    CachedWeakChunk_.Reset();
}

TCachedVersionedChunkMetaPtr TChunkStoreBase::GetCachedVersionedChunkMeta(
    const IChunkReaderPtr& chunkReader,
    const TClientChunkReadOptions& chunkReadOptions,
    bool prepareColumnarMeta)
{
    VERIFY_THREAD_AFFINITY_ANY();

    // Fast lane.
    {
        auto guard = ReaderGuard(VersionedChunkMetaLock_);
        if (auto chunkMeta = CachedWeakVersionedChunkMeta_.Lock()) {
            ChunkMetaManager_->Touch(chunkMeta);
            return chunkMeta->Meta();
        }
    }

    // Slow lane.
    NProfiling::TWallTimer metaWaitTimer;

    auto chunkMetaFuture = ChunkMetaManager_->GetMeta(
        chunkReader,
        Schema_,
        chunkReadOptions,
        prepareColumnarMeta);

    auto chunkMeta = WaitFor(chunkMetaFuture)
        .ValueOrThrow();

    chunkReadOptions.ChunkReaderStatistics->MetaWaitTime += metaWaitTimer.GetElapsedValue();

    {
        auto guard = WriterGuard(VersionedChunkMetaLock_);
        auto oldCachedWeakVersionedChunkMeta = std::move(CachedWeakVersionedChunkMeta_);
        CachedWeakVersionedChunkMeta_ = chunkMeta;
        // Prevent destroying oldCachedWeakVersionedChunkMeta under spinlock.
        guard.Release();
    }

    return chunkMeta->Meta();
}

const std::vector<THunkChunkRef>& TChunkStoreBase::HunkChunkRefs() const
{
    return HunkChunkRefs_;
}

EMemoryCategory TChunkStoreBase::GetMemoryCategory() const
{
    return EMemoryCategory::TabletStatic;
}

bool TChunkStoreBase::IsPreloadAllowed() const
{
    return Now() > AllowedPreloadTimestamp_;
}

void TChunkStoreBase::UpdatePreloadAttempt(bool isBackoff)
{
    AllowedPreloadTimestamp_ = Now() + (isBackoff ? Config_->PreloadBackoffTime : TDuration::Zero());
}

void TChunkStoreBase::UpdateCompactionAttempt()
{
    LastCompactionTimestamp_ = Now();
}

TInstant TChunkStoreBase::GetLastCompactionTimestamp() const
{
    return LastCompactionTimestamp_;
}

EInMemoryMode TChunkStoreBase::GetInMemoryMode() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto guard = ReaderGuard(ReaderLock_);
    return InMemoryMode_;
}

void TChunkStoreBase::SetInMemoryMode(EInMemoryMode mode)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto guard = WriterGuard(ReaderLock_);

    if (InMemoryMode_ != mode) {
        YT_LOG_INFO("Changed in-memory mode (CurrentMode: %v, NewMode: %v)",
            InMemoryMode_,
            mode);

        InMemoryMode_ = mode;

        ChunkState_.Reset();
        PreloadedBlockCache_.Reset();
        DoInvalidateCachedReaders();

        if (PreloadFuture_) {
            PreloadFuture_.Cancel(TError("Preload canceled due to in-memory mode change"));
        }
        PreloadFuture_.Reset();

        PreloadState_ = EStorePreloadState::None;
    }

    if (PreloadState_ == EStorePreloadState::None && mode != EInMemoryMode::None) {
        PreloadState_ = EStorePreloadState::Scheduled;
    }

    YT_VERIFY((mode == EInMemoryMode::None) == (PreloadState_ == EStorePreloadState::None));
}

void TChunkStoreBase::Preload(TInMemoryChunkDataPtr chunkData)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto guard = WriterGuard(ReaderLock_);

    // Otherwise action must be cancelled.
    YT_VERIFY(chunkData->InMemoryMode == InMemoryMode_);
    YT_VERIFY(chunkData->ChunkMeta);

    PreloadedBlockCache_ = New<TPreloadedBlockCache>(
        this,
        chunkData,
        ChunkId_,
        BlockCache_);

    ChunkState_ = New<TChunkState>(
        PreloadedBlockCache_,
        TChunkSpec(),
        chunkData->ChunkMeta,
        ChunkTimestamp_,
        chunkData->LookupHashTable,
        PerformanceCounters_,
        GetKeyComparer(),
        /*virtualValueDirectory*/ nullptr,
        Schema_);
}

TChunkId TChunkStoreBase::GetChunkId() const
{
    return ChunkId_;
}

TTimestamp TChunkStoreBase::GetOverrideTimestamp() const
{
    return ChunkTimestamp_;
}

TChunkReplicaList TChunkStoreBase::GetReplicas(NNodeTrackerClient::TNodeId localNodeId) const
{
    auto guard = ReaderGuard(ReaderLock_);

    if (CachedReaders_ && !CachedReadersLocal_) {
        auto* remoteReader = dynamic_cast<IRemoteChunkReader*>(CachedReaders_.ChunkReader.Get());
        if (remoteReader) {
            auto remoteReplicas = remoteReader->GetReplicas();
            if (!remoteReplicas.empty()) {
                return remoteReplicas;
            }
        }
        return {};
    }

    // Erasure chunks do not have local readers.
    if (TypeFromId(ChunkId_) != EObjectType::Chunk) {
        return {};
    }

    auto makeLocalReplicas = [&] {
        TChunkReplicaList replicas;
        replicas.emplace_back(localNodeId, GenericChunkReplicaIndex);
        return replicas;
    };

    if (CachedReadersLocal_) {
        return makeLocalReplicas();
    }

    auto localChunk = CachedWeakChunk_.Lock();
    if (ChunkRegistry_ && !localChunk) {
        localChunk = ChunkRegistry_->FindChunk(ChunkId_);
    }

    if (IsLocalChunkValid(localChunk)) {
        return makeLocalReplicas();
    }

    return {};
}

IBlockCachePtr TChunkStoreBase::GetBlockCache()
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto guard = ReaderGuard(ReaderLock_);
    return DoGetBlockCache();
}

IBlockCachePtr TChunkStoreBase::DoGetBlockCache()
{
    VERIFY_SPINLOCK_AFFINITY(ReaderLock_);

    return PreloadedBlockCache_ ? PreloadedBlockCache_ : BlockCache_;
}

bool TChunkStoreBase::IsLocalChunkValid(const IChunkPtr& chunk) const
{
    if (!chunk) {
        return false;
    }
    if (chunk->IsRemoveScheduled()) {
        return false;
    }
    return true;
}

TChunkStatePtr TChunkStoreBase::FindPreloadedChunkState()
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto guard = ReaderGuard(ReaderLock_);

    if (InMemoryMode_ == EInMemoryMode::None) {
        return nullptr;
    }

    if (!ChunkState_) {
        THROW_ERROR_EXCEPTION("Chunk data is not preloaded yet")
            << TErrorAttribute("tablet_id", TabletId_)
            << TErrorAttribute("table_path", TablePath_)
            << TErrorAttribute("store_id", StoreId_)
            << TErrorAttribute("chunk_id", ChunkId_);
    }

    YT_VERIFY(ChunkState_);
    YT_VERIFY(ChunkState_->ChunkMeta);

    return ChunkState_;
}

TInstant TChunkStoreBase::GetCreationTime() const
{
    return TInstant::MicroSeconds(MiscExt_.creation_time());
}

////////////////////////////////////////////////////////////////////////////////

TPartition* TSortedStoreBase::GetPartition() const
{
    return Partition_;
}

void TSortedStoreBase::SetPartition(TPartition* partition)
{
    Partition_ = partition;
}

bool TSortedStoreBase::IsSorted() const
{
    return true;
}

ISortedStorePtr TSortedStoreBase::AsSorted()
{
    return this;
}

////////////////////////////////////////////////////////////////////////////////

bool TOrderedStoreBase::IsOrdered() const
{
    return true;
}

IOrderedStorePtr TOrderedStoreBase::AsOrdered()
{
    return this;
}

i64 TOrderedStoreBase::GetStartingRowIndex() const
{
    return StartingRowIndex_;
}

void TOrderedStoreBase::SetStartingRowIndex(i64 value)
{
    YT_VERIFY(value >= 0);
    StartingRowIndex_ = value;
}

void TOrderedStoreBase::Save(TSaveContext& context) const
{
    using NYT::Save;
    Save(context, StartingRowIndex_);
}

void TOrderedStoreBase::Load(TLoadContext& context)
{
    using NYT::Load;
    Load(context, StartingRowIndex_);
}

////////////////////////////////////////////////////////////////////////////////

TLegacyOwningKey RowToKey(const TTableSchema& schema, TSortedDynamicRow row)
{
    if (!row) {
        return TLegacyOwningKey();
    }

    TUnversionedOwningRowBuilder builder;
    for (int index = 0; index < schema.GetKeyColumnCount(); ++index) {
        builder.AddValue(GetUnversionedKeyValue(row, index, schema.Columns()[index].GetWireType()));
    }
    return builder.FinishRow();
}

TTimestamp CalculateRetainedTimestamp(TTimestamp currentTimestamp, TDuration minDataTtl)
{
    return std::min(
        InstantToTimestamp(TimestampToInstant(currentTimestamp).second - minDataTtl).second,
        currentTimestamp);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
