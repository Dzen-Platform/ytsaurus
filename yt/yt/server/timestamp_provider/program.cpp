#include "program.h"

#include <yt/yt/ytlib/program/helpers.h>

#include <yt/yt/library/phdr_cache/phdr_cache.h>
#include <yt/yt/library/mlock/mlock.h>

#include <yt/yt/core/bus/tcp/dispatcher.h>

#include <yt/yt/core/logging/config.h>
#include <yt/yt/core/logging/log_manager.h>

#include <yt/yt/core/misc/ref_counted_tracker_profiler.h>

#include <yt/yt/core/ytalloc/bindings.h>

#include <library/cpp/ytalloc/api/ytalloc.h>

namespace NYT::NTimestampProvider {

////////////////////////////////////////////////////////////////////////////////

TTimestampProviderProgram::TTimestampProviderProgram()
    : TProgramPdeathsigMixin(Opts_)
    , TProgramSetsidMixin(Opts_)
    , TProgramConfigMixin(Opts_)
{ }

void TTimestampProviderProgram::DoRun(const NLastGetopt::TOptsParseResult& /*parseResult*/)
{
    TThread::SetCurrentThreadName("TsProviderMain");

    ConfigureUids();
    ConfigureIgnoreSigpipe();
    ConfigureCrashHandler();
    ConfigureExitZeroOnSigterm();
    EnablePhdrCache();
    EnableRefCountedTrackerProfiling();
    ConfigureAllocator({});

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

    ConfigureSingletons(config);
    StartDiagnosticDump(config);

    // TODO(babenko): This memory leak is intentional.
    // We should avoid destroying bootstrap since some of the subsystems
    // may be holding a reference to it and continue running some actions in background threads.
    auto* bootstrap = CreateBootstrap(std::move(config)).release();
    bootstrap->Initialize();
    bootstrap->Run();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTimestampProvider
