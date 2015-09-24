#include "stdafx.h"
#include "framework.h"

#include <core/misc/protobuf_helpers.h>
#include <core/misc/new.h>

#include <core/ytree/convert.h>

#include <ytlib/table_client/versioned_row.h>
#include <ytlib/table_client/unversioned_row.h>
#include <ytlib/table_client/row_buffer.h>
#include <ytlib/table_client/row_merger.h>
#include <ytlib/table_client/config.h>
#include <ytlib/table_client/versioned_reader.h>
#include <ytlib/table_client/schemaful_reader.h>
#include <ytlib/table_client/schemaful_overlapping_chunk_reader.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

void PrintTo(const TVersionedRow& row, ::std::ostream* os)
{
    *os << ToString(row);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

namespace NYT {
namespace NTableClient {
namespace {

using namespace NYTree;
using namespace NYson;
using namespace NTransactionClient;
using namespace NConcurrency;

using ::ToString;

////////////////////////////////////////////////////////////////////////////////

class TRowMergerTestBase
    : public ::testing::Test
{
protected:
    const TRowBufferPtr Buffer_ = New<TRowBuffer>();
    int KeyCount_ = -1;

    TVersionedRow BuildVersionedRow(
        const Stroka& keyYson,
        const Stroka& valueYson,
        const std::vector<TTimestamp>& deleteTimestamps = std::vector<TTimestamp>())
    {
        TVersionedRowBuilder builder(Buffer_);

        auto keys = ConvertTo<std::vector<INodePtr>>(TYsonString(keyYson, EYsonType::ListFragment));

        if (KeyCount_ == -1) {
            KeyCount_ = keys.size();
        } else {
            YCHECK(KeyCount_ == keys.size());
        }

        int keyId = 0;
        for (auto key : keys) {
            switch (key->GetType()) {
                case ENodeType::Int64:
                    builder.AddKey(MakeUnversionedInt64Value(key->GetValue<i64>(), keyId));
                    break;
                case ENodeType::Uint64:
                    builder.AddKey(MakeUnversionedUint64Value(key->GetValue<ui64>(), keyId));
                    break;
                case ENodeType::Double:
                    builder.AddKey(MakeUnversionedDoubleValue(key->GetValue<double>(), keyId));
                    break;
                case ENodeType::String:
                    builder.AddKey(MakeUnversionedStringValue(key->GetValue<Stroka>(), keyId));
                    break;
                default:
                    YUNREACHABLE();
                    break;
            }
            ++keyId;
        }

        auto values = ConvertTo<std::vector<INodePtr>>(TYsonString(valueYson, EYsonType::ListFragment));
        for (auto value : values) {
            int id = value->Attributes().Get<int>("id");
            auto timestamp = SecondsToTimestamp(value->Attributes().Get<TTimestamp>("ts"));
            switch (value->GetType()) {
                case ENodeType::Entity:
                    builder.AddValue(MakeVersionedSentinelValue(EValueType::Null, timestamp, id));
                    break;
                case ENodeType::Int64:
                    builder.AddValue(MakeVersionedInt64Value(value->GetValue<i64>(), timestamp, id));
                    break;
                case ENodeType::Uint64:
                    builder.AddValue(MakeVersionedUint64Value(value->GetValue<ui64>(), timestamp, id));
                    break;
                case ENodeType::Double:
                    builder.AddValue(MakeVersionedDoubleValue(value->GetValue<double>(), timestamp, id));
                    break;
                case ENodeType::String:
                    builder.AddValue(MakeVersionedStringValue(value->GetValue<Stroka>(), timestamp, id));
                    break;
                default:
                    builder.AddValue(MakeVersionedAnyValue(ConvertToYsonString(value).Data(), timestamp, id));
                    break;
            }
        }

        for (auto timestamp : deleteTimestamps) {
            builder.AddDeleteTimestamp(SecondsToTimestamp(timestamp));
        }

        return builder.FinishRow();
    }

    TUnversionedRow BuildUnversionedRow(const Stroka& valueYson)
    {
        TUnversionedRowBuilder builder;

        auto values = ConvertTo<std::vector<INodePtr>>(TYsonString(valueYson, EYsonType::ListFragment));
        for (auto value : values) {
            int id = value->Attributes().Get<int>("id");
            switch (value->GetType()) {
                case ENodeType::Entity:
                    builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id));
                    break;
                case ENodeType::Int64:
                    builder.AddValue(MakeUnversionedInt64Value(value->GetValue<i64>(), id));
                    break;
                case ENodeType::Uint64:
                    builder.AddValue(MakeUnversionedUint64Value(value->GetValue<ui64>(), id));
                    break;
                case ENodeType::Double:
                    builder.AddValue(MakeUnversionedDoubleValue(value->GetValue<double>(), id));
                    break;
                case ENodeType::String:
                    builder.AddValue(MakeUnversionedStringValue(value->GetValue<Stroka>(), id));
                    break;
                default:
                    builder.AddValue(MakeUnversionedAnyValue(ConvertToYsonString(value).Data(), id));
                    break;
            }
        }

        return Buffer_->Capture(builder.GetRow());
    }


    static TTimestamp SecondsToTimestamp(int seconds)
    {
        return TTimestamp(seconds) << TimestampCounterWidth;
    }

};


////////////////////////////////////////////////////////////////////////////////

class TSchemafulRowMergerTest
    : public TRowMergerTestBase
{ };

TEST_F(TSchemafulRowMergerTest, Simple1)
{
    auto merger = New<TSchemafulRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=2;ts=200> 3.14"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 2; <id=2> 3.14; <id=3> \"test\""),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Simple2)
{
    auto merger = New<TSchemafulRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=300> 3"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 3; <id=2> #; <id=3> #"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Delete1)
{
    auto merger = New<TSchemafulRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));

    EXPECT_EQ(
        TUnversionedRow(),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Delete2)
{
    auto merger = New<TSchemafulRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 1; <id=2;ts=200> 3.14; <id=3;ts=200> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Delete3)
{
    auto merger = New<TSchemafulRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 300 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 1; <id=2;ts=200> 3.14; <id=3;ts=200> \"test\""));

    EXPECT_EQ(
        TUnversionedRow(),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Delete4)
{
    auto merger = New<TSchemafulRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 300 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 1; <id=2;ts=200> 3.14; <id=3;ts=200> \"test\""));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=2;ts=400> 3.15"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> 3.15; <id=3> #"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Filter1)
{
    TColumnFilter filter { 0 };
    auto merger = New<TSchemafulRowMerger>(Buffer_, 4, 1, filter);

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=2;ts=200> 3.14"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Filter2)
{
    TColumnFilter filter { 1, 2 };
    auto merger = New<TSchemafulRowMerger>(Buffer_, 4, 1, filter);

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=2;ts=200> 3.14"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=1> 2; <id=2> 3.14"),
        merger->BuildMergedRow());
}

