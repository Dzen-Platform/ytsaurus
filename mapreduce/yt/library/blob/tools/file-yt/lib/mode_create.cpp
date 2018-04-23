#include "modes.h"

#include <mapreduce/yt/library/blob/tools/file-yt/protos/config.pb.h>

#include <mapreduce/yt/library/blob/api.h>

#include <mapreduce/yt/interface/client.h>

#include <mapreduce/yt/interface/logging/log.h>

#include <library/getopt/small/last_getopt.h>

#include <util/generic/guid.h>

#include <cstdlib>

static NFileYtTool::TCreateConfig ParseOptions(const int argc, const char* argv[]) {
    NFileYtTool::TCreateConfig c;
    auto p = NLastGetopt::TOpts::Default();
    p.SetTitle("create YT table for further files upload");
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
     .Help("table name");
    p.AddLongOption("tx")
     .Optional()
     .RequiredArgument("GUID")
     .Handler1T<TString>([&c](const auto& v) {
         c.SetTransactionID(v);
     })
     .Help("transaction ID to attach");
    p.SetFreeArgsNum(0);
    NLastGetopt::TOptsParseResult{&p, argc, argv};
    c.CheckInitialized();
    return c;
}

static int Main(const NFileYtTool::TCreateConfig& config) {
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

    NYtBlob::CreateTable(config.GetTable(), client);

    return EXIT_SUCCESS;
}

static int Main(const int argc, const char* argv[]) {
    const auto c = ParseOptions(argc, argv);
    return Main(c);
}

int NFileYtTool::MainCreate(const int argc, const char* argv[]) {
    return ::Main(argc, argv);
}
