#include "job_tracker_service.h"
#include "private.h"
#include "scheduler.h"

#include <yt/server/cell_scheduler/bootstrap.h>

#include <yt/ytlib/job_tracker_client/job_tracker_service_proxy.h>

#include <yt/ytlib/node_tracker_client/helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NRpc;
using namespace NCellScheduler;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerServer;

////////////////////////////////////////////////////////////////////

class TJobTrackerService
    : public TServiceBase
{
public:
    TJobTrackerService(TBootstrap* bootstrap)
        : NRpc::TServiceBase(
            bootstrap->GetControlInvoker(),
            TJobTrackerServiceProxy::GetServiceName(),
            SchedulerLogger,
            TJobTrackerServiceProxy::GetProtocolVersion())
        , Bootstrap_(bootstrap)
    {
        RegisterMethod(
            RPC_SERVICE_METHOD_DESC(Heartbeat)
                .SetHeavy(true)
                .SetResponseCodec(NCompression::ECodec::Lz4)
                .SetInvoker(bootstrap->GetControlInvoker(EControlQueue::Heartbeat)));
    }

private:
    TBootstrap* const Bootstrap_;


    DECLARE_RPC_SERVICE_METHOD(NJobTrackerClient::NProto, Heartbeat)
    {
        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();
        scheduler->ProcessHeartbeat(context);
    }

};

IServicePtr CreateJobTrackerService(TBootstrap* bootstrap)
{
    return New<TJobTrackerService>(bootstrap);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

