#pragma once

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

#include <yt/core/rpc/public.h>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger DataNodeLogger;
extern const NProfiling::TProfiler DataNodeProfiler;

extern const TString CellIdFileName;
extern const TString MultiplexedDirectory;
extern const TString TrashDirectory;
extern const TString CleanExtension;
extern const TString SealedFlagExtension;
extern const TString ArtifactMetaSuffix;

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
