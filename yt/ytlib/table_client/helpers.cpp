#include "helpers.h"
#include "chunk_meta_extensions.h"
#include "config.h"
#include "schemaless_chunk_reader.h"
#include "schemaless_chunk_writer.h"
#include "private.h"
#include "schemaless_reader.h"
#include "schemaless_writer.h"
#include "name_table.h"

#include <yt/ytlib/formats/parser.h>

#include <yt/ytlib/scheduler/job.pb.h>

#include <yt/ytlib/ypath/rich.h>

#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/async_stream.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/node.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NFormats;
using namespace NProto;
using namespace NScheduler::NProto;
using namespace NYson;
using namespace NYTree;
using namespace NCypressClient;
using namespace NChunkClient;

using NChunkClient::TChannel;
using NYPath::TRichYPath;


//////////////////////////////////////////////////////////////////////////////////

TTableOutput::TTableOutput(const TFormat& format, IYsonConsumer* consumer)
    : Parser_(CreateParserForFormat(format, EDataType::Tabular, consumer))
{ }

TTableOutput::TTableOutput(std::unique_ptr<IParser> parser)
    : Parser_(std::move(parser))
{ }

TTableOutput::~TTableOutput() throw() = default;

void TTableOutput::DoWrite(const void* buf, size_t len)
{
    YCHECK(IsParserValid_);
    try {
        Parser_->Read(TStringBuf(static_cast<const char*>(buf), len));
    } catch (const std::exception& ex) {
        IsParserValid_ = false;
        throw;
    }
}

void TTableOutput::DoFinish()
{
    if (IsParserValid_) {
        // Dump everything into consumer.
        Parser_->Finish();
    }
}

//////////////////////////////////////////////////////////////////////////////////

void PipeReaderToWriter(
    ISchemalessReaderPtr reader,
    ISchemalessWriterPtr writer,
    int bufferRowCount,
    bool validateValues,
    NConcurrency::IThroughputThrottlerPtr throttler)
{
    std::vector<TUnversionedRow> rows;
    rows.reserve(bufferRowCount);

    while (reader->Read(&rows)) {
        if (rows.empty()) {
            WaitFor(reader->GetReadyEvent())
                .ThrowOnError();
            continue;
        }

        if (validateValues) {
            for (const auto row : rows) {
                for (const auto& value : row) {
                    ValidateStaticValue(value);
                }
            }
        }

        if (throttler) {
            i64 dataWeight = 0;
            for (const auto row : rows) {
                dataWeight += GetDataWeight(row);
            }
            WaitFor(throttler->Throttle(dataWeight))
                .ThrowOnError();
        }

        if (!writer->Write(rows)) {
            WaitFor(writer->GetReadyEvent())
                .ThrowOnError();
        }
    }

    WaitFor(writer->Close())
        .ThrowOnError();

    YCHECK(rows.empty());
}

void PipeInputToOutput(
    TInputStream* input,
    TOutputStream* output,
    i64 bufferBlockSize)
{
    struct TWriteBufferTag { };
    TBlob buffer(TWriteBufferTag(), bufferBlockSize);

    while (true) {
        size_t length = input->Read(buffer.Begin(), buffer.Size());
        if (length == 0)
            break;

        output->Write(buffer.Begin(), length);
    }

    output->Finish();
}

void PipeInputToOutput(
    NConcurrency::IAsyncInputStreamPtr input,
    TOutputStream* output,
    i64 bufferBlockSize)
{
    struct TWriteBufferTag { };
    auto buffer = TSharedMutableRef::Allocate<TWriteBufferTag>(bufferBlockSize);

    while (true) {
        auto length = WaitFor(input->Read(buffer))
            .ValueOrThrow();

        if (length == 0) {
            break;
        }

        output->Write(buffer.Begin(), length);
    }

    output->Finish();
}


//////////////////////////////////////////////////////////////////////////////////

// NB: not using TYsonString here to avoid copying.
TUnversionedValue MakeUnversionedValue(const TStringBuf& ysonString, int id, TStatelessLexer& lexer)
{
    TToken token;
    lexer.GetToken(ysonString, &token);
    YCHECK(!token.IsEmpty());

    switch (token.GetType()) {
        case ETokenType::Int64:
            return MakeUnversionedInt64Value(token.GetInt64Value(), id);

        case ETokenType::Uint64:
            return MakeUnversionedUint64Value(token.GetUint64Value(), id);

        case ETokenType::String:
            return MakeUnversionedStringValue(token.GetStringValue(), id);

        case ETokenType::Double:
            return MakeUnversionedDoubleValue(token.GetDoubleValue(), id);

        case ETokenType::Boolean:
            return MakeUnversionedBooleanValue(token.GetBooleanValue(), id);

        case ETokenType::Hash:
            return MakeUnversionedSentinelValue(EValueType::Null, id);

        default:
            return MakeUnversionedAnyValue(ysonString, id);
    }
}

//////////////////////////////////////////////////////////////////////////////////

int GetSystemColumnCount(TChunkReaderOptionsPtr options)
{
    int systemColumnCount = 0;
    if (options->EnableRowIndex) {
        ++systemColumnCount;
    }

    if (options->EnableRangeIndex) {
        ++systemColumnCount;
    }

    if (options->EnableTableIndex) {
        ++systemColumnCount;
    }

    return systemColumnCount;
}

void ValidateKeyColumns(const TKeyColumns& keyColumns, const TKeyColumns& chunkKeyColumns, bool requireUniqueKeys)
{
    if (requireUniqueKeys) {
        if (chunkKeyColumns.size() > keyColumns.size()) {
            THROW_ERROR_EXCEPTION("Chunk has more key columns than requested: actual %v, expected %v",
                chunkKeyColumns,
                keyColumns);
        }
    } else {
        if (chunkKeyColumns.size() < keyColumns.size()) {
            THROW_ERROR_EXCEPTION("Chunk has less key columns than requested: actual %v, expected %v",
                chunkKeyColumns,
                keyColumns);
        }
    }

    for (int i = 0; i < std::min(keyColumns.size(), chunkKeyColumns.size()); ++i) {
        if (chunkKeyColumns[i] != keyColumns[i]) {
            THROW_ERROR_EXCEPTION("Incompatible key columns: actual %v, expected %v",
                chunkKeyColumns,
                keyColumns);
        }
    }
}

TColumnFilter CreateColumnFilter(const TNullable<std::vector<Stroka>>& columns, TNameTablePtr nameTable)
{
    TColumnFilter columnFilter;
    if (!columns) {
        return columnFilter;
    }

    columnFilter.All = false;
    for (auto column : *columns) {
        auto id = nameTable->GetIdOrRegisterName(column);
        columnFilter.Indexes.push_back(id);
    }

    return columnFilter;
}

//////////////////////////////////////////////////////////////////////////////////

void TTableUploadOptions::Persist(NPhoenix::TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, UpdateMode);
    Persist(context, LockMode);
    Persist(context, TableSchema);
    Persist(context, SchemaMode);
}

//////////////////////////////////////////////////////////////////////////////////

void ValidateKeyColumnsEqual(const TKeyColumns& keyColumns, const TTableSchema& schema)
{
     if (keyColumns != schema.GetKeyColumns()) {
         THROW_ERROR_EXCEPTION("YPath attribute \"sorted_by\" must be compatible with table schema for a \"strong\" schema mode")
             << TErrorAttribute("key_columns", keyColumns)
             << TErrorAttribute("table_schema", schema);
     }
}

void ValidateAppendKeyColumns(const TKeyColumns& keyColumns, const TTableSchema& schema, i64 rowCount)
{
    ValidateKeyColumns(keyColumns);

    if (rowCount == 0) {
        return;
    }

    auto tableKeyColumns = schema.GetKeyColumns();
    bool areKeyColumnsCompatible = true;
    if (tableKeyColumns.size() < keyColumns.size()) {
        areKeyColumnsCompatible = false;
    } else {
        for (int i = 0; i < keyColumns.size(); ++i) {
            if (tableKeyColumns[i] != keyColumns[i]) {
                areKeyColumnsCompatible = false;
                break;
            }
        }
    }

    if (!areKeyColumnsCompatible) {
        THROW_ERROR_EXCEPTION("Key columns mismatch while trying to append sorted data into a non-empty table")
            << TErrorAttribute("append_key_columns", keyColumns)
            << TErrorAttribute("current_key_columns", tableKeyColumns);
    }
}

TTableUploadOptions GetTableUploadOptions(
    const TRichYPath& path,
    const TTableSchema& schema,
    ETableSchemaMode schemaMode,
    i64 rowCount)
{
    // Some ypath attributes are not compatible with attribute "schema".
    if (path.GetAppend() && path.GetSchema()) {
        THROW_ERROR_EXCEPTION("YPath attributes \"append\" and \"schema\" are not compatible")
            << TErrorAttribute("path", path);
    }

    if (!path.GetSortedBy().empty() && path.GetSchema()) {
        THROW_ERROR_EXCEPTION("YPath attributes \"sorted_by\" and \"schema\" are not compatible")
            << TErrorAttribute("path", path);
    }

    TTableUploadOptions result;
    if (path.GetAppend() && !path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Strong)) {
        ValidateKeyColumnsEqual(path.GetSortedBy(), schema);

        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Append;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = schema;
    } else if (path.GetAppend() && !path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Weak)) {
        // Old behaviour.
        ValidateAppendKeyColumns(path.GetSortedBy(), schema, rowCount);

        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Append;
        result.SchemaMode = ETableSchemaMode::Weak;
        result.TableSchema = TTableSchema::FromKeyColumns(path.GetSortedBy());
    } else if (path.GetAppend() && path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Strong)) {
        result.LockMode = schema.IsSorted() ? ELockMode::Exclusive : ELockMode::Shared;
        result.UpdateMode = EUpdateMode::Append;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = schema;
    } else if (path.GetAppend() && path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Weak)) {
        // Old behaviour - reset key columns if there were any.
        result.LockMode = ELockMode::Shared;
        result.UpdateMode = EUpdateMode::Append;
        result.SchemaMode = ETableSchemaMode::Weak;
        result.TableSchema = TTableSchema();
    } else if (!path.GetAppend() && !path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Strong)) {
        ValidateKeyColumnsEqual(path.GetSortedBy(), schema);

        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = schema;
    } else if (!path.GetAppend() && !path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Weak)) {
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Weak;
        result.TableSchema = TTableSchema::FromKeyColumns(path.GetSortedBy());
    } else if (!path.GetAppend() && path.GetSchema() && (schemaMode == ETableSchemaMode::Strong)) {
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = *path.GetSchema();
    } else if (!path.GetAppend() && path.GetSchema() && (schemaMode == ETableSchemaMode::Weak)) {
        // Change from Weak to Strong schema mode.
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = *path.GetSchema();
    } else if (!path.GetAppend() && path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Strong)) {
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Strong;
        result.TableSchema = schema;
    } else if (!path.GetAppend() && path.GetSortedBy().empty() && (schemaMode == ETableSchemaMode::Weak)) {
        result.LockMode = ELockMode::Exclusive;
        result.UpdateMode = EUpdateMode::Overwrite;
        result.SchemaMode = ETableSchemaMode::Weak;
        result.TableSchema = TTableSchema();
    } else {
        // Do not use Y_UNREACHABLE here, since this code is executed inside scheduler.
        THROW_ERROR_EXCEPTION("Failed to define upload parameters")
            << TErrorAttribute("path", path)
            << TErrorAttribute("schema_mode", schemaMode)
            << TErrorAttribute("schema", schema);
    }

    return result;
}

//////////////////////////////////////////////////////////////////////////////////

TUnversionedOwningRow YsonToSchemafulRow(
    const Stroka& yson,
    const TTableSchema& tableSchema,
    bool treatMissingAsNull)
{
    auto nameTable = TNameTable::FromSchema(tableSchema);

    auto rowParts = ConvertTo<yhash_map<Stroka, INodePtr>>(
        TYsonString(yson, EYsonType::MapFragment));

    TUnversionedOwningRowBuilder rowBuilder;
    auto addValue = [&] (int id, INodePtr value) {
        switch (value->GetType()) {
            case ENodeType::Int64:
                rowBuilder.AddValue(MakeUnversionedInt64Value(value->GetValue<i64>(), id));
                break;
            case ENodeType::Uint64:
                rowBuilder.AddValue(MakeUnversionedUint64Value(value->GetValue<ui64>(), id));
                break;
            case ENodeType::Double:
                rowBuilder.AddValue(MakeUnversionedDoubleValue(value->GetValue<double>(), id));
                break;
            case ENodeType::Boolean:
                rowBuilder.AddValue(MakeUnversionedBooleanValue(value->GetValue<bool>(), id));
                break;
            case ENodeType::String:
                rowBuilder.AddValue(MakeUnversionedStringValue(value->GetValue<Stroka>(), id));
                break;
            case ENodeType::Entity:
                rowBuilder.AddValue(MakeUnversionedSentinelValue(
                    value->Attributes().Get<EValueType>("type", EValueType::Null), id));
                break;
            default:
                rowBuilder.AddValue(MakeUnversionedAnyValue(ConvertToYsonString(value).GetData(), id));
                break;
        }
    };

    const auto& keyColumns = tableSchema.GetKeyColumns();

    // Key
    for (int id = 0; id < static_cast<int>(keyColumns.size()); ++id) {
        auto it = rowParts.find(nameTable->GetName(id));
        if (it == rowParts.end()) {
            rowBuilder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id));
        } else {
            addValue(id, it->second);
        }
    }

    // Fixed values
    for (int id = static_cast<int>(keyColumns.size()); id < static_cast<int>(tableSchema.Columns().size()); ++id) {
        auto it = rowParts.find(nameTable->GetName(id));
        if (it != rowParts.end()) {
            addValue(id, it->second);
        } else if (treatMissingAsNull) {
            rowBuilder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id));
        }
    }

    // Variable values
    for (const auto& pair : rowParts) {
        int id = nameTable->GetIdOrRegisterName(pair.first);
        if (id >= tableSchema.Columns().size()) {
            addValue(id, pair.second);
        }
    }

    return rowBuilder.FinishRow();
}

TUnversionedOwningRow YsonToSchemalessRow(const Stroka& valueYson)
{
    TUnversionedOwningRowBuilder builder;

    auto values = ConvertTo<std::vector<INodePtr>>(TYsonString(valueYson, EYsonType::ListFragment));
    for (const auto& value : values) {
        int id = value->Attributes().Get<int>("id");
        bool aggregate = value->Attributes().Find<bool>("aggregate").Get(false);
        switch (value->GetType()) {
            case ENodeType::Entity:
                builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id, aggregate));
                break;
            case ENodeType::Int64:
                builder.AddValue(MakeUnversionedInt64Value(value->GetValue<i64>(), id, aggregate));
                break;
            case ENodeType::Uint64:
                builder.AddValue(MakeUnversionedUint64Value(value->GetValue<ui64>(), id, aggregate));
                break;
            case ENodeType::Double:
                builder.AddValue(MakeUnversionedDoubleValue(value->GetValue<double>(), id, aggregate));
                break;
            case ENodeType::String:
                builder.AddValue(MakeUnversionedStringValue(value->GetValue<Stroka>(), id, aggregate));
                break;
            default:
                builder.AddValue(MakeUnversionedAnyValue(ConvertToYsonString(value).GetData(), id, aggregate));
                break;
        }
    }

    return builder.FinishRow();
}

