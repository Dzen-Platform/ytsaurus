#pragma once

#include "public.h"

#include <yt/core/misc/small_vector.h>

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

struct TWriteContext;

static constexpr ui32 UncommittedRevision = 0;
static constexpr ui32 InvalidRevision = std::numeric_limits<ui32>::max();
static constexpr ui32 MaxRevision = std::numeric_limits<ui32>::max() - 1;

static constexpr int TypicalStoreIdCount = 8;
using TStoreIdList = SmallVector<TStoreId, TypicalStoreIdCount>;

static constexpr int InitialEditListCapacity = 2;
static constexpr int EditListCapacityMultiplier = 2;
static constexpr int MaxEditListCapacity = 256;

static constexpr int MaxOrderedDynamicSegments = 32;
static constexpr int InitialOrderedDynamicSegmentIndex = 10;

static constexpr i64 MemoryUsageGranularity = 16_KB;

static constexpr auto TabletStoresUpdateThrottlerRpcTimeout = TDuration::Minutes(10);

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger TabletNodeLogger;
extern const NProfiling::TRegistry TabletNodeProfiler;
extern const NLogging::TLogger LsmLogger;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
