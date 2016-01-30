#pragma once

#include "public.h"
#include "private.h"

#include <yt/core/misc/shutdownable.h>

#include <yt/core/profiling/profiler.h>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

class TFairShareInvokerQueue
    : public TRefCounted
    , public IShutdownable
{
public:
    TFairShareInvokerQueue(
        std::shared_ptr<TEventCount> callbackEventCount,
        const std::vector<NProfiling::TTagIdList> bucketsTagIds,
        bool enableLogging,
        bool enableProfiling);

    ~TFairShareInvokerQueue();

    void SetThreadId(TThreadId threadId);

    IInvokerPtr GetInvoker(int index);

    virtual void Shutdown() override;

    void Drain();

    bool IsRunning() const;

    EBeginExecuteResult BeginExecute(TEnqueuedAction* action);
    void EndExecute(TEnqueuedAction* action);

private:
    struct TBucket
    {
        TInvokerQueuePtr Queue;
        NProfiling::TCpuDuration ExcessTime;
    };

    std::vector<TBucket> Buckets_;
    NProfiling::TCpuInstant StartInstant_;

    TBucket* CurrentBucket_ = nullptr;

    TBucket* GetStarvingBucket();
};

DEFINE_REFCOUNTED_TYPE(TFairShareInvokerQueue)

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT

