#include "tablet_cell_bundle_proxy.h"
#include "tablet_cell_bundle.h"
#include "tablet_cell.h"
#include "tablet_manager.h"
#include "private.h"

#include <yt/core/ytree/fluent.h>

#include <yt/server/object_server/object_detail.h>

#include <yt/server/cell_master/bootstrap.h>

#include <yt/ytlib/tablet_client/config.h>

namespace NYT {
namespace NTabletServer {

using namespace NYTree;
using namespace NYson;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TTabletCellBundleProxy
    : public TNonversionedObjectProxyBase<TTabletCellBundle>
{
public:
    TTabletCellBundleProxy(
        NCellMaster::TBootstrap* bootstrap,
        TObjectTypeMetadata* metadata,
        TTabletCellBundle* cellBundle)
        : TBase(bootstrap, metadata, cellBundle)
    { }

private:
    typedef TNonversionedObjectProxyBase<TTabletCellBundle> TBase;

    virtual void ValidateRemoval() override
    {
        const auto* cellBundle = GetThisTypedImpl();
        if (!cellBundle->TabletCells().empty()) {
            THROW_ERROR_EXCEPTION("Cannot remove tablet cell bundle %Qv since it has %v active tablet cell(s)",
                cellBundle->GetName(),
                cellBundle->TabletCells().size());
        }
    }

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* attributes) override
    {
        attributes->push_back(TAttributeDescriptor("name")
            .SetReplicated(true));
        attributes->push_back(TAttributeDescriptor("options")
            .SetReplicated(true));
        attributes->push_back("tablet_cell_count");
        attributes->push_back(TAttributeDescriptor("tablet_cell_ids")
            .SetOpaque(true));

        TBase::ListSystemAttributes(attributes);
    }

    virtual bool GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        const auto* cellBundle = GetThisTypedImpl();

        if (key == "name") {
            BuildYsonFluently(consumer)
                .Value(cellBundle->GetName());
            return true;
        }

        if (key == "options") {
            BuildYsonFluently(consumer)
                .Value(cellBundle->GetOptions());
            return true;
        }

        if (key == "tablet_cell_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(cellBundle->TabletCells(), [] (TFluentList fluent, const TTabletCell* cell) {
                    fluent
                        .Item().Value(cell->GetId());
                });
            return true;
        }

        if (key == "tablet_cell_count") {
            BuildYsonFluently(consumer)
                .Value(cellBundle->TabletCells().size());
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual bool SetBuiltinAttribute(const Stroka& key, const TYsonString& value) override
    {
        auto tabletManager = Bootstrap_->GetTabletManager();

        auto* cellBundle = GetThisTypedImpl();

        if (key == "name") {
            auto newName = ConvertTo<Stroka>(value);
            tabletManager->RenameTabletCellBundle(cellBundle, newName);
            return true;
        }

        if (key == "options") {
            auto options = ConvertTo<TTabletCellOptionsPtr>(value);
            if (!cellBundle->TabletCells().empty()) {
                THROW_ERROR_EXCEPTION("Cannot change options since tablet cell bundle has %v tablet cell(s)",
                    cellBundle->TabletCells().size());
            }
            cellBundle->SetOptions(options);
            return true;
        }

        return TBase::SetBuiltinAttribute(key, value);
    }
};

IObjectProxyPtr CreateTabletCellBundleProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTabletCellBundle* cellBundle)
{
    return New<TTabletCellBundleProxy>(bootstrap, metadata, cellBundle);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