////////////////////////////////////////////////////////////////////////////////

class TUnversionedRowMergerTest
    : public TRowMergerTestBase
{ };

TEST_F(TUnversionedRowMergerTest, Simple1)
{
    auto merger = New<TUnversionedRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 2"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=2> 3.14"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 2; <id=2> 3.14; <id=3> \"test\""),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Simple2)
{
    auto merger = New<TUnversionedRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 1"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 2"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 3"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 3;"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Delete1)
{
    auto merger = New<TUnversionedRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Delete2)
{
    auto merger = New<TUnversionedRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Delete3)
{
    auto merger = New<TUnversionedRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""));
    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Delete4)
{
    auto merger = New<TUnversionedRowMerger>(Buffer_, 4, 1, TColumnFilter());

    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""));
    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=2> 3.15"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> 3.15; <id=3> #"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Filter1)
{
    TColumnFilter filter { 0 };
    auto merger = New<TUnversionedRowMerger>(Buffer_, 4, 1, filter);

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 2"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=2> 3.14"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Filter2)
{
    TColumnFilter filter { 1, 2 };
    auto merger = New<TUnversionedRowMerger>(Buffer_, 4, 1, filter);

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 2"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=2> 3.14"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=1> 2; <id=2> 3.14"),
        merger->BuildMergedRow());
}

////////////////////////////////////////////////////////////////////////////////

class TVersionedRowMergerTest
    : public TRowMergerTestBase
{ };

