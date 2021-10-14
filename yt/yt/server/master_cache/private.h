#pragma once

#include <yt/yt/library/profiling/sensor.h>

#include <yt/yt/core/logging/log.h>

namespace NYT::NMasterCache {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TChaosCache)

DECLARE_REFCOUNTED_CLASS(TMasterCacheConfig)

struct IBootstrap;

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger MasterCacheLogger;

extern const NProfiling::TProfiler MasterCacheProfiler;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NMasterCache
