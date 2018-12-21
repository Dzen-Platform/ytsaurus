#include <yp/server/master/bootstrap.h>
#include <yp/server/master/config.h>

#include <yt/ytlib/program/program.h>
#include <yt/ytlib/program/program_config_mixin.h>
#include <yt/ytlib/program/program_pdeathsig_mixin.h>
#include <yt/ytlib/program/configure_singletons.h>

#include <yt/core/logging/log_manager.h>
#include <yt/core/logging/config.h>

#include <yt/core/alloc/alloc.h>

#include <yt/core/phdr_cache/phdr_cache.h>

namespace NYP::NServer::NMaster {

using namespace NYT;

////////////////////////////////////////////////////////////////////////////////

class TMasterProgram
    : public TProgram
    , public TProgramPdeathsigMixin
    , public TProgramConfigMixin<NMaster::TMasterConfig>
{
public:
    TMasterProgram()
        : TProgramPdeathsigMixin(Opts_)
        , TProgramConfigMixin(Opts_)
    { }

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& /*parseResult*/) override
    {
        TThread::CurrentThreadSetName("MasterMain");

        ConfigureSignals();
        ConfigureCrashHandler();
        EnablePhdrCache();
        ConfigureExitZeroOnSigterm();
        NYTAlloc::EnableLogging();
        NYTAlloc::EnableProfiling();

        if (HandlePdeathsigOptions()) {
            return;
        }

        if (HandleConfigOptions()) {
            return;
        }

        auto config = GetConfig();

        ConfigureSingletons(config);

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NMaster::TBootstrap(std::move(config));
        bootstrap->Run();
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NMaster

int main(int argc, const char** argv)
{
    return NYP::NServer::NMaster::TMasterProgram().Run(argc, argv);
}

