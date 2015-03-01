#include "stdafx.h"
#include "profiler.h"
#include "profile_manager.h"
#include "timing.h"

#include <core/misc/nullable.h>

#include <core/ypath/token.h>

namespace NYT {
namespace NProfiling  {

using namespace NYPath;

////////////////////////////////////////////////////////////////////////////////

TTagIdList operator + (const TTagIdList& a, const TTagIdList& b)
{
    auto result = a;
    result += b;
    return result;
}

TTagIdList& operator += (TTagIdList& a, const TTagIdList& b)
{
    a.append(b.begin(), b.end());
    return a;
}

////////////////////////////////////////////////////////////////////////////////

TTimer::TTimer()
    : Start(0)
    , LastCheckpoint(0)
{ }

TTimer::TTimer(
    const TYPath& path,
    TCpuInstant start,
    ETimerMode mode,
    const TTagIdList& tagIds)
    : Path(path)
    , Start(start)
    , LastCheckpoint(0)
    , Mode(mode)
    , TagIds(tagIds)
{ }

////////////////////////////////////////////////////////////////////////////////

TCounterBase::TCounterBase(
    const TYPath& path,
    const TTagIdList& tagIds,
    TDuration interval)
    : Path(path)
    , TagIds(tagIds)
    , Interval(DurationToCpuDuration(interval))
    , Deadline(0)
{ }

////////////////////////////////////////////////////////////////////////////////

TRateCounter::TRateCounter(
    const TYPath& path,
    const TTagIdList& tagIds,
    TDuration interval)
    : TCounterBase(path, tagIds, interval)
    , Value(0)
    , LastValue(0)
    , LastTime(0)
{ }

TRateCounter::TRateCounter(const TRateCounter& other)
    : TCounterBase(other)
{
    *this = other;
}

TRateCounter& TRateCounter::operator=(const TRateCounter& other)
{
    static_cast<TCounterBase&>(*this) = other;
    Value.store(other.Value);
    LastValue = other.LastValue;
    LastTime = other.LastTime;
    return *this;
}

////////////////////////////////////////////////////////////////////////////////

TAggregateCounter::TAggregateCounter(
    const NYPath::TYPath& path,
    const TTagIdList& tagIds,
    EAggregateMode mode,
    TDuration interval)
    : TCounterBase(path, tagIds, interval)
    , Mode(mode)
    , Current(0)
{
    Reset();
}

void TAggregateCounter::Reset()
{
    Min = std::numeric_limits<TValue>::max();
    Max = std::numeric_limits<TValue>::min();
    Sum = 0;
    SampleCount = 0;
}

////////////////////////////////////////////////////////////////////////////////

TSimpleCounter::TSimpleCounter(
    const NYPath::TYPath& path,
    const TTagIdList& tagIds,
    TDuration interval)
    : TCounterBase(path, tagIds, interval)
    , Current(0)
{ }

////////////////////////////////////////////////////////////////////////////////

TProfiler::TProfiler()
    : Enabled_(false)
{ }

TProfiler::TProfiler(
    const TYPath& pathPrefix,
    const TTagIdList& tagIds,
    bool selfProfiling)
    : PathPrefix_(pathPrefix)
    , Enabled_(true)
    , TagIds_(tagIds)
    , SelfProfiling_(selfProfiling)
{ }

void TProfiler::Enqueue(
    const NYPath::TYPath& path,
    TValue value,
    const TTagIdList& tagIds)
{
    if (!Enabled_)
        return;

    TQueuedSample sample;
    sample.Time = GetCpuInstant();
    sample.Path = PathPrefix_ + path;
    sample.Value = value;
    sample.TagIds = TagIds_ + tagIds;
    TProfileManager::Get()->Enqueue(sample, SelfProfiling_);
}

TTimer TProfiler::TimingStart(
    const TYPath& path,
    const TTagIdList& tagIds,
    ETimerMode mode)
{
    return TTimer(path, GetCpuInstant(), mode, tagIds);
}

TDuration TProfiler::TimingStop(
    TTimer& timer,
    const TStringBuf& key)
{
    return DoTimingStop(timer, key, Null);
}

TDuration TProfiler::TimingStop(
    TTimer& timer,
    const TTagIdList& totalTagIds)
{
    return DoTimingStop(timer, Null, totalTagIds);
}

TDuration TProfiler::TimingStop(
    TTimer& timer)
{
    return DoTimingStop(timer, Null, Null);
}

TDuration TProfiler::DoTimingStop(
    TTimer& timer,
    const TNullable<TStringBuf>& key,
    const TNullable<TTagIdList>& totalTagIds)
{
    // Failure here means that the timer was not started or already stopped.
    YASSERT(timer.Start != 0);

    auto now = GetCpuInstant();
    auto cpuDuration = now - timer.Start;
    auto value = CpuDurationToValue(cpuDuration);
    YASSERT(value >= 0);

    auto path = key ? timer.Path + "/" + ToYPathLiteral(*key) : timer.Path;
    auto tagIds = totalTagIds ? timer.TagIds + *totalTagIds : timer.TagIds;
    Enqueue(path, value, tagIds);

    timer.Start = 0;

    return CpuDurationToDuration(cpuDuration);
}

TDuration TProfiler::TimingCheckpoint(
    TTimer& timer,
    const TStringBuf& key)
{
    return DoTimingCheckpoint(timer, key, Null);
}

TDuration TProfiler::TimingCheckpoint(
    TTimer& timer,
    const TTagIdList& tagIds)
{
    return DoTimingCheckpoint(timer, Null, tagIds);
}

TDuration TProfiler::DoTimingCheckpoint(
    TTimer& timer,
    const TNullable<TStringBuf>& key,
    const TNullable<TTagIdList>& checkpointTagIds)
{
    // Failure here means that the timer was not started or already stopped.
    YASSERT(timer.Start != 0);

    auto now = GetCpuInstant();

    // Upon receiving the first checkpoint Simple timer
    // is automatically switched into Sequential.
    if (timer.Mode == ETimerMode::Simple) {
        timer.Mode = ETimerMode::Sequential;
    }

    auto path = key ? timer.Path + "/" + ToYPathLiteral(*key) : timer.Path;
    auto tagIds = checkpointTagIds ? timer.TagIds + *checkpointTagIds : timer.TagIds;
    switch (timer.Mode) {
        case ETimerMode::Sequential: {
            auto lastCheckpoint = timer.LastCheckpoint == 0 ? timer.Start : timer.LastCheckpoint;
            auto duration = CpuDurationToValue(now - lastCheckpoint);
            YASSERT(duration >= 0);
            Enqueue(path, duration, tagIds);
            timer.LastCheckpoint = now;
            return CpuDurationToDuration(duration);
        }

        case ETimerMode::Parallel: {
            auto duration = CpuDurationToValue(now - timer.Start);
            YASSERT(duration >= 0);
            Enqueue(path, duration, tagIds);
            return CpuDurationToDuration(duration);
        }

        default:
            YUNREACHABLE();
    }
}

TValue TProfiler::Increment(TRateCounter& counter, TValue delta)
{
    YASSERT(delta >= 0);

    auto result = (counter.Value += delta);

    if (IsCounterEnabled(counter)) {
        OnUpdated(counter);
    }

    return result;
}

void TProfiler::Update(TAggregateCounter& counter, TValue value)
{
    TGuard<TSpinLock> guard(counter.SpinLock);

    if (IsCounterEnabled(counter)) {
        DoUpdate(counter, value);
    } else {
        counter.Current = value;
    }
}

TValue TProfiler::Increment(TAggregateCounter& counter, TValue delta)
{
    TGuard<TSpinLock> guard(counter.SpinLock);

    auto result = (counter.Current + delta);

    if (IsCounterEnabled(counter)) {
        DoUpdate(counter, counter.Current + delta);
    } else {
        counter.Current = result;
    }

    return result;
}

void TProfiler::Update(TSimpleCounter& counter, TValue value)
{
    counter.Current = value;

    if (IsCounterEnabled(counter)) {
        OnUpdated(counter);
    }
}

TValue TProfiler::Increment(TSimpleCounter& counter, TValue delta)
{
    auto result = (counter.Current += delta);

    if (IsCounterEnabled(counter)) {
        OnUpdated(counter);
    }

    return result;
}

bool TProfiler::IsCounterEnabled(const TCounterBase& counter)
{
    return Enabled_ && !counter.Path.empty();
}

void TProfiler::DoUpdate(TAggregateCounter& counter, TValue value)
{
    ++counter.SampleCount;
    counter.Current = value;
    counter.Min = std::min(counter.Min, value);
    counter.Max = std::max(counter.Max, value);
    counter.Sum += value;
    auto now = GetCpuInstant();
    if (now > counter.Deadline) {
        auto min = counter.Min;
        auto max = counter.Max;
        auto avg = counter.Sum / counter.SampleCount;
        counter.Reset();
        counter.Deadline = now + counter.Interval;
        switch (counter.Mode) {
            case EAggregateMode::All:
                Enqueue(counter.Path + "/min", min, counter.TagIds);
                Enqueue(counter.Path + "/max", max, counter.TagIds);
                Enqueue(counter.Path + "/avg", avg, counter.TagIds);
                break;

            case EAggregateMode::Min:
                Enqueue(counter.Path, min, counter.TagIds);
                break;

            case EAggregateMode::Max:
                Enqueue(counter.Path, max, counter.TagIds);
                break;

            case EAggregateMode::Avg:
                Enqueue(counter.Path, avg, counter.TagIds);
                break;

            default:
                YUNREACHABLE();
        }
    }
}

void TProfiler::OnUpdated(TRateCounter& counter)
{
    auto now = GetCpuInstant();
    if (now < counter.Deadline)
        return;

    TNullable<TValue> sampleValue;
    {
        TGuard<TSpinLock> guard(counter.SpinLock);

        if (now < counter.Deadline)
            return;

        if (counter.LastTime != 0) {
            auto counterDelta = counter.Value - counter.LastValue;
            auto timeDelta = now - counter.LastTime;
            sampleValue = counterDelta * counter.Interval / timeDelta;
        }

        counter.LastTime = now;
        counter.LastValue = counter.Value;
        counter.Deadline = now + counter.Interval;
    }

    if (sampleValue) {
        Enqueue(counter.Path, *sampleValue, counter.TagIds);
    }
}

void TProfiler::OnUpdated(TSimpleCounter& counter)
{
    auto now = GetCpuInstant();
    if (now < counter.Deadline)
        return;

    TValue sampleValue;
    {
        TGuard<TSpinLock> guard(counter.SpinLock);

        if (now < counter.Deadline)
            return;

        sampleValue = counter.Current;
        counter.Deadline = now + counter.Interval;
    }

    Enqueue(counter.Path, sampleValue, counter.TagIds);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NProfiling
} // namespace NYT
