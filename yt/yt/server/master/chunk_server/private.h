#pragma once

#include "public.h"

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TReqUpdateChunkPresence;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger ChunkServerLogger;
extern const NProfiling::TProfiler ChunkServerProfiler;
extern const NProfiling::TRegistry ChunkServerProfilerRegistry;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IChunkVisitor)
DECLARE_REFCOUNTED_STRUCT(IChunkTraverserContext)
DECLARE_REFCOUNTED_STRUCT(IChunkTreeBalancerCallbacks)
DECLARE_REFCOUNTED_CLASS(TExpirationTracker)
DECLARE_REFCOUNTED_CLASS(TJobTracker)

class TChunkScanner;

DEFINE_ENUM(EAddReplicaReason,
    (IncrementalHeartbeat)
    (FullHeartbeat)
    (Confirmation)
);

DEFINE_ENUM(ERemoveReplicaReason,
    (None)
    (IncrementalHeartbeat)
    (ApproveTimeout)
    (ChunkDestroyed)
    (NodeDisposed)
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
