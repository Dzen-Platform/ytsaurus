#pragma once

#include "public.h"

#include <yt/server/misc/config.h>

#include <yt/server/scheduler/config.h>

#include <yt/ytlib/api/config.h>

#include <yt/core/rpc/config.h>

namespace NYT {
namespace NCellScheduler {

////////////////////////////////////////////////////////////////////////////////

class TCellSchedulerConfig
    : public TServerConfig
{
public:
    //! Node-to-master connection.
    NApi::TConnectionConfigPtr ClusterConnection;

    NScheduler::TSchedulerConfigPtr Scheduler;

    NRpc::TResponseKeeperConfigPtr ResponseKeeper;

    //! Known scheduler addresses.
    NNodeTrackerClient::TAddressList Addresses;

    TCellSchedulerConfig()
    {
        RegisterParameter("cluster_connection", ClusterConnection);
        RegisterParameter("scheduler", Scheduler)
            .DefaultNew();
        RegisterParameter("response_keeper", ResponseKeeper)
            .DefaultNew();
        RegisterParameter("addresses", Addresses)
            .Default();

        RegisterInitializer([&] () {
            ResponseKeeper->EnableWarmup = false;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TCellSchedulerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellScheduler
} // namespace NYT
