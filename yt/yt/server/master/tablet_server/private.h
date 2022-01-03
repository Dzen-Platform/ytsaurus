#pragma once

#include "public.h"

#include <yt/yt/core/logging/log.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NTabletServer {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TTabletTracker)

////////////////////////////////////////////////////////////////////////////////

inline const NLogging::TLogger TabletServerLogger("TabletServer");
inline const NProfiling::TProfiler TabletServerProfiler("/tablet_server");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
