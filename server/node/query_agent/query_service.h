#pragma once

#include "public.h"

#include <yt/server/node/cell_node/public.h>

#include <yt/ytlib/query_client/public.h>

#include <yt/core/rpc/public.h>

namespace NYT::NQueryAgent {

////////////////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateQueryService(
    TQueryAgentConfigPtr config,
    NCellNode::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryAgent
