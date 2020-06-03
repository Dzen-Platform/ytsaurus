#include "column_format_ut.h"
#include "helpers.h"

#include <yt/core/test_framework/framework.h>

#include <yt/ytlib/table_chunk_format/public.h>
#include <yt/ytlib/table_chunk_format/string_column_writer.h>
#include <yt/ytlib/table_chunk_format/string_column_reader.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/client/table_client/unversioned_row.h>

namespace NYT::NTableClient {
namespace {

using namespace NTableChunkFormat;

////////////////////////////////////////////////////////////////////////////////

TEST(TAnyColumnTest, Simple)
{
    TUnversionedOwningRowBuilder builder;
    std::vector<TUnversionedOwningRow> rows;

    builder.AddValue(MakeUnversionedInt64Value(-42, 0));
    rows.push_back(builder.FinishRow());

    builder.AddValue(MakeUnversionedUint64Value(777, 0));
    rows.push_back(builder.FinishRow());

    builder.AddValue(MakeUnversionedDoubleValue(0.01, 0));
    rows.push_back(builder.FinishRow());

    builder.AddValue(MakeUnversionedBooleanValue(false, 0));
    rows.push_back(builder.FinishRow());

    builder.AddValue(MakeUnversionedBooleanValue(true, 0));
    rows.push_back(builder.FinishRow());

    builder.AddValue(MakeUnversionedStringValue(AsStringBuf("This is string"), 0));
    rows.push_back(builder.FinishRow());

    builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 0));
    rows.push_back(builder.FinishRow());

    builder.AddValue(MakeUnversionedAnyValue(AsStringBuf("{a = b}"), 0));
    rows.push_back(builder.FinishRow());

    builder.AddValue(MakeUnversionedAnyValue(AsStringBuf("[]"), 0));
    rows.push_back(builder.FinishRow());

    std::vector<TUnversionedRow> expected(rows.size());
    std::transform(
        rows.begin(),
        rows.end(),
        expected.begin(),
        [](TUnversionedOwningRow owningRow) {
            return owningRow.Get();
        });

    TDataBlockWriter blockWriter;
    auto columnWriter = CreateUnversionedAnyColumnWriter(0, &blockWriter);

    columnWriter->WriteUnversionedValues(MakeRange(expected));
    columnWriter->FinishCurrentSegment();

    auto block = blockWriter.DumpBlock(0, 8);
    auto* codec = NCompression::GetCodec(NCompression::ECodec::None);

    auto columnData = codec->Compress(block.Data);
    auto columnMeta = columnWriter->ColumnMeta();

    auto reader = CreateUnversionedAnyColumnReader(columnMeta, 0, 0);
    reader->ResetBlock(columnData, 0);

    EXPECT_EQ(expected.size(), reader->GetReadyUpperRowIndex());

    TChunkedMemoryPool pool;
    std::vector<TMutableUnversionedRow> actual;
    for (int i = 0; i < expected.size(); ++i) {
        actual.push_back(TMutableUnversionedRow::Allocate(&pool, 1));
    }

    reader->ReadValues(TMutableRange<TMutableUnversionedRow>(actual.data(), actual.size()));
    // XXX
    CheckSchemafulResult(expected, actual);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTableCLient
