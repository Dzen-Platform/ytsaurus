#pragma once

#include "public.h"

#include <yt/yt/server/lib/hydra/hydra_service.h>

namespace NYT::NCellMaster {

////////////////////////////////////////////////////////////////////////////////

class TMasterHydraServiceBase
    : public NHydra::THydraServiceBase
{
protected:
    TBootstrap* const Bootstrap_;

    TMasterHydraServiceBase(
        TBootstrap* bootstrap,
        const NRpc::TServiceDescriptor& descriptor,
        EAutomatonThreadQueue defaultQueue,
        const NLogging::TLogger& logger);


    IInvokerPtr GetGuardedAutomatonInvoker(EAutomatonThreadQueue queue);
    void ValidateClusterInitialized();

private:
    virtual NHydra::IHydraManagerPtr GetHydraManager() override;
    virtual TFuture<void> DoSyncWithUpstream() override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
