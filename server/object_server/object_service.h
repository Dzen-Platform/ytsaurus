#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/core/rpc/public.h>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateObjectService(NCellMaster::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
