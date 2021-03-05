#pragma once

#include <yt/yt/ytlib/scheduler/public.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

//! This class contains all controller-related profiling. It is effectively singletone.
//! TODO(max42): move this to controller agent bootstrap when it is finally
//! separate from scheduler.
class TControllerAgentCounterManager
{
public:
    TControllerAgentCounterManager();

    void IncrementAssertionsFailed(NScheduler::EOperationType operationType);

    static TControllerAgentCounterManager* Get();

private:
    TEnumIndexedVector<NScheduler::EOperationType, NProfiling::TCounter> AssertionsFailed_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
