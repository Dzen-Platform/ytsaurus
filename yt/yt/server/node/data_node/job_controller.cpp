#include "job_controller.h"

#include "bootstrap.h"
#include "job_detail.h"
#include "master_connector.h"
#include "private.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>
#include <yt/yt/server/node/cluster_node/config.h>
#include <yt/yt/server/node/cluster_node/dynamic_config_manager.h>

#include <yt/yt/server/node/exec_node/bootstrap.h>
#include <yt/yt/server/node/exec_node/job_controller.h>

#include <yt/yt/server/lib/controller_agent/helpers.h>

#include <yt/yt/server/lib/job_agent/config.h>

#include <yt/yt/ytlib/job_tracker_client/helpers.h>

#include <yt/yt/ytlib/node_tracker_client/helpers.h>

#include <yt/yt/core/concurrency/periodic_executor.h>

namespace NYT::NDataNode {

using namespace NConcurrency;
using namespace NProfiling;
using namespace NYTree;
using namespace NClusterNode;
using namespace NJobAgent;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient;

using NJobTrackerClient::NProto::TJobSpec;
using NNodeTrackerClient::NProto::TNodeResources;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

class TJobController
    : public IJobController
{
public:
    DEFINE_SIGNAL_OVERRIDE(void(const TMasterJobBasePtr& job), JobFinished);

public:
    TJobController(
        IBootstrapBase* bootstrap)
        : Config_(bootstrap->GetConfig()->ExecNode->JobController)
        , Bootstrap_(bootstrap)
        , Profiler_("/job_controller")
    {
        YT_VERIFY(Config_);
        YT_VERIFY(Bootstrap_);

        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetJobInvoker(), JobThread);

        Profiler_.AddProducer("", ActiveJobCountBuffer_);
    }

    void Initialize() override
    {
        JobResourceManager_ = Bootstrap_->GetJobResourceManager();
        JobResourceManager_->RegisterResourcesConsumer(
            BIND_NO_PROPAGATE(&TJobController::OnResourceReleased, MakeWeak(this))
                .Via(Bootstrap_->GetJobInvoker()),
            EResourcesConsumptionPriority::Primary);

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetJobInvoker(),
            BIND_NO_PROPAGATE(&TJobController::OnProfiling, MakeWeak(this)),
            Config_->ProfilingPeriod);
        ProfilingExecutor_->Start();

        const auto& dynamicConfigManager = Bootstrap_->GetDynamicConfigManager();
        dynamicConfigManager->SubscribeConfigChanged(
            BIND_NO_PROPAGATE(&TJobController::OnDynamicConfigChanged, MakeWeak(this)));
    }

    void RegisterJobFactory(EJobType type, TJobFactory factory) override
    {
        YT_VERIFY(type >= FirstMasterJobType && type <= LastMasterJobType);
        EmplaceOrCrash(JobFactoryMap_, type, factory);
    }

    std::vector<TMasterJobBasePtr> GetJobs() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto guard = ReaderGuard(JobMapLock_);

        std::vector<TMasterJobBasePtr> result;
        result.reserve(JobMap_.size());
        for (const auto& [id, job] : JobMap_) {
            result.push_back(job);

        }
        return result;
    }

    TFuture<void> PrepareHeartbeatRequest(
        TCellTag cellTag,
        const TString& jobTrackerAddress,
        const TReqHeartbeatPtr& request) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return
            BIND(&TJobController::DoPrepareHeartbeatRequest, MakeStrong(this))
                .AsyncVia(Bootstrap_->GetJobInvoker())
                .Run(cellTag, jobTrackerAddress, request);
    }

    TFuture<void> ProcessHeartbeatResponse(
        const TString& jobTrackerAddress,
        const TRspHeartbeatPtr& response) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND(&TJobController::DoProcessHeartbeatResponse, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetJobInvoker())
            .Run(jobTrackerAddress, response);
    }

    TMasterJobBasePtr FindJob(TJobId jobId) const
    {
        VERIFY_THREAD_AFFINITY_ANY();
        
        auto guard = ReaderGuard(JobMapLock_);
        auto it = JobMap_.find(jobId);
        return it == JobMap_.end() ? nullptr : it->second;
    }

    void ScheduleStartJobs() override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (std::exchange(StartJobsScheduled_, true)) {
            return;
        }

        Bootstrap_->GetJobInvoker()->Invoke(BIND(
            &TJobController::StartWaitingJobs,
            MakeWeak(this)));
    }

    void BuildJobsInfo(TFluentAny fluent) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto jobs = GetJobs();

        // TODO(pogorelov): Delete useless fields.
        fluent.DoMapFor(
            jobs,
            [&] (TFluentMap fluent, const TMasterJobBasePtr& job) {
                fluent.Item(ToString(job->GetId()))
                    .BeginMap()
                        .Item("job_state").Value(job->GetState())
                        .Item("job_phase").Value(job->GetPhase())
                        .Item("job_type").Value(job->GetType())
                        .Item("slot_index").Value(job->GetSlotIndex())
                        .Item("job_tracker_address").Value(job->GetJobTrackerAddress())
                        .Item("start_time").Value(job->GetStartTime())
                        .Item("duration").Value(TInstant::Now() - job->GetStartTime())
                        .OptionalItem("statistics", job->GetStatistics())
                        .OptionalItem("operation_id", job->GetOperationId())
                        .Item("resource_usage").Value(job->GetResourceUsage())
                        .Do(std::bind(&TMasterJobBase::BuildOrchid, job, std::placeholders::_1))
                    .EndMap();
            });
    }

    int GetActiveJobCount() const override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        return std::ssize(JobMap_);
    }

private:
    const TIntrusivePtr<const TJobControllerConfig> Config_;
    NClusterNode::IBootstrapBase* const Bootstrap_;

    IJobResourceManagerPtr JobResourceManager_;

    TAtomicObject<TJobControllerDynamicConfigPtr> DynamicConfig_ = New<TJobControllerDynamicConfig>();

    THashMap<EJobType, TJobFactory> JobFactoryMap_;

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, JobMapLock_);
    THashMap<TJobId, TMasterJobBasePtr> JobMap_;

    bool StartJobsScheduled_ = false;

    TPeriodicExecutorPtr ProfilingExecutor_;

    TProfiler Profiler_;
    TBufferedProducerPtr ActiveJobCountBuffer_ = New<TBufferedProducer>();
    THashMap<EJobState, TCounter> JobFinalStateCounters_;

    DECLARE_THREAD_AFFINITY_SLOT(JobThread);

    void OnResourceReleased()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ScheduleStartJobs();
    }

    const TJobFactory& GetJobFactory(EJobType type) const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return GetOrCrash(JobFactoryMap_, type);
    }

    void StartWaitingJobs()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        auto resourceAcquiringProxy = JobResourceManager_->GetResourceAcquiringProxy();

        for (const auto& job : GetJobs()) {
            YT_VERIFY(TypeFromId(job->GetId()) == EObjectType::MasterJob);

            if (job->GetState() != EJobState::Waiting) {
                continue;
            }

            auto jobId = job->GetId();
            YT_LOG_DEBUG("Trying to start job (JobId: %v)", jobId);

            if (!resourceAcquiringProxy.TryAcquireResourcesFor(job->AsResourceHolder())) {
                YT_LOG_DEBUG("Job was not started (JobId: %v)", jobId);
            } else {
                YT_LOG_DEBUG("Job started (JobId: %v)", jobId);
            }
        }

        StartJobsScheduled_ = false;
    }

    TMasterJobBasePtr CreateJob(
        TJobId jobId,
        TOperationId operationId,
        const TString& jobTrackerAddress,
        const TNodeResources& resourceLimits,
        TJobSpec&& jobSpec)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        auto type = CheckedEnumCast<EJobType>(jobSpec.type());
        auto factory = GetJobFactory(type);

        auto job = factory.Run(
            jobId,
            operationId,
            jobTrackerAddress,
            resourceLimits,
            std::move(jobSpec));

        YT_LOG_INFO("Master job created (JobId: %v, JobType: %v, JobTrackerAddress: %v)",
            jobId,
            type,
            jobTrackerAddress);

        auto waitingJobTimeout = Config_->WaitingJobsTimeout;

        RegisterJob(jobId, job, waitingJobTimeout);

        return job;
    }

    void RegisterJob(const TJobId jobId, const TMasterJobBasePtr& job, const TDuration waitingJobTimeout)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        {
            auto guard = WriterGuard(JobMapLock_);
            EmplaceOrCrash(JobMap_, jobId, job);
        }

        job->SubscribeJobFinished(
            BIND_NO_PROPAGATE(&TJobController::OnJobFinished, MakeWeak(this), MakeWeak(job))
                .Via(Bootstrap_->GetJobInvoker()));

        ScheduleStartJobs();
        
        TDelayedExecutor::Submit(
            BIND(&TJobController::OnWaitingJobTimeout, MakeWeak(this), MakeWeak(job), waitingJobTimeout),
            waitingJobTimeout,
            Bootstrap_->GetJobInvoker());
    }

    void OnWaitingJobTimeout(const TWeakPtr<TMasterJobBase>& weakJob, TDuration waitingJobTimeout)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        auto job = weakJob.Lock();
        if (!job) {
            return;
        }

        if (job->GetState() == EJobState::Waiting) {
            job->Abort(TError(NExecNode::EErrorCode::WaitingJobTimeout, "Job waiting has timed out")
                << TErrorAttribute("timeout", waitingJobTimeout));
        }
    }

    void OnJobFinished(const TWeakPtr<TMasterJobBase>& weakJob)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        auto job = weakJob.Lock();
        if (!job) {
            return;
        }

        if (job->IsUrgent()) {
            YT_LOG_DEBUG("Urgent job has finished, scheduling out-of-order job heartbeat (JobId: %v, JobType: %v)",
                job->GetId(),
                job->GetType());
            ScheduleHeartbeat(job);
        }

        if (!job->IsStarted()) {
            return;
        }

        auto* jobFinalStateCounter = GetJobFinalStateCounter(job->GetState());
        jobFinalStateCounter->Increment();

        JobFinished_.Fire(job);
    }

    TCounter* GetJobFinalStateCounter(EJobState state)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        auto it = JobFinalStateCounters_.find(state);
        if (it == JobFinalStateCounters_.end()) {
            auto counter = Profiler_
                .WithTag("state", FormatEnum(state))
                .WithTag("origin", FormatEnum(EJobOrigin::Master))
                .Counter("/job_final_state");

            it = JobFinalStateCounters_.emplace(state, counter).first;
        }

        return &it->second;
    }

    void OnDynamicConfigChanged(
        const TClusterNodeDynamicConfigPtr& /* oldNodeConfig */,
        const TClusterNodeDynamicConfigPtr& newNodeConfig)
    {
        VERIFY_INVOKER_AFFINITY(Bootstrap_->GetControlInvoker());

        auto jobControllerConfig = newNodeConfig->ExecNode->JobController;
        YT_ASSERT(jobControllerConfig);
        DynamicConfig_.Store(jobControllerConfig);

        ProfilingExecutor_->SetPeriod(
            jobControllerConfig->ProfilingPeriod.value_or(
                Config_->ProfilingPeriod));
    }

    void ScheduleHeartbeat(const TMasterJobBasePtr& job)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        const auto* bootstrap = Bootstrap_->GetDataNodeBootstrap();
        const auto& masterConnector = bootstrap->GetMasterConnector();
        masterConnector->ScheduleJobHeartbeat(job->GetJobTrackerAddress());
    }

    void DoPrepareHeartbeatRequest(
        TCellTag cellTag,
        const TString& jobTrackerAddress,
        const TReqHeartbeatPtr& request)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        request->set_node_id(Bootstrap_->GetNodeId());
        ToProto(request->mutable_node_descriptor(), Bootstrap_->GetLocalDescriptor());
        *request->mutable_resource_limits() = JobResourceManager_->GetResourceLimits();
        *request->mutable_resource_usage() = JobResourceManager_->GetResourceUsage(/*includeWaiting*/ true);

        *request->mutable_disk_resources() = JobResourceManager_->GetDiskResources();

        for (const auto& job : GetJobs()) {
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

    void DoProcessHeartbeatResponse(
        const TString& jobTrackerAddress,
        const TRspHeartbeatPtr& response)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        for (const auto& protoJobToRemove : response->jobs_to_remove()) {
            auto jobToRemove = FromProto<TJobToRelease>(protoJobToRemove);
            auto jobId = jobToRemove.JobId;
            YT_VERIFY(jobToRemove.ReleaseFlags.IsTrivial());

            if (auto job = FindJob(jobId)) {
                RemoveJob(job);
            } else {
                YT_LOG_WARNING("Requested to remove a non-existent job (JobId: %v)",
                    jobId);
            }
        }

        for (const auto& protoJobToAbort : response->jobs_to_abort()) {
            auto jobToAbort = FromProto<TJobToAbort>(protoJobToAbort);
            YT_VERIFY(!jobToAbort.PreemptionReason);

            if (auto job = FindJob(jobToAbort.JobId)) {
                AbortJob(job, std::move(jobToAbort));
            } else {
                YT_LOG_WARNING("Requested to abort a non-existent job (JobId: %v, AbortReason: %v)",
                    jobToAbort.JobId,
                    jobToAbort.AbortReason);
            }
        }

        YT_VERIFY(std::ssize(response->Attachments()) == response->jobs_to_start_size());
        int attachmentIndex = 0;
        for (const auto& startInfo : response->jobs_to_start()) {
            auto operationId = FromProto<TOperationId>(startInfo.operation_id());
            auto jobId = FromProto<TJobId>(startInfo.job_id());
            YT_LOG_DEBUG("Job spec received (JobId: %v, JobTrackerAddress: %v)",
                jobId,
                jobTrackerAddress);

            const auto& attachment = response->Attachments()[attachmentIndex];

            TJobSpec spec;
            DeserializeProtoWithEnvelope(&spec, attachment);

            const auto& resourceLimits = startInfo.resource_limits();

            CreateJob(
                jobId,
                operationId,
                jobTrackerAddress,
                resourceLimits,
                std::move(spec));

            ++attachmentIndex;
        }
    }

    void AbortJob(const TMasterJobBasePtr& job, TJobToAbort&& abortAttributes)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_LOG_INFO("Aborting job (JobId: %v, AbortReason: %v)",
            job->GetId(),
            abortAttributes.AbortReason);

        TError error("Job aborted by master request");
        if (abortAttributes.AbortReason) {
            error = error << TErrorAttribute("abort_reason", *abortAttributes.AbortReason);
        }

        job->Abort(error);
    }

    void RemoveJob(const TMasterJobBasePtr& job)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_VERIFY(job->GetPhase() >= EJobPhase::Cleanup);
        YT_VERIFY(job->GetResourceUsage() == ZeroNodeResources());

        auto jobId = job->GetId();

        {
            auto guard = WriterGuard(JobMapLock_);
            EraseOrCrash(JobMap_, jobId);
        }

        YT_LOG_INFO("Job removed (JobId: %v)", job->GetId());
    }

    void OnProfiling()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ActiveJobCountBuffer_->Update([this] (ISensorWriter* writer) {
            TWithTagGuard tagGuard(writer, "origin", FormatEnum(EJobOrigin::Master));
            writer->AddGauge("/active_job_count", GetJobs().size());
        });
    }
};

////////////////////////////////////////////////////////////////////////////////

IJobControllerPtr CreateJobController(NClusterNode::IBootstrapBase* bootstrap)
{
    return New<TJobController>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
