#pragma once

#include <yt/ytlib/scheduler/public.h>

#include <yt/core/misc/enum.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

struct IUserJobIO;

DECLARE_REFCOUNTED_CLASS(TJobProxyConfig)

DECLARE_REFCOUNTED_STRUCT(IJob)
DECLARE_REFCOUNTED_STRUCT(IJobHost)

DECLARE_REFCOUNTED_CLASS(TJobProxy)

DECLARE_REFCOUNTED_STRUCT(TJobSignalerArg)
DECLARE_REFCOUNTED_STRUCT(TStrace)
DECLARE_REFCOUNTED_STRUCT(TStracerResult)

DEFINE_ENUM(EJobProxyExitCode,
    ((HeartbeatFailed)        (20))
    ((ResultReportFailed)     (21))
    ((ResourcesUpdateFailed)  (22))
    ((SetRLimitFailed)        (23))
    ((ExecFailed)             (24))
    ((UncaughtException)      (25))
    ((GetJobSpecFailed)       (26))
    ((JobProxyPrepareFailed)  (27))
    ((InvalidSpecVersion)     (28))
);

DEFINE_ENUM(EErrorCode,
    ((MemoryLimitExceeded)     (1200))
    ((MemoryCheckFailed)       (1201))
    ((JobTimeLimitExceeded)    (1202))
    ((UnsupportedJobType)      (1203))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
