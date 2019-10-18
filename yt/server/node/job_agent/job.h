#pragma once

#include "statistics_reporter.h"
#include "gpu_manager.h"

#include <yt/server/lib/job_agent/job_statistics.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/job_tracker_client/proto/job.pb.h>

#include <yt/ytlib/job_prober_client/public.h>

#include <yt/client/node_tracker_client/proto/node.pb.h>

#include <yt/core/actions/signal.h>

#include <yt/core/misc/error.h>

#include <yt/core/misc/optional.h>

namespace NYT::NJobAgent {

////////////////////////////////////////////////////////////////////////////////

/*
 * \note Thread affinity: Control (unless noted otherwise)
 */
struct IJob
    : public virtual TRefCounted
{
    DECLARE_INTERFACE_SIGNAL(void(
        const NNodeTrackerClient::NProto::TNodeResources& resourceDelta),
        ResourcesUpdated);

    DECLARE_INTERFACE_SIGNAL(void(), PortsReleased);

    DECLARE_INTERFACE_SIGNAL(void(), JobFinished);

    virtual void Start() = 0;

    virtual void Abort(const TError& error) = 0;
    virtual void Fail() = 0;

    virtual TJobId GetId() const = 0;
    virtual TOperationId GetOperationId() const = 0;

    virtual EJobType GetType() const = 0;

    virtual const NJobTrackerClient::NProto::TJobSpec& GetSpec() const = 0;

    virtual int GetPortCount() const = 0;

    virtual EJobState GetState() const = 0;

    virtual EJobPhase GetPhase() const = 0;

    virtual NNodeTrackerClient::NProto::TNodeResources GetResourceUsage() const = 0;
    virtual std::vector<int> GetPorts() const = 0;
    virtual void SetPorts(const std::vector<int>& ports) = 0;

    virtual void SetResourceUsage(const NNodeTrackerClient::NProto::TNodeResources& newUsage) = 0;

    virtual NJobTrackerClient::NProto::TJobResult GetResult() const = 0;
    virtual void SetResult(const NJobTrackerClient::NProto::TJobResult& result) = 0;

    virtual double GetProgress() const = 0;
    virtual void SetProgress(double value) = 0;

    virtual ui64 GetStderrSize() const = 0;
    virtual void SetStderrSize(ui64 value) = 0;

    virtual void SetStderr(const TString& value) = 0;
    virtual void SetFailContext(const TString& value) = 0;
    virtual void SetProfile(const TJobProfile& value) = 0;
    virtual void SetCoreInfos(NCoreDump::TCoreInfos value) = 0;

    virtual NYson::TYsonString GetStatistics() const = 0;
    virtual void SetStatistics(const NYson::TYsonString& statistics) = 0;

    virtual void OnJobPrepared() = 0;

    virtual TInstant GetStartTime() const = 0;
    virtual std::optional<TDuration> GetPrepareDuration() const = 0;
    virtual std::optional<TDuration> GetDownloadDuration() const = 0;
    virtual std::optional<TDuration> GetPrepareRootFSDuration() const = 0;
    virtual std::optional<TDuration> GetExecDuration() const = 0;

    virtual TInstant GetStatisticsLastSendTime() const = 0;
    virtual void ResetStatisticsLastSendTime() = 0;

    virtual std::vector<NChunkClient::TChunkId> DumpInputContext() = 0;
    virtual TString GetStderr() = 0;
    virtual std::optional<TString> GetFailContext() = 0;
    virtual NYson::TYsonString StraceJob() = 0;
    virtual void SignalJob(const TString& signalName) = 0;

    /*
     * \note Thread affinity: any
     */
    virtual NYson::TYsonString PollJobShell(const NYson::TYsonString& parameters) = 0;

    virtual bool GetStored() const = 0;
    virtual void SetStored(bool value) = 0;

    virtual void ReportStatistics(TJobStatistics&& statistics) = 0;
    virtual void ReportSpec() = 0;
    virtual void ReportStderr() = 0;
    virtual void ReportFailContext() = 0;
    virtual void ReportProfile() = 0;

    virtual void Interrupt() = 0;
};

DEFINE_REFCOUNTED_TYPE(IJob)

using TJobFactory = TCallback<IJobPtr(
    TJobId jobId,
    TOperationId operationId,
    const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
    NJobTrackerClient::NProto::TJobSpec&& jobSpec)>;

////////////////////////////////////////////////////////////////////////////////

void FillJobStatus(NJobTrackerClient::NProto::TJobStatus* jobStatus, IJobPtr job);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent
