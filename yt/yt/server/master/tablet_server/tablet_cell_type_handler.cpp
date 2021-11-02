#include "tablet_type_handler.h"
#include "tablet_cell.h"
#include "tablet_cell_proxy.h"
#include "tablet_manager.h"

#include <yt/yt/server/master/cell_server/cell_type_handler_base.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/ytree/helpers.h>

namespace NYT::NTabletServer {

using namespace NHydra;
using namespace NObjectServer;
using namespace NObjectClient;
using namespace NTransactionServer;
using namespace NYTree;
using namespace NCellMaster;
using namespace NCellServer;

////////////////////////////////////////////////////////////////////////////////

class TTabletCellTypeHandler
    : public TCellTypeHandlerBase<TTabletCell>
{
public:
    explicit TTabletCellTypeHandler(TBootstrap* bootstrap)
        : TBase(bootstrap)
    { }

    EObjectType GetType() const override
    {
        return EObjectType::TabletCell;
    }

    TObject* CreateObject(
        TObjectId hintId,
        IAttributeDictionary* attributes) override
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::TabletCell, hintId);
        auto holder = TPoolAllocator::New<TTabletCell>(id);
        holder->GossipStatistics().Initialize(Bootstrap_);
        return DoCreateObject(std::move(holder), attributes);
    }

private:
    using TBase = TCellTypeHandlerBase<TTabletCell>;

    IObjectProxyPtr DoGetProxy(TTabletCell* cell, TTransaction* /*transaction*/) override
    {
        return CreateTabletCellProxy(Bootstrap_, &Metadata_, cell);
    }

    void DoZombifyObject(TTabletCell* cell) override
    {
        const auto& tabletManager = TBase::Bootstrap_->GetTabletManager();
        tabletManager->ZombifyTabletCell(cell);

        TBase::DoZombifyObject(cell);
    }
};

////////////////////////////////////////////////////////////////////////////////

IObjectTypeHandlerPtr CreateTabletCellTypeHandler(TBootstrap* bootstrap)
{
    return New<TTabletCellTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
