#pragma once

#include "public.h"

#include <yt/core/profiling/profiler.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

//! Creates a simple client-side block cache.
IBlockCachePtr CreateClientBlockCache(
    TBlockCacheConfigPtr config,
    EBlockType supportedBlockTypes,
    const NProfiling::TRegistry& profiler = {});

//! Returns an always-empty block cache.
IBlockCachePtr GetNullBlockCache();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
