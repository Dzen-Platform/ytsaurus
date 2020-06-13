#include "null_column_reader.h"

#include "column_reader_detail.h"
#include "helpers.h"

#include <yt/ytlib/chunk_client/public.h>

#include <yt/client/table_client/unversioned_row_batch.h>
#include <yt/client/table_client/logical_type.h>

namespace NYT::NTableChunkFormat {

using namespace NProto;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

class TNullValueExtractor
{
public:
    TNullValueExtractor(TRef /*data*/, const TSegmentMeta& /*meta*/)
    { }

    void ExtractValue(TUnversionedValue* value, i64 valueIndex, int id, bool aggregate) const
    {
        *value = MakeUnversionedNullValue(id, aggregate);
    }

    int GetBatchColumnCount()
    {
        return 1;
    }

    void ReadColumnarBatch(
        i64 /*startRowIndex*/,
        i64 /*rowCount*/,
        TMutableRange<NTableClient::IUnversionedRowBatch::TColumn> /*columns*/)
    { }
};

////////////////////////////////////////////////////////////////////////////////

class TUnversionedNullColumnReader
    : public TUnversionedColumnReaderBase
{
public:
    TUnversionedNullColumnReader(const TColumnMeta& columnMeta, int columnIndex, int columnId)
        : TUnversionedColumnReaderBase(columnMeta, columnIndex, columnId)
    { }

    virtual std::pair<i64, i64> GetEqualRange(
        const TUnversionedValue& value,
        i64 lowerRowIndex,
        i64 upperRowIndex) override
    {
        return DoGetEqualRange<EValueType::Null>(
            value,
            lowerRowIndex,
            upperRowIndex);
    }

    virtual i64 EstimateDataWeight(i64 /*lowerRowIndex*/, i64 /*upperRowIndex*/) override
    {
        return 0;
    }

private:
    virtual std::unique_ptr<IUnversionedSegmentReader> CreateSegmentReader(int segmentIndex, bool /* scan */) override
    {
        using TSegmentReader = TDenseUnversionedSegmentReader<
            EValueType::Null,
            TNullValueExtractor>;

        const auto& meta = ColumnMeta_.segments(segmentIndex);
        return DoCreateSegmentReader<TSegmentReader>(meta);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TBlocklessUnversionedNullColumnReader
    : public IUnversionedColumnReader
{
public:
    TBlocklessUnversionedNullColumnReader(int columnIndex, int id)
        : ColumnIndex_(columnIndex)
        , ColumnId_(id)
    { }

    virtual void SetCurrentBlock(TSharedRef block, int blockIndex) override
    {
        YT_ABORT();
    }

    virtual void Rearm() override
    { }

    virtual void SkipToRowIndex(i64 rowIndex) override
    {
        RowIndex_ = rowIndex;
    }

    virtual i64 GetCurrentRowIndex() const override
    {
        return RowIndex_;
    }

    virtual i64 GetBlockUpperRowIndex() const override
    {
        return std::numeric_limits<i64>::max();
    }

    virtual i64 GetReadyUpperRowIndex() const override
    {
        return GetBlockUpperRowIndex();
    }

    virtual int GetCurrentBlockIndex() const override
    {
        YT_ABORT();
    }

    virtual std::optional<int> GetNextBlockIndex() const override
    {
        return std::nullopt;
    }

    virtual std::pair<i64, i64> GetEqualRange(const TUnversionedValue& value, i64 lowerRowIndex, i64 upperRowIndex) override
    {
        if (value.Type < EValueType::Null) {
            return std::make_pair(lowerRowIndex, lowerRowIndex);
        } else if (value.Type == EValueType::Null) {
            return std::make_pair(lowerRowIndex, upperRowIndex);
        } else {
            return std::make_pair(upperRowIndex, upperRowIndex);
        }
    }

    virtual void ReadValues(TMutableRange<NTableClient::TMutableVersionedRow> rows) override
    {
        DoReadValues(rows);
    }

    virtual void ReadValues(TMutableRange<NTableClient::TMutableUnversionedRow> rows) override
    {
        DoReadValues(rows);
    }

    virtual int GetBatchColumnCount() override
    {
        return 1;
    }

    virtual void ReadColumnarBatch(
        TMutableRange<NTableClient::IUnversionedRowBatch::TColumn> columns,
        i64 /*rowCount*/) override
    {
        YT_VERIFY(columns.size() == 1);
        auto& column = columns[0];
        column.Type = SimpleLogicalType(ESimpleLogicalValueType::Null);
    }

    virtual i64 EstimateDataWeight(i64 /*lowerRowIndex*/, i64 /*upperRowIndex*/) override
    {
        return 0;
    }

private:
    const int ColumnIndex_;
    const int ColumnId_;

    i64 RowIndex_ = 0;

    template <class TRow>
    void DoReadValues(TMutableRange<TRow> rows)
    {
        for (auto row : rows) {
            if (row) {
                GetUnversionedValue(row, ColumnIndex_) = MakeUnversionedSentinelValue(EValueType::Null, ColumnId_);
            }
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IUnversionedColumnReader> CreateUnversionedNullColumnReader(
    const TColumnMeta& columnMeta,
    int columnIndex,
    int columnId)
{
    return std::make_unique<TUnversionedNullColumnReader>(columnMeta, columnIndex, columnId);
}

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IUnversionedColumnReader> CreateBlocklessUnversionedNullColumnReader(
    int columnIndex,
    int columnId)
{
    return std::make_unique<TBlocklessUnversionedNullColumnReader>(columnIndex, columnId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableChunkFormat
