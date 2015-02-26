#include "stdafx.h"
#include "blob_chunk.h"
#include "private.h"
#include "location.h"
#include "blob_reader_cache.h"
#include "chunk_cache.h"
#include "block_store.h"

#include <core/profiling/scoped_timer.h>

#include <ytlib/chunk_client/file_reader.h>
#include <ytlib/chunk_client/file_writer.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>

namespace NYT {
namespace NDataNode {

using namespace NCellNode;
using namespace NChunkClient;
using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

static NProfiling::TRateCounter DiskBlobReadThroughputCounter("/disk_blob_read_throughput");

////////////////////////////////////////////////////////////////////////////////

TBlobChunkBase::TBlobChunkBase(
    TBootstrap* bootstrap,
    TLocationPtr location,
    const TChunkDescriptor& descriptor,
    const TChunkMeta* meta)
    : TChunkBase(
        bootstrap,
        location,
        descriptor.Id)
{
    Info_.set_disk_space(descriptor.DiskSpace);
    if (meta) {
        InitializeCachedMeta(*meta);
    }
}

TBlobChunkBase::~TBlobChunkBase()
{
    if (Meta_) {
        auto* tracker = Bootstrap_->GetMemoryUsageTracker();
        tracker->Release(EMemoryConsumer::ChunkMeta, Meta_->SpaceUsed());
    }
}

TChunkInfo TBlobChunkBase::GetInfo() const
{
    return Info_;
}

bool TBlobChunkBase::IsActive() const
{
    return false;
}

TFuture<TRefCountedChunkMetaPtr> TBlobChunkBase::GetMeta(
    i64 priority,
    const TNullable<std::vector<int>>& extensionTags)
{
    {
        TGuard<TSpinLock> guard(SpinLock_);
        if (Meta_) {
            guard.Release();
            LOG_DEBUG("Meta cache hit (ChunkId: %v)", Id_);
            return MakeFuture(FilterCachedMeta(extensionTags));
        }
    }

    LOG_DEBUG("Meta cache miss (ChunkId: %v)", Id_);

    // Make a copy of tags list to pass it into the closure.
    auto this_ = MakeStrong(this);
    auto invoker = Bootstrap_->GetControlInvoker();
    return ReadMeta(priority).Apply(
        BIND([=] () {
            UNUSED(this_);
            return FilterCachedMeta(extensionTags);
        }).AsyncVia(invoker));
}

TFuture<std::vector<TSharedRef>> TBlobChunkBase::ReadBlocks(
    int firstBlockIndex,
    int blockCount,
    i64 priority)
{
    YCHECK(firstBlockIndex >= 0);
    YCHECK(blockCount >= 0);

    i64 pendingSize;
    AdjustReadRange(firstBlockIndex, &blockCount, &pendingSize);

    TPendingReadSizeGuard pendingReadSizeGuard;
    if (pendingSize >= 0) {
        auto blockStore = Bootstrap_->GetBlockStore();
        pendingReadSizeGuard = blockStore->IncreasePendingReadSize(pendingSize);
    }

    auto promise = NewPromise<std::vector<TSharedRef>>();

    auto callback = BIND(
        &TBlobChunkBase::DoReadBlocks,
        MakeStrong(this),
        firstBlockIndex,
        blockCount,
        Passed(std::move(pendingReadSizeGuard)),
        promise);

    Location_
        ->GetDataReadInvoker()
        ->Invoke(callback, priority);

    return promise;
}

void TBlobChunkBase::DoReadBlocks(
    int firstBlockIndex,
    int blockCount,
    TPendingReadSizeGuard pendingReadSizeGuard,
    TPromise<std::vector<TSharedRef>> promise)
{
    auto blockStore = Bootstrap_->GetBlockStore();
    auto readerCache = Bootstrap_->GetBlobReaderCache();

    try {
        auto reader = readerCache->GetReader(this);

        if (!pendingReadSizeGuard) {
            InitializeCachedMeta(reader->GetMeta());
            
            i64 pendingSize;
            AdjustReadRange(firstBlockIndex, &blockCount, &pendingSize);
            YCHECK(pendingSize >= 0);

            pendingReadSizeGuard = blockStore->IncreasePendingReadSize(pendingSize);
        }

        std::vector<TSharedRef> blocks;

        LOG_DEBUG("Started reading blob chunk blocks (BlockIds: %v:%v-%v, LocationId: %v)",
            Id_,
            firstBlockIndex,
            firstBlockIndex + blockCount - 1,
            Location_->GetId());
            
        NProfiling::TScopedTimer timer;

        // NB: The reader is synchronous.
        auto blocksOrError = reader->ReadBlocks(firstBlockIndex, blockCount).Get();

        auto readTime = timer.GetElapsed();

        LOG_DEBUG("Finished reading blob chunk blocks (BlockIds: %v:%v-%v, LocationId: %v)",
            Id_,
            firstBlockIndex,
            firstBlockIndex + blockCount - 1,
            Location_->GetId());

        if (!blocksOrError.IsOK()) {
            auto error = TError(
                NChunkClient::EErrorCode::IOError,
                "Error reading blob chunk %v",
                Id_)
                << TError(blocksOrError);
            Location_->Disable(error);
            THROW_ERROR error;
        }

        auto& locationProfiler = Location_->Profiler();
        i64 pendingSize = pendingReadSizeGuard.GetSize();
        locationProfiler.Enqueue("/blob_block_read_size", pendingSize);
        locationProfiler.Enqueue("/blob_block_read_time", readTime.MicroSeconds());
        locationProfiler.Enqueue("/blob_block_read_throughput", pendingSize * 1000000 / (1 + readTime.MicroSeconds()));
        DataNodeProfiler.Increment(DiskBlobReadThroughputCounter, pendingSize);

        promise.Set(blocksOrError.Value());
    } catch (const std::exception& ex) {
        promise.Set(TError(ex));
    }
}

TFuture<void> TBlobChunkBase::ReadMeta(i64 priority)
{
    auto readGuard = TChunkReadGuard::TryAcquire(this);
    if (!readGuard) {
        return MakeFuture(TError("Cannot read meta of chunk %v: chunk is scheduled for removal",
            Id_));
    }

    auto promise = NewPromise<void>();
    auto callback = BIND(
        &TBlobChunkBase::DoReadMeta,
        MakeStrong(this),
        Passed(std::move(readGuard)),
        promise);
    Location_
        ->GetMetaReadInvoker()
        ->Invoke(callback, priority);
    return promise;
}

void TBlobChunkBase::DoReadMeta(
    TChunkReadGuard /*readGuard*/,
    TPromise<void> promise)
{
    auto& Profiler = Location_->Profiler();
    LOG_DEBUG("Started reading chunk meta (ChunkId: %v, LocationId: %v)",
        Id_,
        Location_->GetId());

    NChunkClient::TFileReaderPtr reader;
    PROFILE_TIMING ("/meta_read_time") {
        auto readerCache = Bootstrap_->GetBlobReaderCache();
        try {
            reader = readerCache->GetReader(this);
        } catch (const std::exception& ex) {
            LOG_WARNING(ex, "Error reading chunk meta (ChunkId: %v)",
                Id_);
            promise.Set(ex);
            return;
        }
    }

    InitializeCachedMeta(reader->GetMeta());

    LOG_DEBUG("Finished reading chunk meta (ChunkId: %v, LocationId: %v)",
        Id_,
        Location_->GetId());

    promise.Set(TError());
}

void TBlobChunkBase::InitializeCachedMeta(const NChunkClient::NProto::TChunkMeta& meta)
{
    TGuard<TSpinLock> guard(SpinLock_);
    // This check is important since this code may get triggered
    // multiple times and readers do not use any locking.
    if (Meta_)
        return;

    BlocksExt_ = GetProtoExtension<TBlocksExt>(meta.extensions());
    Meta_ = New<TRefCountedChunkMeta>(meta);

    auto* tracker = Bootstrap_->GetMemoryUsageTracker();
    tracker->Acquire(EMemoryConsumer::ChunkMeta, Meta_->SpaceUsed());
}

void TBlobChunkBase::AdjustReadRange(
    int firstBlockIndex,
    int* blockCount,
    i64* dataSize)
{
    {
        TGuard<TSpinLock> guard(SpinLock_);
        if (!Meta_) {
            *dataSize = -1;
            return;
        }
    }

    auto config = Bootstrap_->GetConfig()->DataNode;
    *blockCount = std::min(*blockCount, config->MaxBlocksPerRead);

    *dataSize = 0;
    int blockIndex = firstBlockIndex;
    while (
        blockIndex < firstBlockIndex + *blockCount &&
        blockIndex < BlocksExt_.blocks_size() &&
        *dataSize <= config->MaxBytesPerRead)
    {
        const auto& blockInfo = BlocksExt_.blocks(blockIndex);
        *dataSize += blockInfo.size();
        ++blockIndex;
    }

    *blockCount = blockIndex - firstBlockIndex;
}

void TBlobChunkBase::SyncRemove(bool force)
{
    auto readerCache = Bootstrap_->GetBlobReaderCache();
    readerCache->EvictReader(this);

    if (force) {
        Location_->RemoveChunkFiles(Id_);
    } else {
        Location_->MoveChunkFilesToTrash(Id_);
    }
}

TFuture<void> TBlobChunkBase::AsyncRemove()
{
    return BIND(&TBlobChunkBase::SyncRemove, MakeStrong(this), false)
        .AsyncVia(Location_->GetWritePoolInvoker())
        .Run();
}

////////////////////////////////////////////////////////////////////////////////

TStoredBlobChunk::TStoredBlobChunk(
    TBootstrap* bootstrap,
    TLocationPtr location,
    const TChunkDescriptor& descriptor,
    const TChunkMeta* meta)
    : TBlobChunkBase(
        bootstrap,
        location,
        descriptor,
        meta)
{ }

////////////////////////////////////////////////////////////////////////////////

TCachedBlobChunk::TCachedBlobChunk(
    TBootstrap* bootstrap,
    TLocationPtr location,
    const TChunkDescriptor& descriptor,
    const TChunkMeta* meta)
    : TBlobChunkBase(
        bootstrap,
        location,
        descriptor,
        meta)
    , TAsyncCacheValueBase<TChunkId, TCachedBlobChunk>(GetId())
    , ChunkCache_(Bootstrap_->GetChunkCache())
{ }

TCachedBlobChunk::~TCachedBlobChunk()
{
    // This check ensures that we don't remove any chunks from cache upon shutdown.
    if (ChunkCache_.IsExpired())
        return;

    Location_->GetWritePoolInvoker()->Invoke(BIND(
        &TLocation::RemoveChunkFiles,
        Location_,
        Id_));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
