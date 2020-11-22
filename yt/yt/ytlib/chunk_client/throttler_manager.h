
#pragma once

#include "private.h"

#include <yt/ytlib/object_client/public.h>

#include <yt/core/concurrency/config.h>

#include <yt/core/logging/log.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TThrottlerManager
    : public TRefCounted
{
public:
    explicit TThrottlerManager(
        NConcurrency::TThroughputThrottlerConfigPtr config,
        NLogging::TLogger logger = {},
        NProfiling::TRegistry profiler = {});

    NConcurrency::IThroughputThrottlerPtr GetThrottler(NObjectClient::TCellTag cellTag);

    void Reconfigure(NConcurrency::TThroughputThrottlerConfigPtr config);

private:
    NConcurrency::TThroughputThrottlerConfigPtr Config_;
    const NLogging::TLogger Logger_;
    const NProfiling::TRegistry Profiler_;

    //! Protects the section immediately following it.
    YT_DECLARE_SPINLOCK(TAdaptiveLock, SpinLock_);
    THashMap<NObjectClient::TCellTag, NConcurrency::IReconfigurableThroughputThrottlerPtr> ThrottlerMap_;
};

DEFINE_REFCOUNTED_TYPE(TThrottlerManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
