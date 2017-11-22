#include <yt/core/test_framework/framework.h>

#include <yt/core/unittests/proto/protobuf_yson_ut.pb.h>

#include <yt/core/yson/protobuf_interop.h>
#include <yt/core/yson/null_consumer.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/misc/string.h>

#include <contrib/libs/protobuf/io/coded_stream.h>
#include <contrib/libs/protobuf/io/zero_copy_stream_impl_lite.h>

#include <contrib/libs/protobuf/wire_format.h>

namespace NYT {
namespace NYson {
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

#define TEST_PROLOGUE(type) \
    TString str; \
    StringOutputStream output(&str); \
    auto protobufWriter = CreateProtobufWriter(&output, ReflectProtobufMessageType<NYT::NProto::type>()); \
    BuildYsonFluently(protobufWriter.get())

#define TEST_EPILOGUE() \
    Cerr << ToHex(str) << Endl; \
    NYT::NProto::TMessage message; \
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
            .Item("repeated_nested_message").BeginList()
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
        .EndMap();


    TEST_EPILOGUE()
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

    EXPECT_EQ(2, message.repeated_nested_message().size());
    EXPECT_EQ(456, message.repeated_nested_message().Get(0).int32_field());
    EXPECT_EQ(654, message.repeated_nested_message().Get(1).int32_field());

    EXPECT_EQ(3, message.attributes().attributes_size());
    EXPECT_EQ("k1", message.attributes().attributes(0).key());
    EXPECT_EQ(ConvertToYsonString(1).GetData(), message.attributes().attributes(0).value());
    EXPECT_EQ("k2", message.attributes().attributes(1).key());
    EXPECT_EQ(ConvertToYsonString("test").GetData(), message.attributes().attributes(1).value());
    EXPECT_EQ("k3", message.attributes().attributes(2).key());
    EXPECT_EQ(ConvertToYsonString(std::vector<int>{1, 2, 3}).GetData(), message.attributes().attributes(2).value());
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

    TEST_EPILOGUE()
    EXPECT_EQ(10000, message.int32_field_xxx());
    EXPECT_EQ(10000U, message.uint32_field());
    EXPECT_EQ(10000, message.sint32_field());
    EXPECT_EQ(10000, message.int64_field());
    EXPECT_EQ(10000U, message.uint64_field());
    EXPECT_EQ(10000U, message.fixed32_field());
    EXPECT_EQ(10000U, message.fixed64_field());
}

TEST(TYsonToProtobufTest, Failure)
{
    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .Value(0);
    }, "/");

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
                    .Item("int32_field").Entity()
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
                .Item("repeated_nested_message").BeginList()
                    .Item().BeginMap()
                        .Item("color").Value("blue")
                    .EndMap()
                    .Item().BeginMap()
                        .Item("color").Value("black")
                    .EndMap()
                .EndList()
            .EndMap();
    }, "/repeated_nested_message/1/color");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("repeated_nested_message").BeginList()
                    .Item().BeginList()
                    .EndList()
                .EndList()
            .EndMap();
    }, "/repeated_nested_message/0");

    EXPECT_YPATH({
        TEST_PROLOGUE(TMessage)
            .BeginMap()
                .Item("repeated_nested_message").BeginList()
                    .Item().BeginMap()
                        .Item("color").Value("black")
                    .EndMap()
                .EndList()
            .EndMap();
    }, "/repeated_nested_message/0/color");

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
}

#undef TEST_PROLOGUE
#undef TEST_EPILOGUE

////////////////////////////////////////////////////////////////////////////////

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
        auto* proto = message.add_repeated_nested_message();
        proto->set_int32_field(456);
        proto->add_repeated_int32_field(1);
        proto->add_repeated_int32_field(2);
        proto->add_repeated_int32_field(3);
    }
    {
        auto* proto = message.add_repeated_nested_message();
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

    auto serialized = SerializeProtoToRef(message);

    ArrayInputStream inputStream(serialized.Begin(), serialized.Size());
    TString yson;
    TStringOutput outputStream(yson);
    TYsonWriter writer(&outputStream, EYsonFormat::Pretty);
    ParseProtobuf(&writer, &inputStream, ReflectProtobufMessageType<NYT::NProto::TMessage>());
    Cerr << yson << Endl;

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
            .Item("repeated_nested_message").BeginList()
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
        .EndMap();
    EXPECT_TRUE(AreNodesEqual(writtenNode, expectedNode));
}

#define TEST_PROLOGUE() \
    TString serialized; \
    StringOutputStream outputStream(&serialized); \
    CodedOutputStream codedStream(&outputStream);

#define TEST_EPILOGUE(type) \
    codedStream.Trim(); \
    Cerr << ToHex(serialized) << Endl; \
    ArrayInputStream inputStream(serialized.data(), serialized.length()); \
    ParseProtobuf(GetNullYsonConsumer(), &inputStream, ReflectProtobufMessageType<NYT::NProto::type>()); \

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
        codedStream.WriteTag(WireFormatLite::MakeTag(18 /*repeated_nested_message*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(3);
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*color*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(2);
        codedStream.WriteTag(WireFormatLite::MakeTag(18 /*repeated_nested_message*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(3);
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*color*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(4);
        TEST_EPILOGUE(TMessage)
    }, "/repeated_nested_message/1/color");

    EXPECT_YPATH({
        TEST_PROLOGUE()
        codedStream.WriteTag(WireFormatLite::MakeTag(18 /*repeated_nested_message*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(3);
        codedStream.WriteTag(WireFormatLite::MakeTag(19 /*color*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(2);
        codedStream.WriteTag(WireFormatLite::MakeTag(18 /*repeated_nested_message*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        codedStream.WriteVarint64(6);
        codedStream.WriteTag(WireFormatLite::MakeTag(100 /*repeated_int32_field*/, WireFormatLite::WIRETYPE_VARINT));
        codedStream.WriteVarint64(0);
        codedStream.WriteTag(WireFormatLite::MakeTag(100 /*repeated_int32_field*/, WireFormatLite::WIRETYPE_LENGTH_DELIMITED));
        TEST_EPILOGUE(TMessage)
    }, "/repeated_nested_message/1/repeated_int32_field/1");

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

#undef TEST_PROLOGUE
#undef TEST_EPILOGUE

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYson
} // namespace NYT
