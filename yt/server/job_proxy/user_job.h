#pragma once

#include "public.h"
#include "job.h"

#include <yt/server/job_agent/public.h>

#include <yt/ytlib/scheduler/proto/job.pb.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

IJobPtr CreateUserJob(IJobHostPtr host,
    const NScheduler::NProto::TUserJobSpec& userJobSpec,
    const NJobAgent::TJobId& jobId,
    std::unique_ptr<IUserJobIO> userJobIO);

const TString& GetCGroupUserJobBase();
const TString& GetCGroupUserJobPrefix();

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
