#include "supervisor_service.h"
#include "private.h"
#include "job.h"
#include "supervisor_service_proxy.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/server/job_agent/job_controller.h>
#include <yt/server/job_agent/public.h>

#include <yt/ytlib/node_tracker_client/helpers.h>

namespace NYT {
namespace NExecAgent {

using namespace NJobAgent;
using namespace NNodeTrackerClient;
using namespace NCellNode;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TSupervisorService::TSupervisorService(TBootstrap* bootstrap)
    : NRpc::TServiceBase(
        bootstrap->GetControlInvoker(),
        TSupervisorServiceProxy::GetDescriptor(),
        ExecAgentLogger)
    , Bootstrap(bootstrap)
{
    RegisterMethod(
        RPC_SERVICE_METHOD_DESC(GetJobSpec)
        .SetResponseCodec(NCompression::ECodec::Lz4)
        .SetHeavy(true));
    RegisterMethod(RPC_SERVICE_METHOD_DESC(OnJobFinished));
    RegisterMethod(RPC_SERVICE_METHOD_DESC(OnJobProgress)
        .SetOneWay(true));
    RegisterMethod(RPC_SERVICE_METHOD_DESC(OnJobPrepared)
        .SetOneWay(true));
    RegisterMethod(RPC_SERVICE_METHOD_DESC(UpdateResourceUsage)
        .SetOneWay(true));
}

DEFINE_RPC_SERVICE_METHOD(TSupervisorService, GetJobSpec)
{
    auto jobId = FromProto<TJobId>(request->job_id());
    context->SetRequestInfo("JobId: %v", jobId);

    auto jobController = Bootstrap->GetJobController();
    auto job = jobController->GetJobOrThrow(jobId);

    *response->mutable_job_spec() = job->GetSpec();
    const auto& resources = job->GetResourceUsage();

    auto* jobProxyResources = response->mutable_resource_usage();
    jobProxyResources->set_cpu(resources.cpu());
    jobProxyResources->set_memory(resources.memory());
    jobProxyResources->set_network(resources.network());

    context->Reply();
}

DEFINE_RPC_SERVICE_METHOD(TSupervisorService, OnJobFinished)
{
    auto jobId = FromProto<TJobId>(request->job_id());
    const auto& result = request->result();
    auto error = FromProto<TError>(result.error());
    context->SetRequestInfo("JobId: %v, Error: %v",
        jobId,
        error);

    auto jobController = Bootstrap->GetJobController();
    auto job = jobController->GetJobOrThrow(jobId);

    job->SetResult(result);

    auto statistics = TJobStatistics().Error(error);
    if (request->has_statistics()) {
        auto ysonStatistics = TYsonString(request->statistics());
        job->SetStatistics(ysonStatistics);
        statistics.SetStatistics(ysonStatistics);
    }
    if (request->has_start_time()) {
        statistics.SetStartTime(FromProto<TInstant>(request->start_time()));
    }
    if (request->has_finish_time()) {
        statistics.SetFinishTime(FromProto<TInstant>(request->finish_time()));
    }
    job->ReportStatistics(std::move(statistics));

    context->Reply();
}

DEFINE_ONE_WAY_RPC_SERVICE_METHOD(TSupervisorService, OnJobProgress)
{
    auto jobId = FromProto<TJobId>(request->job_id());
    double progress = request->progress();
    const auto& statistics = TYsonString(request->statistics());

    context->SetRequestInfo("JobId: %v, Progress: %lf, Statistics: %v",
        jobId,
        progress,
        NYTree::ConvertToYsonString(statistics, EYsonFormat::Text).GetData());

    auto jobController = Bootstrap->GetJobController();
    auto job = jobController->GetJobOrThrow(jobId);

    job->SetProgress(progress);
    job->SetStatistics(statistics);
}

DEFINE_ONE_WAY_RPC_SERVICE_METHOD(TSupervisorService, OnJobPrepared)
{
    auto jobId = FromProto<TJobId>(request->job_id());

    context->SetRequestInfo("JobId: %v", jobId);

    auto jobController = Bootstrap->GetJobController();
    auto job = jobController->GetJobOrThrow(jobId);

    job->OnJobPrepared();
}

DEFINE_ONE_WAY_RPC_SERVICE_METHOD(TSupervisorService, UpdateResourceUsage)
{
    auto jobId = FromProto<TJobId>(request->job_id());
    const auto& jobProxyResourceUsage = request->resource_usage();

    context->SetRequestInfo("JobId: %v, JobProxyResourceUsage: {Cpu: %v, Memory %v, Network: %v}",
        jobId,
        jobProxyResourceUsage.cpu(),
        jobProxyResourceUsage.memory(),
        jobProxyResourceUsage.network());

    auto jobController = Bootstrap->GetJobController();
    auto job = jobController->GetJobOrThrow(jobId);

    auto resourceUsage = job->GetResourceUsage();
    resourceUsage.set_memory(jobProxyResourceUsage.memory());
    resourceUsage.set_cpu(jobProxyResourceUsage.cpu());
    resourceUsage.set_network(jobProxyResourceUsage.network());

    job->SetResourceUsage(resourceUsage);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
