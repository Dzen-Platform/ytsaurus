#pragma once

#include "public.h"

#include <yt/core/actions/public.h>

#include <yt/core/misc/shutdownable.h>
#include <yt/core/bus/public.h>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

class TDispatcher
    : public IShutdownable
{
public:
    TDispatcher();

    ~TDispatcher();

    static TDispatcher* Get();

    static void StaticShutdown();

    void Configure(const TDispatcherConfigPtr& config);

    virtual void Shutdown() override;

    NYT::NBus::TTosLevel GetTosLevelForBand(EMultiplexingBand band, TNetworkId networkId);

    // Register network names under unique ids.
    TNetworkId GetNetworkId(const TString& networkName);

    //! Returns the invoker for the single thread used to dispatch light callbacks
    //! (e.g. discovery or request cancelation).
    const IInvokerPtr& GetLightInvoker();
    //! Returns the invoker for the thread pool used to dispatch heavy callbacks
    //! (e.g. serialization).
    const IInvokerPtr& GetHeavyInvoker();

    //! Returns the prioritized invoker for the thread pool used to
    //! dispatch compression callbacks.
    const IPrioritizedInvokerPtr& GetPrioritizedCompressionPoolInvoker();
    //! Returns the invoker for the thread pool used to dispatch compression callbacks.
    const IInvokerPtr& GetCompressionPoolInvoker();

private:
    class TImpl;
    const std::unique_ptr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
