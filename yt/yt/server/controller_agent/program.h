#include "bootstrap.h"
#include "config.h"
#include "private.h"

#include <yt/server/lib/misc/cluster_connection.h>

#include <yt/server/lib/scheduler/config.h>

#include <yt/ytlib/program/program.h>
#include <yt/ytlib/program/program_config_mixin.h>
#include <yt/ytlib/program/program_pdeathsig_mixin.h>
#include <yt/ytlib/program/program_setsid_mixin.h>
#include <yt/ytlib/program/program_cgroup_mixin.h>
#include <yt/ytlib/program/helpers.h>

#include <yt/library/phdr_cache/phdr_cache.h>

#include <library/cpp/ytalloc/api/ytalloc.h>

#include <yt/core/ytalloc/bindings.h>

#include <yt/core/misc/ref_counted_tracker_profiler.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

class TControllerAgentProgram
    : public TProgram
    , public TProgramPdeathsigMixin
    , public TProgramSetsidMixin
    , public TProgramCgroupMixin
    , public TProgramConfigMixin<TControllerAgentBootstrapConfig>
{
public:
    TControllerAgentProgram()
        : TProgramPdeathsigMixin(Opts_)
        , TProgramSetsidMixin(Opts_)
        , TProgramCgroupMixin(Opts_)
        , TProgramConfigMixin(Opts_)
    {
        Opts_
            .AddLongOption(
                "remote-cluster-proxy",
                "if set, controller agent would download cluster connection from //sys/@cluster_connection "
                "on cluster CLUSTER using http interface and then run as a local controller agent for CLUSTER."
                "WARNING: Do not use this option unless you are sure that remote cluster has schedulers that "
                "are aware of controller agent tags!")
            .StoreResult(&RemoteClusterProxy_)
            .RequiredArgument("CLUSTER")
            .Optional();
        Opts_
            .AddLongOption(
                "tag",
                "if set, sets controller agent tag for local run mode and does nothing in normal mode.")
            .StoreResult(&Tag_)
            .RequiredArgument("TAG")
            .Optional();
    }

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        TThread::SetCurrentThreadName("Main");

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

        NControllerAgent::TControllerAgentBootstrapConfigPtr config;
        NYTree::INodePtr configNode;
        if (RemoteClusterProxy_) {
            // Form a default controller agent config.
            auto defaultConfig = New<NControllerAgent::TControllerAgentBootstrapConfig>();
            defaultConfig->Logging = NLogging::TLogManagerConfig::CreateYtServer("controller_agent" /* componentName */);
            // Set controller agent tag.
            if (!Tag_) {
                THROW_ERROR_EXCEPTION("Controller agent tag should be presented in local mode");
            }
            defaultConfig->ControllerAgent->Tags = std::vector<TString>({Tag_});
            defaultConfig->RpcPort = 9014;
            // Building snapshots at local controller agent seems pretty dangerous and useless, so let's disable it by default.
            defaultConfig->ControllerAgent->EnableSnapshotBuilding = false;
            // Dump it into node and apply patch from config file (if present).
            configNode = NYTree::ConvertToNode(defaultConfig);
            if (auto configNodePatch = GetConfigNode(true /* returnNullIfNotSupplied */)) {
                configNode = NYTree::PatchNode(configNode, configNodePatch);
            }
            // Finally load it back.
            config = New<NControllerAgent::TControllerAgentBootstrapConfig>();
            config->SetUnrecognizedStrategy(NYTree::EUnrecognizedStrategy::KeepRecursive);
            config->Load(configNode);
        } else {
            config = GetConfig();
            configNode = GetConfigNode();
        }

        ConfigureSingletons(config);
        StartDiagnosticDump(config);

        if (RemoteClusterProxy_) {
            // Set controller agent cluster connection.
            auto clusterConnectionNode = DownloadClusterConnection(RemoteClusterProxy_, NControllerAgent::ControllerAgentLogger);
            auto clusterConnectionConfig = New<NApi::NNative::TConnectionConfig>();
            clusterConnectionConfig->Load(clusterConnectionNode);
            config->ClusterConnection = clusterConnectionConfig;
        }

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new TBootstrap(std::move(config), std::move(configNode));
        bootstrap->Run();
    }

private:
    TString RemoteClusterProxy_;
    TString Tag_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
