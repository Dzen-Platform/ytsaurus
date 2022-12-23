#include "helpers.h"

#include <yt/yt/client/chunk_client/read_limit.h>

#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/versioned_reader.h>

namespace NYT::NTableClient {

using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

void CheckEqual(const TUnversionedValue& expected, const TUnversionedValue& actual)
{
    // Fast path.
    if (AreRowValuesIdentical(expected, actual)) {
        return;
    }

    SCOPED_TRACE(Format("Expected: %v; Actual: %v", expected, actual));
    ASSERT_TRUE(AreRowValuesIdentical(expected, actual));
}

void CheckEqual(const TVersionedValue& expected, const TVersionedValue& actual)
{
    // Fast path.
    if (AreRowValuesIdentical(expected, actual)) {
        return;
    }

    SCOPED_TRACE(Format("Expected: %v; Actual: %v", expected, actual));
    ASSERT_TRUE(AreRowValuesIdentical(expected, actual));
}

void ExpectSchemafulRowsEqual(TUnversionedRow expected, TUnversionedRow actual)
{
    // Fast path.
    if (AreRowsIdentical(expected, actual)) {
        return;
    }

    SCOPED_TRACE(Format("Expected: %v; Actual: %v", expected, actual));

    ASSERT_EQ(static_cast<bool>(expected), static_cast<bool>(actual));
    if (!expected || !actual) {
        return;
    }
    ASSERT_EQ(expected.GetCount(), actual.GetCount());

    for (int valueIndex = 0; valueIndex < static_cast<int>(expected.GetCount()); ++valueIndex) {
        SCOPED_TRACE(Format("Value index %v", valueIndex));
        CheckEqual(expected[valueIndex], actual[valueIndex]);
    }
}

void ExpectSchemalessRowsEqual(TUnversionedRow expected, TUnversionedRow actual, int keyColumnCount)
{
    // Fast path.
    if (AreRowsIdentical(expected, actual)) {
        return;
    }

    SCOPED_TRACE(Format("Expected: %v; Actual: %v", expected, actual));

    ASSERT_EQ(static_cast<bool>(expected), static_cast<bool>(actual));
    if (!expected || !actual) {
        return;
    }
    ASSERT_EQ(expected.GetCount(), actual.GetCount());

    for (int valueIndex = 0; valueIndex < keyColumnCount; ++valueIndex) {
        SCOPED_TRACE(Format("Value index %v", valueIndex));
        CheckEqual(expected[valueIndex], actual[valueIndex]);
    }

    for (int valueIndex = keyColumnCount; valueIndex < static_cast<int>(expected.GetCount()); ++valueIndex) {
        SCOPED_TRACE(Format("Value index %v", valueIndex));

        // Find value with the same id. Since this in schemaless read, value positions can be different.
        bool found = false;
        for (int index = keyColumnCount; index < static_cast<int>(expected.GetCount()); ++index) {
            if (expected[valueIndex].Id == actual[index].Id) {
                CheckEqual(expected[valueIndex], actual[index]);
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found);
    }
}

void ExpectSchemafulRowsEqual(TVersionedRow expected, TVersionedRow actual)
{
    // Fast path.
    if (AreRowsIdentical(expected, actual)) {
        return;
    }

    SCOPED_TRACE(Format("Expected: %v; Actual: %v", expected, actual));

    ASSERT_EQ(static_cast<bool>(expected), static_cast<bool>(actual));
    if (!expected || !actual) {
        return;
    }

    ASSERT_EQ(expected.GetWriteTimestampCount(), actual.GetWriteTimestampCount());
    for (int i = 0; i < expected.GetWriteTimestampCount(); ++i) {
        SCOPED_TRACE(Format("Write Timestamp %v", i));
        ASSERT_EQ(expected.BeginWriteTimestamps()[i], actual.BeginWriteTimestamps()[i]);
    }

    ASSERT_EQ(expected.GetDeleteTimestampCount(), actual.GetDeleteTimestampCount());
    for (int i = 0; i < expected.GetDeleteTimestampCount(); ++i) {
        SCOPED_TRACE(Format("Delete Timestamp %v", i));
        ASSERT_EQ(expected.BeginDeleteTimestamps()[i], actual.BeginDeleteTimestamps()[i]);
    }

    ASSERT_EQ(expected.GetKeyCount(), actual.GetKeyCount());
    for (int index = 0; index < expected.GetKeyCount(); ++index) {
        SCOPED_TRACE(Format("Key index %v", index));
        CheckEqual(expected.BeginKeys()[index], actual.BeginKeys()[index]);
    }

    ASSERT_EQ(expected.GetValueCount(), actual.GetValueCount());
    for (int index = 0; index < expected.GetValueCount(); ++index) {
        SCOPED_TRACE(Format("Value index %v", index));
        CheckEqual(expected.BeginValues()[index], actual.BeginValues()[index]);
    }
}

void CheckResult(std::vector<TVersionedRow>* expected, IVersionedReaderPtr reader)
{
    expected->erase(
        std::remove_if(
            expected->begin(),
            expected->end(),
            [] (TVersionedRow row) {
                return !row;
            }),
        expected->end());

    auto it = expected->begin();
    std::vector<TVersionedRow> actual;
    actual.reserve(100);

    while (auto batch = reader->Read({.MaxRowsPerRead = 100})) {
        if (batch->IsEmpty()) {
            ASSERT_TRUE(reader->GetReadyEvent().Get().IsOK());
            continue;
        }

        auto range = batch->MaterializeRows();
        std::vector<TVersionedRow> actual(range.begin(), range.end());

        actual.erase(
            std::remove_if(
                actual.begin(),
                actual.end(),
                [] (TVersionedRow row) {
                    return !row;
                }),
            actual.end());

        std::vector<TVersionedRow> ex(it, std::min(it + actual.size(), expected->end()));

        CheckSchemafulResult(ex, actual);
        it += ex.size();
    }

    ASSERT_TRUE(it == expected->end());
}

std::vector<std::pair<ui32, ui32>> GetTimestampIndexRanges(
    TRange<TVersionedRow> rows,
    TTimestamp timestamp)
{
    std::vector<std::pair<ui32, ui32>> indexRanges;
    for (auto row : rows) {
        // Find delete timestamp.
        NTableClient::TTimestamp deleteTimestamp = NTableClient::NullTimestamp;
        for (auto deleteIt = row.BeginDeleteTimestamps(); deleteIt != row.EndDeleteTimestamps(); ++deleteIt) {
            if (*deleteIt <= timestamp) {
                deleteTimestamp = std::max(*deleteIt, deleteTimestamp);
            }
        }

        int lowerTimestampIndex = 0;
        while (lowerTimestampIndex < row.GetWriteTimestampCount() &&
               row.BeginWriteTimestamps()[lowerTimestampIndex] > timestamp)
        {
            ++lowerTimestampIndex;
        }

        int upperTimestampIndex = lowerTimestampIndex;
        while (upperTimestampIndex < row.GetWriteTimestampCount() &&
               row.BeginWriteTimestamps()[upperTimestampIndex] > deleteTimestamp)
        {
            ++upperTimestampIndex;
        }

        indexRanges.push_back(std::make_pair(lowerTimestampIndex, upperTimestampIndex));
    }
    return indexRanges;
}

std::vector<TUnversionedRow> CreateFilteredRangedRows(
    const std::vector<TUnversionedRow>& initial,
    TNameTablePtr writeNameTable,
    TNameTablePtr readNameTable,
    TColumnFilter columnFilter,
    TLegacyReadRange readRange,
    TChunkedMemoryPool* pool,
    int keyColumnCount)
{
    std::vector<TUnversionedRow> rows;

    int lowerRowIndex = 0;
    if (readRange.LowerLimit().HasRowIndex()) {
        lowerRowIndex = readRange.LowerLimit().GetRowIndex();
    }

    int upperRowIndex = initial.size();
    if (readRange.UpperLimit().HasRowIndex()) {
        upperRowIndex = readRange.UpperLimit().GetRowIndex();
    }

    auto fulfillLowerKeyLimit = [&] (TUnversionedRow row) {
        return !readRange.LowerLimit().HasLegacyKey() ||
            CompareRows(
                row.Begin(),
                row.Begin() + keyColumnCount,
                readRange.LowerLimit().GetLegacyKey().Begin(),
                readRange.LowerLimit().GetLegacyKey().End()) >= 0;
    };

    auto fulfillUpperKeyLimit = [&] (TUnversionedRow row) {
        return !readRange.UpperLimit().HasLegacyKey() ||
        CompareRows(
            row.Begin(),
            row.Begin() + keyColumnCount,
            readRange.UpperLimit().GetLegacyKey().Begin(),
            readRange.UpperLimit().GetLegacyKey().End()) < 0;
    };

    for (int rowIndex = lowerRowIndex; rowIndex < upperRowIndex; ++rowIndex) {
        auto initialRow = initial[rowIndex];
        if (fulfillLowerKeyLimit(initialRow) && fulfillUpperKeyLimit(initialRow)) {
            auto row = TMutableUnversionedRow::Allocate(pool, initialRow.GetCount());
            int count = 0;
            for (const auto* it = initialRow.Begin(); it != initialRow.End(); ++it) {
                auto name = writeNameTable->GetName(it->Id);
                auto readerId = readNameTable->GetId(name);

                if (columnFilter.ContainsIndex(readerId)) {
                    row[count] = *it;
                    row[count].Id = readerId;
                    ++count;
                }
            }
            row.SetCount(count);
            rows.push_back(row);
        }
    }

    return rows;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
