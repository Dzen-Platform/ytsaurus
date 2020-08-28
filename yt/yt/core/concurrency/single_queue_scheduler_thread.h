#pragma once

#include "private.h"
#include "scheduler_thread.h"
#include "invoker_queue.h"

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

class TSingleQueueSchedulerThread
    : public TSchedulerThread
{
public:
    TSingleQueueSchedulerThread(
        TInvokerQueuePtr queue,
        std::shared_ptr<TEventCount> callbackEventCount,
        const TString& threadName,
        const NProfiling::TTagIdList& tagIds,
        bool enableLogging,
        bool enableProfiling);

    ~TSingleQueueSchedulerThread();

protected:
    const TInvokerQueuePtr Queue;

    TEnqueuedAction CurrentAction;

    virtual TClosure BeginExecute() override;
    virtual void EndExecute() override;
};

DEFINE_REFCOUNTED_TYPE(TSingleQueueSchedulerThread)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency
