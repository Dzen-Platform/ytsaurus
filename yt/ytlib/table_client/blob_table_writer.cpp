#include "blob_table_writer.h"

#include "helpers.h"
#include "name_table.h"
#include "private.h"
#include "public.h"
#include "schemaless_chunk_writer.h"

#include <yt/core/yson/lexer.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/table_client/unversioned_row.h>

namespace NYT {
namespace NTableClient {

static const auto& Logger = TableClientLogger;

using namespace NYson;

using NConcurrency::WaitFor;
using NCypressClient::TTransactionId;
using NChunkClient::TChunkListId;

////////////////////////////////////////////////////////////////////////////////

TTableSchema TBlobTableSchema::ToTableSchema() const
{
    auto columns = BlobIdColumns;
    for (auto& idColumn : columns) {
        idColumn.SetSortOrder(ESortOrder::Ascending);
    }
    columns.emplace_back(PartIndexColumn, EValueType::Int64);
    columns.back().SetSortOrder(ESortOrder::Ascending);
    columns.emplace_back(DataColumn, EValueType::String);
    return TTableSchema(
        std::move(columns),
        true, // strict
        true); // uniqueKeys
}

////////////////////////////////////////////////////////////////////////////////

TBlobTableWriter::TBlobTableWriter(
    const TBlobTableSchema& blobTableSchema,
    const std::vector<TYsonString>& blobIdColumnValues,
    NApi::INativeClientPtr client,
    TBlobTableWriterConfigPtr blobTableWriterConfig,
    TTableWriterOptionsPtr tableWriterOptions,
    const TTransactionId& transactionId,
    const TChunkListId& chunkListId)
    : PartSize_(blobTableWriterConfig->MaxPartSize)
{
    LOG_INFO("Creating blob writer (TransactionId: %v, ChunkListId %v)",
        transactionId,
        chunkListId);

    Buffer_.Reserve(PartSize_);

    auto tableSchema = blobTableSchema.ToTableSchema();
    auto nameTable = TNameTable::FromSchema(tableSchema);

    for (const auto& column : blobTableSchema.BlobIdColumns) {
        BlobIdColumnIds_.emplace_back(nameTable->GetIdOrThrow(column.Name));
    }

    TStatelessLexer lexer;
    TUnversionedOwningRowBuilder builder;

    YCHECK(blobIdColumnValues.size() == blobTableSchema.BlobIdColumns.size());
    for (size_t i = 0; i < BlobIdColumnIds_.size(); ++i) {
        builder.AddValue(MakeUnversionedValue(blobIdColumnValues[i].Data(), BlobIdColumnIds_[i], lexer));
    }
    BlobIdColumnValues_ = builder.FinishRow();

    PartIndexColumnId_ = nameTable->GetIdOrThrow(blobTableSchema.PartIndexColumn);
    DataColumnId_ = nameTable->GetIdOrThrow(blobTableSchema.DataColumn);

    MultiChunkWriter_ = CreateSchemalessMultiChunkWriter(
        blobTableWriterConfig,
        tableWriterOptions,
        nameTable,
        tableSchema,
        TOwningKey(),
        client,
        NObjectClient::CellTagFromId(chunkListId),
        transactionId,
        chunkListId);

    WaitFor(MultiChunkWriter_->Open())
        .ThrowOnError();
}

TBlobTableWriter::~TBlobTableWriter()
{
    try {
        DoFinish();
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Error finishing blob table writer");
    } catch (...) {
        Y_UNREACHABLE();
    }
}

NScheduler::NProto::TOutputResult TBlobTableWriter::GetOutputResult() const
{
    return GetWrittenChunksBoundaryKeys(MultiChunkWriter_);
}

void TBlobTableWriter::DoWrite(const void* buf_, size_t size)
{
    const char* buf = static_cast<const char*>(buf_);
    while (size > 0) {
        const size_t remainingCapacity = Buffer_.Capacity() - Buffer_.Size();
        const size_t toWrite = std::min(size, remainingCapacity);
        Buffer_.Write(buf, toWrite);
        buf += toWrite;
        size -= toWrite;
        if (Buffer_.Size() == Buffer_.Capacity()) {
            Flush();
        }
    }
}

void TBlobTableWriter::DoFlush()
{
    if (Buffer_.Size() == 0) {
        return;
    }

    auto dataBuf = TStringBuf(Buffer_.Begin(), Buffer_.Size());

    const size_t columnCount = BlobIdColumnIds_.size() + 2;

    TUnversionedRowBuilder builder(columnCount);
    for (const auto* value = BlobIdColumnValues_.Begin(); value != BlobIdColumnValues_.End(); value++) {
        builder.AddValue(*value);
    }
    builder.AddValue(MakeUnversionedInt64Value(WrittenPartCount_, PartIndexColumnId_));
    builder.AddValue(MakeUnversionedStringValue(dataBuf, DataColumnId_));

    ++WrittenPartCount_;

    if (!MultiChunkWriter_->Write({builder.GetRow()})) {
        WaitFor(MultiChunkWriter_->GetReadyEvent())
            .ThrowOnError();
    }
    Buffer_.Clear();
}

void TBlobTableWriter::DoFinish()
{
    if (Finished_) {
        return;
    }
    Finished_ = true;
    Flush();
    WaitFor(MultiChunkWriter_->Close())
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
