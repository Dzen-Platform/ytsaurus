#include <yt/yt/core/test_framework/framework.h>

#include "column_format_ut.h"

#include <yt/yt/client/table_client/helpers.h>

#include <yt/yt/ytlib/table_chunk_format/null_column_reader.h>
#include <yt/yt/ytlib/table_chunk_format/null_column_writer.h>

namespace NYT::NTableClient {
namespace {

using namespace NTableChunkFormat;

////////////////////////////////////////////////////////////////////////////////

static const ui16 TestColumnId = 0;

std::unique_ptr<IValueColumnWriter> CreateColumnWriter(TDataBlockWriter* blockWriter)
{
    return CreateUnversionedNullColumnWriter(blockWriter);
}

std::vector<TUnversionedOwningRow> CreateNullRows(size_t count)
{
    return std::vector<TUnversionedOwningRow>(count, MakeUnversionedOwningRow(std::nullopt));
}

TEST(TUnversionedNullColumnTest, ReadValues)
{
    auto writeRead = [] (const std::vector<TUnversionedOwningRow>& rows) {
        const auto& [data, meta] = TSingleColumnWriter(CreateColumnWriter).WriteSingleSegmentBlock(rows);
        return TSingleColumnReader(CreateUnversionedNullColumnReader).ReadBlock(data, meta, TestColumnId);
    };

    {
        const auto expected = CreateNullRows(10);
        EXPECT_EQ(expected, writeRead(expected));
    }

    {
        const auto expected = CreateNullRows(8043);
        EXPECT_EQ(expected, writeRead(expected));
    }

    {
        const auto expected = CreateNullRows(1004080);
        EXPECT_EQ(expected, writeRead(expected));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTableClient
