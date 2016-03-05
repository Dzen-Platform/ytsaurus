#include "chunk_block_manager.h"
#include "private.h"
#include "blob_reader_cache.h"
#include "chunk.h"
#include "chunk_registry.h"
#include "config.h"
#include "location.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/ytlib/chunk_client/chunk_meta.pb.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/data_node_service_proxy.h>
#include <yt/ytlib/chunk_client/file_reader.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/concurrency/thread_affinity.h>

namespace NYT {
namespace NDataNode {

using namespace NObjectClient;
using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NCellNode;

using NChunkClient::NProto::TChunkMeta;
using NChunkClient::NProto::TBlocksExt;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TCachedBlock::TCachedBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const TNullable<TNodeDescriptor>& source)
    : TAsyncCacheValueBase<TBlockId, TCachedBlock>(blockId)
    , Data_(data)
    , Source_(source)
{ }

////////////////////////////////////////////////////////////////////////////////

class TChunkBlockManager::TImpl
    : public TAsyncSlruCacheBase<TBlockId, TCachedBlock>
{
public:
    TImpl(
        TDataNodeConfigPtr config,
        TBootstrap* bootstrap)
        : TAsyncSlruCacheBase(
            config->BlockCache->CompressedData,
            NProfiling::TProfiler(
                DataNodeProfiler.GetPathPrefix() +
                "/block_cache/" +
                FormatEnum(EBlockType::CompressedData)))
        , Config_(config)
        , Bootstrap_(bootstrap)
    { }

    TCachedBlockPtr FindCachedBlock(const TBlockId& blockId)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto cachedBlock = TAsyncSlruCacheBase::Find(blockId);

        if (cachedBlock) {
            LOG_TRACE("Block cache hit (BlockId: %v)", blockId);
        } else {
            LOG_TRACE("Block cache miss (BlockId: %v)", blockId);
        }

        return cachedBlock;
    }

    void PutCachedBlock(
        const TBlockId& blockId,
        const TSharedRef& data,
        const TNullable<TNodeDescriptor>& source)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto cookie = BeginInsert(blockId);
        if (cookie.IsActive()) {
            auto block = New<TCachedBlock>(blockId, data, source);
            cookie.EndInsert(block);

            LOG_DEBUG("Block is put into cache (BlockId: %v, Size: %v, SourceAddress: %v)",
                blockId,
                data.Size(),
                source);
        } else {
            LOG_DEBUG("Failed to cache block due to concurrent read (BlockId: %v, Size: %v, SourceAddress: %v)",
                blockId,
                data.Size(),
                source);
        }
    }

    TCachedBlockCookie BeginInsertCachedBlock(const TBlockId& blockId)
    {
        return BeginInsert(blockId);
    }

    TFuture<std::vector<TSharedRef>> ReadBlockRange(
        const TChunkId& chunkId,
        int firstBlockIndex,
        int blockCount,
        const TWorkloadDescriptor& workloadDescriptor,
        IBlockCachePtr blockCache,
        bool populateCache)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        try {
            auto chunkRegistry = Bootstrap_->GetChunkRegistry();
            // NB: At the moment, range read requests are only possible for the whole chunks.
            auto chunk = chunkRegistry->GetChunkOrThrow(chunkId);

            // Hold the read guard.
            auto readGuard = TChunkReadGuard::AcquireOrThrow(chunk);
            auto asyncBlocks = chunk->ReadBlockRange(
                firstBlockIndex,
                blockCount,
                workloadDescriptor,
                populateCache,
                blockCache);
            // Release the read guard upon future completion.
            return asyncBlocks.Apply(BIND(&TImpl::OnBlocksRead, Passed(std::move(readGuard))));
        } catch (const std::exception& ex) {
            return MakeFuture<std::vector<TSharedRef>>(TError(ex));
        }
    }

    TFuture<std::vector<TSharedRef>> ReadBlockSet(
        const TChunkId& chunkId,
        const std::vector<int>& blockIndexes,
        const TWorkloadDescriptor& workloadDescriptor,
        IBlockCachePtr blockCache,
        bool populateCache)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        try {
            auto chunkRegistry = Bootstrap_->GetChunkRegistry();
            auto chunk = chunkRegistry->FindChunk(chunkId);
            if (!chunk) {
                std::vector<TSharedRef> blocks;
                // During block peering, data nodes exchange individual blocks.
                // Thus the cache may contain a block not bound to any chunk in the registry.
                // We must look for these blocks.
                auto type = TypeFromId(DecodeChunkId(chunkId).Id);
                if (type == EObjectType::Chunk || type == EObjectType::ErasureChunk) {
                    for (int blockIndex : blockIndexes) {
                        auto blockId = TBlockId(chunkId, blockIndex);
                        auto block = blockCache->Find(blockId, EBlockType::CompressedData);
                        blocks.push_back(block);
                    }
                }
                return MakeFuture(blocks);
            }

            auto readGuard = TChunkReadGuard::AcquireOrThrow(chunk);
            auto asyncBlocks = chunk->ReadBlockSet(
                blockIndexes,
                workloadDescriptor,
                populateCache,
                blockCache);
            // Hold the read guard.
            return asyncBlocks.Apply(BIND(&TImpl::OnBlocksRead, Passed(std::move(readGuard))));
        } catch (const std::exception& ex) {
            return MakeFuture<std::vector<TSharedRef>>(TError(ex));
        }
    }

private:
    const TDataNodeConfigPtr Config_;
    TBootstrap* const Bootstrap_;


    virtual i64 GetWeight(const TCachedBlockPtr& block) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return block->GetData().Size();
    }

    static std::vector<TSharedRef> OnBlocksRead(
        TChunkReadGuard /*guard*/,
        const std::vector<TSharedRef>& blocks)
    {
        return blocks;
    }
};

////////////////////////////////////////////////////////////////////////////////

TChunkBlockManager::TChunkBlockManager(
    TDataNodeConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TChunkBlockManager::~TChunkBlockManager()
{ }

TCachedBlockPtr TChunkBlockManager::FindCachedBlock(const TBlockId& blockId)
{
    return Impl_->FindCachedBlock(blockId);
}

void TChunkBlockManager::PutCachedBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const TNullable<TNodeDescriptor>& source)
{
    Impl_->PutCachedBlock(blockId, data, source);
}

TCachedBlockCookie TChunkBlockManager::BeginInsertCachedBlock(const TBlockId& blockId)
{
    return Impl_->BeginInsertCachedBlock(blockId);
}

TFuture<std::vector<TSharedRef>> TChunkBlockManager::ReadBlockRange(
    const TChunkId& chunkId,
    int firstBlockIndex,
    int blockCount,
    const TWorkloadDescriptor& workloadDescriptor,
    IBlockCachePtr blockCache,
    bool populateCache)
{
    return Impl_->ReadBlockRange(
        chunkId,
        firstBlockIndex,
        blockCount,
        workloadDescriptor,
        std::move(blockCache),
        populateCache);
}

TFuture<std::vector<TSharedRef>> TChunkBlockManager::ReadBlockSet(
    const TChunkId& chunkId,
    const std::vector<int>& blockIndexes,
    const TWorkloadDescriptor& workloadDescriptor,
    IBlockCachePtr blockCache,
    bool populateCache)
{
    return Impl_->ReadBlockSet(
        chunkId,
        blockIndexes,
        workloadDescriptor,
        std::move(blockCache),
        populateCache);
}

std::vector<TCachedBlockPtr> TChunkBlockManager::GetAllBlocks() const
{
    return Impl_->GetAll();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
