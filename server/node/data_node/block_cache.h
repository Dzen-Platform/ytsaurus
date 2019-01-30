#pragma once

#include "public.h"

#include <yt/server/node/cell_node/public.h>

#include <yt/ytlib/chunk_client/public.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

NChunkClient::IBlockCachePtr CreateServerBlockCache(
    TDataNodeConfigPtr config,
    NCellNode::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
