#pragma once

#include <yt/core/misc/small_vector.h>
#include <yt/core/misc/enum.h>

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

class TProfiler;
class TProfileManager;

class TResourceTracker;

class TTimer;
class TShardedMonotonicCounter;
class TAtomicGauge;
class TShardedAggregateGauge;
class TAtomicShardedAggregateGauge;

struct TQueuedSample;

//! Generic value for samples.
using TValue = i64;

using TCpuInstant = i64;
using TCpuDuration = i64;

using TTagId = int;

constexpr int TypicalTagCount = 8;
using TTagIdList = SmallVector<TTagId, TypicalTagCount>;

//! Enumeration of metric types.
/*
 *  - Counter: A counter is a cumulative metric that represents a single numerical
 *  value that only ever goes up. A counter is typically used to count requests served,
 *  tasks completed, errors occurred, etc.. Counters should not be used to expose current
 *  counts of items whose number can also go down, e.g. the number of currently running
 *  goroutines. Use gauges for this use case.
 *
 *  - Gauge: A gauge is a metric that represents a single numerical value that can
 *  arbitrarily go up and down. Gauges are typically used for measured values like
 *  temperatures or current memory usage, but also "counts" that can go up and down,
 *  like the number of running goroutines.
 */
DEFINE_ENUM(EMetricType,
    ((Counter) (0))
    ((Gauge)   (1))
);

DECLARE_REFCOUNTED_CLASS(TProfileManagerConfig)
DECLARE_REFCOUNTED_CLASS(TResourceTracker)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
