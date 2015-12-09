#pragma once

#include "public.h"

#include <yt/server/cell_node/public.h>

#include <yt/ytlib/job_tracker_client/job_tracker_service_proxy.h>

#include <yt/core/concurrency/periodic_executor.h>

namespace NYT {
namespace NExecAgent {

////////////////////////////////////////////////////////////////////////////////

class TSchedulerConnector
    : public TRefCounted
{
public:
    TSchedulerConnector(
        TSchedulerConnectorConfigPtr config,
        NCellNode::TBootstrap* bootstrap);

    void Start();

private:
    const TSchedulerConnectorConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;
    const IInvokerPtr ControlInvoker_;

    NConcurrency::TPeriodicExecutorPtr HeartbeatExecutor_;


    void SendHeartbeat();

};

DEFINE_REFCOUNTED_TYPE(TSchedulerConnector)

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
