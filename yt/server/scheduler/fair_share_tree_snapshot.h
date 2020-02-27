#pragma once

#include <yt/ytlib/scheduler/job_resources.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

struct TSchedulerElementStateSnapshot
{
    TJobResources ResourceDemand;
    TJobResources MinShareResources;
};

////////////////////////////////////////////////////////////////////////////////

//! Thread affinity: any
struct IFairShareTreeSnapshot
    : public TIntrinsicRefCounted
{
    virtual TFuture<void> ScheduleJobs(const ISchedulingContextPtr& schedulingContext) = 0;
    virtual void PreemptJobsGracefully(const ISchedulingContextPtr& schedulingContext) = 0;
    virtual void ProcessUpdatedJob(TOperationId operationId, TJobId jobId, const TJobResources& delta) = 0;
    virtual void ProcessFinishedJob(TOperationId operationId, TJobId jobId) = 0;
    virtual bool HasOperation(TOperationId operationId) const = 0;
    virtual bool IsOperationDisabled(TOperationId operationId) const = 0;
    virtual void ApplyJobMetricsDelta(TOperationId operationId, const TJobMetrics& jobMetricsDelta) = 0;
    virtual void ProfileFairShare() const = 0;
    virtual const TSchedulingTagFilter& GetNodesFilter() const = 0;
    virtual TJobResources GetTotalResourceLimits() const = 0;
    virtual std::optional<TSchedulerElementStateSnapshot> GetMaybeStateSnapshotForPool(const TString& poolId) const = 0;
};

DEFINE_REFCOUNTED_TYPE(IFairShareTreeSnapshot);

////////////////////////////////////////////////////////////////////////////////

//! This interface must be thread-safe.
class IFairShareTreeHost
    : public TRefCounted
{
public:
    virtual TResourceTree* GetResourceTree() = 0;

    virtual NProfiling::TAggregateGauge& GetProfilingCounter(const TString& name) = 0;
};

DEFINE_REFCOUNTED_TYPE(IFairShareTreeHost)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
