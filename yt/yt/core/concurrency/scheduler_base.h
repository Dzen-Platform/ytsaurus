#pragma once

#include "scheduler.h"

#include <yt/core/misc/common.h>
#include <yt/core/misc/shutdownable.h>

#include <util/system/thread.h>
#include <util/system/sigset.h>

#include <yt/yt/library/profiling/tag.h>

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

class TSchedulerThreadBase
    : public virtual TRefCounted
    , public IShutdownable
{
public:
    ~TSchedulerThreadBase();

    void Start();
    virtual void Shutdown() override;

    TThreadId GetId() const;

    bool IsStarted() const;
    bool IsShutdown() const;

protected:
    const std::shared_ptr<TEventCount> CallbackEventCount_;
    const TString ThreadName_;
    const bool EnableLogging_;

    TSchedulerThreadBase(
        std::shared_ptr<TEventCount> callbackEventCount,
        const TString& threadName,
        bool enableLogging);

    virtual void OnStart();
    virtual void BeforeShutdown();
    virtual void AfterShutdown();

    virtual void OnThreadStart();
    virtual void OnThreadShutdown();

    virtual bool OnLoop(TEventCount::TCookie* cookie) = 0;

private:
    std::atomic<ui64> Epoch_ = 0;
    static constexpr ui64 StartingEpochMask = 0x1;
    static constexpr ui64 StoppingEpochMask = 0x2;

    TEvent ThreadStartedEvent_;
    TEvent ThreadShutdownEvent_;

    TThreadId ThreadId_ = InvalidThreadId;
    TThread Thread_;

    static void* ThreadTrampoline(void* opaque);
    void ThreadMain();
};

DEFINE_REFCOUNTED_TYPE(TSchedulerThreadBase)

////////////////////////////////////////////////////////////////////////////////

class TFiberReusingAdapter
    : public TSchedulerThreadBase
{
public:
    TFiberReusingAdapter(
        std::shared_ptr<TEventCount> callbackEventCount,
        const TString& threadName,
        bool enableLogging = true);
    TFiberReusingAdapter(
        std::shared_ptr<TEventCount> callbackEventCount,
        const TString& threadName,
        const NProfiling::TTagSet& tags,
        bool enableLogging = true,
        bool enableProfiling = true);

    void CancelWait();
    void PrepareWait();
    void Wait();

    virtual TClosure BeginExecute() = 0;
    virtual void EndExecute() = 0;

private:
    std::optional<TEventCount::TCookie> Cookie_;

    virtual bool OnLoop(TEventCount::TCookie* cookie) override;
};

/////////////////////////////////////////////////////////////////////////////

// Temporary adapters.
using TSchedulerThread = TFiberReusingAdapter;

DECLARE_REFCOUNTED_TYPE(TSchedulerThread)
DEFINE_REFCOUNTED_TYPE(TSchedulerThread)

////////////////////////////////////////////////////////////////////////////////

} //namespace NYT::NConcurrency
