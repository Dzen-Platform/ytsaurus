#pragma once

#include "public.h"
#include "unversioned_row.h"

#include <yt/core/misc/chunked_memory_pool.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct TDefaultRowBufferPoolTag { };

//! Holds data for a bunch of rows.
/*!
 *  Acts as a ref-counted wrapped around TChunkedMemoryPool plus a bunch
 *  of helpers.
 */
class TRowBuffer
    : public TIntrinsicRefCounted
{
public:
    explicit TRowBuffer(
        i64 chunkSize = TChunkedMemoryPool::DefaultChunkSize,
        double maxSmallBlockRatio = TChunkedMemoryPool::DefaultMaxSmallBlockSizeRatio,
        TRefCountedTypeCookie tagCookie = GetRefCountedTypeCookie<TDefaultRowBufferPoolTag>());

    template <class TTag>
    explicit TRowBuffer(
        TTag,
        i64 chunkSize = TChunkedMemoryPool::DefaultChunkSize,
        double maxSmallBlockRatio = TChunkedMemoryPool::DefaultMaxSmallBlockSizeRatio)
        : TRowBuffer(chunkSize, maxSmallBlockRatio, GetRefCountedTypeCookie<TTag>())
    { }

    TChunkedMemoryPool* GetPool();

    void Capture(TUnversionedValue* value);
    TVersionedValue Capture(const TVersionedValue& value);
    TUnversionedValue Capture(const TUnversionedValue& value);

    TMutableUnversionedRow Capture(TUnversionedRow row, bool deep = true);
    TMutableUnversionedRow Capture(const TUnversionedValue* begin, int count, bool deep = true);
    std::vector<TMutableUnversionedRow> Capture(const std::vector<TUnversionedRow>& rows, bool deep = true);

    //! Captures the row applying #idMapping to value ids and placing values to the proper positions.
    //! The returned row is schemaful.
    //! Skips values that map to negative ids with via #idMapping.
    TMutableUnversionedRow CaptureAndPermuteRow(
        TUnversionedRow row,
        const TTableSchema& tableSchema,
        const TNameTableToSchemaIdMapping& idMapping);

    i64 GetSize() const;
    i64 GetCapacity() const;

    void Clear();

private:
    TChunkedMemoryPool Pool_;

};

DEFINE_REFCOUNTED_TYPE(TRowBuffer)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
