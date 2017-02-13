#pragma once

#include "public.h"

#include <yt/core/rpc/config.h>

namespace NYT {
namespace NHiveClient {

////////////////////////////////////////////////////////////////////////////////

class TCellDirectoryConfig
    : public NRpc::TBalancingChannelConfigBase
{ };

DEFINE_REFCOUNTED_TYPE(TCellDirectoryConfig)

////////////////////////////////////////////////////////////////////////////////

class TClusterDirectorySynchronizerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent SyncCells requests to the primary Hive Manager.
    TDuration SyncPeriod;

    TClusterDirectorySynchronizerConfig()
    {
        RegisterParameter("sync_period", SyncPeriod)
            .Default(TDuration::Seconds(15));
    }
};

DEFINE_REFCOUNTED_TYPE(TClusterDirectorySynchronizerConfig)

////////////////////////////////////////////////////////////////////////////////

class TCellDirectorySynchronizerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent SyncCells requests to the primary Hive Manager.
    TDuration SyncPeriod;

    //! Timeout for SyncCells requests.
    TDuration SyncRpcTimeout;

    TCellDirectorySynchronizerConfig()
    {
        RegisterParameter("sync_period", SyncPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("sync_rpc_timeout", SyncRpcTimeout)
            .Default(TDuration::Seconds(15));
    }
};

DEFINE_REFCOUNTED_TYPE(TCellDirectorySynchronizerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHiveClient
} // namespace NYT
