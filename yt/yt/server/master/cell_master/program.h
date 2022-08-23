#include "bootstrap.h"
#include "config.h"
#include "snapshot_exporter.h"

#include <yt/yt/library/program/program.h>
#include <yt/yt/library/program/program_config_mixin.h>
#include <yt/yt/library/program/program_pdeathsig_mixin.h>
#include <yt/yt/library/program/program_setsid_mixin.h>
#include <yt/yt/ytlib/program/helpers.h>

#include <yt/yt/core/logging/log_manager.h>
#include <yt/yt/core/logging/config.h>
#include <yt/yt/core/logging/file_log_writer.h>

#include <library/cpp/ytalloc/api/ytalloc.h>

#include <library/cpp/yt/phdr_cache/phdr_cache.h>

#include <library/cpp/yt/mlock/mlock.h>

#include <yt/yt/core/bus/tcp/dispatcher.h>


#include <yt/yt/core/misc/shutdown.h>

#include <yt/yt/core/ytalloc/bindings.h>

#include <util/system/thread.h>

namespace NYT::NCellMaster {

////////////////////////////////////////////////////////////////////////////////

class TCellMasterProgram
    : public TProgram
    , public TProgramPdeathsigMixin
    , public TProgramSetsidMixin
    , public TProgramConfigMixin<NCellMaster::TCellMasterConfig>
{
public:
    TCellMasterProgram()
        : TProgramPdeathsigMixin(Opts_)
        , TProgramSetsidMixin(Opts_)
        , TProgramConfigMixin(Opts_)
    {
        Opts_
            .AddLongOption("dump-snapshot", "dump master snapshot and exit")
            .StoreMappedResult(&DumpSnapshot_, &CheckPathExistsArgMapper)
            .RequiredArgument("SNAPSHOT");
        Opts_
            .AddLongOption("validate-snapshot", "validate master snapshot and exit")
            .StoreMappedResult(&ValidateSnapshot_, &CheckPathExistsArgMapper)
            .RequiredArgument("SNAPSHOT");
        Opts_
            .AddLongOption("export-snapshot", "export master snapshot\nexpects path to snapshot")
            .StoreMappedResult(&ExportSnapshot_, &CheckPathExistsArgMapper)
            .RequiredArgument("SNAPSHOT");
        Opts_
            .AddLongOption("export-config", "user config for master snapshot exporting\nexpects yson which may have keys "
                           "'attributes', 'first_key', 'last_key', 'types', 'job_index', 'job_count'")
            .StoreResult(&ExportSnapshotConfig_)
            .RequiredArgument("CONFIG_YSON");
        Opts_
            .AddLongOption("dump-config", "config for snapshot dumping, which contains 'lower_limit' and 'upper_limit'")
            .StoreResult(&DumpConfig_)
            .RequiredArgument("CONFIG_YSON");
        Opts_
            .AddLongOption("report-total-write-count")
            .SetFlag(&EnableTotalWriteCountReport_)
            .NoArgument();
        Opts_
            .AddLongOption("sleep-after-initialize", "sleep for 10s after calling TBootstrap::Initialize()")
            .SetFlag(&SleepAfterInitialize_)
            .NoArgument();
    }

protected:
    void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        TThread::SetCurrentThreadName("MasterMain");

        bool dumpSnapshot = parseResult.Has("dump-snapshot");
        bool validateSnapshot = parseResult.Has("validate-snapshot");
        bool exportSnapshot = parseResult.Has("export-snapshot");

        ConfigureUids();
        ConfigureIgnoreSigpipe();
        ConfigureCrashHandler();
        ConfigureExitZeroOnSigterm();
        EnablePhdrCache();
        ConfigureAllocator({});

        NYTAlloc::EnableYTLogging();
        NYTAlloc::EnableYTProfiling();
        NYTAlloc::InitializeLibunwindInterop();
        NYTAlloc::SetEnableEagerMemoryRelease(false);
        NYTAlloc::EnableStockpile();
        NYT::MlockFileMappings();

        if (HandleSetsidOptions()) {
            return;
        }
        if (HandlePdeathsigOptions()) {
            return;
        }

        if (HandleConfigOptions()) {
            return;
        }

        auto config = GetConfig();

        if (dumpSnapshot || validateSnapshot || exportSnapshot) {
            NBus::TTcpDispatcher::Get()->DisableNetworking();
        }

        if (dumpSnapshot) {
            config->Logging = NLogging::TLogManagerConfig::CreateSilent();
        } else if (validateSnapshot) {
            config->Logging = NLogging::TLogManagerConfig::CreateQuiet();

            auto silentRule = New<NLogging::TRuleConfig>();
            silentRule->MinLevel = NLogging::ELogLevel::Debug;
            silentRule->Writers.push_back(TString("dev_null"));

            auto writerConfig = New<NLogging::TLogWriterConfig>();
            writerConfig->Type = NLogging::TFileLogWriterConfig::Type;

            auto fileWriterConfig = New<NLogging::TFileLogWriterConfig>();
            fileWriterConfig->FileName = "/dev/null";

            config->Logging->Rules.push_back(silentRule);
            config->Logging->Writers.emplace(TString("dev_null"), writerConfig->BuildFullConfig(fileWriterConfig));
        } else if (exportSnapshot) {
            config->Logging = NLogging::TLogManagerConfig::CreateQuiet();
        }

        ConfigureNativeSingletons(config);
        StartDiagnosticDump(config);

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NCellMaster::TBootstrap(std::move(config));
        bootstrap->Initialize();

        if (SleepAfterInitialize_) {
            NConcurrency::TDelayedExecutor::WaitForDuration(TDuration::Seconds(10));
        }

        if (dumpSnapshot) {
            bootstrap->TryLoadSnapshot(DumpSnapshot_, true, false, DumpConfig_);
        } else if (validateSnapshot) {
            bootstrap->TryLoadSnapshot(ValidateSnapshot_, false, EnableTotalWriteCountReport_, TString());
        } else if (exportSnapshot) {
            ExportSnapshot(bootstrap, ExportSnapshot_, ExportSnapshotConfig_);
        } else {
            bootstrap->Run();
        }

        // XXX(babenko): ASAN complains about memory leak on graceful exit.
        // Must try to resolve them later.
        _exit(0);
    }

private:
    TString DumpSnapshot_;
    TString ValidateSnapshot_;
    TString ExportSnapshot_;
    TString ExportSnapshotConfig_;
    TString DumpConfig_;
    bool EnableTotalWriteCountReport_ = false;
    bool SleepAfterInitialize_ = false;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
