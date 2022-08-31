#include "job_heartbeat_processor.h"

#include "bootstrap.h"
#include "master_connector.h"
#include "private.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>

namespace NYT::NDataNode {

using namespace NJobAgent;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

using TJobController = NJobAgent::TJobController;
using EJobState = NJobTrackerClient::EJobState;

using namespace NObjectClient;
using namespace NJobTrackerClient::NProto;

////////////////////////////////////////////////////////////////////////////////

void TMasterJobHeartbeatProcessor::ProcessResponse(
    const TString& jobTrackerAddress,
    const TJobController::TRspHeartbeatPtr& response)
{
    ProcessHeartbeatCommonResponsePart(response);

    YT_VERIFY(std::ssize(response->Attachments()) == response->jobs_to_start_size());
    int attachmentIndex = 0;
    for (const auto& startInfo : response->jobs_to_start()) {
        auto operationId = FromProto<TOperationId>(startInfo.operation_id());
        auto jobId = FromProto<TJobId>(startInfo.job_id());
        YT_LOG_DEBUG("Job spec is passed via attachments "
            "(OperationId: %v, JobId: %v, JobTrackerAddress: %v)",
            operationId,
            jobId,
            jobTrackerAddress);

        const auto& attachment = response->Attachments()[attachmentIndex];

        TJobSpec spec;
        DeserializeProtoWithEnvelope(&spec, attachment);

        const auto& resourceLimits = startInfo.resource_limits();

        CreateMasterJob(
            jobId,
            operationId,
            jobTrackerAddress,
            resourceLimits,
            std::move(spec));

        ++attachmentIndex;
    }
}

void TMasterJobHeartbeatProcessor::PrepareRequest(
    TCellTag cellTag,
    const TString& jobTrackerAddress,
    const TJobController::TReqHeartbeatPtr& request)
{
    PrepareHeartbeatCommonRequestPart(request);

    for (const auto& job : JobController_->GetMasterJobs()) {
        auto jobId = job->GetId();

        YT_VERIFY(TypeFromId(jobId) == EObjectType::MasterJob);

        if (job->GetJobTrackerAddress() != jobTrackerAddress) {
            continue;
        }

        YT_VERIFY(CellTagFromId(jobId) == cellTag);

        auto* jobStatus = request->add_jobs();
        FillJobStatus(jobStatus, job);
        switch (job->GetState()) {
            case EJobState::Running:
                *jobStatus->mutable_resource_usage() = job->GetResourceUsage();
                break;

            case EJobState::Completed:
            case EJobState::Aborted:
            case EJobState::Failed:
                *jobStatus->mutable_result() = job->GetResult();
                if (auto statistics = job->GetStatistics()) {
                    auto statisticsString = statistics.ToString();
                    job->ResetStatisticsLastSendTime();
                    jobStatus->set_statistics(statisticsString);
                }
                break;

            default:
                break;
        }
    }

    request->set_confirmed_job_count(0);
}

void TMasterJobHeartbeatProcessor::ScheduleHeartbeat(const IJobPtr& job)
{
    YT_VERIFY(Bootstrap_->IsDataNode());
    auto* bootstrap = Bootstrap_->GetDataNodeBootstrap();
    const auto& masterConnector = bootstrap->GetMasterConnector();
    masterConnector->ScheduleJobHeartbeat(job->GetJobTrackerAddress());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
