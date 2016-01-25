#pragma once

#include "public.h"

#include <yt/ytlib/node_tracker_client/node_tracker_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NNodeTrackerClient {

////////////////////////////////////////////////////////////////////////////////

class TNodeTrackerServiceProxy
    : public NRpc::TProxyBase
{
public:
    static Stroka GetServiceName()
    {
        return "NodeTracker";
    }

    static int GetProtocolVersion()
    {
        return 5;
    }

    explicit TNodeTrackerServiceProxy(NRpc::IChannelPtr channel)
        : TProxyBase(channel, GetServiceName(), GetProtocolVersion())
    { }

    DEFINE_RPC_PROXY_METHOD(NProto, RegisterNode);
    DEFINE_RPC_PROXY_METHOD(NProto, FullHeartbeat);
    DEFINE_RPC_PROXY_METHOD(NProto, IncrementalHeartbeat);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerClient
} // namespace NYT
