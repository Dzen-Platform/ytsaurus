#pragma once

#include "public.h"
#include "operation.h"
#include "operation_controller.h"
#include "job.h"
#include "exec_node.h"
#include "config.h"

#include <core/logging/log.h>

#include <core/profiling/profiler.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

struct TChunkStripe;
typedef TIntrusivePtr<TChunkStripe> TChunkStripePtr;

struct TChunkStripeList;
typedef TIntrusivePtr<TChunkStripeList> TChunkStripeListPtr;

class TChunkListPool;
typedef TIntrusivePtr<TChunkListPool> TChunkListPoolPtr;

struct IChunkPoolInput;
struct IChunkPoolOutput;
struct IChunkPool;
struct IShuffleChunkPool;

class TSnapshotBuilder;
typedef TIntrusivePtr<TSnapshotBuilder> TSnapshotBuilderPtr;

class TSnapshotDownloader;
typedef TIntrusivePtr<TSnapshotDownloader> TSnapshotDownloaderPtr;

class TOperationControllerBase;

extern const double ApproximateSizesBoostFactor;

extern const NLogging::TLogger SchedulerLogger;
extern const NLogging::TLogger OperationLogger;
extern NProfiling::TProfiler SchedulerProfiler;

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

