#include "columnar_chunk_reader_base.h"
#include "columnar_chunk_meta.h"

#include "config.h"

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/chunk_reader_memory_manager.h>

#include <yt/client/table_client/unversioned_row.h>

#include <yt/core/concurrency/async_semaphore.h>

namespace NYT::NTableClient {

using namespace NConcurrency;
using namespace NTableClient::NProto;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTableChunkFormat;
using namespace NTableChunkFormat::NProto;

using NChunkClient::TLegacyReadLimit;

////////////////////////////////////////////////////////////////////////////////

TColumnarChunkReaderBase::TColumnarChunkReaderBase(
    TColumnarChunkMetaPtr chunkMeta,
    TChunkReaderConfigPtr config,
    IChunkReaderPtr underlyingReader,
    IBlockCachePtr blockCache,
    const TClientBlockReadOptions& blockReadOptions,
    std::function<void(int)> onRowsSkipped,
    const TChunkReaderMemoryManagerPtr& memoryManager)
    : ChunkMeta_(std::move(chunkMeta))
    , Config_(std::move(config))
    , UnderlyingReader_(std::move(underlyingReader))
    , BlockCache_(std::move(blockCache))
    , BlockReadOptions_(blockReadOptions)
    , Sampler_(Config_->SamplingRate, std::random_device()())
    , OnRowsSkipped_(onRowsSkipped)
{
    if (memoryManager) {
        MemoryManager_ = memoryManager;
    } else {
        MemoryManager_ = New<TChunkReaderMemoryManager>(TChunkReaderMemoryManagerOptions(Config_->WindowSize));
    }

    if (Config_->SamplingSeed) {
        auto chunkId = UnderlyingReader_->GetChunkId();
        auto seed = *Config_->SamplingSeed;
        seed ^= FarmFingerprint(chunkId.Parts64[0]);
        seed ^= FarmFingerprint(chunkId.Parts64[1]);
        Sampler_ = TBernoulliSampler(Config_->SamplingRate, seed);
    }
}

TDataStatistics TColumnarChunkReaderBase::GetDataStatistics() const
{
    if (!BlockFetcher_) {
        return TDataStatistics();
    }

    TDataStatistics dataStatistics;
    dataStatistics.set_chunk_count(1);
    dataStatistics.set_uncompressed_data_size(BlockFetcher_->GetUncompressedDataSize());
    dataStatistics.set_compressed_data_size(BlockFetcher_->GetCompressedDataSize());
    return dataStatistics;
}

TCodecStatistics TColumnarChunkReaderBase::GetDecompressionStatistics() const
{
    return BlockFetcher_
        ? TCodecStatistics().Append(BlockFetcher_->GetDecompressionTime())
        : TCodecStatistics();
}

bool TColumnarChunkReaderBase::IsFetchingCompleted() const
{
    if (BlockFetcher_) {
        return BlockFetcher_->IsFetchingCompleted();
    } else {
        return true;
    }
}

std::vector<TChunkId> TColumnarChunkReaderBase::GetFailedChunkIds() const
{
    if (ReadyEvent().IsSet() && !ReadyEvent().Get().IsOK()) {
        return { UnderlyingReader_->GetChunkId() };
    } else {
        return std::vector<TChunkId>();
    }
}

void TColumnarChunkReaderBase::FeedBlocksToReaders()
{
    for (int i = 0; i < PendingBlocks_.size(); ++i) {
        const auto& blockFuture = PendingBlocks_[i];
        const auto& column = Columns_[i];
        const auto& columnReader = column.ColumnReader;
        if (blockFuture) {
            YT_VERIFY(blockFuture.IsSet() && blockFuture.Get().IsOK());

            if (columnReader->GetCurrentBlockIndex() != -1) {
                RequiredMemorySize_ -= BlockFetcher_->GetBlockSize(columnReader->GetCurrentBlockIndex());
            }
            MemoryManager_->SetRequiredMemorySize(RequiredMemorySize_);

            const auto& block = blockFuture.Get().Value();
            columnReader->SetCurrentBlock(block.Data, column.PendingBlockIndex);
        }
    }

    if (SampledRangeIndexChanged_) {
        auto rowIndex = SampledRanges_[SampledRangeIndex_].LowerLimit().GetRowIndex();
        for (auto& column : Columns_) {
            column.ColumnReader->SkipToRowIndex(rowIndex);
        }

        SampledRangeIndexChanged_ = false;
    }

    PendingBlocks_.clear();
}

void TColumnarChunkReaderBase::ArmColumnReaders()
{
    for (const auto& column : Columns_) {
        column.ColumnReader->Rearm();
    }
}

i64 TColumnarChunkReaderBase::GetReadyRowCount() const
{
    i64 result = Max<i64>();
    for (const auto& column : Columns_) {
        const auto& reader = column.ColumnReader;
        result = std::min(
            result,
            reader->GetReadyUpperRowIndex() - reader->GetCurrentRowIndex());
        if (SampledColumnIndex_) {
            const auto& sampledColumnReader = Columns_[*SampledColumnIndex_].ColumnReader;
            result = std::min(
                result,
                SampledRanges_[SampledRangeIndex_].UpperLimit().GetRowIndex() - sampledColumnReader->GetCurrentRowIndex());
        }
    }
    return result;
}

TBlockFetcher::TBlockInfo TColumnarChunkReaderBase::CreateBlockInfo(int blockIndex) const
{
    YT_VERIFY(ChunkMeta_);
    const auto& blockMeta = ChunkMeta_->BlockMeta()->blocks(blockIndex);
    TBlockFetcher::TBlockInfo blockInfo;
    blockInfo.Index = blockIndex;
    blockInfo.Priority = blockMeta.chunk_row_count() - blockMeta.row_count();
    blockInfo.UncompressedDataSize = blockMeta.uncompressed_size();
    return blockInfo;
}

i64 TColumnarChunkReaderBase::GetSegmentIndex(const TColumn& column, i64 rowIndex) const
{
    YT_VERIFY(ChunkMeta_);
    const auto& columnMeta = ChunkMeta_->ColumnMeta()->columns(column.ColumnMetaIndex);
    auto it = std::lower_bound(
        columnMeta.segments().begin(),
        columnMeta.segments().end(),
        rowIndex,
        [](const TSegmentMeta& segmentMeta, i64 index) {
            return segmentMeta.chunk_row_count() < index + 1;
        });
    return std::distance(columnMeta.segments().begin(), it);
}

i64 TColumnarChunkReaderBase::GetLowerRowIndex(TLegacyKey key) const
{
    YT_VERIFY(ChunkMeta_);
    auto it = std::lower_bound(
        ChunkMeta_->BlockLastKeys().begin(),
        ChunkMeta_->BlockLastKeys().end(),
        key);

    if (it == ChunkMeta_->BlockLastKeys().end()) {
        return ChunkMeta_->Misc().row_count();
    }

    if (it == ChunkMeta_->BlockLastKeys().begin()) {
        return 0;
    }
    --it;

    int blockIndex = std::distance(ChunkMeta_->BlockLastKeys().begin(), it);
    const auto& blockMeta = ChunkMeta_->BlockMeta()->blocks(blockIndex);
    return blockMeta.chunk_row_count();
}

////////////////////////////////////////////////////////////////////////////////

void TColumnarRangeChunkReaderBase::InitLowerRowIndex()
{
    LowerRowIndex_ = 0;
    if (LowerLimit_.HasRowIndex()) {
        LowerRowIndex_ = std::max(LowerRowIndex_, LowerLimit_.GetRowIndex());
    }

    if (LowerLimit_.HasLegacyKey()) {
        LowerRowIndex_ = std::max(LowerRowIndex_, GetLowerRowIndex(LowerLimit_.GetLegacyKey()));
    }
}

void TColumnarRangeChunkReaderBase::InitUpperRowIndex()
{
    SafeUpperRowIndex_ = HardUpperRowIndex_ = ChunkMeta_->Misc().row_count();
    if (UpperLimit_.HasRowIndex()) {
        SafeUpperRowIndex_ = HardUpperRowIndex_ = std::min(HardUpperRowIndex_, UpperLimit_.GetRowIndex());
    }

    if (UpperLimit_.HasLegacyKey()) {
        auto it = std::lower_bound(
            ChunkMeta_->BlockLastKeys().begin(),
            ChunkMeta_->BlockLastKeys().end(),
            UpperLimit_.GetLegacyKey());

        if (it == ChunkMeta_->BlockLastKeys().end()) {
            SafeUpperRowIndex_ = HardUpperRowIndex_ = std::min(HardUpperRowIndex_, ChunkMeta_->Misc().row_count());
        } else {
            int blockIndex = std::distance(ChunkMeta_->BlockLastKeys().begin(), it);
            const auto& blockMeta = ChunkMeta_->BlockMeta()->blocks(blockIndex);

            HardUpperRowIndex_ = std::min(
                HardUpperRowIndex_,
                blockMeta.chunk_row_count());

            if (it == ChunkMeta_->BlockLastKeys().begin()) {
                SafeUpperRowIndex_ = 0;
            } else {
                --it;

                int prevBlockIndex = std::distance(ChunkMeta_->BlockLastKeys().begin(), it);
                const auto& prevBlockMeta = ChunkMeta_->BlockMeta()->blocks(prevBlockIndex);

                SafeUpperRowIndex_ = std::min(
                    SafeUpperRowIndex_,
                    prevBlockMeta.chunk_row_count());
            }
        }
    }
}

void TColumnarRangeChunkReaderBase::Initialize(NYT::TRange<IUnversionedColumnReader*> keyReaders)
{
    for (const auto& column : Columns_) {
        column.ColumnReader->SkipToRowIndex(LowerRowIndex_);
    }

    if (!LowerLimit_.HasLegacyKey()) {
        return;
    }

    YT_VERIFY(keyReaders.Size() > 0);

    i64 lowerRowIndex = keyReaders[0]->GetCurrentRowIndex();
    i64 upperRowIndex = keyReaders[0]->GetBlockUpperRowIndex();
    int count = std::min(LowerLimit_.GetLegacyKey().GetCount(), static_cast<int>(keyReaders.Size()));
    for (int i = 0; i < count; ++i) {
        std::tie(lowerRowIndex, upperRowIndex) = keyReaders[i]->GetEqualRange(
            LowerLimit_.GetLegacyKey().Begin()[i],
            lowerRowIndex,
            upperRowIndex);
    }

    LowerRowIndex_ = count == LowerLimit_.GetLegacyKey().GetCount()
        ? lowerRowIndex
        : upperRowIndex;
    YT_VERIFY(LowerRowIndex_ < ChunkMeta_->Misc().row_count());
    for (const auto& column : Columns_) {
        column.ColumnReader->SkipToRowIndex(LowerRowIndex_);
    }
}

void TColumnarRangeChunkReaderBase::InitBlockFetcher()
{
    YT_VERIFY(LowerRowIndex_ < ChunkMeta_->Misc().row_count());

    std::vector<TBlockFetcher::TBlockInfo> blockInfos;

    if (Config_->SamplingMode == ESamplingMode::Block) {
        // Select column to sample.
        int maxColumnSegmentCount = -1;
        for (int columnIndex = 0; columnIndex < Columns_.size(); ++columnIndex) {
            const auto& column = Columns_[columnIndex];
            auto columnMetaIndex = column.ColumnMetaIndex;
            if (columnMetaIndex < 0) {
                continue;
            }
            const auto& columnMeta = ChunkMeta_->ColumnMeta()->columns(columnMetaIndex);
            auto columnSegmentCount = columnMeta.segments_size();
            if (columnSegmentCount > maxColumnSegmentCount) {
                maxColumnSegmentCount = columnSegmentCount;
                SampledColumnIndex_ = columnIndex;
            }
        }

        if (!SampledColumnIndex_) {
            return;
        }

        // Sample column blocks.
        const auto& column = Columns_[*SampledColumnIndex_];
        const auto& columnMeta = ChunkMeta_->ColumnMeta()->columns(column.ColumnMetaIndex);
        int segmentIndex = GetSegmentIndex(column, LowerRowIndex_);
        while (segmentIndex < columnMeta.segments_size()) {
            const auto& segment = columnMeta.segments(segmentIndex);
            if (segment.chunk_row_count() - segment.row_count() > HardUpperRowIndex_) {
                break;
            }
            auto blockIndex = segment.block_index();
            int nextBlockSegmentIndex = segmentIndex;
            while (
                nextBlockSegmentIndex < columnMeta.segments_size() &&
                columnMeta.segments(nextBlockSegmentIndex).block_index() == blockIndex)
            {
                ++nextBlockSegmentIndex;
            }

            const auto& lastBlockSegment = columnMeta.segments(nextBlockSegmentIndex - 1);
            if (Sampler_.Sample(blockIndex)) {
                NChunkClient::TLegacyReadRange readRange;
                readRange.LowerLimit().SetRowIndex(std::max<i64>(segment.chunk_row_count() - segment.row_count(), LowerRowIndex_));
                readRange.UpperLimit().SetRowIndex(std::min<i64>(lastBlockSegment.chunk_row_count(), HardUpperRowIndex_ + 1));
                SampledRanges_.push_back(std::move(readRange));
            }

            segmentIndex = nextBlockSegmentIndex;
        }

        if (SampledRanges_.empty()) {
            IsSamplingCompleted_ = true;
        } else {
            LowerRowIndex_ = SampledRanges_[0].LowerLimit().GetRowIndex();
        }
    }

    for (auto& column : Columns_) {
        if (column.ColumnMetaIndex < 0) {
            // Column without meta, blocks, etc.
            // E.g. NullColumnReader.
            continue;
        }

        const auto& columnMeta = ChunkMeta_->ColumnMeta()->columns(column.ColumnMetaIndex);
        i64 segmentIndex = GetSegmentIndex(column, LowerRowIndex_);

        int lastBlockIndex = -1;
        int sampledRangeIndex = 0;
        for (; segmentIndex < columnMeta.segments_size(); ++segmentIndex) {
            const auto& segment = columnMeta.segments(segmentIndex);
            int firstRowIndex = segment.chunk_row_count() - segment.row_count();
            int lastRowIndex = segment.chunk_row_count() - 1;
            if (SampledColumnIndex_) {
                while (
                    sampledRangeIndex < SampledRanges_.size() &&
                    SampledRanges_[sampledRangeIndex].UpperLimit().GetRowIndex() <= firstRowIndex)
                {
                    ++sampledRangeIndex;
                }
                if (sampledRangeIndex == SampledRanges_.size()) {
                    break;
                }
                if (SampledRanges_[sampledRangeIndex].LowerLimit().GetRowIndex() > lastRowIndex) {
                    continue;
                }
            }
            if (segment.block_index() != lastBlockIndex) {
                lastBlockIndex = segment.block_index();
                if (column.BlockIndexSequence.empty()) {
                    column.BlockIndexSequence.push_back(lastBlockIndex);
                }
                blockInfos.push_back(CreateBlockInfo(lastBlockIndex));
            }
            if (segment.chunk_row_count() > HardUpperRowIndex_) {
                break;
            }
        }
    }

    if (!blockInfos.empty()) {
        BlockFetcher_ = New<TBlockFetcher>(
            Config_,
            std::move(blockInfos),
            MemoryManager_,
            UnderlyingReader_,
            BlockCache_,
            CheckedEnumCast<NCompression::ECodec>(ChunkMeta_->Misc().compression_codec()),
            static_cast<double>(ChunkMeta_->Misc().compressed_data_size()) / ChunkMeta_->Misc().uncompressed_data_size(),
            BlockReadOptions_);
    }
}

TFuture<void> TColumnarRangeChunkReaderBase::RequestFirstBlocks()
{
    PendingBlocks_.clear();

    std::vector<TFuture<void>> blockFetchResult;
    for (auto& column : Columns_) {
        if (column.BlockIndexSequence.empty()) {
            // E.g. NullColumnReader.
            PendingBlocks_.emplace_back();
        } else {
            column.PendingBlockIndex = column.BlockIndexSequence.front();
            RequiredMemorySize_ += BlockFetcher_->GetBlockSize(column.PendingBlockIndex);
            MemoryManager_->SetRequiredMemorySize(RequiredMemorySize_);
            PendingBlocks_.push_back(BlockFetcher_->FetchBlock(column.PendingBlockIndex));
            blockFetchResult.push_back(PendingBlocks_.back().template As<void>());
        }
    }

    if (PendingBlocks_.empty()) {
        return VoidFuture;
    } else {
        return AllSucceeded(blockFetchResult);
    }
}

bool TColumnarRangeChunkReaderBase::TryFetchNextRow()
{
    std::vector<TFuture<void>> blockFetchResult;
    YT_VERIFY(PendingBlocks_.empty());
    YT_VERIFY(!IsSamplingCompleted_);

    if (SampledColumnIndex_) {
        auto& sampledColumn = Columns_[*SampledColumnIndex_];
        const auto& sampledColumnReader = sampledColumn.ColumnReader;
        if (sampledColumnReader->GetCurrentRowIndex() == SampledRanges_[SampledRangeIndex_].UpperLimit().GetRowIndex()) {
            ++SampledRangeIndex_;
            SampledRangeIndexChanged_ = true;
            if (SampledRangeIndex_ == SampledRanges_.size()) {
                IsSamplingCompleted_ = true;
                return false;
            }

            int rowsSkipped = SampledRanges_[SampledRangeIndex_].LowerLimit().GetRowIndex() - sampledColumnReader->GetCurrentRowIndex();
            if (OnRowsSkipped_) {
                OnRowsSkipped_(rowsSkipped);
            }
        }
    }

    for (int columnIndex = 0; columnIndex < Columns_.size(); ++columnIndex) {
        auto& column = Columns_[columnIndex];
        const auto& columnReader = column.ColumnReader;
        auto currentRowIndex = columnReader->GetCurrentRowIndex();
        if (SampledRangeIndexChanged_) {
            currentRowIndex = SampledRanges_[SampledRangeIndex_].LowerLimit().GetRowIndex();
        }

        if (currentRowIndex >= columnReader->GetBlockUpperRowIndex()) {
            while (PendingBlocks_.size() < columnIndex) {
                PendingBlocks_.emplace_back();
            }

            const auto& columnMeta = ChunkMeta_->ColumnMeta()->columns(column.ColumnMetaIndex);
            int nextSegmentIndex = columnReader->GetCurrentSegmentIndex();
            while (columnMeta.segments(nextSegmentIndex).chunk_row_count() <= currentRowIndex) {
                ++nextSegmentIndex;
            }
            const auto& nextSegment = columnMeta.segments(nextSegmentIndex);
            auto nextBlockIndex = nextSegment.block_index();
            column.PendingBlockIndex = nextBlockIndex;

            RequiredMemorySize_ += BlockFetcher_->GetBlockSize(column.PendingBlockIndex);
            MemoryManager_->SetRequiredMemorySize(RequiredMemorySize_);
            PendingBlocks_.push_back(BlockFetcher_->FetchBlock(column.PendingBlockIndex));
            blockFetchResult.push_back(PendingBlocks_.back().template As<void>());
        }
    }

    if (!blockFetchResult.empty()) {
        SetReadyEvent(AllSucceeded(blockFetchResult));
    }

    return PendingBlocks_.empty();
}

bool TColumnarChunkReaderBase::IsSamplingCompleted() const
{
    return IsSamplingCompleted_;
}

////////////////////////////////////////////////////////////////////////////////

void TColumnarLookupChunkReaderBase::Initialize()
{
    RowIndexes_.reserve(Keys_.Size());
    for (const auto& key : Keys_) {
        RowIndexes_.push_back(GetLowerRowIndex(key));
    }

    for (auto& column : Columns_) {
        if (column.ColumnMetaIndex < 0) {
            // E.g. null column reader for widened keys.
            continue;
        }

        for (auto rowIndex : RowIndexes_) {
            if (rowIndex < ChunkMeta_->Misc().row_count()) {
                const auto& columnMeta = ChunkMeta_->ColumnMeta()->columns(column.ColumnMetaIndex);
                auto segmentIndex = GetSegmentIndex(column, rowIndex);
                const auto& segment = columnMeta.segments(segmentIndex);
                column.BlockIndexSequence.push_back(segment.block_index());
            } else {
                // All keys left are outside boundary keys.
                break;
            }
        }
    }

    InitBlockFetcher();
}

void TColumnarLookupChunkReaderBase::InitBlockFetcher()
{
    std::vector<TBlockFetcher::TBlockInfo> blockInfos;
    for (const auto& column : Columns_) {
        int lastBlockIndex = -1;
        for (auto blockIndex : column.BlockIndexSequence) {
            if (blockIndex != lastBlockIndex) {
                lastBlockIndex = blockIndex;
                blockInfos.push_back(CreateBlockInfo(lastBlockIndex));
            }
        }
    }

    if (blockInfos.empty()) {
        return;
    }

    BlockFetcher_ = New<TBlockFetcher>(
        Config_,
        std::move(blockInfos),
        MemoryManager_,
        UnderlyingReader_,
        BlockCache_,
        CheckedEnumCast<NCompression::ECodec>(ChunkMeta_->Misc().compression_codec()),
        static_cast<double>(ChunkMeta_->Misc().compressed_data_size()) / ChunkMeta_->Misc().uncompressed_data_size(),
        BlockReadOptions_);
}

bool TColumnarLookupChunkReaderBase::TryFetchNextRow()
{
    SetReadyEvent(RequestFirstBlocks());
    return PendingBlocks_.empty();
}

TFuture<void> TColumnarLookupChunkReaderBase::RequestFirstBlocks()
{
    if (RowIndexes_[NextKeyIndex_] >= ChunkMeta_->Misc().row_count()) {
        return VoidFuture;
    }

    std::vector<TFuture<void>> blockFetchResult;
    PendingBlocks_.clear();
    for (int i = 0; i < Columns_.size(); ++i) {
        auto& column = Columns_[i];

        if (column.ColumnMetaIndex < 0) {
            // E.g. null column reader for widened keys.
            continue;
        }

        if (column.ColumnReader->GetCurrentBlockIndex() != column.BlockIndexSequence[NextKeyIndex_]) {
            while (PendingBlocks_.size() < i) {
                PendingBlocks_.emplace_back();
            }

            column.PendingBlockIndex = column.BlockIndexSequence[NextKeyIndex_];
            RequiredMemorySize_ += BlockFetcher_->GetBlockSize(column.PendingBlockIndex);
            MemoryManager_->SetRequiredMemorySize(RequiredMemorySize_);
            PendingBlocks_.push_back(BlockFetcher_->FetchBlock(column.PendingBlockIndex));
            blockFetchResult.push_back(PendingBlocks_.back().As<void>());
        }
    }

    return AllSucceeded(blockFetchResult);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
