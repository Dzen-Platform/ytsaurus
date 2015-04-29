#include "stdafx.h"
#include "action_queue.h"
#include "scheduler_thread.h"
#include "single_queue_scheduler_thread.h"
#include "private.h"

#include <core/actions/invoker_detail.h>

#include <core/ypath/token.h>

#include <core/profiling/profile_manager.h>

namespace NYT {
namespace NConcurrency {

using namespace NProfiling;
using namespace NYPath;
using namespace NYTree;

///////////////////////////////////////////////////////////////////////////////

namespace {

TTagIdList GetThreadTagIds(const Stroka& threadName)
{
    TTagIdList tagIds;
    auto* profilingManager = TProfileManager::Get();
    tagIds.push_back(profilingManager->RegisterTag("thread", threadName));
    return tagIds;
}

TTagIdList GetBucketTagIds(const Stroka& threadName, const Stroka& bucketName)
{
    TTagIdList tagIds;
    auto* profilingManager = TProfileManager::Get();
    tagIds.push_back(profilingManager->RegisterTag("thread", threadName));
    tagIds.push_back(profilingManager->RegisterTag("bucket", bucketName));
    return tagIds;
}

TTagIdList GetInvokerTagIds(const Stroka& invokerName)
{
    TTagIdList tagIds;
    auto* profilingManager = TProfileManager::Get();
    tagIds.push_back(profilingManager->RegisterTag("invoker", invokerName));
    return tagIds;
}

} // namespace

///////////////////////////////////////////////////////////////////////////////

class TActionQueue::TImpl
    : public TRefCounted
{
public:
    explicit TImpl(
        const Stroka& threadName,
        bool enableLogging,
        bool enableProfiling)
        : Queue_(New<TInvokerQueue>(
            &CallbackEventCount_,
            enableProfiling ? GetThreadTagIds(threadName) : NProfiling::EmptyTagIds,
            enableLogging,
            enableProfiling))
        , Thread_(New<TSingleQueueSchedulerThread>(
            Queue_,
            &CallbackEventCount_,
            threadName,
            enableProfiling ? GetThreadTagIds(threadName) : NProfiling::EmptyTagIds,
            enableLogging,
            enableProfiling))
    {
        Thread_->Start();
        Queue_->SetThreadId(Thread_->GetId());
    }

    ~TImpl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        Queue_->Shutdown();
        Thread_->Shutdown();
    }

    IInvokerPtr GetInvoker()
    {
        return Queue_;
    }

private:
    TEventCount CallbackEventCount_;
    TInvokerQueuePtr Queue_;
    TSingleQueueSchedulerThreadPtr Thread_;

};

TActionQueue::TActionQueue(
    const Stroka& threadName,
    bool enableLogging,
    bool enableProfiling)
    : Impl(New<TImpl>(
        threadName,
        enableLogging,
        enableProfiling))
{ }

TActionQueue::~TActionQueue()
{ }

void TActionQueue::Shutdown()
{
    return Impl->Shutdown();
}

IInvokerPtr TActionQueue::GetInvoker()
{
    return Impl->GetInvoker();
}

TCallback<TActionQueuePtr()> TActionQueue::CreateFactory(
    const Stroka& threadName,
    bool enableLogging,
    bool enableProfiling)
{
    return BIND(&New<TActionQueue, const Stroka&, const bool&, const bool&>,
        threadName,
        enableLogging,
        enableProfiling);
}

///////////////////////////////////////////////////////////////////////////////

class TFairShareActionQueue::TImpl
    : public TSchedulerThread
{
public:
    TImpl(
        const Stroka& threadName,
        const std::vector<Stroka>& bucketNames)
        : TSchedulerThread(
            &CallbackEventCount_,
            threadName,
            GetThreadTagIds(threadName),
            true,
            true)
        , Buckets_(bucketNames.size())
    {
        Start();
        for (int index = 0; index < static_cast<int>(bucketNames.size()); ++index) {
            auto& queue = Buckets_[index].Queue;
            queue = New<TInvokerQueue>(
                &CallbackEventCount_,
                GetBucketTagIds(threadName, bucketNames[index]),
                true,
                true);
            queue->SetThreadId(GetId());
        }
    }

    ~TImpl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        for (auto& bucket : Buckets_) {
            bucket.Queue->Shutdown();
        }
        TSchedulerThread::Shutdown();
    }

