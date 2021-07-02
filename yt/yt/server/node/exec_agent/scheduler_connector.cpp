#include "scheduler_connector.h"

#include "bootstrap.h"
#include "private.h"
#include "job.h"
#include "master_connector.h"

#include <yt/yt/server/lib/exec_agent/config.h>

#include <yt/yt/server/lib/job_agent/job_reporter.h>

#include <yt/yt/server/node/cluster_node/master_connector.h>

#include <yt/yt/server/node/exec_agent/slot_manager.h>

#include <yt/yt/server/node/job_agent/job_controller.h>

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/ytlib/node_tracker_client/helpers.h>

#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/periodic_executor.h>

namespace NYT::NExecAgent {

using namespace NNodeTrackerClient;
using namespace NJobTrackerClient;
using namespace NObjectClient;
using namespace NClusterNode;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ExecAgentLogger;

////////////////////////////////////////////////////////////////////////////////

TSchedulerConnector::TSchedulerConnector(
    TSchedulerConnectorConfigPtr config,
    IBootstrap* bootstrap)
    : Config_(config)
    , Bootstrap_(bootstrap)
    , HeartbeatExecutor_(New<TPeriodicExecutor>(
        Bootstrap_->GetControlInvoker(),
        BIND(&TSchedulerConnector::SendHeartbeat, MakeWeak(this)),
        Config_->HeartbeatPeriod,
        Config_->HeartbeatSplay))
    , TimeBetweenSentHeartbeatsCounter_(ExecAgentProfiler.Timer("/scheduler_connector/time_between_sent_heartbeats"))
    , TimeBetweenAcknowledgedHeartbeatsCounter_(ExecAgentProfiler.Timer("/scheduler_connector/time_between_acknowledged_heartbeats"))
    , TimeBetweenFullyProcessedHeartbeatsCounter_(ExecAgentProfiler.Timer("/scheduler_connector/time_between_fully_processed_heartbeats"))
{
    YT_VERIFY(config);
    YT_VERIFY(bootstrap);
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetControlInvoker(), ControlThread);
}

void TSchedulerConnector::Start()
{
    // Schedule an out-of-order heartbeat whenever a job finishes
    // or its resource usage is updated.
    const auto& jobController = Bootstrap_->GetJobController();
    jobController->SubscribeResourcesUpdated(BIND(
        &TPeriodicExecutor::ScheduleOutOfBand,
        HeartbeatExecutor_));

    HeartbeatExecutor_->Start();
}

void TSchedulerConnector::SendHeartbeat()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (!Bootstrap_->IsConnected()) {
        return;
    }

    const auto slotManager = Bootstrap_->GetSlotManager();
    if (!slotManager->IsInitialized()) {
        return;
    }

    if (TInstant::Now() < std::max(LastFailedHeartbeatTime_, LastThrottledHeartbeatTime_) + FailedHeartbeatBackoffTime_) {
        YT_LOG_INFO("Skipping heartbeat");
        return;
    }

    const auto& client = Bootstrap_->GetMasterClient();

    TJobTrackerServiceProxy proxy(client->GetSchedulerChannel());
    auto req = proxy.Heartbeat();
    req->SetRequestCodec(NCompression::ECodec::Lz4);

    const auto& jobController = Bootstrap_->GetJobController();
    const auto& masterConnection = client->GetNativeConnection();
    YT_VERIFY(WaitFor(jobController->PrepareHeartbeatRequest(
        masterConnection->GetPrimaryMasterCellTag(),
        EObjectType::SchedulerJob,
        req))
        .IsOK());

    auto profileInterval = [&] (TInstant lastTime, NProfiling::TEventTimer& counter) {
        if (lastTime != TInstant::Zero()) {
            auto delta = TInstant::Now() - lastTime;
            counter.Record(delta);
        }
    };

    profileInterval(LastSentHeartbeatTime_, TimeBetweenSentHeartbeatsCounter_);
    LastSentHeartbeatTime_ = TInstant::Now();

    YT_LOG_INFO("Scheduler heartbeat sent (ResourceUsage: %v)",
        FormatResourceUsage(req->resource_usage(), req->resource_limits(), req->disk_resources()));

    auto rspOrError = WaitFor(req->Invoke());
    if (!rspOrError.IsOK()) {
        LastFailedHeartbeatTime_ = TInstant::Now();
        if (FailedHeartbeatBackoffTime_ == TDuration::Zero()) {
            FailedHeartbeatBackoffTime_ = Config_->FailedHeartbeatBackoffStartTime;
        } else {
            FailedHeartbeatBackoffTime_ = std::min(
                FailedHeartbeatBackoffTime_ * Config_->FailedHeartbeatBackoffMultiplier,
                Config_->FailedHeartbeatBackoffMaxTime);
        }
        YT_LOG_ERROR(rspOrError, "Error reporting heartbeat to scheduler (BackoffTime: %v)",
            FailedHeartbeatBackoffTime_);
        return;
    }

    YT_LOG_INFO("Successfully reported heartbeat to scheduler");

    FailedHeartbeatBackoffTime_ = TDuration::Zero();

    profileInterval(std::max(LastFullyProcessedHeartbeatTime_, LastThrottledHeartbeatTime_), TimeBetweenAcknowledgedHeartbeatsCounter_);

    const auto& rsp = rspOrError.Value();
    if (rsp->scheduling_skipped()) {
        LastThrottledHeartbeatTime_ = TInstant::Now();
    } else {
        profileInterval(LastFullyProcessedHeartbeatTime_, TimeBetweenFullyProcessedHeartbeatsCounter_);
        LastFullyProcessedHeartbeatTime_ = TInstant::Now();
    }

    const auto& reporter = Bootstrap_->GetJobReporter();
    if (rsp->has_enable_job_reporter()) {
        reporter->SetEnabled(rsp->enable_job_reporter());
    }
    if (rsp->has_enable_job_spec_reporter()) {
        reporter->SetSpecEnabled(rsp->enable_job_spec_reporter());
    }
    if (rsp->has_enable_job_stderr_reporter()) {
        reporter->SetStderrEnabled(rsp->enable_job_stderr_reporter());
    }
    if (rsp->has_enable_job_profile_reporter()) {
        reporter->SetProfileEnabled(rsp->enable_job_profile_reporter());
    }
    if (rsp->has_enable_job_fail_context_reporter()) {
        reporter->SetFailContextEnabled(rsp->enable_job_fail_context_reporter());
    }
    if (rsp->has_operation_archive_version()) {
        reporter->SetOperationArchiveVersion(rsp->operation_archive_version());
    }

    // TODO(ignat): it should not throw.
    WaitFor(jobController->ProcessHeartbeatResponse(rsp, EObjectType::SchedulerJob))
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecAgent
