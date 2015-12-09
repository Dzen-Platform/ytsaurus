#include "job.h"
#include "exec_node.h"
#include "helpers.h"
#include "operation.h"
#include "operation_controller.h"

namespace NYT {
namespace NScheduler {

using namespace NNodeTrackerClient::NProto;
using namespace NYTree;
using namespace NJobTrackerClient;
using namespace NChunkClient::NProto;

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
    , OperationId_(operation->GetId())
    , Node_(node)
    , StartTime_(startTime)
    , Restarted_(restarted)
    , State_(EJobState::Waiting)
    , ResourceUsage_(resourceLimits)
    , ResourceLimits_(resourceLimits)
    , SpecBuilder_(std::move(specBuilder))
{ }

void TJob::FinalizeJob(const TInstant& finishTime)
{
    FinishTime_ = finishTime;
    Statistics_.AddSample("/time/total", GetDuration().MilliSeconds());
    if (Result()->has_prepare_time()) {
        Statistics_.AddSample("/time/prepare", Result()->prepare_time());
    }

    if (Result()->has_exec_time()) {
        Statistics_.AddSample("/time/exec", Result()->exec_time());
    }
}

TDuration TJob::GetDuration() const
{
    return *FinishTime_ - StartTime_;
}

void TJob::SetResult(NJobTrackerClient::NProto::TJobResult&& result)
{
    Result_ = New<TRefCountedJobResult>(std::move(result));
    Statistics_ = FromProto<TStatistics>(Result()->statistics());
}

Stroka TJob::GetStatisticsSuffix() const
{
    if (GetRestarted() && GetState() == EJobState::Completed) {
        return Format("/$/lost/%lv", GetType());
    } else {
        return Format("/$/%lv/%lv", GetState(), GetType());
    }
}

////////////////////////////////////////////////////////////////////

TJobSummary::TJobSummary(TJobPtr job)
    : Result(job->Result())
    , Id(job->GetId())
    , Statistics(job->Statistics())
    , StatisticsSuffix(job->GetStatisticsSuffix())
{ }

TJobSummary::TJobSummary(const TJobId& id)
    : Result()
    , Id(id)
    , Statistics()
    , StatisticsSuffix()
{ }

////////////////////////////////////////////////////////////////////

TCompletedJobSummary::TCompletedJobSummary(TJobPtr job, bool abandoned)
    : TJobSummary(job)
    , Abandoned(abandoned)
{ }

////////////////////////////////////////////////////////////////////

TAbortedJobSummary::TAbortedJobSummary(const TJobId& id, EAbortReason abortReason)
    : TJobSummary(id)
    , AbortReason(abortReason)
{ }

TAbortedJobSummary::TAbortedJobSummary(TJobPtr job)
    : TJobSummary(job)
    , AbortReason(GetAbortReason(Result))
{ }

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
