#pragma once

#include "private.h"
#include "event_count.h"
#include "execution_context.h"
#include "invoker_queue.h"
#include "scheduler.h"
#include "thread_affinity.h"

#include <yt/core/actions/callback.h>
#include <yt/core/actions/future.h>
#include <yt/core/actions/invoker.h>
#include <yt/core/actions/signal.h>

#include <yt/core/misc/shutdownable.h>

#include <yt/core/profiling/profiler.h>

#include <util/system/thread.h>

#include <util/thread/lfqueue.h>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

class TSchedulerThread
    : public TRefCounted
    , public IScheduler
    , public IShutdownable
{
public:
    virtual ~TSchedulerThread();

    void Start();
    virtual void Shutdown() override;

    TThreadId GetId() const;
    bool IsStarted() const;
    bool IsShutdown() const;

    virtual TFiber* GetCurrentFiber() override;
    virtual void Return() override;
    virtual void Yield() override;
    virtual void YieldTo(TFiberPtr&& other) override;
    virtual void SwitchTo(IInvokerPtr invoker) override;
    virtual void SubscribeContextSwitched(TClosure callback) override;
    virtual void UnsubscribeContextSwitched(TClosure callback) override;
    virtual void WaitFor(TFuture<void> future, IInvokerPtr invoker) override;

protected:
    TSchedulerThread(
        std::shared_ptr<TEventCount> callbackEventCount,
        const Stroka& threadName,
        const NProfiling::TTagIdList& tagIds,
        bool enableLogging,
        bool enableProfiling,
        bool detached);

    virtual EBeginExecuteResult BeginExecute() = 0;
    virtual void EndExecute() = 0;

    virtual void OnStart();

    virtual void BeforeShutdown();
    virtual void AfterShutdown();

    virtual void OnThreadStart();
    virtual void OnThreadShutdown();

    static void* ThreadMain(void* opaque);
    void ThreadMain();
    void ThreadMainStep();

    void FiberMain(ui64 spawnedEpoch);
    bool FiberMainStep(ui64 spawnedEpoch);

    void Reschedule(TFiberPtr fiber, TFuture<void> future, IInvokerPtr invoker);

    void OnContextSwitch();

    const std::shared_ptr<TEventCount> CallbackEventCount;
    const Stroka ThreadName;
    const bool EnableLogging;
    const bool Detached;

    NProfiling::TProfiler Profiler;

    // First bit is an indicator whether startup was performed.
    // Second bit is an indicator whether shutdown was requested.
    std::atomic<ui64> Epoch = {0};
    static constexpr ui64 StartedEpochMask = 0x1;
    static constexpr ui64 ShutdownEpochMask = 0x2;
    static constexpr ui64 TurnShift = 2;
    static constexpr ui64 TurnDelta = 1 << TurnShift;

    TEvent ThreadStartedEvent;
    TEvent ThreadShutdownEvent;

    TThreadId ThreadId = InvalidThreadId;
    TThread Thread;

    TExecutionContext SchedulerContext;

    std::list<TFiberPtr> RunQueue;
    NProfiling::TSimpleCounter CreatedFibersCounter;
    NProfiling::TSimpleCounter AliveFibersCounter;

    TFiberPtr IdleFiber;
    TFiberPtr CurrentFiber;

    TFuture<void> WaitForFuture;
    IInvokerPtr SwitchToInvoker;

    TCallbackList<void()> ContextSwitchCallbacks;

    DECLARE_THREAD_AFFINITY_SLOT(HomeThread);
};

DEFINE_REFCOUNTED_TYPE(TSchedulerThread)

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
