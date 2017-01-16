#include "framework.h"

#include <yt/ytlib/table_client/public.h>
#include <yt/ytlib/table_client/unversioned_row.h>
#include <yt/ytlib/table_client/versioned_row.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

void ExpectSchemafulRowsEqual(TUnversionedRow expected, TUnversionedRow actual);

void ExpectSchemalessRowsEqual(TUnversionedRow expected, TUnversionedRow actual, int keyColumnCount);

void ExpectSchemafulRowsEqual(TVersionedRow expected, TVersionedRow actual);

void CheckResult(const std::vector<TVersionedRow>& expected, IVersionedReaderPtr reader);

template <class TExpectedRow, class TActualRow>
void CheckSchemafulResult(const std::vector<TExpectedRow>& expected, const std::vector<TActualRow>& actual)
{
    EXPECT_EQ(expected.size(), actual.size());
    for (int i = 0; i < expected.size(); ++i) {
        ExpectSchemafulRowsEqual(expected[i], actual[i]);
    }
}

template <class TExpectedRow, class TActualRow>
void CheckSchemalessResult(
    const std::vector<TExpectedRow>& expected,
    const std::vector<TActualRow>& actual,
    int keyColumnCount)
{
    EXPECT_EQ(expected.size(), actual.size());
    for (int i = 0; i < expected.size(); ++i) {
        ExpectSchemalessRowsEqual(expected[i], actual[i], keyColumnCount);
    }
}

template <class TRow, class TReader>
void CheckSchemalessResult(const std::vector<TRow>& expected, TIntrusivePtr<TReader> reader, int keyColumnCount)
{
    auto it = expected.begin();

    std::vector<TRow> actual;
    actual.reserve(997);

    while (reader->Read(&actual)) {
        if (actual.empty()) {
            ASSERT_TRUE(reader->GetReadyEvent().Get().IsOK());
            continue;
        }

        std::vector<TRow> ex(it, it + actual.size());
        CheckSchemalessResult(ex, actual, keyColumnCount);
        it += actual.size();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

