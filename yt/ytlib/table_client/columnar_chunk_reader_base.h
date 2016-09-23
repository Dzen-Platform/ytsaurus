#pragma once

#include "public.h"
#include "chunk_meta_extensions.h"

#include <yt/ytlib/chunk_client/block_fetcher.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/data_statistics.pb.h>
#include <yt/ytlib/chunk_client/public.h>
#include <yt/ytlib/chunk_client/reader_base.h>
#include <yt/ytlib/chunk_client/read_limit.h>

#include <yt/ytlib/table_chunk_format/column_reader.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TColumnarChunkReaderBase
    : public virtual NChunkClient::IReaderBase
{
public:
    TColumnarChunkReaderBase(
        TChunkReaderConfigPtr config,
        NChunkClient::IChunkReaderPtr underlyingReader,
        NChunkClient::IBlockCachePtr blockCache);

protected:
    TColumnarChunkMetaPtr ChunkMeta_;

    TChunkReaderConfigPtr Config_;
    NChunkClient::IChunkReaderPtr UnderlyingReader_;
    NChunkClient::IBlockCachePtr BlockCache_;

    NConcurrency::TAsyncSemaphorePtr Semaphore_;
    NChunkClient::TBlockFetcherPtr BlockFetcher_;

    TFuture<void> ReadyEvent_ = VoidFuture;
    std::vector<TFuture<TSharedRef>> PendingBlocks_;

    struct TColumn
    {
        TColumn(NTableChunkFormat::IColumnReaderBase* reader, int chunkSchemaIndex)
            : ColumnReader(reader)
            , ColumnMetaIndex(chunkSchemaIndex)
        { }

        NTableChunkFormat::IColumnReaderBase* ColumnReader;
        int ColumnMetaIndex;
        std::vector<int> BlockIndexSequence;
        int PendingBlockIndex_ = 0;
    };

    std::vector<TColumn> Columns_;


    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;
    virtual bool IsFetchingCompleted() const override;
    virtual std::vector<NChunkClient::TChunkId> GetFailedChunkIds() const override;
    virtual TFuture<void> GetReadyEvent() override;

    void ResetExhaustedColumns();
    NChunkClient::TBlockFetcher::TBlockInfo CreateBlockInfo(int blockIndex) const;
    i64 GetSegmentIndex(const TColumn& column, i64 rowIndex) const;
    i64 GetLowerRowIndex(TKey key) const;
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

    i64 LowerRowIndex_;
    i64 SafeUpperRowIndex_;
    i64 HardUpperRowIndex_;

    void InitLowerRowIndex();
    void InitUpperRowIndex();

    void Initialize(TRange<std::unique_ptr<NTableChunkFormat::IUnversionedColumnReader>> keyReaders);

    void InitBlockFetcher();
    TFuture<void> RequestFirstBlocks();

    bool TryFetchNextRow();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
