#include "job_satellite_connection.h"
#include "private.h"

#include <yt/core/bus/config.h>

#include <yt/core/misc/fs.h>

#include <util/system/fs.h>

namespace NYT {
namespace NJobProxy {

using NJobTrackerClient::TJobId;
using NYson::EYsonFormat;
using namespace NBus;

////////////////////////////////////////////////////////////////////////////////

TJobSatelliteConnection::TJobSatelliteConnection(
    const TJobId& jobId,
    TTcpBusServerConfigPtr jobProxyRpcServerConfig,
    bool useContainer)
    : JobId_(jobId)
{
    ConnectionConfig_ = New<TJobSatelliteConnectionConfig>();
    auto unixDomainName = Format("%v-job-satellite", JobId_);
    ConnectionConfig_->SatelliteRpcServerConfig->UnixDomainName = unixDomainName;
    ConnectionConfig_->JobProxyRpcClientConfig->UnixDomainName = jobProxyRpcServerConfig->UnixDomainName;
    ConnectionConfig_->UseContainer = useContainer;
}

TString TJobSatelliteConnection::GetConfigPath() const
{
    return ConfigFile_;
}

TTcpBusClientConfigPtr TJobSatelliteConnection::GetRpcClientConfig() const
{
    return TTcpBusClientConfig::CreateUnixDomain(ConnectionConfig_->SatelliteRpcServerConfig->UnixDomainName.Get());
}

const NJobTrackerClient::TJobId& TJobSatelliteConnection::GetJobId() const
{
    return JobId_;
}

void TJobSatelliteConnection::MakeConfig()
{
    ConfigFile_ = NFS::CombinePaths(~NFs::CurrentWorkingDirectory(), SatelliteConfigFileName);
    try {
        TFile file(ConfigFile_, CreateAlways | WrOnly | Seq | CloseOnExec);
        TFileOutput output(file);
        NYson::TYsonWriter writer(&output, EYsonFormat::Pretty);
        Serialize(ConnectionConfig_, &writer);
        writer.Flush();
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Failed to write satellite config into %v", ConfigFile_) << ex;
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
