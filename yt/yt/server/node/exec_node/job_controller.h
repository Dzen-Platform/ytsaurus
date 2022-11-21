#pragma once

#include "job.h"

#include <yt/yt/server/job_proxy/public.h>

#include <yt/yt/server/node/cluster_node/public.h>

#include <yt/yt/server/lib/scheduler/proto/allocation_tracker_service.pb.h>

#include <yt/yt/ytlib/job_tracker_client/proto/job_tracker_service.pb.h>

#include <yt/yt/library/program/build_attributes.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NExecNode {

////////////////////////////////////////////////////////////////////////////////

//! Controls all jobs scheduled to run at this node.
/*!
 *   Maintains a map of jobs, allows new jobs to be started and existing jobs to be stopped.
 *   New jobs are constructed by means of per-type factories registered via #RegisterFactory.
 *
 *   \note Thread affinity: any (unless noted otherwise)
 */
class IJobController
    : public TRefCounted
{
public:
    virtual void Initialize() = 0;

    //! Registers a factory for a given job type.
    virtual void RegisterJobFactory(
        EJobType type,
        TJobFactory factory) = 0;
    
    virtual void ScheduleStartJobs() = 0;
    
    //! Finds the job by its id, returns |nullptr| if no job is found.
    /*
     * \note Thread affinity: any
     */
    virtual TJobPtr FindJob(TJobId jobId) const = 0;

    //! Finds the job by its id, throws if no job is found.
    virtual TJobPtr GetJobOrThrow(TJobId jobId) const = 0;

    //! Returns the list of all currently known jobs.
    virtual std::vector<TJobPtr> GetJobs() const = 0;

    //! Finds the job that is held after it has been removed.
    virtual TJobPtr FindRecentlyRemovedJob(TJobId jobId) const = 0;

    //! Checks dynamic config to see if job proxy profiling is disabled.
    virtual bool IsJobProxyProfilingDisabled() const = 0;

    //! Returns dynamic config of job proxy.
    virtual NJobProxy::TJobProxyDynamicConfigPtr GetJobProxyDynamicConfig() const = 0;

    //! Set value of flag disabling all scheduler jobs.
    virtual void SetDisableSchedulerJobs(bool value) = 0;

    virtual bool AreSchedulerJobsDisabled() const noexcept = 0;

    using TRspHeartbeat = NRpc::TTypedClientResponse<
        NScheduler::NProto::NNode::TRspHeartbeat>;
    using TRspOldHeartbeat = NRpc::TTypedClientResponse<
         NJobTrackerClient::NProto::TRspHeartbeat>;
    using TReqHeartbeat = NRpc::TTypedClientRequest<
        NScheduler::NProto::NNode::TReqHeartbeat,
        TRspHeartbeat>;
    using TReqOldHeartbeat = NRpc::TTypedClientRequest<
        NJobTrackerClient::NProto::TReqHeartbeat,
        TRspOldHeartbeat>;
    using TRspHeartbeatPtr = TIntrusivePtr<TRspHeartbeat>;
    using TReqHeartbeatPtr = TIntrusivePtr<TReqHeartbeat>;

    using TRspOldHeartbeatPtr = TIntrusivePtr<TRspOldHeartbeat>;
    using TReqOldHeartbeatPtr = TIntrusivePtr<TReqOldHeartbeat>;

    //! Prepares a heartbeat request.
    virtual TFuture<void> PrepareHeartbeatRequest(
        const TReqHeartbeatPtr& request) = 0;

    //! Handles heartbeat response, i.e. starts new jobs, aborts and removes old ones etc.
    virtual TFuture<void> ProcessHeartbeatResponse(
        const TRspHeartbeatPtr& response) = 0;
    
    //! Prepares a heartbeat request.
    virtual TFuture<void> PrepareHeartbeatRequest(
        const TReqOldHeartbeatPtr& request) = 0;

    //! Handles heartbeat response, i.e. starts new jobs, aborts and removes old ones etc.
    virtual TFuture<void> ProcessHeartbeatResponse(
        const TRspOldHeartbeatPtr& response) = 0;

    virtual TBuildInfoPtr GetBuildInfo() const = 0;

    virtual void BuildJobProxyBuildInfo(NYTree::TFluentAny fluent) const = 0;
    virtual void BuildJobsInfo(NYTree::TFluentAny fluent) const = 0;

    virtual int GetActiveJobCount() const = 0;

    DECLARE_INTERFACE_SIGNAL(void(const TJobPtr&), JobFinished);
    DECLARE_INTERFACE_SIGNAL(void(const TError& error), JobProxyBuildInfoUpdated);
};

DEFINE_REFCOUNTED_TYPE(IJobController)

////////////////////////////////////////////////////////////////////////////////

IJobControllerPtr CreateJobController(NClusterNode::IBootstrapBase* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecNode
