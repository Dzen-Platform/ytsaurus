#include "stdafx.h"
#include "hydra_facade.h"
#include "master_hydra_service.h"
#include "world_initializer.h"
#include "bootstrap.h"

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

TMasterHydraServiceBase::TMasterHydraServiceBase(
    TBootstrap* bootstrap,
    const Stroka& serviceName,
    const NLog::TLogger& logger,
    int version)
    : NHydra::THydraServiceBase(
        bootstrap->GetHydraFacade()->GetHydraManager(),
        bootstrap->GetHydraFacade()->GetGuardedAutomatonInvoker(),
        NRpc::TServiceId(serviceName, bootstrap->GetCellId()),
        logger,
        version)
    , Bootstrap(bootstrap)
{
    YCHECK(Bootstrap);
}

void TMasterHydraServiceBase::BeforeInvoke()
{
    Bootstrap->GetWorldInitializer()->ValidateInitialized();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
