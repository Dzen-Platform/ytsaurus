#pragma once

#include "public.h"

#include <yt/ytlib/hive/hive_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NHiveClient {

////////////////////////////////////////////////////////////////////////////////

class THiveServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(THiveServiceProxy, RPC_PROXY_DESC(HiveService)
        .SetProtocolVersion(1));

    DEFINE_RPC_PROXY_METHOD(NProto, Ping,
        .SetMultiplexingBand(NRpc::EMultiplexingBand::Control));
    DEFINE_RPC_PROXY_METHOD(NProto, SyncCells);
    DEFINE_RPC_PROXY_METHOD(NProto, PostMessages);
    DEFINE_RPC_PROXY_METHOD(NProto, SendMessages);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NHiveClient
} // namespace NYT
