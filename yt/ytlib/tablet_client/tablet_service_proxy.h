#pragma once

#include "public.h"

#include <yt/ytlib/tablet_client/tablet_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NTabletClient {

////////////////////////////////////////////////////////////////////////////////

class TTabletServiceProxy
    : public NRpc::TProxyBase
{
public:
    static Stroka GetServiceName()
    {
        return "TabletService";
    }

    static int GetProtocolVersion()
    {
        return 9;
    }

    explicit TTabletServiceProxy(NRpc::IChannelPtr channel)
        : TProxyBase(channel, GetServiceName(), GetProtocolVersion())
    { }

    DEFINE_RPC_PROXY_METHOD(NProto, Write);
    DEFINE_RPC_PROXY_METHOD(NProto, RegisterTransactionActions);
    DEFINE_RPC_PROXY_METHOD(NProto, Trim);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletClient
} // namespace NYT

