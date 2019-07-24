#include "row_helpers.h"

#include <yt/client/unittests/protobuf_format_ut.pb.h>

#include <yt/core/test_framework/framework.h>

#include <yt/core/yson/string.h>
#include <yt/core/ytree/fluent.h>

#include <yt/client/formats/config.h>
#include <yt/client/formats/parser.h>
#include <yt/client/formats/lenval_control_constants.h>
#include <yt/client/formats/protobuf_writer.h>
#include <yt/client/formats/protobuf_parser.h>
#include <yt/client/formats/protobuf.h>
#include <yt/client/formats/format.h>
#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/value_consumer.h>
#include <yt/client/table_client/unversioned_row.h>

#include <yt/core/concurrency/async_stream.h>

#include <util/random/fast.h>


namespace NYT {
namespace {

using namespace NYson;
using namespace NYTree;
using namespace NFormats;
using namespace NTableClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

#define EXPECT_THROW_WITH_SUBSTRING(expr, substr) \
    try { \
        expr; \
        ADD_FAILURE(); \
    } catch (const TErrorException& e) { \
        EXPECT_THAT(e.what(), testing::HasSubstr(substr)); \
    }

////////////////////////////////////////////////////////////////////////////////

// Hardcoded serialization of file descriptor used in old format description.
TString FileDescriptor = "\x0a\xb6\x03\x0a\x29\x6a\x75\x6e\x6b\x2f\x65\x72\x6d\x6f\x6c\x6f\x76\x64\x2f\x74\x65\x73\x74\x2d\x70\x72\x6f\x74\x6f\x62"
    "\x75\x66\x2f\x6d\x65\x73\x73\x61\x67\x65\x2e\x70\x72\x6f\x74\x6f\x22\x2d\x0a\x0f\x54\x45\x6d\x62\x65\x64\x65\x64\x4d\x65\x73\x73\x61\x67\x65\x12"
    "\x0b\x0a\x03\x4b\x65\x79\x18\x01\x20\x01\x28\x09\x12\x0d\x0a\x05\x56\x61\x6c\x75\x65\x18\x02\x20\x01\x28\x09\x22\xb3\x02\x0a\x08\x54\x4d\x65\x73"
    "\x73\x61\x67\x65\x12\x0e\x0a\x06\x44\x6f\x75\x62\x6c\x65\x18\x01\x20\x01\x28\x01\x12\x0d\x0a\x05\x46\x6c\x6f\x61\x74\x18\x02\x20\x01\x28\x02\x12"
    "\x0d\x0a\x05\x49\x6e\x74\x36\x34\x18\x03\x20\x01\x28\x03\x12\x0e\x0a\x06\x55\x49\x6e\x74\x36\x34\x18\x04\x20\x01\x28\x04\x12\x0e\x0a\x06\x53\x49"
    "\x6e\x74\x36\x34\x18\x05\x20\x01\x28\x12\x12\x0f\x0a\x07\x46\x69\x78\x65\x64\x36\x34\x18\x06\x20\x01\x28\x06\x12\x10\x0a\x08\x53\x46\x69\x78\x65"
    "\x64\x36\x34\x18\x07\x20\x01\x28\x10\x12\x0d\x0a\x05\x49\x6e\x74\x33\x32\x18\x08\x20\x01\x28\x05\x12\x0e\x0a\x06\x55\x49\x6e\x74\x33\x32\x18\x09"
    "\x20\x01\x28\x0d\x12\x0e\x0a\x06\x53\x49\x6e\x74\x33\x32\x18\x0a\x20\x01\x28\x11\x12\x0f\x0a\x07\x46\x69\x78\x65\x64\x33\x32\x18\x0b\x20\x01\x28"
    "\x07\x12\x10\x0a\x08\x53\x46\x69\x78\x65\x64\x33\x32\x18\x0c\x20\x01\x28\x0f\x12\x0c\x0a\x04\x42\x6f\x6f\x6c\x18\x0d\x20\x01\x28\x08\x12\x0e\x0a"
    "\x06\x53\x74\x72\x69\x6e\x67\x18\x0e\x20\x01\x28\x09\x12\x0d\x0a\x05\x42\x79\x74\x65\x73\x18\x0f\x20\x01\x28\x0c\x12\x14\x0a\x04\x45\x6e\x75\x6d"
    "\x18\x10\x20\x01\x28\x0e\x32\x06\x2e\x45\x45\x6e\x75\x6d\x12\x21\x0a\x07\x4d\x65\x73\x73\x61\x67\x65\x18\x11\x20\x01\x28\x0b\x32\x10\x2e\x54\x45"
    "\x6d\x62\x65\x64\x65\x64\x4d\x65\x73\x73\x61\x67\x65\x2a\x24\x0a\x05\x45\x45\x6e\x75\x6d\x12\x07\x0a\x03\x4f\x6e\x65\x10\x01\x12\x07\x0a\x03\x54"
    "\x77\x6f\x10\x02\x12\x09\x0a\x05\x54\x68\x72\x65\x65\x10\x03";

TString GenerateRandomLenvalString(TFastRng64& rng, ui32 size)
{
    TString result;
    result.append(reinterpret_cast<const char*>(&size), sizeof(size));

    size += sizeof(ui32);

    while (result.size() < size) {
        ui64 num = rng.GenRand();
        result.append(reinterpret_cast<const char*>(&num), sizeof(num));
    }
    if (result.size() > size) {
        result.resize(size);
    }
    return result;
}

INodePtr ParseYson(TStringBuf data)
{
    return ConvertToNode(NYson::TYsonString(data.ToString()));
}

TProtobufFormatConfigPtr ParseFormatConfigFromNode(const INodePtr& configNode)
{
    auto config = New<NFormats::TProtobufFormatConfig>();
    config->Load(configNode);
    return config;
};

TProtobufFormatConfigPtr ParseFormatConfigFromString(TStringBuf configStr)
{
    return ParseFormatConfigFromNode(ParseYson(configStr));
}

TUnversionedOwningRow MakeRow(const std::initializer_list<TUnversionedValue>& rows)
{
    TUnversionedOwningRowBuilder builder;
    for (const auto& r : rows) {
        builder.AddValue(r);
    }

    return builder.FinishRow();
}

TString LenvalBytes(const ::google::protobuf::Message& message)
{
    TStringStream out;
    ui32 messageSize = message.ByteSize();
    out.Write(&messageSize, sizeof(messageSize));
    if (!message.SerializeToStream(&out)) {
        THROW_ERROR_EXCEPTION("Can not serialize message");
    }
    return out.Str();
}

void EnsureTypesMatch(EValueType expected, EValueType actual)
{
    if (expected != actual) {
        THROW_ERROR_EXCEPTION("Expected: %v actual: %v", expected, actual);
    }
}

double GetDouble(const TUnversionedValue& row)
{
    EnsureTypesMatch(EValueType::Double, row.Type);
    return row.Data.Double;
}

INodePtr CreateAllFieldsFileDescriptorConfig()
{
    return BuildYsonNodeFluently()
        .BeginAttributes()
            .Item("file_descriptor_set")
            .Value(FileDescriptor)
            .Item("file_indices")
            .BeginList()
                .Item().Value(0)
            .EndList()
            .Item("message_indices")
            .BeginList()
                .Item().Value(1)
            .EndList()
        .EndAttributes()
        .Value("protobuf");
}

INodePtr CreateAllFieldsSchemaConfig()
{
    return BuildYsonNodeFluently()
        .BeginAttributes()
            .Item("enumerations")
            .BeginMap()
                .Item("EEnum")
                .BeginMap()
                    .Item("One").Value(1)
                    .Item("Two").Value(2)
                    .Item("Three").Value(3)
                    .Item("MinusFortyTwo").Value(-42)
                .EndMap()
            .EndMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("Double")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("double")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("Float")
                            .Item("field_number").Value(2)
                            .Item("proto_type").Value("float")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("Int64")
                            .Item("field_number").Value(3)
                            .Item("proto_type").Value("int64")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("UInt64")
                            .Item("field_number").Value(4)
                            .Item("proto_type").Value("uint64")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("SInt64")
                            .Item("field_number").Value(5)
                            .Item("proto_type").Value("sint64")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("Fixed64")
                            .Item("field_number").Value(6)
                            .Item("proto_type").Value("fixed64")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("SFixed64")
                            .Item("field_number").Value(7)
                            .Item("proto_type").Value("sfixed64")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("Int32")
                            .Item("field_number").Value(8)
                            .Item("proto_type").Value("int32")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("UInt32")
                            .Item("field_number").Value(9)
                            .Item("proto_type").Value("uint32")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("SInt32")
                            .Item("field_number").Value(10)
                            .Item("proto_type").Value("sint32")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("Fixed32")
                            .Item("field_number").Value(11)
                            .Item("proto_type").Value("fixed32")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("SFixed32")
                            .Item("field_number").Value(12)
                            .Item("proto_type").Value("sfixed32")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("Bool")
                            .Item("field_number").Value(13)
                            .Item("proto_type").Value("bool")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("String")
                            .Item("field_number").Value(14)
                            .Item("proto_type").Value("string")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("Bytes")
                            .Item("field_number").Value(15)
                            .Item("proto_type").Value("bytes")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("Enum")
                            .Item("field_number").Value(16)
                            .Item("proto_type").Value("enum_string")
                            .Item("enumeration_name").Value("EEnum")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("Message")
                            .Item("field_number").Value(17)
                            .Item("proto_type").Value("message")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("AnyWithMap")
                            .Item("field_number").Value(18)
                            .Item("proto_type").Value("any")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("AnyWithInt64")
                            .Item("field_number").Value(19)
                            .Item("proto_type").Value("any")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("AnyWithString")
                            .Item("field_number").Value(20)
                            .Item("proto_type").Value("any")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("OtherColumns")
                            .Item("field_number").Value(21)
                            .Item("proto_type").Value("other_columns")
                        .EndMap()

                        .Item()
                        .BeginMap()
                            .Item("name").Value("MissingInt64")
                            .Item("field_number").Value(22)
                            .Item("proto_type").Value("int64")
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndAttributes()
        .Value("protobuf");
}

////////////////////////////////////////////////////////////////////////////////

struct TLenvalEntry
{
    TString RowData;
    ui32 TableIndex;
};

////////////////////////////////////////////////////////////////////////////////

class TLenvalParser
{
public:
    explicit TLenvalParser(IInputStream* input)
        : Input_(input)
    { }

    std::optional<TLenvalEntry> Next()
    {
        ui32 rowSize;
        size_t read = Input_->Load(&rowSize, sizeof(rowSize));
        if (read == 0) {
            return std::nullopt;
        } else if (read < sizeof(rowSize)) {
            THROW_ERROR_EXCEPTION("corrupted lenval: can't read row length");
        }
        if (rowSize == LenvalTableIndexMarker) {
            ui32 tableIndex;
            read = Input_->Load(&tableIndex, sizeof(tableIndex));
            if (read != sizeof(tableIndex)) {
                THROW_ERROR_EXCEPTION("corrupted lenval: can't read table index");
            }
            CurrentTableIndex_ = tableIndex;
            return Next();
        } else if (
            rowSize == LenvalKeySwitch ||
            rowSize == LenvalRangeIndexMarker ||
            rowSize == LenvalRowIndexMarker)
        {
            THROW_ERROR_EXCEPTION("marker is unsupported");
        } else {
            TLenvalEntry result;
            result.RowData.resize(rowSize);
            result.TableIndex = CurrentTableIndex_;
            Input_->Load(result.RowData.Detach(), rowSize);

            return result;
        }
    }
private:
    IInputStream* Input_;
    ui32 CurrentTableIndex_ = 0;
};

////////////////////////////////////////////////////////////////////////////////

TEST(TProtobufFormat, TestConfigParsing)
{
    auto parseAndValidateConfig = [] (const auto& node) {
        auto config = ParseFormatConfigFromNode(node);
        std::vector<TTableSchema> schemas(config->Tables.size());
        New<TProtobufFormatDescription>()->Init(config, schemas, false);
        return config;
    };
    // Empty config.
    EXPECT_THROW_WITH_SUBSTRING(
        parseAndValidateConfig(ParseYson("{}")),
        "\"tables\" attribute is not specified in protobuf format");

    // Broken protobuf.
    EXPECT_THROW_WITH_SUBSTRING(
        parseAndValidateConfig(ParseYson(R"({file_descriptor_set="dfgxx"; file_indices=[0]; message_indices=[0]})")),
        "Error parsing \"file_descriptor_set\" in protobuf config");

    EXPECT_NO_THROW(parseAndValidateConfig(
        CreateAllFieldsFileDescriptorConfig()->Attributes().ToMap()
    ));

    EXPECT_NO_THROW(parseAndValidateConfig(
        CreateAllFieldsSchemaConfig()->Attributes().ToMap()
    ));

    auto multipleOtherColumnsConfig = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("Other1")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("other_columns")
                        .EndMap()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("Other2")
                            .Item("field_number").Value(2)
                            .Item("proto_type").Value("other_columns")
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    EXPECT_THROW_WITH_SUBSTRING(
        parseAndValidateConfig(multipleOtherColumnsConfig),
        "Multiple \"other_columns\" in protobuf config are not allowed");

    auto duplicateColumnNamesConfig = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("SomeColumn")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("int64")
                        .EndMap()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("SomeColumn")
                            .Item("field_number").Value(2)
                            .Item("proto_type").Value("string")
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    EXPECT_THROW_WITH_SUBSTRING(
        parseAndValidateConfig(duplicateColumnNamesConfig),
        "Multiple fields with same column name (\"SomeColumn\") are forbidden in protobuf format");
}

TEST(TProtobufFormat, TestParseBigZigZag)
{
    constexpr i32 value = Min<i32>();

    TCollectingValueConsumer rowCollector;

    auto parser = CreateParserForProtobuf(
        &rowCollector,
        ParseFormatConfigFromNode(CreateAllFieldsSchemaConfig()->Attributes().ToMap()),
        0);
    NProtobufFormatTest::TMessage message;
    message.set_int32_field(value);
    parser->Read(LenvalBytes(message));
    parser->Finish();

    EXPECT_EQ(GetInt64(rowCollector.GetRowValue(0, "Int32")), value);
}

TEST(TProtobufFormat, TestParseEnumerationString)
{
    TCollectingValueConsumer rowCollector;

    auto parser = CreateParserForProtobuf(
        &rowCollector,
        ParseFormatConfigFromNode(CreateAllFieldsSchemaConfig()->Attributes().ToMap()),
        0);

    {
        NProtobufFormatTest::TMessage message;
        message.set_enum_field(NProtobufFormatTest::EEnum::one);
        parser->Read(LenvalBytes(message));
    }
    {
        NProtobufFormatTest::TMessage message;
        message.set_enum_field(NProtobufFormatTest::EEnum::two);
        parser->Read(LenvalBytes(message));
    }
    {
        NProtobufFormatTest::TMessage message;
        message.set_enum_field(NProtobufFormatTest::EEnum::three);
        parser->Read(LenvalBytes(message));
    }
    {
        NProtobufFormatTest::TMessage message;
        message.set_enum_field(NProtobufFormatTest::EEnum::minus_forty_two);
        parser->Read(LenvalBytes(message));
    }

    parser->Finish();

    EXPECT_EQ(GetString(rowCollector.GetRowValue(0, "Enum")), "One");
    EXPECT_EQ(GetString(rowCollector.GetRowValue(1, "Enum")), "Two");
    EXPECT_EQ(GetString(rowCollector.GetRowValue(2, "Enum")), "Three");
    EXPECT_EQ(GetString(rowCollector.GetRowValue(3, "Enum")), "MinusFortyTwo");
}

TEST(TProtobufFormat, TestParseWrongEnumeration)
{
    TCollectingValueConsumer rowCollector;

    auto parser = CreateParserForProtobuf(
        &rowCollector,
        ParseFormatConfigFromNode(CreateAllFieldsSchemaConfig()->Attributes().ToMap()),
        0);

        NProtobufFormatTest::TMessage message;
        auto enumTag = NProtobufFormatTest::TMessage::descriptor()->FindFieldByName("enum_field")->number();
        message.mutable_unknown_fields()->AddVarint(enumTag, 30);

    auto feedParser = [&] {
        parser->Read(LenvalBytes(message));
        parser->Finish();
    };

    EXPECT_ANY_THROW(feedParser());
}

TEST(TProtobufFormat, TestParseEnumerationInt)
{
    TCollectingValueConsumer rowCollector;

    auto config = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("Enum")
                            .Item("field_number").Value(16)
                            .Item("proto_type").Value("enum_int")
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    auto parser = CreateParserForProtobuf(&rowCollector, ParseFormatConfigFromNode(config), 0);

    {
        NProtobufFormatTest::TMessage message;
        message.set_enum_field(NProtobufFormatTest::EEnum::one);
        parser->Read(LenvalBytes(message));
    }
    {
        NProtobufFormatTest::TMessage message;
        message.set_enum_field(NProtobufFormatTest::EEnum::two);
        parser->Read(LenvalBytes(message));
    }
    {
        NProtobufFormatTest::TMessage message;
        message.set_enum_field(NProtobufFormatTest::EEnum::three);
        parser->Read(LenvalBytes(message));
    }
    {
        NProtobufFormatTest::TMessage message;
        message.set_enum_field(NProtobufFormatTest::EEnum::minus_forty_two);
        parser->Read(LenvalBytes(message));
    }
    {
        NProtobufFormatTest::TMessage message;
        auto enumTag = NProtobufFormatTest::TMessage::descriptor()->FindFieldByName("enum_field")->number();
        message.mutable_unknown_fields()->AddVarint(enumTag, 100500);
        parser->Read(LenvalBytes(message));
    }

    parser->Finish();

    EXPECT_EQ(GetInt64(rowCollector.GetRowValue(0, "Enum")), 1);
    EXPECT_EQ(GetInt64(rowCollector.GetRowValue(1, "Enum")), 2);
    EXPECT_EQ(GetInt64(rowCollector.GetRowValue(2, "Enum")), 3);
    EXPECT_EQ(GetInt64(rowCollector.GetRowValue(3, "Enum")), -42);
    EXPECT_EQ(GetInt64(rowCollector.GetRowValue(4, "Enum")), 100500);
}

TEST(TProtobufFormat, TestParseRandomGarbage)
{
    // Check that we never crash.

    TFastRng64 rng(42);
    for (int i = 0; i != 1000; ++i) {
        auto bytes = GenerateRandomLenvalString(rng, 8);

        TCollectingValueConsumer rowCollector;
        auto parser = CreateParserForProtobuf(
            &rowCollector,
            ParseFormatConfigFromNode(CreateAllFieldsSchemaConfig()->Attributes().ToMap()),
            0);
        try {
            parser->Read(bytes);
            parser->Finish();
        } catch (...) {
        }
    }
}

TEST(TProtobufFormat, TestParseZeroColumns)
{
    auto config = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    TCollectingValueConsumer rowCollector;
    auto parser = CreateParserForProtobuf(
        &rowCollector,
        ParseFormatConfigFromNode(config),
        0);

    // Empty lenval values.
    parser->Read(AsStringBuf("\0\0\0\0"));
    parser->Read(AsStringBuf("\0\0\0\0"));

    parser->Finish();

    ASSERT_EQ(rowCollector.Size(), 2);
    EXPECT_EQ(rowCollector.GetRow(0).GetCount(), 0);
    EXPECT_EQ(rowCollector.GetRow(1).GetCount(), 0);
}

TEST(TProtobufFormat, TestWriteEnumerationString)
{
    auto config = CreateAllFieldsSchemaConfig();

    auto nameTable = New<TNameTable>();
    auto enumId = nameTable->RegisterName("Enum");

    TString result;
    TStringOutput resultStream(result);
    auto writer = CreateWriterForProtobuf(
        config->Attributes(),
        {TTableSchema()},
        nameTable,
        CreateAsyncAdapter(&resultStream),
        true,
        New<TControlAttributesConfig>(),
        0);

    writer->Write({
        MakeRow({
            MakeUnversionedStringValue("MinusFortyTwo", enumId),
        }).Get()
    });
    writer->Write({
        MakeRow({
            MakeUnversionedStringValue("Three", enumId),
        }).Get()
    });

    writer->Close()
        .Get()
        .ThrowOnError();

    TStringInput si(result);
    TLenvalParser parser(&si);
    {
        auto row = parser.Next();
        ASSERT_TRUE(row);
        NYT::NProtobufFormatTest::TMessage message;
        ASSERT_TRUE(message.ParseFromString(row->RowData));
        ASSERT_EQ(message.enum_field(), NYT::NProtobufFormatTest::EEnum::minus_forty_two);
    }
    {
        auto row = parser.Next();
        ASSERT_TRUE(row);
        NYT::NProtobufFormatTest::TMessage message;
        ASSERT_TRUE(message.ParseFromString(row->RowData));
        ASSERT_EQ(message.enum_field(), NYT::NProtobufFormatTest::EEnum::three);
    }
    {
        auto row = parser.Next();
        ASSERT_FALSE(row);
    }
}

TEST(TProtobufFormat, TestWriteEnumerationInt)
{
    auto config = BuildYsonNodeFluently()
        .BeginAttributes()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("Enum")
                            .Item("field_number").Value(16)
                            .Item("proto_type").Value("enum_int")
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndAttributes()
        .Value("protobuf");

    auto nameTable = New<TNameTable>();
    auto enumId = nameTable->RegisterName("Enum");

    auto writeAndParseRow = [&] (TUnversionedRow row, NProtobufFormatTest::TMessage* message) {
        TString result;
        TStringOutput resultStream(result);
        auto writer = CreateWriterForProtobuf(
            config->Attributes(),
            {TTableSchema()},
            nameTable,
            CreateAsyncAdapter(&resultStream),
            true,
            New<TControlAttributesConfig>(),
            0);
        writer->Write({row});
        writer->Close()
            .Get()
            .ThrowOnError();

        TStringInput si(result);
        TLenvalParser parser(&si);
        auto protoRow = parser.Next();
        ASSERT_TRUE(protoRow);

        ASSERT_TRUE(message->ParseFromString(protoRow->RowData));

        auto nextProtoRow = parser.Next();
        ASSERT_FALSE(nextProtoRow);
    };

    {
        NProtobufFormatTest::TMessage message;
        writeAndParseRow(
            MakeRow({
                MakeUnversionedInt64Value(-42, enumId),
            }).Get(),
            &message);
        ASSERT_EQ(message.enum_field(), NProtobufFormatTest::EEnum::minus_forty_two);
    }
    {
        NProtobufFormatTest::TMessage message;
        writeAndParseRow(
            MakeRow({
                MakeUnversionedInt64Value(std::numeric_limits<i32>::max(), enumId),
            }).Get(),
            &message);
        ASSERT_EQ(message.enum_field(), NProtobufFormatTest::EEnum::max_int32);
    }
    {
        NProtobufFormatTest::TMessage message;
        writeAndParseRow(
            MakeRow({
                MakeUnversionedUint64Value(std::numeric_limits<i32>::max(), enumId),
            }).Get(),
            &message);
        ASSERT_EQ(message.enum_field(), NProtobufFormatTest::EEnum::max_int32);
    }
    {
        NProtobufFormatTest::TMessage message;
        writeAndParseRow(
            MakeRow({
                MakeUnversionedInt64Value(std::numeric_limits<i32>::min(), enumId),
            }).Get(),
            &message);
        ASSERT_EQ(message.enum_field(), NProtobufFormatTest::EEnum::min_int32);
    }

    NProtobufFormatTest::TMessage message;
    ASSERT_THROW(
        writeAndParseRow(
            MakeRow({
                MakeUnversionedInt64Value(static_cast<i64>(std::numeric_limits<i32>::max()) + 1, enumId),
            }).Get(),
            &message),
        TErrorException);

    ASSERT_THROW(
        writeAndParseRow(
            MakeRow({
                MakeUnversionedInt64Value(static_cast<i64>(std::numeric_limits<i32>::min()) - 1, enumId),
            }).Get(),
            &message),
        TErrorException);

    ASSERT_THROW(
        writeAndParseRow(
            MakeRow({
                MakeUnversionedUint64Value(static_cast<i64>(std::numeric_limits<i32>::max()) + 1, enumId),
            }).Get(),
            &message),
        TErrorException);
}


TEST(TProtobufFormat, TestWriteZeroColumns)
{
    auto config = BuildYsonNodeFluently()
        .BeginAttributes()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                    .EndList()
                .EndMap()
            .EndList()
        .EndAttributes()
        .Value("protobuf");

    auto nameTable = New<TNameTable>();
    auto int64Id = nameTable->RegisterName("Int64");
    auto stringId = nameTable->RegisterName("String");

    TString result;
    TStringOutput resultStream(result);
    auto writer = CreateWriterForProtobuf(
        config->Attributes(),
        {TTableSchema()},
        nameTable,
        CreateAsyncAdapter(&resultStream),
        true,
        New<TControlAttributesConfig>(),
        0);

    writer->Write({
        MakeRow({
            MakeUnversionedInt64Value(-1, int64Id),
            MakeUnversionedStringValue("this_is_string", stringId),
        }).Get()
    });
    writer->Write({MakeRow({ }).Get()});

    writer->Close()
        .Get()
        .ThrowOnError();

    ASSERT_EQ(result, AsStringBuf("\0\0\0\0\0\0\0\0"));
}

TEST(TProtobufFormat, TestContext)
{
    auto config = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    TCollectingValueConsumer rowCollector;
    auto parser = CreateParserForProtobuf(
        &rowCollector,
        ParseFormatConfigFromNode(config),
        0);

    TString context;
    try {
        NProtobufFormatTest::TMessage message;
        message.set_string_field("PYSHCH-PYSHCH");
        parser->Read(LenvalBytes(message));
        parser->Finish();
        GTEST_FATAL_FAILURE_("expected to throw");
    } catch (const NYT::TErrorException& e) {
        context = *e.Error().Attributes().Find<TString>("context");
    }
    ASSERT_NE(context.find("PYSHCH-PYSHCH"), TString::npos);
}

////////////////////////////////////////////////////////////////////////////////

std::pair<TTableSchema, INodePtr> ScreateSchemaAndConfigWithStructuredMessage()
{
    TTableSchema schema({
        {"first", StructLogicalType({
            {"field_missing_from_proto1", SimpleLogicalType(ESimpleLogicalValueType::Int32, false)},
            {"enum_field", SimpleLogicalType(ESimpleLogicalValueType::String, true)},
            {"int64_field", SimpleLogicalType(ESimpleLogicalValueType::Int64, true)},
            {"int64_list", ListLogicalType(SimpleLogicalType(ESimpleLogicalValueType::Int64, true))},
            {"message_field", StructLogicalType({
                {"key", SimpleLogicalType(ESimpleLogicalValueType::String, false)},
                {"value", SimpleLogicalType(ESimpleLogicalValueType::String, false)},
            })},
            {"repeated_message_field", ListLogicalType(StructLogicalType({
                {"key", SimpleLogicalType(ESimpleLogicalValueType::String, false)},
                {"value", SimpleLogicalType(ESimpleLogicalValueType::String, false)},
            }))},
            {"any_int64_field", SimpleLogicalType(ESimpleLogicalValueType::Int64, false)},
            {"any_map_field", SimpleLogicalType(ESimpleLogicalValueType::Any, false)},
            {"optional_int64_field", SimpleLogicalType(ESimpleLogicalValueType::Int64, false)},
            {"field_missing_from_proto2", SimpleLogicalType(ESimpleLogicalValueType::Int32, false)},
        })},
        {"repeated_int64_field", ListLogicalType(SimpleLogicalType(ESimpleLogicalValueType::Int64, true))},
        {"repeated_message_field", ListLogicalType(StructLogicalType({
            {"key", SimpleLogicalType(ESimpleLogicalValueType::String, false)},
            {"value", SimpleLogicalType(ESimpleLogicalValueType::String, false)},
        }))},
        {"second", StructLogicalType({
            {"one", SimpleLogicalType(ESimpleLogicalValueType::Int64, false)},
            {"two", SimpleLogicalType(ESimpleLogicalValueType::Int64, false)},
            {"three", SimpleLogicalType(ESimpleLogicalValueType::Int64, false)},
        })},
        {"any_field", SimpleLogicalType(ESimpleLogicalValueType::Any, true)},
    });

    auto config = BuildYsonNodeFluently()
        .BeginAttributes()
            .Item("enumerations")
            .BeginMap()
                .Item("EEnum")
                .BeginMap()
                    .Item("One").Value(1)
                    .Item("Two").Value(2)
                    .Item("Three").Value(3)
                    .Item("MinusFortyTwo").Value(-42)
                .EndMap()
            .EndMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("first")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("structured_message")
                            .Item("fields")
                            .BeginList()
                                .Item()
                                .BeginMap()
                                    .Item("name").Value("int64_field")
                                    .Item("field_number").Value(2)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                                .Item()
                                .BeginMap()
                                    .Item("name").Value("enum_field")
                                    .Item("field_number").Value(1)
                                    .Item("proto_type").Value("enum_string")
                                    .Item("enumeration_name").Value("EEnum")
                                .EndMap()
                                .Item().BeginMap()
                                    .Item("name").Value("message_field")
                                    .Item("field_number").Value(4)
                                    .Item("proto_type").Value("structured_message")
                                    .Item("fields")
                                    .BeginList()
                                        .Item().BeginMap()
                                            .Item("name").Value("key")
                                            .Item("field_number").Value(1)
                                            .Item("proto_type").Value("string")
                                        .EndMap()
                                        .Item().BeginMap()
                                            .Item("name").Value("value")
                                            .Item("field_number").Value(2)
                                            .Item("proto_type").Value("string")
                                        .EndMap()
                                    .EndList()
                                .EndMap()
                                .Item().BeginMap()
                                    .Item("name").Value("int64_list")
                                    .Item("field_number").Value(3)
                                    .Item("proto_type").Value("int64")
                                    .Item("repeated").Value(true)
                                .EndMap()
                                .Item().BeginMap()
                                    .Item("name").Value("repeated_message_field")
                                    .Item("field_number").Value(5)
                                    .Item("proto_type").Value("structured_message")
                                    .Item("repeated").Value(true)
                                    .Item("fields")
                                    .BeginList()
                                        .Item().BeginMap()
                                            .Item("name").Value("key")
                                            .Item("field_number").Value(1)
                                            .Item("proto_type").Value("string")
                                        .EndMap()
                                        .Item().BeginMap()
                                            .Item("name").Value("value")
                                            .Item("field_number").Value(2)
                                            .Item("proto_type").Value("string")
                                        .EndMap()
                                    .EndList()
                                .EndMap()
                                .Item()
                                .BeginMap()
                                    .Item("name").Value("any_int64_field")
                                    .Item("field_number").Value(6)
                                    .Item("proto_type").Value("any")
                                .EndMap()
                                .Item()
                                .BeginMap()
                                    .Item("name").Value("any_map_field")
                                    .Item("field_number").Value(7)
                                    .Item("proto_type").Value("any")
                                .EndMap()
                                .Item()
                                .BeginMap()
                                    .Item("name").Value("optional_int64_field")
                                    .Item("field_number").Value(8)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                            .EndList()
                        .EndMap()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("second")
                            .Item("field_number").Value(2)
                            .Item("proto_type").Value("structured_message")
                            .Item("fields")
                            .BeginList()
                                .Item().BeginMap()
                                    .Item("name").Value("one")
                                    .Item("field_number").Value(2)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                                .Item().BeginMap()
                                    .Item("name").Value("two")
                                    .Item("field_number").Value(500000000)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                                .Item().BeginMap()
                                    .Item("name").Value("three")
                                    .Item("field_number").Value(100500)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                            .EndList()
                        .EndMap()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("repeated_message_field")
                            .Item("field_number").Value(3)
                            .Item("proto_type").Value("structured_message")
                            .Item("repeated").Value(true)
                            .Item("fields")
                            .BeginList()
                                .Item().BeginMap()
                                    .Item("name").Value("key")
                                    .Item("field_number").Value(1)
                                    .Item("proto_type").Value("string")
                                .EndMap()
                                .Item().BeginMap()
                                    .Item("name").Value("value")
                                    .Item("field_number").Value(2)
                                    .Item("proto_type").Value("string")
                                .EndMap()
                            .EndList()
                        .EndMap()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("repeated_int64_field")
                            .Item("field_number").Value(4)
                            .Item("proto_type").Value("int64")
                            .Item("repeated").Value(true)
                        .EndMap()
                        .Item()
                        .BeginMap()
                            // In schema it is of type "any".
                            .Item("name").Value("any_field")
                            .Item("field_number").Value(5)
                            .Item("proto_type").Value("int64")
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndAttributes()
        .Value("protobuf");
    return {schema, config};
}

TEST(TProtobufFormat, WriteStructuredMessage)
{
    auto nameTable = New<TNameTable>();
    auto firstId = nameTable->RegisterName("first");
    auto secondId = nameTable->RegisterName("second");
    auto repeatedMessageId = nameTable->RegisterName("repeated_message_field");
    auto repeatedInt64Id = nameTable->RegisterName("repeated_int64_field");
    auto anyFieldId = nameTable->RegisterName("any_field");

    auto [schema, config] = ScreateSchemaAndConfigWithStructuredMessage();

    TString result;
    TStringOutput resultStream(result);
    auto writer = CreateWriterForProtobuf(
        ConvertTo<TProtobufFormatConfigPtr>(config->Attributes()),
        {schema},
        nameTable,
        CreateAsyncAdapter(&resultStream),
        true,
        New<TControlAttributesConfig>(),
        0);

    auto firstYson = BuildYsonStringFluently()
        .BeginList()
            .Item().Value(11111)
            .Item().Value("Two")
            .Item().Value(44)
            .Item()
            .BeginList()
                .Item().Value(55)
                .Item().Value(56)
                .Item().Value(57)
            .EndList()
            .Item()
            .BeginList()
                .Item().Value("key")
                .Item().Value("value")
            .EndList()
            .Item()
            .BeginList()
                .Item()
                .BeginList()
                    .Item().Value("key1")
                    .Item().Value("value1")
                .EndList()
                .Item()
                .BeginList()
                    .Item().Value("key2")
                    .Item().Value("value2")
                .EndList()
            .EndList()
            .Item().Value(45)
            .Item()
            .BeginMap()
                .Item("key").Value("value")
            .EndMap()
            .Item().Entity()
        .EndList();

    auto secondYson = BuildYsonStringFluently()
        .BeginList()
            .Item().Value(101)
            .Item().Value(102)
            .Item().Value(103)
        .EndList();

    auto repeatedMessageYson = BuildYsonStringFluently()
        .BeginList()
            .Item()
            .BeginList()
                .Item().Value("key11")
                .Item().Value("value11")
            .EndList()
            .Item()
            .BeginList()
                .Item().Value("key21")
                .Item().Value("value21")
            .EndList()
        .EndList();

    auto repeatedInt64Yson = BuildYsonStringFluently()
        .BeginList()
            .Item().Value(31)
            .Item().Value(32)
            .Item().Value(33)
        .EndList();

    TUnversionedRowBuilder builder;
    builder.AddValue(MakeUnversionedAnyValue(firstYson.GetData(), firstId));
    builder.AddValue(MakeUnversionedAnyValue(secondYson.GetData(), secondId));
    builder.AddValue(MakeUnversionedAnyValue(repeatedMessageYson.GetData(), repeatedMessageId));
    builder.AddValue(MakeUnversionedAnyValue(repeatedInt64Yson.GetData(), repeatedInt64Id));
    builder.AddValue(MakeUnversionedInt64Value(4321, anyFieldId));

    writer->Write({builder.GetRow()});

    writer->Close()
        .Get()
        .ThrowOnError();

    TStringInput input(result);
    TLenvalParser lenvalParser(&input);

    auto entry = lenvalParser.Next();
    ASSERT_TRUE(entry);

    NYT::NProtobufFormatTest::TMessageWithStructuredEmbedded message;
    ASSERT_TRUE(message.ParseFromString(entry->RowData));

    const auto& first = message.first();
    EXPECT_EQ(first.enum_field(), NProtobufFormatTest::EEnum::two);
    EXPECT_EQ(first.int64_field(), 44);
    std::vector<i64> firstRepeatedInt64Field(
        first.repeated_int64_field().begin(),
        first.repeated_int64_field().end());
    EXPECT_EQ(firstRepeatedInt64Field, (std::vector<i64>{55, 56, 57}));
    EXPECT_EQ(first.message_field().key(), "key");
    EXPECT_EQ(first.message_field().value(), "value");
    ASSERT_EQ(first.repeated_message_field_size(), 2);
    EXPECT_EQ(first.repeated_message_field(0).key(), "key1");
    EXPECT_EQ(first.repeated_message_field(0).value(), "value1");
    EXPECT_EQ(first.repeated_message_field(1).key(), "key2");
    EXPECT_EQ(first.repeated_message_field(1).value(), "value2");

    EXPECT_TRUE(AreNodesEqual(
        ConvertToNode(TYsonString(first.any_int64_field())),
        BuildYsonNodeFluently().Value(45)));

    EXPECT_TRUE(AreNodesEqual(
        ConvertToNode(TYsonString(first.any_map_field())),
        BuildYsonNodeFluently().BeginMap()
            .Item("key").Value("value")
        .EndMap()));

    EXPECT_FALSE(first.has_optional_int64_field());

    const auto& second = message.second();
    EXPECT_EQ(second.one(), 101);
    EXPECT_EQ(second.two(), 102);
    EXPECT_EQ(second.three(), 103);

    ASSERT_EQ(message.repeated_message_field_size(), 2);
    EXPECT_EQ(message.repeated_message_field(0).key(), "key11");
    EXPECT_EQ(message.repeated_message_field(0).value(), "value11");
    EXPECT_EQ(message.repeated_message_field(1).key(), "key21");
    EXPECT_EQ(message.repeated_message_field(1).value(), "value21");

    std::vector<i64> repeatedInt64Field(
        message.repeated_int64_field().begin(),
        message.repeated_int64_field().end());
    EXPECT_EQ(repeatedInt64Field, (std::vector<i64>{31, 32, 33}));

    EXPECT_EQ(message.int64_field(), 4321);

    ASSERT_FALSE(lenvalParser.Next());
}

TEST(TProtobufFormat, ParseStructuredMessage)
{
    auto [schema, config] = ScreateSchemaAndConfigWithStructuredMessage();

    TCollectingValueConsumer rowCollector(schema);

    auto parser = CreateParserForProtobuf(
        &rowCollector,
        ParseFormatConfigFromNode(config->Attributes().ToMap()),
        0);

    NYT::NProtobufFormatTest::TMessageWithStructuredEmbedded message;

    auto* first = message.mutable_first();
    first->set_enum_field(NProtobufFormatTest::EEnum::two);
    first->set_int64_field(44);

    first->add_repeated_int64_field(55);
    first->add_repeated_int64_field(56);
    first->add_repeated_int64_field(57);

    first->mutable_message_field()->set_key("key");
    first->mutable_message_field()->set_value("value");
    auto* firstSubfield1 = first->add_repeated_message_field();
    firstSubfield1->set_key("key1");
    firstSubfield1->set_value("value1");
    auto* firstSubfield2 = first->add_repeated_message_field();
    firstSubfield2->set_key("key2");
    firstSubfield2->set_value("value2");

    first->set_any_int64_field(BuildYsonStringFluently().Value(4422).GetData());
    first->set_any_map_field(
        BuildYsonStringFluently()
            .BeginMap()
                .Item("key").Value("value")
            .EndMap()
        .GetData());

    auto* second = message.mutable_second();
    second->set_one(101);
    second->set_two(102);
    second->set_three(103);

    message.add_repeated_int64_field(31);
    message.add_repeated_int64_field(32);
    message.add_repeated_int64_field(33);

    auto* subfield1 = message.add_repeated_message_field();
    subfield1->set_key("key11");
    subfield1->set_value("value11");
    auto* subfield2 = message.add_repeated_message_field();
    subfield2->set_key("key21");
    subfield2->set_value("value21");

    message.set_int64_field(4321);

    TString lenvalBytes;
    {
        TStringOutput out(lenvalBytes);
        auto messageSize = static_cast<ui32>(message.ByteSize());
        out.Write(&messageSize, sizeof(messageSize));
        ASSERT_TRUE(message.SerializeToStream(&out));
    }

    parser->Read(lenvalBytes);
    parser->Finish();

    ASSERT_EQ(rowCollector.Size(), 1);

    auto firstNode = GetAny(rowCollector.GetRowValue(0, "first"));
    ASSERT_EQ(firstNode->GetType(), ENodeType::List);
    const auto& firstList = firstNode->AsList();
    ASSERT_EQ(firstList->GetChildCount(), 10);

    EXPECT_EQ(firstList->GetChild(0)->GetType(), ENodeType::Entity);
    EXPECT_EQ(firstList->GetChild(1)->GetValue<TString>(), "Two");
    EXPECT_EQ(firstList->GetChild(2)->GetValue<i64>(), 44);

    ASSERT_EQ(firstList->GetChild(3)->GetType(), ENodeType::List);
    EXPECT_EQ(ConvertTo<std::vector<i64>>(firstList->GetChild(3)), (std::vector<i64>{55, 56, 57}));

    ASSERT_EQ(firstList->GetChild(4)->GetType(), ENodeType::List);
    EXPECT_EQ(firstList->GetChild(4)->AsList()->GetChild(0)->GetValue<TString>(), "key");
    EXPECT_EQ(firstList->GetChild(4)->AsList()->GetChild(1)->GetValue<TString>(), "value");

    ASSERT_EQ(firstList->GetChild(5)->GetType(), ENodeType::List);
    ASSERT_EQ(firstList->GetChild(5)->AsList()->GetChildCount(), 2);

    const auto& firstSubNode1 = firstList->GetChild(5)->AsList()->GetChild(0);
    ASSERT_EQ(firstSubNode1->GetType(), ENodeType::List);
    ASSERT_EQ(firstSubNode1->AsList()->GetChildCount(), 2);
    EXPECT_EQ(firstSubNode1->AsList()->GetChild(0)->GetValue<TString>(), "key1");
    EXPECT_EQ(firstSubNode1->AsList()->GetChild(1)->GetValue<TString>(), "value1");

    const auto& firstSubNode2 = firstList->GetChild(5)->AsList()->GetChild(1);
    ASSERT_EQ(firstSubNode2->GetType(), ENodeType::List);
    ASSERT_EQ(firstSubNode2->AsList()->GetChildCount(), 2);
    EXPECT_EQ(firstSubNode2->AsList()->GetChild(0)->GetValue<TString>(), "key2");
    EXPECT_EQ(firstSubNode2->AsList()->GetChild(1)->GetValue<TString>(), "value2");

    ASSERT_EQ(firstList->GetChild(6)->GetType(), ENodeType::Int64);
    EXPECT_EQ(firstList->GetChild(6)->GetValue<i64>(), 4422);

    ASSERT_EQ(firstList->GetChild(7)->GetType(), ENodeType::Map);
    EXPECT_TRUE(AreNodesEqual(
        firstList->GetChild(7),
        BuildYsonNodeFluently()
        .BeginMap()
            .Item("key").Value("value")
        .EndMap()));

    ASSERT_EQ(firstList->GetChild(8)->GetType(), ENodeType::Entity);

    ASSERT_EQ(firstList->GetChild(9)->GetType(), ENodeType::Entity);

    auto secondNode = GetAny(rowCollector.GetRowValue(0, "second"));
    ASSERT_EQ(secondNode->GetType(), ENodeType::List);
    EXPECT_EQ(ConvertTo<std::vector<i64>>(secondNode), (std::vector<i64>{101, 102, 103}));

    auto repeatedMessageNode = GetAny(rowCollector.GetRowValue(0, "repeated_message_field"));
    ASSERT_EQ(repeatedMessageNode->GetType(), ENodeType::List);
    ASSERT_EQ(repeatedMessageNode->AsList()->GetChildCount(), 2);

    const auto& subNode1 = repeatedMessageNode->AsList()->GetChild(0);
    ASSERT_EQ(subNode1->GetType(), ENodeType::List);
    ASSERT_EQ(subNode1->AsList()->GetChildCount(), 2);
    EXPECT_EQ(subNode1->AsList()->GetChild(0)->GetValue<TString>(), "key11");
    EXPECT_EQ(subNode1->AsList()->GetChild(1)->GetValue<TString>(), "value11");

    const auto& subNode2 = repeatedMessageNode->AsList()->GetChild(1);
    ASSERT_EQ(subNode2->GetType(), ENodeType::List);
    ASSERT_EQ(subNode2->AsList()->GetChildCount(), 2);
    EXPECT_EQ(subNode2->AsList()->GetChild(0)->GetValue<TString>(), "key21");
    EXPECT_EQ(subNode2->AsList()->GetChild(1)->GetValue<TString>(), "value21");

    auto anyValue = rowCollector.GetRowValue(0, "any_field");
    ASSERT_EQ(anyValue.Type, EValueType::Int64);
    EXPECT_EQ(anyValue.Data.Int64, 4321);
}

std::pair<std::vector<TTableSchema>, INodePtr> CreateSeveralTablesSchemasAndConfig()
{
    std::vector<TTableSchema> schemas = {
        TTableSchema({
            {"embedded", StructLogicalType({
                {"enum_field", SimpleLogicalType(ESimpleLogicalValueType::String, true)},
                {"int64_field", SimpleLogicalType(ESimpleLogicalValueType::Int64, true)},
            })},
            {"repeated_int64_field", ListLogicalType(SimpleLogicalType(ESimpleLogicalValueType::Int64, true))},
            {"any_field", SimpleLogicalType(ESimpleLogicalValueType::Any, true)},
        }),
        TTableSchema({
            {"enum_field", SimpleLogicalType(ESimpleLogicalValueType::String, true)},
            {"int64_field", SimpleLogicalType(ESimpleLogicalValueType::Int64, true)},
        }),
        // Empty schema.
        TTableSchema(),
    };

    auto config = BuildYsonNodeFluently()
        .BeginAttributes()
            .Item("enumerations")
            .BeginMap()
                .Item("EEnum")
                .BeginMap()
                    .Item("One").Value(1)
                    .Item("Two").Value(2)
                    .Item("Three").Value(3)
                    .Item("MinusFortyTwo").Value(-42)
                .EndMap()
            .EndMap()
            .Item("tables")
            .BeginList()
                // Table #1.
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("embedded")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("structured_message")
                            .Item("fields")
                            .BeginList()
                                .Item()
                                .BeginMap()
                                    .Item("name").Value("int64_field")
                                    .Item("field_number").Value(2)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                                .Item()
                                .BeginMap()
                                    .Item("name").Value("enum_field")
                                    .Item("field_number").Value(1)
                                    .Item("proto_type").Value("enum_string")
                                    .Item("enumeration_name").Value("EEnum")
                                .EndMap()
                            .EndList()
                        .EndMap()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("repeated_int64_field")
                            .Item("field_number").Value(2)
                            .Item("proto_type").Value("int64")
                            .Item("repeated").Value(true)
                        .EndMap()
                        .Item()
                        .BeginMap()
                            // In schema it is of type "any".
                            .Item("name").Value("any_field")
                            .Item("field_number").Value(3)
                            .Item("proto_type").Value("int64")
                        .EndMap()
                    .EndList()
                .EndMap()

                // Table #2.
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("int64_field")
                            .Item("field_number").Value(2)
                            .Item("proto_type").Value("int64")
                        .EndMap()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("enum_field")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("enum_string")
                            .Item("enumeration_name").Value("EEnum")
                        .EndMap()
                    .EndList()
                .EndMap()

                // Table #3.
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("string_field")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("string")
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndAttributes()
        .Value("protobuf");
    return {std::move(schemas), std::move(config)};
}

TEST(TProtobufFormat, WriteSeveralTables)
{
    auto [schemas, configNode] = CreateSeveralTablesSchemasAndConfig();
    auto config = ParseFormatConfigFromNode(configNode->Attributes().ToMap());

    auto nameTable = New<TNameTable>();
    auto embeddedId = nameTable->RegisterName("embedded");
    auto anyFieldId = nameTable->RegisterName("any_field");
    auto int64FieldId = nameTable->RegisterName("int64_field");
    auto repeatedInt64Id = nameTable->RegisterName("repeated_int64_field");
    auto enumFieldId = nameTable->RegisterName("enum_field");
    auto stringFieldId = nameTable->RegisterName("string_field");
    auto tableIndexId = nameTable->RegisterName(TableIndexColumnName);

    TString result;
    TStringOutput resultStream(result);
    auto controlAttributesConfig = New<TControlAttributesConfig>();
    controlAttributesConfig->EnableTableIndex = true;
    auto writer = CreateWriterForProtobuf(
        std::move(config),
        schemas,
        nameTable,
        CreateAsyncAdapter(&resultStream),
        true,
        std::move(controlAttributesConfig),
        0);

    auto embeddedYson = BuildYsonStringFluently()
        .BeginList()
            .Item().Value("Two")
            .Item().Value(44)
        .EndList();

    auto repeatedInt64Yson = ConvertToYsonString(std::vector<i64>{31, 32, 33});

    {
        TUnversionedRowBuilder builder;
        builder.AddValue(MakeUnversionedAnyValue(embeddedYson.GetData(), embeddedId));
        builder.AddValue(MakeUnversionedAnyValue(repeatedInt64Yson.GetData(), repeatedInt64Id));
        builder.AddValue(MakeUnversionedInt64Value(4321, anyFieldId));
        writer->Write({builder.GetRow()});
    }
    {
        TUnversionedRowBuilder builder;
        builder.AddValue(MakeUnversionedStringValue("Two", enumFieldId));
        builder.AddValue(MakeUnversionedInt64Value(999, int64FieldId));
        builder.AddValue(MakeUnversionedInt64Value(1, tableIndexId));
        writer->Write({builder.GetRow()});
    }
    {
        TUnversionedRowBuilder builder;
        builder.AddValue(MakeUnversionedStringValue("blah", stringFieldId));
        builder.AddValue(MakeUnversionedInt64Value(2, tableIndexId));
        writer->Write({builder.GetRow()});
    }

    writer->Close()
        .Get()
        .ThrowOnError();

    TStringInput input(result);
    TLenvalParser lenvalParser(&input);

    {
        auto entry = lenvalParser.Next();
        ASSERT_TRUE(entry);

        NYT::NProtobufFormatTest::TSeveralTablesMessageFirst message;
        ASSERT_TRUE(message.ParseFromString(entry->RowData));

        const auto& embedded = message.embedded();
        EXPECT_EQ(embedded.enum_field(), NProtobufFormatTest::EEnum::two);
        EXPECT_EQ(embedded.int64_field(), 44);

        std::vector<i64> repeatedInt64Field(
            message.repeated_int64_field().begin(),
            message.repeated_int64_field().end());
        EXPECT_EQ(repeatedInt64Field, (std::vector<i64>{31, 32, 33}));
        EXPECT_EQ(message.int64_field(), 4321);
    }
    {
        auto entry = lenvalParser.Next();
        ASSERT_TRUE(entry);

        NYT::NProtobufFormatTest::TSeveralTablesMessageSecond message;
        ASSERT_TRUE(message.ParseFromString(entry->RowData));

        EXPECT_EQ(message.enum_field(), NProtobufFormatTest::EEnum::two);
        EXPECT_EQ(message.int64_field(), 999);
    }
    {
        auto entry = lenvalParser.Next();
        ASSERT_TRUE(entry);

        NYT::NProtobufFormatTest::TSeveralTablesMessageThird message;
        ASSERT_TRUE(message.ParseFromString(entry->RowData));

        EXPECT_EQ(message.string_field(), "blah");
    }
    ASSERT_FALSE(lenvalParser.Next());
}

TEST(TProtobufFormat, ParseSeveralTables)
{
    auto [schemas, configNode] = CreateSeveralTablesSchemasAndConfig();
    auto config = ParseFormatConfigFromNode(configNode->Attributes().ToMap());

    std::vector<TCollectingValueConsumer> rowCollectors;
    std::vector<std::unique_ptr<IParser>> parsers;
    for (const auto& schema : schemas) {
        rowCollectors.emplace_back(schema);
    }
    for (int tableIndex = 0; tableIndex < static_cast<int>(schemas.size()); ++tableIndex) {
        parsers.push_back(CreateParserForProtobuf(
            &rowCollectors[tableIndex],
            config,
            tableIndex));
    }

    NYT::NProtobufFormatTest::TSeveralTablesMessageFirst firstMessage;
    auto* embedded = firstMessage.mutable_embedded();
    embedded->set_enum_field(NProtobufFormatTest::EEnum::two);
    embedded->set_int64_field(44);

    firstMessage.add_repeated_int64_field(55);
    firstMessage.add_repeated_int64_field(56);
    firstMessage.add_repeated_int64_field(57);

    firstMessage.set_int64_field(4444);

    NYT::NProtobufFormatTest::TSeveralTablesMessageSecond secondMessage;
    secondMessage.set_enum_field(NProtobufFormatTest::EEnum::two);
    secondMessage.set_int64_field(44);

    NYT::NProtobufFormatTest::TSeveralTablesMessageThird thirdMessage;
    thirdMessage.set_string_field("blah");

    auto parse = [] (auto& parser, const auto& message) {
        TString lenvalBytes;
        {
            TStringOutput out(lenvalBytes);
            auto messageSize = static_cast<ui32>(message.ByteSize());
            out.Write(&messageSize, sizeof(messageSize));
            ASSERT_TRUE(message.SerializeToStream(&out));
        }
        parser->Read(lenvalBytes);
        parser->Finish();
    };

    parse(parsers[0], firstMessage);
    parse(parsers[1], secondMessage);
    parse(parsers[2], thirdMessage);

    {
        const auto& rowCollector = rowCollectors[0];
        ASSERT_EQ(rowCollector.Size(), 1);

        auto embeddedNode = GetAny(rowCollector.GetRowValue(0, "embedded"));
        ASSERT_EQ(embeddedNode->GetType(), ENodeType::List);
        const auto& embeddedList = embeddedNode->AsList();
        ASSERT_EQ(embeddedList->GetChildCount(), 2);

        EXPECT_EQ(embeddedList->GetChild(0)->GetValue<TString>(), "Two");
        EXPECT_EQ(embeddedList->GetChild(1)->GetValue<i64>(), 44);

        auto repeatedInt64Node = GetAny(rowCollector.GetRowValue(0, "repeated_int64_field"));
        ASSERT_EQ(repeatedInt64Node->GetType(), ENodeType::List);
        EXPECT_EQ(ConvertTo<std::vector<i64>>(repeatedInt64Node), (std::vector<i64>{55, 56, 57}));

        auto int64Field = GetInt64(rowCollector.GetRowValue(0, "any_field"));
        EXPECT_EQ(int64Field, 4444);
    }

    {
        const auto& rowCollector = rowCollectors[1];
        ASSERT_EQ(rowCollector.Size(), 1);

        EXPECT_EQ(GetString(rowCollector.GetRowValue(0, "enum_field")), "Two");
        EXPECT_EQ(GetInt64(rowCollector.GetRowValue(0, "int64_field")), 44);
    }

    {
        const auto& rowCollector = rowCollectors[2];
        ASSERT_EQ(rowCollector.Size(), 1);

        EXPECT_EQ(GetString(rowCollector.GetRowValue(0, "string_field")), "blah");
    }
}

TEST(TProtobufFormat, SchemaConfigMismatch)
{
    auto createParser = [] (const TTableSchema& schema, const INodePtr& configNode) {
        TCollectingValueConsumer rowCollector(schema);
        return CreateParserForProtobuf(
            &rowCollector,
            ParseFormatConfigFromNode(configNode),
            0);
    };
    auto createWriter = [] (const TTableSchema& schema, const INodePtr& configNode) {
        TString result;
        TStringOutput resultStream(result);
        return CreateWriterForProtobuf(
            ParseFormatConfigFromNode(configNode),
            {schema},
            New<TNameTable>(),
            CreateAsyncAdapter(&resultStream),
            true,
            New<TControlAttributesConfig>(),
            0);
    };

    auto schema_struct_with_int64 = TTableSchema({
        {"struct", StructLogicalType({
            {"int64_field", SimpleLogicalType(ESimpleLogicalValueType::Int64, false)},
        })},
    });

    auto schema_struct_with_uint64 = TTableSchema({
        {"struct", StructLogicalType({
            {"int64_field", SimpleLogicalType(ESimpleLogicalValueType::Uint64, false)},
        })},
    });

    auto config_struct_with_int64 = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("struct")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("structured_message")
                            .Item("fields")
                            .BeginList()
                                .Item().BeginMap()
                                    .Item("name").Value("int64_field")
                                    .Item("field_number").Value(2)
                                    // Wrong type.
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                            .EndList()
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    // OK.
    EXPECT_NO_THROW(createParser(schema_struct_with_int64, config_struct_with_int64));
    EXPECT_NO_THROW(createWriter(schema_struct_with_int64, config_struct_with_int64));

    // Types mismatch.
    EXPECT_THROW_WITH_SUBSTRING(createParser(schema_struct_with_uint64, config_struct_with_int64), "Simple logical type mismatch");
    EXPECT_THROW_WITH_SUBSTRING(createWriter(schema_struct_with_uint64, config_struct_with_int64), "Simple logical type mismatch");

    // No schema for structured field.
    EXPECT_THROW_WITH_SUBSTRING(
        createParser(TTableSchema(), config_struct_with_int64),
        "Schema is required for repeated and \"structured_message\" protobuf fields");
    EXPECT_THROW_WITH_SUBSTRING(
        createWriter(TTableSchema(), config_struct_with_int64),
         "Schema is required for repeated and \"structured_message\" protobuf fields");

    auto schema_list_int64 = TTableSchema({
        {"repeated", ListLogicalType(
            SimpleLogicalType(ESimpleLogicalValueType::Int64, true)
        )},
    });

    auto schema_list_optional_int64 = TTableSchema({
        {"repeated", ListLogicalType(
            SimpleLogicalType(ESimpleLogicalValueType::Int64, false)
        )},
    });

    auto config_repeated_int64 = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("repeated")
                            .Item("field_number").Value(1)
                            .Item("repeated").Value(true)
                            .Item("proto_type").Value("int64")
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    // OK.
    EXPECT_NO_THROW(createParser(schema_list_int64, config_repeated_int64));
    EXPECT_NO_THROW(createWriter(schema_list_int64, config_repeated_int64));

    // No schema for repeated field.
    EXPECT_THROW_WITH_SUBSTRING(
        createParser(TTableSchema(), config_repeated_int64),
        "Schema is required for repeated and \"structured_message\" protobuf fields");
    EXPECT_THROW_WITH_SUBSTRING(
        createWriter(TTableSchema(), config_repeated_int64),
         "Schema is required for repeated and \"structured_message\" protobuf fields");

    // List of optional is not allowed.
    EXPECT_THROW_WITH_SUBSTRING(
        createParser(schema_list_optional_int64, config_repeated_int64),
        "Schema and protobuf config mismatch: expected metatype \"simple\", got \"optional\"");
    EXPECT_THROW_WITH_SUBSTRING(
        createWriter(schema_list_optional_int64, config_repeated_int64),
        "Schema and protobuf config mismatch: expected metatype \"simple\", got \"optional\"");

    auto schema_optional_list_int64 = TTableSchema({
        {"repeated", OptionalLogicalType(
            ListLogicalType(SimpleLogicalType(ESimpleLogicalValueType::Int64, true))
        )},
    });

    // Optional list is not allowed.
    EXPECT_THROW_WITH_SUBSTRING(
        createParser(schema_optional_list_int64, config_repeated_int64),
        "Optional list is not supported in protobuf");
    EXPECT_THROW_WITH_SUBSTRING(
        createWriter(schema_optional_list_int64, config_repeated_int64),
        "Optional list is not supported in protobuf");

    auto schema_optional_optional_int64 = TTableSchema({
        {"field", OptionalLogicalType(
            SimpleLogicalType(ESimpleLogicalValueType::Int64, false)
        )},
    });

    auto config_int64 = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("field")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("int64")
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    // Optional of optional is not allowed.
    EXPECT_THROW_WITH_SUBSTRING(
        createParser(schema_optional_optional_int64, config_int64),
        "Schema and protobuf config mismatch: expected metatype \"simple\", got \"optional\"");
    EXPECT_THROW_WITH_SUBSTRING(
        createWriter(schema_optional_optional_int64, config_int64),
        "Schema and protobuf config mismatch: expected metatype \"simple\", got \"optional\"");

    auto schema_struct_with_both = TTableSchema({
        {"struct", StructLogicalType({
            {"required_field", SimpleLogicalType(ESimpleLogicalValueType::Int64, true)},
            {"optional_field", SimpleLogicalType(ESimpleLogicalValueType::Int64, false)},
        })},
    });

    auto config_struct_with_required = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("struct")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("structured_message")
                            .Item("fields")
                            .BeginList()
                                .Item().BeginMap()
                                    .Item("name").Value("required_field")
                                    .Item("field_number").Value(2)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                            .EndList()
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    auto config_struct_with_optional = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("struct")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("structured_message")
                            .Item("fields")
                            .BeginList()
                                .Item().BeginMap()
                                    .Item("name").Value("optional_field")
                                    .Item("field_number").Value(2)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                            .EndList()
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    auto config_struct_with_unknown = BuildYsonNodeFluently()
        .BeginMap()
            .Item("tables")
            .BeginList()
                .Item()
                .BeginMap()
                    .Item("columns")
                    .BeginList()
                        .Item()
                        .BeginMap()
                            .Item("name").Value("struct")
                            .Item("field_number").Value(1)
                            .Item("proto_type").Value("structured_message")
                            .Item("fields")
                            .BeginList()
                                .Item().BeginMap()
                                    .Item("name").Value("required_field")
                                    .Item("field_number").Value(1)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                                .Item().BeginMap()
                                    .Item("name").Value("optional_field")
                                    .Item("field_number").Value(2)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                                .Item().BeginMap()
                                    .Item("name").Value("unknown_field")
                                    .Item("field_number").Value(3)
                                    .Item("proto_type").Value("int64")
                                .EndMap()
                            .EndList()
                        .EndMap()
                    .EndList()
                .EndMap()
            .EndList()
        .EndMap();

    // Schema has more fields, required field is missing in protobuf config.
    // Parser should fail.
    EXPECT_THROW_WITH_SUBSTRING(
        createParser(schema_struct_with_both, config_struct_with_optional),
        "Schema and protobuf config mismatch: non-optional field \"required_field\" in schema is missing from protobuf config");
    // Writer feels OK.
    EXPECT_NO_THROW(createWriter(schema_struct_with_both, config_struct_with_optional));

    // Schema has more fields, optional field is missing in protobuf config.
    // It's OK for both the writer and the parser.
    EXPECT_NO_THROW(createParser(schema_struct_with_both, config_struct_with_required));
    EXPECT_NO_THROW(createWriter(schema_struct_with_both, config_struct_with_required));

    // Protobuf config has more fields, it is never OK.
    EXPECT_THROW_WITH_SUBSTRING(
        createParser(schema_struct_with_both, config_struct_with_unknown),
        "Fields [\"unknown_field\"] from protobuf config not found in schema");
    EXPECT_THROW_WITH_SUBSTRING(
        createWriter(schema_struct_with_both, config_struct_with_unknown),
        "Fields [\"unknown_field\"] from protobuf config not found in schema");
}

////////////////////////////////////////////////////////////////////////////////

class TProtobufFormatAllFields
    : public ::testing::TestWithParam<INodePtr>
{
public:
    bool IsNewFormat() const
    {
        return GetParam()->Attributes().Contains("tables");
    }
};

INSTANTIATE_TEST_CASE_P(
    Specification,
    TProtobufFormatAllFields,
    ::testing::Values(CreateAllFieldsSchemaConfig()));

INSTANTIATE_TEST_CASE_P(
    FileDescriptor,
    TProtobufFormatAllFields,
    ::testing::Values(CreateAllFieldsFileDescriptorConfig()));

TEST_P(TProtobufFormatAllFields, Writer)
{
    auto config = GetParam();

    auto nameTable = New<TNameTable>();

    auto doubleId = nameTable->RegisterName("Double");
    auto floatId = nameTable->RegisterName("Float");

    auto int64Id = nameTable->RegisterName("Int64");
    auto uint64Id = nameTable->RegisterName("UInt64");
    auto sint64Id = nameTable->RegisterName("SInt64");
    auto fixed64Id = nameTable->RegisterName("Fixed64");
    auto sfixed64Id = nameTable->RegisterName("SFixed64");

    auto int32Id = nameTable->RegisterName("Int32");
    auto uint32Id = nameTable->RegisterName("UInt32");
    auto sint32Id = nameTable->RegisterName("SInt32");
    auto fixed32Id = nameTable->RegisterName("Fixed32");
    auto sfixed32Id = nameTable->RegisterName("SFixed32");

    auto boolId = nameTable->RegisterName("Bool");
    auto stringId = nameTable->RegisterName("String");
    auto bytesId = nameTable->RegisterName("Bytes");

    auto enumId = nameTable->RegisterName("Enum");

    auto messageId = nameTable->RegisterName("Message");

    auto anyWithMapId = nameTable->RegisterName("AnyWithMap");
    auto anyWithInt64Id = nameTable->RegisterName("AnyWithInt64");
    auto anyWithStringId = nameTable->RegisterName("AnyWithString");

    auto otherInt64ColumnId = nameTable->RegisterName("OtherInt64Column");
    auto otherDoubleColumnId = nameTable->RegisterName("OtherDoubleColumn");
    auto otherStringColumnId = nameTable->RegisterName("OtherStringColumn");
    auto otherNullColumnId = nameTable->RegisterName("OtherNullColumn");
    auto otherBooleanColumnId = nameTable->RegisterName("OtherBooleanColumn");
    auto otherAnyColumnId = nameTable->RegisterName("OtherAnyColumn");

    auto missintInt64Id = nameTable->RegisterName("MissingInt64");

    TString result;
    TStringOutput resultStream(result);
    auto writer = CreateWriterForProtobuf(
        config->Attributes(),
        {TTableSchema()},
        nameTable,
        CreateAsyncAdapter(&resultStream),
        true,
        New<TControlAttributesConfig>(),
        0);

    NProtobufFormatTest::TEmbeddedMessage embeddedMessage;
    embeddedMessage.set_key("embedded_key");
    embeddedMessage.set_value("embedded_value");
    TString embeddedMessageBytes;
    ASSERT_TRUE(embeddedMessage.SerializeToString(&embeddedMessageBytes));

    auto mapNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("Key").Value("Value")
            .Item("Another")
            .BeginList()
                .Item().Value(1)
                .Item().Value("two")
            .EndList()
        .EndMap();
    auto ysonString = ConvertToYsonString(mapNode);

    TUnversionedRowBuilder builder;
    for (const auto& value : {
        MakeUnversionedDoubleValue(3.14159, doubleId),
        MakeUnversionedDoubleValue(2.71828, floatId),

        MakeUnversionedInt64Value(-1, int64Id),
        MakeUnversionedUint64Value(2, uint64Id),
        MakeUnversionedInt64Value(-3, sint64Id),
        MakeUnversionedUint64Value(4, fixed64Id),
        MakeUnversionedInt64Value(-5, sfixed64Id),

        MakeUnversionedInt64Value(-6, int32Id),
        MakeUnversionedUint64Value(7, uint32Id),
        MakeUnversionedInt64Value(-8, sint32Id),
        MakeUnversionedUint64Value(9, fixed32Id),
        MakeUnversionedInt64Value(-10, sfixed32Id),

        MakeUnversionedBooleanValue(true, boolId),
        MakeUnversionedStringValue("this_is_string", stringId),
        MakeUnversionedStringValue("this_is_bytes", bytesId),

        MakeUnversionedStringValue("Two", enumId),

        MakeUnversionedStringValue(embeddedMessageBytes, messageId),

        MakeUnversionedNullValue(missintInt64Id),
    }) {
        builder.AddValue(value);
    }

    if (IsNewFormat()) {
        builder.AddValue(MakeUnversionedAnyValue(ysonString.GetData(), anyWithMapId));
        builder.AddValue(MakeUnversionedInt64Value(22, anyWithInt64Id));
        builder.AddValue(MakeUnversionedStringValue("some_string", anyWithStringId));

        builder.AddValue(MakeUnversionedInt64Value(-123, otherInt64ColumnId));
        builder.AddValue(MakeUnversionedDoubleValue(-123.456, otherDoubleColumnId));
        builder.AddValue(MakeUnversionedStringValue("some_string", otherStringColumnId));
        builder.AddValue(MakeUnversionedBooleanValue(true, otherBooleanColumnId));
        builder.AddValue(MakeUnversionedAnyValue(ysonString.GetData(), otherAnyColumnId));
        builder.AddValue(MakeUnversionedNullValue(otherNullColumnId));
    }

    writer->Write({builder.GetRow()});

    writer->Close()
        .Get()
        .ThrowOnError();

    TStringInput input(result);
    TLenvalParser lenvalParser(&input);

    auto entry = lenvalParser.Next();
    ASSERT_TRUE(entry);

    NYT::NProtobufFormatTest::TMessage message;
    ASSERT_TRUE(message.ParseFromString(entry->RowData));

    EXPECT_DOUBLE_EQ(message.double_field(), 3.14159);
    EXPECT_FLOAT_EQ(message.float_field(), 2.71828);
    EXPECT_EQ(message.int64_field(), -1);
    EXPECT_EQ(message.uint64_field(), 2);
    EXPECT_EQ(message.sint64_field(), -3);
    EXPECT_EQ(message.fixed64_field(), 4);
    EXPECT_EQ(message.sfixed64_field(), -5);

    EXPECT_EQ(message.int32_field(), -6);
    EXPECT_EQ(message.uint32_field(), 7);
    EXPECT_EQ(message.sint32_field(), -8);
    EXPECT_EQ(message.fixed32_field(), 9);
    EXPECT_EQ(message.sfixed32_field(), -10);

    EXPECT_EQ(message.bool_field(), true);
    EXPECT_EQ(message.string_field(), "this_is_string");
    EXPECT_EQ(message.bytes_field(), "this_is_bytes");

    EXPECT_EQ(message.enum_field(), NProtobufFormatTest::EEnum::two);

    EXPECT_EQ(message.message_field().key(), "embedded_key");
    EXPECT_EQ(message.message_field().value(), "embedded_value");

    if (IsNewFormat()) {
        EXPECT_TRUE(AreNodesEqual(ConvertToNode(TYsonString(message.any_field_with_map())), mapNode));
        EXPECT_TRUE(AreNodesEqual(
            ConvertToNode(TYsonString(message.any_field_with_int64())),
            BuildYsonNodeFluently().Value(22)));
        EXPECT_TRUE(AreNodesEqual(
            ConvertToNode(TYsonString(message.any_field_with_string())),
            BuildYsonNodeFluently().Value("some_string")));

        auto otherColumnsMap = ConvertToNode(TYsonString(message.other_columns_field()))->AsMap();
        EXPECT_EQ(otherColumnsMap->GetChild("OtherInt64Column")->GetValue<i64>(), -123);
        EXPECT_DOUBLE_EQ(otherColumnsMap->GetChild("OtherDoubleColumn")->GetValue<double>(), -123.456);
        EXPECT_EQ(otherColumnsMap->GetChild("OtherStringColumn")->GetValue<TString>(), "some_string");
        EXPECT_EQ(otherColumnsMap->GetChild("OtherBooleanColumn")->GetValue<bool>(), true);
        EXPECT_TRUE(AreNodesEqual(otherColumnsMap->GetChild("OtherAnyColumn"), mapNode));
        EXPECT_EQ(otherColumnsMap->GetChild("OtherNullColumn")->GetType(), ENodeType::Entity);

        auto keys = otherColumnsMap->GetKeys();
        std::sort(keys.begin(), keys.end());
        std::vector<TString> expectedKeys = {
            "OtherInt64Column",
            "OtherDoubleColumn",
            "OtherStringColumn",
            "OtherBooleanColumn",
            "OtherAnyColumn",
            "OtherNullColumn"};
        std::sort(expectedKeys.begin(), expectedKeys.end());
        EXPECT_EQ(expectedKeys, keys);
    }

    ASSERT_FALSE(lenvalParser.Next());
}

TEST_P(TProtobufFormatAllFields, Parser)
{
    TCollectingValueConsumer rowCollector;

    auto parser = CreateParserForProtobuf(
        &rowCollector,
        ParseFormatConfigFromNode(GetParam()->Attributes().ToMap()),
        0);

    NProtobufFormatTest::TMessage message;
    message.set_double_field(3.14159);
    message.set_float_field(2.71828);

    message.set_int64_field(-1);
    message.set_uint64_field(2);
    message.set_sint64_field(-3);
    message.set_fixed64_field(4);
    message.set_sfixed64_field(-5);

    message.set_int32_field(-6);
    message.set_uint32_field(7);
    message.set_sint32_field(-8);
    message.set_fixed32_field(9);
    message.set_sfixed32_field(-10);

    message.set_bool_field(true);
    message.set_string_field("this_is_string");
    message.set_bytes_field("this_is_bytes");
    message.set_enum_field(NProtobufFormatTest::EEnum::three);

    message.mutable_message_field()->set_key("embedded_key");
    message.mutable_message_field()->set_value("embedded_value");

    auto mapNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("Key").Value("Value")
            .Item("Another")
            .BeginList()
                .Item().Value(1)
                .Item().Value("two")
            .EndList()
        .EndMap();

    auto otherColumnsNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("OtherInt64Column").Value(-123)
            .Item("OtherDoubleColumn").Value(-123.456)
            .Item("OtherStringColumn").Value("some_string")
            .Item("OtherBooleanColumn").Value(true)
            .Item("OtherAnyColumn").Value(mapNode)
            .Item("OtherNullColumn").Entity()
        .EndMap();

    if (IsNewFormat()) {
        message.set_any_field_with_map(ConvertToYsonString(mapNode).GetData());
        message.set_any_field_with_int64(BuildYsonStringFluently().Value(22).GetData());
        message.set_any_field_with_string(BuildYsonStringFluently().Value("some_string").GetData());
        message.set_other_columns_field(ConvertToYsonString(otherColumnsNode).GetData());
    }

    TString lenvalBytes;
    {
        TStringOutput out(lenvalBytes);
        ui32 messageSize = message.ByteSize();
        out.Write(&messageSize, sizeof(messageSize));
        ASSERT_TRUE(message.SerializeToStream(&out));
    }

    parser->Read(lenvalBytes);
    parser->Finish();

    ASSERT_EQ(rowCollector.Size(), 1);

    int expectedSize = IsNewFormat() ? 26 : 17;
    ASSERT_EQ(rowCollector.GetRow(0).GetCount(), expectedSize);

    ASSERT_DOUBLE_EQ(GetDouble(rowCollector.GetRowValue(0, "Double")), 3.14159);
    ASSERT_NEAR(GetDouble(rowCollector.GetRowValue(0, "Float")), 2.71828, 1e-5);

    ASSERT_EQ(GetInt64(rowCollector.GetRowValue(0, "Int64")), -1);
    ASSERT_EQ(GetUint64(rowCollector.GetRowValue(0, "UInt64")), 2);
    ASSERT_EQ(GetInt64(rowCollector.GetRowValue(0, "SInt64")), -3);
    ASSERT_EQ(GetUint64(rowCollector.GetRowValue(0, "Fixed64")), 4);
    ASSERT_EQ(GetInt64(rowCollector.GetRowValue(0, "SFixed64")), -5);

    ASSERT_EQ(GetInt64(rowCollector.GetRowValue(0, "Int32")), -6);
    ASSERT_EQ(GetUint64(rowCollector.GetRowValue(0, "UInt32")), 7);
    ASSERT_EQ(GetInt64(rowCollector.GetRowValue(0, "SInt32")), -8);
    ASSERT_EQ(GetUint64(rowCollector.GetRowValue(0, "Fixed32")), 9);
    ASSERT_EQ(GetInt64(rowCollector.GetRowValue(0, "SFixed32")), -10);

    ASSERT_EQ(GetBoolean(rowCollector.GetRowValue(0, "Bool")), true);
    ASSERT_EQ(GetString(rowCollector.GetRowValue(0, "String")), "this_is_string");
    ASSERT_EQ(GetString(rowCollector.GetRowValue(0, "Bytes")), "this_is_bytes");

    if (IsNewFormat()) {
        ASSERT_EQ(GetString(rowCollector.GetRowValue(0, "Enum")), "Three");
    } else {
        ASSERT_EQ(GetInt64(rowCollector.GetRowValue(0, "Enum")), 3);
    }

    NProtobufFormatTest::TEmbeddedMessage embededMessage;
    ASSERT_TRUE(embededMessage.ParseFromString(GetString(rowCollector.GetRowValue(0, "Message"))));
    ASSERT_EQ(embededMessage.key(), "embedded_key");
    ASSERT_EQ(embededMessage.value(), "embedded_value");

    if (IsNewFormat()) {
        ASSERT_TRUE(AreNodesEqual(GetAny(rowCollector.GetRowValue(0, "AnyWithMap")), mapNode));
        ASSERT_EQ(GetInt64(rowCollector.GetRowValue(0, "AnyWithInt64")), 22);
        ASSERT_EQ(GetString(rowCollector.GetRowValue(0, "AnyWithString")), "some_string");

        ASSERT_EQ(GetInt64(rowCollector.GetRowValue(0, "OtherInt64Column")), -123);
        ASSERT_DOUBLE_EQ(GetDouble(rowCollector.GetRowValue(0, "OtherDoubleColumn")), -123.456);
        ASSERT_EQ(GetString(rowCollector.GetRowValue(0, "OtherStringColumn")), "some_string");
        ASSERT_EQ(GetBoolean(rowCollector.GetRowValue(0, "OtherBooleanColumn")), true);
        ASSERT_TRUE(AreNodesEqual(GetAny(rowCollector.GetRowValue(0, "OtherAnyColumn")), mapNode));
        ASSERT_EQ(rowCollector.GetRowValue(0, "OtherNullColumn").Type, EValueType::Null);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT
