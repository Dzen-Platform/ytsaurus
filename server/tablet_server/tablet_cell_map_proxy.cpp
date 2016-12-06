#include "tablet_cell_map_proxy.h"
#include "tablet_manager.h"
#include "tablet_cell.h"

#include <yt/core/ytree/fluent.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/server/cypress_server/node_proxy_detail.h>

namespace NYT {
namespace NTabletServer {

using namespace NYson;
using namespace NYTree;
using namespace NCypressServer;
using namespace NObjectServer;
using namespace NTransactionServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

class TTabletCellMapProxy
    : public TMapNodeProxy
{
public:
    TTabletCellMapProxy(
        TBootstrap* bootstrap,
        TObjectTypeMetadata* metadata,
        TTransaction* transaction,
        TMapNode* trunkNode)
        : TBase(
            bootstrap,
            metadata,
            transaction,
            trunkNode)
    { }

private:
    typedef TMapNodeProxy TBase;

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override
    {
        TBase::ListSystemAttributes(descriptors);

        descriptors->push_back("count_by_health");
    }

    virtual bool GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        if (key == "count_by_health") {
            const auto& tabletManager = Bootstrap_->GetTabletManager();
            TEnumIndexedVector<int, ETabletCellHealth> counts;
            for (const auto& pair : tabletManager->TabletCells()) {
                const auto* cell = pair.second;
                if (!IsObjectAlive(cell)) {
                    continue;
                }
                ++counts[cell->GetHealth()];
            }
            BuildYsonFluently(consumer)
                .DoMapFor(TEnumTraits<ETabletCellHealth>::GetDomainValues(), [&] (TFluentMap fluent, ETabletCellHealth health) {
                    fluent
                        .Item(FormatEnum(health)).Value(counts[health]);
                });
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }
};

////////////////////////////////////////////////////////////////////////////////

ICypressNodeProxyPtr CreateTabletCellMapProxy(
    TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTransaction* transaction,
    TMapNode* trunkNode)
{
    return New<TTabletCellMapProxy>(
        bootstrap,
        metadata,
        transaction,
        trunkNode);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
