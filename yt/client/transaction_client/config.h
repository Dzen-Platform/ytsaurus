#pragma once

#include "public.h"

#include <yt/core/rpc/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT::NTransactionClient {

////////////////////////////////////////////////////////////////////////////////

class TRemoteTimestampProviderConfig
    : public NRpc::TBalancingChannelConfig
    , public NRpc::TRetryingChannelConfig
{
public:
    //! Timeout for RPC requests to timestamp provider.
    TDuration RpcTimeout;

    //! Interval between consecutive current timestamp updates.
    TDuration UpdatePeriod;

    TDuration BatchPeriod;

    TRemoteTimestampProviderConfig()
    {
        RegisterParameter("rpc_timeout", RpcTimeout)
            .Default(TDuration::Seconds(3));
        RegisterParameter("update_period", UpdatePeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("batch_period", BatchPeriod)
            .Default(TDuration::MilliSeconds(10));
    }
};

DEFINE_REFCOUNTED_TYPE(TRemoteTimestampProviderConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionClient
