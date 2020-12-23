#pragma once

#include "public.h"

#include <yt/core/actions/callback.h>

#include <yt/core/misc/shutdownable.h>

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

// XXX(sandello): Facade does not have to be ref-counted.
class TThreadPool
    : public TRefCounted
    , public IShutdownable
{
public:
    TThreadPool(
        int threadCount,
        const TString& threadNamePrefix);

    virtual ~TThreadPool();

    virtual void Shutdown() override;

    void Configure(int threadCount);

    const IInvokerPtr& GetInvoker();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TThreadPool)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency
