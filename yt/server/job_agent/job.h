#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/job_tracker_client/job.pb.h>

#include <yt/ytlib/node_tracker_client/node.pb.h>

#include <yt/core/actions/signal.h>

#include <yt/core/misc/error.h>

namespace NYT {
namespace NJobAgent {

////////////////////////////////////////////////////////////////////////////////

struct IJob
    : public virtual TRefCounted
{
    DECLARE_INTERFACE_SIGNAL(void(
        const NNodeTrackerClient::NProto::TNodeResources& resourceDelta),
        ResourcesUpdated);

    virtual void Start() = 0;

    virtual void Abort(const TError& error) = 0;

    virtual const TJobId& GetId() const = 0;
    virtual const TOperationId& GetOperationId() const = 0;

    virtual EJobType GetType() const = 0;

    virtual const NJobTrackerClient::NProto::TJobSpec& GetSpec() const = 0;

    virtual EJobState GetState() const = 0;

    virtual EJobPhase GetPhase() const = 0;

    virtual NNodeTrackerClient::NProto::TNodeResources GetResourceUsage() const = 0;

    /*!
     *  New usage should not exceed the previous one.
     */
    virtual void SetResourceUsage(const NNodeTrackerClient::NProto::TNodeResources& newUsage) = 0;

    virtual NJobTrackerClient::NProto::TJobResult GetResult() const = 0;
    virtual void SetResult(const NJobTrackerClient::NProto::TJobResult& result) = 0;

    virtual double GetProgress() const = 0;
    virtual void SetProgress(double value) = 0;

    virtual TNullable<NYson::TYsonString> GetStatistics() const = 0;
    virtual void SetStatistics(const NYson::TYsonString& statistics) = 0;

    virtual TNullable<TDuration> GetPrepareDuration() const = 0;
    virtual TNullable<TDuration> GetExecDuration() const = 0;

    virtual bool ShouldSendStatistics() const = 0;
    
    // The following two methods will be called from the single thread.
    virtual TInstant GetStatisticsLastSendTime() const = 0;
    virtual void ResetStatisticsLastSendTime() = 0;

    virtual std::vector<NChunkClient::TChunkId> DumpInputContexts() const = 0;
    virtual NYson::TYsonString Strace() const = 0;
    virtual void SignalJob(const Stroka& signalName) = 0;
    virtual NYson::TYsonString PollJobShell(const NYson::TYsonString& parameters) = 0;
};

DEFINE_REFCOUNTED_TYPE(IJob)

using TJobFactory = TCallback<IJobPtr(
    const TJobId& jobId,
    const TOperationId& operationId,
    const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
    NJobTrackerClient::NProto::TJobSpec&& jobSpec)>;

////////////////////////////////////////////////////////////////////////////////

void FillJobStatus(NJobTrackerClient::NProto::TJobStatus* jobStatus, IJobPtr job);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobAgent
} // namespace NYT

