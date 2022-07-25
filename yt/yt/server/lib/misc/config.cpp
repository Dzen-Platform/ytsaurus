#include "config.h"

#include <yt/yt/core/ytree/ephemeral_node_factory.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

void TServerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("bus_server", &TThis::BusServer)
        .DefaultNew();
    registrar.Parameter("rpc_server", &TThis::RpcServer)
        .DefaultNew();
    registrar.Parameter("core_dumper", &TThis::CoreDumper)
        .Default();

    registrar.Parameter("rpc_port", &TThis::RpcPort)
        .Default(0)
        .GreaterThanOrEqual(0)
        .LessThan(65536);
    registrar.Parameter("tvm_only_rpc_port", &TThis::TvmOnlyRpcPort)
        .Default(0)
        .GreaterThanOrEqual(0)
        .LessThan(65536);
    registrar.Parameter("monitoring_port", &TThis::MonitoringPort)
        .Default(0)
        .GreaterThanOrEqual(0)
        .LessThan(65536);

    registrar.Postprocessor([] (TThis* config) {
        if (config->RpcPort > 0) {
            if (config->BusServer->Port || config->BusServer->UnixDomainSocketPath) {
                THROW_ERROR_EXCEPTION("Explicit socket configuration for bus server is forbidden");
            }
            config->BusServer->Port = config->RpcPort;
        }
    });
}

NHttp::TServerConfigPtr TServerConfig::CreateMonitoringHttpServerConfig()
{
    auto config = New<NHttp::TServerConfig>();
    config->Port = MonitoringPort;
    config->BindRetryCount = BusServer->BindRetryCount;
    config->BindRetryBackoff = BusServer->BindRetryBackoff;
    config->ServerName = "HttpMon";
    return config;
}

////////////////////////////////////////////////////////////////////////////////

void TDiskLocationConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("path", &TThis::Path)
        .NonEmpty();
    registrar.Parameter("min_disk_space", &TThis::MinDiskSpace)
        .GreaterThanOrEqual(0)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

TDiskHealthCheckerConfig::TDiskHealthCheckerConfig()
{
    RegisterParameter("check_period", CheckPeriod)
        .Default(TDuration::Minutes(1));
    RegisterParameter("test_size", TestSize)
        .InRange(0, 1_GB)
        .Default(1_MB);
    RegisterParameter("timeout", Timeout)
        .Default(TDuration::Seconds(60));
}

////////////////////////////////////////////////////////////////////////////////

TFormatConfigBase::TFormatConfigBase()
{
    RegisterParameter("enable", Enable)
        .Default(true);
    RegisterParameter("default_attributes", DefaultAttributes)
        .Default(NYTree::GetEphemeralNodeFactory()->CreateMap());
}

////////////////////////////////////////////////////////////////////////////////

TFormatConfig::TFormatConfig()
{
    RegisterParameter("user_overrides", UserOverrides)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

void TArchiveReporterConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("enabled", &TThis::Enabled)
        .Default(true);
    registrar.Parameter("reporting_period", &TThis::ReportingPeriod)
        .Default(TDuration::Seconds(5));
    registrar.Parameter("min_repeat_delay", &TThis::MinRepeatDelay)
        .Default(TDuration::Seconds(10));
    registrar.Parameter("max_repeat_delay", &TThis::MaxRepeatDelay)
        .Default(TDuration::Minutes(5));
    registrar.Parameter("max_items_in_batch", &TThis::MaxItemsInBatch)
        .Default(1000);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

