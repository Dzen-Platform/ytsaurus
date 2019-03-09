#include <yt/core/test_framework/framework.h>

#include <yt/core/logging/log.h>
#include <yt/core/logging/log_manager.h>
#include <yt/core/logging/writer.h>
#include <yt/core/logging/random_access_gzip.h>

#include <yt/core/json/json_parser.h>

#include <yt/core/tracing/trace_context.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/ytree/convert.h>

#include <util/system/fs.h>

#include <util/stream/zlib.h>

#ifdef _unix_
#include <unistd.h>
#endif

namespace NYT::NLogging {

using namespace NYTree;
using namespace NYson;
using namespace NJson;

////////////////////////////////////////////////////////////////////////////////

class TLoggingTest
    : public ::testing::Test
{
public:
    TLoggingTest()
        : SomeDate("2014-04-24 23:41:09,804")
        , DateLength(SomeDate.length())
        , Logger("Test")
    {
        Category.Name = "category";
    }

protected:
    TLoggingCategory Category;
    TString SomeDate;
    int DateLength;

    TLogger Logger;

    IMapNodePtr DeserializeJson(const TString& source)
    {
        auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
        builder->BeginTree();
        TStringStream stream(source);
        ParseJson(&stream, builder.get());
        return builder->EndTree()->AsMap();
    }

    void WritePlainTextEvent(ILogWriter* writer) {
        TLogEvent event;
        event.MessageFormat = ELogMessageFormat::PlainText;
        event.Category = &Category;
        event.Level = ELogLevel::Debug;
        event.Message = TSharedRef::FromString("message");
        event.ThreadId = 0xba;

        WriteEvent(writer, event);
    }

    void WriteEvent(ILogWriter* writer, const TLogEvent& event)
    {
        writer->Write(event);
        writer->Flush();
    }

    std::vector<TString> ReadFile(const TString& fileName, bool compressed = false)
    {
        std::vector<TString> lines;

        TString line;
        auto input = TUnbufferedFileInput(fileName);
        if (!compressed) {
            while (input.ReadLine(line)) {
                lines.push_back(line + "\n");
            }
        } else {
            TZLibDecompress decompressor(&input);
            while (decompressor.ReadLine(line)) {
                lines.push_back(line + "\n");
            }
        }

        return lines;
    }
};

#ifndef _win_

TEST_F(TLoggingTest, ReloadsOnSigHup)
{
    YT_LOG_INFO("Preparing logging thread");
    Sleep(TDuration::MilliSeconds(100)); // In sleep() we trust.

    int version = TLogManager::Get()->GetVersion();

    kill(getpid(), SIGHUP);

    YT_LOG_INFO("Awaking logging thread");
    Sleep(TDuration::Seconds(1)); // In sleep() we trust.

    int newVersion = TLogManager::Get()->GetVersion();

    EXPECT_NE(version, newVersion);
}

#endif

TEST_F(TLoggingTest, FileWriter)
{
    NFs::Remove("test.log");

    TIntrusivePtr<TFileLogWriter> writer;
    writer = New<TFileLogWriter>(std::make_unique<TPlainTextLogFormatter>(), "test_writer", "test.log", false);
    WritePlainTextEvent(writer.Get());

    {
        auto lines = ReadFile("test.log");
        EXPECT_EQ(2, lines.size());
        EXPECT_TRUE(lines[0].find("Logging started") != -1);
        EXPECT_EQ("\tD\tcategory\tmessage\tba\t\t\n", lines[1].substr(DateLength, lines[1].size()));
    }

    writer->Reload();
    WritePlainTextEvent(writer.Get());

    {
        auto lines = ReadFile("test.log");
        EXPECT_EQ(5, lines.size());
        EXPECT_TRUE(lines[0].find("Logging started") != -1);
        EXPECT_EQ("\tD\tcategory\tmessage\tba\t\t\n", lines[1].substr(DateLength));
        EXPECT_EQ("\n", lines[2]);
        EXPECT_TRUE(lines[3].find("Logging started") != -1);
        EXPECT_EQ("\tD\tcategory\tmessage\tba\t\t\n", lines[4].substr(DateLength));
    }

    NFs::Remove("test.log");
}

TEST_F(TLoggingTest, Compression)
{
    NFs::Remove("test.log.gz");

    TIntrusivePtr<TFileLogWriter> writer;
    writer = New<TFileLogWriter>(std::make_unique<TPlainTextLogFormatter>(), "test_writer", "test.log.gz", true);
    WritePlainTextEvent(writer.Get());

    writer->Reload();

    {
        auto lines = ReadFile("test.log.gz", true);
        EXPECT_EQ(2, lines.size());
        EXPECT_TRUE(lines[0].find("Logging started") != -1);
        EXPECT_EQ("\tD\tcategory\tmessage\tba\t\t\n", lines[1].substr(DateLength, lines[1].size()));
    }

    NFs::Remove("test.log.gz");
}

TEST_F(TLoggingTest, StreamWriter)
{
    TStringStream stringOutput;
    auto writer = New<TStreamLogWriter>(&stringOutput, std::make_unique<TPlainTextLogFormatter>(), "test_writer");

    WritePlainTextEvent(writer.Get());

    EXPECT_EQ(
       "\tD\tcategory\tmessage\tba\t\t\n",
       stringOutput.Str().substr(DateLength));
}

TEST_F(TLoggingTest, Rule)
{
    auto rule = New<TRuleConfig>();
    rule->Load(ConvertToNode(TYsonString(
        R"({
            exclude_categories = [ bus ];
            min_level = info;
            writers = [ some_writer ];
        })")));

    EXPECT_TRUE(rule->IsApplicable("some_service", ELogMessageFormat::PlainText));
    EXPECT_FALSE(rule->IsApplicable("bus", ELogMessageFormat::PlainText));
    EXPECT_FALSE(rule->IsApplicable("bus", ELogLevel::Debug, ELogMessageFormat::PlainText));
    EXPECT_FALSE(rule->IsApplicable("some_service", ELogLevel::Debug, ELogMessageFormat::PlainText));
    EXPECT_TRUE(rule->IsApplicable("some_service", ELogLevel::Warning, ELogMessageFormat::PlainText));
    EXPECT_TRUE(rule->IsApplicable("some_service", ELogLevel::Info, ELogMessageFormat::PlainText));
}

TEST_F(TLoggingTest, LogManager)
{
    NFs::Remove("test.log");
    NFs::Remove("test.error.log");

    auto configText = R"({
        rules = [
            {
                "min_level" = "info";
                "writers" = [ "info" ];
            };
            {
                "min_level" = "error";
                "writers" = [ "error" ];
            };
        ];
        "writers" = {
            "error" = {
                "file_name" = "test.error.log";
                "type" = "file";
            };
            "info" = {
                "file_name" = "test.log";
                "type" = "file";
            };
        };
    })";

    auto configNode = ConvertToNode(TYsonString(configText));

    auto config = ConvertTo<TLogConfigPtr>(configNode);

    TLogManager::Get()->Configure(config);

    YT_LOG_DEBUG("Debug message");
    YT_LOG_INFO("Info message");
    YT_LOG_ERROR("Error message");

    TLogManager::Get()->Synchronize();

    auto infoLog = ReadFile("test.log");
    auto errorLog = ReadFile("test.error.log");

    EXPECT_EQ(3, infoLog.size());
    EXPECT_EQ(2, errorLog.size());

    NFs::Remove("test.log");
    NFs::Remove("test.error.log");
}

TEST_F(TLoggingTest, StructuredJsonLogging)
{
    NFs::Remove("test.log");

    TLogEvent event;
    event.MessageFormat = ELogMessageFormat::Structured;
    event.Category = &Category;
    event.Level = ELogLevel::Debug;
    event.StructuredMessage = NYTree::BuildYsonStringFluently<EYsonType::MapFragment>()
        .Item("message")
        .Value("test_message")
        .Finish();

    auto writer = New<TFileLogWriter>(std::make_unique<TJsonLogFormatter>(), "test_writer", "test.log");
    WriteEvent(writer.Get(), event);
    TLogManager::Get()->Synchronize();

    auto log = ReadFile("test.log");

    auto logStartedJson = DeserializeJson(log[0]);
    EXPECT_EQ(logStartedJson->GetChild("message")->AsString()->GetValue(), "Logging started");
    EXPECT_EQ(logStartedJson->GetChild("level")->AsString()->GetValue(), "info");
    EXPECT_EQ(logStartedJson->GetChild("category")->AsString()->GetValue(), "Logging");

    auto contentJson = DeserializeJson(log[1]);
    EXPECT_EQ(contentJson->GetChild("message")->AsString()->GetValue(), "test_message");
    EXPECT_EQ(contentJson->GetChild("level")->AsString()->GetValue(), "debug");
    EXPECT_EQ(contentJson->GetChild("category")->AsString()->GetValue(), "category");

    NFs::Remove("test.log");
}

TEST(TRandomAccessGZipTest, Write)
{
    NFs::Remove("test.txt.gz");

    {
        TFile rawFile("test.txt.gz", OpenAlways|RdWr|CloseOnExec);
        TRandomAccessGZipFile file(&rawFile);
        file << "foo\n";
        file.Flush();
        file << "bar\n";
        file.Finish();
    }
    {
        TFile rawFile("test.txt.gz", OpenAlways|RdWr|CloseOnExec);
        TRandomAccessGZipFile file(&rawFile);
        file << "zog\n";
        file.Finish();
    }

    auto input = TUnbufferedFileInput("test.txt.gz");
    TZLibDecompress decompress(&input);
    EXPECT_EQ("foo\nbar\nzog\n", decompress.ReadAll());

    NFs::Remove("test.txt.gz");
}

TEST(TRandomAccessGZipTest, RepairIncompleteBlocks)
{
    NFs::Remove("test.txt.gz");
    {
        TFile rawFile("test.txt.gz", OpenAlways|RdWr|CloseOnExec);
        TRandomAccessGZipFile file(&rawFile);
        file << "foo\n";
        file.Flush();
        file << "bar\n";
        file.Finish();
    }

    i64 fullSize;
    {
        TFile file("test.txt.gz", OpenAlways|RdWr);
        fullSize = file.GetLength();
        file.Resize(fullSize - 1);
    }

    {
        TFile rawFile("test.txt.gz", OpenAlways | RdWr | CloseOnExec);
        TRandomAccessGZipFile file(&rawFile);
    }

    {
        TFile file("test.txt.gz", OpenAlways|RdWr);
        EXPECT_LE(file.GetLength(), fullSize - 1);
    }

    NFs::Remove("test.txt.gz");
}

// This test is for manual check of YT_LOG_FATAL
TEST_F(TLoggingTest, DISABLED_LogFatal)
{
    NFs::Remove("test.log");
    NFs::Remove("test.error.log");

    auto configText = R"({
        rules = [
            {
                "min_level" = "info";
                "writers" = [ "info" ];
            };
        ];
        "writers" = {
            "info" = {
                "file_name" = "test.log";
                "type" = "file";
            };
        };
    })";

    auto configNode = ConvertToNode(TYsonString(configText));

    auto config = ConvertTo<TLogConfigPtr>(configNode);

    TLogManager::Get()->Configure(config);

    YT_LOG_INFO("Info message");

    Sleep(TDuration::MilliSeconds(100));

    YT_LOG_INFO("Info message");
    YT_LOG_FATAL("FATAL");

    NFs::Remove("test.log");
    NFs::Remove("test.error.log");
}

TEST_F(TLoggingTest, TraceSuppression)
{
    NFs::Remove("test.log");

    auto configText = R"({
        rules = [
            {
                "min_level" = "info";
                "writers" = [ "info" ];
            };
        ];
        "writers" = {
            "info" = {
                "file_name" = "test.log";
                "type" = "file";
            };
        };
        "trace_suppression_timeout" = 100;
    })";

    auto configNode = ConvertToNode(TYsonString(configText));

    auto config = ConvertTo<TLogConfigPtr>(configNode);

    TLogManager::Get()->Configure(config);

    {
        auto traceContext = NTracing::CreateRootTraceContext();
        NTracing::TTraceContextGuard guard(traceContext);

        YT_LOG_INFO("Traced message");

        TLogManager::Get()->SuppressTrace(traceContext.GetTraceId());
    }

    YT_LOG_INFO("Info message");

    TLogManager::Get()->Synchronize();

    auto lines = ReadFile("test.log");

    EXPECT_EQ(2, lines.size());
    EXPECT_TRUE(lines[0].find("Logging started") != -1);
    EXPECT_TRUE(lines[1].find("Info message") != -1);

    NFs::Remove("test.log");
}

TEST_F(TLoggingTest, LongMessages)
{
    NFs::Remove("test.log");

    auto configText = R"({
        rules = [
            {
                "min_level" = "info";
                "writers" = [ "info" ];
            };
        ];
        "writers" = {
            "info" = {
                "file_name" = "test.log";
                "type" = "file";
            };
        };
    })";

    auto configNode = ConvertToNode(TYsonString(configText));
    auto config = ConvertTo<TLogConfigPtr>(configNode);
    TLogManager::Get()->Configure(config);

    constexpr int N = 500;
    std::vector<TString> chunks;
    for (int i = 0; i < N; ++i) {
        chunks.push_back(Format("PayloadPayloadPayloadPayloadPayload%v", i));
    }

    for (int i = 0; i < N; ++i) {
        YT_LOG_INFO("%v", MakeRange(chunks.data(), chunks.data() + i));
    }

    TLogManager::Get()->Synchronize();

    auto infoLog = ReadFile("test.log");
    EXPECT_EQ(N + 1, infoLog.size());
    for (int i = 0; i < N; ++i) {
        auto expected = Format("%v", MakeRange(chunks.data(), chunks.data() + i));
        auto actual = infoLog[i + 1];
        EXPECT_NE(TString::npos, actual.find(expected));
    }

    NFs::Remove("test.log");
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NLogging
