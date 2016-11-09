#include "tablet_type_handler.h"
#include "tablet_cell_bundle.h"
#include "tablet_cell_bundle_proxy.h"
#include "tablet_manager.h"

#include <yt/server/object_server/type_handler_detail.h>

#include <yt/ytlib/object_client/helpers.h>

namespace NYT {
namespace NTabletServer {

using namespace NHydra;
using namespace NObjectServer;
using namespace NTransactionServer;
using namespace NSecurityServer;
using namespace NYTree;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

class TTabletCellBundleTypeHandler
    : public TObjectTypeHandlerWithMapBase<TTabletCellBundle>
{
public:
    TTabletCellBundleTypeHandler(
        TBootstrap* bootstrap,
        TEntityMap<TTabletCellBundle>* map)
        : TObjectTypeHandlerWithMapBase(bootstrap, map)
        , Bootstrap_(bootstrap)
    { }

    virtual EObjectType GetType() const override
    {
        return EObjectType::TabletCellBundle;
    }

    virtual ETypeFlags GetFlags() const override
    {
        return
            ETypeFlags::ReplicateCreate |
            ETypeFlags::ReplicateDestroy |
            ETypeFlags::ReplicateAttributes |
            ETypeFlags::Creatable;
    }

    virtual TObjectBase* CreateObject(
        const TObjectId& hintId,
        IAttributeDictionary* attributes) override
    {
        auto name = attributes->GetAndRemove<Stroka>("name");
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        return tabletManager->CreateTabletCellBundle(name, hintId);
    }

private:
    TBootstrap* const Bootstrap_;

    virtual TCellTagList DoGetReplicationCellTags(const TTabletCellBundle* /*cellBundle*/) override
    {
        return AllSecondaryCellTags();
    }

    virtual Stroka DoGetName(const TTabletCellBundle* cellBundle) override
    {
        return Format("tablet cell bundle %Qv", cellBundle->GetName());
    }

    virtual TAccessControlDescriptor* DoFindAcd(TTabletCellBundle* cellBundle) override
    {
        return &cellBundle->Acd();
    }

    virtual IObjectProxyPtr DoGetProxy(TTabletCellBundle* cellBundle, TTransaction* /*transaction*/) override
    {
        return CreateTabletCellBundleProxy(Bootstrap_, &Metadata_, cellBundle);
    }

    virtual void DoDestroyObject(TTabletCellBundle* cellBundle) override
    {
        TObjectTypeHandlerWithMapBase::DoDestroyObject(cellBundle);
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->DestroyTabletCellBundle(cellBundle);
    }
};

IObjectTypeHandlerPtr CreateTabletCellBundleTypeHandler(
    TBootstrap* bootstrap,
    TEntityMap<TTabletCellBundle>* map)
{
    return New<TTabletCellBundleTypeHandler>(bootstrap, map);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
