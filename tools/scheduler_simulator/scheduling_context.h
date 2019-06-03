#pragma once

#include "scheduler_strategy_host.h"

#include <yt/server/scheduler/scheduling_context_detail.h>
#include <yt/server/scheduler/exec_node.h>

namespace NYT::NSchedulerSimulator {

////////////////////////////////////////////////////////////////////////////////

class TSchedulingContext
    : public NScheduler::TSchedulingContextBase
{
public:
    DEFINE_BYVAL_RW_PROPERTY(NProfiling::TCpuInstant, Now);

public:
    TSchedulingContext(
        NScheduler::TSchedulerConfigPtr schedulerConfig,
        NScheduler::TExecNodePtr node,
        const std::vector<NScheduler::TJobPtr>& runningJobs)
        : TSchedulingContextBase(0, schedulerConfig, node, runningJobs)
    { }

    void SetDurationForStartedJob(NScheduler::TJobId jobId, const TDuration& duration)
    {
        Durations_[jobId] = duration;
    }

    const THashMap<NScheduler::TJobId, TDuration>& GetStartedJobsDurations() const
    {
        return Durations_;
    }

private:
    THashMap<NScheduler::TJobId, TDuration> Durations_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSchedulerSimulator
