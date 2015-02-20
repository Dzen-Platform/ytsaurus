#pragma once

#include "public.h"

#include <core/actions/signal.h>

#include <core/concurrency/periodic_executor.h>

#include <core/misc/error.h>

#include <core/logging/log.h>

#include <atomic>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

/*!
 *  Checks disk health by writing a small file of random content
 *  periodically, reading it, and comparing the content.
 */
class TDiskHealthChecker
    : public TRefCounted
{
public:
    TDiskHealthChecker(
        TDiskHealthCheckerConfigPtr config,
        const Stroka& path,
        IInvokerPtr invoker);

    //! Runs single health check. 
    //! Don't call after #Start(), otherwise two checks may interfere.
    TFuture<void> RunCheck();

    void Start();

    DEFINE_SIGNAL(void(const TError&), Failed);

private:
    TDiskHealthCheckerConfigPtr Config_;
    Stroka Path_;

    IInvokerPtr CheckInvoker_;

    NConcurrency::TPeriodicExecutorPtr PeriodicExecutor_;
    std::atomic_flag FailedLock_;

    NLogging::TLogger Logger;


    void OnCheck();
    void OnCheckCompleted(const TError& error);

    void DoRunCheck();

};

DEFINE_REFCOUNTED_TYPE(TDiskHealthChecker)

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

