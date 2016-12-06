#include "scheduler.h"
#include "fiber.h"
#include "fls.h"

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

PER_THREAD IScheduler* CurrentScheduler = nullptr;

IScheduler* GetCurrentScheduler()
{
    YCHECK(CurrentScheduler);
    return CurrentScheduler;
}

IScheduler* TryGetCurrentScheduler()
{
    return CurrentScheduler;
}

TCurrentSchedulerGuard::TCurrentSchedulerGuard(IScheduler* scheduler)
    : SavedScheduler_(CurrentScheduler)
{
    CurrentScheduler = scheduler;
}

TCurrentSchedulerGuard::~TCurrentSchedulerGuard()
{
    CurrentScheduler = SavedScheduler_;
}

////////////////////////////////////////////////////////////////////////////////

TFiberId GetCurrentFiberId()
{
    auto* scheduler = TryGetCurrentScheduler();
    if (!scheduler) {
        return InvalidFiberId;
    }
    auto* fiber = scheduler->GetCurrentFiber();
    if (!fiber) {
        return InvalidFiberId;
    }
    return fiber->GetId();
}

void Yield()
{
    WaitFor(VoidFuture);
}

void SwitchTo(IInvokerPtr invoker)
{
    Y_ASSERT(invoker);
    GetCurrentScheduler()->SwitchTo(std::move(invoker));
}

void SubscribeContextSwitched(TClosure callback)
{
    GetCurrentScheduler()->SubscribeContextSwitched(std::move(callback));
}

void UnsubscribeContextSwitched(TClosure callback)
{
    GetCurrentScheduler()->UnsubscribeContextSwitched(std::move(callback));
}

////////////////////////////////////////////////////////////////////////////////

TContextSwitchedGuard::TContextSwitchedGuard(TClosure callback)
    : Callback_(std::move(callback))
{
    if (Callback_) {
        SubscribeContextSwitched(Callback_);
    }
}

TContextSwitchedGuard::~TContextSwitchedGuard()
{
    if (Callback_) {
        UnsubscribeContextSwitched(Callback_);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT

