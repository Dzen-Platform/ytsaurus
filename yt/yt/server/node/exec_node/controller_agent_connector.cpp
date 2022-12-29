#include "controller_agent_connector.h"

#include "bootstrap.h"
#include "job.h"
#include "job_controller.h"
#include "private.h"

#include <yt/yt/server/node/cluster_node/master_connector.h>

#include <yt/yt/server/lib/exec_node/config.h>
#include <yt/yt/server/lib/controller_agent/job_tracker_service_proxy.h>

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/ytlib/controller_agent/public.h>

#include <yt/yt/core/concurrency/throughput_throttler.h>

namespace NYT::NExecNode {

using namespace NConcurrency;
using namespace NObjectClient;
using namespace NRpc;

using NControllerAgent::TJobTrackerServiceProxy;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ExecNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TControllerAgentConnectorPool::TControllerAgentConnector::TControllerAgentConnector(
    TControllerAgentConnectorPool* controllerAgentConnectorPool,
    TControllerAgentDescriptor controllerAgentDescriptor)
    : ControllerAgentConnectorPool_(MakeStrong(controllerAgentConnectorPool))
    , ControllerAgentDescriptor_(std::move(controllerAgentDescriptor))
    , Channel_(ControllerAgentConnectorPool_->CreateChannel(ControllerAgentDescriptor_))
    , HeartbeatExecutor_(New<TPeriodicExecutor>(
        ControllerAgentConnectorPool_->Bootstrap_->GetJobInvoker(),
        BIND_NO_PROPAGATE(&TControllerAgentConnector::SendHeartbeat, MakeWeak(this)),
        TPeriodicExecutorOptions{
            .Period = ControllerAgentConnectorPool_->CurrentConfig_->HeartbeatPeriod,
            .Splay = ControllerAgentConnectorPool_->CurrentConfig_->HeartbeatSplay
        }))
    , StatisticsThrottler_(CreateReconfigurableThroughputThrottler(
        ControllerAgentConnectorPool_->CurrentConfig_->StatisticsThrottler))
    , RunningJobInfoSendingBackoff_(
        ControllerAgentConnectorPool_->CurrentConfig_->RunningJobInfoSendingBackoff)
{
    YT_LOG_DEBUG("Controller agent connector created (AgentAddress: %v, IncarnationId: %v)",
        ControllerAgentDescriptor_.Address,
        ControllerAgentDescriptor_.IncarnationId);
    HeartbeatExecutor_->Start();
}

NRpc::IChannelPtr TControllerAgentConnectorPool::TControllerAgentConnector::GetChannel() const noexcept
{
    return Channel_;
}

void TControllerAgentConnectorPool::TControllerAgentConnector::SendOutOfBandHeartbeatIfNeeded()
{
    VERIFY_INVOKER_THREAD_AFFINITY(ControllerAgentConnectorPool_->Bootstrap_->GetJobInvoker(), JobThread);

    if (ShouldSendOutOfBand_) {
        HeartbeatExecutor_->ScheduleOutOfBand();
        ShouldSendOutOfBand_ = false;
    }
}

void TControllerAgentConnectorPool::TControllerAgentConnector::EnqueueFinishedJob(const TJobPtr& job)
{
    VERIFY_INVOKER_THREAD_AFFINITY(ControllerAgentConnectorPool_->Bootstrap_->GetJobInvoker(), JobThread);

    EnqueuedFinishedJobs_.insert(job);
    ShouldSendOutOfBand_ = true;
}

void TControllerAgentConnectorPool::TControllerAgentConnector::OnConfigUpdated()
{
    VERIFY_INVOKER_THREAD_AFFINITY(ControllerAgentConnectorPool_->Bootstrap_->GetJobInvoker(), JobThread);

    const auto& currentConfig = *ControllerAgentConnectorPool_->CurrentConfig_;

    HeartbeatExecutor_->SetPeriod(currentConfig.HeartbeatPeriod);
    RunningJobInfoSendingBackoff_ = currentConfig.RunningJobInfoSendingBackoff;
    StatisticsThrottler_->Reconfigure(currentConfig.StatisticsThrottler);
}

TControllerAgentConnectorPool::TControllerAgentConnector::~TControllerAgentConnector()
{
    VERIFY_INVOKER_THREAD_AFFINITY(ControllerAgentConnectorPool_->Bootstrap_->GetJobInvoker(), JobThread);

    YT_LOG_DEBUG("Controller agent connector destroyed (AgentAddress: %v, IncarnationId: %v)",
        ControllerAgentDescriptor_.Address,
        ControllerAgentDescriptor_.IncarnationId);

    HeartbeatExecutor_->Stop();
}

// This method will be called in control thread when controller agent controls job lifetime.
void TControllerAgentConnectorPool::TControllerAgentConnector::SendHeartbeat()
{
    VERIFY_INVOKER_THREAD_AFFINITY(ControllerAgentConnectorPool_->Bootstrap_->GetJobInvoker(), JobThread);

    if (!ControllerAgentConnectorPool_->Bootstrap_->IsConnected()) {
        return;
    }

    if (TInstant::Now() < HeartbeatInfo_.LastFailedHeartbeatTime + HeartbeatInfo_.FailedHeartbeatBackoffTime) {
        YT_LOG_INFO(
            "Skipping heartbeat to agent since backoff after previous heartbeat failure (AgentAddress: %v, IncarnationId: %v)",
            ControllerAgentDescriptor_.Address,
            ControllerAgentDescriptor_.IncarnationId);
        return;
    }

    TJobTrackerServiceProxy proxy(Channel_);
    auto request = proxy.Heartbeat();

    auto context = New<TAgentHeartbeatContext>();

    context->AgentDescriptor = ControllerAgentDescriptor_;
    context->StatisticsThrottler = StatisticsThrottler_;
    context->RunningJobInfoSendingBackoff = RunningJobInfoSendingBackoff_;
    context->LastTotalConfirmationTime = LastTotalConfirmationTime_;
    context->SentEnqueuedJobs = std::move(EnqueuedFinishedJobs_);

    const auto& jobController = ControllerAgentConnectorPool_->Bootstrap_->GetJobController();

    jobController->PrepareAgentHeartbeatRequest(request, context);

    HeartbeatInfo_.LastSentHeartbeatTime = TInstant::Now();

    if (ControllerAgentConnectorPool_->TestHeartbeatDelay_) {
        TDelayedExecutor::WaitForDuration(ControllerAgentConnectorPool_->TestHeartbeatDelay_);
    }

    auto requestFuture = request->Invoke();
    YT_LOG_INFO(
        "Heartbeat sent to agent (AgentAddress: %v, IncarnationId: %v)",
        ControllerAgentDescriptor_.Address,
        ControllerAgentDescriptor_.IncarnationId);

    auto responseOrError = WaitFor(std::move(requestFuture));
    if (!responseOrError.IsOK()) {
        HeartbeatInfo_.LastFailedHeartbeatTime = TInstant::Now();
        if (HeartbeatInfo_.FailedHeartbeatBackoffTime == TDuration::Zero()) {
            HeartbeatInfo_.FailedHeartbeatBackoffTime = ControllerAgentConnectorPool_->CurrentConfig_->FailedHeartbeatBackoffStartTime;
        } else {
            HeartbeatInfo_.FailedHeartbeatBackoffTime = std::min(
                HeartbeatInfo_.FailedHeartbeatBackoffTime * ControllerAgentConnectorPool_->CurrentConfig_->FailedHeartbeatBackoffMultiplier,
                ControllerAgentConnectorPool_->CurrentConfig_->FailedHeartbeatBackoffMaxTime);
        }
        YT_LOG_ERROR(responseOrError, "Error reporting heartbeat to agent (AgentAddress: %v, BackoffTime: %v)",
            ControllerAgentDescriptor_.Address,
            HeartbeatInfo_.FailedHeartbeatBackoffTime);

        if (responseOrError.GetCode() == NControllerAgent::EErrorCode::IncarnationMismatch) {
            OnAgentIncarnationOutdated();
        }
        return;
    }

    jobController->ProcessAgentHeartbeatResponse(responseOrError.Value(), context);

    LastTotalConfirmationTime_ = context->LastTotalConfirmationTime;

    YT_LOG_INFO(
        "Successfully reported heartbeat to agent (AgentAddress: %v, IncarnationId: %v)",
        ControllerAgentDescriptor_.Address,
        ControllerAgentDescriptor_.IncarnationId);

    HeartbeatInfo_.FailedHeartbeatBackoffTime = TDuration::Zero();
}

void TControllerAgentConnectorPool::TControllerAgentConnector::OnAgentIncarnationOutdated() noexcept
{
    VERIFY_INVOKER_THREAD_AFFINITY(ControllerAgentConnectorPool_->Bootstrap_->GetJobInvoker(), JobThread);

    HeartbeatExecutor_->Stop();
}

////////////////////////////////////////////////////////////////////////////////

TControllerAgentConnectorPool::TControllerAgentConnectorPool(
    TControllerAgentConnectorConfigPtr config,
    IBootstrap* const bootstrap)
    : StaticConfig_(std::move(config))
    , CurrentConfig_(CloneYsonSerializable(StaticConfig_))
    , Bootstrap_(bootstrap)
{ }

void TControllerAgentConnectorPool::SendOutOfBandHeartbeatsIfNeeded()
{
    for (auto& [agentDescriptor, controllerAgentConnector] : ControllerAgentConnectors_) {
        controllerAgentConnector->SendOutOfBandHeartbeatIfNeeded();
    }
}

TWeakPtr<TControllerAgentConnectorPool::TControllerAgentConnector>
TControllerAgentConnectorPool::GetControllerAgentConnector(
    const TJob* job)
{
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetJobInvoker(), JobThread);

    if (!job->GetControllerAgentDescriptor()) {
        return nullptr;
    }

    if (const auto it = ControllerAgentConnectors_.find(job->GetControllerAgentDescriptor());
        it != std::cend(ControllerAgentConnectors_))
    {
        auto result = it->second;
        YT_VERIFY(result);
        return result;
    }

    YT_LOG_DEBUG(
        "Non-registered controller agent is assigned for job (JobId: %v, ControllerAgentDescriptor: %v)",
        job->GetId(),
        job->GetControllerAgentDescriptor());

    return nullptr;
}

void TControllerAgentConnectorPool::OnDynamicConfigChanged(
    const TExecNodeDynamicConfigPtr& oldConfig,
    const TExecNodeDynamicConfigPtr& newConfig)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!newConfig->ControllerAgentConnector && !oldConfig->ControllerAgentConnector) {
        return;
    }

    TDuration testHeartbeatDelay;
    TControllerAgentConnectorConfigPtr newCurrentConfig;
    if (newConfig->ControllerAgentConnector) {
        testHeartbeatDelay = newConfig->ControllerAgentConnector->TestHeartbeatDelay;
        newCurrentConfig = StaticConfig_->ApplyDynamic(newConfig->ControllerAgentConnector);
    }

    Bootstrap_->GetJobInvoker()->Invoke(
        BIND([
            this,
            this_{MakeStrong(this)},
            newConfig{std::move(newConfig)},
            testHeartbeatDelay,
            newCurrentConfig{std::move(newCurrentConfig)}]
        {
            TestHeartbeatDelay_ = testHeartbeatDelay;
            CurrentConfig_ = newCurrentConfig ? newCurrentConfig : StaticConfig_;
            OnConfigUpdated();
        }));
}

void TControllerAgentConnectorPool::OnRegisteredAgentSetReceived(
    THashSet<TControllerAgentDescriptor> controllerAgentDescriptors)
{
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetJobInvoker(), JobThread);

    YT_LOG_DEBUG(
        "Received registered controller agents (ControllerAgentCount: %v)",
        std::size(controllerAgentDescriptors));

    THashSet<TControllerAgentDescriptor> controllerAgentDescriptorsToRemove;
    for (const auto& [descriptor, connector] : ControllerAgentConnectors_) {
        if (!controllerAgentDescriptors.contains(descriptor)) {
            YT_LOG_DEBUG(
                "Found outdated controller agent connector, remove it (ControllerAgentDescriptor: %v)",
                descriptor);

            EmplaceOrCrash(controllerAgentDescriptorsToRemove, descriptor);
        } else {
            EraseOrCrash(controllerAgentDescriptors, descriptor);
        }
    }

    {
        TForbidContextSwitchGuard guard;
        for (const auto& descriptor : controllerAgentDescriptorsToRemove) {
            EraseOrCrash(ControllerAgentConnectors_, descriptor);
        }

        for (const auto& job : Bootstrap_->GetJobController()->GetJobs()) {
            if (controllerAgentDescriptorsToRemove.contains(job->GetControllerAgentDescriptor())) {
                job->UpdateControllerAgentDescriptor(TControllerAgentDescriptor{});
            }
        }
    }

    for (auto& descriptor : controllerAgentDescriptors) {
        YT_LOG_DEBUG("Add new controller agent connector (ControllerAgentDescriptor: %v)", descriptor);
        AddControllerAgentConnector(std::move(descriptor));
    }
}

IChannelPtr TControllerAgentConnectorPool::CreateChannel(const TControllerAgentDescriptor& agentDescriptor)
{
    const auto& client = Bootstrap_->GetClient();
    const auto& channelFactory = client->GetNativeConnection()->GetChannelFactory();
    return channelFactory->CreateChannel(agentDescriptor.Address);
}

IChannelPtr TControllerAgentConnectorPool::GetOrCreateChannel(
    const TControllerAgentDescriptor& agentDescriptor)
{
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetJobInvoker(), JobThread);

    if (const auto it = ControllerAgentConnectors_.find(agentDescriptor); it != std::cend(ControllerAgentConnectors_)) {
        return it->second->GetChannel();
    }

    return CreateChannel(agentDescriptor);
}

void TControllerAgentConnectorPool::OnConfigUpdated()
{
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetJobInvoker(), JobThread);

    for (const auto& [agentDescriptor, controllerAgentConnector] : ControllerAgentConnectors_) {
        controllerAgentConnector->OnConfigUpdated();
    }
}

TWeakPtr<TControllerAgentConnectorPool::TControllerAgentConnector>
TControllerAgentConnectorPool::AddControllerAgentConnector(
    TControllerAgentDescriptor descriptor)
{
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetJobInvoker(), JobThread);

    auto controllerAgentConnector = New<TControllerAgentConnector>(this, descriptor);

    EmplaceOrCrash(ControllerAgentConnectors_, std::move(descriptor), controllerAgentConnector);

    return controllerAgentConnector;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecNode
