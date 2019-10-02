#pragma once

#include "public.h"

#include <yt/server/lib/controller_agent/private.h>

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

extern const TString IntermediatePath;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ELegacyLivePreviewMode,
    (ExplicitlyEnabled)
    (ExplicitlyDisabled)
    (DoNotCare)
    (NotSupported)
);

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IOperationController)
using IOperationControllerWeakPtr = TWeakPtr<IOperationController>;

DECLARE_REFCOUNTED_STRUCT(TSnapshotJob)

DECLARE_REFCOUNTED_CLASS(TSnapshotBuilder)
DECLARE_REFCOUNTED_CLASS(TSnapshotDownloader)

class TOperationControllerBase;

DECLARE_REFCOUNTED_CLASS(TChunkListPool)

DECLARE_REFCOUNTED_STRUCT(TFinishedJobInfo)
DECLARE_REFCOUNTED_STRUCT(TJobInfo)
DECLARE_REFCOUNTED_CLASS(TJoblet)
DECLARE_REFCOUNTED_STRUCT(TCompletedJob)

DECLARE_REFCOUNTED_CLASS(TTask)
DECLARE_REFCOUNTED_STRUCT(TTaskGroup)

DECLARE_REFCOUNTED_CLASS(TAutoMergeTask)

DECLARE_REFCOUNTED_STRUCT(ITaskHost)

DECLARE_REFCOUNTED_STRUCT(TInputTable)
DECLARE_REFCOUNTED_STRUCT(TOutputTable)
DECLARE_REFCOUNTED_STRUCT(TIntermediateTable)

struct IJobSplitter;

struct TLivePreviewTableBase;

class TAutoMergeDirector;
struct TJobNodeDescriptor;

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger ControllerLogger;
extern const NLogging::TLogger ControllerAgentLogger;

extern const NProfiling::TProfiler ControllerAgentProfiler;

////////////////////////////////////////////////////////////////////////////////

using TOperationIdToControllerMap = THashMap<TOperationId, IOperationControllerPtr>;
using TOperationIdToWeakControllerMap = THashMap<TOperationId, IOperationControllerWeakPtr>;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent

