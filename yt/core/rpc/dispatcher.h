#pragma once

#include "public.h"

#include <yt/core/actions/public.h>

#include <yt/core/misc/shutdownable.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

class TDispatcher
    : public IShutdownable
{
public:
    TDispatcher();

    ~TDispatcher();

    static TDispatcher* Get();

    static void StaticShutdown();

    void Configure(int poolSize);

    virtual void Shutdown() override;

    /*!
     * This invoker is used by RPC to dispatch callbacks.
     */
    IInvokerPtr GetInvoker();

private:
    class TImpl;
    std::unique_ptr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
