#include "fair_share_queue_scheduler_thread.h"

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

TFairShareQueueSchedulerThread::TFairShareQueueSchedulerThread(
    TFairShareInvokerQueuePtr queue,
    std::shared_ptr<TEventCount> callbackEventCount,
    const TString& threadName,
    const NProfiling::TTagIdList& tagIds,
    bool enableLogging,
    bool enableProfiling)
    : TSchedulerThread(
        std::move(callbackEventCount),
        threadName,
        tagIds,
        enableLogging,
        enableProfiling)
    , Queue_(std::move(queue))
{ }

TFairShareQueueSchedulerThread::~TFairShareQueueSchedulerThread()
{ }

IInvokerPtr TFairShareQueueSchedulerThread::GetInvoker(int index)
{
    return Queue_->GetInvoker(index);
}

TClosure TFairShareQueueSchedulerThread::BeginExecute()
{
    return Queue_->BeginExecute(&CurrentAction_);
}

void TFairShareQueueSchedulerThread::EndExecute()
{
    Queue_->EndExecute(&CurrentAction_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency

