#pragma once

#include "public.h"

#include <yt/core/actions/callback.h>

#include <yt/core/misc/shutdownable.h>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

// XXX(sandello): Facade does not have to be ref-counted.
class TFairShareActionQueue
    : public TRefCounted
    , public IShutdownable
{
public:
    TFairShareActionQueue(
        const Stroka& threadName,
        const std::vector<Stroka>& bucketNames,
        bool enableLogging = true,
        bool enableProfiling = true);

    virtual ~TFairShareActionQueue();

    virtual void Shutdown() override;

    IInvokerPtr GetInvoker(int index);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TFairShareActionQueue)

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
