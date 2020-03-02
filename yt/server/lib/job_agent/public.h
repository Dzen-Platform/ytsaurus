#pragma once

#include <yt/core/misc/public.h>

namespace NYT::NJobAgent {

////////////////////////////////////////////////////////////////////////////////

struct TJobStatistics;

DECLARE_REFCOUNTED_CLASS(TGpuManagerConfig)
DECLARE_REFCOUNTED_CLASS(TMappedMemoryControllerConfig)
DECLARE_REFCOUNTED_CLASS(TResourceLimitsConfig)
DECLARE_REFCOUNTED_CLASS(TJobControllerConfig)
DECLARE_REFCOUNTED_CLASS(TStatisticsReporterConfig)
DECLARE_REFCOUNTED_CLASS(TShellCommandConfig)
DECLARE_REFCOUNTED_CLASS(TStatisticsReporter)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent
