#include "stdafx.h"
#include "block_store.h"
#include "private.h"
#include "chunk.h"
#include "config.h"
#include "chunk_registry.h"
#include "blob_reader_cache.h"
#include "location.h"

#include <ytlib/chunk_client/file_reader.h>
#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/data_node_service_proxy.h>
#include <ytlib/chunk_client/chunk_meta.pb.h>

#include <server/cell_node/bootstrap.h>

#include <core/concurrency/parallel_awaiter.h>

namespace NYT {
namespace NDataNode {

using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NCellNode;

using NChunkClient::NProto::TChunkMeta;
using NChunkClient::NProto::TBlocksExt;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;
static auto& Profiler = DataNodeProfiler;

static NProfiling::TRateCounter CacheReadThroughputCounter("/cache_read_throughput");

////////////////////////////////////////////////////////////////////////////////

TCachedBlock::TCachedBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const TNullable<TNodeDescriptor>& source)
    : TAsyncCacheValueBase<TBlockId, TCachedBlock>(blockId)
    , Data_(data)
    , Source_(source)
{ }

TCachedBlock::~TCachedBlock()
{
    LOG_DEBUG("Cached block purged (BlockId: %v)", GetKey());
}

////////////////////////////////////////////////////////////////////////////////

class TBlockStore::TStoreImpl
    : public TAsyncSlruCacheBase<TBlockId, TCachedBlock>
{
public:
    TStoreImpl(
        TDataNodeConfigPtr config,
        TBootstrap* bootstrap)
        : TAsyncSlruCacheBase(config->CompressedBlockCache)
        , Config_(config)
        , Bootstrap_(bootstrap)
        , PendingReadSize_(0)
    { }

    void Initialize()
    {
        auto result = Bootstrap_->GetMemoryUsageTracker()->TryAcquire(
            NCellNode::EMemoryConsumer::BlockCache,
            Config_->CompressedBlockCache->Capacity + Config_->UncompressedBlockCache->Capacity);
        THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error reserving memory for block cache");
    }

    void PutBlock(
        const TBlockId& blockId,
        const TSharedRef& data,
        const TNullable<TNodeDescriptor>& source)
    {
        while (true) {
            TInsertCookie cookie(blockId);
            if (BeginInsert(&cookie)) {
                auto block = New<TCachedBlock>(blockId, data, source);
                cookie.EndInsert(block);

                LOG_DEBUG("Block is put into cache (BlockId: %v, Size: %v, SourceAddress: %v)",
                    blockId,
                    data.Size(),
                    source);
                return;
            }

            auto result = cookie.GetValue().Get();
            if (!result.IsOK()) {
                // Looks like a parallel Get request has completed unsuccessfully.
                continue;
            }

            // This is a cruel reality.
            // Since we never evict blocks of removed chunks from the cache
            // it is possible for a block to be put there more than once.
            // We shall reuse the cached copy but for sanity's sake let's
            // check that the content is the same.
            auto block = result.Value();

            if (!TRef::AreBitwiseEqual(data, block->GetData())) {
                LOG_FATAL("Trying to cache block %v for which a different cached copy already exists",
                    blockId);
            }

            LOG_DEBUG("Block is resurrected in cache (BlockId: %v)",
                blockId);
            return;
        }
    }

    TFuture<TSharedRef> FindBlock(
        const TChunkId& chunkId,
        int blockIndex,
        i64 priority,
        bool enableCaching)
    {
        // During block peering, data nodes exchange individual blocks.
        // Thus the cache may contain a block not bound to any chunk in the registry.
        // Handle these "unbounded" blocks first.
        // If none is found then look for the owning chunk.
        TBlockId blockId(chunkId, blockIndex);
        auto cachedBlock = FindBlock(blockId);
        if (cachedBlock) {
            LogCacheHit(cachedBlock);
            return MakeFuture(cachedBlock->GetData());
        }
        
        TInsertCookie cookie(blockId);
        if (enableCaching) {
            if (!BeginInsert(&cookie)) {
                return cookie
                    .GetValue()
                    .Apply(BIND(&TStoreImpl::OnCachedBlockReady, MakeStrong(this)));
            }
        }

        auto chunkRegistry = Bootstrap_->GetChunkRegistry();
        auto chunk = chunkRegistry->FindChunk(chunkId);
        if (!chunk) {
            return MakeFuture(TSharedRef());
        }

        auto readGuard = TChunkReadGuard::TryAcquire(chunk);
        if (!readGuard) {
            return MakeFuture(TSharedRef());
        }

        return chunk
            ->ReadBlocks(blockIndex, 1, priority)
            .Apply(BIND(
                &TStoreImpl::OnBlockRead,
                chunk,
                blockIndex,
                Passed(std::move(cookie)),
                Passed(std::move(readGuard))));
    }

    TFuture<std::vector<TSharedRef>> FindBlocks(
        const TChunkId& chunkId,
        int firstBlockIndex,
        int blockCount,
        i64 priority)
    {
        // NB: Range requests bypass block cache.

        auto chunkRegistry = Bootstrap_->GetChunkRegistry();
        auto chunk = chunkRegistry->FindChunk(chunkId);
        if (!chunk) {
            return MakeFuture(std::vector<TSharedRef>());
        }

        auto readGuard = TChunkReadGuard::TryAcquire(chunk);
        if (!readGuard) {
            return MakeFuture<std::vector<TSharedRef>>(TError(
                NChunkClient::EErrorCode::NoSuchChunk,
                "Cannot read chunk %v since it is scheduled for removal",
                chunkId));
        }

        return chunk
            ->ReadBlocks(firstBlockIndex, blockCount, priority)
             .Apply(BIND(
                &TStoreImpl::OnBlocksRead,
                Passed(std::move(readGuard))));
    }

    TCachedBlockPtr FindBlock(const TBlockId& id)
    {
        auto block = TAsyncSlruCacheBase::Find(id);
        if (block) {
            LogCacheHit(block);
        }
        return block;
    }

    i64 GetPendingReadSize() const
    {
        return PendingReadSize_.load();
    }

    TPendingReadSizeGuard IncreasePendingReadSize(i64 delta)
    {
        YASSERT(delta >= 0);
        UpdatePendingReadSize(delta);
        return TPendingReadSizeGuard(delta, Bootstrap_->GetBlockStore());
    }

    void DecreasePendingReadSize(i64 delta)
    {
        UpdatePendingReadSize(-delta);
    }

private:
    TDataNodeConfigPtr Config_;
    TBootstrap* Bootstrap_;

    std::atomic<i64> PendingReadSize_;


    virtual i64 GetWeight(TCachedBlock* block) const override
    {
        return block->GetData().Size();
    }

    void LogCacheHit(TCachedBlockPtr block)
    {
        Profiler.Increment(CacheReadThroughputCounter, block->GetData().Size());
        LOG_DEBUG("Block cache hit (BlockId: %v)",
            block->GetKey());
    }

    void UpdatePendingReadSize(i64 delta)
    {
        i64 result = (PendingReadSize_ += delta);
        LOG_DEBUG("Pending read size updated (PendingReadSize: %v, Delta: %v)",
            result,
            delta);
    }

    TSharedRef OnCachedBlockReady(TCachedBlockPtr cachedBlock)
    {
        LogCacheHit(cachedBlock);
        return cachedBlock->GetData();
    }

    static TSharedRef OnBlockRead(
        IChunkPtr chunk,
        int blockIndex,
        TInsertCookie cookie,
        TChunkReadGuard /*readGuard*/,
        const std::vector<TSharedRef>& blocks)
    {
        YASSERT(blocks.size() <= 1);

        TBlockId blockId(chunk->GetId(), blockIndex);
        if (blocks.empty()) {
            THROW_ERROR_EXCEPTION(
                NChunkClient::EErrorCode::NoSuchBlock,
                "No such block %v",
                blockId);
        }

        const auto& block = blocks[0];
        auto cachedBlock = New<TCachedBlock>(blockId, block, Null);
        cookie.EndInsert(cachedBlock);

        return block;
    }

    static std::vector<TSharedRef> OnBlocksRead(
        TChunkReadGuard /*readGuard*/,
        const std::vector<TSharedRef>& blocks)
    {
        return blocks;
    }

};

////////////////////////////////////////////////////////////////////////////////

class TBlockStore::TCacheImpl
    : public IBlockCache
{
public:
    explicit TCacheImpl(TIntrusivePtr<TStoreImpl> storeImpl)
        : StoreImpl_(storeImpl)
    { }

    virtual void Put(
        const TBlockId& id,
        const TSharedRef& data,
        const TNullable<TNodeDescriptor>& source) override
    {
        StoreImpl_->PutBlock(id, data, source);
    }

    virtual TSharedRef Find(const TBlockId& id) override
    {
        auto block = StoreImpl_->Find(id);
        return block ? block->GetData() : TSharedRef();
    }

private:
    TIntrusivePtr<TStoreImpl> StoreImpl_;

};

////////////////////////////////////////////////////////////////////////////////

TBlockStore::TBlockStore(
    TDataNodeConfigPtr config,
    TBootstrap* bootstrap)
    : StoreImpl_(New<TStoreImpl>(config, bootstrap))
    , CacheImpl_(New<TCacheImpl>(StoreImpl_))
{ }

void TBlockStore::Initialize()
{
    StoreImpl_->Initialize();
}

TBlockStore::~TBlockStore()
{ }

TFuture<TSharedRef> TBlockStore::FindBlock(
    const TChunkId& chunkId,
    int blockIndex,
    i64 priority,
    bool enableCaching)
{
    return StoreImpl_->FindBlock(
        chunkId,
        blockIndex,
        priority,
        enableCaching);
}

TFuture<std::vector<TSharedRef>> TBlockStore::FindBlocks(
    const TChunkId& chunkId,
    int firstBlockIndex,
    int blockCount,
    i64 priority)
{
    return StoreImpl_->FindBlocks(
        chunkId,
        firstBlockIndex,
        blockCount,
        priority);
}

void TBlockStore::PutBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const TNullable<TNodeDescriptor>& source)
{
    StoreImpl_->PutBlock(blockId, data, source);
}

i64 TBlockStore::GetPendingReadSize() const
{
    return StoreImpl_->GetPendingReadSize();
}

TPendingReadSizeGuard TBlockStore::IncreasePendingReadSize(i64 delta)
{
    return StoreImpl_->IncreasePendingReadSize(delta);
}

IBlockCachePtr TBlockStore::GetCompressedBlockCache()
{
    return CacheImpl_;
}

std::vector<TCachedBlockPtr> TBlockStore::GetAllBlocks() const
{
    return StoreImpl_->GetAll();
}

////////////////////////////////////////////////////////////////////////////////

TPendingReadSizeGuard::TPendingReadSizeGuard(
    i64 size,
    TBlockStorePtr owner)
    : Size_(size)
    , Owner_(owner)
{ }

TPendingReadSizeGuard& TPendingReadSizeGuard::operator=(TPendingReadSizeGuard&& other)
{
    swap(*this, other);
    return *this;
}

TPendingReadSizeGuard::~TPendingReadSizeGuard()
{
    if (Owner_) {
        Owner_->StoreImpl_->DecreasePendingReadSize(Size_);
    }
}

TPendingReadSizeGuard::operator bool() const
{
    return Owner_ != nullptr;
}

i64 TPendingReadSizeGuard::GetSize() const
{
    return Size_;
}

void swap(TPendingReadSizeGuard& lhs, TPendingReadSizeGuard& rhs)
{
    using std::swap;
    swap(lhs.Size_, rhs.Size_);
    swap(lhs.Owner_, rhs.Owner_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
