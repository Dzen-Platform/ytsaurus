#pragma once

#include "public.h"
#include "row_sampler.h"
#include "schemaless_chunk_reader.h"
#include "config.h"

#include <yt/core/logging/log.h>

#include <yt/core/yson/lexer.h>

#include <yt/core/concurrency/async_semaphore.h>

#include <yt/ytlib/chunk_client/chunk_spec.pb.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

//! Reads legacy (prior to 0.17) table chunks exposing schemaless reader interface.
class TLegacyTableChunkReader
    : public ISchemalessChunkReader
{
public:
    TLegacyTableChunkReader(
        const NChunkClient::NProto::TChunkSpec& chunkSpec,
        TChunkReaderConfigPtr config,
        TChunkReaderOptionsPtr options,
        const TColumnFilter& columnFilter,
        TNameTablePtr nameTable,
        const TKeyColumns& keyColumns,
        NChunkClient::IChunkReaderPtr underlyingReader,
        NChunkClient::IBlockCachePtr blockCache);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;
    virtual TFuture<void> GetReadyEvent() override;

    virtual TNameTablePtr GetNameTable() const override;

    virtual TKeyColumns GetKeyColumns() const override;

    virtual i64 GetTableRowIndex() const override;

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;

    virtual bool IsFetchingCompleted() const override;

    virtual std::vector<NChunkClient::TChunkId> GetFailedChunkIds() const override;

private:
    struct TLegacyTableChunkReaderMemoryPoolTag {};

    struct TColumnInfo
    {
        int ChunkKeyIndex = -1;
        int ReaderKeyIndex = -1;
        i64 RowIndex = -1;
        bool InChannel = false;
    };

    class TInitializer;

    void SkipToKey(const TOwningKey& key);

    void ResetCurrentRow();

    void MakeAndValidateRow();

    TColumnInfo& GetColumnInfo(int id);

    bool FetchNextRow();
    bool ContinueFetchNextRow();
    bool DoFetchNextRow();

    void FinishReader();

    const NChunkClient::NProto::TChunkSpec ChunkSpec_;

    TChunkReaderConfigPtr Config_;
    TChunkReaderOptionsPtr Options_;

    NConcurrency::TAsyncSemaphore AsyncSemaphore_;

    NChunkClient::IChunkReaderPtr UnderlyingReader_;
    NChunkClient::TSequentialBlockFetcherPtr SequentialBlockFetcher_;
    TColumnFilter ColumnFilter_;
    TNameTablePtr NameTable_;
    TKeyColumns KeyColumns_;

    NChunkClient::TReadLimit UpperLimit_;

    TFuture<void> ReadyEvent_;
    TFuture<TSharedRef> CurrentBlock_;

    TIntrusivePtr<TInitializer> Initializer_;

    std::vector<TUnversionedValue> EmptyKey_;
    std::vector<TUnversionedValue> CurrentKey_;
    std::vector<TUnversionedValue> CurrentRow_;
    TChunkedMemoryPool MemoryPool_;

    std::vector<TColumnInfo> ColumnInfo_;

    i64 CurrentRowIndex_ = -1;
    i64 BeginRowIndex_ = 0;
    i64 EndRowIndex_ = 0;
    i64 RowCount_ = 0;

    int RowIndexId_ = -1;
    int RangeIndexId_ = -1;
    int TableIndexId_ = -1;

    int SystemColumnCount_;

    bool IsFinished_ = false;

    std::unique_ptr<IRowSampler> RowSampler_;

    int UnfetchedChannelIndex_ = -1;

    std::vector<TLegacyChannelReaderPtr> ChannelReaders_;

    NYson::TStatelessLexer Lexer_;

    NLogging::TLogger Logger;
};

DEFINE_REFCOUNTED_TYPE(TLegacyTableChunkReader)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
