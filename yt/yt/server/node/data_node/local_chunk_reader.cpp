#include "local_chunk_reader.h"
#include "chunk_block_manager.h"
#include "chunk_store.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>

#include <yt/yt/server/node/data_node/chunk.h>

#include <yt/yt/ytlib/chunk_client/block_cache.h>
#include <yt/yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/yt/ytlib/chunk_client/config.h>

#include <yt/yt/ytlib/table_client/chunk_meta_extensions.h>

namespace NYT::NDataNode {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTableClient;
using namespace NDataNode;
using namespace NClusterNode;
using namespace NTableClient;
using namespace NTableClient::NProto;

////////////////////////////////////////////////////////////////////////////////

class TLocalChunkReader
    : public IChunkReader
{
public:
    TLocalChunkReader(
        TReplicationReaderConfigPtr config,
        IChunkPtr chunk,
        IChunkBlockManagerPtr chunkBlockManager,
        IBlockCachePtr blockCache,
        TBlockMetaCachePtr blockMetaCache)
        : Config_(std::move(config))
        , Chunk_(std::move(chunk))
        , ChunkBlockManager_(std::move(chunkBlockManager))
        , BlockCache_(std::move(blockCache))
        , BlockMetaCache_(std::move(blockMetaCache))
    { }

    TFuture<std::vector<TBlock>> ReadBlocks(
        const TClientChunkReadOptions& options,
        const std::vector<int>& blockIndexes,
        std::optional<i64> /* estimatedSize */) override
    {
        auto session = New<TReadBlockSetSession>();
        static_cast<TClientChunkReadOptions&>(session->Options) = options;
        session->Options.BlockCache = BlockCache_;
        session->Options.PopulateCache = Config_->PopulateCache;
        session->BlockIndexes = blockIndexes;
        session->Blocks.resize(blockIndexes.size());
        RequestBlockSet(session);
        return session->Promise.ToFuture();
    }

    TFuture<std::vector<TBlock>> ReadBlocks(
        const TClientChunkReadOptions& clientOptions,
        int firstBlockIndex,
        int blockCount,
        std::optional<i64> /* estimatedSize */) override
    {
        TChunkReadOptions options;
        static_cast<TClientChunkReadOptions&>(options) = clientOptions;
        options.BlockCache = BlockCache_;
        options.PopulateCache = Config_->PopulateCache;

        auto asyncResult = ChunkBlockManager_->ReadBlockRange(
            Chunk_->GetId(),
            firstBlockIndex,
            blockCount,
            options);

        return asyncResult.Apply(BIND([=] (const TErrorOr<std::vector<TBlock>>& blocksOrError) {
            if (!blocksOrError.IsOK()) {
                ThrowError(blocksOrError);
            }
            return blocksOrError.Value();
        }));
    }

    TFuture<TRefCountedChunkMetaPtr> GetMeta(
        const TClientChunkReadOptions& clientOptions,
        std::optional<int> partitionTag,
        const std::optional<std::vector<int>>& extensionTags) override
    {
        TChunkReadOptions options;
        static_cast<TClientChunkReadOptions&>(options) = clientOptions;

        auto asyncResult = Chunk_->ReadMeta(options, extensionTags);
        return asyncResult.Apply(BIND([=] (const TErrorOr<TRefCountedChunkMetaPtr>& metaOrError) {
            if (!metaOrError.IsOK()) {
                ThrowError(metaOrError);
            }
            const auto& meta = metaOrError.Value();

            if (!partitionTag) {
                return meta;
            }

            auto cachedBlockMeta = BlockMetaCache_
                ? BlockMetaCache_->Find(GetChunkId())
                : TCachedBlockMetaPtr();

            if (!cachedBlockMeta) {
                auto blockMetaExt = GetProtoExtension<TBlockMetaExt>(meta->extensions());
                cachedBlockMeta = New<TCachedBlockMeta>(GetChunkId(), std::move(blockMetaExt));
                if (BlockMetaCache_) {
                    BlockMetaCache_->TryInsert(cachedBlockMeta);
                }
            }

            return New<TRefCountedChunkMeta>(FilterChunkMetaByPartitionTag(*meta, cachedBlockMeta, *partitionTag));
        }));
    }

    TChunkId GetChunkId() const override
    {
        return Chunk_->GetId();
    }

    TInstant GetLastFailureTime() const override
    {
        return TInstant();
    }

private:
    const TReplicationReaderConfigPtr Config_;
    const IChunkPtr Chunk_;
    const IChunkBlockManagerPtr ChunkBlockManager_;
    const IBlockCachePtr BlockCache_;
    const TBlockMetaCachePtr BlockMetaCache_;

    struct TReadBlockSetSession
        : public TRefCounted
    {
        TChunkReadOptions Options;
        std::vector<int> BlockIndexes;
        std::vector<TBlock> Blocks;
        TPromise<std::vector<TBlock>> Promise = NewPromise<std::vector<TBlock>>();
    };

    using TReadBlockSetSessionPtr = TIntrusivePtr<TReadBlockSetSession>;

    void RequestBlockSet(TReadBlockSetSessionPtr session)
    {
        try {
            std::vector<int> localIndexes;
            std::vector<int> blockIndexes;
            for (int index = 0; index < std::ssize(session->Blocks); ++index) {
                if (!session->Blocks[index]) {
                    localIndexes.push_back(index);
                    blockIndexes.push_back(session->BlockIndexes[index]);
                }
            }

            if (localIndexes.empty()) {
                session->Promise.Set(std::move(session->Blocks));
                return;
            }

            auto asyncResult = ChunkBlockManager_->ReadBlockSet(
                Chunk_->GetId(),
                blockIndexes,
                session->Options);

            asyncResult.Subscribe(
                BIND(&TLocalChunkReader::OnBlockSetRead, MakeStrong(this), session, localIndexes));
        } catch (const std::exception& ex) {
            session->Promise.Set(TError(ex));
        }
    }

    void OnBlockSetRead(
        TReadBlockSetSessionPtr session,
        const std::vector<int>& localIndexes,
        const TErrorOr<std::vector<TBlock>>& blocksOrError)
    {
        try {
            if (!blocksOrError.IsOK()) {
                ThrowError(blocksOrError);
            }

            const auto& blocks = blocksOrError.Value();
            for (int responseIndex = 0; responseIndex < std::ssize(blocks); ++responseIndex) {
                const auto& block = blocks[responseIndex];
                int localIndex = localIndexes[responseIndex];
                int blockIndex =  session->BlockIndexes[localIndex];
                if (!block) {
                    ThrowError(TError("Block %v cannot be read",
                        TBlockId(Chunk_->GetId(), blockIndex)));
                }
                session->Blocks[localIndex] = block;
            }

            RequestBlockSet(session);
        } catch (const std::exception& ex) {
            session->Promise.Set(TError(ex));
        }
    }

    void ThrowError(const TError& error)
    {
        THROW_ERROR_EXCEPTION(
            NDataNode::EErrorCode::LocalChunkReaderFailed,
            "Error accessing local chunk %v",
            Chunk_->GetId())
            << error;
    }
};

IChunkReaderPtr CreateLocalChunkReader(
    TReplicationReaderConfigPtr config,
    IChunkPtr chunk,
    IChunkBlockManagerPtr chunkBlockManager,
    IBlockCachePtr blockCache,
    TBlockMetaCachePtr blockMetaCache)
{
    return New<TLocalChunkReader>(
        std::move(config),
        std::move(chunk),
        std::move(chunkBlockManager),
        std::move(blockCache),
        std::move(blockMetaCache));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
