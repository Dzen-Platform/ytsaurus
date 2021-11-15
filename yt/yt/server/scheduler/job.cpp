#include "private.h"
#include "job.h"
#include "exec_node.h"
#include "helpers.h"
#include "operation.h"

namespace NYT::NScheduler {

using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

TJob::TJob(
    TJobId id,
    EJobType type,
    TOperationId operationId,
    TIncarnationId incarnationId,
    TControllerEpoch controllerEpoch,
    TExecNodePtr node,
    TInstant startTime,
    const TJobResources& resourceLimits,
    bool interruptible,
    EPreemptionMode preemptionMode,
    TString treeId,
    int schedulingIndex,
    std::optional<EJobSchedulingStage> schedulingStage,
    NNodeTrackerClient::TNodeId revivalNodeId,
    TString revivalNodeAddress)
    : Id_(id)
    , Type_(type)
    , OperationId_(operationId)
    , IncarnationId_(incarnationId)
    , ControllerEpoch_(controllerEpoch)
    , Node_(std::move(node))
    , RevivalNodeId_(revivalNodeId)
    , RevivalNodeAddress_(std::move(revivalNodeAddress))
    , StartTime_(startTime)
    , Interruptible_(interruptible)
    , TreeId_(std::move(treeId))
    , ResourceUsage_(resourceLimits)
    , ResourceLimits_(resourceLimits)
    , PreemptionMode_(preemptionMode)
    , SchedulingIndex_(schedulingIndex)
    , SchedulingStage_(schedulingStage)
{ }

TDuration TJob::GetDuration() const
{
    return *FinishTime_ - StartTime_;
}

bool TJob::IsRevived() const
{
    return RevivalNodeId_ != NNodeTrackerClient::InvalidNodeId;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
