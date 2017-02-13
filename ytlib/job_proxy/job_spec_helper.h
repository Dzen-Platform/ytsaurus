#pragma once

#include "public.h"

#include <yt/ytlib/job_tracker_client/job.pb.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/ytlib/scheduler/config.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

namespace NScheduler {
namespace NProto {

class TSchedulerJobSpecExt;

} // namespace NProto
} // namespace NScheduler

////////////////////////////////////////////////////////////////////////////////

namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

// Wrapper around TJobSpec, that provides convenient accessors for frequently used functions.
struct IJobSpecHelper
    : public virtual TRefCounted
{
    virtual NJobTrackerClient::EJobType GetJobType() const = 0;
    virtual const NJobTrackerClient::NProto::TJobSpec& GetJobSpec() const = 0;
    virtual NScheduler::TJobIOConfigPtr GetJobIOConfig() const = 0;
    virtual NNodeTrackerClient::TNodeDirectoryPtr GetInputNodeDirectory() const = 0;
    virtual const NScheduler::NProto::TSchedulerJobSpecExt& GetSchedulerJobSpecExt() const = 0;
    virtual int GetKeySwitchColumnCount() const = 0;
    virtual bool IsReaderInterruptionSupported() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IJobSpecHelper);

////////////////////////////////////////////////////////////////////////////////

IJobSpecHelperPtr CreateJobSpecHelper(const NJobTrackerClient::NProto::TJobSpec& jobSpec);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
