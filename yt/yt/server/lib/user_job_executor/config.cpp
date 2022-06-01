#include "config.h"

#include <yt/yt/server/lib/user_job_synchronizer_client/user_job_synchronizer.h>

namespace NYT::NUserJobExecutor {

////////////////////////////////////////////////////////////////////////////////

void TUserJobExecutorConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("command", &TThis::Command);

    registrar.Parameter("pipes", &TThis::Pipes)
        .Default();

    registrar.Parameter("job_id", &TThis::JobId);

    registrar.Parameter("environment", &TThis::Environment)
        .Default();

    registrar.Parameter("uid", &TThis::Uid)
        .Default(-1);

    registrar.Parameter("enable_core_dump", &TThis::EnableCoreDump)
        .Default(false);

    registrar.Parameter("user_job_synchronizer_connection_config", &TThis::UserJobSynchronizerConnectionConfig);

    registrar.Postprocessor([] (TThis* config) {
        for (const auto& variable : config->Environment) {
            if (variable.find('=') == TString::npos) {
                THROW_ERROR_EXCEPTION("Bad environment variable: missing '=' in %Qv", variable);
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

DEFINE_REFCOUNTED_TYPE(TUserJobExecutorConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NUserJobExector
