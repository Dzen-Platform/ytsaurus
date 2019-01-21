#pragma once

#include "public.h"

#include <yt/core/rpc/public.h>

namespace NYT::NObjectServer {

////////////////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateMasterCacheService(
    TMasterCacheServiceConfigPtr config,
    NRpc::IChannelPtr masterChannel,
    NRpc::TRealmId masterCellId);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectServer
