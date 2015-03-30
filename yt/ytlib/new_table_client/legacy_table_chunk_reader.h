#pragma once

#include "public.h"

#include "schemaless_chunk_reader.h"

#include <core/logging/log.h>

#include <core/yson/lexer.h>

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

//! Reads legacy (prior to 0.17) table chunks exposing schemaless reader interface.
class TLegacyTableChunkReader
    : public ISchemalessChunkReader
{
public:
    TLegacyTableChunkReader(
        TChunkReaderConfigPtr config,
        const TColumnFilter& columnFilter,
        TNameTablePtr nameTable,
        const TKeyColumns& keyColumns,
        NChunkClient::IChunkReaderPtr underlyingReader,
        NChunkClient::IBlockCachePtr blockCache,
        const NChunkClient::TReadLimit& lowerLimit,
        const NChunkClient::TReadLimit& upperLimit,
        i64 tableRowIndex);

    virtual TFuture<void> Open() override;
    virtual bool Read(std::vector<TUnversionedRow>* rows) override;
    virtual TFuture<void> GetReadyEvent() override;

    virtual TNameTablePtr GetNameTable() const override;

    virtual i64 GetTableRowIndex() const override;

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;
    virtual TFuture<void> GetFetchingCompletedEvent() override;

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

    NChunkClient::TSequentialReaderPtr SequentialReader_;
    TColumnFilter ColumnFilter_;
    TNameTablePtr NameTable_;

    NChunkClient::TReadLimit UpperLimit_;

    TFuture<void> ReadyEvent_;

    TIntrusivePtr<TInitializer> Initializer_;

    std::vector<TUnversionedValue> EmptyKey_;
    std::vector<TUnversionedValue> CurrentKey_;
    std::vector<TUnversionedValue> CurrentRow_;
    TChunkedMemoryPool MemoryPool_;

    std::vector<TColumnInfo> ColumnInfo_;

    i64 TableRowIndex_;

    int KeyColumnCount_;

    i64 CurrentRowIndex_ = -1;
    i64 BeginRowIndex_ = 0;
    i64 EndRowIndex_ = 0;

    int UnfetchedChannelIndex_ = -1;

    std::vector<TLegacyChannelReaderPtr> ChannelReaders_;

    NYson::TStatelessLexer Lexer_;

    NLogging::TLogger Logger;
};

DEFINE_REFCOUNTED_TYPE(TLegacyTableChunkReader)

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
