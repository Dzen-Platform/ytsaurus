#pragma once

#include "public.h"

#include <yt/yt/server/lib/hydra2/proto/snapshot_service.pb.h>

#include <yt/yt/core/rpc/client.h>

namespace NYT::NHydra2 {

////////////////////////////////////////////////////////////////////////////////

class TSnapshotServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TSnapshotServiceProxy, SnapshotService);

    DEFINE_RPC_PROXY_METHOD(NProto, ReadSnapshot);
    DEFINE_RPC_PROXY_METHOD(NProto, LookupSnapshot,
        .SetMultiplexingBand(NRpc::EMultiplexingBand::Control));
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra2
