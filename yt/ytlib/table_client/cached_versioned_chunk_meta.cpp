#include "stdafx.h"
#include "cached_versioned_chunk_meta.h"
#include "schema.h"

#include <ytlib/chunk_client/chunk_reader.h>
#include <ytlib/chunk_client/dispatcher.h>

#include <core/concurrency/scheduler.h>

#include <core/misc/bloom_filter.h>

namespace NYT {
namespace NTableClient {

using namespace NConcurrency;
using namespace NTableClient::NProto;
using namespace NChunkClient;
using namespace NChunkClient::NProto;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

TFuture<TCachedVersionedChunkMetaPtr> TCachedVersionedChunkMeta::Load(
    IChunkReaderPtr chunkReader,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns)
{
    auto cachedMeta = New<TCachedVersionedChunkMeta>();
    return BIND(&TCachedVersionedChunkMeta::DoLoad, cachedMeta)
        .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
        .Run(chunkReader, schema, keyColumns);
}

int TCachedVersionedChunkMeta::GetKeyColumnCount() const
{
    return KeyColumns_.size();
}

TCachedVersionedChunkMetaPtr TCachedVersionedChunkMeta::DoLoad(
    IChunkReaderPtr chunkReader,
    const TTableSchema& readerSchema,
    const TKeyColumns& keyColumns)
{
    ChunkId_ = chunkReader->GetChunkId();
    KeyColumns_ = keyColumns;

    try {
        ValidateTableSchemaAndKeyColumns(readerSchema, keyColumns);

        auto asyncChunkMeta = chunkReader->GetMeta();
        ChunkMeta_ = WaitFor(asyncChunkMeta)
            .ValueOrThrow();

        ValidateChunkMeta();
        ValidateSchema(readerSchema);

        auto boundaryKeysExt = GetProtoExtension<TBoundaryKeysExt>(ChunkMeta_.extensions());
        MinKey_ = FromProto<TOwningKey>(boundaryKeysExt.min());
        MaxKey_ = FromProto<TOwningKey>(boundaryKeysExt.max());

        Misc_ = GetProtoExtension<TMiscExt>(ChunkMeta_.extensions());
        BlockMeta_ = GetProtoExtension<TBlockMetaExt>(ChunkMeta_.extensions());

        BlockIndexKeys_.reserve(BlockMeta_.blocks_size());
        for (const auto& block : BlockMeta_.blocks()) {
            YCHECK(block.has_last_key());
            auto key = FromProto<TOwningKey>(block.last_key());
            BlockIndexKeys_.push_back(WidenKey(key, GetKeyColumnCount()));
        }

        return this;
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error caching meta of chunk %v",
            ChunkId_)
            << ex;
    }
}

void TCachedVersionedChunkMeta::ValidateChunkMeta()
{
    auto type = EChunkType(ChunkMeta_.type());
    if (type != EChunkType::Table) {
        THROW_ERROR_EXCEPTION("Incorrect chunk type: actual %Qlv, expected %Qlv",
            type,
            EChunkType::Table);
    }

    auto formatVersion = ETableChunkFormat(ChunkMeta_.version());
    if (formatVersion != ETableChunkFormat::VersionedSimple) {
        THROW_ERROR_EXCEPTION("Incorrect chunk format version: actual %Qlv, expected: %Qlv",
            formatVersion,
            ETableChunkFormat::VersionedSimple);
    }
}

void TCachedVersionedChunkMeta::ValidateKeyColumns(const TKeyColumns& chunkKeyColumns)
{
    if (KeyColumns_.size() < chunkKeyColumns.size()) {
        THROW_ERROR_EXCEPTION("Key column count is less than expected: chunk key columns [%v], reader key columns [%v]",
            JoinToString(chunkKeyColumns),
            JoinToString(KeyColumns_));
    }

    for (int i = 0; i < chunkKeyColumns.size(); ++i) {
        if (KeyColumns_[i] != chunkKeyColumns[i]) {
            THROW_ERROR_EXCEPTION("Incompatible key columns: chunk key columns [%v], reader key colums [%v]",
                JoinToString(chunkKeyColumns),
                JoinToString(KeyColumns_));
        }
    }

    for (int i = chunkKeyColumns.size(); i < KeyColumns_.size(); ++i) {
        if (ChunkSchema_.FindColumn(KeyColumns_[i])) {
            THROW_ERROR_EXCEPTION("Incompatible wider key columns: %Qv is a non-key column",
                KeyColumns_[i]);
        }
    }

    ChunkKeyColumnCount_ = chunkKeyColumns.size();
}

void TCachedVersionedChunkMeta::ValidateSchema(const TTableSchema& readerSchema)
{
    auto protoSchema = GetProtoExtension<TTableSchemaExt>(ChunkMeta_.extensions());
    FromProto(&ChunkSchema_, protoSchema);

    auto chunkKeyColumnsExt = GetProtoExtension<TKeyColumnsExt>(ChunkMeta_.extensions());
    auto chunkKeyColumns = NYT::FromProto<TKeyColumns>(chunkKeyColumnsExt);

    ValidateKeyColumns(chunkKeyColumns);

    SchemaIdMapping_.reserve(readerSchema.Columns().size() - KeyColumns_.size());
    for (int readerIndex = KeyColumns_.size(); readerIndex < readerSchema.Columns().size(); ++readerIndex) {
        auto& column = readerSchema.Columns()[readerIndex];
        auto* chunkColumn = ChunkSchema_.FindColumn(column.Name);
        if (!chunkColumn) {
            // This is a valid case, simply skip the column.
            continue;
        }

        if (chunkColumn->Type != column.Type) {
            THROW_ERROR_EXCEPTION("Incompatible type for column %Qv: actual: %Qlv, expected %Qlv",
                column.Name,
                chunkColumn->Type,
                column.Type);
        }

        TColumnIdMapping mapping;
        mapping.ChunkSchemaIndex = ChunkSchema_.GetColumnIndex(*chunkColumn);
        mapping.ReaderSchemaIndex = readerIndex;
        SchemaIdMapping_.push_back(mapping);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
