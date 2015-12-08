#pragma once

#include "public.h"

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

static const ui32 UncommittedRevision = 0;
static const ui32 InvalidRevision = std::numeric_limits<ui32>::max();
static const ui32 MaxRevision = std::numeric_limits<ui32>::max() - 1;

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger TabletNodeLogger;
extern const NProfiling::TProfiler TabletNodeProfiler;

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
