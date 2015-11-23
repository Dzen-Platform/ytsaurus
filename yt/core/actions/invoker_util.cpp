#include "stdafx.h"
#include "invoker_util.h"

#include <stack>

#include <core/misc/singleton.h>
#include <core/misc/lazy_ptr.h>

#include <core/actions/bind.h>
#include <core/actions/callback.h>

#include <core/concurrency/fls.h>
#include <core/concurrency/action_queue.h>

namespace NYT {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TSyncInvoker
    : public IInvoker
{
public:
    virtual void Invoke(const TClosure& callback) override
    {
        callback.Run();
    }

#ifdef YT_ENABLE_THREAD_AFFINITY_CHECK
    virtual bool CheckAffinity(IInvokerPtr invoker) const override
    {
        return invoker.Get() == this;
    }

    virtual TThreadId GetThreadId() const override
    {
        return InvalidThreadId;
    }
#endif
};

IInvokerPtr GetSyncInvoker()
{
    return RefCountedSingleton<TSyncInvoker>();
}

////////////////////////////////////////////////////////////////////////////////

class TNullInvoker
    : public IInvoker
{
public:
    virtual void Invoke(const TClosure& /*callback*/) override
    { }

#ifdef YT_ENABLE_THREAD_AFFINITY_CHECK
    virtual bool CheckAffinity(IInvokerPtr /*invoker*/) const override
    {
        return false;
    }

    virtual TThreadId GetThreadId() const override
    {
        return InvalidThreadId;
    }
#endif
};

IInvokerPtr GetNullInvoker()
{
    return RefCountedSingleton<TNullInvoker>();
}

////////////////////////////////////////////////////////////////////////////////

static TLazyIntrusivePtr<TActionQueue> FinalizerThread(
    TActionQueue::CreateFactory("Finalizer", false, false));
static std::atomic<bool> FinalizerThreadIsDead = {false};

IInvokerPtr GetFinalizerInvoker()
{
    // When |FinalizedThread| is already destructed, we would like to avoid
    // member variables to avoid crashes. Since we force end-users to shutdown
    // finalizer thread explicitly (and in a single thread) we can rely on
    // |FinalizerThreadIsDead| to be set by appropriate shutdown code.
    if (!FinalizerThreadIsDead.load(std::memory_order_relaxed)) {
        return FinalizerThread->GetInvoker();
    } else {
        return GetSyncInvoker();
    }
}

void ShutdownFinalizerThread()
{
    if (FinalizerThread.HasValue()) {
#if 0
        if (FinalizerThread->IsRunning()) {
            // Await completion of every action enqueued before this shutdown call.
            for (int i = 0; i < 100; ++i) {
                auto sentinel = BIND([] () {}).AsyncVia(FinalizerThread->GetInvoker()).Run();
                sentinel.Get().ThrowOnError();
           }
        }
#endif
        // Now kill the thread.
        FinalizerThread->Shutdown();
        // This code is (usually) run in a single-threaded context,
        // so we simply raise the flag.
        FinalizerThreadIsDead.store(true, std::memory_order_relaxed);
    }
}

////////////////////////////////////////////////////////////////////////////////

void GuardedInvoke(
    IInvokerPtr invoker,
    TClosure onSuccess,
    TClosure onCancel)
{
    YASSERT(invoker);
    YASSERT(onSuccess);
    YASSERT(onCancel);

    class TGuard
    {
    public:
        explicit TGuard(TClosure onCancel)
            : OnCancel_(std::move(onCancel))
        { }

        TGuard(TGuard&& other) = default;

        ~TGuard()
        {
            if (OnCancel_) {
                OnCancel_.Run();
            }
        }

        void Release()
        {
            OnCancel_.Reset();
        }

    private:
        TClosure OnCancel_;

    };

    auto doInvoke = [] (TClosure onSuccess, TGuard guard) {
        guard.Release();
        onSuccess.Run();
    };

    invoker->Invoke(BIND(
        std::move(doInvoke),
        Passed(std::move(onSuccess)),
        Passed(TGuard(std::move(onCancel)))));
}

////////////////////////////////////////////////////////////////////////////////

static TFls<IInvokerPtr>& CurrentInvoker()
{
    static TFls<IInvokerPtr> invoker;
    return invoker;
}

IInvokerPtr GetCurrentInvoker()
{
    auto invoker = *CurrentInvoker();
    if (!invoker) {
        invoker = GetSyncInvoker();
    }
    return invoker;
}

void SetCurrentInvoker(IInvokerPtr invoker)
{
    *CurrentInvoker().Get() = std::move(invoker);
}

void SetCurrentInvoker(IInvokerPtr invoker, TFiber* fiber)
{
    *CurrentInvoker().Get(fiber) = std::move(invoker);
}

////////////////////////////////////////////////////////////////////////////////

TCurrentInvokerGuard::TCurrentInvokerGuard(IInvokerPtr invoker)
    : SavedInvoker_(std::move(invoker))
{
    CurrentInvoker()->Swap(SavedInvoker_);
}

TCurrentInvokerGuard::~TCurrentInvokerGuard()
{
    CurrentInvoker()->Swap(SavedInvoker_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
