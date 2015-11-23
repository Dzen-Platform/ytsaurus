#pragma once

#include "public.h"

#include <core/actions/callback.h>

#include <core/misc/shutdownable.h>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

// XXX(sandello): Facade does not have to be ref-counted.
class TActionQueue
    : public TRefCounted
    , public IShutdownable
{
public:
    explicit TActionQueue(
        const Stroka& threadName = "<ActionQueue>",
        bool enableLogging = true,
        bool enableProfiling = true);

    virtual ~TActionQueue();

    virtual void Shutdown() override;

    IInvokerPtr GetInvoker();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TActionQueue)

////////////////////////////////////////////////////////////////////////////////

//! Creates an invoker that executes all callbacks in the
//! context of #underlyingInvoker (possibly in different threads)
//! but in a serialized fashion (i.e. all queued callbacks are executed
//! in the proper order and no two callbacks are executed in parallel).
IInvokerPtr CreateSerializedInvoker(IInvokerPtr underlyingInvoker);

////////////////////////////////////////////////////////////////////////////////

//! Creates a wrapper around IInvoker that supports callback reordering.
//! Callbacks with the highest priority are executed first.
IPrioritizedInvokerPtr CreatePrioritizedInvoker(IInvokerPtr underlyingInvoker);

//! Creates a wrapper around IInvoker that implements IPrioritizedInvoker but
//! does not perform any actual reordering. Priorities passed to #IPrioritizedInvoker::Invoke
//! are ignored.
IPrioritizedInvokerPtr CreateFakePrioritizedInvoker(IInvokerPtr underlyingInvoker);

//! Creates a wrapper around IPrioritizedInvoker turning it into a regular IInvoker.
//! All callbacks are propagated with a given fixed #priority.
IInvokerPtr CreateFixedPriorityInvoker(
    IPrioritizedInvokerPtr underlyingInvoker,
    i64 priority);

////////////////////////////////////////////////////////////////////////////////

//! Creates an invoker that executes all callbacks in the
//! context of #underlyingInvoker allowing up to #maxConcurrentInvocations
//! outstanding requests to the latter.
IInvokerPtr CreateBoundedConcurrencyInvoker(
    IInvokerPtr underlyingInvoker,
    int maxConcurrentInvocations,
    const Stroka& invokerName = "<BCI>");

////////////////////////////////////////////////////////////////////////////////

ISuspendableInvokerPtr CreateSuspendableInvoker(IInvokerPtr underlyingInvoker);

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
