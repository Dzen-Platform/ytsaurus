#pragma once

#include "public.h"

#include <yt/ytlib/table_client/chunk_meta.pb.h>

#include <yt/core/misc/blob_output.h>
#include <yt/core/misc/chunked_output_stream.h>
#include <yt/core/misc/ref.h>

#include <util/generic/bitmap.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TBlockWriter
{
public:
    struct TBlock
    {
        std::vector<TSharedRef> Data;
        NProto::TBlockMeta Meta;
    };

    TBlockWriter(const std::vector<int> columnSizes);

    void WriteInt64(const TUnversionedValue& value, int index);
    void WriteUint64(const TUnversionedValue& value, int index);
    void WriteDouble(const TUnversionedValue& value, int index);
    void WriteBoolean(const TUnversionedValue& value, int index);
    void WriteString(const TUnversionedValue& value, int index);
    void WriteAny(const TUnversionedValue& value, int index);

    // Stores string in a contiguous memory region.
    // Return TStingBuf containing stored string.
    TStringBuf WriteKeyString(const TUnversionedValue& value, int index);

    void WriteTimestamp(TTimestamp timestamp, bool deleted, int index);

    void WriteVariable(const TUnversionedValue& value, int index);

    void EndRow();

    void PushEndOfKey(bool endOfKey);

    i64 GetSize() const;
    i64 GetCapacity() const;
    i64 GetRowCount() const;

    TBlock FlushBlock();

private:
    struct TColumn
    {
        TChunkedOutputStream Stream;
        // Bit is set, if corresponding value is not null.
        TDynBitMap NullBitmap = TDynBitMap();
        int ValueSize = 0;
    };

    TDynBitMap EndOfKeyFlags;

    std::vector<TColumn> FixedColumns;
    TChunkedOutputStream VariableColumn;

    TChunkedOutputStream VariableBuffer;
    TChunkedOutputStream FixedBuffer;

    // In current row.
    ui32 VariableColumnCount;
    ui32 VariableOffset;
    i64 RowCount;

    ui32 RowSize;

    TBlobOutput IntermediateBuffer;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
