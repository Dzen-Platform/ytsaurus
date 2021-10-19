#pragma once

#include "private.h"
#include "scheduler_thread.h"
#include "invoker_queue.h"

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

template <class TQueueImpl>
class TSingleQueueSchedulerThread
    : public TSchedulerThread
{
public:
    TSingleQueueSchedulerThread(
        TInvokerQueuePtr<TQueueImpl> queue,
        TIntrusivePtr<TEventCount> callbackEventCount,
        const TString& threadGroupName,
        const TString& threadName,
        int shutdownPriority = 0);

protected:
    const TInvokerQueuePtr<TQueueImpl> Queue_;
    typename TQueueImpl::TConsumerToken Token_;

    TEnqueuedAction CurrentAction_;

    TClosure BeginExecute() override;
    void EndExecute() override;

    void OnStart() override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency
