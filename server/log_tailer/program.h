#include "bootstrap.h"
#include "config.h"

#include <yt/ytlib/program/configure_singletons.h>
#include <yt/ytlib/program/program.h>
#include <yt/ytlib/program/program_config_mixin.h>

namespace NYT::NLogTailer {

////////////////////////////////////////////////////////////////////////////////

class TLogTailerProgram
    : public TProgram
    , public TProgramConfigMixin<TLogTailerBootstrapConfig>
{
public:
    TLogTailerProgram()
        : TProgramConfigMixin(Opts_, false)
    {
        Opts_.SetFreeArgsMin(0);
        Opts_.SetFreeArgsMax(1);
        Opts_.SetFreeArgTitle(0, "writer-pid");
    }

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        TThread::SetCurrentThreadName("LogTailerMain");

        ConfigureCrashHandler();

        auto config = GetConfig();
        if (parseResult.GetFreeArgCount() == 1) {
            auto freeArgs = parseResult.GetFreeArgs();
            config->LogTailer->LogRotation->LogWriterPid = FromString<int>(freeArgs[0]);
        }

        ConfigureSingletons(config);

        TBootstrap bootstrap{std::move(config)};
        bootstrap.Run();
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NLogTailer
