#include "framework.h"

#include <yt/core/misc/protobuf_helpers.h>

#include <yt/ytlib/table_client/config.h>
#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/row_merger.h>
#include <yt/ytlib/table_client/unversioned_row.h>
#include <yt/ytlib/table_client/versioned_row.h>

#include <yt/ytlib/chunk_client/data_statistics.h>

#include <yt/ytlib/table_client/versioned_row.h>
#include <yt/ytlib/table_client/unversioned_row.h>
#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/row_merger.h>
#include <yt/ytlib/table_client/config.h>
#include <yt/ytlib/table_client/versioned_reader.h>
#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/schemaful_overlapping_chunk_reader.h>

#include <yt/ytlib/query_client/config.h>
#include <yt/ytlib/query_client/column_evaluator.h>
#include <yt/ytlib/query_client/function_registry.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

void PrintTo(TVersionedRow row, ::std::ostream* os)
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
using namespace NQueryClient;
using namespace NChunkClient::NProto;
using namespace NChunkClient;

using ::ToString;

////////////////////////////////////////////////////////////////////////////////

class TRowMergerTestBase
    : public ::testing::Test
{
protected:
    const TRowBufferPtr Buffer_ = New<TRowBuffer>();
    int KeyCount_ = -1;
    TColumnEvaluatorCachePtr ColumnEvaluatorCache_ = New<TColumnEvaluatorCache>(
        New<TColumnEvaluatorCacheConfig>(),
        CreateBuiltinFunctionRegistry());

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
            bool aggregate = value->Attributes().Find<bool>("aggregate").Get(false);
            switch (value->GetType()) {
                case ENodeType::Entity:
                    builder.AddValue(MakeVersionedSentinelValue(EValueType::Null, timestamp, id, aggregate));
                    break;
                case ENodeType::Int64:
                    builder.AddValue(MakeVersionedInt64Value(value->GetValue<i64>(), timestamp, id, aggregate));
                    break;
                case ENodeType::Uint64:
                    builder.AddValue(MakeVersionedUint64Value(value->GetValue<ui64>(), timestamp, id, aggregate));
                    break;
                case ENodeType::Double:
                    builder.AddValue(MakeVersionedDoubleValue(value->GetValue<double>(), timestamp, id, aggregate));
                    break;
                case ENodeType::String:
                    builder.AddValue(MakeVersionedStringValue(value->GetValue<Stroka>(), timestamp, id, aggregate));
                    break;
                default:
                    builder.AddValue(MakeVersionedAnyValue(ConvertToYsonString(value).Data(), timestamp, id, aggregate));
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
            bool aggregate = value->Attributes().Find<bool>("aggregate").Get(false);
            switch (value->GetType()) {
                case ENodeType::Entity:
                    builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id, aggregate));
                    break;
                case ENodeType::Int64:
                    builder.AddValue(MakeUnversionedInt64Value(value->GetValue<i64>(), id, aggregate));
                    break;
                case ENodeType::Uint64:
                    builder.AddValue(MakeUnversionedUint64Value(value->GetValue<ui64>(), id, aggregate));
                    break;
                case ENodeType::Double:
                    builder.AddValue(MakeUnversionedDoubleValue(value->GetValue<double>(), id, aggregate));
                    break;
                case ENodeType::String:
                    builder.AddValue(MakeUnversionedStringValue(value->GetValue<Stroka>(), id, aggregate));
                    break;
                default:
                    builder.AddValue(MakeUnversionedAnyValue(ConvertToYsonString(value).Data(), id, aggregate));
                    break;
            }
        }

        return Buffer_->Capture(builder.GetRow());
    }


    static TTimestamp SecondsToTimestamp(int seconds)
    {
        return TTimestamp(seconds) << TimestampCounterWidth;
    }

    static TTableSchema GetTypicalSchema()
    {
        TTableSchema schema;
        schema.Columns().push_back({ "k", EValueType::Int64 });
        schema.Columns().push_back({ "l", EValueType::Int64 });
        schema.Columns().push_back({ "m", EValueType::Int64 });
        schema.Columns().push_back({ "n", EValueType::Int64 });
        return schema;
    }

    static TTableSchema GetAggregateSumSchema()
    {
        TTableSchema schema;
        schema.Columns().push_back({ "k", EValueType::Int64 });
        schema.Columns().push_back({ "l", EValueType::Int64 });
        schema.Columns().push_back({ "m", EValueType::Int64 });
        schema.Columns().push_back({ "n", EValueType::Int64, Null, Null, Stroka("sum") });
        return schema;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TSchemafulRowMergerTest
    : public TRowMergerTestBase
{
public:
    TSchemafulRowMergerPtr GetTypicalMerger(
        TColumnFilter filter = TColumnFilter(),
        TTableSchema schema = GetTypicalSchema())
    {
        auto evaluator = ColumnEvaluatorCache_->Find(schema, 1);
        return New<TSchemafulRowMerger>(MergedRowBuffer_, 1, filter, evaluator);
    }

protected:
    const TRowBufferPtr MergedRowBuffer_ = New<TRowBuffer>();
};

TEST_F(TSchemafulRowMergerTest, Simple1)
{
    auto merger = GetTypicalMerger();

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=2;ts=200> 3.14"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 2; <id=2> 3.14; <id=3> \"test\""),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Simple2)
{
    auto merger = GetTypicalMerger();

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=300> 3"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 3; <id=2> #; <id=3> #"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Delete1)
{
    auto merger = GetTypicalMerger();

    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));

    EXPECT_EQ(
        TUnversionedRow(),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Delete2)
{
    auto merger = GetTypicalMerger();

    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 1; <id=2;ts=200> 3.14; <id=3;ts=200> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Delete3)
{
    auto merger = GetTypicalMerger();

    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 300 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=200> 1; <id=2;ts=200> 3.14; <id=3;ts=200> \"test\""));

    EXPECT_EQ(
        TUnversionedRow(),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Delete4)
{
    auto merger = GetTypicalMerger();

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
    auto merger = GetTypicalMerger(filter);

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
    auto merger = GetTypicalMerger(filter);

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=2;ts=200> 3.14"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=1> 2; <id=2> 3.14"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Aggregate1)
{
    auto merger = GetTypicalMerger(TColumnFilter(), GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1; ts=100> 1"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> #; <id=3;aggregate=false> #;"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, Aggregate2)
{
    auto merger = GetTypicalMerger(TColumnFilter(), GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=true> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 3"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> #"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> #; <id=3;aggregate=false> 6;"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, DeletedAggregate1)
{
    auto merger = GetTypicalMerger(TColumnFilter(), GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200 }));

    EXPECT_EQ(
        TUnversionedRow(),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, DeletedAggregate2)
{
    auto merger = GetTypicalMerger(TColumnFilter(), GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 100 }));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> #; <id=3;aggregate=false> 1;"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, DeletedAggregate3)
{
    auto merger = GetTypicalMerger(TColumnFilter(), GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 1"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> #; <id=3;aggregate=false> 1;"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, DeletedAggregate4)
{
    auto merger = GetTypicalMerger(TColumnFilter(), GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=400;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200 }));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> #; <id=3;aggregate=false> 2;"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, ResettedAggregate1)
{
    auto merger = GetTypicalMerger(TColumnFilter(), GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 3"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=false> 2"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> #; <id=3;aggregate=false> 5;"),
        merger->BuildMergedRow());
}

TEST_F(TSchemafulRowMergerTest, ResettedAggregate2)
{
    auto merger = GetTypicalMerger(TColumnFilter(), GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=false> #"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 2"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> #; <id=3;aggregate=false> 2;"),
        merger->BuildMergedRow());
}

////////////////////////////////////////////////////////////////////////////////

class TUnversionedRowMergerTest
    : public TRowMergerTestBase
{
public:
    TUnversionedRowMergerPtr GetTypicalMerger(
        TTableSchema schema = GetTypicalSchema())
    {
        auto evaluator = ColumnEvaluatorCache_->Find(schema, 1);
        return New<TUnversionedRowMerger>(MergedRowBuffer_, 1, evaluator);
    }

protected:
    const TRowBufferPtr MergedRowBuffer_ = New<TRowBuffer>();
};

TEST_F(TUnversionedRowMergerTest, Simple1)
{
    auto merger = GetTypicalMerger();

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 2"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=2> 3.14"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 2; <id=2> 3.14; <id=3> \"test\""),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Simple2)
{
    auto merger = GetTypicalMerger();

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 1"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 2"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 3"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 3;"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Delete1)
{
    auto merger = GetTypicalMerger();

    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Delete2)
{
    auto merger = GetTypicalMerger();

    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Delete3)
{
    auto merger = GetTypicalMerger();

    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""));
    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Delete4)
{
    auto merger = GetTypicalMerger();

    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 1; <id=2> 3.14; <id=3> \"test\""));
    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=2> 3.15"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> 3.15; <id=3> #"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Aggregate1)
{
    auto merger = GetTypicalMerger(GetAggregateSumSchema());

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=1> 1"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> 1;"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, Aggregate2)
{
    auto merger = GetTypicalMerger(GetAggregateSumSchema());

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3;aggregate=true> 1"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3;aggregate=true> 2"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3;aggregate=true> 3"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=3;aggregate=true> 6;"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, DeletedAggregate1)
{
    auto merger = GetTypicalMerger(GetAggregateSumSchema());

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3;aggregate=true> 1"));
    merger->DeletePartialRow(BuildUnversionedRow("<id=0> 0"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=2> 3.15"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=1> #; <id=2> 3.15; <id=3;aggregate=false> #"),
        merger->BuildMergedRow());
}

TEST_F(TUnversionedRowMergerTest, ResettedAggregate1)
{
    auto merger = GetTypicalMerger(GetAggregateSumSchema());

    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3;aggregate=true> 1"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3;aggregate=false> 2"));
    merger->AddPartialRow(BuildUnversionedRow("<id=0> 0; <id=3;aggregate=true> 3"));

    EXPECT_EQ(
        BuildUnversionedRow("<id=0> 0; <id=3;aggregate=false> 5"),
        merger->BuildMergedRow());
}

////////////////////////////////////////////////////////////////////////////////

class TVersionedRowMergerTest
    : public TRowMergerTestBase
{
public:
    TVersionedRowMergerPtr GetTypicalMerger(
        TRetentionConfigPtr config,
        TTimestamp currentTimestamp,
        TTimestamp majorTimestamp,
        TTableSchema schema = GetTypicalSchema())
    {
        auto evaluator = ColumnEvaluatorCache_->Find(schema, 1);
        return New<TVersionedRowMerger>(
            MergedRowBuffer_,
            1,
            config,
            currentTimestamp,
            majorTimestamp,
            evaluator);
    }

protected:
    const TRowBufferPtr MergedRowBuffer_ = New<TRowBuffer>();
};

TEST_F(TVersionedRowMergerTest, KeepAll1)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 10;

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), 0);

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));

    EXPECT_EQ(
        BuildVersionedRow("0", "<id=1;ts=100> 1"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepAll2)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 10;

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), SecondsToTimestamp(200));

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), SecondsToTimestamp(201));

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1;ts=100> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200 }));

    EXPECT_FALSE(merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, KeepLatest5)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 3;
    config->MaxDataVersions = 3;

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), SecondsToTimestamp(400));

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000000), SecondsToTimestamp(150));

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1101), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1102), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1100), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1100), 0);

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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1100), 0);

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

