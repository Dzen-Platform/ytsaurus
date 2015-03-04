#pragma once

#include "public.h"

#include <core/logging/log.h>
#include <core/profiling/profiler.h>

namespace NYT {
namespace NHive {

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger HiveLogger;
extern NProfiling::TProfiler HiveProfiler;

////////////////////////////////////////////////////////////////////////////////

} // namespace NHive
} // namespace NYT
