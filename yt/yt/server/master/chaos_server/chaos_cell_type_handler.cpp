#include "chaos_cell.h"
#include "chaos_cell_proxy.h"
#include "chaos_manager.h"

#include <yt/yt/server/master/cell_server/cell_type_handler_base.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/ytree/helpers.h>

namespace NYT::NChaosServer {

using namespace NHydra;
using namespace NObjectServer;
using namespace NObjectClient;
using namespace NTransactionServer;
using namespace NYTree;
using namespace NCellMaster;
using namespace NCellServer;

////////////////////////////////////////////////////////////////////////////////

class TChaosCellTypeHandler
    : public TCellTypeHandlerBase<TChaosCell>
{
public:
    using TCellTypeHandlerBase::TCellTypeHandlerBase;

    EObjectType GetType() const override
    {
        return EObjectType::ChaosCell;
    }

    TObject* CreateObject(
        TObjectId hintId,
        IAttributeDictionary* attributes) override
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        hintId = attributes->Get("chaos_cell_id", NullObjectId);
        auto id = objectManager->GenerateId(EObjectType::ChaosCell, hintId);
        auto holder = TPoolAllocator::New<TChaosCell>(id);
        return DoCreateObject(std::move(holder), attributes);
    }

private:
    using TBase = TCellTypeHandlerBase<TChaosCell>;

    TString DoGetName(const TChaosCell* cell) override
    {
        return Format("chaos cell %v", cell->GetId());
    }

    IObjectProxyPtr DoGetProxy(TChaosCell* cell, TTransaction* /*transaction*/) override
    {
        return CreateChaosCellProxy(Bootstrap_, &Metadata_, cell);
    }
};

////////////////////////////////////////////////////////////////////////////////

IObjectTypeHandlerPtr CreateChaosCellTypeHandler(
    TBootstrap* bootstrap)
{
    return New<TChaosCellTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosServer
