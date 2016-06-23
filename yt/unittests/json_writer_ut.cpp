#include "framework.h"

#include <yt/ytlib/formats/json_writer.h>
#include <yt/ytlib/formats/config.h>

#include <yt/core/yson/consumer.h>

#include <util/string/base64.h>

namespace NYT {
namespace NFormats {
namespace {

using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

inline Stroka SurroundWithQuotes(const Stroka& s)
{
    Stroka quote = "\"";
    return quote + s + quote;
}

// Basic types:
TEST(TJsonWriterTest, List)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnBeginList();
        consumer->OnListItem();
        consumer->OnInt64Scalar(1);
        consumer->OnListItem();
        consumer->OnStringScalar("aaa");
        consumer->OnListItem();
        consumer->OnDoubleScalar(3.5);
    consumer->OnEndList();
    consumer->Flush();

    Stroka output = "[1,\"aaa\",3.5]";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, Map)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnBeginMap();
        consumer->OnKeyedItem("hello");
        consumer->OnStringScalar("world");
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\"hello\":\"world\",\"foo\":\"bar\"}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, DoubleMap)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream, NYson::EYsonType::ListFragment);

    consumer->OnListItem();
    consumer->OnBeginMap();
        consumer->OnKeyedItem("hello");
        consumer->OnStringScalar("world");
    consumer->OnEndMap();
    consumer->OnListItem();
    consumer->OnBeginMap();
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\"hello\":\"world\"}\n{\"foo\":\"bar\"}\n";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, ListFragmentWithEntity)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream, NYson::EYsonType::ListFragment);

    consumer->OnListItem();
    consumer->OnBeginAttributes();
        consumer->OnKeyedItem("x");
        consumer->OnStringScalar("y");
    consumer->OnEndAttributes();
    consumer->OnEntity();
    consumer->OnListItem();
    consumer->OnBeginMap();
        consumer->OnKeyedItem("hello");
        consumer->OnStringScalar("world");
    consumer->OnEndMap();
    consumer->OnListItem();
    consumer->OnBeginMap();
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\"$attributes\":{\"x\":\"y\"},\"$value\":null}\n{\"hello\":\"world\"}\n{\"foo\":\"bar\"}\n";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, Entity)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnEntity();
    consumer->Flush();

    Stroka output = "null";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, EmptyString)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnStringScalar("");
    consumer->Flush();

    Stroka output = SurroundWithQuotes("");
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, AsciiString)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    Stroka s = Stroka("\x7F\x32", 2);
    consumer->OnStringScalar(s);
    consumer->Flush();

    Stroka output = SurroundWithQuotes(s);
    EXPECT_EQ(output, outputStream.Str());
}


TEST(TJsonWriterTest, NonAsciiString)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    Stroka s = Stroka("\xFF\x00\x80", 3);
    consumer->OnStringScalar(s);
    consumer->Flush();

    Stroka output = SurroundWithQuotes("\xC3\xBF\\u0000\xC2\x80");
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, NonAsciiStringWithoutEscaping)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->EncodeUtf8 = false;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    Stroka s = Stroka("\xC3\xBF", 2);
    consumer->OnStringScalar(s);
    consumer->Flush();

    Stroka output = SurroundWithQuotes(Stroka("\xC3\xBF", 2));
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, IncorrectUtfWithoutEscaping)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->EncodeUtf8 = false;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    Stroka s = Stroka("\xFF", 1);
    EXPECT_ANY_THROW(
        consumer->OnStringScalar(s);
    );
}

TEST(TJsonWriterTest, StringStartingWithSpecailSymbol)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    Stroka s = "&some_string";
    consumer->OnStringScalar(s);
    consumer->Flush();

    Stroka output = SurroundWithQuotes(s);
    EXPECT_EQ(output, outputStream.Str());
}

////////////////////////////////////////////////////////////////////////////////

// Values with attributes:
TEST(TJsonWriterTest, ListWithAttributes)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnBeginAttributes();
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndAttributes();

    consumer->OnBeginList();
        consumer->OnListItem();
        consumer->OnInt64Scalar(1);
    consumer->OnEndList();
    consumer->Flush();

    Stroka output =
        "{"
            "\"$attributes\":{\"foo\":\"bar\"}"
            ","
            "\"$value\":[1]"
        "}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, MapWithAttributes)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnBeginAttributes();
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndAttributes();

    consumer->OnBeginMap();
        consumer->OnKeyedItem("spam");
        consumer->OnStringScalar("bad");
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output =
        "{"
            "\"$attributes\":{\"foo\":\"bar\"}"
            ","
            "\"$value\":{\"spam\":\"bad\"}"
        "}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, Int64WithAttributes)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnBeginAttributes();
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndAttributes();

    consumer->OnInt64Scalar(42);
    consumer->Flush();

    Stroka output =
        "{"
            "\"$attributes\":{\"foo\":\"bar\"}"
            ","
            "\"$value\":42"
        "}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, Uint64)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnUint64Scalar(42);
    consumer->Flush();

    Stroka output = "42";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, EntityWithAttributes)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnBeginAttributes();
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndAttributes();

    consumer->OnEntity();
    consumer->Flush();

    Stroka output =
        "{"
            "\"$attributes\":{\"foo\":\"bar\"}"
            ","
            "\"$value\":null"
        "}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, StringWithAttributes)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnBeginAttributes();
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndAttributes();

    consumer->OnStringScalar("some_string");
    consumer->Flush();

    Stroka output =
        "{"
            "\"$attributes\":{\"foo\":\"bar\"}"
            ","
            "\"$value\":\"some_string\""
        "}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, DoubleAttributes)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnBeginAttributes();
        consumer->OnKeyedItem("foo");
        consumer->OnBeginAttributes();
            consumer->OnKeyedItem("another_foo");
            consumer->OnStringScalar("another_bar");
        consumer->OnEndAttributes();
        consumer->OnStringScalar("bar");
    consumer->OnEndAttributes();

    consumer->OnStringScalar("some_string");
    consumer->Flush();

    Stroka output =
        "{"
            "\"$attributes\":{\"foo\":"
                "{"
                    "\"$attributes\":{\"another_foo\":\"another_bar\"}"
                    ","
                    "\"$value\":\"bar\"}"
                "}"
            ","
            "\"$value\":\"some_string\""
        "}";
    EXPECT_EQ(output, outputStream.Str());
}

////////////////////////////////////////////////////////////////////////////////

TEST(TJsonWriterTest, NeverAttributes)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->AttributesMode = EJsonAttributesMode::Never;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    consumer->OnBeginAttributes();
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndAttributes();

    consumer->OnBeginMap();
        consumer->OnKeyedItem("answer");
        consumer->OnInt64Scalar(42);

        consumer->OnKeyedItem("question");
        consumer->OnBeginAttributes();
            consumer->OnKeyedItem("foo");
            consumer->OnStringScalar("bar");
        consumer->OnEndAttributes();
        consumer->OnStringScalar("strange question");
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output =
        "{"
            "\"answer\":42,"
            "\"question\":\"strange question\""
        "}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, AlwaysAttributes)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->AttributesMode = EJsonAttributesMode::Always;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    consumer->OnBeginAttributes();
        consumer->OnKeyedItem("foo");
        consumer->OnStringScalar("bar");
    consumer->OnEndAttributes();

    consumer->OnBeginMap();
        consumer->OnKeyedItem("answer");
        consumer->OnInt64Scalar(42);

        consumer->OnKeyedItem("question");
        consumer->OnBeginAttributes();
            consumer->OnKeyedItem("foo");
            consumer->OnStringScalar("bar");
        consumer->OnEndAttributes();
        consumer->OnStringScalar("strange question");
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output =
        "{"
            "\"$attributes\":{\"foo\":{\"$attributes\":{},\"$value\":\"bar\"}},"
            "\"$value\":"
            "{"
                "\"answer\":{\"$attributes\":{},\"$value\":42},"
                "\"question\":"
                "{"
                    "\"$attributes\":{\"foo\":{\"$attributes\":{},\"$value\":\"bar\"}},"
                    "\"$value\":\"strange question\""
                "}"
            "}"
        "}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, SpecialKeys)
{
    TStringStream outputStream;
    auto consumer = CreateJsonConsumer(&outputStream);

    consumer->OnBeginMap();
        consumer->OnKeyedItem("$value");
        consumer->OnStringScalar("foo");
        consumer->OnKeyedItem("$$attributes");
        consumer->OnStringScalar("bar");
        consumer->OnKeyedItem("$other");
        consumer->OnInt64Scalar(42);
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\"$$value\":\"foo\",\"$$$attributes\":\"bar\",\"$$other\":42}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, TestStringLengthLimit)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->StringLengthLimit = 2;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    consumer->OnBeginMap();
        consumer->OnKeyedItem("hello");
        consumer->OnStringScalar(Stroka(10000, 'A'));
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\"hello\":{\"$incomplete\":true,\"$value\":\"AA\"}}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, TestAnnotateWithTypes)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->AnnotateWithTypes = true;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    consumer->OnBeginMap();
        consumer->OnKeyedItem("hello");
        consumer->OnStringScalar("world");
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\"hello\":{\"$type\":\"string\",\"$value\":\"world\"}}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, TestAnnotateWithTypesStringify)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->AnnotateWithTypes = true;
    config->Stringify = true;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    consumer->OnBeginMap();
        consumer->OnKeyedItem("hello");
        consumer->OnUint64Scalar(-1);
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\"hello\":{\"$type\":\"uint64\",\"$value\":\"18446744073709551615\"}}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, SeveralOptions)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->StringLengthLimit = 2;
    config->AnnotateWithTypes = true;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    consumer->OnBeginMap();
        consumer->OnKeyedItem("hello");
        consumer->OnStringScalar(Stroka(10000, 'A'));
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\"hello\":{\"$incomplete\":true,\"$type\":\"string\",\"$value\":\"AA\"}}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, SeveralOptions2)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->StringLengthLimit = 4;
    config->AnnotateWithTypes = true;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    consumer->OnBeginMap();
        consumer->OnKeyedItem("hello");
        consumer->OnBeginAttributes();
            consumer->OnKeyedItem("mood");
            consumer->OnInt64Scalar(42);
        consumer->OnEndAttributes();
        consumer->OnStringScalar(Stroka(10000, 'A'));
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\"hello\":{\"$attributes\":{\"mood\":{\"$type\":\"int64\",\"$value\":42}},"
        "\"$incomplete\":true,\"$type\":\"string\",\"$value\":\"AAAA\"}}";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, SeveralOptionsFlushBuffer)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->StringLengthLimit = 2;
    config->AnnotateWithTypes = true;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::ListFragment, config);

    consumer->OnListItem();
    consumer->OnStringScalar(Stroka(10000, 'A'));
    consumer->Flush();

    Stroka output = "{\"$incomplete\":true,\"$type\":\"string\",\"$value\":\"AA\"}\n";
    EXPECT_EQ(output, outputStream.Str());
}

TEST(TJsonWriterTest, DISABLED_TestPrettyFormat)
{
    TStringStream outputStream;
    auto config = New<TJsonFormatConfig>();
    config->Format = EJsonFormat::Pretty;
    auto consumer = CreateJsonConsumer(&outputStream, EYsonType::Node, config);

    consumer->OnBeginMap();
        consumer->OnKeyedItem("hello");
        consumer->OnInt64Scalar(1);
    consumer->OnEndMap();
    consumer->Flush();

    Stroka output = "{\n"
                    "    \"hello\": 1\n"
                    "}";
    EXPECT_EQ(output, outputStream.Str());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NFormats
} // namespace NYT