    IInvokerPtr GetInvoker(int index)
    {
        YASSERT(0 <= index && index < static_cast<int>(Buckets_.size()));
        return Buckets_[index].Queue;
    }

private:
    TEventCount CallbackEventCount_;

    struct TBucket
    {
        TInvokerQueuePtr Queue;
        TCpuDuration ExcessTime = 0;
    };

    std::vector<TBucket> Buckets_;
    TCpuInstant StartInstant_;

    TEnqueuedAction CurrentAction_;
    TBucket* CurrentBucket_ = nullptr;


    TBucket* GetStarvingBucket()
    {
        // Compute min excess over non-empty queues.
        i64 minExcess = std::numeric_limits<i64>::max();
        TBucket* minBucket = nullptr;
        for (auto& bucket : Buckets_) {
            auto queue = bucket.Queue;
            // NB: queue can be null during startup due to race with ctor
            if (queue && !queue->IsEmpty()) {
                if (bucket.ExcessTime < minExcess) {
                    minExcess = bucket.ExcessTime;
                    minBucket = &bucket;
                }
            }
        }
        return minBucket;
    }

    virtual EBeginExecuteResult BeginExecute() override
    {
        YCHECK(!CurrentBucket_);

        // Check if any callback is ready at all.
        CurrentBucket_ = GetStarvingBucket();
        if (!CurrentBucket_) {
            return EBeginExecuteResult::QueueEmpty;
        }

        // Reduce excesses (with truncation).
        for (auto& bucket : Buckets_) {
            bucket.ExcessTime = std::max<i64>(0, bucket.ExcessTime - CurrentBucket_->ExcessTime);
        }

        // Pump the starving queue.
        StartInstant_ = GetCpuInstant();
        return CurrentBucket_->Queue->BeginExecute(&CurrentAction_);
    }

    virtual void EndExecute() override
    {
        if (!CurrentBucket_)
            return;

        CurrentBucket_->Queue->EndExecute(&CurrentAction_);
        CurrentBucket_->ExcessTime += (GetCpuInstant() - StartInstant_);
        CurrentBucket_ = nullptr;
    }

};

TFairShareActionQueue::TFairShareActionQueue(
    const Stroka& threadName,
    const std::vector<Stroka>& bucketNames)
    : Impl_(New<TImpl>(threadName, bucketNames))
{ }

TFairShareActionQueue::~TFairShareActionQueue()
{ }

IInvokerPtr TFairShareActionQueue::GetInvoker(int index)
{
    return Impl_->GetInvoker(index);
}

void TFairShareActionQueue::Shutdown()
{
    return Impl_->Shutdown();
}

///////////////////////////////////////////////////////////////////////////////

class TThreadPool::TImpl
    : public TRefCounted
{
public:
    TImpl(int threadCount, const Stroka& threadNamePrefix)
        : Queue_(New<TInvokerQueue>(
            &CallbackEventCount_,
            GetThreadTagIds(threadNamePrefix),
            true,
            true))
    {
        for (int i = 0; i < threadCount; ++i) {
            auto thread = New<TSingleQueueSchedulerThread>(
                Queue_,
                &CallbackEventCount_,
                Format("%v:%v", threadNamePrefix, i),
                GetThreadTagIds(threadNamePrefix),
                true,
                true);
            Threads_.push_back(thread);
            thread->Start();
        }
    }

    ~TImpl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        Queue_->Shutdown();
        for (auto& thread : Threads_) {
            thread->Shutdown();
        }
    }

    IInvokerPtr GetInvoker()
    {
        return Queue_;
    }

private:
    TEventCount CallbackEventCount_;
    TInvokerQueuePtr Queue_;
    std::vector<TSchedulerThreadPtr> Threads_;

};

TThreadPool::TThreadPool(int threadCount, const Stroka& threadNamePrefix)
    : Impl(New<TImpl>(threadCount, threadNamePrefix))
{ }

TThreadPool::~TThreadPool()
{ }

void TThreadPool::Shutdown()
{
    return Impl->Shutdown();
}

IInvokerPtr TThreadPool::GetInvoker()
{
    return Impl->GetInvoker();
}

TCallback<TThreadPoolPtr()> TThreadPool::CreateFactory(int threadCount, const Stroka& threadName)
{
    return BIND([=] () {
        return NYT::New<NConcurrency::TThreadPool>(threadCount, threadName);
    });
}

///////////////////////////////////////////////////////////////////////////////

class TSerializedInvoker
    : public TInvokerWrapper
{
public:
    explicit TSerializedInvoker(IInvokerPtr underlyingInvoker)
        : TInvokerWrapper(std::move(underlyingInvoker))
        , FinishedCallback_(BIND(&TSerializedInvoker::OnFinished, MakeWeak(this)))
    {
        Lock_.clear();
    }

    virtual void Invoke(const TClosure& callback) override
    {
        Queue_.Enqueue(callback);
        TrySchedule();
    }

private:
    TLockFreeQueue<TClosure> Queue_;
    std::atomic_flag Lock_;
    bool LockReleased_;
    TClosure FinishedCallback_;

    static PER_THREAD TSerializedInvoker* CurrentRunningInvoker_;


    class TInvocationGuard
    {
    public:
        explicit TInvocationGuard(TIntrusivePtr<TSerializedInvoker> owner)
            : Owner_(std::move(owner))
        { }

        TInvocationGuard(TInvocationGuard&& other) = default;

        ~TInvocationGuard()
        {
            if (Owner_) {
                Owner_->OnFinished();
            }
        }

    private:
        TIntrusivePtr<TSerializedInvoker> Owner_;

    };

    void TrySchedule()
    {
        if (Queue_.IsEmpty()) {
            return;
        }

        if (!Lock_.test_and_set(std::memory_order_acquire)) {
            UnderlyingInvoker_->Invoke(BIND(
                &TSerializedInvoker::RunCallbacks,
                MakeStrong(this),
                Passed(TInvocationGuard(this))));
        }
    }

    void RunCallbacks(TInvocationGuard /*invocationGuard*/)
    {
        TCurrentInvokerGuard currentInvokerGuard(this);
        TContextSwitchedGuard contextSwitchGuard(FinishedCallback_);

        LockReleased_ = false;

        TClosure callback;
        if (Queue_.Dequeue(&callback)) {
            callback.Run();
        }
    }

    void OnFinished()
    {
        if (!LockReleased_) {
            LockReleased_ = true;
            Lock_.clear(std::memory_order_release);
            TrySchedule();
        }
    }

};

IInvokerPtr CreateSerializedInvoker(IInvokerPtr underlyingInvoker)
{
    return New<TSerializedInvoker>(underlyingInvoker);
}

///////////////////////////////////////////////////////////////////////////////

class TPrioritizedInvoker
    : public TInvokerWrapper
    , public virtual IPrioritizedInvoker
{
public:
    explicit TPrioritizedInvoker(IInvokerPtr underlyingInvoker)
        : TInvokerWrapper(std::move(underlyingInvoker))
    { }

    using TInvokerWrapper::Invoke;

    virtual void Invoke(const TClosure& callback, i64 priority) override
    {
        {
            TGuard<TSpinLock> guard(SpinLock_);
            TEntry entry;
            entry.Callback = callback;
            entry.Priority = priority;
            Heap_.emplace_back(std::move(entry));
            std::push_heap(Heap_.begin(), Heap_.end());
        }
        UnderlyingInvoker_->Invoke(BIND(&TPrioritizedInvoker::DoExecute, MakeStrong(this)));
    }

private:
    struct TEntry
    {
        TClosure Callback;
        i64 Priority;

        bool operator < (const TEntry& other) const
        {
            return Priority < other.Priority;
        }
    };

    TSpinLock SpinLock_;
    std::vector<TEntry> Heap_;

    void DoExecute()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        std::pop_heap(Heap_.begin(), Heap_.end());
        auto callback = std::move(Heap_.back().Callback);
        Heap_.pop_back();
        guard.Release();
        callback.Run();
    }

};

IPrioritizedInvokerPtr CreatePrioritizedInvoker(IInvokerPtr underlyingInvoker)
{
    return New<TPrioritizedInvoker>(std::move(underlyingInvoker));
}

///////////////////////////////////////////////////////////////////////////////

class TFakePrioritizedInvoker
    : public TInvokerWrapper
    , public virtual IPrioritizedInvoker
{
public:
    explicit TFakePrioritizedInvoker(IInvokerPtr underlyingInvoker)
        : TInvokerWrapper(std::move(underlyingInvoker))
    { }

    using TInvokerWrapper::Invoke;

    virtual void Invoke(const TClosure& callback, i64 /*priority*/) override
    {
        return UnderlyingInvoker_->Invoke(callback);
    }
};

IPrioritizedInvokerPtr CreateFakePrioritizedInvoker(IInvokerPtr underlyingInvoker)
{
    return New<TFakePrioritizedInvoker>(std::move(underlyingInvoker));
}

///////////////////////////////////////////////////////////////////////////////

class TBoundedConcurrencyInvoker
    : public TInvokerWrapper
{
public:
    TBoundedConcurrencyInvoker(
        IInvokerPtr underlyingInvoker,
        int maxConcurrentInvocations,
        const NProfiling::TTagIdList& tagIds)
        : TInvokerWrapper(std::move(underlyingInvoker))
        , MaxConcurrentInvocations_(maxConcurrentInvocations)
        , Semaphore_(0)
        , Profiler("/bounded_concurrency_invoker")
        , SemaphoreCounter_("/semaphore", tagIds)
    { }

    virtual void Invoke(const TClosure& callback) override
    {
        Queue_.Enqueue(callback);
        ScheduleMore();
    }

private:
    int MaxConcurrentInvocations_;

    std::atomic<int> Semaphore_;
    TLockFreeQueue<TClosure> Queue_;

    static PER_THREAD TBoundedConcurrencyInvoker* CurrentSchedulingInvoker_;

    NProfiling::TProfiler Profiler;
    NProfiling::TSimpleCounter SemaphoreCounter_;

    class TInvocationGuard
    {
    public:
        explicit TInvocationGuard(TIntrusivePtr<TBoundedConcurrencyInvoker> owner)
            : Owner_(std::move(owner))
        { }

        TInvocationGuard(TInvocationGuard&& other) = default;

        ~TInvocationGuard()
        {
            if (Owner_) {
                Owner_->OnFinished();
            }
        }

    private:
        TIntrusivePtr<TBoundedConcurrencyInvoker> Owner_;

    };


    void RunCallback(TClosure callback, TInvocationGuard /*invocationGuard*/)
    {
        TCurrentInvokerGuard guard(UnderlyingInvoker_); // sic!
        callback.Run();
    }

    void OnFinished()
    {
        ReleaseSemaphore();
        ScheduleMore();
    }

    void ScheduleMore()
    {
        // Prevent reenterant invocations.
        if (CurrentSchedulingInvoker_ == this)
            return;

        while (true) {
            if (!TryAcquireSemaphore())
                break;

            TClosure callback;
            if (!Queue_.Dequeue(&callback)) {
                ReleaseSemaphore();
                break;
            }

            // If UnderlyingInvoker_ is already terminated, Invoke may drop the guard right away.
            // Protect by setting CurrentSchedulingInvoker_ and checking it on entering ScheduleMore.
            CurrentSchedulingInvoker_ = this;

            UnderlyingInvoker_->Invoke(BIND(
                &TBoundedConcurrencyInvoker::RunCallback,
                MakeStrong(this),
                Passed(std::move(callback)),
                Passed(TInvocationGuard(this))));

            // Don't leave a dangling pointer behind.
            CurrentSchedulingInvoker_ = nullptr;
        }        
    }

    bool TryAcquireSemaphore()
    {
        if (++Semaphore_ <= MaxConcurrentInvocations_) {
            Profiler.Increment(SemaphoreCounter_, 1);
            return true;
        } else {
            --Semaphore_;
            return false;
        }
    }

    void ReleaseSemaphore()
    {
        YCHECK(--Semaphore_ >= 0);
        Profiler.Increment(SemaphoreCounter_, -1);
    }
};

PER_THREAD TBoundedConcurrencyInvoker* TBoundedConcurrencyInvoker::CurrentSchedulingInvoker_ = nullptr;

IInvokerPtr CreateBoundedConcurrencyInvoker(
    IInvokerPtr underlyingInvoker,
    int maxConcurrentInvocations,
    const Stroka& invokerName)
{
    return New<TBoundedConcurrencyInvoker>(
        underlyingInvoker,
        maxConcurrentInvocations,
        GetInvokerTagIds(invokerName));
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
