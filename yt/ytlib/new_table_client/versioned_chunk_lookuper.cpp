#include "stdafx.h"
#include "versioned_chunk_lookuper.h"
#include "cached_versioned_chunk_meta.h"
#include "chunk_meta_extensions.h"
#include "versioned_block_reader.h"
#include "versioned_lookuper.h"
#include "unversioned_row.h"

#include <core/compression/codec.h>

#include <ytlib/chunk_client/chunk_reader.h>
#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/dispatcher.h>

#include <ytlib/node_tracker_client/node_directory.h>

namespace NYT {
namespace NVersionedTableClient {

using namespace NChunkClient;
using namespace NCompression;

////////////////////////////////////////////////////////////////////////////////

struct TVersionedChunkLookuperPoolTag { };

template <class TBlockReader>
class TVersionedChunkLookuper
    : public IVersionedLookuper
{
public:
    TVersionedChunkLookuper(
        TChunkReaderConfigPtr config,
        TCachedVersionedChunkMetaPtr chunkMeta,
        IChunkReaderPtr chunkReader,
        IBlockCachePtr uncompressedBlockCache,
        const TColumnFilter& columnFilter,
        TLookuperPerformanceCountersPtr performanceCounters,
        TTimestamp timestamp)
        : Config_(std::move(config))
        , ChunkMeta_(std::move(chunkMeta))
        , ChunkReader_(std::move(chunkReader))
        , UncompressedBlockCache_(std::move(uncompressedBlockCache))
        , PerformanceCounters_(std::move(performanceCounters))
        , Timestamp_(timestamp)
        , MemoryPool_(TVersionedChunkLookuperPoolTag())
    {
        YCHECK(ChunkMeta_->Misc().sorted());
        YCHECK(EChunkType(ChunkMeta_->ChunkMeta().type()) == EChunkType::Table);
        YCHECK(ETableChunkFormat(ChunkMeta_->ChunkMeta().version()) == TBlockReader::FormatVersion);
        YCHECK(Timestamp_ != AsyncAllCommittedTimestamp || columnFilter.All);

        if (columnFilter.All) {
            SchemaIdMapping_ = ChunkMeta_->SchemaIdMapping();
        } else {
            SchemaIdMapping_.reserve(ChunkMeta_->SchemaIdMapping().size());
            int keyColumnCount = static_cast<int>(ChunkMeta_->KeyColumns().size());
            for (auto index : columnFilter.Indexes) {
                if (index >= keyColumnCount) {
                    auto mappingIndex = index - keyColumnCount;
                    SchemaIdMapping_.push_back(ChunkMeta_->SchemaIdMapping()[mappingIndex]);
                }
            }
        }
    }

    virtual TFutureHolder<TVersionedRow> Lookup(TKey key) override
    {
        ++PerformanceCounters_->StaticChunkRowLookupCount;

        MemoryPool_.Clear();
        UncompressedBlock_.Reset();

        if (key < ChunkMeta_->GetMinKey().Get() || key > ChunkMeta_->GetMaxKey().Get()) {
            return NullRow_;
        }

        if (!ChunkMeta_->KeyFilter().Contains(key)) {
            ++PerformanceCounters_->StaticChunkRowLookupTrueNegativeCount;
            return NullRow_;
        }

        int blockIndex = GetBlockIndex(key);
        TBlockId blockId(ChunkReader_->GetChunkId(), blockIndex);

        auto uncompressedBlock = UncompressedBlockCache_->Find(blockId);
        if (uncompressedBlock) {
            return MakeFuture<TVersionedRow>(DoLookup(
                uncompressedBlock,
                key,
                blockId));
        }

        BlockIndexes_.clear();
        BlockIndexes_.push_back(blockIndex);

        auto asyncResult = ChunkReader_->ReadBlocks(BlockIndexes_);
        return asyncResult.Apply(
            BIND(&TVersionedChunkLookuper::OnBlockRead, MakeStrong(this), key, blockId)
                .AsyncVia(TDispatcher::Get()->GetCompressionPoolInvoker()));
    }


private:
    TChunkReaderConfigPtr Config_;
    TCachedVersionedChunkMetaPtr ChunkMeta_;
    IChunkReaderPtr ChunkReader_;
    IBlockCachePtr UncompressedBlockCache_;
    TLookuperPerformanceCountersPtr PerformanceCounters_;
    TTimestamp Timestamp_;

    std::vector<TColumnIdMapping> SchemaIdMapping_;

    std::vector<int> BlockIndexes_;

    //! Holds row values for the returned row.
    TChunkedMemoryPool MemoryPool_;

    //! Holds the block for the returned row (for string references).
    TSharedRef UncompressedBlock_;

    TFuture<TVersionedRow> NullRow_ = MakeFuture(TVersionedRow());


    int GetBlockIndex(TKey key)
    {
        const auto& blockIndexKeys = ChunkMeta_->BlockIndexKeys();

        typedef decltype(blockIndexKeys.end()) TIter;
        auto rbegin = std::reverse_iterator<TIter>(blockIndexKeys.end());
        auto rend = std::reverse_iterator<TIter>(blockIndexKeys.begin());
        auto it = std::upper_bound(
            rbegin,
            rend,
            key,
            [] (TKey pivot, const TOwningKey& indexKey) {
                return pivot > indexKey.Get();
            });

        if (it == rend) {
            return 0;
        } else {
            return std::distance(it, rend);
        }
    }

    TVersionedRow OnBlockRead(
        TKey key,
        const TBlockId& blockId,
        const std::vector<TSharedRef>& compressedBlocks)
    {
        YASSERT(compressedBlocks.size() == 1);

        const auto& compressedBlock = compressedBlocks[0];
        auto codecId = ECodec(ChunkMeta_->Misc().compression_codec());
        auto* codec = GetCodec(codecId);
        auto uncompressedBlock = codec->Decompress(compressedBlock);

        if (codecId != ECodec::None) {
            UncompressedBlockCache_->Put(blockId, uncompressedBlock, Null);
        }

        return DoLookup(uncompressedBlock, key, blockId);
    }

    TVersionedRow DoLookup(
        const TSharedRef& uncompressedBlock,
        TKey key,
        const TBlockId& blockId)
    {
        TBlockReader blockReader(
            uncompressedBlock,
            ChunkMeta_->BlockMeta().blocks(blockId.BlockIndex),
            ChunkMeta_->ChunkSchema(),
            ChunkMeta_->KeyColumns(),
            SchemaIdMapping_,
            Timestamp_);

        if (!blockReader.SkipToKey(key) || blockReader.GetKey() != key) {
            ++PerformanceCounters_->StaticChunkRowLookupFalsePositiveCount;
            return TVersionedRow();
        }

        UncompressedBlock_ = std::move(uncompressedBlock);
        return blockReader.GetRow(&MemoryPool_);
    }

};

IVersionedLookuperPtr CreateVersionedChunkLookuper(
    TChunkReaderConfigPtr config,
    IChunkReaderPtr chunkReader,
    IBlockCachePtr uncompressedBlockCache,
    TCachedVersionedChunkMetaPtr chunkMeta,
    const TColumnFilter& columnFilter,
    TLookuperPerformanceCountersPtr performanceCounters,
    TTimestamp timestamp)
{
    auto formatVersion = ETableChunkFormat(chunkMeta->ChunkMeta().version());
    switch (formatVersion) {
        case ETableChunkFormat::VersionedSimple:
            return New<TVersionedChunkLookuper<TSimpleVersionedBlockReader>>(
                std::move(config),
                std::move(chunkMeta),
                std::move(chunkReader),
                std::move(uncompressedBlockCache),
                columnFilter,
                std::move(performanceCounters),
                timestamp);

        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