TEST_F(TVersionedRowMergerTest, KeepAll1)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 10;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));

    EXPECT_EQ(
        BuildVersionedRow("0", "<id=1;ts=100> 1"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepAll2)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 10;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=300> 3"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=1;ts=300> 3; <id=1;ts=200> 2; <id=1;ts=100> 1;"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepAll3)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 10;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 2", {  50 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1", { 150 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=300> 3", { 250 }));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=1;ts=300> 3; <id=1;ts=200> 2; <id=1;ts=100> 1;",
            { 50, 150, 250 }),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepAll4)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 10;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 2; <id=2;ts=200> 3.14"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=300> 3; <id=3;ts=500> \"test\""));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=1;ts=300> 3; <id=1;ts=200> 2; <id=1;ts=100> 1;"
            "<id=2;ts=200> 3.14;"
            "<id=3;ts=500> \"test\";"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepAll5)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 10;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1; <id=1;ts=200> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=2;ts=100> 3; <id=2;ts=200> 4"));
    
    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=1;ts=200> 2; <id=1;ts=100> 1;"
            "<id=2;ts=200> 4; <id=2;ts=100> 3;"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepLatest1)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;
    config->MaxDataVersions = 1;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=300> 3"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=1;ts=300> 3"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepLatest2)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;
    config->MaxDataVersions = 1;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 2; <id=1;ts=199> 20"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=2;ts=100> 3.14; <id=2;ts=99> 3.15"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300> \"test\"; <id=3;ts=299> \"tset\""));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=1;ts=200> 2;"
            "<id=2;ts=100> 3.14;"
            "<id=3;ts=300> \"test\""),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepLatest3)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;
    config->MaxDataVersions = 1;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), SecondsToTimestamp(200));
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200 }));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "",
            { 200 }),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepLatest4)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;
    config->MaxDataVersions = 1;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), SecondsToTimestamp(201));
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200 }));

    EXPECT_FALSE(merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepLatest5)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 3;
    config->MaxDataVersions = 3;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), SecondsToTimestamp(400));

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=300> 3"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 150, 250 }));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=1;ts=300> 3; <id=1;ts=200> 2;",
            { 250 }),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepLatest6)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 2;
    config->MaxDataVersions = 2;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000000), SecondsToTimestamp(150));

    merger->AddPartialRow(BuildVersionedRow("0", "", { 100, 200, 300 }));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "",
            { 200, 300 }),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, Expire1)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 0;
    config->MaxDataTtl = TDuration::Seconds(1000);

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1101), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));

    EXPECT_EQ(
        BuildVersionedRow("0", "<id=1;ts=100> 1"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, Expire2)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 0;
    config->MaxDataTtl = TDuration::Seconds(1000);

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1102), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));

    EXPECT_FALSE(merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, Expire3)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;
    config->MaxDataVersions = 3;
    config->MinDataTtl = TDuration::Seconds(0);
    config->MaxDataTtl = TDuration::Seconds(10000);

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000), 0);
    
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=300> 3"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=400> 4"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=2;ts=200> 3.14"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300> \"test\""));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 350 }));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=1;ts=400> 4; <id=1;ts=300> 3;"
            "<id=2;ts=200> 3.14;"
            "<id=3;ts=300> \"test\";",
            { 350 }),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, DeleteOnly)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 10;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000), 0);

    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "",
            { 100 }),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, ManyDeletes)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 10;

    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000), 0);

    merger->AddPartialRow(BuildVersionedRow("0", "", { 200 }));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 300 }));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "",
            { 100, 200, 300 }),
        merger->BuildMergedRow());
}

///////////////////////////////////////////////////////////////////////////////

class TMockVersionedReader
    : public IVersionedReader
{
public:
    explicit TMockVersionedReader(std::vector<TVersionedRow> rows)
        : Rows_(rows)
    { }

    virtual TFuture<void> Open() override
    {
        return VoidFuture;
    }

    virtual bool Read(std::vector<TVersionedRow>* rows) override
    {
        rows->clear();

        if (Position_ == Rows_.size()) {
            return false;
        }

        while (Position_ < Rows_.size() && rows->size() < rows->capacity()) {
            rows->push_back(Rows_[Position_]);
            ++Position_;
        }

        return true;
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return VoidFuture;
    }

private:
    std::vector<TVersionedRow> Rows_;
    int Position_ = 0;
};

///////////////////////////////////////////////////////////////////////////////

class TSchemafulMergingReaderTest
    : public TRowMergerTestBase
{
public:
    void ReadAll(ISchemafulReaderPtr reader, std::vector<TUnversionedRow>* result)
    {
        std::vector<TUnversionedRow> partial;
        partial.reserve(1024);

        bool wait;
        do {
            WaitFor(reader->GetReadyEvent());
            wait = reader->Read(&partial);

            for (const auto& row : partial) {
                result->push_back(Buffer_->Capture(row));
            }

        } while (wait || partial.size() > 0);
    }
};

TEST_F(TSchemafulMergingReaderTest, Merge1)
{
    auto readers = std::vector<IVersionedReaderPtr>{
        New<TMockVersionedReader>(std::vector<TVersionedRow>{BuildVersionedRow("0", "<id=1;ts=200> 1")}),
        New<TMockVersionedReader>(std::vector<TVersionedRow>{BuildVersionedRow("0", "<id=1;ts=900> 2")}),
        New<TMockVersionedReader>(std::vector<TVersionedRow>{BuildVersionedRow("0", "<id=1;ts=600> 7")})};

    auto boundaries = std::vector<TUnversionedOwningRow>{
        TUnversionedOwningRow(BuildUnversionedRow("<id=0> 0")),
        TUnversionedOwningRow(BuildUnversionedRow("<id=0> 0")),
        TUnversionedOwningRow(BuildUnversionedRow("<id=0> 0"))};

    auto merger = New<TSchemafulRowMerger>(New<TRowBuffer>(), 4, 1, TColumnFilter());

    auto reader = CreateSchemafulOverlappingChunkReader(
        boundaries,
        std::move(merger),
        [readers] (int index) {
            return readers[index];
        },
        [] (
            const TUnversionedValue* lhsBegin,
            const TUnversionedValue* lhsEnd,
            const TUnversionedValue* rhsBegin,
            const TUnversionedValue* rhsEnd)
        {
            return CompareRows(lhsBegin, lhsEnd, rhsBegin, rhsEnd);
        },
        1);

    std::vector<TUnversionedRow> result;
    ReadAll(reader, &result);

    EXPECT_EQ(1, result.size());
    EXPECT_EQ(BuildUnversionedRow("<id=0> 0; <id=1> 2; <id=2> #; <id=3> #"), result[0]);
}

TEST_F(TSchemafulMergingReaderTest, Merge2)
{
    auto readers = std::vector<IVersionedReaderPtr>{
        New<TMockVersionedReader>(std::vector<TVersionedRow>{
            BuildVersionedRow("0", "<id=1;ts=200> 0"),
            BuildVersionedRow("1", "<id=1;ts=200> 1")}),
        New<TMockVersionedReader>(std::vector<TVersionedRow>{
            BuildVersionedRow("2", "<id=1;ts=100> 2"),
            BuildVersionedRow("3", "<id=1;ts=300> 3")}),
        New<TMockVersionedReader>(std::vector<TVersionedRow>{
            BuildVersionedRow("1", "<id=1;ts=300> 4"),
            BuildVersionedRow("2", "<id=1;ts=600> 5")})};

    auto boundaries = std::vector<TUnversionedOwningRow>{
        TUnversionedOwningRow(BuildUnversionedRow("<id=0> 0")),
        TUnversionedOwningRow(BuildUnversionedRow("<id=0> 2")),
        TUnversionedOwningRow(BuildUnversionedRow("<id=0> 1"))};

    auto merger = New<TSchemafulRowMerger>(New<TRowBuffer>(), 4, 1, TColumnFilter());

    auto reader = CreateSchemafulOverlappingChunkReader(
        boundaries,
        std::move(merger),
        [readers] (int index) {
            return readers[index];
        },
        [] (
            const TUnversionedValue* lhsBegin,
            const TUnversionedValue* lhsEnd,
            const TUnversionedValue* rhsBegin,
            const TUnversionedValue* rhsEnd)
        {
            return CompareRows(lhsBegin, lhsEnd, rhsBegin, rhsEnd);
        },
        1);

    std::vector<TUnversionedRow> result;
    ReadAll(reader, &result);

    EXPECT_EQ(4, result.size());
    EXPECT_EQ(BuildUnversionedRow("<id=0> 0; <id=1> 0; <id=2> #; <id=3> #"), result[0]);
    EXPECT_EQ(BuildUnversionedRow("<id=0> 1; <id=1> 4; <id=2> #; <id=3> #"), result[1]);
    EXPECT_EQ(BuildUnversionedRow("<id=0> 2; <id=1> 5; <id=2> #; <id=3> #"), result[2]);
    EXPECT_EQ(BuildUnversionedRow("<id=0> 3; <id=1> 3; <id=2> #; <id=3> #"), result[3]);
}

TEST_F(TSchemafulMergingReaderTest, Lookup)
{
    auto readers = std::vector<IVersionedReaderPtr>{
        New<TMockVersionedReader>(std::vector<TVersionedRow>{
            BuildVersionedRow("0", "<id=1;ts=200> 0"),
            BuildVersionedRow("1", "<id=1;ts=400> 1")}),
        New<TMockVersionedReader>(std::vector<TVersionedRow>{
            BuildVersionedRow("0", "<id=1;ts=300> 2"),
            BuildVersionedRow("1", "<id=1;ts=300> 3")}),
        New<TMockVersionedReader>(std::vector<TVersionedRow>{
            BuildVersionedRow("0", "<id=1;ts=100> 4"),
            BuildVersionedRow("1", "<id=1;ts=600> 5")})};

    auto merger = New<TSchemafulRowMerger>(New<TRowBuffer>(), 4, 1, TColumnFilter());

    auto reader = CreateSchemafulOverlappingChunkLookupReader(
        std::move(merger),
        [readers, index = 0] () mutable -> IVersionedReaderPtr {
            if (index < readers.size()) {
                return readers[index++];
            } else {
                return nullptr;
            }
        });

    std::vector<TUnversionedRow> result;
    ReadAll(reader, &result);

    EXPECT_EQ(2, result.size());
    EXPECT_EQ(BuildUnversionedRow("<id=0> 0; <id=1> 2; <id=2> #; <id=3> #"), result[0]);
    EXPECT_EQ(BuildUnversionedRow("<id=0> 1; <id=1> 5; <id=2> #; <id=3> #"), result[1]);
}

///////////////////////////////////////////////////////////////////////////////

class TVersionedMergingReaderTest
    : public TRowMergerTestBase
{
public:
    void ReadAll(IVersionedReaderPtr reader, std::vector<TVersionedRow>* result)
    {
        std::vector<TVersionedRow> partial;
        partial.reserve(1024);

        WaitFor(reader->Open());

        bool wait;
        do {
            WaitFor(reader->GetReadyEvent());
            wait = reader->Read(&partial);

            for (const auto& row : partial) {
                Result_.push_back(TVersionedOwningRow(row));
                result->push_back(Result_.back().Get());
            }

        } while (wait || partial.size() > 0);
    }

private:
    std::vector<TVersionedOwningRow> Result_;
};

TEST_F(TVersionedMergingReaderTest, Merge1)
{
    auto readers = std::vector<IVersionedReaderPtr>{
        New<TMockVersionedReader>(std::vector<TVersionedRow>{BuildVersionedRow("0", "<id=1;ts=200> 1")}),
        New<TMockVersionedReader>(std::vector<TVersionedRow>{BuildVersionedRow("0", "<id=1;ts=900> 2")}),
        New<TMockVersionedReader>(std::vector<TVersionedRow>{BuildVersionedRow("0", "<id=1;ts=600> 3")})};

    auto boundaries = std::vector<TUnversionedOwningRow>{
        TUnversionedOwningRow(BuildUnversionedRow("<id=0> 0")),
        TUnversionedOwningRow(BuildUnversionedRow("<id=0> 0")),
        TUnversionedOwningRow(BuildUnversionedRow("<id=0> 0"))};

    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 2;
    auto merger = New<TVersionedRowMerger>(Buffer_, 1, config, SecondsToTimestamp(1000), 0);

    auto reader = CreateVersionedOverlappingChunkReader(
        boundaries,
        std::move(merger),
        [readers] (int index) {
            return readers[index];
        },
        [] (
            const TUnversionedValue* lhsBegin,
            const TUnversionedValue* lhsEnd,
            const TUnversionedValue* rhsBegin,
            const TUnversionedValue* rhsEnd)
        {
            return CompareRows(lhsBegin, lhsEnd, rhsBegin, rhsEnd);
        },
        1);

    std::vector<TVersionedRow> result;
    ReadAll(reader, &result);

    EXPECT_EQ(1, result.size());
    EXPECT_EQ(BuildVersionedRow("0", "<id=1;ts=600> 3; <id=1;ts=900> 2"), result[0]);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NTableClient
} // namespace NYT
