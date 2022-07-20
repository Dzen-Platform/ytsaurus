#pragma once

#include "chunk_meta_extensions.h"
#include "public.h"
#include "schemaless_block_reader.h"

#include <yt/yt/client/table_client/public.h>
#include <yt/yt/client/table_client/versioned_row.h>
#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/core/misc/bitmap.h>

#include <library/cpp/yt/memory/ref.h>

#include <library/cpp/yt/small_containers/compact_vector.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TSimpleVersionedBlockReader
{
public:
    TSimpleVersionedBlockReader(
        TSharedRef block,
        const NProto::TDataBlockMeta& meta,
        TTableSchemaPtr chunkSchema,
        int keyColumnCount,
        const std::vector<TColumnIdMapping>& schemaIdMapping,
        const TKeyComparer& keyComparer,
        TTimestamp timestamp,
        bool produceAllVersions,
        bool initialize);

    bool NextRow();

    bool SkipToRowIndex(i64 rowIndex);
    bool SkipToKey(TLegacyKey key);

    TLegacyKey GetKey() const;
    TMutableVersionedRow GetRow(TChunkedMemoryPool* memoryPool);

    i64 GetRowIndex() const;

private:
    const TSharedRef Block_;

    const TTimestamp Timestamp_;
    const bool ProduceAllVersions_;
    const int ChunkKeyColumnCount_;
    const int ChunkColumnCount_;
    const int KeyColumnCount_;

    const std::vector<TColumnIdMapping>& SchemaIdMapping_;

    const NProto::TDataBlockMeta& Meta_;
    const NProto::TSimpleVersionedBlockMeta& VersionedMeta_;

    const std::unique_ptr<bool[]> ColumnHunkFlags_;
    const std::unique_ptr<bool[]> ColumnAggregateFlags_;
    const std::unique_ptr<EValueType[]> ColumnTypes_;

    // NB: chunk reader holds the comparer.
    const TKeyComparer& KeyComparer_;

    TRef KeyData_;
    TReadOnlyBitmap KeyNullFlags_;

    TRef ValueData_;
    TReadOnlyBitmap ValueNullFlags_;
    std::optional<TReadOnlyBitmap> ValueAggregateFlags_;

    TRef TimestampsData_;

    TRef StringData_;

    bool Closed_ = false;

    i64 RowIndex_;

    const static size_t DefaultKeyBufferCapacity = 128;
    TCompactVector<char, DefaultKeyBufferCapacity> KeyBuffer_;
    TLegacyMutableKey Key_;

    const char* KeyDataPtr_;
    i64 TimestampOffset_;
    i64 ValueOffset_;
    ui16 WriteTimestampCount_;
    ui16 DeleteTimestampCount_;

    bool JumpToRowIndex(i64 index);
    TMutableVersionedRow ReadAllVersions(TChunkedMemoryPool* memoryPool);
    TMutableVersionedRow ReadOneVersion(TChunkedMemoryPool* memoryPool);

    TTimestamp ReadTimestamp(int timestampIndex);
    void ReadValue(TVersionedValue* value, int valueIndex, int id, int chunkSchemaId);
    void ReadKeyValue(TUnversionedValue* value, int id);

    Y_FORCE_INLINE TTimestamp ReadValueTimestamp(int valueIndex);
    Y_FORCE_INLINE void ReadStringLike(TUnversionedValue* value, const char* ptr);

    ui32 GetColumnValueCount(int schemaColumnId) const;
};

////////////////////////////////////////////////////////////////////////////////

class THorizontalSchemalessVersionedBlockReader
    : public THorizontalBlockReader
{
public:
    THorizontalSchemalessVersionedBlockReader(
        const TSharedRef& block,
        const NProto::TDataBlockMeta& meta,
        const std::vector<bool>& compositeColumnFlags,
        const std::vector<int>& chunkToReaderIdMapping,
        TRange<ESortOrder> sortOrders,
        int commonKeyPrefix,
        TTimestamp timestamp);

    TLegacyKey GetKey() const;
    TMutableVersionedRow GetRow(TChunkedMemoryPool* memoryPool);

private:
    TTimestamp Timestamp_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
