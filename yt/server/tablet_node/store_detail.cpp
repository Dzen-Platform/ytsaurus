#include "store_detail.h"
#include "private.h"
#include "automaton.h"
#include "tablet.h"
#include "config.h"

#include <yt/server/tablet_node/tablet_manager.pb.h>

#include <yt/server/data_node/chunk_registry.h>
#include <yt/server/data_node/chunk.h>
#include <yt/server/data_node/chunk_block_manager.h>
#include <yt/server/data_node/chunk_registry.h>
#include <yt/server/data_node/local_chunk_reader.h>

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/ytlib/table_client/row_buffer.h>

#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/replication_reader.h>
#include <yt/ytlib/chunk_client/chunk_meta.pb.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/concurrency/delayed_executor.h>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTransactionClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NApi;
using namespace NDataNode;

using NTabletNode::NProto::TAddStoreDescriptor;

////////////////////////////////////////////////////////////////////////////////

static const auto ChunkExpirationTimeout = TDuration::Seconds(15);
static const auto ChunkReaderExpirationTimeout = TDuration::Seconds(15);

////////////////////////////////////////////////////////////////////////////////

TStoreBase::TStoreBase(
    TTabletManagerConfigPtr config,
    const TStoreId& id,
    TTablet* tablet)
    : Config_(std::move(config))
    , ReaderConfig_(tablet->GetReaderConfig())
    , StoreId_(id)
    , Tablet_(tablet)
    , PerformanceCounters_(Tablet_->GetPerformanceCounters())
    , TabletId_(Tablet_->GetId())
    , Schema_(Tablet_->PhysicalSchema())
    , KeyColumnCount_(Tablet_->PhysicalSchema().GetKeyColumnCount())
    , SchemaColumnCount_(Tablet_->PhysicalSchema().GetColumnCount())
    , ColumnLockCount_(Tablet_->GetColumnLockCount())
    , LockIndexToName_(Tablet_->LockIndexToName())
    , ColumnIndexToLockIndex_(Tablet_->ColumnIndexToLockIndex())
{
    Logger = TabletNodeLogger;
    Logger.AddTag("StoreId: %v, TabletId: %v",
        StoreId_,
        TabletId_);
}

TStoreBase::~TStoreBase()
{
    i64 delta = -MemoryUsage_;
    MemoryUsage_ = 0;
    MemoryUsageUpdated_.Fire(delta);
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
    }
}

TOwningKey TStoreBase::RowToKey(TUnversionedRow row)
{
    return NTableClient::RowToKey(Schema_, row);
}

TOwningKey TStoreBase::RowToKey(TSortedDynamicRow row)
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

void TStoreBase::BuildOrchidYson(IYsonConsumer* consumer)
{
    BuildYsonMapFluently(consumer)
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
    const TStoreId& id,
    TTablet* tablet)
    : TStoreBase(std::move(config), id, tablet)
    , Atomicity_(Tablet_->GetAtomicity())
    , RowBuffer_(New<TRowBuffer>(
        TDynamicStoreBufferTag(),
        Config_->PoolChunkSize,
        Config_->MaxPoolSmallBlockRatio))
{
    StoreState_ = EStoreState::ActiveDynamic;
}

i64 TDynamicStoreBase::GetLockCount() const
{
    return StoreLockCount_;
}

i64 TDynamicStoreBase::Lock()
{
    Y_ASSERT(Atomicity_ == EAtomicity::Full);

    auto result = ++StoreLockCount_;
    LOG_TRACE("Store locked (Count: %v)",
        result);
    return result;
}

i64 TDynamicStoreBase::Unlock()
{
    Y_ASSERT(Atomicity_ == EAtomicity::Full);
    Y_ASSERT(StoreLockCount_ > 0);

    auto result = --StoreLockCount_;
    LOG_TRACE("Store unlocked (Count: %v)",
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

void TDynamicStoreBase::BuildOrchidYson(IYsonConsumer* consumer)
{
    TStoreBase::BuildOrchidYson(consumer);

    BuildYsonMapFluently(consumer)
        .Item("flush_state").Value(FlushState_)
        .Item("row_count").Value(GetRowCount())
        .Item("lock_count").Value(GetLockCount())
        .Item("value_count").Value(GetValueCount())
        .Item("pool_size").Value(GetPoolSize())
        .Item("pool_capacity").Value(GetPoolCapacity());
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

///////////////////////////////////////////////////////////////////////////////

TChunkStoreBase::TChunkStoreBase(
    TTabletManagerConfigPtr config,
    const TStoreId& id,
    TTablet* tablet,
    IBlockCachePtr blockCache,
    TChunkRegistryPtr chunkRegistry,
    TChunkBlockManagerPtr chunkBlockManager,
    INativeClientPtr client,
    const TNodeDescriptor& localDescriptor)
    : TStoreBase(std::move(config), id, tablet)
    , BlockCache_(std::move(blockCache))
    , ChunkRegistry_(std::move(chunkRegistry))
    , ChunkBlockManager_(std::move(chunkBlockManager))
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
    SetInMemoryMode(Tablet_->GetConfig()->InMemoryMode);

    if (descriptor) {
        ChunkMeta_->CopyFrom(descriptor->chunk_meta());
        PrecacheProperties();
    }
}

const TChunkMeta& TChunkStoreBase::GetChunkMeta() const
{
    return *ChunkMeta_;
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

void TChunkStoreBase::BuildOrchidYson(IYsonConsumer* consumer)
{
    TStoreBase::BuildOrchidYson(consumer);

    auto backingStore = GetBackingStore();
    BuildYsonMapFluently(consumer)
        .Item("preload_state").Value(PreloadState_)
        .Item("compaction_state").Value(CompactionState_)
        .Item("compressed_data_size").Value(MiscExt_.compressed_data_size())
        .Item("uncompressed_data_size").Value(MiscExt_.uncompressed_data_size())
        .Item("row_count").Value(MiscExt_.row_count())
        .Item("creation_time").Value(TInstant(MiscExt_.creation_time()))
        .DoIf(backingStore.operator bool(), [&] (TFluentMap fluent) {
            fluent.Item("backing_store_id").Value(backingStore->GetId());
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
    BackingStore_ = std::move(store);
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

IChunkReaderPtr TChunkStoreBase::GetChunkReader()
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto chunk = PrepareChunk();
    auto chunkReader = PrepareChunkReader(std::move(chunk));

    return chunkReader;
}

IChunkPtr TChunkStoreBase::PrepareChunk()
{
    VERIFY_THREAD_AFFINITY_ANY();

    {
        TReaderGuard guard(SpinLock_);
        if (ChunkInitialized_) {
            return Chunk_;
        }
    }

    auto chunk = ChunkRegistry_->FindChunk(StoreId_);

    {
        TWriterGuard guard(SpinLock_);
        ChunkInitialized_ = true;
        Chunk_ = chunk;
    }

    TDelayedExecutor::Submit(
        BIND(&TChunkStoreBase::OnChunkExpired, MakeWeak(this)),
        ChunkExpirationTimeout);

    return chunk;
}

IChunkReaderPtr TChunkStoreBase::PrepareChunkReader(IChunkPtr chunk)
{
    VERIFY_THREAD_AFFINITY_ANY();

    {
        TReaderGuard guard(SpinLock_);
        if (ChunkReader_) {
            return ChunkReader_;
        }
    }

    IChunkReaderPtr chunkReader;
    if (ReaderConfig_->PreferLocalReplicas && chunk && !chunk->IsRemoveScheduled()) {
        chunkReader = CreateLocalChunkReader(
            ReaderConfig_,
            chunk,
            ChunkBlockManager_,
            GetBlockCache(),
            BIND(&TChunkStoreBase::OnLocalReaderFailed, MakeWeak(this)));
    } else {
        TChunkSpec chunkSpec;
        ToProto(chunkSpec.mutable_chunk_id(), StoreId_);
        chunkSpec.set_erasure_codec(MiscExt_.erasure_codec());
        *chunkSpec.mutable_chunk_meta() = *ChunkMeta_;

        chunkReader = CreateRemoteReader(
            chunkSpec,
            ReaderConfig_,
            New<TRemoteReaderOptions>(),
            Client_,
            New<TNodeDirectory>(),
            LocalDescriptor_,
            GetBlockCache(),
            GetUnlimitedThrottler());
    }

    {
        TWriterGuard guard(SpinLock_);
        ChunkReader_ = chunkReader;
    }

    TDelayedExecutor::Submit(
        BIND(&TChunkStoreBase::OnChunkReaderExpired, MakeWeak(this)),
        ChunkReaderExpirationTimeout);

    return chunkReader;
}

void TChunkStoreBase::OnLocalReaderFailed()
{
    VERIFY_THREAD_AFFINITY_ANY();

    OnChunkExpired();
    OnChunkReaderExpired();
}

void TChunkStoreBase::OnChunkExpired()
{
    VERIFY_THREAD_AFFINITY_ANY();

    TWriterGuard guard(SpinLock_);
    ChunkInitialized_ = false;
    Chunk_.Reset();
}

void TChunkStoreBase::OnChunkReaderExpired()
{
    VERIFY_THREAD_AFFINITY_ANY();

    TWriterGuard guard(SpinLock_);
    ChunkReader_.Reset();
}

void TChunkStoreBase::PrecacheProperties()
{
    MiscExt_ = GetProtoExtension<TMiscExt>(ChunkMeta_->extensions());
}

bool TChunkStoreBase::IsPreloadAllowed() const
{
    return Now() > AllowedPreloadTimestamp_;
}

void TChunkStoreBase::UpdatePreloadAttempt()
{
    AllowedPreloadTimestamp_ = Now() + Config_->ErrorBackoffTime;
}

bool TChunkStoreBase::IsCompactionAllowed() const
{
    return Now() > AllowedCompactionTimestamp_;
}

void TChunkStoreBase::UpdateCompactionAttempt()
{
    AllowedCompactionTimestamp_ = Now() + Config_->ErrorBackoffTime;
}

TInstant TChunkStoreBase::GetCreationTime() const
{
    return TInstant(MiscExt_.creation_time());
}

////////////////////////////////////////////////////////////////////////////////

TSortedStoreBase::TSortedStoreBase(
    TTabletManagerConfigPtr config,
    const TStoreId& id,
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
    const TStoreId& id,
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

} // namespace NTabletNode
} // namespace NYT

