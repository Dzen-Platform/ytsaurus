#include <yt/core/test_framework/framework.h>

#include <yt/core/unittests/proto/protobuf_yson_ut.pb.h>
#include <yt/core/unittests/proto/protobuf_yson_casing_ut.pb.h>

#include <yt/core/yson/protobuf_interop.h>
#include <yt/core/yson/null_consumer.h>

#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/ypath_client.h>

#include <yt/core/misc/string.h>
#include <yt/core/misc/protobuf_helpers.h>

#include <contrib/libs/protobuf/io/coded_stream.h>
#include <contrib/libs/protobuf/io/zero_copy_stream_impl_lite.h>

#include <contrib/libs/protobuf/wire_format.h>

namespace NYT::NYson {
namespace {

using namespace NYTree;
using namespace NYPath;
using namespace ::google::protobuf::io;
using namespace ::google::protobuf::internal;

////////////////////////////////////////////////////////////////////////////////

TString ToHex(const TString& data)
{
    TStringBuilder builder;
    for (char ch : data) {
        builder.AppendFormat("%x%x ",
            static_cast<unsigned char>(ch) >> 4,
            static_cast<unsigned char>(ch) & 0xf);
    }
    return builder.Flush();
}

#define EXPECT_YPATH(body, ypath) \
    do { \
        bool thrown = false; \
        try body \
        catch (const TErrorException& ex) { \
            thrown = true; \
            Cerr << ToString(ex.Error()) << Endl; \
            EXPECT_EQ(ypath, ex.Error().Attributes().Get<TYPath>("ypath")); \
        } \
        EXPECT_TRUE(thrown); \
    } while (false);

#define TEST_PROLOGUE_WITH_OPTIONS(type, options) \
    TString str; \
    StringOutputStream output(&str); \
    auto protobufWriter = CreateProtobufWriter(&output, ReflectProtobufMessageType<NYT::NProto::type>(), options); \
    BuildYsonFluently(protobufWriter.get())

#define TEST_PROLOGUE(type) \
    TEST_PROLOGUE_WITH_OPTIONS(type, TProtobufWriterOptions())

#define TEST_EPILOGUE(type) \
    Cerr << ToHex(str) << Endl; \
    NYT::NProto::type message; \
    EXPECT_TRUE(message.ParseFromArray(str.data(), str.length()));

TEST(TYsonToProtobufYsonTest, Success)
{
    TEST_PROLOGUE(TMessage)
        .BeginMap()
            .Item("int32_field").Value(10000)
            .Item("uint32_field").Value(10000U)
            .Item("sint32_field").Value(10000)
            .Item("int64_field").Value(10000)
            .Item("uint64_field").Value(10000U)
            .Item("fixed32_field").Value(10000U)
            .Item("fixed64_field").Value(10000U)
            .Item("bool_field").Value(true)
            .Item("repeated_int32_field").BeginList()
                .Item().Value(1)
                .Item().Value(2)
                .Item().Value(3)
            .EndList()
            .Item("nested_message1").BeginMap()
                .Item("int32_field").Value(123)
                .Item("color").Value("blue")
                .Item("nested_message").BeginMap()
                    .Item("color").Value("green")
                    .Item("nested_message").BeginMap()
                    .EndMap()
                .EndMap()
            .EndMap()
            .Item("nested_message2").BeginMap()
            .EndMap()
            .Item("string_field").Value("hello")
            .Item("repeated_nested_message1").BeginList()
                .Item().BeginMap()
                    .Item("int32_field").Value(456)
                .EndMap()
                .Item().BeginMap()
                    .Item("int32_field").Value(654)
                .EndMap()
            .EndList()
            .Item("float_field").Value(3.14)
            .Item("double_field").Value(3.14)
            .Item("attributes").BeginMap()
                .Item("k1").Value(1)
                .Item("k2").Value("test")
                .Item("k3").BeginList()
                    .Item().Value(1)
                    .Item().Value(2)
                    .Item().Value(3)
                .EndList()
            .EndMap()
            .Item("yson_field").BeginMap()
                .Item("a").Value(1)
                .Item("b").BeginList()
                    .Item().Value("foobar")
                .EndList()
            .EndMap()
            .Item("int32_map").BeginMap()
                .Item("hello").Value(0)
                .Item("world").Value(1)
            .EndMap()
            .Item("nested_message_map").BeginMap()
                .Item("hello").BeginMap()
                    .Item("int32_field").Value(123)
                .EndMap()
                .Item("world").BeginMap()
                    .Item("color").Value("blue")
                    .Item("nested_message_map").BeginMap()
                        .Item("test").BeginMap()
                            .Item("repeated_int32_field").BeginList()
                                .Item().Value(1)
                                .Item().Value(2)
                                .Item().Value(3)
                            .EndList()
                        .EndMap()
                    .EndMap()
                .EndMap()
            .EndMap()
        .EndMap();


    TEST_EPILOGUE(TMessage)
    EXPECT_EQ(10000, message.int32_field_xxx());
    EXPECT_EQ(10000U, message.uint32_field());
    EXPECT_EQ(10000, message.sint32_field());
    EXPECT_EQ(10000, message.int64_field());
    EXPECT_EQ(10000U, message.uint64_field());
    EXPECT_EQ(10000U, message.fixed32_field());
    EXPECT_EQ(10000U, message.fixed64_field());
    EXPECT_TRUE(message.bool_field());
    EXPECT_EQ("hello", message.string_field());
    EXPECT_FLOAT_EQ(3.14, message.float_field());
    EXPECT_DOUBLE_EQ(3.14, message.double_field());

    EXPECT_TRUE(message.has_nested_message1());
    EXPECT_EQ(123, message.nested_message1().int32_field());
    EXPECT_EQ(NYT::NProto::EColor::Color_Blue, message.nested_message1().color());
    EXPECT_TRUE(message.nested_message1().has_nested_message());
    EXPECT_FALSE(message.nested_message1().nested_message().has_int32_field());
    EXPECT_EQ(NYT::NProto::EColor::Color_Green, message.nested_message1().nested_message().color());
    EXPECT_TRUE(message.nested_message1().nested_message().has_nested_message());
    EXPECT_FALSE(message.nested_message1().nested_message().nested_message().has_nested_message());
    EXPECT_FALSE(message.nested_message1().nested_message().nested_message().has_int32_field());

    EXPECT_TRUE(message.has_nested_message2());
    EXPECT_FALSE(message.nested_message2().has_int32_field());
    EXPECT_FALSE(message.nested_message2().has_nested_message());

    EXPECT_EQ(3, message.repeated_int32_field().size());
    EXPECT_EQ(1, message.repeated_int32_field().Get(0));
    EXPECT_EQ(2, message.repeated_int32_field().Get(1));
    EXPECT_EQ(3, message.repeated_int32_field().Get(2));

    EXPECT_EQ(2, message.repeated_nested_message1().size());
    EXPECT_EQ(456, message.repeated_nested_message1().Get(0).int32_field());
    EXPECT_EQ(654, message.repeated_nested_message1().Get(1).int32_field());

    EXPECT_EQ(3, message.attributes().attributes_size());
    EXPECT_EQ("k1", message.attributes().attributes(0).key());
    EXPECT_EQ(ConvertToYsonString(1).GetData(), message.attributes().attributes(0).value());
    EXPECT_EQ("k2", message.attributes().attributes(1).key());
    EXPECT_EQ(ConvertToYsonString("test").GetData(), message.attributes().attributes(1).value());
    EXPECT_EQ("k3", message.attributes().attributes(2).key());
    EXPECT_EQ(ConvertToYsonString(std::vector<int>{1, 2, 3}).GetData(), message.attributes().attributes(2).value());

    auto node = BuildYsonNodeFluently().BeginMap()
            .Item("a").Value(1)
            .Item("b").BeginList()
                .Item().Value("foobar")
            .EndList()
        .EndMap();

    EXPECT_EQ(ConvertToYsonString(node).GetData(), message.yson_field());

    EXPECT_EQ(2, message.int32_map_size());
    EXPECT_EQ(0, message.int32_map().at("hello"));
    EXPECT_EQ(1, message.int32_map().at("world"));

    EXPECT_EQ(2, message.nested_message_map_size());
    EXPECT_EQ(123, message.nested_message_map().at("hello").int32_field());
    EXPECT_EQ(NYT::NProto::Color_Blue, message.nested_message_map().at("world").color());
    EXPECT_EQ(1, message.nested_message_map().at("world").nested_message_map_size());
    EXPECT_EQ(3, message.nested_message_map().at("world").nested_message_map().at("test").repeated_int32_field_size());
    EXPECT_EQ(1, message.nested_message_map().at("world").nested_message_map().at("test").repeated_int32_field(0));
    EXPECT_EQ(2, message.nested_message_map().at("world").nested_message_map().at("test").repeated_int32_field(1));
    EXPECT_EQ(3, message.nested_message_map().at("world").nested_message_map().at("test").repeated_int32_field(2));
}

TEST(TYsonToProtobufYsonTest, Aliases)
{
    TEST_PROLOGUE(TMessage)
        .BeginMap()
            .Item("int32_field_alias1").Value(10000)
        .EndMap();


    TEST_EPILOGUE(TMessage)
    EXPECT_EQ(10000, message.int32_field_xxx());
}

TEST(TYsonToProtobufTest, TypeConversions)
{
    TEST_PROLOGUE(TMessage)
        .BeginMap()
            .Item("int32_field").Value(10000U)
            .Item("uint32_field").Value(10000)
            .Item("sint32_field").Value(10000U)
            .Item("int64_field").Value(10000U)
            .Item("uint64_field").Value(10000)
            .Item("fixed32_field").Value(10000)
            .Item("fixed64_field").Value(10000)
        .EndMap();

    TEST_EPILOGUE(TMessage)
    EXPECT_EQ(10000, message.int32_field_xxx());
    EXPECT_EQ(10000U, message.uint32_field());
    EXPECT_EQ(10000, message.sint32_field());
    EXPECT_EQ(10000, message.int64_field());
    EXPECT_EQ(10000U, message.uint64_field());
    EXPECT_EQ(10000U, message.fixed32_field());
    EXPECT_EQ(10000U, message.fixed64_field());
}

TEST(TYsonToProtobufYsonTest, Entities)
{
    TEST_PROLOGUE(TMessage)
        .BeginMap()
            .Item("int32_field").Entity()
            .Item("repeated_int32_field").Entity()
            .Item("nested_message1").Entity()
            .Item("repeated_nested_message1").Entity()
            .Item("attributes").Entity()
            .Item("yson_field").Entity()
            .Item("int32_map").Entity()
        .EndMap();

    TEST_EPILOGUE(TMessage)
    EXPECT_FALSE(message.has_int32_field_xxx());
    EXPECT_TRUE(message.repeated_int32_field().empty());
    EXPECT_FALSE(message.has_nested_message1());
    EXPECT_TRUE(message.repeated_nested_message1().empty());
    EXPECT_FALSE(message.has_attributes());
    EXPECT_EQ("#", message.yson_field());
    EXPECT_TRUE(message.int32_map().empty());
}

TEST(TYsonToProtobufTest, RootEntity)
{
    TEST_PROLOGUE(TMessage)
        .Entity();

    TEST_EPILOGUE(TMessage)
    EXPECT_FALSE(message.has_int32_field_xxx());
}

TEST(TYsonToProtobufTest, Failure)
{
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .Value(0);
    }, "");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginList()
            .EndList();
    }, "");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int32_field").Value(true)
            .EndMap();
    }, "/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int32_field").Value(static_cast<i64>(std::numeric_limits<i32>::max()) + 1)
            .EndMap();
    }, "/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int32_field").Value(static_cast<i64>(std::numeric_limits<i32>::min()) - 1)
            .EndMap();
    }, "/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("uint32_field").Value(static_cast<ui64>(std::numeric_limits<ui32>::max()) + 1)
            .EndMap();
    }, "/uint32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("nested_message1").BeginMap()
                    .Item("int32_field").Value("test")
                .EndMap()
            .EndMap();
    }, "/nested_message1/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("nested_message1").BeginMap()
                    .Item("int32_field").BeginAttributes().EndAttributes().Value(123)
                .EndMap()
            .EndMap();
    }, "/nested_message1/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("nested_message1").BeginMap()
                    .Item("color").Value("white")
                .EndMap()
            .EndMap();
    }, "/nested_message1/color");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("nested_message1").Value(123)
            .EndMap();
    }, "/nested_message1");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("repeated_nested_message1").BeginList()
                    .Item().BeginMap()
                        .Item("color").Value("blue")
                    .EndMap()
                    .Item().BeginMap()
                        .Item("color").Value("black")
                    .EndMap()
                .EndList()
            .EndMap();
    }, "/repeated_nested_message1/1/color");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("repeated_nested_message1").BeginList()
                    .Item().BeginList()
                    .EndList()
                .EndList()
            .EndMap();
    }, "/repeated_nested_message1/0");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("repeated_nested_message1").BeginList()
                    .Item().BeginMap()
                        .Item("color").Value("black")
                    .EndMap()
                .EndList()
            .EndMap();
    }, "/repeated_nested_message1/0/color");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int32_field").Value(0)
                .Item("int32_field").Value(1)
            .EndMap();
    }, "/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int32_field").Value(0)
                .Item("int32_field").Value(1)
            .EndMap();
    }, "/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessageWithRequiredFields)
            .BeginMap()
                .Item("required_field").Value(0)
                .Item("required_field").Value(1)
            .EndMap();
    }, "/required_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessageWithRequiredFields)
            .BeginMap()
            .EndMap();
    }, "/required_field");

    // int32
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int32_field").Value(10000000000)
            .EndMap();
    }, "/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int32_field").Value(10000000000U)
            .EndMap();
    }, "/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int32_field").Value(-10000000000)
            .EndMap();
    }, "/int32_field");

    // sint32
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("sint32_field").Value(10000000000)
            .EndMap();
    }, "/sint32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("sint32_field").Value(10000000000U)
            .EndMap();
    }, "/sint32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("sint32_field").Value(-10000000000)
            .EndMap();
    }, "/sint32_field");

    // uint32
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("uint32_field").Value(10000000000)
            .EndMap();
    }, "/uint32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("uint32_field").Value(10000000000U)
            .EndMap();
    }, "/uint32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("uint32_field").Value(-1)
            .EndMap();
    }, "/uint32_field");

    // int32
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int64_field").Value(std::numeric_limits<ui64>::max())
            .EndMap();
    }, "/int64_field");

    // uint64
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("uint64_field").Value(-1)
            .EndMap();
    }, "/uint64_field");

    // fixed32
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("fixed32_field").Value(10000000000)
            .EndMap();
    }, "/fixed32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("fixed32_field").Value(10000000000U)
            .EndMap();
    }, "/fixed32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("fixed32_field").Value(-10000000000)
            .EndMap();
    }, "/fixed32_field");

    // fixed64
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("fixed64_field").Value(-1)
            .EndMap();
    }, "/fixed64_field");

    // YT-9094
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("repeated_int32_field").BeginList()
                .EndList()
                .Item("repeated_nested_message1").BeginList()
                    .Item().BeginMap()
                    .EndMap()
                    .Item().BeginMap()
                        .Item("int32_field").Value(1)
                    .EndMap()
                    .Item().BeginMap()
                        .Item("int32_field").Value(1)
                    .EndMap()
                    .Item().BeginMap()
                        .Item("int32_field").Value(1)
                    .EndMap()
                .EndList()
                .Item("repeated_nested_message2").BeginList()
                    .Item().BeginMap()
                        .Item("int32_field").Value(1)
                    .EndMap()
                .EndList()
            .Item("attributes").BeginMap()
                .Item("host").Value("localhost")
            .EndMap()
            .Item("nested_message1").BeginList()
                .EndList()
            .EndMap();
    }, "/nested_message1");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("nested_message_map").BeginList()
                    .Item().Value(123)
                .EndList()
            .EndMap();
    }, "/nested_message_map");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("nested_message_map").Value(123)
            .EndMap();
    }, "/nested_message_map");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("int32_map").BeginMap()
                    .Item("a").Value("b")
                .EndMap()
            .EndMap();
    }, "/int32_map/a");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("nested_message_map").BeginMap()
                    .Item("a").BeginMap()
                        .Item("nested_message_map").Value(123)
                    .EndMap()
                .EndMap()
            .EndMap();
    }, "/nested_message_map/a/nested_message_map");
}

TEST(TYsonToProtobufTest, ErrorProto)
{
    TEST_PROLOGUE(TError)
        .BeginMap()
            .Item("message").Value("Hello world")
            .Item("code").Value(1)
            .Item("attributes").BeginMap()
                .Item("host").Value("localhost")
            .EndMap()
        .EndMap();

    TEST_EPILOGUE(TError);

    EXPECT_EQ("Hello world", message.message());
    EXPECT_EQ(1, message.code());

    auto attribute = message.attributes().attributes()[0];
    EXPECT_EQ(attribute.key(), "host");
    EXPECT_EQ(ConvertTo<TString>(TYsonString(attribute.value())), "localhost");
}

TEST(TYsonToProtobufTest, SkipUnknownFields)
{
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("unknown_field").Value(1)
            .EndMap();
    }, "");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("repeated_nested_message1").BeginList()
                    .Item().BeginMap()
                        .Item("unknown_field").Value(1)
                    .EndMap()
                .EndList()
            .EndMap();
    }, "/repeated_nested_message1/0");

    {
        TProtobufWriterOptions options;
        options.UnknownYsonFieldsMode = EUnknownYsonFieldsMode::Keep;

        TEST_PROLOGUE_WITH_OPTIONS(TMessage, options)
            .BeginMap()
                .Item("int32_field").Value(10000)
                .Item("unknown_field").Value(1)
                .Item("nested_message1").BeginMap()
                    .Item("int32_field").Value(123)
                    .Item("nested_message").BeginMap()
                        .Item("unknown_map").BeginMap()
                        .EndMap()
                    .EndMap()
                .EndMap()
                .Item("repeated_nested_message1").BeginList()
                    .Item().BeginMap()
                        .Item("int32_field").Value(456)
                        .Item("unknown_list").BeginList()
                        .EndList()
                    .EndMap()
                .EndList()
            .EndMap();

        TEST_EPILOGUE(TMessage)
        EXPECT_EQ(10000, message.int32_field_xxx());

        EXPECT_TRUE(message.has_nested_message1());
        EXPECT_EQ(123, message.nested_message1().int32_field());
        EXPECT_TRUE(message.nested_message1().has_nested_message());

        EXPECT_EQ(1, message.repeated_nested_message1().size());
        EXPECT_EQ(456, message.repeated_nested_message1().Get(0).int32_field());
    }
}