TEST_F(TVersionedRowMergerTest, Aggregate1)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(300),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=1; ts=100> 1"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=100> 1"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, Aggregate2)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(100),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=true> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 10"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=100;aggregate=true> 1; <id=3;ts=200;aggregate=true> 2; <id=3;ts=300;aggregate=true> 10"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, Aggregate3)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(200),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=true> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 10"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=100;aggregate=false> 1; <id=3;ts=200;aggregate=true> 2; <id=3;ts=300;aggregate=true> 10"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, Aggregate4)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(300),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=true> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 10"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=200;aggregate=false> 3; <id=3;ts=300;aggregate=true> 10"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, Aggregate5)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(400),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=true> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 10"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=300;aggregate=false> 13"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, DeletedAggregate1)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(200),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 300 }));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=100;aggregate=false> 1",
            { 300 }),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, DeletedAggregate2)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(300),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200 }));

    EXPECT_EQ(
        TVersionedRow(),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, DeletedAggregate3)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(500),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200, 400 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=500;aggregate=true> 3"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=500;aggregate=true> 3"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, DeletedAggregate4)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(500),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 100, 300 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=400;aggregate=true> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=500;aggregate=true> 3"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=400;aggregate=false> 2; <id=3;ts=500;aggregate=true> 3"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, DeletedAggregate5)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(500),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200, 600 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 2; <id=3;ts=400;aggregate=true> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=500;aggregate=true> 3"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=400;aggregate=false> 4; <id=3;ts=500;aggregate=true> 3",
            { 600 }),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, DeletedAggregate6)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(200),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 100, 600 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=500;aggregate=true> 3"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=200;aggregate=true> 1; <id=3;ts=500;aggregate=true> 3",
            { 600 }),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, ResettedAggregate1)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(300),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=false> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=200;aggregate=false> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=false> 10"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=200;aggregate=false> 2; <id=3;ts=300;aggregate=false> 10"),
        merger->BuildMergedRow());
}

TEST_F(TVersionedRowMergerTest, ResettedAggregate2)
{
    auto config = New<TRetentionConfig>();
    config->MinDataVersions = 1;

    auto merger = GetTypicalMerger(
        config,
        SecondsToTimestamp(1000),
        SecondsToTimestamp(500),
        GetAggregateSumSchema());

    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=100;aggregate=true> 1"));
    merger->AddPartialRow(BuildVersionedRow("0", "", { 200, 600 }));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=300;aggregate=true> 2; <id=3;ts=400;aggregate=false> 2"));
    merger->AddPartialRow(BuildVersionedRow("0", "<id=3;ts=500;aggregate=false> 3"));

    EXPECT_EQ(
        BuildVersionedRow(
            "0",
            "<id=3;ts=400;aggregate=false> 2; <id=3;ts=500;aggregate=false> 3",
            { 600 }),
        merger->BuildMergedRow());
}

///////////////////////////////////////////////////////////////////////////////

class TMockVersionedReader
    : public IVersionedReader
{
public:
    explicit TMockVersionedReader(std::vector<TVersionedRow> rows)
        : Rows_(std::move(rows))
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

    virtual TDataStatistics GetDataStatistics() const override
    {
        return TDataStatistics();
    }

    virtual bool IsFetchingCompleted() const override
    {
        return true;
    }

    virtual std::vector<TChunkId> GetFailedChunkIds() const override
    {
        return std::vector<TChunkId>();
    }

private:
    std::vector<TVersionedRow> Rows_;
    int Position_ = 0;
};

///////////////////////////////////////////////////////////////////////////////

class TSchemafulMergingReaderTest
    : public TSchemafulRowMergerTest
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

    auto merger = GetTypicalMerger();

    auto reader = CreateSchemafulOverlappingRangeChunkReader(
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

    auto merger = GetTypicalMerger();

    auto reader = CreateSchemafulOverlappingRangeChunkReader(
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

    auto merger = GetTypicalMerger();

    auto reader = CreateSchemafulOverlappingLookupChunkReader(
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
    : public TVersionedRowMergerTest
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

    auto merger = GetTypicalMerger(config, SecondsToTimestamp(1000), 0);

    auto reader = CreateVersionedOverlappingRangeChunkReader(
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
