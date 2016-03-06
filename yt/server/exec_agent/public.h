#pragma once

#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/core/misc/public.h>

namespace NYT {
namespace NExecAgent {

////////////////////////////////////////////////////////////////////////////////

using NJobTrackerClient::TJobId;
using NJobTrackerClient::TOperationId;
using NJobTrackerClient::EJobType;
using NJobTrackerClient::EJobState;
using NJobTrackerClient::EJobPhase;

DEFINE_ENUM(EErrorCode,
    ((ConfigCreationFailed)  (1100))
    ((AbortByScheduler)      (1101))
    ((ResourceOverdraft)     (1102))
);

DEFINE_ENUM(ESandboxKind,
    (User)
    (Udf)
);

extern const TEnumIndexedVector<Stroka, ESandboxKind> SandboxDirectoryNames;

extern const Stroka ProxyConfigFileName;

DECLARE_REFCOUNTED_CLASS(TSlotManager)
DECLARE_REFCOUNTED_CLASS(TSlot)
DECLARE_REFCOUNTED_CLASS(TEnvironmentManager)
DECLARE_REFCOUNTED_CLASS(TSchedulerConnector)

DECLARE_REFCOUNTED_STRUCT(IProxyController)
DECLARE_REFCOUNTED_STRUCT(IEnvironmentBuilder)

DECLARE_REFCOUNTED_CLASS(TEnvironmentConfig)
DECLARE_REFCOUNTED_CLASS(TEnvironmentManagerConfig)
DECLARE_REFCOUNTED_CLASS(TSlotManagerConfig)
DECLARE_REFCOUNTED_CLASS(TSchedulerConnectorConfig)
DECLARE_REFCOUNTED_CLASS(TExecAgentConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
