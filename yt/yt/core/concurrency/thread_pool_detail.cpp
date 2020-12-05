#include "thread_pool_detail.h"

#include <yt/core/misc/numeric_helpers.h>

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

TThreadPoolBase::TThreadPoolBase(
    int threadCount,
    const TString& threadNamePrefix,
    bool enableLogging,
    bool enableProfiling)
    : ThreadNamePrefix_(threadNamePrefix)
    , EnableLogging_(enableLogging)
    , EnableProfiling_(enableProfiling)
{ }

void TThreadPoolBase::Configure(int threadCount)
{
    DoConfigure(Clamp(threadCount, 1, MaxThreadCount));
}

void TThreadPoolBase::Shutdown()
{
    if (!ShutdownFlag_.exchange(true)) {
        StartFlag_ = true;
        DoShutdown();
    }
}

void TThreadPoolBase::EnsureStarted()
{
    if (!StartFlag_.exchange(true)) {
        DoStart();
    }
}

TString TThreadPoolBase::MakeThreadName(int index)
{
    return Format("%v:%v", ThreadNamePrefix_, index);
}

void TThreadPoolBase::DoStart()
{
    decltype(Threads_) threads;
    {
        auto guard = Guard(SpinLock_);
        threads = Threads_;
    }

    for (const auto& thread : threads) {
        thread->Start();
    }
}

void TThreadPoolBase::DoShutdown()
{
    FinalizerInvoker_->Invoke(MakeFinalizerCallback());
    FinalizerInvoker_.Reset();
}

TClosure TThreadPoolBase::MakeFinalizerCallback()
{
    decltype(Threads_) threads;
    {
        auto guard = Guard(SpinLock_);
        std::swap(threads, Threads_);
    }

    return BIND([threads = std::move(threads)] () {
        for (const auto& thread : threads) {
            thread->Shutdown();
        }
    });
}

void TThreadPoolBase::DoConfigure(int threadCount)
{
    decltype(Threads_) threadsToStart;
    decltype(Threads_) threadsToShutdown;
    {
        auto guard = Guard(SpinLock_);

        while (static_cast<int>(Threads_.size()) < threadCount) {
            auto thread = SpawnThread(static_cast<int>(Threads_.size()));
            threadsToStart.push_back(thread);
            Threads_.push_back(thread);
        }

        while (static_cast<int>(Threads_.size()) > threadCount) {
            threadsToShutdown.push_back(Threads_.back());
            Threads_.pop_back();
        }
    }

    for (const auto& thread : threadsToShutdown) {
        thread->Shutdown();
    }

    // New threads will be started eventually due to EnsureStarted call.
    StartFlag_.store(false);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency

