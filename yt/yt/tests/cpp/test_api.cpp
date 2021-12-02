#include <yt/yt/tests/cpp/test_base/api_test_base.h>

#include <yt/yt/client/api/rowset.h>
#include <yt/yt/client/api/transaction.h>
#include <yt/yt/client/api/table_reader.h>
#include <yt/yt/client/api/table_writer.h>

#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/ytlib/object_client/public.h>
#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/client/table_client/helpers.h>
#include <yt/yt/client/table_client/logical_type.h>
#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/ypath/rich.h>

#include <yt/yt/core/concurrency/scheduler.h>

#include <yt/yt/core/logging/config.h>
#include <yt/yt/core/logging/log_manager.h>

#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/core/yson/string.h>

#include <util/datetime/base.h>

#include <tuple>

////////////////////////////////////////////////////////////////////////////////

namespace NYT::NCppTests {
namespace {

using namespace NApi;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NRpc;
using namespace NSecurityClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TEST_F(TApiTestBase, TestClusterConnection)
{
    auto resOrError = Client_->GetNode(TYPath("/"));
    EXPECT_TRUE(resOrError.Get().IsOK());
}

TEST_F(TApiTestBase, TestCreateInvalidNode)
{
    auto resOrError = Client_->CreateNode(TYPath("//tmp/a"), EObjectType::SortedDynamicTabletStore);
    EXPECT_FALSE(resOrError.Get().IsOK());
}

////////////////////////////////////////////////////////////////////////////////

using TLookupFilterTestParam = std::tuple<
    std::vector<TString>,
    TString,
    TColumnFilter::TIndexes,
    TString,
    TString,
    TString>;

class TLookupFilterTest
    : public TDynamicTablesTestBase
    , public ::testing::WithParamInterface<TLookupFilterTestParam>
{
public:
    static void SetUpTestCase()
    {
        TDynamicTablesTestBase::SetUpTestCase();

        CreateTable(
            "//tmp/lookup_test", // tablePath
            "[" // schema
            "{name=k0;type=int64;sort_order=ascending};"
            "{name=k1;type=int64;sort_order=ascending};"
            "{name=k2;type=int64;sort_order=ascending};"
            "{name=v3;type=int64};"
            "{name=v4;type=int64};"
            "{name=v5;type=int64}]");

        InitializeRows();
    }

protected:
    static THashMap<int, TTimestamp> CommitTimestamps_;
    TRowBufferPtr Buffer_ = New<TRowBuffer>();

    static void InitializeRows()
    {
        WriteUnversionedRow(
            {"k0", "k1", "k2", "v3", "v4", "v5"},
            "<id=0> 10; <id=1> 11; <id=2> 12; <id=3> 13; <id=4> 14; <id=5> 15",
            0);
    }

    static void WriteUnversionedRow(
        std::vector<TString> names,
        const TString& rowString,
        int timestampTag)
    {
        auto preparedRow = PrepareUnversionedRow(names, rowString);
        WriteRows(
            std::get<1>(preparedRow),
            std::get<0>(preparedRow),
            timestampTag);
    }

    static void WriteRows(
        TNameTablePtr nameTable,
        TSharedRange<TUnversionedRow> rows,
        int timestampTag)
    {
        auto transaction = WaitFor(Client_->StartTransaction(NTransactionClient::ETransactionType::Tablet))
            .ValueOrThrow();

        transaction->WriteRows(
            Table_,
            nameTable,
            rows);

        auto commitResult = WaitFor(transaction->Commit())
            .ValueOrThrow();

        const auto& timestamps = commitResult.CommitTimestamps.Timestamps;
        ASSERT_EQ(1u, timestamps.size());
        CommitTimestamps_[timestampTag] = timestamps[0].second;
    }

    static void DeleteRow(
        std::vector<TString> names,
        const TString& rowString,
        int timestampTag)
    {
        auto preparedKey = PrepareUnversionedRow(names, rowString);
        DeleteRows(
            std::get<1>(preparedKey),
            std::get<0>(preparedKey),
            timestampTag);
    }

    static void DeleteRows(
        TNameTablePtr nameTable,
        TSharedRange<TUnversionedRow> rows,
        int timestampTag)
    {
        auto transaction = WaitFor(Client_->StartTransaction(NTransactionClient::ETransactionType::Tablet))
            .ValueOrThrow();

        transaction->DeleteRows(Table_, nameTable, rows);

        auto commitResult = WaitFor(transaction->Commit())
            .ValueOrThrow();

        const auto& timestamps = commitResult.CommitTimestamps.Timestamps;
        ASSERT_EQ(1u, timestamps.size());
        CommitTimestamps_[timestampTag] = timestamps[0].second;
    }

    TVersionedRow BuildVersionedRow(
        const TString& keyYson,
        const TString& valueYson,
        const std::vector<TTimestamp>& extraWriteTimestamps = {},
        const std::vector<TTimestamp>& deleteTimestamps = {})
    {
        auto immutableRow = YsonToVersionedRow(
            Buffer_,
            keyYson,
            valueYson,
            deleteTimestamps,
            extraWriteTimestamps);
        auto row = TMutableVersionedRow(const_cast<TVersionedRowHeader*>(immutableRow.GetHeader()));

        for (auto* value = row.BeginValues(); value < row.EndValues(); ++value) {
            value->Timestamp = CommitTimestamps_.at(value->Timestamp);
        }
        for (auto* timestamp = row.BeginWriteTimestamps(); timestamp < row.EndWriteTimestamps(); ++timestamp) {
            *timestamp = CommitTimestamps_.at(*timestamp);
        }
        for (auto* timestamp = row.BeginDeleteTimestamps(); timestamp < row.EndDeleteTimestamps(); ++timestamp) {
            *timestamp = CommitTimestamps_.at(*timestamp);
        }

        return row;
    }
};

THashMap<int, TTimestamp> TLookupFilterTest::CommitTimestamps_;

////////////////////////////////////////////////////////////////////////////////

static auto su = TString("<unique_keys=%false;strict=%true>");
static auto ku0 = "{name=k0;type=int64};";
static auto ku1 = "{name=k1;type=int64};";
static auto ku2 = "{name=k2;type=int64};";
static auto v3 = "{name=v3;type=int64};";
static auto v4 = "{name=v4;type=int64};";
static auto v5 = "{name=v5;type=int64};";


TEST_F(TLookupFilterTest, TestLookupAll)
{
    auto preparedKey = PrepareUnversionedRow(
        {"k0", "k1", "k2"},
        "<id=0> 10; <id=1> 11; <id=2> 12");

    auto res = WaitFor(Client_->LookupRows(
        Table_,
        std::get<1>(preparedKey),
        std::get<0>(preparedKey)))
        .ValueOrThrow();

    auto actual = ToString(res->GetRows()[0]);
    auto expected = ToString(YsonToSchemalessRow("<id=0> 10; <id=1> 11; <id=2> 12; <id=3> 13; <id=4> 14; <id=5> 15"));
    EXPECT_EQ(expected, actual);

    auto schema = ConvertTo<TTableSchema>(TYsonString(
        su + "[" + ku0 + ku1 + ku2 + v3 + v4 + v5 + "]"));

    auto actualSchema = ConvertToYsonString(res->GetSchema(), EYsonFormat::Text).ToString();
    auto expectedSchema = ConvertToYsonString(schema, EYsonFormat::Text).ToString();
    EXPECT_EQ(expectedSchema, actualSchema);
}

TEST_F(TLookupFilterTest, TestVersionedLookupAll)
{
    auto preparedKey = PrepareUnversionedRow(
        {"k0", "k1", "k2"},
        "<id=0> 10; <id=1> 11; <id=2> 12");

    auto res = WaitFor(Client_->VersionedLookupRows(
        Table_,
        std::get<1>(preparedKey),
        std::get<0>(preparedKey)))
        .ValueOrThrow();

    auto actual = ToString(res->GetRows()[0]);
    auto expected = ToString(BuildVersionedRow(
        "<id=0> 10; <id=1> 11; <id=2> 12",
        "<id=3;ts=0> 13; <id=4;ts=0> 14; <id=5;ts=0> 15"));
    EXPECT_EQ(expected, actual);

    auto schema = ConvertTo<TTableSchema>(TYsonString(
        su + "[" + ku0 + ku1 + ku2 + v3 + v4 + v5 + "]"));

    auto actualSchema = ConvertToYsonString(res->GetSchema(), EYsonFormat::Text).ToString();
    auto expectedSchema = ConvertToYsonString(schema, EYsonFormat::Text).ToString();
    EXPECT_EQ(expectedSchema, actualSchema);
}

TEST_P(TLookupFilterTest, TestLookupFilter)
{
    const auto& param = GetParam();
    const auto& namedColumns = std::get<0>(param);
    const auto& keyString = std::get<1>(param);
    auto columnFilter = std::get<2>(param);
    const auto& resultKeyString = std::get<3>(param);
    const auto& resultValueString = std::get<4>(param);
    const auto& schemaString = std::get<5>(param);
    auto rowString = resultKeyString + resultValueString;

    auto preparedKey = PrepareUnversionedRow(
        namedColumns,
        keyString);

    TLookupRowsOptions options;
    options.ColumnFilter = TColumnFilter(std::move(columnFilter));

    auto res = WaitFor(Client_->LookupRows(
        Table_,
        std::get<1>(preparedKey),
        std::get<0>(preparedKey),
        options))
        .ValueOrThrow();

    ASSERT_EQ(1u, res->GetRows().Size());

    auto actual = ToString(res->GetRows()[0]);
    auto expected = ToString(YsonToSchemalessRow(rowString));
    EXPECT_EQ(expected, actual)
        << "key: " << keyString << std::endl
        << "namedColumns: " << ::testing::PrintToString(namedColumns) << std::endl
        << "columnFilter: " << ::testing::PrintToString(columnFilter) << std::endl
        << "expectedRow: " << rowString << std::endl
        << "expectedSchema: " << schemaString << std::endl;

    auto schema = ConvertTo<TTableSchema>(TYsonString(schemaString));
    auto actualSchema = ConvertToYsonString(res->GetSchema(), EYsonFormat::Text).ToString();
    auto expectedSchema = ConvertToYsonString(schema, EYsonFormat::Text).ToString();
    EXPECT_EQ(expectedSchema, actualSchema)
        << "key: " << keyString << std::endl
        << "namedColumns: " << ::testing::PrintToString(namedColumns) << std::endl
        << "columnFilter: " << ::testing::PrintToString(columnFilter) << std::endl
        << "expectedRow: " << rowString << std::endl
        << "expectedSchema: " << schemaString << std::endl;
}

TEST_P(TLookupFilterTest, TestVersionedLookupFilter)
{
    const auto& param = GetParam();
    const auto& namedColumns = std::get<0>(param);
    const auto& keyString = std::get<1>(param);
    auto columnFilter = std::get<2>(param);
    const auto& resultKeyString = std::get<3>(param);
    const auto& resultValueString = std::get<4>(param);
    const auto& schemaString = std::get<5>(param);

    bool hasNonKeyColumns = false;
    for (const auto& column : namedColumns) {
        if (column.StartsWith("v")) {
            hasNonKeyColumns = true;
        }
    }

    auto preparedKey = PrepareUnversionedRow(
        namedColumns,
        keyString);

    TVersionedLookupRowsOptions options;
    options.ColumnFilter = TColumnFilter(std::move(columnFilter));

    auto res = WaitFor(Client_->VersionedLookupRows(
        Table_,
        std::get<1>(preparedKey),
        std::get<0>(preparedKey),
        options))
        .ValueOrThrow();

    ASSERT_EQ(1u, res->GetRows().Size());

    auto actual = ToString(res->GetRows()[0]);
    auto expected = ToString(BuildVersionedRow(
        resultKeyString,
        resultValueString,
        hasNonKeyColumns ? std::vector<TTimestamp>{} : std::vector<TTimestamp>{0}));
    EXPECT_EQ(expected, actual)
        << "key: " << keyString << std::endl
        << "namedColumns: " << ::testing::PrintToString(namedColumns) << std::endl
        << "columnFilter: " << ::testing::PrintToString(columnFilter) << std::endl
        << "expectedRowKeys: " << resultKeyString << std::endl
        << "expectedRowValues: " << resultValueString << std::endl
        << "expectedSchema: " << schemaString << std::endl;

    auto schema = ConvertTo<TTableSchema>(TYsonString(schemaString));
    auto actualSchema = ConvertToYsonString(res->GetSchema(), EYsonFormat::Text).ToString();
    auto expectedSchema = ConvertToYsonString(schema, EYsonFormat::Text).ToString();
    EXPECT_EQ(expectedSchema, actualSchema)
        << "key: " << keyString << std::endl
        << "namedColumns: " << ::testing::PrintToString(namedColumns) << std::endl
        << "columnFilter: " << ::testing::PrintToString(columnFilter) << std::endl
        << "expectedRowKeys: " << resultKeyString << std::endl
        << "expectedRowValues: " << resultValueString << std::endl
        << "expectedSchema: " << schemaString << std::endl;
}

INSTANTIATE_TEST_SUITE_P(
    TLookupFilterTest,
    TLookupFilterTest,
    ::testing::Values(
        TLookupFilterTestParam(
            {"k0", "k1", "k2"},
            "<id=0> 10; <id=1> 11; <id=2> 12;",
            {0,1,2},
            "<id=0> 10; <id=1> 11; <id=2> 12;", "",
            su + "[" + ku0 + ku1 + ku2 + "]"),
        TLookupFilterTestParam(
            {"k0", "k1", "k2"},
            "<id=0> 10; <id=1> 11; <id=2> 12;",
            {0,2,1},
            "<id=0> 10; <id=1> 12; <id=2> 11;", "",
            su + "[" + ku0 + ku2 + ku1 + "]"),
        TLookupFilterTestParam(
            {"k1", "k0", "k2"},
            "<id=2> 12; <id=0> 11; <id=1> 10;",
            {1,0,2},
            "<id=0> 10; <id=1> 11; <id=2> 12;", "",
            su + "[" + ku0 + ku1 + ku2 + "]"),
        TLookupFilterTestParam(
            {"k0", "k1", "k2", "v3", "v4", "v5"},
            "<id=0> 10; <id=1> 11; <id=2> 12;",
            {3,4,5},
            "", "<id=0;ts=0> 13; <id=1;ts=0> 14; <id=2;ts=0> 15;",
            su + "[" + v3 + v4 + v5 + "]"),
        TLookupFilterTestParam(
            {"k0", "k1", "k2", "v3", "v4", "v5"},
            "<id=0> 10; <id=1> 11; <id=2> 12;",
            {1,5,3},
            "<id=0> 11;", "<id=1;ts=0> 15; <id=2;ts=0> 13;",
            su + "[" + ku1 + v5 + v3 + "]"),
        TLookupFilterTestParam(
            {"k0", "k1", "k2", "v3", "v4", "v5"},
            "<id=0> 10; <id=1> 11; <id=2> 12;",
            {3,4,5},
            "", "<id=0;ts=0> 13; <id=1;ts=0> 14; <id=2;ts=0> 15;",
            su + "[" + v3 + v4 + v5 + "]"),
        TLookupFilterTestParam(
            {"k0", "k1", "k2", "v3", "v4", "v5"},
            "<id=0> 10; <id=1> 11; <id=2> 12;",
            {5,3,4},
            "", "<id=0;ts=0> 15; <id=1;ts=0> 13; <id=2;ts=0> 14;",
            su + "[" + v5 + v3 + v4 + "]"),
        TLookupFilterTestParam(
            {"k1", "k0", "k2", "v5", "v3", "v4"},
            "<id=2> 12; <id=0> 11; <id=1> 10;",
            {1,0,2,4,5,3},
            "<id=0> 10; <id=1> 11; <id=2> 12;", "<id=3;ts=0> 13; <id=4;ts=0> 14; <id=5;ts=0> 15;",
            su + "[" + ku0 + ku1 + ku2 + v3 + v4 + v5 + "]"),
        TLookupFilterTestParam(
            {"k1", "k0", "k2", "v5", "v3", "v4"},
            "<id=2> 12; <id=0> 11; <id=1> 10;",
            {2,1,5,4},
            "<id=0> 12; <id=1> 10;", "<id=2;ts=0> 14; <id=3;ts=0> 13;",
            su + "[" + ku2 + ku0 + v4 + v3 + "]")
));

TEST_F(TLookupFilterTest, TestRetentionConfig)
{
    WriteUnversionedRow(
        {"k0", "k1", "k2", "v3", "v4", "v5"},
        "<id=0> 20; <id=1> 20; <id=2> 20; <id=3> 20;",
        1);
    WriteUnversionedRow(
        {"k0", "k1", "k2", "v3", "v4", "v5"},
        "<id=0> 20; <id=1> 20; <id=2> 20; <id=3> 21;",
        2);

    auto preparedKey = PrepareUnversionedRow(
        {"k0", "k1", "k2", "v4"},
        "<id=0> 20; <id=1> 20; <id=2> 20");

    auto res = WaitFor(Client_->VersionedLookupRows(
        Table_,
        std::get<1>(preparedKey),
        std::get<0>(preparedKey)))
        .ValueOrThrow();

    ASSERT_EQ(1u, res->GetRows().Size());

    auto actual = ToString(res->GetRows()[0]);
    auto expected = ToString(BuildVersionedRow(
        "<id=0> 20; <id=1> 20; <id=2> 20",
        "<id=3;ts=2> 21; <id=3;ts=1> 20;"));
    EXPECT_EQ(expected, actual);

    TVersionedLookupRowsOptions options;
    options.RetentionConfig = New<TRetentionConfig>();
    options.RetentionConfig->MinDataTtl = TDuration::MilliSeconds(0);
    options.RetentionConfig->MaxDataTtl = TDuration::MilliSeconds(1800000);
    options.RetentionConfig->MinDataVersions = 1;
    options.RetentionConfig->MaxDataVersions = 1;
    options.Timestamp = CommitTimestamps_[2] + 1;

    res = WaitFor(Client_->VersionedLookupRows(
        Table_,
        std::get<1>(preparedKey),
        std::get<0>(preparedKey),
        options))
        .ValueOrThrow();

    ASSERT_EQ(1u, res->GetRows().Size());

    actual = ToString(res->GetRows()[0]);
    expected = ToString(BuildVersionedRow(
        "<id=0> 20; <id=1> 20; <id=2> 20",
        "<id=3;ts=2> 21;"));
    EXPECT_EQ(expected, actual);

    options.ColumnFilter = TColumnFilter({0,1,2,3});

    res = WaitFor(Client_->VersionedLookupRows(
        Table_,
        std::get<1>(preparedKey),
        std::get<0>(preparedKey),
        options))
        .ValueOrThrow();

    ASSERT_EQ(1u, res->GetRows().Size());

    actual = ToString(res->GetRows()[0]);
    expected = ToString(BuildVersionedRow(
        "<id=0> 20; <id=1> 20; <id=2> 20",
        "",
        {2}));
    EXPECT_EQ(expected, actual);

    options.ColumnFilter = TColumnFilter({3});

    preparedKey = PrepareUnversionedRow(
        {"k0", "k1", "k2", "v3"},
        "<id=0> 20; <id=1> 20; <id=2> 20");
    res = WaitFor(Client_->VersionedLookupRows(
        Table_,
        std::get<1>(preparedKey),
        std::get<0>(preparedKey),
        options))
        .ValueOrThrow();

    ASSERT_EQ(1u, res->GetRows().Size());

    actual = ToString(res->GetRows()[0]);
    expected = ToString(BuildVersionedRow(
        "",
        "<id=0;ts=2> 21;"));
    EXPECT_EQ(expected, actual);
}

// YT-7668
// Checks that in cases like
//   insert(key=k, value1=x, value2=y)
//   delete(key=k)
//   insert(key=k, value1=x)
//   versioned_lookup(key=k, column_filter=[value1])
// the information about the presence of the second insertion is not lost,
// although no versioned values are returned.
TEST_F(TLookupFilterTest, TestFilteredOutTimestamps)
{
    auto preparedKey = PrepareUnversionedRow(
        {"k0", "k1", "k2", "v3", "v4", "v5"},
        "<id=0> 30; <id=1> 30; <id=2> 30");
    TVersionedLookupRowsOptions options;

    auto executeLookup = [&] {
        auto res = WaitFor(Client_->VersionedLookupRows(
            Table_,
            std::get<1>(preparedKey),
            std::get<0>(preparedKey),
            options)).ValueOrThrow();
        EXPECT_EQ(1u, res->GetRows().Size());
        return ToString(res->GetRows()[0]);
    };

    WriteUnversionedRow(
        {"k0", "k1", "k2", "v3", "v4", "v5"},
        "<id=0> 30; <id=1> 30; <id=2> 30; <id=3> 1; <id=4> 1; <id=5> 1",
        1);

    DeleteRows(std::get<1>(preparedKey), std::get<0>(preparedKey), 2);

    WriteUnversionedRow(
        {"k0", "k1", "k2", "v3"},
        "<id=0> 30; <id=1> 30; <id=2> 30; <id=3> 3;",
        3);

    options.ColumnFilter = TColumnFilter();
    options.RetentionConfig = New<TRetentionConfig>();
    options.RetentionConfig->MinDataTtl = TDuration::MilliSeconds(0);
    options.RetentionConfig->MaxDataTtl = TDuration::MilliSeconds(1800000);
    options.RetentionConfig->MinDataVersions = 1;
    options.RetentionConfig->MaxDataVersions = 1;

    auto actual = executeLookup();
    auto expected = ToString(BuildVersionedRow(
        "<id=0> 30; <id=1> 30; <id=2> 30",
        "<id=3;ts=3> 3",
        {},
        {2}));
    EXPECT_EQ(expected, actual);

    options.ColumnFilter = TColumnFilter({0, 1, 2, 4});

    actual = executeLookup();
    expected = ToString(BuildVersionedRow(
        "<id=0> 30; <id=1> 30; <id=2> 30",
        "",
        {3},
        {2}));
    EXPECT_EQ(expected, actual);

    WriteUnversionedRow(
        {"k0", "k1", "k2", "v4"},
        "<id=0> 30; <id=1> 30; <id=2> 30; <id=3> 4",
        4);

    actual = executeLookup();
    expected = ToString(BuildVersionedRow(
        "<id=0> 30; <id=1> 30; <id=2> 30",
        "<id=3;ts=4> 4",
        {3},
        {2}
    ));
    EXPECT_EQ(expected, actual);

    DeleteRows(std::get<1>(preparedKey), std::get<0>(preparedKey), 5);

    WriteUnversionedRow(
        {"k0", "k1", "k2", "v3"},
        "<id=0> 30; <id=1> 30; <id=2> 30; <id=3> 6;",
        6);

    options.ColumnFilter = TColumnFilter({0, 1, 2, 4, 5});
    options.RetentionConfig->MinDataVersions = 2;
    options.RetentionConfig->MaxDataVersions = 2;

    actual = executeLookup();
    expected = ToString(BuildVersionedRow(
        "<id=0> 30; <id=1> 30; <id=2> 30;",
        "<id=3;ts=4> 4",
        {6},
        {2, 5}
    ));
    EXPECT_EQ(expected, actual);

    options.RetentionConfig->MinDataVersions = 1;
    options.RetentionConfig->MaxDataVersions = 1;

    actual = executeLookup();
    expected = ToString(BuildVersionedRow(
        "<id=0> 30; <id=1> 30; <id=2> 30;",
        "",
        {6},
        {2, 5}
    ));
    EXPECT_EQ(expected, actual);
}

TEST_F(TLookupFilterTest, TestLookupDuplicateKeyColumns)
{
    auto preparedKey = PrepareUnversionedRow(
        {"k0", "k1", "k2"},
        "<id=0> 20; <id=1> 21; <id=2> 22; <id=2> 22");

    EXPECT_THROW(WaitFor(Client_->LookupRows(
        Table_,
        std::get<1>(preparedKey),
        std::get<0>(preparedKey)))
            .ValueOrThrow(), TErrorException);
}

TEST_F(TLookupFilterTest, YT_10159)
{
    WriteUnversionedRow(
        {"k0", "k1", "k2", "v3"},
        "<id=0> 1; <id=1> 1; <id=2> 1; <id=3> 1",
        7);
    WriteUnversionedRow(
        {"k0", "k1", "k2", "v3"},
        "<id=0> 99; <id=1> 99; <id=2> 99; <id=3> 22",
        8);

    auto preparedKey = PrepareUnversionedRow(
        {"k0", "k1", "k2"},
        "<id=0> 99; <id=1> 99; <id=2> 99");

    for (int iter = 0; iter < 2; ++iter) {
        TVersionedLookupRowsOptions options;

        {
            options.Timestamp = CommitTimestamps_[7];
            auto res = WaitFor(Client_->VersionedLookupRows(
                Table_,
                std::get<1>(preparedKey),
                std::get<0>(preparedKey),
                options)).ValueOrThrow();
            EXPECT_EQ(0u, res->GetRows().Size());
        }

        {
            options.Timestamp = CommitTimestamps_[8];
            auto res = WaitFor(Client_->VersionedLookupRows(
                Table_,
                std::get<1>(preparedKey),
                std::get<0>(preparedKey),
                options)).ValueOrThrow();
            EXPECT_EQ(1u, res->GetRows().Size());
        }

        if (iter == 0) {
            SyncUnmountTable(Table_);
            SyncMountTable(Table_);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

class TOrderedDynamicTablesTest
    : public TDynamicTablesTestBase
{
public:
    static void SetUpTestCase()
    {
        TDynamicTablesTestBase::SetUpTestCase();

        CreateTable(
            "//tmp/write_ordered_test", // tablePath
            "[" // schema
            "{name=v1;type=int64};"
            "{name=v2;type=int64};"
            "{name=v3;type=int64}]");
    }
};

////////////////////////////////////////////////////////////////////////////////

TEST_F(TOrderedDynamicTablesTest, TestOrderedTableWrite)
{
    WriteUnversionedRow(
        {"v3", "v1", "v2"},
        "<id=0> 15; <id=1> 13; <id=2> 14;");
    WriteUnversionedRow(
        {"v2", "v3", "v1"},
        "<id=0> 24; <id=1> 25; <id=2> 23;");

    WriteUnversionedRow(
        {"v3", "v1", "v2", "$tablet_index"},
        "<id=0> 15; <id=1> 13; <id=2> 14; <id=3> #;");
    WriteUnversionedRow(
        {"v2", "v3", "v1", "$tablet_index"},
        "<id=0> 24; <id=1> 25; <id=2> 23; <id=3> 0;");

    auto res = WaitFor(Client_->SelectRows(Format("* from [%v]", Table_))).ValueOrThrow();
    auto rows = res.Rowset->GetRows();

    ASSERT_EQ(4u, rows.Size());

    auto actual = ToString(rows[0]);
    auto expected = ToString(YsonToSchemalessRow(
        "<id=0> 0; <id=1> 0; <id=2> 13; <id=3> 14; <id=4> 15;"));
    EXPECT_EQ(expected, actual);

    actual = ToString(rows[1]);
    expected = ToString(YsonToSchemalessRow(
        "<id=0> 0; <id=1> 1; <id=2> 23; <id=3> 24; <id=4> 25;"));
    EXPECT_EQ(expected, actual);

    actual = ToString(rows[2]);
    expected = ToString(YsonToSchemalessRow(
        "<id=0> 0; <id=1> 2; <id=2> 13; <id=3> 14; <id=4> 15;"));
    EXPECT_EQ(expected, actual);

    actual = ToString(rows[3]);
    expected = ToString(YsonToSchemalessRow(
        "<id=0> 0; <id=1> 3; <id=2> 23; <id=3> 24; <id=4> 25;"));
    EXPECT_EQ(expected, actual);
}

////////////////////////////////////////////////////////////////////////////////

class TTypeV3Test : public TApiTestBase
{ };

TEST_F(TTypeV3Test, TestCreateTable)
{
    auto schema = New<TTableSchema>(std::vector<TColumnSchema>{
        TColumnSchema("key", SimpleLogicalType(ESimpleLogicalValueType::String)),
        TColumnSchema("value", ListLogicalType(SimpleLogicalType(ESimpleLogicalValueType::Int64))),
    });
    TCreateNodeOptions options;
    options.Attributes = NYTree::CreateEphemeralAttributes();
    options.Attributes->Set("schema", schema);
    WaitFor(Client_->CreateNode("//tmp/f", EObjectType::Table, options))
        .ThrowOnError();

    auto rowBuffer = New<TRowBuffer>();
    std::vector<TUnversionedRow> writtenData;

    {
        auto tableWriter = WaitFor(Client_->CreateTableWriter(NYPath::TRichYPath("//tmp/f")))
            .ValueOrThrow();

        auto nameTable = tableWriter->GetNameTable();
        auto writerSchema = tableWriter->GetSchema();
        EXPECT_EQ(*writerSchema, *schema);
        auto createRow = [&] (TStringBuf key, TStringBuf value) {
            TUnversionedOwningRowBuilder builder;

            builder.AddValue(MakeUnversionedStringValue(key, nameTable->GetIdOrRegisterName("key")));
            builder.AddValue(MakeUnversionedCompositeValue(value, nameTable->GetIdOrRegisterName("value")));

            return rowBuffer->CaptureRow(builder.FinishRow().Get());
        };

        writtenData = {
            createRow("foo", "[3; 4; 5]"),
            createRow("bar", "[6; 7]"),
        };
        auto written = tableWriter->Write(MakeRange<TUnversionedRow>(writtenData));
        EXPECT_EQ(written, true);

        WaitFor(tableWriter->Close())
            .ThrowOnError();
    }

    {
        auto tableReader = WaitFor(Client_->CreateTableReader(NYPath::TRichYPath("//tmp/f")))
            .ValueOrThrow();

        auto readerSchema = tableReader->GetTableSchema();
        EXPECT_EQ(*readerSchema, *schema);

        std::vector<TUnversionedRow> tableRows;
        while (auto batch = tableReader->Read()) {
            if (batch->IsEmpty()) {
                WaitFor(tableReader->GetReadyEvent())
                    .ThrowOnError();
            }

            for (const auto row : batch->MaterializeRows()) {
                tableRows.emplace_back(rowBuffer->CaptureRow(row));
            }
        }

        auto toStringVector = [] (const std::vector<TUnversionedRow>& rows) {
            std::vector<TString> result;
            for (const auto& r : rows) {
                result.push_back(ToString(r));
            }
            return result;
        };

        EXPECT_EQ(toStringVector(tableRows), toStringVector(writtenData));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NCppTests
