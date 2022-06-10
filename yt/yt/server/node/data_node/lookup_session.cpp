#include "lookup_session.h"

#include "bootstrap.h"
#include "chunk.h"
#include "chunk_registry.h"
#include "local_chunk_reader.h"
#include "private.h"
#include "table_schema_cache.h"
#include "chunk_meta_manager.h"

#include <yt/yt/server/node/tablet_node/versioned_chunk_meta_manager.h>

#include <yt/yt/client/misc/workload.h>

#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/client/table_client/config.h>
#include <yt/yt/client/table_client/row_buffer.h>

#include <yt/yt/ytlib/table_client/chunk_state.h>
#include <yt/yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/table_client/versioned_chunk_reader.h>

#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/proto/data_node_service.pb.h>

namespace NYT::NDataNode {

using namespace NYT::NChunkClient;
using namespace NYT::NChunkClient::NProto;
using namespace NYT::NConcurrency;
using namespace NTableClient;
using namespace NTabletNode;
using namespace NObjectClient;
using namespace NClusterNode;
using namespace NHydra;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TLookupSession::TLookupSession(
    IBootstrap* bootstrap,
    IChunkPtr chunk,
    TReadSessionId readSessionId,
    TWorkloadDescriptor workloadDescriptor,
    TColumnFilter columnFilter,
    TTimestamp timestamp,
    bool produceAllVersions,
    TTableSchemaPtr tableSchema,
    const std::vector<TSharedRef>& serializedKeys,
    NCompression::ECodec codecId,
    TTimestamp overrideTimestamp,
    bool populateCache)
    : Bootstrap_(bootstrap)
    , Chunk_(std::move(chunk))
    , ChunkId_(Chunk_->GetId())
    , ReadSessionId_(readSessionId)
    , ColumnFilter_(std::move(columnFilter))
    , Timestamp_(timestamp)
    , ProduceAllVersions_(produceAllVersions)
    , TableSchema_(std::move(tableSchema))
    , Codec_(NCompression::GetCodec(codecId))
    , OverrideTimestamp_(overrideTimestamp)
{
    Options_.ChunkReaderStatistics = ChunkReaderStatistics_;
    Options_.ReadSessionId = ReadSessionId_;
    Options_.WorkloadDescriptor = std::move(workloadDescriptor);
    Options_.PopulateCache = populateCache;

    // May be slow because of chunk meta cache misses.
    YT_ASSERT(CheckKeyColumnCompatibility());

    // NB: TableSchema is assumed to be fetched upon calling LookupSession.
    YT_VERIFY(TableSchema_);
    if (!TableSchema_->GetUniqueKeys()) {
        THROW_ERROR_EXCEPTION("Table schema for chunk %v must have unique keys", ChunkId_)
            << TErrorAttribute("read_session_id", ReadSessionId_);
    }
    if (!TableSchema_->GetStrict()) {
        THROW_ERROR_EXCEPTION("Table schema for chunk %v must be strict", ChunkId_)
            << TErrorAttribute("read_session_id", ReadSessionId_);
    }

    // Use cache for readers?
    UnderlyingChunkReader_ = CreateLocalChunkReader(
        New<TReplicationReaderConfig>(),
        Chunk_,
        Bootstrap_->GetBlockCache(),
        Bootstrap_->GetChunkMetaManager()->GetBlockMetaCache());

    auto keysReader = CreateWireProtocolReader(
        MergeRefsToRef<TKeyReaderBufferTag>(serializedKeys),
        KeyReaderRowBuffer_);
    RequestedKeys_ = keysReader->ReadUnversionedRowset(true);
    YT_VERIFY(!RequestedKeys_.Empty());

    YT_LOG_DEBUG("Local chunk reader is created for lookup request (ChunkId: %v, ReadSessionId: %v, KeyCount: %v)",
        ChunkId_,
        ReadSessionId_,
        RequestedKeys_.Size());
}

TFuture<TSharedRef> TLookupSession::Run()
{
    NProfiling::TWallTimer metaWaitTimer;
    const auto& chunkMetaManager = Bootstrap_->GetVersionedChunkMetaManager();

    return
        chunkMetaManager->GetMeta(UnderlyingChunkReader_, TableSchema_, Options_)
            .Apply(BIND(
                [=, this_ = MakeStrong(this), metaWaitTimer = std::move(metaWaitTimer)]
                (const TVersionedChunkMetaCacheEntryPtr& entry)
            {
                Options_.ChunkReaderStatistics->MetaWaitTime.fetch_add(
                    metaWaitTimer.GetElapsedValue(),
                    std::memory_order_relaxed);
                return DoRun(entry->Meta());
            })
            .AsyncVia(Bootstrap_->GetStorageLookupInvoker()));
}

const TChunkReaderStatisticsPtr& TLookupSession::GetChunkReaderStatistics()
{
    return ChunkReaderStatistics_;
}

std::tuple<TTableSchemaPtr, bool> TLookupSession::FindTableSchema(
    TChunkId chunkId,
    TReadSessionId readSessionId,
    const TReqLookupRows::TTableSchemaData& schemaData,
    const TTableSchemaCachePtr& tableSchemaCache)
{
    auto tableId = FromProto<TObjectId>(schemaData.table_id());
    auto revision = schemaData.revision();
    i64 schemaSize = schemaData.has_schema_size() ? schemaData.schema_size() : 1_MB;

    auto tableSchemaWrapper = tableSchemaCache->GetOrCreate(TSchemaCacheKey{tableId, revision}, schemaSize);
    YT_VERIFY(tableSchemaWrapper);
    if (tableSchemaWrapper->IsSet()) {
        return {tableSchemaWrapper->GetValue(), false};
    }

    if (!schemaData.has_schema()) {
        bool isSchemaRequested = tableSchemaWrapper->TryRequestSchema();

        YT_LOG_DEBUG("Schema for lookup request is missing"
            "(ChunkId: %v, ReadSessionId: %v, TableId: %v, Revision: %llx, SchemaSize: %v, IsSchemaRequested: %v)",
            chunkId,
            readSessionId,
            tableId,
            revision,
            schemaSize,
            isSchemaRequested);

        return {nullptr, isSchemaRequested};
    }

    auto tableSchema = FromProto<TTableSchemaPtr>(schemaData.schema());
    tableSchemaWrapper->SetValue(tableSchema);

    YT_LOG_DEBUG("Inserted schema to schema cache for lookup request"
        "(ChunkId: %v, ReadSessionId: %v, TableId: %v, Revision: %llx, SchemaSize: %v)",
        chunkId,
        readSessionId,
        tableId,
        revision,
        schemaSize);

    return {tableSchema, false};
}

bool TLookupSession::CheckKeyColumnCompatibility()
{
    auto chunkMeta = WaitFor(Chunk_->ReadMeta(Options_))
        .ValueOrThrow();
    auto type = CheckedEnumCast<EChunkType>(chunkMeta->type());
    if (type != EChunkType::Table) {
        THROW_ERROR_EXCEPTION("Chunk %v is of invalid type", ChunkId_)
            << TErrorAttribute("read_session_id", ReadSessionId_)
            << TErrorAttribute("expected_chunk_type", EChunkType::Table)
            << TErrorAttribute("chunk_type", type);
    }

    const auto& tableKeyColumns = TableSchema_->GetKeyColumns();
    for (auto key : RequestedKeys_) {
        YT_VERIFY(key.GetCount() == tableKeyColumns.size());
    }

    TKeyColumns chunkKeyColumns;
    auto optionalKeyColumnsExt = FindProtoExtension<NTableClient::NProto::TKeyColumnsExt>(chunkMeta->extensions());
    // COMPAT(akozhikhov)
    if (optionalKeyColumnsExt) {
        chunkKeyColumns = FromProto<TKeyColumns>(*optionalKeyColumnsExt);
    } else {
        const auto& schemaExt = GetProtoExtension<NTableClient::NProto::TTableSchemaExt>(chunkMeta->extensions());
        chunkKeyColumns = FromProto<TTableSchema>(schemaExt).GetKeyColumns();
    }

    bool isCompatibleKeyColumns =
        tableKeyColumns.size() >= chunkKeyColumns.size() &&
        std::equal(
            chunkKeyColumns.begin(),
            chunkKeyColumns.end(),
            tableKeyColumns.begin());
    if (!isCompatibleKeyColumns) {
        THROW_ERROR_EXCEPTION("Chunk %v has incompatible key columns", ChunkId_)
            << TErrorAttribute("read_session_id", ReadSessionId_)
            << TErrorAttribute("table_key_columns", tableKeyColumns)
            << TErrorAttribute("chunk_key_columns", chunkKeyColumns);
    }

    return true;
}

TSharedRef TLookupSession::DoRun(TCachedVersionedChunkMetaPtr chunkMeta)
{
    TChunkSpec chunkSpec;
    ToProto(chunkSpec.mutable_chunk_id(), ChunkId_);

    auto chunkState = New<TChunkState>(
        Bootstrap_->GetBlockCache(),
        std::move(chunkSpec),
        chunkMeta,
        OverrideTimestamp_,
        /*lookupHashTable*/ nullptr,
        New<TChunkReaderPerformanceCounters>(),
        TKeyComparer(Bootstrap_->GetRowComparerProvider()->Get(TableSchema_->GetKeyColumnTypes()).UUComparer),
        /*virtualValueDirectory*/ nullptr,
        TableSchema_);

    auto writer = CreateWireProtocolWriter();
    auto onRow = [&] (TVersionedRow row) {
        writer->WriteVersionedRow(row);
    };

    auto rowReaderAdapter = New<TRowReaderAdapter>(
        TChunkReaderConfig::GetDefault(),
        UnderlyingChunkReader_,
        chunkState,
        chunkMeta,
        Options_,
        RequestedKeys_,
        ColumnFilter_,
        Timestamp_,
        ProduceAllVersions_);
    rowReaderAdapter->ReadRowset(onRow);

    // TODO(akozhikhov): update compression statistics.
    return Codec_->Compress(writer->Finish());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
