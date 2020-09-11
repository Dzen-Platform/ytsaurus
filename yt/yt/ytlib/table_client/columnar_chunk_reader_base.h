#pragma once

#include "chunk_meta_extensions.h"
#include "timing_reader.h"

#include <yt/ytlib/chunk_client/public.h>
#include <yt/ytlib/chunk_client/block_fetcher.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/block.h>

#include <yt/ytlib/table_chunk_format/column_reader.h>

#include <yt/client/chunk_client/proto/data_statistics.pb.h>
#include <yt/client/chunk_client/reader_base.h>
#include <yt/client/chunk_client/read_limit.h>

#include <yt/library/random/bernoulli_sampler.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TColumnarChunkReaderBase
    : public virtual NChunkClient::IReaderBase
    , public TTimingReaderBase
{
public:
    TColumnarChunkReaderBase(
        TColumnarChunkMetaPtr chunkMeta,
        TChunkReaderConfigPtr config,
        NChunkClient::IChunkReaderPtr underlyingReader,
        NChunkClient::IBlockCachePtr blockCache,
        const NChunkClient::TClientBlockReadOptions& blockReadOptions,
        std::function<void(int /*skippedRowCount*/)> onRowsSkipped,
        const NChunkClient::TChunkReaderMemoryManagerPtr& memoryManager = nullptr);

protected:
    const TColumnarChunkMetaPtr ChunkMeta_;
    const TChunkReaderConfigPtr Config_;
    const NChunkClient::IChunkReaderPtr UnderlyingReader_;
    const NChunkClient::IBlockCachePtr BlockCache_;
    const NChunkClient::TClientBlockReadOptions BlockReadOptions_;

    TBernoulliSampler Sampler_;

    std::function<void(int)> OnRowsSkipped_;

    NChunkClient::TChunkReaderMemoryManagerPtr MemoryManager_;
    NChunkClient::TBlockFetcherPtr BlockFetcher_;

    std::vector<TFuture<NChunkClient::TBlock>> PendingBlocks_;

    i64 RequiredMemorySize_ = 0;

    struct TColumn
    {
        TColumn(std::unique_ptr<NTableChunkFormat::IColumnReaderBase> reader, int columnMetaIndex = -1, int columnId = -1)
            : ColumnReader(std::move(reader))
            , ColumnMetaIndex(columnMetaIndex)
            , ColumnId(columnId)
        { }

        std::unique_ptr<NTableChunkFormat::IColumnReaderBase> ColumnReader;
        int ColumnMetaIndex;
        int ColumnId;
        std::vector<int> BlockIndexSequence;
        int PendingBlockIndex = 0;
    };

    std::vector<TColumn> Columns_;

    std::optional<int> SampledColumnIndex_;
    std::vector<NChunkClient::TReadRange> SampledRanges_;
    int SampledRangeIndex_ = 0;
    bool SampledRangeIndexChanged_ = false;

    bool IsSamplingCompleted_ = false;

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;
    virtual NChunkClient::TCodecStatistics GetDecompressionStatistics() const override;
    virtual bool IsFetchingCompleted() const override;
    virtual std::vector<NChunkClient::TChunkId> GetFailedChunkIds() const override;

    void FeedBlocksToReaders();
    void ArmColumnReaders();
    i64 GetReadyRowCount() const;

    NChunkClient::TBlockFetcher::TBlockInfo CreateBlockInfo(int blockIndex) const;
    i64 GetSegmentIndex(const TColumn& column, i64 rowIndex) const;
    i64 GetLowerRowIndex(TKey key) const;

    //! Returns |true| if block sampling is enabled and all sampling ranges have been read.
    bool IsSamplingCompleted() const;
};

////////////////////////////////////////////////////////////////////////////////

class TColumnarRangeChunkReaderBase
    : public TColumnarChunkReaderBase
{
public:
    using TColumnarChunkReaderBase::TColumnarChunkReaderBase;

protected:
    NChunkClient::TReadLimit LowerLimit_;
    NChunkClient::TReadLimit UpperLimit_;

    // Lower limit (both, key and row index) is greater or equal than this row index.
    // No need to read and check keys with lesser row indexes.
    i64 LowerRowIndex_;

    // Upper limit (both, key and row index) is greater or equal than this row index.
    // No need to check keys below this row index.
    i64 SafeUpperRowIndex_;

    // Upper limit (both, key and row index) is less or equal than this row index.
    // We should check UpperLimit_.GetKey() between SafeUpperRowIndex and HardUpperRowIndex.
    i64 HardUpperRowIndex_;

    void InitLowerRowIndex();
    void InitUpperRowIndex();

    void Initialize(TRange<NTableChunkFormat::IUnversionedColumnReader*> keyReaders);

    void InitBlockFetcher();
    TFuture<void> RequestFirstBlocks();

    bool TryFetchNextRow();
};

////////////////////////////////////////////////////////////////////////////////

class TColumnarLookupChunkReaderBase
    : public TColumnarChunkReaderBase
{
public:
    using TColumnarChunkReaderBase::TColumnarChunkReaderBase;

protected:
    TSharedRange<TKey> Keys_;
    std::vector<i64> RowIndexes_;
    i64 NextKeyIndex_ = 0;

    void Initialize();
    void InitBlockFetcher();
    TFuture<void> RequestFirstBlocks();

    bool TryFetchNextRow();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
