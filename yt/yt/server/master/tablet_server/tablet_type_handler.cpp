#include "tablet_type_handler.h"
#include "tablet.h"
#include "tablet_proxy.h"
#include "tablet_manager.h"

#include <yt/yt/server/master/object_server/type_handler_detail.h>

#include <yt/yt/client/object_client/helpers.h>

namespace NYT::NTabletServer {

using namespace NHydra;
using namespace NObjectServer;
using namespace NTransactionServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

class TTabletTypeHandler
    : public TObjectTypeHandlerWithMapBase<TTablet>
{
public:
    TTabletTypeHandler(
        TBootstrap* bootstrap,
        TEntityMap<TTablet>* map)
        : TObjectTypeHandlerWithMapBase(bootstrap, map)
        , Bootstrap_(bootstrap)
    { }

    EObjectType GetType() const override
    {
        return EObjectType::Tablet;
    }

private:
    TBootstrap* const Bootstrap_;

    IObjectProxyPtr DoGetProxy(TTablet* tablet, TTransaction* /*transaction*/) override
    {
        return CreateTabletProxy(Bootstrap_, &Metadata_, tablet);
    }

    void DoDestroyObject(TTablet* tablet) noexcept override
    {
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->DestroyTablet(tablet);

        TObjectTypeHandlerWithMapBase::DoDestroyObject(tablet);
    }
};

IObjectTypeHandlerPtr CreateTabletTypeHandler(
    TBootstrap* bootstrap,
    TEntityMap<TTablet>* map)
{
    return New<TTabletTypeHandler>(bootstrap, map);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
