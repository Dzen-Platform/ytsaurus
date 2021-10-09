#pragma once

#include "chunk_meta_extensions.h"
#include "public.h"
#include "schemaless_block_reader.h"

#include <yt/yt/client/table_client/public.h>
#include <yt/yt/client/table_client/versioned_row.h>
#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/core/misc/bitmap.h>
#include <yt/yt/core/misc/ref.h>
#include <yt/yt/core/misc/small_vector.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct IVersionedBlockReader
{
    virtual ~IVersionedBlockReader() = default;

    virtual bool NextRow() = 0;

    virtual bool SkipToRowIndex(i64 rowIndex) = 0;
    virtual bool SkipToKey(TLegacyKey key) = 0;

    virtual TLegacyKey GetKey() const = 0;
    virtual TMutableVersionedRow GetRow(TChunkedMemoryPool* memoryPool) = 0;

    virtual i64 GetRowIndex() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TSimpleVersionedBlockReader
    : public IVersionedBlockReader
{
public:
    TSimpleVersionedBlockReader(
        TSharedRef block,
        const NProto::TBlockMeta& meta,
        TTableSchemaPtr chunkSchema,
        int chunkKeyColumnCount,
        int keyColumnCount,
        const std::vector<TColumnIdMapping>& schemaIdMapping,
        const TKeyComparer& keyComparer,
        TTimestamp timestamp,
        bool produceAllVersions,
        bool initialize);

    bool NextRow() override;

    bool SkipToRowIndex(i64 rowIndex) override;
    bool SkipToKey(TLegacyKey key) override;

    TLegacyKey GetKey() const override;
    TMutableVersionedRow GetRow(TChunkedMemoryPool* memoryPool) override;

    i64 GetRowIndex() const override;

private:
    const TSharedRef Block_;

    const TTimestamp Timestamp_;
    const bool ProduceAllVersions_;
    const int ChunkKeyColumnCount_;
    const int KeyColumnCount_;

    const std::vector<TColumnIdMapping>& SchemaIdMapping_;
    const TTableSchemaPtr ChunkSchema_;

    const NProto::TBlockMeta& Meta_;
    const NProto::TSimpleVersionedBlockMeta& VersionedMeta_;

    const std::unique_ptr<bool[]> ColumnHunkFlags_;
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

    const static size_t DefaultKeyBufferCapacity = 256;
    SmallVector<char, DefaultKeyBufferCapacity> KeyBuffer_;
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
    : public IVersionedBlockReader
{
public:
    THorizontalSchemalessVersionedBlockReader(
        const TSharedRef& block,
        const NProto::TBlockMeta& meta,
        const TTableSchemaPtr& schema,
        const std::vector<TColumnIdMapping>& idMapping,
        int chunkKeyColumnCount,
        int keyColumnCount,
        TTimestamp timestamp);

    bool NextRow() override;

    bool SkipToRowIndex(i64 rowIndex) override;
    bool SkipToKey(TLegacyKey key) override;

    TLegacyKey GetKey() const override;
    TMutableVersionedRow GetRow(TChunkedMemoryPool* memoryPool) override;

    i64 GetRowIndex() const override;

private:
    std::unique_ptr<THorizontalBlockReader> UnderlyingReader_;
    TTimestamp Timestamp_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
