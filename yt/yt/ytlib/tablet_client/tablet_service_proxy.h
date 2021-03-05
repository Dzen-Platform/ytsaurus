#pragma once

#include "public.h"

#include <yt/yt/ytlib/tablet_client/proto/tablet_service.pb.h>

#include <yt/yt/core/rpc/client.h>

namespace NYT::NTabletClient {

////////////////////////////////////////////////////////////////////////////////

class TTabletServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TTabletServiceProxy, TabletService,
        .SetProtocolVersion(12));

    DEFINE_RPC_PROXY_METHOD(NProto, Write);
    DEFINE_RPC_PROXY_METHOD(NProto, RegisterTransactionActions);
    DEFINE_RPC_PROXY_METHOD(NProto, Trim);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletClient

