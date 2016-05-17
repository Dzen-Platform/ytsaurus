#pragma once

#include "timing.h"
#include "profiler.h"

namespace NYT {
namespace NProfiling {

////////////////////////////////////////////////////////////////////////////////

//! Continuously tracks the wall time passes since the instance has been created.
class TScopedTimer
{
public:
    TScopedTimer()
        : StartTime_(GetCpuInstant())
    { }

    TInstant GetStart() const
    {
        return CpuInstantToInstant(StartTime_);
    }

    TDuration GetElapsed() const
    {
        return CpuDurationToDuration(GetCpuInstant() - StartTime_);
    }

    void Restart()
    {
        StartTime_ = GetCpuInstant();
    }

private:
    TCpuInstant StartTime_;

};

class TAggregatingTimingGuard
{
public:
    explicit TAggregatingTimingGuard(TDuration* value)
        : Value_(value)
        , StartInstant_(GetCpuInstant())
    { }

    ~TAggregatingTimingGuard()
    {
        *Value_ += CpuDurationToDuration(GetCpuInstant() - StartInstant_);
    }

private:
    TDuration* const Value_;
    const TCpuInstant StartInstant_;
    
};

class TProfilingTimingGuard
{
public:
    TProfilingTimingGuard(
        const TProfiler& profiler,
        TSimpleCounter* counter)
        : Profiler_(profiler)
        , Counter_(counter)
        , StartInstant_(GetCpuInstant())
    { }

    ~TProfilingTimingGuard()
    {
        Profiler_.Increment(*Counter_, GetCpuInstant() - StartInstant_);
    }

private:
    const TProfiler& Profiler_;
    TSimpleCounter* const Counter_;
    const TCpuInstant StartInstant_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NProfiling
} // namespace NYT