TEST(TYsonToProtobufTest, KeepUnknownFields)
{
    auto ysonString = BuildYsonStringFluently()
        .BeginMap()
            .Item("known_string").Value("hello")
            .Item("unknown_int").Value(123)
            .Item("unknown_map").BeginMap()
                .Item("a").Value(1)
                .Item("b").Value("test")
            .EndMap()
            .Item("known_submessage").BeginMap()
                .Item("known_int").Value(555)
                .Item("unknown_list").BeginList()
                    .Item().Value(1)
                    .Item().Value(2)
                    .Item().Value(3)
                .EndList()
            .EndMap()
            .Item("known_submessages").BeginList()
                .Item().BeginMap()
                    .Item("known_string").Value("first")
                    .Item("unknown_int").Value(10)
                .EndMap()
                .Item().BeginMap()
                    .Item("known_string").Value("second")
                    .Item("unknown_int").Value(20)
                .EndMap()
            .EndList()
            .Item("another_unknown_int").Value(777)
        .EndMap();

    TString protobufString;
    StringOutputStream protobufOutput(&protobufString);
    auto protobufWriter = CreateProtobufWriter(&protobufOutput, ReflectProtobufMessageType<NYT::NProto::TExtensibleMessage>());
    ParseYsonStringBuffer(ysonString.GetData(), EYsonType::Node, protobufWriter.get());

    NYT::NProto::TExtensibleMessage message;
    EXPECT_TRUE(message.ParseFromArray(protobufString.data(), protobufString.length()));

    EXPECT_EQ("hello", message.known_string());
    EXPECT_EQ(555, message.known_submessage().known_int());
    EXPECT_EQ(2, message.known_submessages_size());
    EXPECT_EQ("first", message.known_submessages(0).known_string());
    EXPECT_EQ("second", message.known_submessages(1).known_string());

    TString newYsonString;
    TStringOutput newYsonOutputStream(newYsonString);
    TYsonWriter ysonWriter(&newYsonOutputStream, EYsonFormat::Pretty);
    ArrayInputStream protobufInput(protobufString.data(), protobufString.length());
    ParseProtobuf(&ysonWriter, &protobufInput, ReflectProtobufMessageType<NYT::NProto::TExtensibleMessage>());

    Cerr << newYsonString << Endl;

    EXPECT_TRUE(AreNodesEqual(ConvertToNode(TYsonString(newYsonString)), ConvertToNode(ysonString)));
}

TEST(TYsonToProtobufTest, Entities)
{
    TProtobufWriterOptions options;

    TEST_PROLOGUE_WITH_OPTIONS(TMessage, options)
        .BeginMap()
            .Item("nested_message1").Entity()
        .EndMap();
    TEST_EPILOGUE(TMessage)

    EXPECT_FALSE(message.has_nested_message1());
}

TEST(TYsonToProtobufTest, ReservedFields)
{
    TProtobufWriterOptions options;

    TEST_PROLOGUE_WITH_OPTIONS(TMessageWithReservedFields, options)
        .BeginMap()
            .Item("reserved_field1").Value(1)
            .Item("reserved_field1").Entity()
            .Item("reserved_field3").BeginMap()
                .Item("key").Value("value")
            .EndMap()
        .EndMap();
    TEST_EPILOGUE(TMessage)
}

#undef TEST_PROLOGUE
#undef TEST_PROLOGUE_WITH_OPTIONS
#undef TEST_EPILOGUE

////////////////////////////////////////////////////////////////////////////////

#define TEST_PROLOGUE() \
    TString protobuf; \
    StringOutputStream protobufOutputStream(&protobuf); \
    CodedOutputStream codedStream(&protobufOutputStream);

#define TEST_EPILOGUE_WITH_OPTIONS(type, options) \
    codedStream.Trim(); \
    Cerr << ToHex(protobuf) << Endl; \
    ArrayInputStream protobufInputStream(protobuf.data(), protobuf.length()); \
    TString yson; \
    TStringOutput ysonOutputStream(yson); \
    TYsonWriter writer(&ysonOutputStream, EYsonFormat::Pretty); \
    ParseProtobuf(&writer, &protobufInputStream, ReflectProtobufMessageType<NYT::NProto::type>(), options); \
    Cerr << ConvertToYsonString(TYsonString(yson), EYsonFormat::Pretty).GetData() << Endl;

#define TEST_EPILOGUE(type) \
    TEST_EPILOGUE_WITH_OPTIONS(type, TProtobufParserOptions())

TEST(TProtobufToYsonTest, Success)
{
    NYT::NProto::TMessage message;
    message.set_int32_field_xxx(10000);
    message.set_uint32_field(10000U);
    message.set_sint32_field(10000);
    message.set_int64_field(10000);
    message.set_uint64_field(10000U);
    message.set_fixed32_field(10000U);
    message.set_fixed64_field(10000U);
    message.set_bool_field(true);
    message.set_string_field("hello");
    message.set_float_field(3.14);
    message.set_double_field(3.14);

    message.add_repeated_int32_field(1);
    message.add_repeated_int32_field(2);
    message.add_repeated_int32_field(3);

    message.mutable_nested_message1()->set_int32_field(123);
    message.mutable_nested_message1()->set_color(NYT::NProto::Color_Blue);

    message.mutable_nested_message1()->mutable_nested_message()->set_color(NYT::NProto::Color_Green);

    {
        auto* proto = message.add_repeated_nested_message1();
        proto->set_int32_field(456);
        proto->add_repeated_int32_field(1);
        proto->add_repeated_int32_field(2);
        proto->add_repeated_int32_field(3);
    }
    {
        auto* proto = message.add_repeated_nested_message1();
        proto->set_int32_field(654);
    }
    {
        auto* proto = message.mutable_attributes();
        {
            auto* entry = proto->add_attributes();
            entry->set_key("k1");
            entry->set_value(ConvertToYsonString(1).GetData());
        }
        {
            auto* entry = proto->add_attributes();
            entry->set_key("k2");
            entry->set_value(ConvertToYsonString("test").GetData());
        }
        {
            auto* entry = proto->add_attributes();
            entry->set_key("k3");
            entry->set_value(ConvertToYsonString(std::vector<int>{1, 2, 3}).GetData());
        }
    }

    message.set_yson_field("{a=1;b=[\"foobar\";];}");

    {
        auto& map = *message.mutable_int32_map();
        map["hello"] = 0;
        map["world"] = 1;
    }

    {
        auto& map = *message.mutable_nested_message_map();
        {
            NYT::NProto::TNestedMessage value;
            value.set_int32_field(123);
            map["hello"] = value;
        }
        {
            NYT::NProto::TNestedMessage value;
            value.set_color(NYT::NProto::Color_Blue);
            {
                auto& submap = *value.mutable_nested_message_map();
                {
                    NYT::NProto::TNestedMessage subvalue;
                    subvalue.add_repeated_int32_field(1);
                    subvalue.add_repeated_int32_field(2);
                    subvalue.add_repeated_int32_field(3);
                    submap["test"] = subvalue;
                }
            }
            map["world"] = value;
        }
    }

    TEST_PROLOGUE()
    message.SerializeToCodedStream(&codedStream);
    TEST_EPILOGUE(TMessage)

    auto writtenNode = ConvertToNode(TYsonString(yson));
    auto expectedNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("int32_field").Value(10000)
            .Item("uint32_field").Value(10000U)
            .Item("sint32_field").Value(10000)
            .Item("int64_field").Value(10000)
            .Item("uint64_field").Value(10000U)
            .Item("fixed32_field").Value(10000U)
            .Item("fixed64_field").Value(10000U)
            .Item("bool_field").Value(true)
            .Item("string_field").Value("hello")
            .Item("float_field").Value(3.14)
            .Item("double_field").Value(3.14)
            .Item("repeated_int32_field").BeginList()
                .Item().Value(1)
                .Item().Value(2)
                .Item().Value(3)
            .EndList()
            .Item("nested_message1").BeginMap()
                .Item("int32_field").Value(123)
                .Item("color").Value("blue")
                .Item("nested_message").BeginMap()
                    .Item("color").Value("green")
                .EndMap()
            .EndMap()
            .Item("repeated_nested_message1").BeginList()
                .Item().BeginMap()
                    .Item("int32_field").Value(456)
                    .Item("repeated_int32_field").BeginList()
                        .Item().Value(1)
                        .Item().Value(2)
                        .Item().Value(3)
                    .EndList()
                .EndMap()
                .Item().BeginMap()
                    .Item("int32_field").Value(654)
                .EndMap()
            .EndList()
            .Item("attributes").BeginMap()
                .Item("k1").Value(1)
                .Item("k2").Value("test")
                .Item("k3").BeginList()
                    .Item().Value(1)
                    .Item().Value(2)
                    .Item().Value(3)
                .EndList()
            .EndMap()
            .Item("yson_field").BeginMap()
                .Item("a").Value(1)
                .Item("b").BeginList()
                    .Item().Value("foobar")
                .EndList()
            .EndMap()
            .Item("int32_map").BeginMap()
                .Item("hello").Value(0)
                .Item("world").Value(1)
            .EndMap()
            .Item("nested_message_map").BeginMap()
                .Item("hello").BeginMap()
                    .Item("int32_field").Value(123)
                .EndMap()
                .Item("world").BeginMap()
                    .Item("color").Value("blue")
                    .Item("nested_message_map").BeginMap()
                        .Item("test").BeginMap()
                            .Item("repeated_int32_field").BeginList()
                                .Item().Value(1)
                                .Item().Value(2)
                                .Item().Value(3)
                            .EndList()
                        .EndMap()
                    .EndMap()
                .EndMap()
            .EndMap()
        .EndMap();
    EXPECT_TRUE(AreNodesEqual(writtenNode, expectedNode));
}

TEST(TProtobufToYsonTest, Casing)
{
    NYT::NProto::TCamelCaseStyleMessage message;
    message.set_somefield(1);
    message.set_anotherfield123(2);
    message.set_crazy_field(3);

    TEST_PROLOGUE()
    message.SerializeToCodedStream(&codedStream);
    TEST_EPILOGUE(TCamelCaseStyleMessage)

    auto writtenNode = ConvertToNode(TYsonString(yson));
    auto expectedNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("some_field").Value(1)
            .Item("another_field123").Value(2)
            .Item("crazy_field").Value(3)
        .EndMap();
    EXPECT_TRUE(AreNodesEqual(writtenNode, expectedNode));
}

TEST(TProtobufToYsonTest, ErrorProto)
{
    NYT::NProto::TError errorProto;
    errorProto.set_message("Hello world");
    errorProto.set_code(1);
    auto attributeProto = errorProto.mutable_attributes()->add_attributes();
    attributeProto->set_key("host");
    attributeProto->set_value(ConvertToYsonString("localhost").GetData());

    auto serialized = SerializeProtoToRef(errorProto);

    ArrayInputStream inputStream(serialized.Begin(), serialized.Size());
    TString yson;
    TStringOutput outputStream(yson);
    TYsonWriter writer(&outputStream, EYsonFormat::Pretty);
    ParseProtobuf(&writer, &inputStream, ReflectProtobufMessageType<NYT::NProto::TError>());

    auto writtenNode = ConvertToNode(TYsonString(yson));
    auto expectedNode = BuildYsonNodeFluently()
        .BeginMap()
            .Item("message").Value("Hello world")
            .Item("code").Value(1)
            .Item("attributes").BeginMap()
                .Item("host").Value("localhost")
            .EndMap()
        .EndMap();
    EXPECT_TRUE(AreNodesEqual(writtenNode, expectedNode));
}

TEST(TProtobufToYsonTest, Failure)
{
    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*int32_field_xxx*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        TEST_EPILOGUE(TMessage)
    }, "/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(15 /*nested_message1*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(3);
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*color*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(100);
        TEST_EPILOGUE(TMessage)
    }, "/nested_message1/color");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(17 /*repeated_int32_field*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        TEST_EPILOGUE(TMessage)
    }, "/repeated_int32_field/0");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(17 /*repeated_int32_field*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(1);
        codedStream.WriteTag(WireFormatLite::MakeTag(17 /*repeated_int32_field*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        TEST_EPILOGUE(TMessage)
    }, "/repeated_int32_field/1");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(18 /*repeated_nested_message1*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(3);
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*color*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(2);
        codedStream.WriteTag(WireFormatLite::MakeTag(18 /*repeated_nested_message1*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(3);
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*color*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(4);
        TEST_EPILOGUE(TMessage)
    }, "/repeated_nested_message1/1/color");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(18 /*repeated_nested_message1*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(3);
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*color*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(2);
        codedStream.WriteTag(WireFormatLite::MakeTag(18 /*repeated_nested_message1*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(6);
        codedStream.WriteTag(WireFormatLite::MakeTag(100 /*repeated_int32_field*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(0);
        codedStream.WriteTag(WireFormatLite::MakeTag(100 /*repeated_int32_field*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        TEST_EPILOGUE(TMessage)
    }, "/repeated_nested_message1/1/repeated_int32_field/1");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        TEST_EPILOGUE(TMessageWithRequiredFields)
    }, "/required_field");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(3 /*nested_messages*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(0);
        TEST_EPILOGUE(TMessageWithRequiredFields)
    }, "/nested_messages/0/required_field");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(3 /*nested_messages*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(4);
        codedStream.WriteTag(WireFormatLite::MakeTag(2 /*required_field*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(0);
        codedStream.WriteTag(WireFormatLite::MakeTag(2 /*required_field*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(0);
        TEST_EPILOGUE(TMessageWithRequiredFields)
    }, "/nested_messages/0/required_field");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*int32_field_xxx*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(0);
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*int32_field_xxx*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(0);
        TEST_EPILOGUE(TMessage)
    }, "/int32_field");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*attributes*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(2);
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*attribute*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(0);
        TEST_EPILOGUE(TMessage)
    }, "/attributes");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*attributes*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(4);
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*attribute*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(2);
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*key*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(0);
        TEST_EPILOGUE(TMessage)
    }, "/attributes");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*attributes*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(4);
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*attribute*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(2);
        codedStream.WriteTag(WireFormatLite::MakeTag(2 /*value*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(0);
        TEST_EPILOGUE(TMessage)
    }, "/attributes");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*attributes*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(6);
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*attribute*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(4);
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*key*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(0);
        codedStream.WriteTag(WireFormatLite::MakeTag(1 /*key*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(0);
        TEST_EPILOGUE(TMessage)
    }, "/attributes");
}

TEST(TProtobufToYsonTest, UnknownFields)
{
    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(100 /*unknown*/, WireFormatLite::WIRETYPE_FIXED32));
        TEST_EPILOGUE(TMessage)
    }, "");

    {
        TProtobufParserOptions options;
        options.SkipUnknownFields = true;

        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(100 /*unknown*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(9);
        codedStream.WriteRaw("blablabla", 9);
        codedStream.WriteTag(WireFormatLite::MakeTag(15 /*nested_message1*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(3);
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*color*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(2 /*red*/);
        TEST_EPILOGUE_WITH_OPTIONS(TMessage, options)

        auto writtenNode = ConvertToNode(TYsonString(yson));
        auto expectedNode = BuildYsonNodeFluently()
            .BeginMap()
                .Item("nested_message1").BeginMap()
                    .Item("color").Value("red")
                .EndMap()
            .EndMap();
        EXPECT_TRUE(AreNodesEqual(writtenNode, expectedNode));
    }
}

TEST(TProtobufToYsonTest, ReservedFields)
{
    TEST_PROLOGUE()
    codedStream.WriteTag(WireFormatLite::MakeTag(100 /*unknown*/, WireFormatLite::WIRETYPE_VARINT));
    codedStream.WriteVarint64(0);
    TEST_EPILOGUE(TMessageWithReservedFields)
}

#undef TEST_PROLOGUE
#undef TEST_EPILOGUE
#undef TEST_EPILOGUE_WITH_OPTIONS

////////////////////////////////////////////////////////////////////////////////

template <class T>
void TestMessageByYPath(const TYPath& path)
{
    auto result = ResolveProtobufElementByYPath(ReflectProtobufMessageType<NYT::NProto::TMessage>(), path);
    EXPECT_TRUE(std::holds_alternative<std::unique_ptr<TProtobufMessageElement>>(result.Element));
    const auto& messageElement = std::get<std::unique_ptr<TProtobufMessageElement>>(result.Element);
    EXPECT_EQ(ReflectProtobufMessageType<T>(), messageElement->Type);
    EXPECT_EQ(path, result.HeadPath);
    EXPECT_EQ("", result.TailPath);
}

TEST(TResolveProtobufElementByYPath, Message)
{
    TestMessageByYPath<NYT::NProto::TMessage>("");
    TestMessageByYPath<NYT::NProto::TNestedMessage>("/nested_message1");
    TestMessageByYPath<NYT::NProto::TNestedMessage>("/repeated_nested_message1/0/nested_message");
    TestMessageByYPath<NYT::NProto::TNestedMessage>("/nested_message_map/k");
    TestMessageByYPath<NYT::NProto::TNestedMessage>("/nested_message_map/k/nested_message");
    TestMessageByYPath<NYT::NProto::TNestedMessage>("/nested_message_map/k/nested_message/nested_message_map/k");
}

////////////////////////////////////////////////////////////////////////////////

void TestScalarByYPath(const TYPath& path)
{
    auto result = ResolveProtobufElementByYPath(ReflectProtobufMessageType<NYT::NProto::TMessage>(), path);
    EXPECT_TRUE(std::holds_alternative<std::unique_ptr<TProtobufScalarElement>>(result.Element));
    EXPECT_EQ(path, result.HeadPath);
    EXPECT_EQ("", result.TailPath);
}

TEST(TResolveProtobufElementByYPath, Scalar)
{
    TestScalarByYPath("/uint32_field");
    TestScalarByYPath("/repeated_int32_field/123");
    TestScalarByYPath("/repeated_nested_message1/0/color");
    TestScalarByYPath("/nested_message_map/abc/int32_field");
    TestScalarByYPath("/int32_map/abc");
}

////////////////////////////////////////////////////////////////////////////////

void TestAttributeDictionaryByYPath(const TYPath& path, const TYPath& headPath)
{
    auto result = ResolveProtobufElementByYPath(ReflectProtobufMessageType<NYT::NProto::TMessage>(), path);
    EXPECT_TRUE(std::holds_alternative<std::unique_ptr<TProtobufAttributeDictionaryElement>>(result.Element));
    EXPECT_EQ(headPath, result.HeadPath);
    EXPECT_EQ(path.substr(headPath.length()), result.TailPath);
}

TEST(TResolveProtobufElementByYPath, AttributeDictionary)
{
    TestAttributeDictionaryByYPath("/attributes", "/attributes");
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
void TestAnyByYPath(const TYPath& path, const TYPath& headPath)
{
    TResolveProtobufElementByYPathOptions options;
    options.AllowUnknownYsonFields = true;
    auto result = ResolveProtobufElementByYPath(ReflectProtobufMessageType<T>(), path);
    EXPECT_TRUE(std::holds_alternative<std::unique_ptr<TProtobufAnyElement>>(result.Element));
    EXPECT_EQ(headPath, result.HeadPath);
    EXPECT_EQ(path.substr(headPath.length()), result.TailPath);
}

TEST(TResolveProtobufElementByYPath, Any)
{
    TestAnyByYPath<NYT::NProto::TMessage>("/yson_field", "/yson_field");
    TestAnyByYPath<NYT::NProto::TMessage>("/yson_field/abc", "/yson_field");
    TestAnyByYPath<NYT::NProto::TMessage>("/attributes/abc", "/attributes/abc");
    TestAnyByYPath<NYT::NProto::TMessage>("/attributes/abc/xyz", "/attributes/abc");
    TestAnyByYPath<NYT::NProto::TExtensibleMessage>("/hello", "/hello");
    TestAnyByYPath<NYT::NProto::TExtensibleMessage>("/hello/world", "/hello");
    TestAnyByYPath<NYT::NProto::TExtensibleMessage>("/known_submessage/hello", "/known_submessage/hello");
    TestAnyByYPath<NYT::NProto::TExtensibleMessage>("/known_submessage/hello/world", "/known_submessage/hello");
    TestAnyByYPath<NYT::NProto::TExtensibleMessage>("/known_submessages/123/hello", "/known_submessages/123/hello");
    TestAnyByYPath<NYT::NProto::TExtensibleMessage>("/known_submessages/123/hello/world", "/known_submessages/123/hello");
}

////////////////////////////////////////////////////////////////////////////////

void TestRepeatedByYPath(const TYPath& path)
{
    auto result = ResolveProtobufElementByYPath(ReflectProtobufMessageType<NYT::NProto::TMessage>(), path);
    EXPECT_TRUE(std::holds_alternative<std::unique_ptr<TProtobufRepeatedElement>>(result.Element));
    EXPECT_EQ(path, result.HeadPath);
    EXPECT_EQ("", result.TailPath);
}

TEST(TResolveProtobufElementByYPath, Repeated)
{
    TestRepeatedByYPath("/repeated_int32_field");
    TestRepeatedByYPath("/nested_message1/repeated_int32_field");
}

////////////////////////////////////////////////////////////////////////////////

void TestMapByYPath(const TYPath& path)
{
    auto result = ResolveProtobufElementByYPath(ReflectProtobufMessageType<NYT::NProto::TMessage>(), path);
    EXPECT_TRUE(std::holds_alternative<std::unique_ptr<TProtobufMapElement>>(result.Element));
    EXPECT_EQ(path, result.HeadPath);
    EXPECT_EQ("", result.TailPath);
}

TEST(TResolveProtobufElementByYPath, Map)
{
    TestMapByYPath("/int32_map");
    TestMapByYPath("/nested_message_map");
    TestMapByYPath("/nested_message_map/abc/nested_message_map");
    TestMapByYPath("/nested_message1/nested_message_map");
}

////////////////////////////////////////////////////////////////////////////////

#define DO(path, errorPath) \
    EXPECT_YPATH({ResolveProtobufElementByYPath(ReflectProtobufMessageType<NYT::NProto::TMessage>(), path);}, errorPath);

TEST(TResolveProtobufElementByYPath, Failure)
{
    DO("/repeated_int32_field/1/2", "/repeated_int32_field/1")
    DO("/missing", "/missing")
    DO("/repeated_nested_message1/1/xyz/abc", "/repeated_nested_message1/1/xyz")
}

#undef DO

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NYson
