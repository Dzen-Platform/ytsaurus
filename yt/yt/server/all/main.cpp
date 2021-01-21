#include <yt/server/master/cell_master/program.h>
#include <yt/server/clock_server/cluster_clock/program.h>
#include <yt/server/http_proxy/program.h>
#include <yt/server/rpc_proxy/program.h>
#include <yt/server/job_proxy/program.h>
#include <yt/server/scheduler/program.h>
#include <yt/server/controller_agent/program.h>
#include <yt/server/tools/program.h>
#include <yt/server/node/cluster_node/program.h>
#include <yt/server/exec/program.h>
#include <yt/server/log_tailer/program.h>
#include <yt/server/discovery_server/program.h>

#include <yt/ytlib/program/program.h>

#include <library/cpp/getopt/small/last_getopt_parse_result.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TAllProgram
    : public TProgram
{
public:
    int Run(int argc, const char** argv)
    {
        NLastGetopt::TOptsParseResult result(&Opts_, argc, argv);

        return 0;
    }

private:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& /* result */) override
    {
        YT_ABORT();
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

int main(int argc, const char** argv)
{
    std::vector<std::pair<TString, std::function<int()>>> programs = {
        {"ytserver-master", [&] { return NYT::TCellMasterProgram().Run(argc, argv); }},
        {"ytserver-clock", [&] { return NYT::TClusterClockProgram().Run(argc, argv); }},
        {"ytserver-http-proxy", [&] { return NYT::THttpProxyProgram().Run(argc, argv); }},
        {"ytserver-proxy", [&] { return NYT::NRpcProxy::TRpcProxyProgram().Run(argc, argv); }},
        {"ytserver-node", [&] { return NYT::NClusterNode::TClusterNodeProgram().Run(argc, argv); }},
        {"ytserver-job-proxy", [&] { return NYT::NJobProxy::TJobProxyProgram().Run(argc, argv); }},
        {"ytserver-exec", [&] { return NYT::NExec::TExecProgram().Run(argc, argv); }},
        {"ytserver-tools", [&] { return NYT::TToolsProgram().Run(argc, argv); }},
        {"ytserver-scheduler", [&] { return NYT::NScheduler::TSchedulerProgram().Run(argc, argv); }},
        {"ytserver-controller-agent", [&] { return NYT::NControllerAgent::TControllerAgentProgram().Run(argc, argv); }},
        {"ytserver-log-tailer", [&] { return NYT::NLogTailer::TLogTailerProgram().Run(argc, argv); }},
        {"ytserver-discovery", [&] { return NYT::NClusterDiscoveryServer::TClusterDiscoveryServerProgram().Run(argc, argv); }},
    };

    for (const auto program : programs) {
        if (TStringBuf(argv[0]).EndsWith(program.first)) {
            return program.second();
        }
    }

    // Handles auxiliary flags like --version and --build.
    NYT::TAllProgram().Run(argc, argv);

    Cerr << "Program " << argv[0] << " is not known" << Endl;
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
