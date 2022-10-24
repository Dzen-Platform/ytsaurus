#include "invoker_queue.h"
#include "private.h"

#include <yt/yt/core/actions/invoker_detail.h>
#include <yt/yt/core/actions/invoker_util.h>

#include <yt/yt/core/profiling/tscp.h>

namespace NYT::NConcurrency {

using namespace NProfiling;
using namespace NYTProf;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ConcurrencyLogger;

////////////////////////////////////////////////////////////////////////////////

constinit thread_local TCpuProfilerTagGuard CpuProfilerTagGuard;

////////////////////////////////////////////////////////////////////////////////

static constexpr int MaxTryDequeueCount = 100;

Y_FORCE_INLINE void TMpmcQueueImpl::Enqueue(TEnqueuedAction action)
{
    Queue_.enqueue(std::move(action));
    Size_.fetch_add(1, std::memory_order_release);
}

Y_FORCE_INLINE bool TMpmcQueueImpl::TryDequeue(TEnqueuedAction* action, TConsumerToken* token)
{
    if (Size_.load() <= 0) {
        return false;
    }

    // Fast path.
    if (Size_.fetch_sub(1) <= 0) {
        Size_.fetch_add(1);

        // Slow path.
        auto queueSize = Size_.load();
        while (queueSize > 0 && !Size_.compare_exchange_weak(queueSize, queueSize - 1));

        if (queueSize <= 0) {
            return false;
        }
    }

    for (int tryIndex = 0; tryIndex < MaxTryDequeueCount; ++tryIndex) {
        bool result = token
            ? Queue_.try_dequeue(*token, *action)
            : Queue_.try_dequeue(*action);
        if (result) {
            if (tryIndex > 1) {
                YT_LOG_DEBUG("Action has been dequeued (TryIndex: %v)", tryIndex);
            }

            return true;
        }
    }

    YT_ABORT();
}

void TMpmcQueueImpl::DrainProducer()
{
    auto size = Size_.exchange(0);
    TEnqueuedAction action;
    while (size-- > 0) {
        YT_VERIFY(Queue_.try_dequeue(action));
    }
}

void TMpmcQueueImpl::DrainConsumer()
{
    DrainProducer();
}

TMpmcQueueImpl::TConsumerToken TMpmcQueueImpl::MakeConsumerToken()
{
    return TConsumerToken(Queue_);
}

bool TMpmcQueueImpl::IsEmpty() const
{
    return Size_.load() <= 0;
}

bool TMpmcQueueImpl::HasSingleConsumer() const
{
    return false;
}

////////////////////////////////////////////////////////////////////////////////

Y_FORCE_INLINE void TMpscQueueImpl::Enqueue(TEnqueuedAction action)
{
    Queue_.Enqueue(std::move(action));
}

Y_FORCE_INLINE bool TMpscQueueImpl::TryDequeue(TEnqueuedAction* action, TConsumerToken* /*token*/)
{
    return Queue_.TryDequeue(action);
}

void TMpscQueueImpl::DrainProducer()
{
    Queue_.DrainProducer();
}

void TMpscQueueImpl::DrainConsumer()
{
    Queue_.DrainConsumer();
}

TMpscQueueImpl::TConsumerToken TMpscQueueImpl::MakeConsumerToken()
{
    return {};
}

bool TMpscQueueImpl::IsEmpty() const
{
    return Queue_.IsEmpty();
}

bool TMpscQueueImpl::HasSingleConsumer() const
{
    return true;
}

////////////////////////////////////////////////////////////////////////////////

template <class TQueueImpl>
class TProfilingTagSettingInvoker
    : public IInvoker
{
public:
    TProfilingTagSettingInvoker(
        TWeakPtr<TInvokerQueue<TQueueImpl>> queue,
        int profilingTag,
        TProfilerTagPtr profilerTag)
        : Queue_(std::move(queue))
        , ProfilingTag_(profilingTag)
        , ProfilerTag_(std::move(profilerTag))
    { }

    void Invoke(TClosure callback) override
    {
        if (auto queue = Queue_.Lock()) {
            queue->Invoke(std::move(callback), ProfilingTag_, ProfilerTag_);
        }
    }

    TThreadId GetThreadId() const override
    {
        if (auto queue = Queue_.Lock()) {
            return queue->GetThreadId();
        } else {
            return {};
        }
    }

    bool CheckAffinity(const IInvokerPtr& invoker) const override
    {
        return invoker.Get() == this;
    }

    bool IsSerialized() const override
    {
        if (auto queue = Queue_.Lock()) {
            return queue->IsSerialized();
        } else {
            return true;
        }
    }

private:
    const TWeakPtr<TInvokerQueue<TQueueImpl>> Queue_;
    const int ProfilingTag_;
    const TProfilerTagPtr ProfilerTag_;
};

////////////////////////////////////////////////////////////////////////////////

template <class TQueueImpl>
TInvokerQueue<TQueueImpl>::TInvokerQueue(
    TIntrusivePtr<NThreading::TEventCount> callbackEventCount,
    const TTagSet& counterTagSet)
    : CallbackEventCount_(std::move(callbackEventCount))
{
    Counters_.push_back(CreateCounters(counterTagSet));
}

template <class TQueueImpl>
TInvokerQueue<TQueueImpl>::TInvokerQueue(
    TIntrusivePtr<NThreading::TEventCount> callbackEventCount,
    const std::vector<TTagSet>& counterTagSets,
    const std::vector<NYTProf::TProfilerTagPtr>& profilerTags,
    const TTagSet& cumulativeCounterTagSet)
    : CallbackEventCount_(std::move(callbackEventCount))
{
    YT_VERIFY(counterTagSets.size() == profilerTags.size());

    Counters_.reserve(counterTagSets.size());
    for (const auto& tagSet : counterTagSets) {
        Counters_.push_back(CreateCounters(tagSet));
    }

    CumulativeCounters_ = CreateCounters(cumulativeCounterTagSet);

    ProfilingTagSettingInvokers_.reserve(Counters_.size());
    for (int index = 0; index < std::ssize(Counters_); ++index) {
        ProfilingTagSettingInvokers_.push_back(
            New<TProfilingTagSettingInvoker<TQueueImpl>>(MakeWeak(this), index, profilerTags[index]));
    }
}

template <class TQueueImpl>
void TInvokerQueue<TQueueImpl>::SetThreadId(TThreadId threadId)
{
    ThreadId_ = threadId;
}

template <class TQueueImpl>
void TInvokerQueue<TQueueImpl>::Invoke(TClosure callback)
{
    YT_ASSERT(Counters_.size() == 1);
    Invoke(std::move(callback), /*profilingTag*/ 0, /*profilerTag*/ nullptr);
}

template <class TQueueImpl>
void TInvokerQueue<TQueueImpl>::Invoke(
    TClosure callback,
    int profilingTag,
    TProfilerTagPtr profilerTag)
{
    EnqueueCallback(std::move(callback), profilingTag, profilerTag);
    CallbackEventCount_->NotifyOne();
}

template <class TQueueImpl>
TCpuInstant TInvokerQueue<TQueueImpl>::EnqueueCallback(
    TClosure callback,
    int profilingTag,
    TProfilerTagPtr profilerTag)
{
    YT_ASSERT(callback);
    YT_ASSERT(profilingTag >= 0 && profilingTag < std::ssize(Counters_));

    YT_LOG_TRACE("Callback enqueued (Callback: %v, ProfilingTag: %v)",
        callback.GetHandle(),
        profilingTag);

    auto cpuInstant = GetCpuInstant();

    auto updateCounters = [&] (const TCountersPtr& counters) {
        if (counters) {
            counters->ActiveCallbacks += 1;
            counters->EnqueuedCounter.Increment();
        }
    };
    updateCounters(Counters_[profilingTag]);
    updateCounters(CumulativeCounters_);

    TEnqueuedAction action{
        .Finished = false,
        .EnqueuedAt = cpuInstant,
        .Callback = std::move(callback),
        .ProfilingTag = profilingTag,
        .ProfilerTag = std::move(profilerTag)
    };
    QueueImpl_.Enqueue(std::move(action));

    if (!Running_.load(std::memory_order_relaxed)) {
        DrainProducer();
        YT_LOG_TRACE(
            "Queue had been shut down, incoming action ignored (Callback: %v)",
            callback.GetHandle());
        return cpuInstant;
    }

    return cpuInstant;
}

template <class TQueueImpl>
TThreadId TInvokerQueue<TQueueImpl>::GetThreadId() const
{
    return ThreadId_;
}

template <class TQueueImpl>
bool TInvokerQueue<TQueueImpl>::CheckAffinity(const IInvokerPtr& invoker) const
{
    return invoker.Get() == this;
}

template <class TQueueImpl>
bool TInvokerQueue<TQueueImpl>::IsSerialized() const
{
    return QueueImpl_.HasSingleConsumer();
}

template <class TQueueImpl>
void TInvokerQueue<TQueueImpl>::Shutdown()
{
    Running_.store(false, std::memory_order_relaxed);
}

template <class TQueueImpl>
void TInvokerQueue<TQueueImpl>::DrainProducer()
{
    YT_VERIFY(!Running_.load(std::memory_order_relaxed));

    QueueImpl_.DrainProducer();
}

template <class TQueueImpl>
void TInvokerQueue<TQueueImpl>::DrainConsumer()
{
    YT_VERIFY(!Running_.load(std::memory_order_relaxed));

    QueueImpl_.DrainConsumer();
}

template <class TQueueImpl>
TClosure TInvokerQueue<TQueueImpl>::BeginExecute(TEnqueuedAction* action, typename TQueueImpl::TConsumerToken* token)
{
    YT_ASSERT(action && action->Finished);

    if (!QueueImpl_.TryDequeue(action, token)) {
        return {};
    }

    auto cpuInstant = GetCpuInstant();

    action->StartedAt = cpuInstant;

    auto waitTime = CpuDurationToDuration(action->StartedAt - action->EnqueuedAt);

    auto updateCounters = [&] (const TCountersPtr& counters) {
        if (counters) {
            counters->DequeuedCounter.Increment();
            counters->WaitTimer.Record(waitTime);
        }
    };
    updateCounters(Counters_[action->ProfilingTag]);
    updateCounters(CumulativeCounters_);

    if (const auto& profilerTag = action->ProfilerTag) {
        CpuProfilerTagGuard = TCpuProfilerTagGuard(profilerTag);
    } else {
        CpuProfilerTagGuard = {};
    }

    SetCurrentInvoker(GetProfilingTagSettingInvoker(action->ProfilingTag));

    return std::move(action->Callback);
}

template <class TQueueImpl>
void TInvokerQueue<TQueueImpl>::EndExecute(TEnqueuedAction* action)
{
    CpuProfilerTagGuard = TCpuProfilerTagGuard{};
    SetCurrentInvoker(nullptr);

    YT_ASSERT(action);

    if (action->Finished) {
        return;
    }

    auto cpuInstant = GetCpuInstant();
    action->FinishedAt = cpuInstant;
    action->Finished = true;

    auto timeFromStart = CpuDurationToDuration(action->FinishedAt - action->StartedAt);
    auto timeFromEnqueue = CpuDurationToDuration(action->FinishedAt - action->EnqueuedAt);

    auto updateCounters = [&] (const TCountersPtr& counters) {
        if (counters) {
            counters->ExecTimer.Record(timeFromStart);
            counters->CumulativeTimeCounter.Add(timeFromStart);
            counters->TotalTimer.Record(timeFromEnqueue);
            counters->ActiveCallbacks -= 1;
        }
    };
    updateCounters(Counters_[action->ProfilingTag]);
    updateCounters(CumulativeCounters_);
}

template <class TQueueImpl>
typename TQueueImpl::TConsumerToken TInvokerQueue<TQueueImpl>::MakeConsumerToken()
{
    return QueueImpl_.MakeConsumerToken();
}

template <class TQueueImpl>
bool TInvokerQueue<TQueueImpl>::IsEmpty() const
{
    return QueueImpl_.IsEmpty();
}

template <class TQueueImpl>
bool TInvokerQueue<TQueueImpl>::IsRunning() const
{
    return Running_.load(std::memory_order_relaxed);
}

template <class TQueueImpl>
IInvokerPtr TInvokerQueue<TQueueImpl>::GetProfilingTagSettingInvoker(int profilingTag)
{
    if (ProfilingTagSettingInvokers_.empty()) {
        // Fast path.
        YT_ASSERT(profilingTag == 0);
        return this;
    } else {
        YT_ASSERT(0 <= profilingTag && profilingTag < std::ssize(Counters_));
        return ProfilingTagSettingInvokers_[profilingTag];
    }
}

template <class TQueueImpl>
typename TInvokerQueue<TQueueImpl>::TCountersPtr TInvokerQueue<TQueueImpl>::CreateCounters(const TTagSet& tagSet)
{
    auto profiler = TProfiler("/action_queue").WithTags(tagSet).WithHot();

    auto counters = std::make_unique<TCounters>();
    counters->EnqueuedCounter = profiler.Counter("/enqueued");
    counters->DequeuedCounter = profiler.Counter("/dequeued");
    counters->WaitTimer = profiler.Timer("/time/wait");
    counters->ExecTimer = profiler.Timer("/time/exec");
    counters->CumulativeTimeCounter = profiler.TimeCounter("/time/cumulative");
    counters->TotalTimer = profiler.Timer("/time/total");

    profiler.AddFuncGauge("/size", MakeStrong(this), [counters = counters.get()] {
        return counters->ActiveCallbacks.load();
    });

    return counters;
}

////////////////////////////////////////////////////////////////////////////////

template class TInvokerQueue<TMpmcQueueImpl>;
template class TInvokerQueue<TMpscQueueImpl>;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency
