#include "bootstrap.h"
#include "config.h"

#include <yt/server/lib/misc/cluster_connection.h>

#include <yt/ytlib/auth/config.h>

#include <yt/ytlib/program/program.h>
#include <yt/ytlib/program/program_config_mixin.h>
#include <yt/ytlib/program/program_pdeathsig_mixin.h>
#include <yt/ytlib/program/program_setsid_mixin.h>
#include <yt/ytlib/program/program_cgroup_mixin.h>
#include <yt/ytlib/program/helpers.h>

#include <yt/core/json/json_parser.h>

#include <yt/library/phdr_cache/phdr_cache.h>

#include <library/cpp/ytalloc/api/ytalloc.h>

#include <yt/core/ytalloc/bindings.h>

#include <yt/core/misc/ref_counted_tracker_profiler.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class THttpProxyProgram
    : public TProgram
    , public TProgramPdeathsigMixin
    , public TProgramSetsidMixin
    , public TProgramCgroupMixin
    , public TProgramConfigMixin<NHttpProxy::TProxyConfig>
{
public:
    THttpProxyProgram()
        : TProgramPdeathsigMixin(Opts_)
        , TProgramSetsidMixin(Opts_)
        , TProgramCgroupMixin(Opts_)
        , TProgramConfigMixin(Opts_, false)
    {
        Opts_
            .AddLongOption(
                "remote-cluster-proxy",
                "if set, proxy would download cluster connection from //sys/@cluster_connection "
                "on cluster CLUSTER using http interface and then run as an unexposed local proxy "
                "for CLUSTER; if port is not specified, .yt.yandex.net:80 will be assumed automatically; "
                "proxy will be run with default http proxy config on port 8080, but config patch may be "
                "provided via --config option")
            .StoreResult(&RemoteClusterProxy_)
            .RequiredArgument("CLUSTER")
            .Optional();
    }

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        TThread::SetCurrentThreadName("ProxyMain");

        ConfigureUids();
        ConfigureSignals();
        ConfigureCrashHandler();
        ConfigureExitZeroOnSigterm();
        EnablePhdrCache();
        EnableRefCountedTrackerProfiling();
        NYTAlloc::EnableYTLogging();
        NYTAlloc::EnableYTProfiling();
        NYTAlloc::InitializeLibunwindInterop();
        NYTAlloc::SetEnableEagerMemoryRelease(false);
        NYTAlloc::EnableStockpile();
        NYTAlloc::MlockFileMappings();
        NYTAlloc::ConfigureFromEnv();

        if (HandleSetsidOptions()) {
            return;
        }
        if (HandleCgroupOptions()) {
            return;
        }
        if (HandlePdeathsigOptions()) {
            return;
        }

        if (HandleConfigOptions()) {
            return;
        }

        NHttpProxy::TProxyConfigPtr config;
        NYTree::INodePtr configNode;
        if (RemoteClusterProxy_) {
            // Form a default http proxy config listening port 8080.
            auto defaultConfig = New<NHttpProxy::TProxyConfig>();
            defaultConfig->SetDefaults();
            defaultConfig->Port = 8080;
            defaultConfig->Logging = NLogging::TLogManagerConfig::CreateYtServer("http_proxy" /* componentName */);
            // One may disable authentication at all via config, but by default it is better
            // to require authentication. Even YT developers may unintentionally do something
            // harmful, in which case we do not want to see requests under root in cluster logs.
            defaultConfig->Auth->BlackboxTokenAuthenticator = New<NAuth::TCachingBlackboxTokenAuthenticatorConfig>();
            defaultConfig->Auth->BlackboxTokenAuthenticator->Scope = "yt:api";
            // Dump it into node and apply patch from config file (if present).
            configNode = NYTree::ConvertToNode(defaultConfig);
            if (auto configNodePatch = GetConfigNode(true /* returnNullIfNotSupplied */)) {
                configNode = NYTree::PatchNode(configNode, configNodePatch);
            }
            // Finally load it back.
            config = New<NHttpProxy::TProxyConfig>();
            config->SetUnrecognizedStrategy(NYTree::EUnrecognizedStrategy::KeepRecursive);
            config->Load(configNode);
        } else {
            config = GetConfig();
            configNode = GetConfigNode();
        }

        ConfigureSingletons(config);
        StartDiagnosticDump(config);

        if (RemoteClusterProxy_) {
            config->Driver = DownloadClusterConnection(RemoteClusterProxy_, NHttpProxy::HttpProxyLogger);
        }

        auto bootstrap = New<NHttpProxy::TBootstrap>(std::move(config), std::move(configNode));
        bootstrap->Run();
    }

private:
    TString RemoteClusterProxy_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
