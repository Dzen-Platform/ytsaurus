#pragma once

#include "cluster_nodes.h"

#include <yt/server/clickhouse_server/directory.h>

#include <Interpreters/Context.h>

#include <string>
#include <unordered_set>
#include <functional>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

using TClusterNodeTicket = IEphemeralNodeKeeperPtr;

////////////////////////////////////////////////////////////////////////////////

/// Cluster node discovery service

class IClusterNodeTracker
{
public:
    virtual ~IClusterNodeTracker() = default;

    virtual void StartTrack(const DB::Context& context) = 0;
    virtual void StopTrack() = 0;

    virtual TClusterNodeTicket EnterCluster(
        const std::string& instanceId,
        const std::string& host,
        ui16 tcpPort,
        ui16 httpPort) = 0;

    virtual TClusterNodeNames ListAvailableNodes() = 0;

    virtual TClusterNodes GetAvailableNodes() = 0;
};

using IClusterNodeTrackerPtr = std::shared_ptr<IClusterNodeTracker>;

using IExecutionClusterPtr = IClusterNodeTrackerPtr;

////////////////////////////////////////////////////////////////////////////////

IClusterNodeTrackerPtr CreateClusterNodeTracker(
    ICoordinationServicePtr coordinationService,
    IAuthorizationTokenPtr authToken,
    const std::string directoryPath,
    uint64_t clickhousePort);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
