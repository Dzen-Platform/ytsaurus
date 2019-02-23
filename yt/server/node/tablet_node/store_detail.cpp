#include "automaton.h"
#include "in_memory_manager.h"
#include "private.h"
#include "store_detail.h"
#include "store_manager.h"
#include "tablet.h"
#include "tablet_profiling.h"

#include <yt/server/lib/tablet_node/proto/tablet_manager.pb.h>

#include <yt/server/lib/tablet_node/config.h>

#include <yt/server/node/data_node/chunk_registry.h>
#include <yt/server/node/data_node/chunk.h>
#include <yt/server/node/data_node/chunk_block_manager.h>
#include <yt/server/node/data_node/chunk_registry.h>
#include <yt/server/node/data_node/local_chunk_reader.h>

#include <yt/server/node/cell_node/bootstrap.h>
#include <yt/server/node/cell_node/config.h>

#include <yt/client/table_client/row_buffer.h>

#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/replication_reader.h>
#include <yt/client/chunk_client/proto/chunk_meta.pb.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/block_cache.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/client/object_client/helpers.h>

#include <yt/ytlib/table_client/chunk_state.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/concurrency/delayed_executor.h>

namespace NYT::NTabletNode {

using namespace NApi;
using namespace NChunkClient;
using namespace NConcurrency;
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

using NTabletNode::NProto::TAddStoreDescriptor;

////////////////////////////////////////////////////////////////////////////////

static const auto LocalChunkRecheckPeriod = TDuration::Seconds(15);

////////////////////////////////////////////////////////////////////////////////

TStoreBase::TStoreBase(
    TTabletManagerConfigPtr config,
    TStoreId id,
    TTablet* tablet)
    : Config_(std::move(config))
    , ReaderConfig_(tablet->GetReaderConfig())
    , StoreId_(id)
    , Tablet_(tablet)
    , PerformanceCounters_(Tablet_->PerformanceCounters())
    , RuntimeData_(Tablet_->RuntimeData())
    , TabletId_(Tablet_->GetId())
    , TablePath_(Tablet_->GetTablePath())
    , Schema_(Tablet_->PhysicalSchema())
    , KeyColumnCount_(Tablet_->PhysicalSchema().GetKeyColumnCount())
    , SchemaColumnCount_(Tablet_->PhysicalSchema().GetColumnCount())
    , ColumnLockCount_(Tablet_->GetColumnLockCount())
    , LockIndexToName_(Tablet_->LockIndexToName())
    , ColumnIndexToLockIndex_(Tablet_->ColumnIndexToLockIndex())
    , Logger(NLogging::TLogger(TabletNodeLogger)
        .AddTag("StoreId: %v, TabletId: %v",
            StoreId_,
            TabletId_))
{ }

TStoreBase::~TStoreBase()
{
    i64 delta = -MemoryUsage_;
    MemoryUsage_ = 0;
    MemoryUsageUpdated_.Fire(delta);
    RuntimeData_->DynamicMemoryPoolSize += delta;
}

TStoreId TStoreBase::GetId() const
{
    return StoreId_;
}

TTablet* TStoreBase::GetTablet() const
{
    return Tablet_;
}

EStoreState TStoreBase::GetStoreState() const
{
    return StoreState_;
}

void TStoreBase::SetStoreState(EStoreState state)
{
    StoreState_ = state;
}

i64 TStoreBase::GetMemoryUsage() const
{
    return MemoryUsage_;
}

void TStoreBase::SubscribeMemoryUsageUpdated(const TCallback<void(i64 delta)>& callback)
{
    MemoryUsageUpdated_.Subscribe(callback);
    callback.Run(+GetMemoryUsage());
}

void TStoreBase::UnsubscribeMemoryUsageUpdated(const TCallback<void(i64 delta)>& callback)
{
    MemoryUsageUpdated_.Unsubscribe(callback);
    callback.Run(-GetMemoryUsage());
}

void TStoreBase::SetMemoryUsage(i64 value)
{
    if (std::abs(value - MemoryUsage_) > MemoryUsageGranularity) {
        i64 delta = value - MemoryUsage_;
        MemoryUsage_ = value;
        MemoryUsageUpdated_.Fire(delta);
        RuntimeData_->DynamicMemoryPoolSize += delta;
    }
}

TOwningKey TStoreBase::RowToKey(TUnversionedRow row) const
{
    return NTableClient::RowToKey(Schema_, row);
}

TOwningKey TStoreBase::RowToKey(TSortedDynamicRow row) const
{
    return NTabletNode::RowToKey(Schema_, row);
}

void TStoreBase::Save(TSaveContext& context) const
{
    using NYT::Save;
    Save(context, StoreState_);
}

void TStoreBase::Load(TLoadContext& context)
{
    using NYT::Load;
    Load(context, StoreState_);
}

void TStoreBase::OnAftereStoreLoaded()
{ }

void TStoreBase::BuildOrchidYson(TFluentMap fluent)
{
    fluent
        .Item("store_state").Value(StoreState_)
        .Item("min_timestamp").Value(GetMaxTimestamp())
        .Item("max_timestamp").Value(GetMaxTimestamp());
}

bool TStoreBase::IsDynamic() const
{
    return false;
}

IDynamicStorePtr TStoreBase::AsDynamic()
{
    Y_UNREACHABLE();
}

bool TStoreBase::IsChunk() const
{
    return false;
}

IChunkStorePtr TStoreBase::AsChunk()
{
    Y_UNREACHABLE();
}

bool TStoreBase::IsSorted() const
{
    return false;
}

ISortedStorePtr TStoreBase::AsSorted()
{
    Y_UNREACHABLE();
}

TSortedDynamicStorePtr TStoreBase::AsSortedDynamic()
{
    Y_UNREACHABLE();
}

TSortedChunkStorePtr TStoreBase::AsSortedChunk()
{
    Y_UNREACHABLE();
}

bool TStoreBase::IsOrdered() const
{
    return false;
}

IOrderedStorePtr TStoreBase::AsOrdered()
{
    Y_UNREACHABLE();
}

TOrderedDynamicStorePtr TStoreBase::AsOrderedDynamic()
{
    Y_UNREACHABLE();
}

TOrderedChunkStorePtr TStoreBase::AsOrderedChunk()
{
    Y_UNREACHABLE();
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
    StoreState_ = EStoreState::ActiveDynamic;
    UpdateMemoryProfilingCallback();
}

TDynamicStoreBase::~TDynamicStoreBase()
{ }

void TDynamicStoreBase::OnAftereStoreLoaded()
{
    UpdateMemoryProfilingCallback();
}

i64 TDynamicStoreBase::GetLockCount() const
{
    return StoreLockCount_;
}

i64 TDynamicStoreBase::Lock()
{
    Y_ASSERT(Atomicity_ == EAtomicity::Full);

    auto result = ++StoreLockCount_;
    YT_LOG_TRACE("Store locked (Count: %v)",
        result);
    return result;
}

i64 TDynamicStoreBase::Unlock()
{
    Y_ASSERT(Atomicity_ == EAtomicity::Full);
    Y_ASSERT(StoreLockCount_ > 0);

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

    UpdateMemoryProfilingCallback();
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
        .Item("flush_state").Value(FlushState_)
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

void TDynamicStoreBase::UpdateMemoryProfilingCallback()
{
    if (MemoryProfilingCallback_) {
        UnsubscribeMemoryUsageUpdated(MemoryProfilingCallback_);
    }

    TStringBuf memoryType;
    switch (StoreState_) {
        case EStoreState::ActiveDynamic:
            memoryType = "active";
            break;

        case EStoreState::PassiveDynamic:
            memoryType = "passive";
            break;

        case EStoreState::Removed:
            memoryType = "backing";
            break;

        default:
            memoryType = "other";
            break;
    }

    auto tags = Tablet_->GetProfilerTags();
    tags.push_back(NProfiling::TProfileManager::Get()->RegisterTag("memory_type", memoryType));

    auto callback = BIND([tags = std::move(tags)] (i64 delta) {
        ProfileDynamicMemoryUsage(tags, delta);
    });

    TDelayedExecutorCookie cookie;
    MemoryProfilingCallback_ = BIND([=] (i64 delta) mutable {
        callback(delta);

        if (cookie) {
            NConcurrency::TDelayedExecutor::CancelAndClear(cookie);
        }

        // Update profiler counter after it's interval expires.
        cookie = NConcurrency::TDelayedExecutor::Submit(
            BIND(callback, 0),
            TDuration::Seconds(2));
    });

    SubscribeMemoryUsageUpdated(MemoryProfilingCallback_);
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

    virtual void Put(
        const TBlockId& id,
        EBlockType type,
        const TBlock& data,
        const std::optional<NNodeTrackerClient::TNodeDescriptor>& source) override
    {
        UnderlyingCache_->Put(id, type, data, source);
    }

    virtual TBlock Find(
        const TBlockId& id,
        EBlockType type) override
    {
        if (type == GetSupportedBlockTypes()) {
            Y_ASSERT(id.ChunkId == ChunkId_);
            Y_ASSERT(id.BlockIndex >= 0 && id.BlockIndex < ChunkData_->Blocks.size());
            return ChunkData_->Blocks[id.BlockIndex];
        } else {
            return UnderlyingCache_->Find(id, type);
        }
    }

    virtual EBlockType GetSupportedBlockTypes() const override
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
    TTabletManagerConfigPtr config,
    TStoreId id,
    TTablet* tablet,
    IBlockCachePtr blockCache,
    TChunkRegistryPtr chunkRegistry,
    TChunkBlockManagerPtr chunkBlockManager,
    TVersionedChunkMetaManagerPtr chunkMetaManager,
    NNative::IClientPtr client,
    const TNodeDescriptor& localDescriptor)
    : TStoreBase(std::move(config), id, tablet)
    , BlockCache_(std::move(blockCache))
    , ChunkRegistry_(std::move(chunkRegistry))
    , ChunkBlockManager_(std::move(chunkBlockManager))
    , ChunkMetaManager_(std::move(chunkMetaManager))
    , Client_(std::move(client))
    , LocalDescriptor_(localDescriptor)
    , ChunkMeta_(New<TRefCountedChunkMeta>())
{
    YCHECK(
        TypeFromId(StoreId_) == EObjectType::Chunk ||
        TypeFromId(StoreId_) == EObjectType::ErasureChunk);

    StoreState_ = EStoreState::Persistent;
}

void TChunkStoreBase::Initialize(const TAddStoreDescriptor* descriptor)
{
    auto inMemoryMode = Tablet_->GetConfig()->InMemoryMode;

    SetInMemoryMode(inMemoryMode);

    if (descriptor) {
        ChunkMeta_->CopyFrom(descriptor->chunk_meta());
        PrecacheProperties();
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
    return MiscExt_.min_timestamp();
}

TTimestamp TChunkStoreBase::GetMaxTimestamp() const
{
    return MiscExt_.max_timestamp();
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

    PrecacheProperties();
}

void TChunkStoreBase::BuildOrchidYson(TFluentMap fluent)
{
    TStoreBase::BuildOrchidYson(fluent);

    auto backingStore = GetBackingStore();
    fluent
        .Item("preload_state").Value(PreloadState_)
        .Item("compaction_state").Value(CompactionState_)
        .Item("compressed_data_size").Value(MiscExt_.compressed_data_size())
        .Item("uncompressed_data_size").Value(MiscExt_.uncompressed_data_size())
        .Item("row_count").Value(MiscExt_.row_count())
        .Item("creation_time").Value(TInstant::MicroSeconds(MiscExt_.creation_time()))
        .DoIf(backingStore.operator bool(), [&] (TFluentMap fluent) {
            fluent
                .Item("backing_store").DoMap([&] (TFluentMap fluent) {
                    fluent
                        .Item(ToString(backingStore->GetId()))
                        .DoMap(BIND(&IStore::BuildOrchidYson, backingStore));
                });
        });
}

IDynamicStorePtr TChunkStoreBase::GetBackingStore()
{
    VERIFY_THREAD_AFFINITY_ANY();

    TReaderGuard guard(SpinLock_);
    return BackingStore_;
}

void TChunkStoreBase::SetBackingStore(IDynamicStorePtr store)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TWriterGuard guard(SpinLock_);
    std::swap(store, BackingStore_);
}

bool TChunkStoreBase::HasBackingStore() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TReaderGuard guard(SpinLock_);
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

IChunkReaderPtr TChunkStoreBase::GetChunkReader(const NConcurrency::IThroughputThrottlerPtr& throttler)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto isLocalChunkValid = [] (const IChunkPtr& chunk) {
        if (!chunk) {
            return false;
        }
        if (chunk->IsRemoveScheduled()) {
            return false;
        }
        return true;
    };

    auto hasValidCachedRemoteReader = [&] {
        if (!CachedChunkReader_) {
            return false;
        }
        if (CachedChunkReaderIsLocal_) {
            return false;
        }
        return true;
    };

    auto hasValidCachedLocalReader = [&] {
        if (!ReaderConfig_->PreferLocalReplicas) {
            return false;
        }
        if (!CachedChunkReader_) {
            return false;
        }
        if (!CachedChunkReaderIsLocal_) {
            return false;
        }
        auto chunk = CachedWeakChunk_.Lock();
        if (!isLocalChunkValid(chunk)) {
            return false;
        }
        return true;
    };

    auto createLocalChunkReader = [&] (const IChunkPtr& chunk) {
        CachedWeakChunk_ = chunk;
        CachedChunkReader_ = CreateLocalChunkReader(
            ReaderConfig_,
            chunk,
            ChunkBlockManager_,
            GetBlockCache(),
            nullptr /* blockMetaCache */);
        CachedChunkReaderIsLocal_ = true;
        YT_LOG_DEBUG("Local chunk reader created and cached");
    };

    auto createRemoveChunkReader = [&] {
        TChunkSpec chunkSpec;
        ToProto(chunkSpec.mutable_chunk_id(), StoreId_);
        chunkSpec.set_erasure_codec(MiscExt_.erasure_codec());
        *chunkSpec.mutable_chunk_meta() = *ChunkMeta_;
        CachedWeakChunk_.Reset();
        CachedChunkReader_ = CreateRemoteReader(
            chunkSpec,
            ReaderConfig_,
            New<TRemoteReaderOptions>(),
            Client_,
            New<TNodeDirectory>(),
            LocalDescriptor_,
            std::nullopt,
            GetBlockCache(),
            /* trafficMeter */ nullptr,
            throttler,
            GetUnlimitedThrottler() /* rpsThrottler */);
        CachedChunkReaderIsLocal_ = false;
        YT_LOG_DEBUG("Remote chunk reader created and cached");
    };

    auto now = NProfiling::GetCpuInstant();
    auto createCachedReader = [&] {
        LocalChunkCheckDeadline_.store(now + NProfiling::DurationToCpuDuration(LocalChunkRecheckPeriod));

        if (hasValidCachedLocalReader()) {
            return;
        }

        if (CachedChunkReaderIsLocal_) {
            CachedChunkReaderIsLocal_ = false;
            CachedChunkReader_.Reset();
            CachedWeakChunk_.Reset();
            YT_LOG_DEBUG("Cached local chunk reader is no longer valid");
        }

        if (ReaderConfig_->PreferLocalReplicas) {
            auto chunk = ChunkRegistry_->FindChunk(StoreId_);
            if (isLocalChunkValid(chunk)) {
                createLocalChunkReader(chunk);
                return;
            }
        }

        if (!CachedChunkReader_) {
            createRemoveChunkReader();
        }
    };

    // Periodic check.
    if (now > LocalChunkCheckDeadline_.load()) {
        TWriterGuard guard(SpinLock_);
        createCachedReader();
        return CachedChunkReader_;
    }

    // Fast lane.
    {
        TReaderGuard guard(SpinLock_);
        if (hasValidCachedLocalReader() || hasValidCachedRemoteReader()) {
            return CachedChunkReader_;
        }
    }

    // Slow lane.
    {
        TWriterGuard guard(SpinLock_);
        createCachedReader();
        return CachedChunkReader_;
    }
}

void TChunkStoreBase::PrecacheProperties()
{
    MiscExt_ = GetProtoExtension<TMiscExt>(ChunkMeta_->extensions());
}

bool TChunkStoreBase::IsPreloadAllowed() const
{
    return Now() > AllowedPreloadTimestamp_;
}

void TChunkStoreBase::UpdatePreloadAttempt(bool isBackoff)
{
    AllowedPreloadTimestamp_ = Now() + (isBackoff ? Config_->PreloadBackoffTime : TDuration::Zero());
}

bool TChunkStoreBase::IsCompactionAllowed() const
{
    return Now() > AllowedCompactionTimestamp_;
}

void TChunkStoreBase::UpdateCompactionAttempt()
{
    AllowedCompactionTimestamp_ = Now() + Config_->CompactionBackoffTime;
}

EInMemoryMode TChunkStoreBase::GetInMemoryMode() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TReaderGuard guard(SpinLock_);
    return InMemoryMode_;
}

void TChunkStoreBase::SetInMemoryMode(EInMemoryMode mode)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TWriterGuard guard(SpinLock_);

    if (InMemoryMode_ != mode) {
        YT_LOG_INFO("Changed in-memory mode (CurrentMode: %v, NewMode: %v)",
            InMemoryMode_,
            mode);

        InMemoryMode_ = mode;

        ChunkState_.Reset();
        PreloadedBlockCache_.Reset();
        CachedChunkReader_.Reset();
        CachedChunkReaderIsLocal_ = false;
        CachedWeakChunk_.Reset();

        if (PreloadFuture_) {
            YT_LOG_INFO("Cancelling current preload");
            PreloadFuture_.Cancel();
            PreloadFuture_.Reset();
        }

        PreloadState_ = EStorePreloadState::None;
    }

    if (PreloadState_ == EStorePreloadState::None && mode != EInMemoryMode::None) {
        PreloadState_ = EStorePreloadState::Scheduled;
    }

    YCHECK((mode == EInMemoryMode::None) == (PreloadState_ == EStorePreloadState::None));
}

void TChunkStoreBase::Preload(TInMemoryChunkDataPtr chunkData)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TWriterGuard guard(SpinLock_);

    // Otherwise action must be cancelled.
    YCHECK(chunkData->InMemoryMode == InMemoryMode_);
    YCHECK(chunkData->Finalized);
    YCHECK(chunkData->ChunkMeta);

    PreloadedBlockCache_ = New<TPreloadedBlockCache>(
            this,
            chunkData,
            StoreId_,
            BlockCache_);

    ChunkState_ = New<TChunkState>(
        PreloadedBlockCache_,
        TChunkSpec(),
        chunkData->ChunkMeta,
        chunkData->LookupHashTable,
        PerformanceCounters_,
        GetKeyComparer());
}

IBlockCachePtr TChunkStoreBase::GetBlockCache()
{
    VERIFY_THREAD_AFFINITY_ANY();

    TReaderGuard guard(SpinLock_);
    return PreloadedBlockCache_ ? PreloadedBlockCache_ : BlockCache_;
}


bool TChunkStoreBase::ValidateBlockCachePreloaded()
{
    // Under spin lock

    if (InMemoryMode_ == EInMemoryMode::None) {
        return false;
    }

    if (!ChunkState_) {
        THROW_ERROR_EXCEPTION("Chunk data is not preloaded yet")
            << TErrorAttribute("tablet_id", TabletId_)
            << TErrorAttribute("table_path", TablePath_)
            << TErrorAttribute("store_id", StoreId_);
    }

    return true;
}

TInstant TChunkStoreBase::GetCreationTime() const
{
    return TInstant::MicroSeconds(MiscExt_.creation_time());
}

////////////////////////////////////////////////////////////////////////////////

TSortedStoreBase::TSortedStoreBase(
    TTabletManagerConfigPtr config,
    TStoreId id,
    TTablet* tablet)
    : TStoreBase(std::move(config), id, tablet)
{ }

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

TOrderedStoreBase::TOrderedStoreBase(
    TTabletManagerConfigPtr config,
    TStoreId id,
    TTablet* tablet)
    : TStoreBase(std::move(config), id, tablet)
{ }

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
    YCHECK(value >= 0);
    StartingRowIndex_ = value;
}

void TOrderedStoreBase::Save(TSaveContext& context) const
{
    TStoreBase::Save(context);

    using NYT::Save;
    Save(context, StartingRowIndex_);
}

void TOrderedStoreBase::Load(TLoadContext& context)
{
    TStoreBase::Load(context);

    using NYT::Load;
    Load(context, StartingRowIndex_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode

