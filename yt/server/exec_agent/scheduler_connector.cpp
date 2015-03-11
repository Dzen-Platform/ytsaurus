﻿#include "stdafx.h"
#include "scheduler_connector.h"
#include "private.h"
#include "job.h"
#include "config.h"

#include <ytlib/api/client.h>

#include <server/job_agent/job_controller.h>

#include <server/data_node/master_connector.h>

#include <server/cell_node/bootstrap.h>

namespace NYT {
namespace NExecAgent {

using namespace NNodeTrackerClient;
using namespace NJobTrackerClient;
using namespace NCellNode;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ExecAgentLogger;

////////////////////////////////////////////////////////////////////////////////

TSchedulerConnector::TSchedulerConnector(
    TSchedulerConnectorConfigPtr config,
    TBootstrap* bootstrap)
    : Config(config)
    , Bootstrap(bootstrap)
    , ControlInvoker(bootstrap->GetControlInvoker())
{
    YCHECK(config);
    YCHECK(bootstrap);
}

void TSchedulerConnector::Start()
{
    HeartbeatExecutor = New<TPeriodicExecutor>(
        ControlInvoker,
        BIND(&TSchedulerConnector::SendHeartbeat, MakeWeak(this)),
        Config->HeartbeatPeriod,
        EPeriodicExecutorMode::Manual,
        Config->HeartbeatSplay);

    // Schedule an out-of-order heartbeat whenever a job finishes
    // or its resource usage is updated.
    auto jobController = Bootstrap->GetJobController();
    jobController->SubscribeResourcesUpdated(BIND(
        &TPeriodicExecutor::ScheduleOutOfBand,
        HeartbeatExecutor));

    HeartbeatExecutor->Start();
}

void TSchedulerConnector::SendHeartbeat()
{
    auto masterConnector = Bootstrap->GetMasterConnector();
    if (!masterConnector->IsConnected()) {
        HeartbeatExecutor->ScheduleNext();
        return;
    }

    TJobTrackerServiceProxy proxy(Bootstrap->GetMasterClient()->GetSchedulerChannel());
    auto req = proxy.Heartbeat();

    auto jobController = Bootstrap->GetJobController();
    jobController->PrepareHeartbeat(req.Get());

    req->Invoke().Subscribe(
        BIND(&TSchedulerConnector::OnHeartbeatResponse, MakeStrong(this))
            .Via(ControlInvoker));

    LOG_INFO("Scheduler heartbeat sent (ResourceUsage: {%v})",
        FormatResourceUsage(req->resource_usage(), req->resource_limits()));
}

void TSchedulerConnector::OnHeartbeatResponse(const TJobTrackerServiceProxy::TErrorOrRspHeartbeatPtr& rspOrError)
{
    HeartbeatExecutor->ScheduleNext();

    if (!rspOrError.IsOK()) {
        LOG_ERROR(rspOrError, "Error reporting heartbeat to scheduler");
        return;
    }

    LOG_INFO("Successfully reported heartbeat to scheduler");

    const auto& rsp = rspOrError.Value();
    auto jobController = Bootstrap->GetJobController();
    jobController->ProcessHeartbeat(rsp.Get());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
