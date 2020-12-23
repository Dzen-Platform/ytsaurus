#include "fair_share_invoker_queue.h"
#include "invoker_queue.h"

namespace NYT::NConcurrency {

using namespace NProfiling;
using namespace NYPath;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TFairShareInvokerQueue::TFairShareInvokerQueue(
    std::shared_ptr<TEventCount> callbackEventCount,
    const std::vector<NProfiling::TTagSet>& bucketsTags)
    : Buckets_(bucketsTags.size())
{
    for (size_t index = 0; index < bucketsTags.size(); ++index) {
        Buckets_[index].Queue = New<TMpscInvokerQueue>(
            callbackEventCount,
            bucketsTags[index]);
        Buckets_[index].Invoker = Buckets_[index].Queue;
    }
}

TFairShareInvokerQueue::~TFairShareInvokerQueue() = default;

void TFairShareInvokerQueue::SetThreadId(TThreadId threadId)
{
    for (auto& bucket : Buckets_) {
        bucket.Queue->SetThreadId(threadId);
    }
}

const IInvokerPtr& TFairShareInvokerQueue::GetInvoker(int index)
{
    YT_ASSERT(0 <= index && index < static_cast<int>(Buckets_.size()));
    return Buckets_[index].Invoker;
}

void TFairShareInvokerQueue::Shutdown()
{
    for (auto& bucket : Buckets_) {
        bucket.Queue->Shutdown();
    }
}

void TFairShareInvokerQueue::Drain()
{
    for (auto& bucket : Buckets_) {
        bucket.Queue->Drain();
    }
}

bool TFairShareInvokerQueue::IsRunning() const
{
    for (const auto& bucket : Buckets_) {
        if (!bucket.Queue->IsRunning()) {
            return false;
        }
    }
    return true;
}

TClosure TFairShareInvokerQueue::BeginExecute(TEnqueuedAction* action)
{
    YT_VERIFY(!CurrentBucket_);

    // Check if any callback is ready at all.
    CurrentBucket_ = GetStarvingBucket();
    if (!CurrentBucket_) {
        return TClosure();
    }

    // Reduce excesses (with truncation).
    auto delta = CurrentBucket_->ExcessTime;
    for (auto& bucket : Buckets_) {
        bucket.ExcessTime = std::max<NProfiling::TCpuDuration>(bucket.ExcessTime - delta, 0);
    }

    // Pump the starving queue.
    return CurrentBucket_->Queue->BeginExecute(action);
}

void TFairShareInvokerQueue::EndExecute(TEnqueuedAction* action)
{
    if (!CurrentBucket_) {
        return;
    }

    CurrentBucket_->Queue->EndExecute(action);
    CurrentBucket_->ExcessTime += (action->FinishedAt - action->StartedAt);
    CurrentBucket_ = nullptr;
}

TFairShareInvokerQueue::TBucket* TFairShareInvokerQueue::GetStarvingBucket()
{
    // Compute min excess over non-empty queues.
    auto minExcessTime = std::numeric_limits<NProfiling::TCpuDuration>::max();
    TBucket* minBucket = nullptr;
    for (auto& bucket : Buckets_) {
        const auto& queue = bucket.Queue;
        YT_ASSERT(queue);
        if (!queue->IsEmpty()) {
            if (bucket.ExcessTime < minExcessTime) {
                minExcessTime = bucket.ExcessTime;
                minBucket = &bucket;
            }
        }
    }
    return minBucket;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency

