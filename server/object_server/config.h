#pragma once

#include "public.h"

#include <yt/core/misc/config.h>

#include <yt/core/rpc/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

class TObjectManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Maximum number to objects to destroy per a single GC mutation.
    int MaxObjectsPerGCSweep;

    //! Period between subsequent GC queue checks.
    TDuration GCSweepPeriod;

    //! Amount of time to wait before yielding meta state thread to another request.
    TDuration YieldTimeout;

    //! Timeout for forwarded requests (used when client timeout is omitted).
    TDuration ForwardingRpcTimeout;

    TObjectManagerConfig()
    {
        RegisterParameter("max_objects_per_gc_sweep", MaxObjectsPerGCSweep)
            .Default(1000);
        RegisterParameter("gc_sweep_period", GCSweepPeriod)
            .Default(TDuration::MilliSeconds(1000));
        RegisterParameter("yield_timeout", YieldTimeout)
            .Default(TDuration::MilliSeconds(10));
        RegisterParameter("forwarding_rpc_timeout", ForwardingRpcTimeout)
            .Default(TDuration::Seconds(30));
    }
};

DEFINE_REFCOUNTED_TYPE(TObjectManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TMasterCacheServiceConfig
    : public NRpc::TThrottlingChannelConfig
    , public TSlruCacheConfig
{
public:
    TMasterCacheServiceConfig()
    {
        RegisterInitializer([&] () {
            Capacity = (i64) 16 * 1024 * 1024;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TMasterCacheServiceConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
