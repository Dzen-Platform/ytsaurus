#include "modes.h"

#include <mapreduce/yt/library/blob/tools/file-yt/protos/config.pb.h>

#include <mapreduce/yt/library/blob/api.h>
#include <mapreduce/yt/library/blob/protos/info.pb.h>

#include <mapreduce/yt/common/log.h>
#include <mapreduce/yt/interface/client.h>

#include <library/getopt/small/last_getopt.h>
#include <library/streams/factory/factory.h>

#include <util/generic/guid.h>
#include <util/generic/vector.h>

#include <cstdlib>

static NFileYtTool::TListConfig ParseOptions(const int argc, const char* argv[]) {
    NFileYtTool::TListConfig c;
    auto p = NLastGetopt::TOpts::Default();
    p.SetTitle("list files in YT table, will write JSONs containing info about documents");
    p.AddLongOption('p', "yt-proxy")
     .Required()
     .RequiredArgument("YT_PROXY")
     .Handler1T<TString>([&c](const auto& v) {
         c.SetProxy(v);
     })
     .Help("YT cluster proxy");
    p.AddLongOption('t', "yt-table")
     .Required()
     .RequiredArgument("TABLE")
     .Handler1T<TString>([&c](const auto& v) {
         c.SetTable(v);
     })
     .Help("table with files");
    p.AddLongOption("tx")
     .Optional()
     .RequiredArgument("GUID")
     .Handler1T<TString>([&c](const auto& v) {
         c.SetTransactionID(v);
     })
     .Help("transaction ID to attach");
    p.AddLongOption('o', "output")
     .DefaultValue("-")
     .RequiredArgument("FILE")
     .Handler1T<TString>([&c](const auto& v) {
         c.SetOutputFile(v);
     });
    p.SetFreeArgsNum(0);
    NLastGetopt::TOptsParseResult{&p, argc, argv};
    c.CheckInitialized();
    return c;
}

static int Main(const NFileYtTool::TListConfig& config) {
    NYT::SetLogger(NYT::CreateStdErrLogger(NYT::ILogger::INFO));
    NYT::IClientBasePtr client = NYT::CreateClient(config.GetProxy());
    if (config.HasTransactionID()) {
        const auto transactionId = [&config]{
            TGUID guid;
            Y_ENSURE(GetGuid(config.GetTransactionID(), guid));
            return guid;
        }();
        client = dynamic_cast<NYT::IClient*>(client.Get())->AttachTransaction(transactionId);
    }

    const auto output = OpenOutput(config.GetOutputFile());
    for (const auto& info : NYtBlob::List(config.GetTable(), client)) {
        *output << info.AsJSON() << '\n';
    }

    return EXIT_SUCCESS;
}

static int Main(const int argc, const char* argv[]) {
    const auto c = ParseOptions(argc, argv);
    return Main(c);
}

int NFileYtTool::MainList(const int argc, const char* argv[]) {
    return ::Main(argc, argv);
}