TVersionedRow YsonToVersionedRow(
    const TRowBufferPtr& rowBuffer,
    const Stroka& keyYson,
    const Stroka& valueYson,
    const std::vector<TTimestamp>& deleteTimestamps)
{
    TVersionedRowBuilder builder(rowBuffer);

    auto keys = ConvertTo<std::vector<INodePtr>>(TYsonString(keyYson, EYsonType::ListFragment));

    int keyId = 0;
    for (auto key : keys) {
        switch (key->GetType()) {
            case ENodeType::Int64:
                builder.AddKey(MakeUnversionedInt64Value(key->GetValue<i64>(), keyId));
                break;
            case ENodeType::Uint64:
                builder.AddKey(MakeUnversionedUint64Value(key->GetValue<ui64>(), keyId));
                break;
            case ENodeType::Double:
                builder.AddKey(MakeUnversionedDoubleValue(key->GetValue<double>(), keyId));
                break;
            case ENodeType::String:
                builder.AddKey(MakeUnversionedStringValue(key->GetValue<Stroka>(), keyId));
                break;
            default:
                Y_UNREACHABLE();
                break;
        }
        ++keyId;
    }

    auto values = ConvertTo<std::vector<INodePtr>>(TYsonString(valueYson, EYsonType::ListFragment));
    for (auto value : values) {
        int id = value->Attributes().Get<int>("id");
        auto timestamp = value->Attributes().Get<TTimestamp>("ts");
        bool aggregate = value->Attributes().Find<bool>("aggregate").Get(false);
        switch (value->GetType()) {
            case ENodeType::Entity:
                builder.AddValue(MakeVersionedSentinelValue(EValueType::Null, timestamp, id, aggregate));
                break;
            case ENodeType::Int64:
                builder.AddValue(MakeVersionedInt64Value(value->GetValue<i64>(), timestamp, id, aggregate));
                break;
            case ENodeType::Uint64:
                builder.AddValue(MakeVersionedUint64Value(value->GetValue<ui64>(), timestamp, id, aggregate));
                break;
            case ENodeType::Double:
                builder.AddValue(MakeVersionedDoubleValue(value->GetValue<double>(), timestamp, id, aggregate));
                break;
            case ENodeType::String:
                builder.AddValue(MakeVersionedStringValue(value->GetValue<Stroka>(), timestamp, id, aggregate));
                break;
            default:
                builder.AddValue(MakeVersionedAnyValue(ConvertToYsonString(value).GetData(), timestamp, id, aggregate));
                break;
        }
    }

    for (auto timestamp : deleteTimestamps) {
        builder.AddDeleteTimestamp(timestamp);
    }

    return builder.FinishRow();
}

TUnversionedOwningRow YsonToKey(const Stroka& yson)
{
    TUnversionedOwningRowBuilder keyBuilder;
    auto keyParts = ConvertTo<std::vector<INodePtr>>(
        TYsonString(yson, EYsonType::ListFragment));

    for (int id = 0; id < keyParts.size(); ++id) {
        const auto& keyPart = keyParts[id];
        switch (keyPart->GetType()) {
            case ENodeType::Int64:
                keyBuilder.AddValue(MakeUnversionedInt64Value(
                    keyPart->GetValue<i64>(),
                    id));
                break;
            case ENodeType::Uint64:
                keyBuilder.AddValue(MakeUnversionedUint64Value(
                    keyPart->GetValue<ui64>(),
                    id));
                break;
            case ENodeType::Double:
                keyBuilder.AddValue(MakeUnversionedDoubleValue(
                    keyPart->GetValue<double>(),
                    id));
                break;
            case ENodeType::String:
                keyBuilder.AddValue(MakeUnversionedStringValue(
                    keyPart->GetValue<Stroka>(),
                    id));
                break;
            case ENodeType::Entity:
                keyBuilder.AddValue(MakeUnversionedSentinelValue(
                    keyPart->Attributes().Get<EValueType>("type", EValueType::Null),
                    id));
                break;
            default:
                keyBuilder.AddValue(MakeUnversionedAnyValue(
                    ConvertToYsonString(keyPart).GetData(),
                    id));
                break;
        }
    }

    return keyBuilder.FinishRow();
}

Stroka KeyToYson(TUnversionedRow row)
{
    return ConvertToYsonString(row, EYsonFormat::Text).GetData();
}

//////////////////////////////////////////////////////////////////////////////////

TOutputResult GetWrittenChunksBoundaryKeys(ISchemalessMultiChunkWriterPtr writer)
{
    TOutputResult result;

    const auto& chunks = writer->GetWrittenChunksMasterMeta();
    result.set_empty(chunks.empty());

    if (chunks.empty()) {
        return result;
    }

    result.set_sorted(writer->GetSchema().IsSorted());

    if (!writer->GetSchema().IsSorted()) {
        return result;
    }

    result.set_unique_keys(writer->GetSchema().GetUniqueKeys());

    auto frontBoundaryKeys = GetProtoExtension<TBoundaryKeysExt>(chunks.front().chunk_meta().extensions());
    result.set_min(frontBoundaryKeys.min());
    auto backBoundaryKeys = GetProtoExtension<TBoundaryKeysExt>(chunks.back().chunk_meta().extensions());
    result.set_max(backBoundaryKeys.max());

    return result;
}

//////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
} //// namespace NTableClient
