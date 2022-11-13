#include "config.h"

#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/client/security_client/public.h>

#include <yt/yt/library/auth/auth.h>

namespace NYT::NYqlAgent {

using namespace NSecurityClient;
using namespace NAuth;

////////////////////////////////////////////////////////////////////////////////

void TYqlEmbeddedConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("mr_job_binary", &TThis::MRJobBinary)
        .Default("./mrjob");
    registrar.Parameter("yt_token", &TThis::YTToken)
        .Default();

    registrar.Postprocessor([] (TThis* config) {
        if (config->YTToken.empty()) {
            if (auto envToken = LoadToken()) {
                config->YTToken = *envToken;
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TYqlAgentConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("bus_client", &TThis::BusClient)
        .DefaultNew();
    registrar.Parameter("additional_clusters", &TThis::AdditionalClusters)
        .Default();
    registrar.Parameter("yql_thread_count", &TThis::YqlThreadCount)
        .Default(256);
}

////////////////////////////////////////////////////////////////////////////////

void TYqlAgentDynamicConfig::Register(TRegistrar /*registrar*/)
{ }

////////////////////////////////////////////////////////////////////////////////

void TYqlAgentServerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cluster_connection", &TThis::ClusterConnection);

    registrar.Parameter("yql_agent", &TThis::YqlAgent)
        .DefaultNew();
    registrar.Parameter("abort_on_unrecognized_options", &TThis::AbortOnUnrecognizedOptions)
        .Default(false);
    registrar.Parameter("user", &TThis::User)
        .Default(YqlAgentUserName);
    registrar.Parameter("cypress_annotations", &TThis::CypressAnnotations)
        .Default(NYTree::BuildYsonNodeFluently()
            .BeginMap()
            .EndMap()
        ->AsMap());
    registrar.Parameter("root", &TThis::Root)
        .Default("//sys/yql_agents");
    registrar.Parameter("election_manager", &TThis::ElectionManager)
        .DefaultNew();
    registrar.Parameter("dynamic_config_manager", &TThis::DynamicConfigManager)
        .DefaultNew();
    registrar.Parameter("dynamic_config_path", &TThis::DynamicConfigPath)
        .Default();

    registrar.Postprocessor([] (TThis* config) {
        if (auto& lockPath = config->ElectionManager->LockPath; lockPath.empty()) {
            lockPath = config->Root + "/leader_lock";
        }
        if (auto& dynamicConfigPath = config->DynamicConfigPath; dynamicConfigPath.empty()) {
            dynamicConfigPath = config->Root + "/config";
        }
    });
};

////////////////////////////////////////////////////////////////////////////////

void TYqlAgentServerDynamicConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("yql_agent", &TThis::YqlAgent)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYqlAgent
