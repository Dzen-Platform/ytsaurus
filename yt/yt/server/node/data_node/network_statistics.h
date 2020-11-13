#pragma once

#include "public.h"

#include <yt/core/misc/ref_counted.h>

#include <yt/core/concurrency/rw_spinlock.h>

#include <yt/client/node_tracker_client/proto/node.pb.h>

#include <yt/yt/library/syncmap/map.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

struct TNetworkCounters final
{
    std::atomic<NProfiling::TCpuInstant> UpdateTime{};
    NProfiling::TCounter ThrottledReadsCounter;
};

class TNetworkStatistics
{
public:
    explicit TNetworkStatistics(TDataNodeConfigPtr config);

    void IncrementReadThrottlingCounter(const TString& name);
    void UpdateStatistics(NNodeTrackerClient::NProto::TNodeStatistics* statistics);

private:
    const TDataNodeConfigPtr Config_;

    NConcurrency::TSyncMap<TString, TIntrusivePtr<TNetworkCounters>> Counters_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
