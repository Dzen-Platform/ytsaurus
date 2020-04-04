#include "chunk_reader_base.h"
#include "private.h"
#include "config.h"

#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/chunk_reader_memory_manager.h>

#include <yt/client/chunk_client/proto/data_statistics.pb.h>

#include <algorithm>

namespace NYT::NTableClient {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NCompression;
using namespace NTableClient::NProto;
using NConcurrency::TAsyncSemaphore;

using NYT::FromProto;
using NChunkClient::TReadLimit;

////////////////////////////////////////////////////////////////////////////////

TChunkReaderBase::TChunkReaderBase(
    TBlockFetcherConfigPtr config,
    IChunkReaderPtr underlyingReader,
    IBlockCachePtr blockCache,
    const TClientBlockReadOptions& blockReadOptions,
    const TChunkReaderMemoryManagerPtr& memoryManager)
    : Config_(std::move(config))
    , BlockCache_(std::move(blockCache))
    , UnderlyingReader_(std::move(underlyingReader))
    , BlockReadOptions_(blockReadOptions)
    , Logger(NLogging::TLogger(TableClientLogger)
        .AddTag("ChunkId: %v", UnderlyingReader_->GetChunkId()))
{
    if (memoryManager) {
        MemoryManager_ = memoryManager;
    } else {
        MemoryManager_ = New<TChunkReaderMemoryManager>(TChunkReaderMemoryManagerOptions(Config_->WindowSize));
    }

    MemoryManager_->AddReadSessionInfo(BlockReadOptions_.ReadSessionId);

    if (BlockReadOptions_.ReadSessionId) {
        Logger.AddTag("ReadSessionId: %v", BlockReadOptions_.ReadSessionId);
    }
}

TFuture<void> TChunkReaderBase::DoOpen(
    std::vector<TBlockFetcher::TBlockInfo> blockSequence,
    const TMiscExt& miscExt)
{
    if (blockSequence.empty()) {
        return VoidFuture;
    }

    SequentialBlockFetcher_ = New<TSequentialBlockFetcher>(
        Config_,
        std::move(blockSequence),
        MemoryManager_,
        UnderlyingReader_,
        BlockCache_,
        CheckedEnumCast<ECodec>(miscExt.compression_codec()),
        static_cast<double>(miscExt.compressed_data_size()) / miscExt.uncompressed_data_size(),
        BlockReadOptions_);

    InitFirstBlockNeeded_ = true;
    YT_VERIFY(SequentialBlockFetcher_->HasMoreBlocks());
    MemoryManager_->SetRequiredMemorySize(SequentialBlockFetcher_->GetNextBlockSize());
    CurrentBlock_ = SequentialBlockFetcher_->FetchNextBlock();
    return CurrentBlock_.As<void>();
}

TFuture<void> TChunkReaderBase::GetReadyEvent()
{
    return ReadyEvent_;
}

bool TChunkReaderBase::BeginRead()
{
    if (!ReadyEvent_.IsSet()) {
        return false;
    }

    if (!ReadyEvent_.Get().IsOK()) {
        return false;
    }

    if (InitFirstBlockNeeded_) {
        InitFirstBlock();
        InitFirstBlockNeeded_ = false;
    }

    if (InitNextBlockNeeded_) {
        InitNextBlock();
        InitNextBlockNeeded_ = false;
    }

    return true;
}

bool TChunkReaderBase::OnBlockEnded()
{
    BlockEnded_ = false;

    if (!SequentialBlockFetcher_->HasMoreBlocks()) {
        return false;
    }

    MemoryManager_->SetRequiredMemorySize(SequentialBlockFetcher_->GetNextBlockSize());
    CurrentBlock_ = SequentialBlockFetcher_->FetchNextBlock();
    ReadyEvent_ = CurrentBlock_.As<void>();
    InitNextBlockNeeded_ = true;
    return true;
}

int TChunkReaderBase::GetBlockIndexByKey(
    TKey pivotKey,
    const TSharedRange<TKey>& blockIndexKeys,
    std::optional<int> keyColumnCount) const
{
    YT_VERIFY(!blockIndexKeys.Empty());
    TChunkedMemoryPool pool;
    auto maxKey = blockIndexKeys.Back();
    auto wideMaxKey = WidenKey(maxKey, keyColumnCount, &pool);
    bool overstep = CompareRows(pivotKey, wideMaxKey) > 0;
    if (overstep) {
        return blockIndexKeys.Size();
    }

    typedef decltype(blockIndexKeys.end()) TIter;
    auto rbegin = std::reverse_iterator<TIter>(blockIndexKeys.end() - 1);
    auto rend = std::reverse_iterator<TIter>(blockIndexKeys.begin());
    auto it = std::upper_bound(
        rbegin,
        rend,
        pivotKey,
        [=, &pool] (TKey pivot, TKey key) {
            auto wideKey = WidenKey(key, keyColumnCount, &pool);
            return CompareRows(pivot, wideKey) > 0;
        });

    return (it != rend) ? std::distance(it, rend) : 0;
}

void TChunkReaderBase::CheckBlockUpperKeyLimit(
    TKey blockLastKey,
    TKey upperLimit,
    std::optional<int> keyColumnCount)
{
    TChunkedMemoryPool pool;
    auto wideKey = WidenKey(blockLastKey, keyColumnCount, &pool);

    auto upperKey = upperLimit;
    CheckKeyLimit_ = CompareRows(upperKey, wideKey) <= 0;
}

void TChunkReaderBase::CheckBlockUpperLimits(
    i64 blockChunkRowCount,
    TKey blockLastKey,
    const TReadLimit& upperLimit,
    std::optional<int> keyColumnCount)
{
    if (upperLimit.HasRowIndex()) {
        CheckRowLimit_ = upperLimit.GetRowIndex() < blockChunkRowCount;
    }

    if (upperLimit.HasKey()) {
        CheckBlockUpperKeyLimit(blockLastKey, upperLimit.GetKey(), keyColumnCount);
    }
}

int TChunkReaderBase::ApplyLowerRowLimit(const TBlockMetaExt& blockMeta, const TReadLimit& lowerLimit) const
{
    if (!lowerLimit.HasRowIndex()) {
        return 0;
    }

    const auto& blockMetaEntries = blockMeta.blocks();
    const auto& lastBlock = *(--blockMetaEntries.end());

    if (lowerLimit.GetRowIndex() >= lastBlock.chunk_row_count()) {
        YT_LOG_DEBUG("Lower limit oversteps chunk boundaries (LowerLimit: %v, RowCount: %v)",
            lowerLimit,
            lastBlock.chunk_row_count());

        return blockMeta.blocks_size();
    }

    typedef decltype(blockMetaEntries.end()) TIter;
    auto rbegin = std::reverse_iterator<TIter>(blockMetaEntries.end() - 1);
    auto rend = std::reverse_iterator<TIter>(blockMetaEntries.begin());

    auto it = std::upper_bound(
        rbegin,
        rend,
        lowerLimit.GetRowIndex(),
        [] (i64 index, const TBlockMeta& blockMeta) {
            // Global (chunk-wide) index of last row in block.
            auto maxRowIndex = blockMeta.chunk_row_count() - 1;
            return index > maxRowIndex;
        });

    return (it != rend) ? std::distance(it, rend) : 0;
}

int TChunkReaderBase::ApplyLowerKeyLimit(const TSharedRange<TKey>& blockIndexKeys, const TReadLimit& lowerLimit, std::optional<int> keyColumnCount) const
{
    if (!lowerLimit.HasKey()) {
        return 0;
    }

    int blockIndex = GetBlockIndexByKey(lowerLimit.GetKey(), blockIndexKeys, keyColumnCount);
    if (blockIndex == blockIndexKeys.Size()) {
        YT_LOG_DEBUG("Lower limit oversteps chunk boundaries (LowerLimit: %v, MaxKey: %v)",
            lowerLimit,
            blockIndexKeys.Back());
    }
    return blockIndex;
}

int TChunkReaderBase::ApplyUpperRowLimit(const TBlockMetaExt& blockMeta, const TReadLimit& upperLimit) const
{
    if (!upperLimit.HasRowIndex()) {
        return blockMeta.blocks_size();
    }

    auto begin = blockMeta.blocks().begin();
    auto end = blockMeta.blocks().end() - 1;
    auto it = std::lower_bound(
        begin,
        end,
        upperLimit.GetRowIndex(),
        [] (const TBlockMeta& blockMeta, i64 index) {
            auto maxRowIndex = blockMeta.chunk_row_count() - 1;
            return maxRowIndex < index;
        });

    return  (it != end) ? std::distance(begin, it) + 1 : blockMeta.blocks_size();
}

int TChunkReaderBase::ApplyUpperKeyLimit(const TSharedRange<TKey>& blockIndexKeys, const TReadLimit& upperLimit, std::optional<int> keyColumnCount) const
{
    YT_VERIFY(!blockIndexKeys.Empty());
    if (!upperLimit.HasKey()) {
        return blockIndexKeys.Size();
    }

    TChunkedMemoryPool pool;
    auto begin = blockIndexKeys.begin();
    auto end = blockIndexKeys.end() - 1;
    auto it = std::lower_bound(
        begin,
        end,
        upperLimit.GetKey(),
        [=, &pool] (TKey key, TKey pivot) {
            auto wideKey = WidenKey(key, keyColumnCount, &pool);
            return CompareRows(pivot, wideKey) > 0;
        });

    return  (it != end) ? std::distance(begin, it) + 1 : blockIndexKeys.Size();
}

TDataStatistics TChunkReaderBase::GetDataStatistics() const
{
    if (!SequentialBlockFetcher_) {
        return TDataStatistics();
    }

    TDataStatistics dataStatistics;
    dataStatistics.set_chunk_count(1);
    dataStatistics.set_uncompressed_data_size(SequentialBlockFetcher_->GetUncompressedDataSize());
    dataStatistics.set_compressed_data_size(SequentialBlockFetcher_->GetCompressedDataSize());
    return dataStatistics;
}

TCodecStatistics TChunkReaderBase::GetDecompressionStatistics() const
{
    TCodecStatistics statistics;
    if (SequentialBlockFetcher_) {
        statistics.Append(SequentialBlockFetcher_->GetDecompressionTime());
    }
    return statistics;
}

bool TChunkReaderBase::IsFetchingCompleted() const
{
    if (!SequentialBlockFetcher_) {
        return true;
    }
    return SequentialBlockFetcher_->IsFetchingCompleted();
}

std::vector<TChunkId> TChunkReaderBase::GetFailedChunkIds() const
{
    if (ReadyEvent_.IsSet() && !ReadyEvent_.Get().IsOK()) {
        return std::vector<TChunkId>(1, UnderlyingReader_->GetChunkId());
    } else {
        return std::vector<TChunkId>();
    }
}

TKey TChunkReaderBase::WidenKey(
    const TKey& key,
    std::optional<int> nullableKeyColumnCount,
    TChunkedMemoryPool* pool) const
{
    auto keyColumnCount = nullableKeyColumnCount.value_or(key.GetCount());
    YT_VERIFY(keyColumnCount >= key.GetCount());

    if (keyColumnCount == key.GetCount()) {
        return key;
    }

    auto wideKey = TMutableUnversionedRow::Allocate(pool, keyColumnCount);

    for (int index = 0; index < key.GetCount(); ++index) {
        wideKey[index] = key[index];
    }

    for (int index = key.GetCount(); index < keyColumnCount; ++index) {
        wideKey[index] = MakeUnversionedSentinelValue(EValueType::Null);
    }

    return wideKey;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
