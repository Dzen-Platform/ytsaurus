#pragma once

#include "public.h"
#include "chunk_meta_extensions.h"
#include "schema.h"
#include "versioned_row.h"
#include "unversioned_row.h"

#include <yt/core/misc/bitmap.h>
#include <yt/core/misc/ref.h>
#include <yt/core/misc/small_vector.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TSimpleVersionedBlockReader
{
public:
    TSimpleVersionedBlockReader(
        const TSharedRef& block,
        const NProto::TBlockMeta& meta,
        const TTableSchema& chunkSchema,
        int chunkKeyColumnCount,
        int keyColumnCount,
        const std::vector<TColumnIdMapping>& schemaIdMapping,
        const TKeyComparer& keyComparer,
        TTimestamp timestamp);

    bool NextRow();

    bool SkipToRowIndex(i64 rowIndex);
    bool SkipToKey(TKey key);

    TKey GetKey() const;
    TVersionedRow GetRow(TChunkedMemoryPool* memoryPool);

    i64 GetRowIndex() const;

    static const ETableChunkFormat FormatVersion = ETableChunkFormat::VersionedSimple;

private:
    TSharedRef Block_;
    typedef TReadOnlyBitmap<ui64> TBitmap;

    TTimestamp Timestamp_;
    const int ChunkKeyColumnCount_;
    const int KeyColumnCount_;

    const std::vector<TColumnIdMapping>& SchemaIdMapping_;
    const TTableSchema& ChunkSchema_;

    const NProto::TBlockMeta& Meta_;
    const NProto::TSimpleVersionedBlockMeta& VersionedMeta_;

    TRef KeyData_;
    TBitmap KeyNullFlags_;

    TRef ValueData_;
    TBitmap ValueNullFlags_;

    TRef TimestampsData_;

    TRef StringData_;

    bool Closed_ = false;

    i64 RowIndex_;

    const static size_t DefaultKeyBufferCapacity = 256;
    SmallVector<char, DefaultKeyBufferCapacity> KeyBuffer_;
    TKey Key_;

    const char* KeyDataPtr_;
    i64 TimestampOffset_;
    i64 ValueOffset_;
    ui16 WriteTimestampCount_;
    ui16 DeleteTimestampCount_;

    // NB: chunk reader holds the comparer.
    const TKeyComparer& KeyComparer_;

    bool JumpToRowIndex(i64 index);
    TVersionedRow ReadAllValues(TChunkedMemoryPool* memoryPool);
    TVersionedRow ReadValuesByTimestamp(TChunkedMemoryPool* memoryPool);

    TTimestamp ReadTimestamp(int timestampIndex);
    void ReadValue(TVersionedValue* value, int valueIndex, int id, int chunkSchemaId);
    TTimestamp ReadValueTimestamp(int valueIndex, int id);
    void ReadKeyValue(TUnversionedValue* value, int id);

    void ReadInt64(TUnversionedValue* value, const char* ptr);
    void ReadUint64(TUnversionedValue* value, const char* ptr);
    void ReadDouble(TUnversionedValue* value, const char* ptr);
    void ReadBoolean(TUnversionedValue* value, const char* ptr);
    void ReadStringLike(TUnversionedValue* value, const char* ptr);

    ui32 GetColumnValueCount(int schemaColumnId) const;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
