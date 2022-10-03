#include "job_tracker_service.h"
#include "bootstrap.h"
#include "config.h"
#include "controller_agent.h"
#include "operation.h"
#include "private.h"

#include <yt/yt/server/lib/controller_agent/job_tracker_service_proxy.h>

#include <yt/yt/core/rpc/service_detail.h>

namespace NYT::NControllerAgent {

using namespace NConcurrency;

using NYT::FromProto;
using NYT::ToProto;

using namespace NRpc;
using NJobTrackerClient::EJobState;

////////////////////////////////////////////////////////////////////

class TJobTrackerService
    : public TServiceBase
{
public:
    explicit TJobTrackerService(TBootstrap* bootstrap)
        : TServiceBase(
            NRpc::TDispatcher::Get()->GetHeavyInvoker(),
            TJobTrackerServiceProxy::GetDescriptor(),
            ControllerAgentLogger,
            NullRealmId,
            bootstrap->GetNativeAuthenticator())
        , Bootstrap_(bootstrap)
        , HeartbeatStatisticBytes_(ControllerAgentProfiler.WithHot().Counter("/node_heartbeat/statistic_bytes"))
        , HeartbeatJobResultBytes_(ControllerAgentProfiler.WithHot().Counter("/node_heartbeat/job_result_bytes"))
        , HeartbeatProtoMessageBytes_(ControllerAgentProfiler.WithHot().Counter("/node_heartbeat/proto_message_bytes"))
        , HeartbeatCount_(ControllerAgentProfiler.WithHot().Counter("/node_heartbeat/count"))
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Heartbeat));
    }

private:
    TBootstrap* const Bootstrap_;
    NProfiling::TCounter HeartbeatStatisticBytes_;
    NProfiling::TCounter HeartbeatJobResultBytes_;
    NProfiling::TCounter HeartbeatProtoMessageBytes_;
    NProfiling::TCounter HeartbeatCount_;

    void ProfileHeartbeatRequest(NProto::TReqHeartbeat* const request)
    {
        i64 totalJobStatisticsSize = 0;
        i64 totalJobResultSize = 0;
        for (auto& job : *request->mutable_jobs()) {
            if (job.has_statistics()) {
                totalJobStatisticsSize += std::size(job.statistics());
            }
            if (job.has_result()) {
                totalJobResultSize += job.result().ByteSizeLong();
            }
        }

        HeartbeatProtoMessageBytes_.Increment(request->ByteSizeLong());
        HeartbeatStatisticBytes_.Increment(totalJobStatisticsSize);
        HeartbeatJobResultBytes_.Increment(totalJobResultSize);
        HeartbeatCount_.Increment();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Heartbeat)
    {
        ProfileHeartbeatRequest(request);
        THashMap<TOperationId, std::vector<std::unique_ptr<TJobSummary>>> groupedJobSummaries;
        for (auto& job : *request->mutable_jobs()) {
            const auto operationId = FromProto<TOperationId>(job.operation_id());
            groupedJobSummaries[operationId].push_back(ParseJobSummary(&job, Logger));
        }

        SwitchTo(Bootstrap_->GetControlInvoker());

        const auto& controllerAgent = Bootstrap_->GetControllerAgent();
        if (FromProto<NScheduler::TIncarnationId>(request->controller_agent_incarnation_id()) != controllerAgent->GetIncarnationId()) {
            context->Reply(TError{EErrorCode::IncarnationMismatch, "Controller agent incarnation mismatch"});
            return;
        }
        controllerAgent->ValidateConnected();
        context->Reply();

        for (auto& [operationId, jobSummaries] : groupedJobSummaries) {
            const auto operation = controllerAgent->FindOperation(operationId);
            if (!operation) {
                continue;
            }
            const auto controller = operation->GetController();
            controller->GetCancelableInvoker(controllerAgent->GetConfig()->JobEventsControllerQueue)->Invoke(
                BIND(
                    [&Logger{Logger}, controller, jobSummaries{std::move(jobSummaries)}] () mutable {
                        for (auto& jobSummary : jobSummaries) {
                            const auto jobState = jobSummary->State;
                            const auto jobId = jobSummary->Id;

                            try {
                                controller->OnJobInfoReceivedFromNode(std::move(jobSummary));
                            } catch (const std::exception& ex) {
                                YT_LOG_WARNING(
                                    ex,
                                    "Failed to process job info from node (JobId: %v, JobState: %v)",
                                    jobId,
                                    jobState);
                            }
                        }
                    }));
        }
    }
};

////////////////////////////////////////////////////////////////////

IServicePtr CreateJobTrackerService(TBootstrap* const bootstrap)
{
    return New<TJobTrackerService>(bootstrap);
}

////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
