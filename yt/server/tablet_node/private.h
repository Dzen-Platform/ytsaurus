#pragma once

#include "public.h"

#include <core/logging/log.h>

#include <core/profiling/profiler.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger TabletNodeLogger;
extern NProfiling::TProfiler TabletNodeProfiler;

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
