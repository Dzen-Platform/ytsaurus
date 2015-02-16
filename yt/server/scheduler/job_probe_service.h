#pragma once

#include "public.h"

#include <core/rpc/public.h>

#include <server/cell_scheduler/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateJobProbeService(NCellScheduler::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT