#include "tablet_type_handler.h"
#include "tablet.h"
#include "tablet_proxy.h"
#include "tablet_manager.h"

#include <yt/server/object_server/type_handler_detail.h>

#include <yt/ytlib/object_client/helpers.h>

namespace NYT {
namespace NTabletServer {

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

    virtual EObjectType GetType() const override
    {
        return EObjectType::Tablet;
    }

private:
    TBootstrap* const Bootstrap_;

    virtual Stroka DoGetName(const TTablet* object) override
    {
        return Format("tablet %v", object->GetId());
    }

    virtual IObjectProxyPtr DoGetProxy(TTablet* tablet, TTransaction* /*transaction*/) override
    {
        return CreateTabletProxy(Bootstrap_, &Metadata_, tablet);
    }

    virtual void DoDestroyObject(TTablet* tablet) override
    {
        TObjectTypeHandlerWithMapBase::DoDestroyObject(tablet);
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->DestroyTablet(tablet);
    }
};

IObjectTypeHandlerPtr CreateTabletTypeHandler(
    TBootstrap* bootstrap,
    TEntityMap<TTablet>* map)
{
    return New<TTabletTypeHandler>(bootstrap, map);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
