#include "dispatcher.h"

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/misc/lazy_ptr.h>
#include <yt/core/misc/singleton.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

class TDispatcher::TImpl
{
public:
    TImpl()
        : PoolSize_(8)
        , Pool_(BIND(
            NYT::New<NConcurrency::TThreadPool, const int&, const Stroka&>,
            ConstRef(PoolSize_),
            "Rpc"))
    { }

    void Configure(int poolSize)
    {
        if (PoolSize_ == poolSize) {
            return;
        }

        // We believe in proper memory ordering here.
        YCHECK(!Pool_.HasValue());
        PoolSize_ = poolSize;
        // This is not redundant, since the check and the assignment above are
        // not atomic and (adversary) thread can initialize thread pool in parallel.
        YCHECK(!Pool_.HasValue());
    }

    IInvokerPtr GetInvoker()
    {
        return Pool_->GetInvoker();
    }

    void Shutdown()
    {
        if (Pool_.HasValue()) {
            Pool_->Shutdown();
        }
    }

private:
    int PoolSize_;
    TLazyIntrusivePtr<NConcurrency::TThreadPool> Pool_;
};

TDispatcher::TDispatcher()
    : Impl_(new TImpl())
{ }

TDispatcher::~TDispatcher()
{ }

TDispatcher* TDispatcher::Get()
{
    return TSingletonWithFlag<TDispatcher>::Get();
}

void TDispatcher::StaticShutdown()
{
    if (TSingletonWithFlag<TDispatcher>::WasCreated()) {
        TSingletonWithFlag<TDispatcher>::Get()->Shutdown();
    }
}

void TDispatcher::Configure(int poolSize)
{
    Impl_->Configure(poolSize);
}

void TDispatcher::Shutdown()
{
    Impl_->Shutdown();
}

IInvokerPtr TDispatcher::GetInvoker()
{
    return Impl_->GetInvoker();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
