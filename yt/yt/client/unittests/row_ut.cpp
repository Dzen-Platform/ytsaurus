#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/core/misc/protobuf_helpers.h>

#include <limits>

namespace NYT::NTableClient {
namespace {

////////////////////////////////////////////////////////////////////////////////

void CheckSerialize(TUnversionedRow original)
{
    auto serialized = NYT::ToProto<TString>(original);
    auto deserialized =  NYT::FromProto<TUnversionedOwningRow>(serialized);

    ASSERT_EQ(original, deserialized);
}

TEST(TUnversionedRowTest, Serialize1)
{
    TUnversionedOwningRowBuilder builder;
    auto row = builder.FinishRow();
    CheckSerialize(row);
}

TEST(TUnversionedRowTest, Serialize2)
{
    TUnversionedOwningRowBuilder builder;
    builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 0));
    builder.AddValue(MakeUnversionedInt64Value(42, 1));
    builder.AddValue(MakeUnversionedDoubleValue(0.25, 2));
    CheckSerialize(builder.FinishRow());
}

TEST(TUnversionedRowTest, Serialize3)
{
    // TODO(babenko): cannot test Any type at the moment since CompareRowValues does not work
    // for it.
    TUnversionedOwningRowBuilder builder;
    builder.AddValue(MakeUnversionedStringValue("string1", 10));
    builder.AddValue(MakeUnversionedInt64Value(1234, 20));
    builder.AddValue(MakeUnversionedStringValue("string2", 30));
    builder.AddValue(MakeUnversionedDoubleValue(4321.0, 1000));
    builder.AddValue(MakeUnversionedStringValue("", 10000));
    CheckSerialize(builder.FinishRow());
}

TEST(TUnversionedRowTest, Serialize4)
{
    // TODO(babenko): cannot test Any type at the moment since CompareRowValues does not work
    // for it.
    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedStringValue("string1"));
    builder.AddValue(MakeUnversionedStringValue("string2"));
    CheckSerialize(builder.GetRow());
}

TEST(TUnversionedRowTest, Serialize5)
{
    CheckSerialize(TUnversionedRow());
}

TEST(TUnversionedValueTest, CompareNaN)
{
    auto nanValue = MakeUnversionedDoubleValue(std::numeric_limits<double>::quiet_NaN());
    auto doubleValue = MakeUnversionedDoubleValue(3.14);
    static const char* stringValueData = "foo";
    auto stringValue = MakeUnversionedStringValue(stringValueData);
    EXPECT_THROW(CompareRowValues(nanValue, nanValue), std::exception);
    EXPECT_THROW(CompareRowValues(nanValue, doubleValue), std::exception);
    EXPECT_THROW(CompareRowValues(doubleValue, nanValue), std::exception);
    EXPECT_THROW(CompareRowValues(nanValue, stringValue), std::exception);
    EXPECT_THROW(CompareRowValues(stringValue, nanValue), std::exception);
    EXPECT_NO_THROW(CompareRowValues(stringValue, doubleValue));
}

TEST(TUnversionedValueTest, CompareComposite)
{
    auto compositeValue = MakeUnversionedCompositeValue("[]");
    auto stringValue = MakeUnversionedStringValue("foo");
    auto anyValue = MakeUnversionedAnyValue("[]");
    auto nullValue = MakeUnversionedSentinelValue(EValueType::Null);
    EXPECT_THROW_WITH_SUBSTRING(CompareRowValues(compositeValue, stringValue), "Cannot compare values of types");
    EXPECT_THROW_WITH_SUBSTRING(CompareRowValues(stringValue, compositeValue), "Cannot compare values of types");

    EXPECT_THROW_WITH_SUBSTRING(CompareRowValues(compositeValue, anyValue), "Cannot compare values of types");
    EXPECT_THROW_WITH_SUBSTRING(CompareRowValues(anyValue, compositeValue), "Cannot compare values of types");

    EXPECT_TRUE(CompareRowValues(compositeValue, nullValue) > 0);
    EXPECT_TRUE(CompareRowValues(nullValue, compositeValue) < 0);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTableClient
