#include "tablet_cell_proxy.h"
#include "private.h"
#include "tablet.h"
#include "tablet_cell.h"
#include "tablet_manager.h"

#include <yt/server/cell_master/bootstrap.h>

#include <yt/server/node_tracker_server/node.h>

#include <yt/server/object_server/object_detail.h>

#include <yt/server/transaction_server/transaction.h>

#include <yt/ytlib/tablet_client/config.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NTabletServer {

using namespace NYTree;
using namespace NYson;
using namespace NRpc;
using namespace NObjectServer;
using namespace NNodeTrackerServer;

////////////////////////////////////////////////////////////////////////////////

class TTabletCellProxy
    : public TNonversionedObjectProxyBase<TTabletCell>
{
public:
    TTabletCellProxy(
        NCellMaster::TBootstrap* bootstrap,
        TObjectTypeMetadata* metadata,
        TTabletCell* cell)
        : TBase(bootstrap, metadata, cell)
    { }

private:
    typedef TNonversionedObjectProxyBase<TTabletCell> TBase;

    virtual void ValidateRemoval() override
    {
        const auto* cell = GetThisImpl();
        if (!cell->Tablets().empty()) {
            THROW_ERROR_EXCEPTION("Cannot remove tablet cell %v since it has %v active tablet(s)",
                cell->GetId(),
                cell->Tablets().size());
        }
    }

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override
    {
        TBase::ListSystemAttributes(descriptors);

        const auto* cell = GetThisImpl();

        descriptors->push_back("leading_peer_id");
        descriptors->push_back("health");
        descriptors->push_back("peers");
        descriptors->push_back(TAttributeDescriptor("tablet_ids")
            .SetOpaque(true));
        descriptors->push_back("tablet_count");
        descriptors->push_back("config_version");
        descriptors->push_back("total_statistics");
        descriptors->push_back(TAttributeDescriptor("prerequisite_transaction_id")
            .SetPresent(cell->GetPrerequisiteTransaction()));
        descriptors->push_back("tablet_cell_bundle");
    }

    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override
    {
        const auto* cell = GetThisImpl();

        if (key == "leading_peer_id") {
            BuildYsonFluently(consumer)
                .Value(cell->GetLeadingPeerId());
            return true;
        }

        if (key == "health") {
            BuildYsonFluently(consumer)
                .Value(cell->GetHealth());
            return true;
        }

        if (key == "peers") {
            BuildYsonFluently(consumer)
                .DoListFor(cell->Peers(), [&] (TFluentList fluent, const TTabletCell::TPeer& peer) {
                    if (peer.Descriptor.IsNull()) {
                        fluent
                            .Item().BeginMap()
                                .Item("state").Value(EPeerState::None)
                            .EndMap();
                    } else {
                        const auto* slot = peer.Node ? peer.Node->GetTabletSlot(cell) : nullptr;
                        auto state = slot ? slot->PeerState : EPeerState::None;
                        fluent
                            .Item().BeginMap()
                                .Item("address").Value(peer.Descriptor.GetDefaultAddress())
                                .Item("state").Value(state)
                                .Item("last_seen_time").Value(peer.LastSeenTime)
                            .EndMap();
                    }
                });
            return true;
        }

        if (key == "tablet_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(cell->Tablets(), [] (TFluentList fluent, const TTablet* tablet) {
                    fluent
                        .Item().Value(tablet->GetId());
                });
            return true;
        }

        if (key == "tablet_count") {
            BuildYsonFluently(consumer)
                .Value(cell->Tablets().size());
            return true;
        }

        if (key == "config_version") {
            BuildYsonFluently(consumer)
                .Value(cell->GetConfigVersion());
            return true;
        }

        if (key == "total_statistics") {
            BuildYsonFluently(consumer)
                .Value(cell->TotalStatistics());
            return true;
        }

        if (key == "prerequisite_transaction_id" && cell->GetPrerequisiteTransaction()) {
            BuildYsonFluently(consumer)
                .Value(cell->GetPrerequisiteTransaction()->GetId());
            return true;
        }

        if (key == "tablet_cell_bundle" && cell->GetCellBundle()) {
            BuildYsonFluently(consumer)
                .Value(cell->GetCellBundle()->GetName());
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

};

IObjectProxyPtr CreateTabletCellProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTabletCell* cell)
{
    return New<TTabletCellProxy>(bootstrap, metadata, cell);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

