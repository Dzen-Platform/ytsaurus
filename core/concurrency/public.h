#pragma once

#include <yt/core/misc/public.h>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TActionQueue)
DECLARE_REFCOUNTED_CLASS(TFairShareActionQueue)
DECLARE_REFCOUNTED_CLASS(TThreadPool)
DECLARE_REFCOUNTED_CLASS(TPeriodicExecutor)
DECLARE_REFCOUNTED_CLASS(TFiber)
DECLARE_REFCOUNTED_CLASS(TAsyncSemaphore)

DECLARE_REFCOUNTED_STRUCT(TDelayedExecutorEntry)
typedef TDelayedExecutorEntryPtr TDelayedExecutorCookie;

DECLARE_REFCOUNTED_CLASS(TThroughputThrottlerConfig)
DECLARE_REFCOUNTED_STRUCT(IThroughputThrottler)
DECLARE_REFCOUNTED_STRUCT(IReconfigurableThroughputThrottler)

DECLARE_REFCOUNTED_STRUCT(IAsyncInputStream)
DECLARE_REFCOUNTED_STRUCT(IAsyncOutputStream)
DECLARE_REFCOUNTED_STRUCT(IAsyncClosableOutputStream)

DECLARE_REFCOUNTED_STRUCT(IAsyncZeroCopyInputStream)
DECLARE_REFCOUNTED_STRUCT(IAsyncZeroCopyOutputStream)

DECLARE_REFCOUNTED_CLASS(TAsyncStreamPipe)

DEFINE_ENUM(ESyncStreamAdapterStrategy,
    (WaitFor)
    (Get)
);

class TAsyncSemaphore;

class TFiber;

template <class TSignature>
class TCoroutine;

template <class T>
class TNonblockingQueue;

DECLARE_REFCOUNTED_STRUCT(TLeaseEntry)
typedef TLeaseEntryPtr TLease;

typedef size_t TThreadId;
const size_t InvalidThreadId = 0;

typedef size_t TFiberId;
const size_t InvalidFiberId = 0;

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
