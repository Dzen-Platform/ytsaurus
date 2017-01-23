#include "job_prober_service.h"
#include "private.h"
#include "scheduler.h"

#include <yt/server/cell_scheduler/bootstrap.h>

#include <yt/ytlib/api/client.h>

#include <yt/ytlib/scheduler/helpers.h>
#include <yt/ytlib/scheduler/job_prober_service_proxy.h>

#include <yt/core/rpc/service_detail.h>

#include <yt/core/ytree/permission.h>

#include <yt/core/misc/signaler.h>

namespace NYT {
namespace NScheduler {

using namespace NRpc;
using namespace NCellScheduler;
using namespace NConcurrency;
using namespace NYson;
using namespace NYTree;
using namespace NSecurityClient;

////////////////////////////////////////////////////////////////////

class TJobProberService
    : public TServiceBase
{
public:
    TJobProberService(TBootstrap* bootstrap)
        : TServiceBase(
            bootstrap->GetControlInvoker(),
            TJobProberServiceProxy::GetDescriptor(),
            SchedulerLogger)
        , Bootstrap_(bootstrap)
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(DumpInputContext));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetJobNode));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Strace));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(SignalJob));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AbandonJob));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(PollJobShell));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AbortJob));
    }

private:
    TBootstrap* const Bootstrap_;

    DECLARE_RPC_SERVICE_METHOD(NProto, DumpInputContext)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        const auto& path = request->path();
        context->SetRequestInfo("JobId: %v, Path: %v",
            jobId,
            path);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        WaitFor(scheduler->DumpInputContext(jobId, path, context->GetUser()))
            .ThrowOnError();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, GetJobNode)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        context->SetRequestInfo("JobId: %v", jobId);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        auto jobNodeDescriptor = WaitFor(scheduler->GetJobNode(jobId, context->GetUser()))
            .ValueOrThrow();

        context->SetResponseInfo("NodeDescriptor: %v", jobNodeDescriptor);

        ToProto(response->mutable_node_descriptor(), jobNodeDescriptor);

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Strace)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        context->SetRequestInfo("JobId: %v", jobId);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        auto trace = WaitFor(scheduler->Strace(jobId, context->GetUser()))
            .ValueOrThrow();

        context->SetResponseInfo("Trace: %v", trace.GetData());

        ToProto(response->mutable_trace(), trace.GetData());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, SignalJob)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        const auto& signalName = request->signal_name();

        ValidateSignalName(request->signal_name());

        context->SetRequestInfo("JobId: %v, SignalName: %v",
            jobId,
            signalName);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        WaitFor(scheduler->SignalJob(jobId, signalName, context->GetUser()))
            .ThrowOnError();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, AbandonJob)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        context->SetRequestInfo("JobId: %v", jobId);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        WaitFor(scheduler->AbandonJob(jobId, context->GetUser()))
            .ThrowOnError();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, PollJobShell)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        auto parameters = TYsonString(request->parameters());

        context->SetRequestInfo("JobId: %v, Parameters: %v",
            jobId,
            ConvertToYsonString(parameters, EYsonFormat::Text));

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        auto result = WaitFor(scheduler->PollJobShell(jobId, parameters, context->GetUser()))
            .ValueOrThrow();

        ToProto(response->mutable_result(), result.GetData());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, AbortJob)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        auto interruptTimeout = request->has_interrupt_timeout()
            ? MakeNullable(FromProto<TDuration>(request->interrupt_timeout()))
            : Null;
        context->SetRequestInfo("JobId: %v, InterruptTimeout: %v", jobId, interruptTimeout);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        WaitFor(scheduler->AbortJob(jobId, interruptTimeout, context->GetUser()))
            .ThrowOnError();

        context->Reply();
    }
};

IServicePtr CreateJobProberService(TBootstrap* bootstrap)
{
    return New<TJobProberService>(bootstrap);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
