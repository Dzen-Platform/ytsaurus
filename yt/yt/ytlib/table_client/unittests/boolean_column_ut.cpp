#include "column_format_ut.h"
#include "helpers.h"

#include <yt/core/test_framework/framework.h>

#include <yt/ytlib/table_chunk_format/boolean_column_writer.h>
#include <yt/ytlib/table_chunk_format/boolean_column_reader.h>

namespace NYT::NTableClient {
namespace {

using namespace NTableChunkFormat;

////////////////////////////////////////////////////////////////////////////////

class TUnversionedBooleanColumnTest
    : public TUnversionedColumnTestBase<bool>
{
protected:
    std::vector<std::optional<bool>> CreateDirectDense()
    {
        std::vector<std::optional<bool>> data;
        for (int i = 0; i < 100 * 100; ++i) {
            data.push_back(i % 2 == 0);
        }
        return data;
    }

    std::vector<std::optional<bool>> CreateDirectRle()
    {
        std::vector<std::optional<bool>> data;
        for (int i = 0; i < 100; ++i) {
            for (int j = 0; j < 100; ++j) {
                data.push_back(i % 2 == 0);
            }
        }
        return data;
    }

    void Write(IValueColumnWriter* columnWriter)
    {
        WriteSegment(columnWriter, CreateDirectDense());
        WriteSegment(columnWriter, CreateDirectRle());
    }

    virtual std::optional<bool> DecodeValueFromColumn(
        const IUnversionedRowBatch::TColumn* column,
        i64 index) override
    {
        YT_VERIFY(column->StartIndex >= 0);
        index += column->StartIndex;
        
        ResolveRleEncoding(column, index);

        if (IsColumnValueNull(column, index)) {
            return std::nullopt;
        }

        return DecodeBoolFromColumn(*column, index);
    }

    virtual std::unique_ptr<IUnversionedColumnReader> DoCreateColumnReader() override
    {
        return CreateUnversionedBooleanColumnReader(ColumnMeta_, ColumnIndex, ColumnId);
    }

    virtual std::unique_ptr<IValueColumnWriter> CreateColumnWriter(TDataBlockWriter* blockWriter) override
    {
        return CreateUnversionedBooleanColumnWriter(
            ColumnIndex,
            blockWriter);
    }
};

TEST_F(TUnversionedBooleanColumnTest, ReadValues)
{
    std::vector<std::optional<bool>> expected;
    AppendVector(&expected, CreateDirectDense());
    AppendVector(&expected, CreateDirectRle());

    ValidateRows(CreateRows(expected), 1111, 15555);
    ValidateColumn(expected, 1111, 15555);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTableClient
