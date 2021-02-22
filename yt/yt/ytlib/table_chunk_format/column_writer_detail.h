#pragma once

#include "column_writer.h"
#include "private.h"

#include <yt/core/misc/bitmap.h>
#include <yt/core/misc/ref.h>

namespace NYT::NTableChunkFormat {

////////////////////////////////////////////////////////////////////////////////

class TColumnWriterBase
    : public IValueColumnWriter
{
public:
    TColumnWriterBase(TDataBlockWriter* blockWriter);

    virtual void FinishBlock(int blockIndex) override;

    virtual const NProto::TColumnMeta& ColumnMeta() const override;

    virtual i64 GetMetaSize() const override;

protected:
    TDataBlockWriter* BlockWriter_;

    i64 RowCount_ = 0;

    i64 MetaSize_ = 0;
    NProto::TColumnMeta ColumnMeta_;
    std::vector<NProto::TSegmentMeta> CurrentBlockSegments_;

    void DumpSegment(TSegmentInfo* segmentInfo);
};

////////////////////////////////////////////////////////////////////////////////

class TVersionedColumnWriterBase
    : public TColumnWriterBase
{
public:
    TVersionedColumnWriterBase(int columnId, bool aggregate, TDataBlockWriter* blockWriter);

    virtual i32 GetCurrentSegmentSize() const override;

    virtual void WriteUnversionedValues(TRange<NTableClient::TUnversionedRow> rows) override;

protected:
    const int ColumnId_;
    const bool Aggregate_;

    i64 EmptyPendingRowCount_ = 0;

    std::vector<ui32> TimestampIndexes_;
    TBitmapOutput NullBitmap_;
    TBitmapOutput AggregateBitmap_;

    std::vector<ui32> ValuesPerRow_;

    TTimestampIndex MaxTimestampIndex_;


    void Reset();

    void AddPendingValues(
        TRange<NTableClient::TVersionedRow> rows,
        std::function<void (const NTableClient::TVersionedValue& value)> onValue);

    void DumpVersionedData(TSegmentInfo* segmentInfo);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableChunkFormat
