#include "stdafx.h"
#include "job.h"
#include "operation.h"
#include "exec_node.h"
#include "operation_controller.h"

namespace NYT {
namespace NScheduler {

using namespace NNodeTrackerClient::NProto;
using namespace NYTree;

////////////////////////////////////////////////////////////////////

TJob::TJob(
    const TJobId& id,
    EJobType type,
    TOperationPtr operation,
    TExecNodePtr node,
    TInstant startTime,
    const TNodeResources& resourceLimits,
    bool restarted,
    TJobSpecBuilder specBuilder)
    : Id_(id)
    , Type_(type)
    , Operation_(operation.Get())
    , Node_(node)
    , StartTime_(startTime)
    , Restarted_(restarted)
    , State_(EJobState::Waiting)
    , ResourceUsage_(resourceLimits)
    , ResourceLimits_(resourceLimits)
    , SpecBuilder_(std::move(specBuilder))
    , Preemptable_(false)
{ }

TDuration TJob::GetDuration() const
{
    return *FinishTime_ - StartTime_;
}

void TJob::SetResult(const NJobTrackerClient::NProto::TJobResult& result)
{
    Result_ = result;
    Statistics_ = NYTree::ConvertTo<TStatistics>(NYTree::TYsonString(Result_.statistics()));
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

