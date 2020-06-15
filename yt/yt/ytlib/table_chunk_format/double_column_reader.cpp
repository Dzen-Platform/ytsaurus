#include "double_column_reader.h"

#include "column_reader_detail.h"
#include "helpers.h"

#include <yt/client/table_client/unversioned_row_batch.h>
#include <yt/client/table_client/logical_type.h>

#include <yt/core/misc/bitmap.h>

namespace NYT::NTableChunkFormat {

using namespace NTableClient;
using namespace NProto;

////////////////////////////////////////////////////////////////////////////////

class TDoubleValueExtractorBase
{
public:
    void ExtractValue(TUnversionedValue* value, i64 valueIndex, int id, bool aggregate) const
    {
        if (NullBitmap_[valueIndex]) {
            *value = MakeUnversionedSentinelValue(EValueType::Null, id, aggregate);
        } else {
            *value = MakeUnversionedDoubleValue(Values_[valueIndex], id, aggregate);
        }
    }

protected:
    TRange<double> Values_;
    TReadOnlyBitmap<ui64> NullBitmap_;

    const char* InitValueReader(const char* ptr)
    {
        ui64 valueCount = *reinterpret_cast<const ui64*>(ptr);
        ptr += sizeof(ui64);

        Values_ = MakeRange(reinterpret_cast<const double*>(ptr), valueCount);
        ptr += sizeof(double) * valueCount;

        NullBitmap_ = TReadOnlyBitmap<ui64>(reinterpret_cast<const ui64*>(Values_.end()), valueCount);
        ptr += NullBitmap_.GetByteSize();

        return ptr;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TDirectDenseVersionedDoubleValueExtractor
    : public TDenseVersionedValueExtractorBase
    , public TDoubleValueExtractorBase
{
public:
    TDirectDenseVersionedDoubleValueExtractor(TRef data, const NProto::TSegmentMeta& meta, bool aggregate)
        : TDenseVersionedValueExtractorBase(meta, aggregate)
    {
        const char* ptr = data.Begin();
        ptr = InitDenseReader(ptr);
        ptr = InitValueReader(ptr);
        YT_VERIFY(ptr == data.End());
    }
};

////////////////////////////////////////////////////////////////////////////////

class TDirectSparseVersionedDoubleValueExtractor
    : public TSparseVersionedValueExtractorBase
    , public TDoubleValueExtractorBase
{
public:
    TDirectSparseVersionedDoubleValueExtractor(TRef data, const NProto::TSegmentMeta& meta, bool aggregate)
        : TSparseVersionedValueExtractorBase(meta, aggregate)
    {
        const char* ptr = data.Begin();
        ptr = InitSparseReader(ptr);
        ptr = InitValueReader(ptr);
        YT_VERIFY(ptr == data.End());
    }
};

////////////////////////////////////////////////////////////////////////////////

class TVersionedDoubleColumnReader
    : public TVersionedColumnReaderBase
{
public:
    TVersionedDoubleColumnReader(const TColumnMeta& columnMeta, int columnId, bool aggregate)
        : TVersionedColumnReaderBase(columnMeta, columnId, aggregate)
    { }

private:
    virtual std::unique_ptr<IVersionedSegmentReader> CreateSegmentReader(int segmentIndex) override
    {
        using TDirectDenseReader = TDenseVersionedSegmentReader<TDirectDenseVersionedDoubleValueExtractor>;
        using TDirectSparseReader = TSparseVersionedSegmentReader<TDirectSparseVersionedDoubleValueExtractor>;

        const auto& meta = ColumnMeta_.segments(segmentIndex);
        auto dense = meta.HasExtension(TDenseVersionedSegmentMeta::dense_versioned_segment_meta);

        if (dense) {
            return DoCreateSegmentReader<TDirectDenseReader>(meta);
        } else {
            return DoCreateSegmentReader<TDirectSparseReader>(meta);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IVersionedColumnReader> CreateVersionedDoubleColumnReader(
    const TColumnMeta& columnMeta,
    int columnId,
    bool aggregate)
{
    return std::make_unique<TVersionedDoubleColumnReader>(
        columnMeta,
        columnId,
        aggregate);
}

////////////////////////////////////////////////////////////////////////////////

class TUnversionedDoubleValueExtractor
    : public TDoubleValueExtractorBase
{
public:
    TUnversionedDoubleValueExtractor(TRef data, const TSegmentMeta& meta)
    {
        const char* ptr = data.Begin();
        ptr = InitValueReader(ptr);
        YT_VERIFY(ptr == data.End());
    }

    int GetBatchColumnCount()
    {
        return 1;
    }

    void ReadColumnarBatch(
        i64 startRowIndex,
        i64 rowCount,
        TMutableRange<NTableClient::IUnversionedColumnarRowBatch::TColumn> columns)
    {
        YT_VERIFY(columns.size() == 1);
        auto& column = columns[0];
        ReadColumnarDoubleValues(
            &column,
            startRowIndex,
            rowCount,
            Values_);
        ReadColumnarNullBitmap(
            &column,
            startRowIndex,
            rowCount,
            NullBitmap_.GetData());
    }

    i64 EstimateDataWeight(i64 lowerRowIndex, i64 upperRowIndex)
    {
        return (upperRowIndex - lowerRowIndex) * 8;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TUnversionedDoubleColumnReader
    : public TUnversionedColumnReaderBase
{
public:
    TUnversionedDoubleColumnReader(const TColumnMeta& columnMeta, int columnIndex, int columnId)
        : TUnversionedColumnReaderBase(
            columnMeta,
            columnIndex,
            columnId)
    { }

    virtual std::pair<i64, i64> GetEqualRange(
        const TUnversionedValue& value,
        i64 lowerRowIndex,
        i64 upperRowIndex) override
    {
        return DoGetEqualRange<EValueType::Double>(
            value,
            lowerRowIndex,
            upperRowIndex);
    }

private:
    virtual std::unique_ptr<IUnversionedSegmentReader> CreateSegmentReader(int segmentIndex, bool /* scan */) override
    {
        typedef TDenseUnversionedSegmentReader<
            EValueType::Double,
            TUnversionedDoubleValueExtractor> TSegmentReader;

        const auto& meta = ColumnMeta_.segments(segmentIndex);
        return DoCreateSegmentReader<TSegmentReader>(meta);
    }
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IUnversionedColumnReader> CreateUnversionedDoubleColumnReader(
    const TColumnMeta& columnMeta,
    int columnIndex,
    int columnId)
{
    return std::make_unique<TUnversionedDoubleColumnReader>(
        columnMeta,
        columnIndex,
        columnId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableChunkFormat
