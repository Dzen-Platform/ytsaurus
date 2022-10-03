#include "job_tracker_service.h"
#include "private.h"
#include "scheduler.h"
#include "bootstrap.h"

#include <yt/yt/ytlib/job_tracker_client/job_tracker_service_proxy.h>

namespace NYT::NScheduler {

using namespace NRpc;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

class TJobTrackerService
    : public TServiceBase
{
public:
    explicit TJobTrackerService(TBootstrap* bootstrap)
        : NRpc::TServiceBase(
            GetSyncInvoker(),
            TJobTrackerServiceProxy::GetDescriptor(),
            SchedulerLogger,
            NullRealmId,
            bootstrap->GetNativeAuthenticator())
        , Bootstrap_(bootstrap)
    {
        RegisterMethod(
            RPC_SERVICE_METHOD_DESC(Heartbeat)
                .SetHeavy(true)
                .SetResponseCodec(NCompression::ECodec::Lz4)
                .SetPooled(false));
    }

private:
    TBootstrap* const Bootstrap_;


    DECLARE_RPC_SERVICE_METHOD(NJobTrackerClient::NProto, Heartbeat)
    {
        const auto& scheduler = Bootstrap_->GetScheduler();
        scheduler->ProcessNodeHeartbeat(context);
    }
};

IServicePtr CreateJobTrackerService(TBootstrap* bootstrap)
{
    return New<TJobTrackerService>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler

