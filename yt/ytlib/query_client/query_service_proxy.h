#pragma once

#include "public.h"

#include <yt/ytlib/query_client/proto/query_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT::NQueryClient {

////////////////////////////////////////////////////////////////////////////////

class TQueryServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TQueryServiceProxy, QueryService,
        .SetProtocolVersion(33));

    DEFINE_RPC_PROXY_METHOD(NProto, Execute);
    DEFINE_RPC_PROXY_METHOD(NProto, Read);
    DEFINE_RPC_PROXY_METHOD(NProto, Multiread);
    DEFINE_RPC_PROXY_METHOD(NProto, GetTabletInfo);
    DEFINE_RPC_PROXY_METHOD(NProto, ReadDynamicStore,
        .SetStreamingEnabled(true));
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient
