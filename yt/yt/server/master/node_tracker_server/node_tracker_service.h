#pragma once

#include "public.h"

#include <yt/server/master/cell_master/public.h>

#include <yt/core/rpc/public.h>

namespace NYT::NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateNodeTrackerService(NCellMaster::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNodeTrackerServer
