#include <yt/server/node/cell_node/bootstrap.h>
#include <yt/server/node/cell_node/config.h>

#include <yt/ytlib/program/program.h>
#include <yt/ytlib/program/program_config_mixin.h>
#include <yt/ytlib/program/program_tool_mixin.h>
#include <yt/ytlib/program/program_pdeathsig_mixin.h>
#include <yt/ytlib/program/configure_singletons.h>

#include <yt/core/phdr_cache/phdr_cache.h>

#include <yt/core/alloc/alloc.h>

namespace NYT::NCellNode {

////////////////////////////////////////////////////////////////////////////////

class TCellNodeProgram
    : public TProgram
    , public TProgramPdeathsigMixin
    , public TProgramToolMixin
    , public TProgramConfigMixin<NCellNode::TCellNodeConfig>
{
public:
    TCellNodeProgram()
        : TProgramPdeathsigMixin(Opts_)
        , TProgramToolMixin(Opts_)
        , TProgramConfigMixin(Opts_, false)
    { }

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        TThread::CurrentThreadSetName("NodeMain");

        ConfigureUids();
        ConfigureSignals();
        ConfigureCrashHandler();
        ConfigureExitZeroOnSigterm();
        EnablePhdrCache();
        NYTAlloc::EnableLogging();
        NYTAlloc::EnableProfiling();
        NYTAlloc::EnableStockpile();

        if (HandlePdeathsigOptions()) {
            return;
        }

        if (HandleToolOptions()) {
            return;
        }

        if (HandleConfigOptions()) {
            return;
        }

        auto config = GetConfig();
        auto configNode = GetConfigNode();

        ConfigureSingletons(config);

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new NCellNode::TBootstrap(std::move(config), std::move(configNode));
        bootstrap->Run();
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellNode

int main(int argc, const char** argv)
{
    return NYT::NCellNode::TCellNodeProgram().Run(argc, argv);
}

