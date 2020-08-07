#pragma once

#include "public.h"

#include <yt/core/ytree/attributes.h>

#include <yt/core/ytree/yson_serializable.h>

#include <yt/ytlib/discovery_client/public.h>

namespace NYT::NDistributedThrottler {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EDistributedThrottlerMode,
    (Uniform)
    (Adaptive)
    (Precise)
);

class TDistributedThrottlerConfig
    : public NYTree::TYsonSerializable
{
public:
    NDiscoveryClient::TMemberClientConfigPtr MemberClient;
    NDiscoveryClient::TDiscoveryClientConfigPtr DiscoveryClient;

    TDuration ControlRpcTimeout;
    TDuration ThrottleRpcTimeout;

    TDuration LimitUpdatePeriod;
    TDuration LeaderUpdatePeriod;

    TDuration ThrottlerExpirationTime;

    EDistributedThrottlerMode Mode;
    double ExtraLimitRatio;
    double EmaAlpha;

    TDistributedThrottlerConfig();
};

DEFINE_REFCOUNTED_TYPE(TDistributedThrottlerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDistributedThrottler
