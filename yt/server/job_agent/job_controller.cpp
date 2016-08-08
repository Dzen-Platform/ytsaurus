#include "job_controller.h"
#include "private.h"
#include "config.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/server/data_node/master_connector.h>

#include <yt/server/exec_agent/slot_manager.h>

#include <yt/server/misc/memory_usage_tracker.h>

#include <yt/ytlib/node_tracker_client/helpers.h>
#include <yt/ytlib/node_tracker_client/node.pb.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/misc/fs.h>

namespace NYT {
namespace NJobAgent {

using namespace NObjectClient;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NYson;
using namespace NYTree;
using namespace NCellNode;
using namespace NConcurrency;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = JobTrackerServerLogger;
static const auto ProfilingPeriod = TDuration::Seconds(1);

////////////////////////////////////////////////////////////////////////////////

class TJobController::TImpl
    : public TRefCounted
{
public:
    DEFINE_SIGNAL(void(), ResourcesUpdated);

public:
    TImpl(
        TJobControllerConfigPtr config,
        TBootstrap* bootstrap);

    void RegisterFactory(
        EJobType type,
        TJobFactory factory);

    IJobPtr FindJob(const TJobId& jobId) const;
    IJobPtr GetJobOrThrow(const TJobId& jobId) const;
    std::vector<IJobPtr> GetJobs() const;

    TNodeResources GetResourceLimits() const;
    TNodeResources GetResourceUsage(bool includeWaiting = true) const;
    void SetResourceLimitsOverrides(const TNodeResourceLimitsOverrides& resourceLimits);

    void PrepareHeartbeatRequest(
        TCellTag cellTag,
        EObjectType jobObjectType,
        TReqHeartbeat* request);

    void ProcessHeartbeatResponse(TRspHeartbeat* response);

    NYTree::IYPathServicePtr GetOrchidService();

private:
    const TJobControllerConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;

    yhash_map<EJobType, TJobFactory> Factories_;
    yhash_map<TJobId, IJobPtr> Jobs_;

    bool StartScheduled_ = false;

    IThroughputThrottlerPtr StatisticsThrottler_;

    TNodeResourceLimitsOverrides ResourceLimitsOverrides_;

    TProfiler ResourceLimitsProfiler_;
    TProfiler ResourceUsageProfiler_;
    TEnumIndexedVector<TTagId, EJobOrigin> JobOriginToTag_;

    TPeriodicExecutorPtr ProfilingExecutor_;

    //! Starts a new job.
    IJobPtr CreateJob(
        const TJobId& jobId,
        const TOperationId& operationId,
        const TNodeResources& resourceLimits,
        TJobSpec&& jobSpec);

    //! Stops a job.
    /*!
     *  If the job is running, aborts it.
     */
    void AbortJob(IJobPtr job);

    //! Removes the job from the map.
    /*!
     *  It is illegal to call #Remove before the job is stopped.
     */
    void RemoveJob(IJobPtr job);

    TJobFactory GetFactory(EJobType type) const;

    void ScheduleStart();

    void OnResourcesUpdated(
        TWeakPtr<IJob> job,
        const TNodeResources& resourceDelta);

    void StartWaitingJobs();

    //! Compares new usage with resource limits. Detects resource overdraft.
    bool CheckResourceUsageDelta(const TNodeResources& delta);

    //! Returns |true| if a job with given #jobResources can be started.
    //! Takes special care with ReplicationDataSize and RepairDataSize enabling
    // an arbitrary large overdraft for the
    //! first job.
    bool HasEnoughResources(
        const TNodeResources& jobResources,
        const TNodeResources& usedResources);

    void BuildOrchid(IYsonConsumer* consumer) const;

    void OnProfiling();

    TEnumIndexedVector<int, EJobOrigin> GetJobCountByOrigin() const;
};

////////////////////////////////////////////////////////////////////////////////

TJobController::TImpl::TImpl(
    TJobControllerConfigPtr config,
    TBootstrap* bootstrap)
    : Config_(config)
    , Bootstrap_(bootstrap)
    , StatisticsThrottler_(CreateReconfigurableThroughputThrottler(Config_->StatisticsThrottler))
    , ResourceLimitsProfiler_(Profiler.GetPathPrefix() + "/resource_limits")
    , ResourceUsageProfiler_(Profiler.GetPathPrefix() + "/resource_usage")
{
    YCHECK(config);
    YCHECK(bootstrap);

    for (auto origin : TEnumTraits<EJobOrigin>::GetDomainValues()) {
        JobOriginToTag_[origin] = TProfileManager::Get()->RegisterTag("origin", Format("%lv", origin));
    }

    ProfilingExecutor_ = New<TPeriodicExecutor>(
        Bootstrap_->GetControlInvoker(),
        BIND(&TImpl::OnProfiling, MakeWeak(this)),
        ProfilingPeriod);
    ProfilingExecutor_->Start();
}

void TJobController::TImpl::RegisterFactory(EJobType type, TJobFactory factory)
{
    YCHECK(Factories_.insert(std::make_pair(type, factory)).second);
}

TJobFactory TJobController::TImpl::GetFactory(EJobType type) const
{
    auto it = Factories_.find(type);
    YCHECK(it != Factories_.end());
    return it->second;
}

IJobPtr TJobController::TImpl::FindJob(const TJobId& jobId) const
{
    auto it = Jobs_.find(jobId);
    return it == Jobs_.end() ? nullptr : it->second;
}

IJobPtr TJobController::TImpl::GetJobOrThrow(const TJobId& jobId) const
{
    auto job = FindJob(jobId);
    if (!job) {
        THROW_ERROR_EXCEPTION("No such job %v", jobId);
    }
    return job;
}

std::vector<IJobPtr> TJobController::TImpl::GetJobs() const
{
    std::vector<IJobPtr> result;
    for (const auto& pair : Jobs_) {
        result.push_back(pair.second);
    }
    return result;
}

TNodeResources TJobController::TImpl::GetResourceLimits() const
{
    TNodeResources result;

    result.set_user_slots(Bootstrap_->GetExecSlotManager()->GetSlotCount());

    #define XX(name, Name) \
        result.set_##name(ResourceLimitsOverrides_.has_##name() \
            ? ResourceLimitsOverrides_.name() \
            : Config_->ResourceLimits->Name);
    ITERATE_NODE_RESOURCE_LIMITS_OVERRIDES(XX)
    #undef XX

    const auto* tracker = Bootstrap_->GetMemoryUsageTracker();
    result.set_memory(std::min(
        tracker->GetLimit(EMemoryCategory::Jobs),
        // NB: The sum of per-category limits can be greater than the total memory limit.
        // Therefore we need bound memory limit by actually available memory.
        tracker->GetUsed(EMemoryCategory::Jobs) + tracker->GetTotalFree()));

    return result;
}

TNodeResources TJobController::TImpl::GetResourceUsage(bool includeWaiting) const
{
    auto result = ZeroNodeResources();
    for (const auto& pair : Jobs_) {
        const auto& job = pair.second;
        if (includeWaiting || job->GetState() != EJobState::Waiting) {
            result += job->GetResourceUsage();
        }
    }
    return result;
}

void TJobController::TImpl::SetResourceLimitsOverrides(const TNodeResourceLimitsOverrides& resourceLimits)
{
    ResourceLimitsOverrides_ = resourceLimits;
}

void TJobController::TImpl::StartWaitingJobs()
{
    auto* tracker = Bootstrap_->GetMemoryUsageTracker();

    bool resourcesUpdated = false;

    {
        auto usedResources = GetResourceUsage(false);
        auto memoryToRelease = tracker->GetUsed(EMemoryCategory::Jobs) - usedResources.memory();
        if (memoryToRelease > 0) {
            tracker->Release(EMemoryCategory::Jobs, memoryToRelease);
            resourcesUpdated = true;
        }
    }

    for (const auto& pair : Jobs_) {
        auto job = pair.second;
        if (job->GetState() != EJobState::Waiting)
            continue;

        auto jobResources = job->GetResourceUsage();
        auto usedResources = GetResourceUsage(false);
        if (!HasEnoughResources(jobResources, usedResources)) {
            LOG_DEBUG("Not enough resources to start waiting job (JobId: %v, JobResources: %v, UsedResources: %v)",
                job->GetId(),
                FormatResources(jobResources),
                FormatResources(usedResources));
            continue;
        }

        if (jobResources.memory() > 0) {
            auto error = tracker->TryAcquire(EMemoryCategory::Jobs, jobResources.memory());
            if (!error.IsOK()) {
                LOG_DEBUG(error, "Not enough memory to start waiting job (JobId: %v)",
                    job->GetId());
                continue;
            }
        }

        LOG_INFO("Starting job (JobId: %v)", job->GetId());

        job->SubscribeResourcesUpdated(
            BIND(&TImpl::OnResourcesUpdated, MakeWeak(this), MakeWeak(job))
                .Via(Bootstrap_->GetControlInvoker()));

        job->Start();

        resourcesUpdated = true;
    }

    if (resourcesUpdated) {
        ResourcesUpdated_.Fire();
    }

    StartScheduled_ = false;
}

IJobPtr TJobController::TImpl::CreateJob(
    const TJobId& jobId,
    const TOperationId& operationId,
    const TNodeResources& resourceLimits,
    TJobSpec&& jobSpec)
{
    auto type = EJobType(jobSpec.type());

    auto factory = GetFactory(type);

    auto job = factory.Run(
        jobId,
        operationId,
        resourceLimits,
        std::move(jobSpec));

    LOG_INFO("Job created (JobId: %v, OperationId: %v, JobType: %v)",
        jobId,
        operationId,
        type);

    YCHECK(Jobs_.insert(std::make_pair(jobId, job)).second);
    ScheduleStart();

    return job;
}

void TJobController::TImpl::ScheduleStart()
{
    if (!StartScheduled_) {
        Bootstrap_->GetControlInvoker()->Invoke(BIND(
            &TImpl::StartWaitingJobs,
            MakeWeak(this)));
        StartScheduled_ = true;
    }
}

void TJobController::TImpl::AbortJob(IJobPtr job)
{
    LOG_INFO("Job abort requested (JobId: %v)",
        job->GetId());

    job->Abort(TError(NExecAgent::EErrorCode::AbortByScheduler, "Job aborted by scheduler"));
}

void TJobController::TImpl::RemoveJob(IJobPtr job)
{
    LOG_INFO("Job removed (JobId: %v)", job->GetId());

    YCHECK(job->GetPhase() > EJobPhase::Cleanup);
    YCHECK(job->GetResourceUsage() == ZeroNodeResources());
    YCHECK(Jobs_.erase(job->GetId()) == 1);
}

void TJobController::TImpl::OnResourcesUpdated(TWeakPtr<IJob> job, const TNodeResources& resourceDelta)
{
    if (!CheckResourceUsageDelta(resourceDelta)) {
        auto job_ = job.Lock();
        if (job_) {
            job_->Abort(TError(
                NExecAgent::EErrorCode::ResourceOverdraft,
                "Failed to increase resource usage")
                << TErrorAttribute("resource_delta", FormatResources(resourceDelta)));
        }
        return;
    }

    if (!Dominates(resourceDelta, ZeroNodeResources())) {
        // Some resources decreased.
        ScheduleStart();
    }
}

bool TJobController::TImpl::CheckResourceUsageDelta(const TNodeResources& delta)
{
    // Nonincreasing resources cannot lead to overdraft.
    auto nodeLimits = GetResourceLimits();
    auto newUsage = GetResourceUsage(false) + delta;

    #define XX(name, Name) if (delta.name() > 0 && nodeLimits.name() < newUsage.name()) { return false; }
    ITERATE_NODE_RESOURCES(XX)
    #undef XX

    if (delta.memory() > 0) {
        auto* tracker = Bootstrap_->GetMemoryUsageTracker();
        auto error = tracker->TryAcquire(EMemoryCategory::Jobs, delta.memory());
        if (!error.IsOK()) {
            return false;
        }
    }

    return true;
}

bool TJobController::TImpl::HasEnoughResources(
    const TNodeResources& jobResources,
    const TNodeResources& usedResources)
{
    auto totalResources = GetResourceLimits();
    auto spareResources = MakeNonnegative(totalResources - usedResources);
    if (usedResources.replication_slots() == 0) {
        spareResources.set_replication_data_size(InfiniteNodeResources().replication_data_size());
    }
    if (usedResources.repair_slots() == 0) {
        spareResources.set_repair_data_size(InfiniteNodeResources().repair_data_size());
    }
    return Dominates(spareResources, jobResources);
}

void TJobController::TImpl::PrepareHeartbeatRequest(
    TCellTag cellTag,
    EObjectType jobObjectType,
    TReqHeartbeat* request)
{
    auto masterConnector = Bootstrap_->GetMasterConnector();
    request->set_node_id(masterConnector->GetNodeId());
    ToProto(request->mutable_node_descriptor(), masterConnector->GetLocalDescriptor());
    *request->mutable_resource_limits() = GetResourceLimits();
    *request->mutable_resource_usage() = GetResourceUsage();

    // A container for all scheduler jobs that are candidate to send statistics. This set contains
    // only the runnning jobs since all completed/aborted/failed jobs always send their statistics.
    std::vector<std::pair<IJobPtr, TJobStatus*>> runningJobs;

    i64 completedJobsStatisticsSize = 0;

    for (const auto& pair : Jobs_) {
        const auto& jobId = pair.first;
        const auto& job = pair.second;
        if (CellTagFromId(jobId) != cellTag)
            continue;
        if (TypeFromId(jobId) != jobObjectType)
            continue;

        auto* jobStatus = request->add_jobs();
        FillJobStatus(jobStatus, job);
        switch (job->GetState()) {
            case EJobState::Running:
                *jobStatus->mutable_resource_usage() = job->GetResourceUsage();
                if (jobObjectType == EObjectType::SchedulerJob) {
                    runningJobs.emplace_back(job, jobStatus);
                }
                break;

            case EJobState::Completed:
            case EJobState::Aborted:
            case EJobState::Failed:
                *jobStatus->mutable_result() = job->GetResult();
                if (auto statistics = job->GetStatistics()) {
                    completedJobsStatisticsSize += statistics->Data().size();
                    job->ResetStatisticsLastSendTime();
                    jobStatus->set_statistics((*statistics).Data());
                }
                break;

            default:
                break;
        }
    }

    if (jobObjectType == EObjectType::SchedulerJob) {
        std::sort(
            runningJobs.begin(),
            runningJobs.end(),
            [] (const auto& lhs, const auto& rhs) {
                return lhs.first->GetStatisticsLastSendTime() < rhs.first->GetStatisticsLastSendTime();
            });

        i64 runningJobsStatisticsSize = 0;

        for (const auto& pair : runningJobs) {
            const auto& job = pair.first;
            auto* jobStatus = pair.second;
            auto statistics = job->GetStatistics();
            if (statistics && StatisticsThrottler_->TryAcquire(statistics->Data().size())) {
                runningJobsStatisticsSize += statistics->Data().size();
                job->ResetStatisticsLastSendTime();
                jobStatus->set_statistics((*statistics).Data());
            }
        }

        LOG_DEBUG("Total size of statistics to send is %v bytes (RunningJobsStatisticsSize: %v, CompletedJobsStatisticsSize: %v)",
            runningJobsStatisticsSize + completedJobsStatisticsSize,
            runningJobsStatisticsSize,
            completedJobsStatisticsSize);
    }
}

void TJobController::TImpl::ProcessHeartbeatResponse(TRspHeartbeat* response)
{
    for (const auto& protoJobId : response->jobs_to_remove()) {
        auto jobId = FromProto<TJobId>(protoJobId);
        auto job = FindJob(jobId);
        if (job) {
            RemoveJob(job);
        } else {
            LOG_WARNING("Requested to remove a non-existing job (JobId: %v)",
                jobId);
        }
    }

    for (const auto& protoJobId : response->jobs_to_abort()) {
        auto jobId = FromProto<TJobId>(protoJobId);
        auto job = FindJob(jobId);
        if (job) {
            AbortJob(job);
        } else {
            LOG_WARNING("Requested to abort a non-existing job (JobId: %v)",
                jobId);
        }
    }

    for (auto& info : *response->mutable_jobs_to_start()) {
        auto jobId = FromProto<TJobId>(info.job_id());
        auto operationId = FromProto<TJobId>(info.operation_id());
        const auto& resourceLimits = info.resource_limits();
        auto& spec = *info.mutable_spec();
        CreateJob(jobId, operationId, resourceLimits, std::move(spec));
    }
}

TEnumIndexedVector<int, EJobOrigin> TJobController::TImpl::GetJobCountByOrigin() const
{
    auto jobCount = TEnumIndexedVector<int, EJobOrigin>();
    for (const auto& pair : Jobs_) {
        switch (TypeFromId(pair.first)) {
            case EObjectType::MasterJob:
                ++jobCount[EJobOrigin::Master];
                break;
            case EObjectType::SchedulerJob:
                ++jobCount[EJobOrigin::Scheduler];
                break;
            default:
                YUNREACHABLE();
        }
    }
    return jobCount;
}

void TJobController::TImpl::BuildOrchid(IYsonConsumer* consumer) const
{
    auto jobCount = GetJobCountByOrigin();
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("resource_limits").Value(GetResourceLimits())
            .Item("resource_usage").Value(GetResourceUsage())
            .Item("active_job_count").DoMapFor(
                TEnumTraits<EJobOrigin>::GetDomainValues(),
                [&] (TFluentMap fluent, const EJobOrigin origin) {
                    fluent.Item(Format("%lv", origin)).Value(jobCount[origin]);
                })
        .EndMap();
}

IYPathServicePtr TJobController::TImpl::GetOrchidService()
{
    auto producer = BIND(&TImpl::BuildOrchid, MakeStrong(this));
    return IYPathService::FromProducer(producer);
}

void TJobController::TImpl::OnProfiling()
{
    auto jobCount = GetJobCountByOrigin();
    for (auto origin : TEnumTraits<EJobOrigin>::GetDomainValues()) {
        Profiler.Enqueue("/active_job_count", jobCount[origin], EMetricType::Gauge, {JobOriginToTag_[origin]});
    }
    ProfileResources(ResourceUsageProfiler_, GetResourceUsage(false /* includeWaiting */));
    ProfileResources(ResourceLimitsProfiler_, GetResourceLimits());
}

////////////////////////////////////////////////////////////////////////////////

TJobController::TJobController(
    TJobControllerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(
        config,
        bootstrap
    ))
{ }

void TJobController::RegisterFactory(
    EJobType type,
    TJobFactory factory)
{
    Impl_->RegisterFactory(type, factory);
}

IJobPtr TJobController::FindJob(const TJobId& jobId) const
{
    return Impl_->FindJob(jobId);
}

IJobPtr TJobController::GetJobOrThrow(const TJobId& jobId) const
{
    return Impl_->GetJobOrThrow(jobId);
}

std::vector<IJobPtr> TJobController::GetJobs() const
{
    return Impl_->GetJobs();
}

TNodeResources TJobController::GetResourceLimits() const
{
    return Impl_->GetResourceLimits();
}

TNodeResources TJobController::GetResourceUsage(bool includeWaiting) const
{
    return Impl_->GetResourceUsage(includeWaiting);
}

void TJobController::SetResourceLimitsOverrides(const TNodeResourceLimitsOverrides& resourceLimits)
{
    Impl_->SetResourceLimitsOverrides(resourceLimits);
}

void TJobController::PrepareHeartbeatRequest(
    TCellTag cellTag,
    EObjectType jobObjectType,
    TReqHeartbeat* request)
{
    Impl_->PrepareHeartbeatRequest(cellTag, jobObjectType, request);
}

void TJobController::ProcessHeartbeatResponse(TRspHeartbeat* response)
{
    Impl_->ProcessHeartbeatResponse(response);
}

IYPathServicePtr TJobController::GetOrchidService()
{
    return Impl_->GetOrchidService();
}

DELEGATE_SIGNAL(TJobController, void(), ResourcesUpdated, *Impl_)

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobAgent
} // namespace NYT
