#include "invoker_queue.h"
#include "private.h"

namespace NYT {
namespace NConcurrency {

using namespace NProfiling;

///////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ConcurrencyLogger;

///////////////////////////////////////////////////////////////////////////////

TInvokerQueue::TInvokerQueue(
    std::shared_ptr<TEventCount> callbackEventCount,
    const NProfiling::TTagIdList& tagIds,
    bool enableLogging,
    bool enableProfiling)
    : CallbackEventCount(std::move(callbackEventCount))
    , EnableLogging(enableLogging)
    , Profiler("/action_queue")
    , EnqueuedCounter("/enqueued", tagIds)
    , DequeuedCounter("/dequeued", tagIds)
    , SizeCounter("/size", tagIds)
    , WaitTimeCounter("/time/wait", tagIds)
    , ExecTimeCounter("/time/exec", tagIds)
    , CumulativeTimeCounter("/time/cumulative", tagIds)
    , TotalTimeCounter("/time/total", tagIds)
    , CumulativeTimeCounter("/time/cumulative", tagIds)
{
    Profiler.SetEnabled(enableProfiling);
}

TInvokerQueue::~TInvokerQueue() = default;

void TInvokerQueue::SetThreadId(TThreadId threadId)
{
    ThreadId = threadId;
}

void TInvokerQueue::Invoke(const TClosure& callback)
{
    YASSERT(callback);

    if (!Running.load(std::memory_order_relaxed)) {
        LOG_TRACE_IF(
            EnableLogging,
            "Queue had been shut down, incoming action ignored: %p",
            callback.GetHandle());
        return;
    }

    QueueSize.fetch_add(1, std::memory_order_relaxed);

    Profiler.Increment(EnqueuedCounter);

    LOG_TRACE_IF(EnableLogging, "Callback enqueued: %p",
        callback.GetHandle());

    TEnqueuedAction action;
    action.Finished = false;
    action.EnqueuedAt = GetCpuInstant();
    action.Callback = callback;
    Queue.Enqueue(action);

    CallbackEventCount->NotifyOne();
}

#ifdef YT_ENABLE_THREAD_AFFINITY_CHECK
TThreadId TInvokerQueue::GetThreadId() const
{
    return ThreadId;
}

bool TInvokerQueue::CheckAffinity(IInvokerPtr invoker) const
{
    return invoker.Get() == this;
}
#endif

void TInvokerQueue::Shutdown()
{
    Running.store(false, std::memory_order_relaxed);
}

void TInvokerQueue::Drain()
{
    YCHECK(!Running.load(std::memory_order_relaxed));

    int dequeued = 0;

    TEnqueuedAction action;
    while (Queue.Dequeue(&action)) {
        ++dequeued;
        action.Callback.Reset();
    }

    QueueSize.fetch_sub(dequeued, std::memory_order_relaxed);
    YCHECK(QueueSize.load(std::memory_order_relaxed) == 0);
}

EBeginExecuteResult TInvokerQueue::BeginExecute(TEnqueuedAction* action)
{
    YASSERT(action && action->Finished);

    if (!Queue.Dequeue(action)) {
        return EBeginExecuteResult::QueueEmpty;
    }

    CallbackEventCount->CancelWait();

    Profiler.Increment(DequeuedCounter);

    action->StartedAt = GetCpuInstant();

    Profiler.Update(
        WaitTimeCounter,
        CpuDurationToValue(action->StartedAt - action->EnqueuedAt));

    // Move callback to the stack frame to ensure that we hold it as long as it runs.
    auto callback = std::move(action->Callback);
    try {
        TCurrentInvokerGuard guard(this);
        callback.Run();
        return EBeginExecuteResult::Success;
    } catch (const TFiberCanceledException&) {
        return EBeginExecuteResult::Terminated;
    }
}

void TInvokerQueue::EndExecute(TEnqueuedAction* action)
{
    YASSERT(action);

    if (action->Finished) {
        return;
    }

    int queueSize = QueueSize.fetch_sub(1, std::memory_order_relaxed) - 1;
    Profiler.Update(SizeCounter, queueSize);

    auto finishedAt = GetCpuInstant();
    auto timeFromStart = CpuDurationToValue(finishedAt - action->StartedAt);
    auto timeFromEnqueue = CpuDurationToValue(finishedAt - action->EnqueuedAt);
    Profiler.Update(ExecTimeCounter, timeFromStart);
    Profiler.Increment(CumulativeTimeCounter, timeFromStart);
    Profiler.Update(TotalTimeCounter, timeFromEnqueue);

    action->Finished = true;
}

int TInvokerQueue::GetSize() const
{
    return QueueSize.load(std::memory_order_relaxed);
}

bool TInvokerQueue::IsEmpty() const
{
    return const_cast<TLockFreeQueue<TEnqueuedAction>&>(Queue).IsEmpty();
}

bool TInvokerQueue::IsRunning() const
{
    return Running.load(std::memory_order_relaxed);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
