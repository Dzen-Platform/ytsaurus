#include "private.h"
#include "job.h"
#include "exec_node.h"
#include "helpers.h"
#include "operation.h"
#include "operation_controller.h"

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/misc/enum.h>
#include <yt/core/misc/protobuf_helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NNodeTrackerClient::NProto;
using namespace NYTree;
using namespace NYson;
using namespace NObjectClient;
using namespace NYPath;
using namespace NJobTrackerClient;
using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;

////////////////////////////////////////////////////////////////////

static class TJobHelper
{
public:
    TJobHelper()
    {
        for (auto state : TEnumTraits<EJobState>::GetDomainValues()) {
            for (auto type : TEnumTraits<EJobType>::GetDomainValues()) {
                StatisticsSuffixes_[state][type] = Format("/$/%lv/%lv", state, type);
            }
        }
    }

    const Stroka& GetStatisticsSuffix(EJobState state, EJobType type) const
    {
        return StatisticsSuffixes_[state][type];
    }

private:
    TEnumIndexedVector<TEnumIndexedVector<Stroka, EJobType>, EJobState> StatisticsSuffixes_;

} JobHelper;

////////////////////////////////////////////////////////////////////

TJob::TJob(
    const TJobId& id,
    EJobType type,
    const TOperationId& operationId,
    TExecNodePtr node,
    TInstant startTime,
    const TJobResources& resourceLimits,
    bool restarted,
    TJobSpecBuilder specBuilder)
    : Id_(id)
    , Type_(type)
    , OperationId_(operationId)
    , Node_(node)
    , StartTime_(startTime)
    , Restarted_(restarted)
    , State_(EJobState::None)
    , ResourceUsage_(resourceLimits)
    , ResourceLimits_(resourceLimits)
    , SpecBuilder_(std::move(specBuilder))
{ }

TDuration TJob::GetDuration() const
{
    return *FinishTime_ - StartTime_;
}

TFuture<TBriefJobStatisticsPtr> TJob::BuildBriefStatistics(IInvokerPtr invoker) const
{
    return BIND(&TJob::DoBuildBriefStatistics, MakeStrong(this)).AsyncVia(invoker).Run();
}

TBriefJobStatisticsPtr TJob::DoBuildBriefStatistics() const
{
    auto statistics = ConvertTo<NJobTrackerClient::TStatistics>(*StatisticsYson_);

    auto getValue = [] (const TSummary& summary) {
        return summary.GetSum();
    };

    auto briefStatistics = New<TBriefJobStatistics>();

    briefStatistics->ProcessedInputRowCount = GetValues<i64>(statistics, "/data/input/row_count", getValue);
    briefStatistics->ProcessedInputDataSize = GetValues<i64>(statistics, "/data/input/uncompressed_data_size", getValue);

    if (std::any_of(
        statistics.Data().begin(),
        statistics.Data().end(),
        [] (auto pair) { return HasPrefix(pair.first, "/user_job"); }))
    {
        briefStatistics->UserJobCpuUsage = GetValues<i64>(statistics, "/user_job/cpu/user", getValue);
    }

    auto outputDataStatistics = GetTotalOutputDataStatistics(statistics);

    briefStatistics->ProcessedOutputDataSize = outputDataStatistics.uncompressed_data_size();
    briefStatistics->ProcessedOutputRowCount = outputDataStatistics.row_count();
    return briefStatistics;
}

void TJob::AnalyzeBriefStatistics(
    TDuration suspiciousInactivityTimeout,
    i64 suspiciousUserJobCpuUsageThreshold,
    TBriefJobStatisticsPtr briefStatistics)
{
    bool wasActive = false;

    if (!BriefStatistics_) {
        wasActive = true;
    } else if (briefStatistics->ProcessedInputRowCount != BriefStatistics_->ProcessedInputRowCount ||
        briefStatistics->ProcessedInputDataSize != BriefStatistics_->ProcessedInputDataSize ||
        briefStatistics->ProcessedOutputRowCount != BriefStatistics_->ProcessedOutputRowCount ||
        briefStatistics->ProcessedOutputDataSize != BriefStatistics_->ProcessedOutputDataSize ||
        (briefStatistics->UserJobCpuUsage && BriefStatistics_->UserJobCpuUsage &&
        *briefStatistics->UserJobCpuUsage > *BriefStatistics_->UserJobCpuUsage + suspiciousUserJobCpuUsageThreshold))
    {
        wasActive = true;
    }
    BriefStatistics_ = briefStatistics;

    if (Suspicious_ = (!wasActive && TInstant::Now() - LastActivityTime_ > suspiciousInactivityTimeout)) {
        LOG_WARNING("Found a suspicious job (JobId: %v, LastActivityTime: %v, suspiciousInactivityTimeout: %v)",
            Id_,
            LastActivityTime_,
            suspiciousInactivityTimeout);
    }

    if (wasActive) {
        LastActivityTime_ = TInstant::Now();
    }
}

void TJob::SetStatus(TRefCountedJobStatusPtr status)
{
    Status_ = std::move(status);
    if (Status_->has_statistics()) {
        StatisticsYson_ = TYsonString(Status_->statistics());
    }
}

const Stroka& TJob::GetStatisticsSuffix() const
{
    auto state = (GetRestarted() && GetState() == EJobState::Completed) ? EJobState::Lost : GetState();
    auto type = GetType();
    return JobHelper.GetStatisticsSuffix(state, type);
}

////////////////////////////////////////////////////////////////////

TJobSummary::TJobSummary(const TJobPtr& job)
    : Result(New<TRefCountedJobResult>(job->Status()->result()))
    , Id(job->GetId())
    , StatisticsSuffix(job->GetStatisticsSuffix())
    , FinishTime(*job->GetFinishTime())
    , ShouldLog(true)
{
    const auto& status = job->Status();
    if (status->has_prepare_duration()) {
        PrepareDuration = FromProto<TDuration>(status->prepare_duration());
    }
    if (status->has_exec_duration()) {
        ExecDuration.Emplace();
        FromProto(ExecDuration.GetPtr(), status->exec_duration());
    }
    StatisticsYson = job->StatisticsYson();
}

TJobSummary::TJobSummary(const TJobId& id)
    : Result()
    , Id(id)
    , StatisticsSuffix()
    , ShouldLog(false)
{ }

void TJobSummary::ParseStatistics()
{
    if (StatisticsYson) {
        Statistics = ConvertTo<NJobTrackerClient::TStatistics>(*StatisticsYson);
    }
}

////////////////////////////////////////////////////////////////////

TCompletedJobSummary::TCompletedJobSummary(const TJobPtr& job, bool abandoned)
    : TJobSummary(job)
    , Abandoned(abandoned)
{ }

////////////////////////////////////////////////////////////////////

TAbortedJobSummary::TAbortedJobSummary(const TJobId& id, EAbortReason abortReason)
    : TJobSummary(id)
    , AbortReason(abortReason)
{ }

TAbortedJobSummary::TAbortedJobSummary(const TJobPtr& job)
    : TJobSummary(job)
    , AbortReason(GetAbortReason(job->Status()->result()))
{ }

////////////////////////////////////////////////////////////////////

TRefCountedJobStatusPtr JobStatusFromError(const TError& error)
{
    auto status = New<TRefCountedJobStatus>();
    ToProto(status->mutable_result()->mutable_error(), error);
    return status;
}

////////////////////////////////////////////////////////////////////

TJobStartRequest::TJobStartRequest(
    TJobId id,
    EJobType type,
    const TJobResources& resourceLimits,
    bool restarted,
    const TJobSpecBuilder& specBuilder)
    : Id(id)
    , Type(type)
    , ResourceLimits(resourceLimits)
    , Restarted(restarted)
    , SpecBuilder(specBuilder)
{ }

////////////////////////////////////////////////////////////////////

void TScheduleJobResult::RecordFail(EScheduleJobFailReason reason)
{
    ++Failed[reason];
}

////////////////////////////////////////////////////////////////////

TJobId MakeJobId(NObjectClient::TCellTag tag, NNodeTrackerClient::TNodeId nodeId)
{
    return MakeId(
        EObjectType::SchedulerJob,
        tag,
        RandomNumber<ui64>(),
        nodeId);
}

NNodeTrackerClient::TNodeId NodeIdFromJobId(const TJobId& jobId)
{
    return jobId.Parts32[0];
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
