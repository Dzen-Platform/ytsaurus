#pragma once

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/misc/public.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger RpcServerLogger;
extern const NLogging::TLogger RpcClientLogger;

extern const NProfiling::TProfiler RpcServerProfiler;
extern const NProfiling::TProfiler RpcClientProfiler;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
