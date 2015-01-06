#pragma once

#include "public.h"
#include "invoker.h"

#include <core/actions/future.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! Returns the synchronous invoker, i.e. the invoker whose |Invoke|
//! method invokes the closure immediately.
IInvokerPtr GetSyncInvoker();

//! Returns the null invoker, i.e. the invoker whose |Invoke|
//! method does nothing.
IInvokerPtr GetNullInvoker();

//! Returns a special per-process invoker that handles all asynchronous finalization
//! activities (fiber unwinding, abandoned promise cancelation etc).
IInvokerPtr GetFinalizerInvoker();

// TODO(babenko): remove this when Shutdown Club is finished
void ShutdownFinalizerThread();

//! Tries to invoke #onSuccess via #invoker.
//! If the invoker discards the callback without executing it then
//! #onCancel is run.
void GuardedInvoke(
    IInvokerPtr invoker,
    TClosure onSuccess,
    TClosure onCancel);

////////////////////////////////////////////////////////////////////////////////
// Provides a way to work with the current invoker (per-fiber).
// Invoker is fiber-scoped so this is an access to FLS.

IInvokerPtr GetCurrentInvoker();
void SetCurrentInvoker(IInvokerPtr invoker);
void SetCurrentInvoker(IInvokerPtr invoker, NConcurrency::TFiber* fiber);

//! Swaps the current active invoker with a provided one.
class TCurrentInvokerGuard
{
public:
    explicit TCurrentInvokerGuard(IInvokerPtr invoker);
    ~TCurrentInvokerGuard();

private:
    IInvokerPtr SavedInvoker_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
