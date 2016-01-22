#pragma once

#include "public.h"

#include <core/rpc/config.h>

#include <ytlib/api/config.h>

#include <server/misc/config.h>

#include <server/scheduler/config.h>

namespace NYT {
namespace NCellScheduler {

////////////////////////////////////////////////////////////////////////////////

class TCellSchedulerConfig
    : public TServerConfig
{
public:
    //! Orchid cache expiration timeout.
    TDuration OrchidCacheExpirationTime;

    //! Node-to-master connection.
    NApi::TConnectionConfigPtr ClusterConnection;

    NScheduler::TSchedulerConfigPtr Scheduler;

    NRpc::TResponseKeeperConfigPtr ResponseKeeper;

    TCellSchedulerConfig()
    {
        RegisterParameter("orchid_cache_expiration_time", OrchidCacheExpirationTime)
            .Default(TDuration::Seconds(1));
        RegisterParameter("cluster_connection", ClusterConnection);
        RegisterParameter("scheduler", Scheduler)
            .DefaultNew();
        RegisterParameter("response_keeper", ResponseKeeper)
            .DefaultNew();

        RegisterInitializer([&] () {
            ResponseKeeper->EnableWarmup = false;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TCellSchedulerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellScheduler
} // namespace NYT
