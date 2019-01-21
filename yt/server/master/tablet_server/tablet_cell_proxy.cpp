#include "tablet_cell_proxy.h"
#include "private.h"
#include "tablet.h"
#include "tablet_cell.h"
#include "tablet_manager.h"

#include <yt/server/master/cell_master/bootstrap.h>

#include <yt/server/master/node_tracker_server/node.h>

#include <yt/server/lib/misc/interned_attributes.h>

#include <yt/server/master/object_server/object_detail.h>

#include <yt/server/master/transaction_server/transaction.h>

#include <yt/ytlib/tablet_client/config.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/proto/ypath.pb.h>

#include <yt/core/misc/protobuf_helpers.h>

namespace NYT::NTabletServer {

using namespace NConcurrency;
using namespace NNodeTrackerServer;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NRpc;
using namespace NYTree;
using namespace NYson;
using namespace NTabletClient;

using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

TYsonString CombineObjectIds(
    const std::vector<TObjectId>& objectIds,
    const std::vector<std::vector<TObjectId>>& remoteObjectIds)
{
    TString result;
    TStringOutput stringOutput(result);

    auto writer = CreateYsonWriter(
        &stringOutput,
        EYsonFormat::Binary,
        EYsonType::Node,
        /* enableRaw */ false,
        /* booleanAsString */ false);

    BuildYsonFluently(writer.get())
        .BeginList()
        .DoFor(objectIds, [=] (TFluentList fluent, const auto objectId) {
            fluent.Item().Value(objectId);
        })
        .DoFor(remoteObjectIds, [=] (TFluentList fluent, const auto& objectIds) {
            fluent
            .DoFor(objectIds, [=] (TFluentList fluent, const auto objectId) {
                fluent.Item().Value(objectId);
            });
        })
        .EndList();

    writer->Flush();

    return TYsonString(result);
}

} // namespace NDetail

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

        ValidatePermission(cell->GetCellBundle(), EPermission::Write);

        if (!cell->DecommissionCompleted()) {
            THROW_ERROR_EXCEPTION("Cannot remove tablet cell %v since it is not decommissioned on node",
                cell->GetId());
        }

        if (!cell->ClusterStatistics().Decommissioned) {
            THROW_ERROR_EXCEPTION("Cannot remove tablet cell %v since it is not decommissioned on all masters",
                cell->GetId());
        }

        if (cell->ClusterStatistics().TabletCount != 0) {
            THROW_ERROR_EXCEPTION("Cannot remove tablet cell %v since it has active tablet(s)",
                cell->GetId());
        }
    }

    virtual void RemoveSelf(TReqRemove* request, TRspRemove* response, const TCtxRemovePtr& context) override
    {
        auto* cell = GetThisImpl();
        if (cell->DecommissionCompleted()) {
            TBase::RemoveSelf(request, response, context);
        } else {
            ValidatePermission(EPermissionCheckScope::This, EPermission::Remove);

            if (!Bootstrap_->IsPrimaryMaster()) {
                THROW_ERROR_EXCEPTION("Tablet cell is the primary world object and cannot be removed by a secondary master");
            }

            const auto& tabletManager = Bootstrap_->GetTabletManager();
            tabletManager->RemoveTabletCell(cell, request->force());

            context->Reply();
        }
    }

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override
    {
        TBase::ListSystemAttributes(descriptors);

        const auto* cell = GetThisImpl();

        descriptors->push_back(EInternedAttributeKey::LeadingPeerId);
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Health)
            .SetOpaque(true));
        descriptors->push_back(EInternedAttributeKey::Peers);
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::TabletIds)
            .SetOpaque(true));
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::ActionIds)
            .SetOpaque(true));
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::TabletCount)
            .SetOpaque(true));
        descriptors->push_back(EInternedAttributeKey::ConfigVersion);
        descriptors->push_back(EInternedAttributeKey::TotalStatistics);
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::PrerequisiteTransactionId)
            .SetPresent(cell->GetPrerequisiteTransaction()));
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::TabletCellBundle)
            .SetReplicated(true)
            .SetMandatory(true));
        descriptors->push_back(EInternedAttributeKey::TabletCellLifeStage);
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::MulticellStatistics)
            .SetOpaque(true));
    }

    virtual bool GetBuiltinAttribute(TInternedAttributeKey key, NYson::IYsonConsumer* consumer) override
    {
        const auto* cell = GetThisImpl();

        const auto& chunkManager = Bootstrap_->GetChunkManager();

        switch (key) {
            case EInternedAttributeKey::LeadingPeerId:
                BuildYsonFluently(consumer)
                    .Value(cell->GetLeadingPeerId());
                return true;

            case EInternedAttributeKey::Health:
                if (Bootstrap_->IsMulticell()) {
                    BuildYsonFluently(consumer)
                        .Value(cell->GetMulticellHealth());
                } else {
                    BuildYsonFluently(consumer)
                        .Value(cell->GetHealth());
                }
                return true;

            case EInternedAttributeKey::Peers:
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

            case EInternedAttributeKey::TabletIds:
                if (!Bootstrap_->IsPrimaryMaster()) {
                    BuildYsonFluently(consumer)
                        .DoListFor(cell->Tablets(), [] (TFluentList fluent, const TTablet* tablet) {
                            fluent
                                .Item().Value(tablet->GetId());
                        });
                    return true;
                }
                break;

            case EInternedAttributeKey::ActionIds:
                if (!Bootstrap_->IsPrimaryMaster()) {
                    BuildYsonFluently(consumer)
                        .DoListFor(cell->Actions(), [] (TFluentList fluent, const TTabletAction* action) {
                            fluent
                                .Item().Value(action->GetId());
                        });
                    return true;
                }
                break;

            case EInternedAttributeKey::TabletCount: {
                if (!Bootstrap_->IsPrimaryMaster()) {
                    BuildYsonFluently(consumer)
                        .Value(cell->Tablets().size());
                    return true;
                }
                break;
            }

            case EInternedAttributeKey::ConfigVersion:
                BuildYsonFluently(consumer)
                    .Value(cell->GetConfigVersion());
                return true;

            case EInternedAttributeKey::TotalStatistics:
                BuildYsonFluently(consumer)
                    .Value(New<TSerializableTabletCellStatistics>(
                        cell->ClusterStatistics(),
                        chunkManager));
                return true;

            case EInternedAttributeKey::MulticellStatistics:
                BuildYsonFluently(consumer)
                    .DoMapFor(cell->MulticellStatistics(), [&] (TFluentMap fluent, const auto& pair) {
                        auto serializableStatistics = New<TSerializableTabletCellStatistics>(
                            pair.second,
                            chunkManager);
                        fluent.Item(ToString(pair.first)).Value(serializableStatistics);
                    });
                return true;

            case EInternedAttributeKey::PrerequisiteTransactionId:
                if (!cell->GetPrerequisiteTransaction()) {
                    break;
                }
                BuildYsonFluently(consumer)
                    .Value(cell->GetPrerequisiteTransaction()->GetId());
                return true;

            case EInternedAttributeKey::TabletCellBundle:
                if (!cell->GetCellBundle()) {
                    break;
                }
                BuildYsonFluently(consumer)
                    .Value(cell->GetCellBundle()->GetName());
                return true;

            case EInternedAttributeKey::TabletCellLifeStage:
                BuildYsonFluently(consumer)
                    .Value(cell->GetTabletCellLifeStage());
                return true;

            default:
                break;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual TFuture<NYson::TYsonString> GetBuiltinAttributeAsync(NYTree::TInternedAttributeKey key) override
    {
        const auto* cell = GetThisImpl();

        switch (key) {
            case EInternedAttributeKey::TabletCount: {
                YCHECK(Bootstrap_->IsPrimaryMaster());

                int tabletCount = cell->Tablets().size();
                return FetchFromSwarm<int>(key)
                    .Apply(BIND([tabletCount] (const std::vector<int>& tabletCounts) {
                        auto totalTabletCount = std::accumulate(
                            tabletCounts.begin(),
                            tabletCounts.end(),
                            tabletCount);
                        return ConvertToYsonString(totalTabletCount);
                    })
                    .AsyncVia(NRpc::TDispatcher::Get()->GetHeavyInvoker()));
            }

            case EInternedAttributeKey::TabletIds: {
                YCHECK(Bootstrap_->IsPrimaryMaster());

                std::vector<TTabletId> tabletIds;
                for (const auto* tablet : cell->Tablets()) {
                    tabletIds.push_back(tablet->GetId());
                }

                return FetchFromSwarm<std::vector<TTabletId>>(key)
                    .Apply(BIND([tabletIds = std::move(tabletIds)] (const std::vector<std::vector<TTabletId>>& remoteTabletIds) {
                        return NDetail::CombineObjectIds(tabletIds, remoteTabletIds);
                    })
                    .AsyncVia(NRpc::TDispatcher::Get()->GetHeavyInvoker()));
            }

            case EInternedAttributeKey::ActionIds: {
                YCHECK(Bootstrap_->IsPrimaryMaster());

                std::vector<TTabletActionId> actionIds;
                for (const auto* action : cell->Actions()) {
                    actionIds.push_back(action->GetId());
                }

                return FetchFromSwarm<std::vector<TTabletActionId>>(key)
                    .Apply(BIND([actionIds = std::move(actionIds)] (const std::vector<std::vector<TTabletActionId>>& remoteActionIds) {
                        return NDetail::CombineObjectIds(actionIds, remoteActionIds);
                    })
                    .AsyncVia(NRpc::TDispatcher::Get()->GetHeavyInvoker()));
            }

            default:
                break;
        }

        return TBase::GetBuiltinAttributeAsync(key);
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

} // namespace NYT::NTabletServer

