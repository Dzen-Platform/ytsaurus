#pragma once

#include <yt/yt/server/lib/job_agent/public.h>

#include <yt/yt/ytlib/job_tracker_client/public.h>

namespace NYT::NJobAgent {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EJobOrigin,
    ((Master)    (0))
    ((Scheduler) (1))
);

////////////////////////////////////////////////////////////////////////////////

using NJobTrackerClient::TJobId;
using NJobTrackerClient::TOperationId;
using NJobTrackerClient::EJobType;
using NJobTrackerClient::EJobState;
using NJobTrackerClient::EJobPhase;

DECLARE_REFCOUNTED_STRUCT(IJob)

DECLARE_REFCOUNTED_CLASS(TMappedMemoryControllerConfig)

class TResourceHolder;

DECLARE_REFCOUNTED_CLASS(IJobResourceManager)
DECLARE_REFCOUNTED_CLASS(IOrchidServiceProvider)

struct TChunkCacheStatistics
{
    i64 CacheHitArtifactsSize = 0;
    i64 CacheMissArtifactsSize = 0;
    i64 CacheBypassedArtifactsSize = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent
