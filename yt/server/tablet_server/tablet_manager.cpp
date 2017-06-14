#include "config.h"
#include "cypress_integration.h"
#include "private.h"
#include "table_replica.h"
#include "table_replica_type_handler.h"
#include "tablet.h"
#include "tablet_cell.h"
#include "tablet_cell_bundle.h"
#include "tablet_cell_bundle_type_handler.h"
#include "tablet_cell_type_handler.h"
#include "tablet_balancer.h"
#include "tablet_manager.h"
#include "tablet_tracker.h"
#include "tablet_type_handler.h"

#include <yt/server/cell_master/config.h>
#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>
#include <yt/server/cell_master/serialize.h>

#include <yt/server/chunk_server/helpers.h>
#include <yt/server/chunk_server/chunk_list.h>
#include <yt/server/chunk_server/chunk_manager.h>
#include <yt/server/chunk_server/chunk_tree_traverser.h>

#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/hive/hive_manager.h>
#include <yt/server/hive/helpers.h>

#include <yt/server/node_tracker_server/node.h>
#include <yt/server/node_tracker_server/node_tracker.h>

#include <yt/server/object_server/object_manager.h>

#include <yt/server/security_server/security_manager.h>
#include <yt/server/security_server/group.h>
#include <yt/server/security_server/subject.h>

#include <yt/server/table_server/table_node.h>
#include <yt/server/table_server/replicated_table_node.h>

#include <yt/server/tablet_node/config.h>
#include <yt/server/tablet_node/tablet_manager.pb.h>

#include <yt/server/tablet_server/tablet_manager.pb.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/config.h>

#include <yt/ytlib/election/config.h>

#include <yt/ytlib/hive/cell_directory.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/schema.h>
#include <yt/ytlib/table_client/helpers.h>

#include <yt/ytlib/tablet_client/config.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/misc/address.h>
#include <yt/core/misc/collection_helpers.h>
#include <yt/core/misc/string.h>

#include <util/random/random.h>

#include <algorithm>

namespace NYT {
namespace NTabletServer {

using namespace NConcurrency;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NObjectServer;
using namespace NYTree;
using namespace NSecurityServer;
using namespace NTableServer;
using namespace NTabletClient;
using namespace NTabletClient::NProto;
using namespace NHydra;
using namespace NHiveClient;
using namespace NHiveServer;
using namespace NTransactionServer;
using namespace NTabletServer::NProto;
using namespace NNodeTrackerServer;
using namespace NNodeTrackerServer::NProto;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NTabletNode::NProto;
using namespace NChunkServer;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NCypressServer;
using namespace NCypressClient;
using namespace NCellMaster;
using namespace NYson;

using NTabletNode::TTableMountConfigPtr;
using NTabletNode::EInMemoryMode;
using NTabletNode::EStoreType;
using NNodeTrackerServer::NProto::TReqIncrementalHeartbeat;
using NNodeTrackerClient::TNodeDescriptor;
using NTransactionServer::TTransaction;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletServerLogger;
static const auto CleanupPeriod = TDuration::Seconds(10);

////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////

class TTabletManager::TImpl
    : public TMasterAutomatonPart
{
public:
    explicit TImpl(
        TTabletManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap)
        , Config_(config)
        , TabletTracker_(New<TTabletTracker>(Config_, Bootstrap_))
        , TabletBalancer_(New<TTabletBalancer>(Config_->TabletBalancer, Bootstrap_))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(), AutomatonThread);

        RegisterLoader(
            "TabletManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "TabletManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "TabletManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "TabletManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));

        auto cellTag = Bootstrap_->GetPrimaryCellTag();
        DefaultTabletCellBundleId_ = MakeWellKnownId(EObjectType::TabletCellBundle, cellTag, 0xffffffffffffffff);

        RegisterMethod(BIND(&TImpl::HydraAssignPeers, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRevokePeers, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSetLeadingPeer, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletMounted, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletUnmounted, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletFrozen, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletUnfrozen, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdateTableReplicaStatistics, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTableReplicaDisabled, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdateTabletTrimmedRowCount, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraCreateTabletAction, Unretained(this)));

        if (Bootstrap_->IsPrimaryMaster()) {
            const auto& nodeTracker = Bootstrap_->GetNodeTracker();
            nodeTracker->SubscribeNodeRegistered(BIND(&TImpl::OnNodeRegistered, MakeWeak(this)));
            nodeTracker->SubscribeNodeUnregistered(BIND(&TImpl::OnNodeUnregistered, MakeWeak(this)));
            nodeTracker->SubscribeIncrementalHeartbeat(BIND(&TImpl::OnIncrementalHeartbeat, MakeWeak(this)));
        }
    }

    void Initialize()
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(CreateTabletCellBundleTypeHandler(Bootstrap_, &TabletCellBundleMap_));
        objectManager->RegisterHandler(CreateTabletCellTypeHandler(Bootstrap_, &TabletCellMap_));
        objectManager->RegisterHandler(CreateTabletTypeHandler(Bootstrap_, &TabletMap_));
        objectManager->RegisterHandler(CreateTableReplicaTypeHandler(Bootstrap_, &TableReplicaMap_));
        objectManager->RegisterHandler(CreateTabletActionTypeHandler(Bootstrap_, &TabletActionMap_));

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->SubscribeTransactionCommitted(BIND(&TImpl::OnTransactionFinished, MakeWeak(this)));
        transactionManager->SubscribeTransactionAborted(BIND(&TImpl::OnTransactionFinished, MakeWeak(this)));
        transactionManager->RegisterPrepareActionHandler(MakeTransactionActionHandlerDescriptor(BIND(&TImpl::HydraPrepareUpdateTabletStores, MakeStrong(this))));
        transactionManager->RegisterCommitActionHandler(MakeTransactionActionHandlerDescriptor(BIND(&TImpl::HydraCommitUpdateTabletStores, MakeStrong(this))));
        transactionManager->RegisterAbortActionHandler(MakeTransactionActionHandlerDescriptor(BIND(&TImpl::HydraAbortUpdateTabletStores, MakeStrong(this))));

        if (Bootstrap_->IsPrimaryMaster()) {
            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            multicellManager->SubscribeReplicateKeysToSecondaryMaster(
                BIND(&TImpl::OnReplicateKeysToSecondaryMaster, MakeWeak(this)));
            multicellManager->SubscribeReplicateValuesToSecondaryMaster(
                BIND(&TImpl::OnReplicateValuesToSecondaryMaster, MakeWeak(this)));
        }
    }

    TTabletCellBundle* CreateTabletCellBundle(const TString& name, const TObjectId& hintId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        ValidateTabletCellBundleName(name);

        if (FindTabletCellBundleByName(name)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Tablet cell bundle %Qv already exists",
                name);
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::TabletCellBundle, hintId);
        return DoCreateTabletCellBundle(id, name);
    }

    TTabletCellBundle* DoCreateTabletCellBundle(const TTabletCellBundleId& id, const TString& name)
    {
        auto cellBundleHolder = std::make_unique<TTabletCellBundle>(id);
        cellBundleHolder->SetName(name);

        auto* cellBundle = TabletCellBundleMap_.Insert(id, std::move(cellBundleHolder));
        YCHECK(NameToTabletCellBundleMap_.insert(std::make_pair(cellBundle->GetName(), cellBundle)).second);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(cellBundle);

        return cellBundle;
    }

    void DestroyTabletCellBundle(TTabletCellBundle* cellBundle)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // Remove tablet cell bundle from maps.
        YCHECK(NameToTabletCellBundleMap_.erase(cellBundle->GetName()) == 1);
    }

    TTabletCell* CreateTabletCell(TTabletCellBundle* cellBundle, const TObjectId& hintId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(cellBundle, EPermission::Use);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::TabletCell, hintId);
        auto cellHolder = std::make_unique<TTabletCell>(id);

        cellHolder->Peers().resize(cellBundle->GetOptions()->PeerCount);
        cellHolder->SetCellBundle(cellBundle);
        YCHECK(cellBundle->TabletCells().insert(cellHolder.get()).second);
        objectManager->RefObject(cellBundle);

        ReconfigureCell(cellHolder.get());

        auto* cell = TabletCellMap_.Insert(id, std::move(cellHolder));

        // Make the fake reference.
        YCHECK(cell->RefObject() == 1);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        hiveManager->CreateMailbox(id);

        auto cellMapNodeProxy = GetCellMapNode();
        auto cellNodePath = "/" + ToString(id);

        try {
            // NB: Users typically are not allowed to create these types.
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            auto* rootUser = securityManager->GetRootUser();
            TAuthenticatedUserGuard userGuard(securityManager, rootUser);

            // Create Cypress node.
            {
                auto req = TCypressYPathProxy::Create(cellNodePath);
                req->set_type(static_cast<int>(EObjectType::TabletCellNode));

                auto attributes = CreateEphemeralAttributes();
                attributes->Set("opaque", true);
                ToProto(req->mutable_node_attributes(), *attributes);

                SyncExecuteVerb(cellMapNodeProxy, req);
            }

            // Create "snapshots" child.
            {
                auto req = TCypressYPathProxy::Create(cellNodePath + "/snapshots");
                req->set_type(static_cast<int>(EObjectType::MapNode));

                SyncExecuteVerb(cellMapNodeProxy, req);
            }

            // Create "changelogs" child.
            {
                auto req = TCypressYPathProxy::Create(cellNodePath + "/changelogs");
                req->set_type(static_cast<int>(EObjectType::MapNode));

                SyncExecuteVerb(cellMapNodeProxy, req);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error registering tablet cell in Cypress");
        }

        return cell;
    }

    void DestroyTabletCell(TTabletCell* cell)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto actions = cell->Actions();
        for (auto* action : actions) {
            // NB: If destination cell disappears, don't drop action - let it continue with some other cells.
            UnbindTabletActionFromCells(action);
            OnTabletActionDisturbed(action, TError("Tablet cell %v has been removed", cell->GetId()));
        }
        YCHECK(cell->Actions().empty());

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        const auto& cellId = cell->GetId();
        auto* mailbox = hiveManager->FindMailbox(cellId);
        if (mailbox) {
            hiveManager->RemoveMailbox(mailbox);
        }

        for (const auto& peer : cell->Peers()) {
            if (peer.Node) {
                peer.Node->DetachTabletCell(cell);
            }
            if (!peer.Descriptor.IsNull()) {
                RemoveFromAddressToCellMap(peer.Descriptor, cell);
            }
        }
        cell->Peers().clear();

        auto* cellBundle = cell->GetCellBundle();
        YCHECK(cellBundle->TabletCells().erase(cell) == 1);
        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->UnrefObject(cellBundle);
        cell->SetCellBundle(nullptr);

        // NB: Code below interacts with other master parts and may require root permissions
        // (for example, when aborting a transaction).
        // We want this code to always succeed.
        auto securityManager = Bootstrap_->GetSecurityManager();
        auto* rootUser = securityManager->GetRootUser();
        TAuthenticatedUserGuard userGuard(securityManager, rootUser);

        AbortPrerequisiteTransaction(cell);
        AbortCellSubtreeTransactions(cell);

        auto cellNodeProxy = FindCellNode(cellId);
        if (cellNodeProxy) {
            try {
                // NB: Subtree transactions were already aborted in AbortPrerequisiteTransaction.
                cellNodeProxy->GetParent()->RemoveChild(cellNodeProxy);
            } catch (const std::exception& ex) {
                LOG_ERROR_UNLESS(IsRecovery(), ex, "Error unregisterting tablet cell from Cypress");
            }
        }
    }


    TTablet* CreateTablet(TTableNode* table)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::Tablet, NullObjectId);
        auto tabletHolder = std::make_unique<TTablet>(id);
        tabletHolder->SetTable(table);

        auto* tablet = TabletMap_.Insert(id, std::move(tabletHolder));
        objectManager->RefObject(tablet);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet created (TableId: %v, TabletId: %v)",
            table->GetId(),
            tablet->GetId());

        return tablet;
    }

    void DestroyTablet(TTablet* tablet)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        YCHECK(!tablet->GetCell());

        if (auto* action = tablet->GetAction()) {
            OnTabletActionTabletsTouched(
                action,
                yhash_set<TTablet*>{tablet},
                TError("Tablet %v has been removed", tablet->GetId()));
        }
    }


    TTableReplica* CreateTableReplica(
        TReplicatedTableNode* table,
        const TString& clusterName,
        const TYPath& replicaPath,
        ETableReplicaMode mode,
        TTimestamp startReplicationTimestamp)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        for (const auto* replica : table->Replicas()) {
            if (replica->GetClusterName() == clusterName &&
                replica->GetReplicaPath() == replicaPath)
            {
                THROW_ERROR_EXCEPTION("Replica table %v at cluster %Qv already exists",
                    replicaPath,
                    clusterName);
            }
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::TableReplica, NullObjectId);
        auto replicaHolder = std::make_unique<TTableReplica>(id);
        replicaHolder->SetTable(table);
        replicaHolder->SetClusterName(clusterName);
        replicaHolder->SetReplicaPath(replicaPath);
        replicaHolder->SetMode(mode);
        replicaHolder->SetStartReplicationTimestamp(startReplicationTimestamp);
        replicaHolder->SetState(ETableReplicaState::Disabled);

        auto* replica = TableReplicaMap_.Insert(id, std::move(replicaHolder));
        objectManager->RefObject(replica);

        YCHECK(table->Replicas().insert(replica).second);

        LOG_DEBUG_UNLESS(IsRecovery(), "Table replica created (TableId: %v, ReplicaId: %v, Mode: %v, StartReplicationTimestamp: %llx)",
            table->GetId(),
            replica->GetId(),
            mode,
            startReplicationTimestamp);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        for (auto* tablet : table->Tablets()) {
            auto pair = tablet->Replicas().emplace(replica, TTableReplicaInfo());
            YCHECK(pair.second);
            auto& replicaInfo = pair.first->second;

            if (!tablet->IsActive()) {
                replicaInfo.SetState(ETableReplicaState::None);
                continue;
            }

            replicaInfo.SetState(ETableReplicaState::Disabled);

            auto* cell = tablet->GetCell();
            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            TReqAddTableReplica req;
            ToProto(req.mutable_tablet_id(), tablet->GetId());
            PopulateTableReplicaDescriptor(req.mutable_replica(), replica, replicaInfo);
            hiveManager->PostMessage(mailbox, req);
        }

        return replica;

    }

    void DestroyTableReplica(TTableReplica* replica)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* table = replica->GetTable();
        if (table) {
            YCHECK(table->Replicas().erase(replica) == 1);

            const auto& hiveManager = Bootstrap_->GetHiveManager();
            for (auto* tablet : table->Tablets()) {
                YCHECK(tablet->Replicas().erase(replica) == 1);

                if (!tablet->IsActive()) {
                    continue;
                }

                auto* cell = tablet->GetCell();
                auto* mailbox = hiveManager->GetMailbox(cell->GetId());
                TReqRemoveTableReplica req;
                ToProto(req.mutable_tablet_id(), tablet->GetId());
                ToProto(req.mutable_replica_id(), replica->GetId());
                hiveManager->PostMessage(mailbox, req);
            }
        }
    }

    void SetTableReplicaEnabled(TTableReplica* replica, bool enabled)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto state = replica->GetState();
        if (enabled) {
            if (state == ETableReplicaState::Enabled) {
                return;
            }
            if (state != ETableReplicaState::Disabled) {
                replica->ThrowInvalidState();
            }
        } else {
            if (state == ETableReplicaState::Disabled || state == ETableReplicaState::Disabling) {
                return;
            }
            if (state != ETableReplicaState::Enabled) {
                replica->ThrowInvalidState();
            }
        }

        auto* table = replica->GetTable();

        if (enabled) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Table replica enabled (TableId: %v, ReplicaId: %v)",
                table->GetId(),
                replica->GetId());

            replica->SetState(ETableReplicaState::Enabled);
        } else {
            for (auto* tablet : table->Tablets()) {
                if (tablet->GetState() == ETabletState::Unmounting) {
                    THROW_ERROR_EXCEPTION("Cannot disable replica since tablet %v is in %Qlv state",
                        tablet->GetId(),
                        tablet->GetState());
                }
            }

            LOG_DEBUG_UNLESS(IsRecovery(), "Disabling table replica (TableId: %v, ReplicaId: %v)",
                table->GetId(),
                replica->GetId());

            replica->SetState(ETableReplicaState::Disabling);
        }

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        for (auto* tablet : table->Tablets()) {
            if (!tablet->IsActive()) {
                continue;
            }

            auto* replicaInfo = tablet->GetReplicaInfo(replica);

            if (enabled) {
                replicaInfo->SetState(ETableReplicaState::Enabled);
            } else {
                replicaInfo->SetState(ETableReplicaState::Disabling);
                YCHECK(replica->DisablingTablets().insert(tablet).second);
            }

            auto* cell = tablet->GetCell();
            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            TReqSetTableReplicaEnabled req;
            ToProto(req.mutable_tablet_id(), tablet->GetId());
            ToProto(req.mutable_replica_id(), replica->GetId());
            req.set_enabled(enabled);
            hiveManager->PostMessage(mailbox, req);
        }

        CheckForReplicaDisabled(replica);
    }

    void SetTableReplicaMode(TTableReplica* replica, ETableReplicaMode mode)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (replica->GetMode() == mode) {
            return;
        }

        auto* table = replica->GetTable();

        LOG_DEBUG_UNLESS(IsRecovery(), "Table replica mode updated (TableId: %v, ReplicaId: %v, Mode: %v)",
            table->GetId(),
            replica->GetId(),
            mode);

        replica->SetMode(mode);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        for (auto* tablet : table->Tablets()) {
            if (!tablet->IsActive()) {
                continue;
            }

            auto* cell = tablet->GetCell();
            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            TReqSetTableReplicaMode req;
            ToProto(req.mutable_tablet_id(), tablet->GetId());
            ToProto(req.mutable_replica_id(), replica->GetId());
            req.set_mode(static_cast<int>(mode));
            hiveManager->PostMessage(mailbox, req);
        }
    }


    TTabletAction* CreateTabletAction(
        const TObjectId& hintId,
        ETabletActionKind kind,
        const std::vector<TTablet*>& tablets,
        const std::vector<TTabletCell*>& cells,
        const std::vector<NTableClient::TOwningKey>& pivotKeys,
        const TNullable<int>& tabletCount,
        bool skipFreezing,
        TNullable<bool> freeze,
        bool keepFinished)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (tablets.empty()) {
            THROW_ERROR_EXCEPTION("Invalud number of tablets: expected more than zero");
        }

        for (const auto* tablet : tablets) {
            if (auto* action = tablet->GetAction()) {
                THROW_ERROR_EXCEPTION("Tablet %v already participating in action %v",
                    tablet->GetId(),
                    action->GetId());
            }
            if (tablet->GetState() != ETabletState::Mounted && tablet->GetState() != ETabletState::Frozen) {
                THROW_ERROR_EXCEPTION("Tablet %v is in state %Qlv",
                    tablet->GetId(),
                    tablet->GetState());
            }
        }

        if (!freeze) {
            auto state = tablets[0]->GetState();
            for (const auto* tablet : tablets) {
                if (tablet->GetState() != state) {
                    THROW_ERROR_EXCEPTION("Tablets are in mixed state");
                }
            }
            freeze = state == ETabletState::Frozen;
        }

        switch (kind) {
            case ETabletActionKind::Move:
                if (cells.size() != 0 && cells.size() != tablets.size()) {
                    THROW_ERROR_EXCEPTION("Number of destination cells and tablets mismatch: %v tablets, %v cells",
                        cells.size());
                }
                if (pivotKeys.size() != 0) {
                    THROW_ERROR_EXCEPTION("Invalid number of pivot keys: expected 0, actual %v",
                        pivotKeys.size());
                }
                if (!!tabletCount) {
                    THROW_ERROR_EXCEPTION("Invalid number of tablets: expected Null, actual %v",
                        *tabletCount);
                }
                break;

            case ETabletActionKind::Reshard:
                if (cells.size() != 0 && cells.size() != pivotKeys.size()) {
                    THROW_ERROR_EXCEPTION("Number of destination cells and pivot keys mismatch: pivot keys %v, cells %",
                        cells.size());
                }
                if (pivotKeys.size() == 0 && (!tabletCount || *tabletCount < 1)) {
                    THROW_ERROR_EXCEPTION("Invalid number of new tablets: expected pivot keys or tablet count greater than 1");
                }
                for (int index = 1; index < tablets.size(); ++index) {
                    const auto& cur = tablets[index];
                    const auto& prev = tablets[index - 1];
                    if (cur->GetTable() != prev->GetTable()) {
                        THROW_ERROR_EXCEPTION("Tablets %v and %v belong to different tables",
                            prev->GetId(),
                            cur->GetId());
                    }
                    if (cur->GetIndex() != prev->GetIndex() + 1) {
                        THROW_ERROR_EXCEPTION("Tablets %v and %v are not consequent",
                            prev->GetId(),
                            cur->GetId());
                    }
                }
                break;

            default:
                Y_UNREACHABLE();
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::TabletAction, hintId);
        auto actionHolder = std::make_unique<TTabletAction>(id);
        auto* action = TabletActionMap_.Insert(id, std::move(actionHolder));
        objectManager->RefObject(action);

        for (auto* tablet : tablets) {
            tablet->SetAction(action);
        }
        for (auto* cell : cells) {
            cell->Actions().insert(action);
        }

        action->SetKind(kind);
        action->SetState(ETabletActionState::Preparing);
        action->Tablets() = std::move(tablets);
        action->TabletCells() = std::move(cells);
        action->PivotKeys() = std::move(pivotKeys);
        action->SetTabletCount(tabletCount);
        action->SetSkipFreezing(skipFreezing);
        action->SetFreeze(*freeze);
        action->SetKeepFinished(keepFinished);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet action created (%v)",
            *action);

        OnTabletActionStateChanged(action);

        return action;
    }

    void UnbindTabletActionFromCells(TTabletAction* action)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        for (auto* cell : action->TabletCells()) {
            cell->Actions().erase(action);
        }

        action->TabletCells().clear();
    }

    void UnbindTabletActionFromTablets(TTabletAction* action)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        for (auto* tablet : action->Tablets()) {
            YCHECK(tablet->GetAction() == action);
            tablet->SetAction(nullptr);
        }

        action->Tablets().clear();
    }

    void UnbindTabletAction(TTabletAction* action)
    {
        UnbindTabletActionFromTablets(action);
        UnbindTabletActionFromCells(action);
    }

    void DestroyTabletAction(TTabletAction* action)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        UnbindTabletAction(action);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet action destroyed (ActionId: %v)",
            action->GetId());
    }

    std::vector<TOwningKey> CalculatePivotKeys(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount)
    {
        struct TEntry
        {
            TOwningKey MinKey;
            TOwningKey MaxKey;
            i64 Size;

            bool operator<(const TEntry& other) const
            {
                return MinKey < other.MinKey;
            }
        };

        std::vector<TEntry> entries;
        i64 totalSize = 0;

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            for (const auto* chunkList : table->GetChunkList()->Children()[index]->AsChunkList()->Children()) {
                const auto* chunk = chunkList->AsChunk();
                if (chunk->MiscExt().eden()) {
                    continue;
                }

                auto boundaryKeysExt = GetProtoExtension<TBoundaryKeysExt>(chunk->ChunkMeta().extensions());
                i64 size = chunk->MiscExt().uncompressed_data_size();
                entries.push_back({
                    FromProto<TOwningKey>(boundaryKeysExt.min()),
                    FromProto<TOwningKey>(boundaryKeysExt.max()),
                    size});
                totalSize += size;
            }
        }

        std::sort(entries.begin(), entries.end());

        i64 desired = totalSize / newTabletCount;
        std::vector<TOwningKey> pivotKeys{table->Tablets()[firstTabletIndex]->GetPivotKey()};
        TOwningKey lastKey;
        i64 current = 0;

        for (const auto& entry : entries) {
            if (lastKey && lastKey < entry.MinKey) {
                if (current >= desired) {
                    current = 0;
                    pivotKeys.push_back(entry.MinKey);
                    lastKey = entry.MaxKey;
                }
            } else if (entry.MaxKey > lastKey) {
                lastKey = entry.MaxKey;
            }
            current += entry.Size;
        }

        return pivotKeys;
    }

    void MountMissedInActionTablets(TTabletAction* action)
    {
        for (auto* tablet : action->Tablets()) {
            try {
                if (!IsObjectAlive(tablet)) {
                    continue;
                }

                switch (tablet->GetState()) {
                    case ETabletState::Mounted:
                        break;

                    case ETabletState::Unmounted:
                        DoMountTablet(tablet, nullptr, action->GetFreeze());
                        break;

                    case ETabletState::Frozen:
                        if (!action->GetFreeze()) {
                            DoUnfreezeTablet(tablet);
                        }
                        break;

                    default:
                        THROW_ERROR_EXCEPTION("Tablet %v is in unrecognized state %Qv",
                            tablet->GetId(),
                            tablet->GetState());
                }
            } catch (const std::exception& ex) {
                LOG_ERROR_UNLESS(IsRecovery(), ex, "Error mounting missed in action tablet (TabletId: %v, ActionId: %v)",
                    tablet->GetId(),
                    action->GetId());
            }
        }
    }

    void OnTabletActionTabletsTouched(
        TTabletAction* action,
        const yhash_set<TTablet*>& touchedTablets,
        const TError& error)
    {
        bool touched = false;
        for (auto* tablet : action->Tablets()) {
            if (touchedTablets.find(tablet) != touchedTablets.end()) {
                YCHECK(tablet->GetAction() == action);
                tablet->SetAction(nullptr);
                touched = true;
            }
        }

        if (!touched) {
            return;
        }

        auto& tablets = action->Tablets();
        tablets.erase(
            std::remove_if(
                tablets.begin(),
                tablets.end(),
                [&] (auto* tablet) {
                    return touchedTablets.find(tablet) != touchedTablets.end();
                }),
            tablets.end());

        UnbindTabletActionFromCells(action);
        OnTabletActionDisturbed(action, error);
    }

    void TouchAffectedTabletActions(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        const TString& request)
    {
        YCHECK(firstTabletIndex >= 0 && firstTabletIndex <= lastTabletIndex && lastTabletIndex < table->Tablets().size());

        auto error = TError("User request %Qv interfered with the action", request);
        yhash_set<TTablet*> touchedTablets;
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            touchedTablets.insert(table->Tablets()[index]);
        }
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            if (auto* action = table->Tablets()[index]->GetAction()) {
                OnTabletActionTabletsTouched(action, touchedTablets, error);
            }
        }
    }

    void ChangeTabletActionState(TTabletAction* action, ETabletActionState state, bool recursive = true)
    {
        action->SetState(state);
        LOG_DEBUG_UNLESS(IsRecovery(), "Change tablet action state (ActionId: %v, State: %Qv)",
            action->GetId(),
            state);
        if (recursive) {
            OnTabletActionStateChanged(action);
        }
    }

    void OnTabletActionDisturbed(TTabletAction* action, const TError& error)
    {
        if (action->Tablets().empty()) {
            action->Error() = error.Sanitize();
            ChangeTabletActionState(action, ETabletActionState::Failed);
            return;
        }

        switch (action->GetState()) {
            case ETabletActionState::Unmounting:
            case ETabletActionState::Freezing:
                // Wait until tablets are unmounted, then mount them.
                action->Error() = error.Sanitize();
                break;

            case ETabletActionState::Mounting:
                // Nothing can be done here.
                action->Error() = error.Sanitize();
                ChangeTabletActionState(action, ETabletActionState::Failed);
                break;

            case ETabletActionState::Completed:
            case ETabletActionState::Failed:
                // All tablets have been already taken care of. Do nothing.
                break;

            case ETabletActionState::Mounted:
            case ETabletActionState::Frozen:
            case ETabletActionState::Unmounted:
            case ETabletActionState::Preparing:
            case ETabletActionState::Failing:
                // Transient states inside mutation. Nothing wrong should happen here.
                Y_UNREACHABLE();

            default:
                Y_UNREACHABLE();
        }
    }

    void OnTabletActionStateChanged(TTabletAction* action)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (!action) {
            return;
        }

        bool repeat;
        do {
            repeat = false;
            try {
                DoTabletActionStateChanged(action);
            } catch (const std::exception& ex) {
                YCHECK(action->GetState() != ETabletActionState::Failing);
                action->Error() = TError(ex).Sanitize();
                if (action->GetState() != ETabletActionState::Unmounting) {
                    ChangeTabletActionState(action, ETabletActionState::Failing, false);
                }
                repeat = true;
            }
        } while (repeat);
    }

    void DoTabletActionStateChanged(TTabletAction* action)
    {
        switch (action->GetState()) {
            case ETabletActionState::Preparing: {
                if (action->GetSkipFreezing()) {
                    ChangeTabletActionState(action, ETabletActionState::Frozen);
                    break;
                }

                for (auto* tablet : action->Tablets()) {
                    DoFreezeTablet(tablet);
                }

                ChangeTabletActionState(action, ETabletActionState::Freezing);
                break;
            }

            case ETabletActionState::Freezing: {
                int freezingCount = 0;
                for (const auto* tablet : action->Tablets()) {
                    YCHECK(IsObjectAlive(tablet));
                    if (tablet->GetState() == ETabletState::Freezing) {
                        ++freezingCount;
                    }
                }
                if (freezingCount == 0) {
                    auto state = action->Error().IsOK()
                        ? ETabletActionState::Frozen
                        : ETabletActionState::Failing;
                    ChangeTabletActionState(action, state);
                }
                break;
            }

            case ETabletActionState::Frozen: {
                for (auto* tablet : action->Tablets()) {
                    YCHECK(IsObjectAlive(tablet));
                    DoUnmountTablet(tablet, false);
                }

                ChangeTabletActionState(action, ETabletActionState::Unmounting);
                break;
            }

            case ETabletActionState::Unmounting: {
                int unmountingCount = 0;
                for (const auto* tablet : action->Tablets()) {
                    YCHECK(IsObjectAlive(tablet));
                    if (tablet->GetState() == ETabletState::Unmounting) {
                        ++unmountingCount;
                    }
                }
                if (unmountingCount == 0) {
                    auto state = action->Error().IsOK()
                        ? ETabletActionState::Unmounted
                        : ETabletActionState::Failing;
                    ChangeTabletActionState(action, state);
                }
                break;
            }

            case ETabletActionState::Unmounted: {
                switch (action->GetKind()) {
                    case ETabletActionKind::Move: {
                        for (int index = 0; index < action->Tablets().size(); ++index) {
                            if (!IsObjectAlive(action->Tablets()[index]->GetTable())) {
                                THROW_ERROR_EXCEPTION("Table is not alive");
                            }
                            DoMountTablet(
                                action->Tablets()[index],
                                action->TabletCells().empty()
                                    ? nullptr
                                    : action->TabletCells()[index],
                                action->GetFreeze());
                        }
                        break;
                    }

                    case ETabletActionKind::Reshard: {
                        auto* table = action->Tablets().front()->GetTable();
                        if (!IsObjectAlive(table)) {
                            THROW_ERROR_EXCEPTION("Table is not alive");
                        }

                        int firstTabletIndex = action->Tablets().front()->GetIndex();
                        int lastTabletIndex = action->Tablets().back()->GetIndex();

                        std::vector<TOwningKey> pivotKeys;
                        int newTabletCount;

                        if (table->IsPhysicallySorted()) {
                            if (auto tabletCount = action->GetTabletCount()) {
                                pivotKeys = CalculatePivotKeys(table, firstTabletIndex, lastTabletIndex, *tabletCount);
                            } else {
                                pivotKeys = action->PivotKeys();
                            }
                            newTabletCount = pivotKeys.size();
                        } else {
                            newTabletCount = *action->GetTabletCount();
                        }

                        std::vector<TTablet*> oldTablets;
                        oldTablets.swap(action->Tablets());
                        for (auto* tablet : oldTablets) {
                            tablet->SetAction(nullptr);
                        }

                        try {
                            ReshardTable(
                                table,
                                firstTabletIndex,
                                lastTabletIndex,
                                newTabletCount,
                                pivotKeys);
                        } catch (const std::exception& ex) {
                            for (auto* tablet : oldTablets) {
                                YCHECK(IsObjectAlive(tablet));
                                tablet->SetAction(action);
                            }
                            action->Tablets() = std::move(oldTablets);
                            throw;
                        }

                        action->Tablets() = std::vector<TTablet*>(
                            table->Tablets().begin() + firstTabletIndex,
                            table->Tablets().begin() + firstTabletIndex + pivotKeys.size());
                        for (auto* tablet : action->Tablets()) {
                            tablet->SetAction(action);
                        }

                        TTableMountConfigPtr mountConfig;
                        NTabletNode::TTabletChunkReaderConfigPtr readerConfig;
                        NTabletNode::TTabletChunkWriterConfigPtr writerConfig;
                        NTabletNode::TTabletWriterOptionsPtr writerOptions;
                        GetTableSettings(table, &mountConfig, &readerConfig, &writerConfig, &writerOptions);
                        auto serializedMountConfig = ConvertToYsonString(mountConfig);
                        auto serializedReaderConfig = ConvertToYsonString(readerConfig);
                        auto serializedWriterConfig = ConvertToYsonString(writerConfig);
                        auto serializedWriterOptions = ConvertToYsonString(writerOptions);

                        std::vector<std::pair<TTablet*, TTabletCell*>> assignment;
                        if (action->TabletCells().empty()) {
                            assignment = ComputeTabletAssignment(
                                table,
                                mountConfig,
                                nullptr,
                                action->Tablets());
                        } else {
                            for (int index = 0; index < action->Tablets().size(); ++index) {
                                assignment.emplace_back(
                                    action->Tablets()[index],
                                    action->TabletCells()[index]);
                            }
                        }

                        DoMountTablets(
                            assignment,
                            mountConfig->InMemoryMode,
                            action->GetFreeze(),
                            serializedMountConfig,
                            serializedReaderConfig,
                            serializedWriterConfig,
                            serializedWriterOptions);

                        break;
                    }

                    default:
                        Y_UNREACHABLE();
                }

                ChangeTabletActionState(action, ETabletActionState::Mounting);
                break;
            }

            case ETabletActionState::Mounting: {
                int mountedCount = 0;
                for (const auto* tablet : action->Tablets()) {
                    YCHECK(IsObjectAlive(tablet));
                    if (tablet->GetState() == ETabletState::Mounted ||
                        tablet->GetState() == ETabletState::Frozen)
                    {
                        ++mountedCount;
                    }
                }

                if (mountedCount == action->Tablets().size()) {
                    ChangeTabletActionState(action, ETabletActionState::Mounted);
                }
                break;
            }

            case ETabletActionState::Mounted: {
                ChangeTabletActionState(action, ETabletActionState::Completed);
                break;
            }

            case ETabletActionState::Failing: {
                LOG_DEBUG_UNLESS(IsRecovery(), action->Error(), "Tablet action failed (ActionId: %v)",
                    action->GetId());

                MountMissedInActionTablets(action);
                UnbindTabletAction(action);
                ChangeTabletActionState(action, ETabletActionState::Failed);
                break;
            }

            case ETabletActionState::Completed:
                if (!action->Error().IsOK()) {
                    ChangeTabletActionState(action, ETabletActionState::Failed, false);
                }
                // No break intentionaly.
            case ETabletActionState::Failed: {
                if (!action->GetKeepFinished()) {
                    const auto& objectManager = Bootstrap_->GetObjectManager();
                    objectManager->UnrefObject(action);
                }
                break;
            }

            default:
                Y_UNREACHABLE();
        }
    }

    int GetAssignedTabletCellCount(const TString& address) const
    {
        auto range = AddressToCell_.equal_range(address);
        return std::distance(range.first, range.second);
    }

    TTabletStatistics GetTabletStatistics(const TTablet* tablet)
    {
        const auto* table = tablet->GetTable();
        const auto* tabletChunkList = tablet->GetChunkList();
        const auto& treeStatistics = tabletChunkList->Statistics();
        const auto& nodeStatistics = tablet->NodeStatistics();

        TTabletStatistics tabletStatistics;
        tabletStatistics.PartitionCount = nodeStatistics.partition_count();
        tabletStatistics.StoreCount = nodeStatistics.store_count();
        tabletStatistics.PreloadPendingStoreCount = nodeStatistics.preload_pending_store_count();
        tabletStatistics.PreloadCompletedStoreCount = nodeStatistics.preload_completed_store_count();
        tabletStatistics.PreloadFailedStoreCount = nodeStatistics.preload_failed_store_count();
        tabletStatistics.OverlappingStoreCount = nodeStatistics.overlapping_store_count();
        tabletStatistics.UnmergedRowCount = treeStatistics.RowCount;
        tabletStatistics.UncompressedDataSize = treeStatistics.UncompressedDataSize;
        tabletStatistics.CompressedDataSize = treeStatistics.CompressedDataSize;
        switch (tablet->GetInMemoryMode()) {
            case EInMemoryMode::Compressed:
                tabletStatistics.MemorySize = tabletStatistics.CompressedDataSize;
                break;
            case EInMemoryMode::Uncompressed:
                tabletStatistics.MemorySize = tabletStatistics.UncompressedDataSize;
                break;
            case EInMemoryMode::None:
                tabletStatistics.MemorySize = 0;
                break;
            default:
                Y_UNREACHABLE();
        }
        for (int mediumIndex = 0; mediumIndex < NChunkClient::MaxMediumCount; ++mediumIndex) {
            tabletStatistics.DiskSpace[mediumIndex] = CalculateDiskSpaceUsage(
                table->Properties()[mediumIndex].GetReplicationFactor(),
                treeStatistics.RegularDiskSpace,
                treeStatistics.ErasureDiskSpace);
        }
        tabletStatistics.ChunkCount = treeStatistics.ChunkCount;
        tabletStatistics.TabletCountPerMemoryMode[tablet->GetInMemoryMode()] = 1;
        return tabletStatistics;
    }


    void MountTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        TTabletCell* hintCell,
        bool freeze)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot mount a static table");
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw

        if (hintCell && hintCell->GetCellBundle() != table->GetTabletCellBundle()) {
            // Will throw :)
            THROW_ERROR_EXCEPTION("Cannot mount tablets into cell %v since it belongs to bundle %Qv while the table "
                "is configured to use bundle %Qv",
                hintCell->GetId(),
                hintCell->GetCellBundle()->GetName(),
                table->GetTabletCellBundle()->GetName());
        }

        if (!hintCell) {
            ValidateHasHealthyCells(table->GetTabletCellBundle()); // may throw
        }

        const auto& allTablets = table->Tablets();

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            const auto* tablet = allTablets[index];
            auto state = tablet->GetState();
            if (state != ETabletState::Unmounted && (freeze
                    ? state != ETabletState::Frozen &&
                        state != ETabletState::Freezing &&
                        state != ETabletState::FrozenMounting
                    : state != ETabletState::Mounted &&
                        state != ETabletState::Mounting &&
                        state != ETabletState::Unfreezing))
            {
                THROW_ERROR_EXCEPTION("Tablet %v is in %Qlv state",
                    tablet->GetId(),
                    state);
            }
        }

        TTableMountConfigPtr mountConfig;
        NTabletNode::TTabletChunkReaderConfigPtr readerConfig;
        NTabletNode::TTabletChunkWriterConfigPtr writerConfig;
        NTabletNode::TTabletWriterOptionsPtr writerOptions;
        GetTableSettings(table, &mountConfig, &readerConfig, &writerConfig, &writerOptions);
        ValidateTableMountConfig(table, mountConfig);
        ValidateTabletStaticMemoryUpdate(
            table,
            firstTabletIndex,
            lastTabletIndex,
            mountConfig,
            false);

        if (mountConfig->InMemoryMode != EInMemoryMode::None &&
            writerOptions->ErasureCodec != NErasure::ECodec::None)
        {
            THROW_ERROR_EXCEPTION("Cannot mount erasure coded table in memory");
        }

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "reshard_table");

        auto serializedMountConfig = ConvertToYsonString(mountConfig);
        auto serializedReaderConfig = ConvertToYsonString(readerConfig);
        auto serializedWriterConfig = ConvertToYsonString(writerConfig);
        auto serializedWriterOptions = ConvertToYsonString(writerOptions);

        std::vector<TTablet*> tabletsToMount;
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = allTablets[index];
            if (!tablet->GetCell()) {
                tabletsToMount.push_back(tablet);
            }
        }

        auto assignment = ComputeTabletAssignment(
            table,
            mountConfig,
            hintCell,
            std::move(tabletsToMount));

        DoMountTablets(
            assignment,
            mountConfig->InMemoryMode,
            freeze,
            serializedMountConfig,
            serializedReaderConfig,
            serializedWriterConfig,
            serializedWriterOptions);

        CommitTabletStaticMemoryUpdate(table);
    }

    void DoMountTablet(
        TTablet* tablet,
        TTabletCell* cell,
        bool freeze)
    {
        auto* table = tablet->GetTable();
        TTableMountConfigPtr mountConfig;
        NTabletNode::TTabletChunkReaderConfigPtr readerConfig;
        NTabletNode::TTabletChunkWriterConfigPtr writerConfig;
        NTabletNode::TTabletWriterOptionsPtr writerOptions;
        GetTableSettings(table, &mountConfig, &readerConfig, &writerConfig, &writerOptions);

        auto serializedMountConfig = ConvertToYsonString(mountConfig);
        auto serializedReaderConfig = ConvertToYsonString(readerConfig);
        auto serializedWriterConfig = ConvertToYsonString(writerConfig);
        auto serializedWriterOptions = ConvertToYsonString(writerOptions);

        auto assignment = ComputeTabletAssignment(
            table,
            mountConfig,
            cell,
            std::vector<TTablet*>{tablet});

        DoMountTablets(
            assignment,
            mountConfig->InMemoryMode,
            freeze,
            serializedMountConfig,
            serializedReaderConfig,
            serializedWriterConfig,
            serializedWriterOptions);
    }

    void DoMountTablets(
        const std::vector<std::pair<TTablet*, TTabletCell*>>& assignment,
        EInMemoryMode inMemoryMode,
        bool freeze,
        const TYsonString& serializedMountConfig,
        const TYsonString& serializedReaderConfig,
        const TYsonString& serializedWriterConfig,
        const TYsonString& serializedWriterOptions)
    {
        for (const auto& pair : assignment) {
            auto* tablet = pair.first;
            auto* cell = pair.second;
            const auto& objectManager = Bootstrap_->GetObjectManager();
            int tabletIndex = tablet->GetIndex();
            auto* table = tablet->GetTable();
            const auto& allTablets = table->Tablets();
            const auto& chunkLists = table->GetChunkList()->Children();
            YCHECK(allTablets.size() == chunkLists.size());

            tablet->SetCell(cell);
            YCHECK(cell->Tablets().insert(tablet).second);
            objectManager->RefObject(cell);

            YCHECK(tablet->GetState() == ETabletState::Unmounted);
            tablet->SetState(freeze ? ETabletState::FrozenMounting : ETabletState::Mounting);
            tablet->SetInMemoryMode(inMemoryMode);

            const auto* context = GetCurrentMutationContext();
            tablet->SetMountRevision(context->GetVersion().ToRevision());

            const auto& hiveManager = Bootstrap_->GetHiveManager();
            auto* mailbox = hiveManager->GetMailbox(cell->GetId());

            {
                TReqMountTablet req;
                ToProto(req.mutable_tablet_id(), tablet->GetId());
                req.set_mount_revision(tablet->GetMountRevision());
                ToProto(req.mutable_table_id(), table->GetId());
                ToProto(req.mutable_schema(), table->TableSchema());
                if (table->IsPhysicallySorted()) {
                    ToProto(req.mutable_pivot_key(), tablet->GetPivotKey());
                    ToProto(req.mutable_next_pivot_key(), tablet->GetIndex() + 1 == allTablets.size()
                        ? MaxKey()
                        : allTablets[tabletIndex + 1]->GetPivotKey());
                } else {
                    req.set_trimmed_row_count(tablet->GetTrimmedRowCount());
                }
                req.set_mount_config(serializedMountConfig.GetData());
                req.set_reader_config(serializedReaderConfig.GetData());
                req.set_writer_config(serializedWriterConfig.GetData());
                req.set_writer_options(serializedWriterOptions.GetData());
                req.set_atomicity(static_cast<int>(table->GetAtomicity()));
                req.set_commit_ordering(static_cast<int>(table->GetCommitOrdering()));
                req.set_freeze(freeze);
                ToProto(req.mutable_upstream_replica_id(), table->GetUpstreamReplicaId());
                if (table->IsReplicated()) {
                    auto* replicatedTable = table->As<TReplicatedTableNode>();
                    for (auto* replica : replicatedTable->Replicas()) {
                        const auto* replicaInfo = tablet->GetReplicaInfo(replica);
                        PopulateTableReplicaDescriptor(req.add_replicas(), replica, *replicaInfo);
                    }
                }

                auto* chunkList = chunkLists[tabletIndex]->AsChunkList();
                const auto& chunkListStatistics = chunkList->Statistics();
                auto chunks = EnumerateChunksInChunkTree(chunkList);
                auto storeType = table->IsPhysicallySorted() ? EStoreType::SortedChunk : EStoreType::OrderedChunk;
                i64 startingRowIndex = chunkListStatistics.LogicalRowCount - chunkListStatistics.RowCount;
                for (const auto* chunk : chunks) {
                    auto* descriptor = req.add_stores();
                    descriptor->set_store_type(static_cast<int>(storeType));
                    ToProto(descriptor->mutable_store_id(), chunk->GetId());
                    descriptor->mutable_chunk_meta()->CopyFrom(chunk->ChunkMeta());
                    descriptor->set_starting_row_index(startingRowIndex);
                    startingRowIndex += chunk->MiscExt().row_count();
                }

                LOG_DEBUG_UNLESS(IsRecovery(), "Mounting tablet (TableId: %v, TabletId: %v, CellId: %v, ChunkCount: %v, "
                    "Atomicity: %v, CommitOrdering: %v, Freeze: %v, UpstreamReplicaId: %v)",
                    table->GetId(),
                    tablet->GetId(),
                    cell->GetId(),
                    chunks.size(),
                    table->GetAtomicity(),
                    table->GetCommitOrdering(),
                    freeze,
                    table->GetUpstreamReplicaId());

                hiveManager->PostMessage(mailbox, req);
            }

            for (auto& pair  : tablet->Replicas()) {
                auto* replica = pair.first;
                auto& replicaInfo = pair.second;
                if (replica->GetState() != ETableReplicaState::Enabled) {
                    replicaInfo.SetState(ETableReplicaState::Disabled);
                    continue;
                }

                TReqSetTableReplicaEnabled req;
                ToProto(req.mutable_tablet_id(), tablet->GetId());
                ToProto(req.mutable_replica_id(), replica->GetId());
                req.set_enabled(true);
                hiveManager->PostMessage(mailbox, req);

                replicaInfo.SetState(ETableReplicaState::Enabled);
            }
        }
    }

    void UnmountTable(
        TTableNode* table,
        bool force,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot unmount a static table");
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw

        if (!force) {
            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* tablet = table->Tablets()[index];
                auto state = tablet->GetState();
                if (state != ETabletState::Mounted &&
                    state != ETabletState::Frozen &&
                    state != ETabletState::Freezing &&
                    state != ETabletState::Unmounted &&
                    state != ETabletState::Unmounting)
                {
                    THROW_ERROR_EXCEPTION("Tablet %v is in %Qlv state",
                        tablet->GetId(),
                        state);
                }
            }
        }

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "reshard_table");

        DoUnmountTable(table, force, firstTabletIndex, lastTabletIndex);
    }

    void RemountTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot remount a static table");
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw

        TTableMountConfigPtr mountConfig;
        NTabletNode::TTabletChunkReaderConfigPtr readerConfig;
        NTabletNode::TTabletChunkWriterConfigPtr writerConfig;
        NTabletNode::TTabletWriterOptionsPtr writerOptions;
        GetTableSettings(table, &mountConfig, &readerConfig, &writerConfig, &writerOptions);
        ValidateTableMountConfig(table, mountConfig);
        ValidateTabletStaticMemoryUpdate(
            table,
            firstTabletIndex,
            lastTabletIndex,
            mountConfig,
            true);

        if (mountConfig->InMemoryMode != EInMemoryMode::None &&
            writerOptions->ErasureCodec != NErasure::ECodec::None)
        {
            THROW_ERROR_EXCEPTION("Cannot mount erasure coded table in memory");
        }

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "reshard_table");

        auto serializedMountConfig = ConvertToYsonString(mountConfig);
        auto serializedReaderConfig = ConvertToYsonString(readerConfig);
        auto serializedWriterConfig = ConvertToYsonString(writerConfig);
        auto serializedWriterOptions = ConvertToYsonString(writerOptions);

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            auto* cell = tablet->GetCell();
            auto state = tablet->GetState();

            if (state == ETabletState::Mounted ||
                state == ETabletState::Mounting ||
                state == ETabletState::FrozenMounting ||
                state == ETabletState::Frozen ||
                state == ETabletState::Freezing)
            {
                LOG_DEBUG_UNLESS(IsRecovery(), "Remounting tablet (TableId: %v, TabletId: %v, CellId: %v)",
                    table->GetId(),
                    tablet->GetId(),
                    cell->GetId());

                cell->TotalStatistics() -= GetTabletStatistics(tablet);
                tablet->SetInMemoryMode(mountConfig->InMemoryMode);
                cell->TotalStatistics() += GetTabletStatistics(tablet);

                const auto& hiveManager = Bootstrap_->GetHiveManager();

                TReqRemountTablet request;
                request.set_mount_config(serializedMountConfig.GetData());
                request.set_reader_config(serializedReaderConfig.GetData());
                request.set_writer_config(serializedWriterConfig.GetData());
                request.set_writer_options(serializedWriterOptions.GetData());
                ToProto(request.mutable_tablet_id(), tablet->GetId());

                auto* mailbox = hiveManager->GetMailbox(cell->GetId());
                hiveManager->PostMessage(mailbox, request);
            }
        }

        CommitTabletStaticMemoryUpdate(table);
    }

    void FreezeTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot freeze a static table");
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            auto state = tablet->GetState();
            if (state != ETabletState::Mounted &&
                state != ETabletState::FrozenMounting &&
                state != ETabletState::Freezing &&
                state != ETabletState::Frozen)
            {
                THROW_ERROR_EXCEPTION("Tablet %v is in %Qlv state",
                    tablet->GetId(),
                    state);
            }
        }

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "reshard_table");

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            DoFreezeTablet(tablet);
        }
    }

    void DoFreezeTablet(TTablet* tablet)
    {
        const auto& hiveManager = Bootstrap_->GetHiveManager();
        auto* cell = tablet->GetCell();
        auto state = tablet->GetState();
        YCHECK(state == ETabletState::Mounted ||
            state == ETabletState::FrozenMounting ||
            state == ETabletState::Frozen ||
            state == ETabletState::Freezing);

        if (tablet->GetState() == ETabletState::Mounted) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Freezing tablet (TableId: %v, TabletId: %v, CellId: %v)",
                tablet->GetTable()->GetId(),
                tablet->GetId(),
                cell->GetId());

            tablet->SetState(ETabletState::Freezing);

            TReqFreezeTablet request;
            ToProto(request.mutable_tablet_id(), tablet->GetId());

            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            hiveManager->PostMessage(mailbox, request);
        }
    }

    void UnfreezeTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot unfreeze a static table");
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            auto state = tablet->GetState();
            if (state != ETabletState::Mounted &&
                state != ETabletState::Frozen &&
                state != ETabletState::Unfreezing)
            {
                THROW_ERROR_EXCEPTION("Tablet %v is in %Qlv state",
                    tablet->GetId(),
                    state);
            }
        }

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "reshard_table");

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            DoUnfreezeTablet(tablet);
        }
    }

    void DoUnfreezeTablet(TTablet* tablet)
    {
        const auto& hiveManager = Bootstrap_->GetHiveManager();
        auto* cell = tablet->GetCell();
        auto state = tablet->GetState();
        YCHECK(state == ETabletState::Mounted ||
            state == ETabletState::Frozen ||
            state == ETabletState::Unfreezing);

        if (tablet->GetState() == ETabletState::Frozen) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Unfreezing tablet (TableId: %v, TabletId: %v, CellId: %v)",
                tablet->GetTable()->GetId(),
                tablet->GetId(),
                cell->GetId());

            tablet->SetState(ETabletState::Unfreezing);

            TReqUnfreezeTablet request;
            ToProto(request.mutable_tablet_id(), tablet->GetId());

            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            hiveManager->PostMessage(mailbox, request);
        }
    }

    void DestroyTable(TTableNode* table)
    {
        if (table->GetTabletCellBundle()) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->UnrefObject(table->GetTabletCellBundle());
            table->SetTabletCellBundle(nullptr);
        }

        if (!table->Tablets().empty()) {
            int firstTabletIndex = 0;
            int lastTabletIndex =  static_cast<int>(table->Tablets().size()) - 1;

            TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "remove");

            DoUnmountTable(table, true, firstTabletIndex, lastTabletIndex);

            const auto& objectManager = Bootstrap_->GetObjectManager();
            for (auto* tablet : table->Tablets()) {
                tablet->SetTable(nullptr);
                YCHECK(tablet->GetState() == ETabletState::Unmounted);
                objectManager->UnrefObject(tablet);
            }

            table->Tablets().clear();
        }

        if (table->GetType() == EObjectType::ReplicatedTable) {
            auto* replicatedTable = table->As<TReplicatedTableNode>();
            const auto& objectManager = Bootstrap_->GetObjectManager();
            for (auto* replica : replicatedTable->Replicas()) {
                replica->SetTable(nullptr);
                replica->DisablingTablets().clear();
                objectManager->UnrefObject(replica);
            }
            replicatedTable->Replicas().clear();
        }
    }

    void ReshardTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount,
        const std::vector<TOwningKey>& pivotKeys)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot reshard a static table");
        }

        if (table->IsReplicated() && !table->IsEmpty()) {
            THROW_ERROR_EXCEPTION("Cannot reshard non-empty replicated table");
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw

        auto& tablets = table->Tablets();
        YCHECK(tablets.size() == table->GetChunkList()->Children().size());

        if (newTabletCount <= 0) {
            THROW_ERROR_EXCEPTION("Tablet count must be positive");
        }

        int oldTabletCount = lastTabletIndex - firstTabletIndex + 1;

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidateResourceUsageIncrease(
            table->GetAccount(),
            TClusterResources().SetTabletCount(newTabletCount - oldTabletCount));

        if (tablets.size() - oldTabletCount + newTabletCount > MaxTabletCount) {
            THROW_ERROR_EXCEPTION("Tablet count cannot exceed the limit of %v",
                MaxTabletCount);
        }

        if (table->IsSorted()) {
            if (pivotKeys.empty()) {
                THROW_ERROR_EXCEPTION("Table is sorted; must provide pivot keys");
            }

            if (pivotKeys.size() != newTabletCount) {
                THROW_ERROR_EXCEPTION("Wrong pivot key count: %v instead of %v",
                    pivotKeys.size(),
                    newTabletCount);
            }

            if (pivotKeys[0] != tablets[firstTabletIndex]->GetPivotKey()) {
                THROW_ERROR_EXCEPTION(
                    "First pivot key must match that of the first tablet "
                        "in the resharded range");
            }

            if (lastTabletIndex != tablets.size() - 1) {
                if (pivotKeys.back() >= tablets[lastTabletIndex + 1]->GetPivotKey()) {
                    THROW_ERROR_EXCEPTION(
                        "Last pivot key must be strictly less than that of the tablet "
                            "which follows the resharded range");
                }
            }

            for (int index = 0; index < static_cast<int>(pivotKeys.size()) - 1; ++index) {
                if (pivotKeys[index] >= pivotKeys[index + 1]) {
                    THROW_ERROR_EXCEPTION("Pivot keys must be strictly increasing");
                }
            }

            // Validate pivot keys against table schema.
            for (const auto& pivotKey : pivotKeys) {
                ValidatePivotKey(pivotKey, table->TableSchema());
            }
        } else {
            if (!pivotKeys.empty()) {
                THROW_ERROR_EXCEPTION("Table is sorted; must provide tablet count");
            }
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Reshard table (TableId: %v, FirstTabletInded: %v, LastTabletIndex: %v, TabletCount %v, PivotKeys: %v)",
            table->GetId(),
            firstTabletIndex,
            lastTabletIndex,
            newTabletCount,
            pivotKeys);

        // Validate that all affected tablets are unmounted.
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            const auto* tablet = tablets[index];
            if (tablet->GetState() != ETabletState::Unmounted) {
                THROW_ERROR_EXCEPTION("Cannot reshard table since tablet %v is not unmounted",
                    tablet->GetId());
            }
        }

        // Calculate retained timestamp for removed tablets.
        auto retainedTimestamp = MinTimestamp;
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            retainedTimestamp = std::max(retainedTimestamp, tablets[index]->GetRetainedTimestamp());
        }

        // For ordered tablets, if the number of tablets decreases then validate that the trailing ones
        // (which are about to drop) are properly trimmed.
        if (newTabletCount < oldTabletCount) {
            for (int index = firstTabletIndex + newTabletCount; index < firstTabletIndex + oldTabletCount; ++index) {
                const auto* tablet = table->Tablets()[index];
                const auto& chunkListStatistics = tablet->GetChunkList()->Statistics();
                if (tablet->GetTrimmedRowCount() != chunkListStatistics.LogicalRowCount - chunkListStatistics.RowCount) {
                    THROW_ERROR_EXCEPTION("Some chunks of tablet %v are not fully trimmed; such a tablet cannot "
                        "participate in resharding",
                        tablet->GetId());
                }
            }
        }

        std::vector<TChunk*> chunks;

        // For each chunk verify that it is covered (without holes) by old tablets.
        if (table->IsPhysicallySorted()) {
            const auto& tabletChunkTrees = table->GetChunkList()->Children();
            std::vector<yhash_set<TChunk*>> chunkSets(lastTabletIndex + 1);

            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                std::vector<TChunk*> tabletChunks;
                EnumerateChunksInChunkTree(tabletChunkTrees[index]->AsChunkList(), &tabletChunks);
                chunkSets[index].insert(tabletChunks.begin(), tabletChunks.end());
                chunks.insert(chunks.end(), tabletChunks.begin(), tabletChunks.end());
            }

            std::sort(chunks.begin(), chunks.end(), TObjectRefComparer::Compare);
            chunks.erase(
                std::unique(chunks.begin(), chunks.end()),
                chunks.end());
            int keyColumnCount = table->TableSchema().GetKeyColumnCount();
            auto oldTablets = std::vector<TTablet*>(
                tablets.begin() + firstTabletIndex,
                tablets.begin() + lastTabletIndex + 1);

            for (auto* chunk : chunks) {
                auto keyPair = GetChunkBoundaryKeys(chunk->ChunkMeta(), keyColumnCount);
                auto range = GetIntersectingTablets(oldTablets, keyPair.first, keyPair.second);
                for (auto it = range.first; it != range.second; ++it) {
                    auto* tablet = *it;
                    if (chunkSets[tablet->GetIndex()].find(chunk) == chunkSets[tablet->GetIndex()].end()) {
                        THROW_ERROR_EXCEPTION("Chunk %v crosses boundary of tablet %v but is missing from its chunk list;"
                            " please wait until stores are compacted",
                            chunk->GetId(),
                            tablet->GetId())
                        << TErrorAttribute("chunk_min_key", keyPair.first)
                        << TErrorAttribute("chunk_max_key", keyPair.second)
                        << TErrorAttribute("pivot_key", tablet->GetPivotKey())
                        << TErrorAttribute("next_pivot_key", tablet->GetIndex() < table->Tablets().size() - 1
                            ? table->Tablets()[tablet->GetIndex() + 1]->GetPivotKey()
                            : MaxKey());
                    }
                }
            }
        }

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "reshard_table");

        // Create new tablets.
        std::vector<TTablet*> newTablets;
        for (int index = 0; index < newTabletCount; ++index) {
            auto* newTablet = CreateTablet(table);
            auto* oldTablet = index < oldTabletCount ? tablets[index + firstTabletIndex] : nullptr;
            if (table->IsSorted()) {
                newTablet->SetPivotKey(pivotKeys[index]);
            } else if (oldTablet) {
                newTablet->SetTrimmedRowCount(oldTablet->GetTrimmedRowCount());
            }
            newTablet->SetRetainedTimestamp(retainedTimestamp);
            newTablets.push_back(newTablet);

            if (table->IsReplicated()) {
                const auto* replicatedTable = table->As<TReplicatedTableNode>();
                for (auto* replica : replicatedTable->Replicas()) {
                    YCHECK(newTablet->Replicas().emplace(replica, TTableReplicaInfo()).second);
                }
            }
        }

        // Drop old tablets.
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = tablets[index];
            tablet->SetTable(nullptr);
            objectManager->UnrefObject(tablet);
        }

        // NB: Evaluation order is important here, consider the case lastTabletIndex == -1.
        tablets.erase(tablets.begin() + firstTabletIndex, tablets.begin() + (lastTabletIndex + 1));
        tablets.insert(tablets.begin() + firstTabletIndex, newTablets.begin(), newTablets.end());

        // Update all indexes.
        for (int index = 0; index < static_cast<int>(tablets.size()); ++index) {
            auto* tablet = tablets[index];
            tablet->SetIndex(index);
        }

        // Copy chunk tree if somebody holds a reference.
        CopyChunkListIfShared(table, firstTabletIndex, lastTabletIndex);

        auto* oldRootChunkList = table->GetChunkList();
        const auto& oldTabletChunkTrees = oldRootChunkList->Children();

        auto* newRootChunkList = chunkManager->CreateChunkList(oldRootChunkList->GetKind());
        const auto& newTabletChunkTrees = newRootChunkList->Children();

        // Update tablet chunk lists.
        chunkManager->AttachToChunkList(
            newRootChunkList,
            oldTabletChunkTrees.data(),
            oldTabletChunkTrees.data() + firstTabletIndex);
        for (int index = 0; index < newTabletCount; ++index) {
            auto* tabletChunkList = chunkManager->CreateChunkList(table->IsPhysicallySorted()
                ? EChunkListKind::SortedDynamicTablet
                : EChunkListKind::OrderedDynamicTablet);
            if (table->IsPhysicallySorted()) {
                tabletChunkList->SetPivotKey(pivotKeys[index]);
            }

            chunkManager->AttachToChunkList(newRootChunkList, tabletChunkList);
        }
        chunkManager->AttachToChunkList(
            newRootChunkList,
            oldTabletChunkTrees.data() + lastTabletIndex + 1,
            oldTabletChunkTrees.data() + oldTabletChunkTrees.size());

        auto enumerateChunks = [&] (int firstTabletIndex, int lastTabletIndex) {
            std::vector<TChunk*> chunks;
            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                EnumerateChunksInChunkTree(oldTabletChunkTrees[index]->AsChunkList(), &chunks);
            }
            return chunks;
        };

        if (table->IsPhysicallySorted()) {
            // Move chunks from the resharded tablets to appropriate chunk lists.
            int keyColumnCount = table->TableSchema().GetKeyColumnCount();
            for (auto* chunk : chunks) {
                auto keyPair = GetChunkBoundaryKeys(chunk->ChunkMeta(), keyColumnCount);
                auto range = GetIntersectingTablets(newTablets, keyPair.first, keyPair.second);
                for (auto it = range.first; it != range.second; ++it) {
                    auto* tablet = *it;
                    chunkManager->AttachToChunkList(
                        newTabletChunkTrees[tablet->GetIndex()]->AsChunkList(),
                        chunk);
                }
            }
        } else {
            // If the number of tablets increases, just leave the new trailing ones empty.
            // If the number of tablets decreases, merge the original trailing ones.
            for (int index = firstTabletIndex; index < firstTabletIndex + std::min(oldTabletCount, newTabletCount); ++index) {
                auto chunks = enumerateChunks(
                    index,
                    index == firstTabletIndex + newTabletCount - 1 ? lastTabletIndex : index);
                auto* chunkList = newTabletChunkTrees[index]->AsChunkList();
                for (auto* chunk : chunks) {
                    chunkManager->AttachToChunkList(chunkList, chunk);
                }
            }
        }

        securityManager->UpdateAccountNodeUsage(table);

        // Replace root chunk list.
        table->SetChunkList(newRootChunkList);
        newRootChunkList->AddOwningNode(table);
        objectManager->RefObject(newRootChunkList);
        oldRootChunkList->RemoveOwningNode(table);
        objectManager->UnrefObject(oldRootChunkList);

        table->SnapshotStatistics() = table->GetChunkList()->Statistics().ToDataStatistics();
    }

    void CloneTable(
        TTableNode* sourceTable,
        TTableNode* clonedTable,
        TTransaction* transaction,
        ENodeCloneMode mode)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* trunkSourceTable = sourceTable->GetTrunkNode();
        auto* trunkClonedTable = clonedTable; // sic!

        YCHECK(!trunkSourceTable->Tablets().empty());
        YCHECK(trunkClonedTable->Tablets().empty());

        try {
            if (trunkSourceTable->IsReplicated()) {
                THROW_ERROR_EXCEPTION("Cannot clone a replicated table");
            }

            auto tabletState = trunkSourceTable->GetTabletState();
            switch (mode) {
                case ENodeCloneMode::Copy:
                    if (tabletState != ETabletState::Unmounted && tabletState != ETabletState::Frozen) {
                        THROW_ERROR_EXCEPTION("Cannot copy dynamic table since not all of its tablets are in %Qlv or %Qlv state",
                            ETabletState::Unmounted,
                            ETabletState::Frozen);
                    }
                    break;

                case ENodeCloneMode::Move:
                    if (tabletState != ETabletState::Unmounted) {
                        THROW_ERROR_EXCEPTION("Cannot move dynamic table since not all of its tablets are in %Qlv state",
                            ETabletState::Unmounted);
                    }
                    break;

                default:
                    Y_UNREACHABLE();
            }
        } catch (const std::exception& ex) {
            const auto& cypressManager = Bootstrap_->GetCypressManager();
            auto sourceTableProxy = cypressManager->GetNodeProxy(trunkSourceTable, transaction);
            THROW_ERROR_EXCEPTION("Error cloning table %v",
                sourceTableProxy->GetPath())
                << ex;
        }

        // Undo the harm done in TChunkOwnerTypeHandler::DoClone.
        auto* fakeClonedRootChunkList = trunkClonedTable->GetChunkList();
        fakeClonedRootChunkList->RemoveOwningNode(trunkClonedTable);
        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->UnrefObject(fakeClonedRootChunkList);

        const auto& sourceTablets = trunkSourceTable->Tablets();
        YCHECK(!sourceTablets.empty());
        auto& clonedTablets = trunkClonedTable->Tablets();
        YCHECK(clonedTablets.empty());

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* clonedRootChunkList = chunkManager->CreateChunkList(fakeClonedRootChunkList->GetKind());
        trunkClonedTable->SetChunkList(clonedRootChunkList);
        objectManager->RefObject(clonedRootChunkList);
        clonedRootChunkList->AddOwningNode(trunkClonedTable);

        clonedTablets.reserve(sourceTablets.size());
        auto* sourceRootChunkList = trunkSourceTable->GetChunkList();
        YCHECK(sourceRootChunkList->Children().size() == sourceTablets.size());
        for (int index = 0; index < static_cast<int>(sourceTablets.size()); ++index) {
            const auto* sourceTablet = sourceTablets[index];

            auto* clonedTablet = CreateTablet(trunkClonedTable);
            clonedTablet->CopyFrom(*sourceTablet);

            auto* tabletChunkList = sourceRootChunkList->Children()[index];
            chunkManager->AttachToChunkList(clonedRootChunkList, tabletChunkList);

            clonedTablets.push_back(clonedTablet);
        }
    }

    void MakeTableDynamic(TTableNode* table)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (table->IsDynamic()) {
            return;
        }

        if (table->IsExternal()) {
            THROW_ERROR_EXCEPTION("Cannot switch mode from static to dynamic: table is external");
        }

        const auto& securityManager = this->Bootstrap_->GetSecurityManager();
        securityManager->ValidateResourceUsageIncrease(table->GetAccount(), TClusterResources().SetTabletCount(1));

        auto* oldRootChunkList = table->GetChunkList();

        std::vector<TChunk*> chunks;
        EnumerateChunksInChunkTree(oldRootChunkList, &chunks);

        // Check for duplicates.
        // Compute last commit timestamp.
        yhash_set<TChunk*> chunkSet;
        chunkSet.reserve(chunks.size());
        auto lastCommitTimestamp = NTransactionClient::MinTimestamp;
        for (auto* chunk : chunks) {
            if (!chunkSet.insert(chunk).second) {
                THROW_ERROR_EXCEPTION("Cannot switch mode from static to dynamic: table contains duplicate chunk %v",
                    chunk->GetId());
            }

            const auto& miscExt = chunk->MiscExt();
            if (miscExt.has_max_timestamp()) {
                lastCommitTimestamp = std::max(lastCommitTimestamp, static_cast<TTimestamp>(miscExt.max_timestamp()));
            }
        }
        table->SetLastCommitTimestamp(lastCommitTimestamp);

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* newRootChunkList = chunkManager->CreateChunkList(table->IsPhysicallySorted()
            ? EChunkListKind::SortedDynamicRoot
            : EChunkListKind::OrderedDynamicRoot);
        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(newRootChunkList);

        table->SetChunkList(newRootChunkList);
        newRootChunkList->AddOwningNode(table);

        auto* tablet = CreateTablet(table);
        tablet->SetIndex(0);
        if (table->IsSorted()) {
            tablet->SetPivotKey(EmptyKey());
        }
        table->Tablets().push_back(tablet);

        auto* tabletChunkList = chunkManager->CreateChunkList(table->IsPhysicallySorted()
            ? EChunkListKind::SortedDynamicTablet
            : EChunkListKind::OrderedDynamicTablet);
        if (table->IsPhysicallySorted()) {
            tabletChunkList->SetPivotKey(EmptyKey());
        }
        chunkManager->AttachToChunkList(newRootChunkList, tabletChunkList);

        std::vector<TChunkTree*> chunkTrees(chunks.begin(), chunks.end());
        chunkManager->AttachToChunkList(tabletChunkList, chunkTrees);

        oldRootChunkList->RemoveOwningNode(table);
        objectManager->UnrefObject(oldRootChunkList);

        securityManager->UpdateAccountNodeUsage(table);

        LOG_DEBUG_UNLESS(IsRecovery(), "Table is switched to dynamic mode (TableId: %v)",
            table->GetId());
    }

    void MakeTableStatic(TTableNode* table)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            return;
        }

        if (table->IsReplicated()) {
            THROW_ERROR_EXCEPTION("Cannot switch mode from dynamic to static: table is replicated");
        }

        if (table->IsSorted()) {
            THROW_ERROR_EXCEPTION("Cannot switch mode from dynamic to static: table is sorted");
        }

        if (table->GetTabletState() != ETabletState::Unmounted) {
            THROW_ERROR_EXCEPTION("Cannot switch mode from dynamic to static: table has mounted tablets");
        }

        auto* oldRootChunkList = table->GetChunkList();

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto newRootChunkList = chunkManager->CreateChunkList(EChunkListKind::Static);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(newRootChunkList);

        table->SetChunkList(newRootChunkList);
        newRootChunkList->AddOwningNode(table);

        std::vector<TChunk*> chunks;
        EnumerateChunksInChunkTree(oldRootChunkList, &chunks);
        std::vector<TChunkTree*> chunkTrees(chunks.begin(), chunks.end());
        chunkManager->AttachToChunkList(newRootChunkList, chunkTrees);

        oldRootChunkList->RemoveOwningNode(table);
        objectManager->UnrefObject(oldRootChunkList);

        for (auto* tablet : table->Tablets()) {
            tablet->SetTable(nullptr);
            objectManager->UnrefObject(tablet);
        }
        table->Tablets().clear();

        table->SetLastCommitTimestamp(NullTimestamp);

        const auto& securityManager = this->Bootstrap_->GetSecurityManager();
        securityManager->UpdateAccountNodeUsage(table);

        LOG_DEBUG_UNLESS(IsRecovery(), "Table is switched to static mode (TableId: %v)",
            table->GetId());
    }


    TTablet* GetTabletOrThrow(const TTabletId& id)
    {
        auto* tablet = FindTablet(id);
        if (!IsObjectAlive(tablet)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No tablet %v",
                id);
        }
        return tablet;
    }


    TTabletCell* GetTabletCellOrThrow(const TTabletCellId& id)
    {
        auto* cell = FindTabletCell(id);
        if (!IsObjectAlive(cell)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No such tablet cell %v",
                id);
        }
        return cell;
    }

    TTabletCellBundle* FindTabletCellBundleByName(const TString& name)
    {
        auto it = NameToTabletCellBundleMap_.find(name);
        return it == NameToTabletCellBundleMap_.end() ? nullptr : it->second;
    }

    TTabletCellBundle* GetTabletCellBundleByNameOrThrow(const TString& name)
    {
        auto* cellBundle = FindTabletCellBundleByName(name);
        if (!cellBundle) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No such tablet cell bundle %Qv",
                name);
        }
        return cellBundle;
    }

    void RenameTabletCellBundle(TTabletCellBundle* cellBundle, const TString& newName)
    {
        if (newName == cellBundle->GetName()) {
            return;
        }

        ValidateTabletCellBundleName(newName);

        if (FindTabletCellBundleByName(newName)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Tablet cell bundle %Qv already exists",
                newName);
        }

        YCHECK(NameToTabletCellBundleMap_.erase(cellBundle->GetName()) == 1);
        YCHECK(NameToTabletCellBundleMap_.insert(std::make_pair(newName, cellBundle)).second);
        cellBundle->SetName(newName);
    }

    TTabletCellBundle* GetDefaultTabletCellBundle()
    {
        return GetBuiltin(DefaultTabletCellBundle_);
    }

    void SetTabletCellBundle(TTableNode* table, TTabletCellBundle* cellBundle)
    {
        YCHECK(table->IsTrunk());

        if (table->GetTabletCellBundle() == cellBundle) {
            return;
        }

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(cellBundle, EPermission::Use);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        if (table->GetTabletCellBundle()) {
            objectManager->UnrefObject(table->GetTabletCellBundle());
        }
        objectManager->RefObject(cellBundle);

        table->SetTabletCellBundle(cellBundle);
    }


    DECLARE_ENTITY_MAP_ACCESSORS(TabletCellBundle, TTabletCellBundle);
    DECLARE_ENTITY_MAP_ACCESSORS(TabletCell, TTabletCell);
    DECLARE_ENTITY_MAP_ACCESSORS(Tablet, TTablet);
    DECLARE_ENTITY_MAP_ACCESSORS(TableReplica, TTableReplica);
    DECLARE_ENTITY_MAP_ACCESSORS(TabletAction, TTabletAction);

private:
    friend class TTabletCellBundleTypeHandler;

    const TTabletManagerConfigPtr Config_;

    const TTabletTrackerPtr TabletTracker_;
    const TTabletBalancerPtr TabletBalancer_;

    TEntityMap<TTabletCellBundle> TabletCellBundleMap_;
    TEntityMap<TTabletCell> TabletCellMap_;
    TEntityMap<TTablet> TabletMap_;
    TEntityMap<TTableReplica> TableReplicaMap_;
    TEntityMap<TTabletAction> TabletActionMap_;

    yhash<TString, TTabletCellBundle*> NameToTabletCellBundleMap_;

    yhash_mm<TString, TTabletCell*> AddressToCell_;
    yhash<TTransaction*, TTabletCell*> TransactionToCellMap_;

    bool InitializeCellBundles_ = false;
    TTabletCellBundleId DefaultTabletCellBundleId_;
    TTabletCellBundle* DefaultTabletCellBundle_ = nullptr;

    bool UpdateChunkListsKind_ = false;

    TPeriodicExecutorPtr CleanupExecutor_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void SaveKeys(NCellMaster::TSaveContext& context) const
    {
        TabletCellBundleMap_.SaveKeys(context);
        TabletCellMap_.SaveKeys(context);
        TabletMap_.SaveKeys(context);
        TableReplicaMap_.SaveKeys(context);
        TabletActionMap_.SaveKeys(context);
    }

    void SaveValues(NCellMaster::TSaveContext& context) const
    {
        TabletCellBundleMap_.SaveValues(context);
        TabletCellMap_.SaveValues(context);
        TabletMap_.SaveValues(context);
        TableReplicaMap_.SaveValues(context);
        TabletActionMap_.SaveValues(context);
    }


    void LoadKeys(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TabletCellBundleMap_.LoadKeys(context);
        TabletCellMap_.LoadKeys(context);
        TabletMap_.LoadKeys(context);
        // COMPAT(babenko)
        if (context.GetVersion() >= 400) {
            TableReplicaMap_.LoadKeys(context);
        }
        // COMPAT(savrus)
        if (context.GetVersion() >= 600) {
            TabletActionMap_.LoadKeys(context);
        }
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TabletCellBundleMap_.LoadValues(context);
        TabletCellMap_.LoadValues(context);
        TabletMap_.LoadValues(context);
        // COMPAT(babenko)
        if (context.GetVersion() >= 400) {
            TableReplicaMap_.LoadValues(context);
        }
        // COMPAT(savrus)
        if (context.GetVersion() >= 600) {
            TabletActionMap_.LoadValues(context);
        }

        // COMPAT(babenko)
        InitializeCellBundles_ = (context.GetVersion() < 400);
        // COMPAT(savrus)
        UpdateChunkListsKind_ = (context.GetVersion() < 600);
    }


    virtual void OnAfterSnapshotLoaded() override
    {
        TMasterAutomatonPart::OnAfterSnapshotLoaded();

        NameToTabletCellBundleMap_.clear();
        for (const auto& pair : TabletCellBundleMap_) {
            auto* cellBundle = pair.second;
            YCHECK(NameToTabletCellBundleMap_.insert(std::make_pair(cellBundle->GetName(), cellBundle)).second);
        }

        AddressToCell_.clear();
        for (const auto& pair : TabletCellMap_) {
            auto* cell = pair.second;
            if (!IsObjectAlive(cell)) {
                continue;
            }
            for (const auto& peer : cell->Peers()) {
                if (!peer.Descriptor.IsNull()) {
                    AddToAddressToCellMap(peer.Descriptor, cell);
                }
            }
            auto* transaction = cell->GetPrerequisiteTransaction();
            if (transaction) {
                YCHECK(TransactionToCellMap_.insert(std::make_pair(transaction, cell)).second);
            }
        }

        InitBuiltins();

        // COMPAT(babenko)
        if (InitializeCellBundles_) {
            const auto& cypressManager = Bootstrap_->GetCypressManager();
            for (const auto& pair : cypressManager->Nodes()) {
                auto* node = pair.second;
                if (node->IsTrunk() && node->GetType() == EObjectType::Table) {
                    auto* table = node->As<TTableNode>();
                    if (table->IsDynamic()) {
                        table->SetTabletCellBundle(DefaultTabletCellBundle_);
                        DefaultTabletCellBundle_->RefObject();
                    }
                }
            }

            for (const auto& pair : TabletCellMap_) {
                auto* cell = pair.second;
                cell->SetCellBundle(DefaultTabletCellBundle_);
                DefaultTabletCellBundle_->RefObject();
            }
        }

        // COMPAT(savrus)
        if (UpdateChunkListsKind_) {
            const auto& cypressManager = Bootstrap_->GetCypressManager();
            for (const auto& pair : cypressManager->Nodes()) {
                auto* node = pair.second;
                if (node->IsTrunk() && node->GetType() == EObjectType::Table) {
                    auto* table = node->As<TTableNode>();
                    if (table->IsDynamic()) {
                        auto* rootChunkList = table->GetChunkList();
                        YCHECK(rootChunkList->GetKind() == EChunkListKind::Static);
                        rootChunkList->SetKind(table->IsPhysicallySorted()
                            ? EChunkListKind::SortedDynamicRoot
                            : EChunkListKind::OrderedDynamicRoot);
                        YCHECK(rootChunkList->Children().size() == table->Tablets().size());
                        for (int index = 0; index < table->Tablets().size(); ++index) {
                            auto* child = rootChunkList->Children()[index];
                            auto* tabletChunkList = child->AsChunkList();
                            YCHECK(tabletChunkList->GetKind() == EChunkListKind::Static);
                            tabletChunkList->SetKind(table->IsPhysicallySorted()
                                ? EChunkListKind::SortedDynamicTablet
                                : EChunkListKind::OrderedDynamicTablet);
                            tabletChunkList->SetPivotKey(table->Tablets()[index]->GetPivotKey());
                        }
                    }
                }
            }
        }
    }

    virtual void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::Clear();

        TabletCellBundleMap_.Clear();
        TabletCellMap_.Clear();
        TabletMap_.Clear();
        TableReplicaMap_.Clear();
        TabletActionMap_.Clear();
        NameToTabletCellBundleMap_.clear();
        AddressToCell_.clear();
        TransactionToCellMap_.clear();


        DefaultTabletCellBundle_ = nullptr;
    }

    virtual void SetZeroState() override
    {
        InitBuiltins();
    }

    template <class T>
    T* GetBuiltin(T*& builtin)
    {
        if (!builtin) {
            InitBuiltins();
        }
        YCHECK(builtin);
        return builtin;
    }

    void InitBuiltins()
    {
        const auto& securityManager = Bootstrap_->GetSecurityManager();

        // Cell bundles

        // default
        if (EnsureBuiltinCellBundleInitialized(DefaultTabletCellBundle_, DefaultTabletCellBundleId_, DefaultTabletCellBundleName)) {
            DefaultTabletCellBundle_->Acd().AddEntry(TAccessControlEntry(
                ESecurityAction::Allow,
                securityManager->GetUsersGroup(),
                EPermission::Use));
        }
    }

    bool EnsureBuiltinCellBundleInitialized(TTabletCellBundle*& cellBundle, const TTabletCellBundleId& id, const TString& name)
    {
        if (cellBundle) {
            return false;
        }
        cellBundle = FindTabletCellBundle(id);
        if (cellBundle) {
            return false;
        }
        cellBundle = DoCreateTabletCellBundle(id, name);
        return true;
    }


    void OnNodeRegistered(TNode* node)
    {
        node->InitTabletSlots();
    }

    void OnNodeUnregistered(TNode* node)
    {
        for (const auto& slot : node->TabletSlots()) {
            auto* cell = slot.Cell;
            if (cell) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell peer offline: node unregistered (Address: %v, CellId: %v, PeerId: %v)",
                    node->GetDefaultAddress(),
                    cell->GetId(),
                    slot.PeerId);
                cell->DetachPeer(node);
            }
        }
        node->ClearTabletSlots();
    }

    void OnIncrementalHeartbeat(
        TNode* node,
        TReqIncrementalHeartbeat* request,
        TRspIncrementalHeartbeat* response)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // Various request helpers.
        auto requestCreateSlot = [&] (const TTabletCell* cell) {
            if (!response)
                return;

            if (!cell->GetPrerequisiteTransaction())
                return;

            auto* protoInfo = response->add_tablet_slots_to_create();

            const auto& cellId = cell->GetId();
            auto peerId = cell->GetPeerId(node->GetDefaultAddress());

            ToProto(protoInfo->mutable_cell_id(), cell->GetId());
            protoInfo->set_peer_id(peerId);

            const auto* cellBundle = cell->GetCellBundle();
            protoInfo->set_options(ConvertToYsonString(cellBundle->GetOptions()).GetData());

            LOG_DEBUG_UNLESS(IsRecovery(), "Tablet slot creation requested (Address: %v, CellId: %v, PeerId: %v)",
                node->GetDefaultAddress(),
                cellId,
                peerId);
        };

        auto requestConfigureSlot = [&] (const TNode::TTabletSlot* slot) {
            if (!response)
                return;

            const auto* cell = slot->Cell;
            if (!cell->GetPrerequisiteTransaction())
                return;

            auto* protoInfo = response->add_tablet_slots_configure();

            const auto& cellId = cell->GetId();
            auto cellDescriptor = cell->GetDescriptor();

            const auto& prerequisiteTransactionId = cell->GetPrerequisiteTransaction()->GetId();

            ToProto(protoInfo->mutable_cell_descriptor(), cellDescriptor);
            ToProto(protoInfo->mutable_prerequisite_transaction_id(), prerequisiteTransactionId);

            LOG_DEBUG_UNLESS(IsRecovery(), "Tablet slot configuration update requested "
                "(Address: %v, CellId: %v, Version: %v, PrerequisiteTransactionId: %v)",
                node->GetDefaultAddress(),
                cellId,
                cellDescriptor.ConfigVersion,
                prerequisiteTransactionId);
        };

        auto requestRemoveSlot = [&] (const TTabletCellId& cellId) {
            if (!response)
                return;

            auto* protoInfo = response->add_tablet_slots_to_remove();
            ToProto(protoInfo->mutable_cell_id(), cellId);

            LOG_DEBUG_UNLESS(IsRecovery(), "Tablet slot removal requested (Address: %v, CellId: %v)",
                node->GetDefaultAddress(),
                cellId);
        };

        const auto* mutationContext = GetCurrentMutationContext();
        auto mutationTimestamp = mutationContext->GetTimestamp();

        const auto& address = node->GetDefaultAddress();

        // Our expectations.
        yhash_set<TTabletCell*> expectedCells;
        for (const auto& slot : node->TabletSlots()) {
            auto* cell = slot.Cell;
            if (!IsObjectAlive(cell)) {
                continue;
            }
            YCHECK(expectedCells.insert(cell).second);
        }

        // Figure out and analyze the reality.
        yhash_set<TTabletCell*> actualCells;
        for (int slotIndex = 0; slotIndex < request->tablet_slots_size(); ++slotIndex) {
            // Pre-erase slot.
            auto& slot = node->TabletSlots()[slotIndex];
            slot = TNode::TTabletSlot();

            const auto& slotInfo = request->tablet_slots(slotIndex);

            auto state = EPeerState(slotInfo.peer_state());
            if (state == EPeerState::None)
                continue;

            auto cellInfo = FromProto<TCellInfo>(slotInfo.cell_info());
            const auto& cellId = cellInfo.CellId;
            auto* cell = FindTabletCell(cellId);
            if (!IsObjectAlive(cell)) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Unknown tablet slot is running (Address: %v, CellId: %v)",
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            auto peerId = cell->FindPeerId(address);
            if (peerId == InvalidPeerId) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Unexpected tablet cell is running (Address: %v, CellId: %v)",
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            if (slotInfo.peer_id() != InvalidPeerId && slotInfo.peer_id() != peerId)  {
                LOG_DEBUG_UNLESS(IsRecovery(), "Invalid peer id for tablet cell: %v instead of %v (Address: %v, CellId: %v)",
                    slotInfo.peer_id(),
                    peerId,
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            auto expectedIt = expectedCells.find(cell);
            if (expectedIt == expectedCells.end()) {
                cell->AttachPeer(node, peerId);
                LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell peer online (Address: %v, CellId: %v, PeerId: %v)",
                    address,
                    cellId,
                    peerId);
            }

            cell->UpdatePeerSeenTime(peerId, mutationTimestamp);
            YCHECK(actualCells.insert(cell).second);

            // Populate slot.
            slot.Cell = cell;
            slot.PeerState = state;
            slot.PeerId = slot.Cell->GetPeerId(node); // don't trust peerInfo, it may still be InvalidPeerId

            LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell is running (Address: %v, CellId: %v, PeerId: %v, State: %v, ConfigVersion: %v)",
                address,
                slot.Cell->GetId(),
                slot.PeerId,
                slot.PeerState,
                cellInfo.ConfigVersion);

            if (cellInfo.ConfigVersion != slot.Cell->GetConfigVersion()) {
                requestConfigureSlot(&slot);
            }
        }

        // Check for expected slots that are missing.
        for (auto* cell : expectedCells) {
            if (actualCells.find(cell) == actualCells.end()) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell peer offline: slot is missing (CellId: %v, Address: %v)",
                    cell->GetId(),
                    address);
                cell->DetachPeer(node);
            }
        }

        // Request slot starts.
        {
            int availableSlots = node->Statistics().available_tablet_slots();
            auto range = AddressToCell_.equal_range(address);
            for (auto it = range.first; it != range.second; ++it) {
                auto* cell = it->second;
                if (!IsObjectAlive(cell)) {
                    continue;
                }
                if (actualCells.find(cell) == actualCells.end()) {
                    requestCreateSlot(cell);
                    --availableSlots;
                }
            }
        }

        // Copy tablet statistics, update performance counters and table replica statistics.
        auto now = TInstant::Now();
        for (auto& tabletInfo : request->tablets()) {
            auto tabletId = FromProto<TTabletId>(tabletInfo.tablet_id());
            auto* tablet = FindTablet(tabletId);
            if (!IsObjectAlive(tablet) || tablet->GetState() == ETabletState::Unmounted) {
                continue;
            }

            auto* cell = tablet->GetCell();
            if (!IsObjectAlive(cell) || expectedCells.find(cell) == expectedCells.end()) {
                continue;
            }

            cell->TotalStatistics() -= GetTabletStatistics(tablet);
            tablet->NodeStatistics() = tabletInfo.statistics();
            cell->TotalStatistics() += GetTabletStatistics(tablet);

            auto* table = tablet->GetTable();
            if (table) {
                table->SetLastCommitTimestamp(std::max(
                    table->GetLastCommitTimestamp(),
                    tablet->NodeStatistics().last_commit_timestamp()));
            }

            auto updatePerformanceCounter = [&] (TTabletPerformanceCounter* counter, i64 curValue) {
                i64 prevValue = counter->Count;
                auto timeDelta = std::max(1.0, (now - tablet->PerformanceCounters().Timestamp).SecondsFloat());
                counter->Rate = (std::max(curValue, prevValue) - prevValue) / timeDelta;
                counter->Count = curValue;
            };

            #define XX(name, Name) updatePerformanceCounter( \
                &tablet->PerformanceCounters().Name, \
                tabletInfo.performance_counters().name ## _count());
            ITERATE_TABLET_PERFORMANCE_COUNTERS(XX)
            #undef XX
            tablet->PerformanceCounters().Timestamp = now;

            for (const auto& protoReplicaInfo : tabletInfo.replicas()) {
                auto replicaId = FromProto<TTableReplicaId>(protoReplicaInfo.replica_id());
                auto* replica = FindTableReplica(replicaId);
                if (!replica) {
                    continue;
                }

                auto* replicaInfo = tablet->FindReplicaInfo(replica);
                if (!replicaInfo) {
                    continue;
                }

                PopulateTableReplicaInfoFromStatistics(replicaInfo, protoReplicaInfo.statistics());
            }

            TabletBalancer_->OnTabletHeartbeat(tablet);
        }
    }


    void AddToAddressToCellMap(const TNodeDescriptor& descriptor, TTabletCell* cell)
    {
        AddressToCell_.insert(std::make_pair(descriptor.GetDefaultAddress(), cell));
    }

    void RemoveFromAddressToCellMap(const TNodeDescriptor& descriptor, TTabletCell* cell)
    {
        auto range = AddressToCell_.equal_range(descriptor.GetDefaultAddress());
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == cell) {
                AddressToCell_.erase(it);
                break;
            }
        }
    }


    void HydraAssignPeers(TReqAssignPeers* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TTabletCellId>(request->cell_id());
        auto* cell = FindTabletCell(cellId);
        if (!IsObjectAlive(cell)) {
            return;
        }

        const auto* mutationContext = GetCurrentMutationContext();
        auto mutationTimestamp = mutationContext->GetTimestamp();

        bool leadingPeerAssigned = false;
        for (const auto& peerInfo : request->peer_infos()) {
            auto peerId = peerInfo.peer_id();
            auto descriptor = FromProto<TNodeDescriptor>(peerInfo.node_descriptor());

            auto& peer = cell->Peers()[peerId];
            if (!peer.Descriptor.IsNull())
                continue;

            if (peerId == cell->GetLeadingPeerId()) {
                leadingPeerAssigned = true;
            }

            AddToAddressToCellMap(descriptor, cell);
            cell->AssignPeer(descriptor, peerId);
            cell->UpdatePeerSeenTime(peerId, mutationTimestamp);

            LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell peer assigned (CellId: %v, Address: %v, PeerId: %v)",
                cellId,
                descriptor.GetDefaultAddress(),
                peerId);
        }

        // Once a peer is assigned, we must ensure that the cell has a valid prerequisite transaction.
        if (leadingPeerAssigned || !cell->GetPrerequisiteTransaction()) {
            RestartPrerequisiteTransaction(cell);
        }

        ReconfigureCell(cell);
    }

    void HydraRevokePeers(TReqRevokePeers* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TTabletCellId>(request->cell_id());
        auto* cell = FindTabletCell(cellId);
        if (!IsObjectAlive(cell)) {
            return;
        }

        bool leadingPeerRevoked = false;
        for (auto peerId : request->peer_ids()) {
            if (peerId == cell->GetLeadingPeerId()) {
                leadingPeerRevoked = true;
            }
            DoRevokePeer(cell, peerId);
        }

        if (leadingPeerRevoked) {
            AbortPrerequisiteTransaction(cell);
            AbortCellSubtreeTransactions(cell);
        }
        ReconfigureCell(cell);
    }

    void HydraSetLeadingPeer(TReqSetLeadingPeer* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TTabletCellId>(request->cell_id());
        auto* cell = FindTabletCell(cellId);
        if (!IsObjectAlive(cell)) {
            return;
        }

        auto peerId = request->peer_id();
        cell->SetLeadingPeerId(peerId);

        const auto& descriptor = cell->Peers()[peerId].Descriptor;
        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell leading peer updated (CellId: %v, Address: %v, PeerId: %v)",
            cellId,
            descriptor.GetDefaultAddress(),
            peerId);

        RestartPrerequisiteTransaction(cell);
        ReconfigureCell(cell);
    }

    void HydraOnTabletMounted(TRspMountTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto state = tablet->GetState();
        if (state != ETabletState::Mounting && state != ETabletState::FrozenMounting) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Mounted notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                state,
                tabletId);
            return;
        }

        bool frozen = response->frozen();
        auto* table = tablet->GetTable();
        auto* cell = tablet->GetCell();

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet mounted (TableId: %v, TabletId: %v, MountRevision: %v, CellId: %v, Frozen: %v)",
            table->GetId(),
            tablet->GetId(),
            tablet->GetMountRevision(),
            cell->GetId(),
            frozen);

        cell->TotalStatistics() += GetTabletStatistics(tablet);

        tablet->SetState(frozen ? ETabletState::Frozen : ETabletState::Mounted);

        OnTabletActionStateChanged(tablet->GetAction());
    }

    void HydraOnTabletUnmounted(TRspUnmountTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto state = tablet->GetState();
        if (state != ETabletState::Unmounting) {
            LOG_WARNING_UNLESS(IsRecovery(), "Unmounted notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                state,
                tabletId);
            return;
        }

        DoTabletUnmounted(tablet);
        OnTabletActionStateChanged(tablet->GetAction());
    }

    void HydraOnTabletFrozen(TRspFreezeTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        const auto* table = tablet->GetTable();
        const auto* cell = tablet->GetCell();

        auto state = tablet->GetState();
        if (state != ETabletState::Freezing) {
            LOG_WARNING_UNLESS(IsRecovery(), "Frozen notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                state,
                tabletId);
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet frozen (TableId: %v, TabletId: %v, CellId: %v)",
            table->GetId(),
            tablet->GetId(),
            cell->GetId());

        tablet->SetState(ETabletState::Frozen);
        OnTabletActionStateChanged(tablet->GetAction());
    }

    void HydraOnTabletUnfrozen(TRspUnfreezeTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        const auto* table = tablet->GetTable();
        const auto* cell = tablet->GetCell();

        auto state = tablet->GetState();
        if (state != ETabletState::Unfreezing) {
            LOG_WARNING_UNLESS(IsRecovery(), "Unfrozen notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                state,
                tabletId);
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet unfrozen (TableId: %v, TabletId: %v, CellId: %v)",
            table->GetId(),
            tablet->GetId(),
            cell->GetId());

        tablet->SetState(ETabletState::Mounted);
        OnTabletActionStateChanged(tablet->GetAction());
    }

    void HydraUpdateTableReplicaStatistics(TReqUpdateTableReplicaStatistics* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto replicaId = FromProto<TTableReplicaId>(request->replica_id());
        auto* replica = FindTableReplica(replicaId);
        if (!IsObjectAlive(replica)) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        auto* replicaInfo = tablet->GetReplicaInfo(replica);
        PopulateTableReplicaInfoFromStatistics(replicaInfo, request->statistics());

        LOG_DEBUG_UNLESS(IsRecovery(), "Table replica statistics updated (TabletId: %v, ReplicaId: %v, "
            "CurrentReplicationRowIndex: %v, CurrentReplicationTimestamp: %llx)",
            tabletId,
            replicaId,
            replicaInfo->GetCurrentReplicationRowIndex(),
            replicaInfo->GetCurrentReplicationTimestamp());
    }

    void HydraOnTableReplicaDisabled(TRspDisableTableReplica* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto replicaId = FromProto<TTableReplicaId>(response->replica_id());
        auto* replica = FindTableReplica(replicaId);
        if (!IsObjectAlive(replica)) {
            return;
        }

        auto mountRevision = response->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        auto* replicaInfo = tablet->GetReplicaInfo(replica);
        if (replicaInfo->GetState() != ETableReplicaState::Disabling) {
            LOG_WARNING_UNLESS(IsRecovery(), "Disabled replica notification received for a replica in %Qlv state, "
                "ignored (TabletId: %v, ReplicaId: %v)",
                replicaInfo->GetState(),
                tabletId,
                replicaId);
            return;
        }

        replicaInfo->SetState(ETableReplicaState::Disabled);

        LOG_DEBUG_UNLESS(IsRecovery(), "Table replica tablet disabled (TabletId: %v, ReplicaId: %v)",
            tabletId,
            replicaId);

        YCHECK(replica->DisablingTablets().erase(tablet) == 1);
        CheckForReplicaDisabled(replica);
    }

    void CheckForReplicaDisabled(TTableReplica* replica)
    {
        if (replica->GetState() != ETableReplicaState::Disabling) {
            return;
        }

        if (!replica->DisablingTablets().empty()) {
            return;
        }

        auto* table = replica->GetTable();

        LOG_DEBUG_UNLESS(IsRecovery(), "Table replica disabled (TableId: %v, ReplicaId: %v)",
            table->GetId(),
            replica->GetId());

        replica->SetState(ETableReplicaState::Disabled);
    }

    void DoTabletUnmounted(TTablet* tablet)
    {
        auto* table = tablet->GetTable();
        auto* cell = tablet->GetCell();

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet unmounted (TableId: %v, TabletId: %v, CellId: %v)",
            table->GetId(),
            tablet->GetId(),
            cell->GetId());

        cell->TotalStatistics() -= GetTabletStatistics(tablet);

        tablet->NodeStatistics().Clear();
        tablet->PerformanceCounters() = TTabletPerformanceCounters();
        tablet->SetInMemoryMode(EInMemoryMode::None);
        tablet->SetState(ETabletState::Unmounted);
        tablet->SetCell(nullptr);
        tablet->SetStoresUpdatePreparedTransaction(nullptr);

        CommitTabletStaticMemoryUpdate(table);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        YCHECK(cell->Tablets().erase(tablet) == 1);
        objectManager->UnrefObject(cell);
    }

    void CopyChunkListIfShared(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        auto* oldRootChunkList = table->GetChunkList();
        auto& chunkLists = oldRootChunkList->Children();
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto& objectManager = Bootstrap_->GetObjectManager();

        if (objectManager->GetObjectRefCounter(oldRootChunkList) > 1) {
            auto statistics = oldRootChunkList->Statistics();
            auto* newRootChunkList = chunkManager->CreateChunkList(oldRootChunkList->GetKind());
            chunkManager->AttachToChunkList(
                newRootChunkList,
                chunkLists.data(),
                chunkLists.data() + firstTabletIndex);

            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* oldTabletChunkList = chunkLists[index]->AsChunkList();
                auto* newTabletChunkList = chunkManager->CreateChunkList(oldTabletChunkList->GetKind());
                newTabletChunkList->SetPivotKey(oldTabletChunkList->GetPivotKey());
                chunkManager->AttachToChunkList(
                    newTabletChunkList,
                    oldTabletChunkList->Children().data() + oldTabletChunkList->GetTrimmedChildCount(),
                    oldTabletChunkList->Children().data() + oldTabletChunkList->Children().size());
                chunkManager->AttachToChunkList(newRootChunkList, newTabletChunkList);
            }

            chunkManager->AttachToChunkList(
                newRootChunkList,
                chunkLists.data() + lastTabletIndex + 1,
                chunkLists.data() + chunkLists.size());

            // Replace root chunk list.
            table->SetChunkList(newRootChunkList);
            newRootChunkList->AddOwningNode(table);
            objectManager->RefObject(newRootChunkList);
            oldRootChunkList->RemoveOwningNode(table);
            objectManager->UnrefObject(oldRootChunkList);
            YCHECK(newRootChunkList->Statistics() == statistics);
        } else {
            auto statistics = oldRootChunkList->Statistics();

            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* oldTabletChunkList = chunkLists[index]->AsChunkList();
                if (objectManager->GetObjectRefCounter(oldTabletChunkList) > 1) {
                    auto* newTabletChunkList = chunkManager->CreateChunkList(oldTabletChunkList->GetKind());
                    newTabletChunkList->SetPivotKey(oldTabletChunkList->GetPivotKey());
                    chunkManager->AttachToChunkList(
                        newTabletChunkList,
                        oldTabletChunkList->Children().data() + oldTabletChunkList->GetTrimmedChildCount(),
                        oldTabletChunkList->Children().data() + oldTabletChunkList->Children().size());
                    chunkLists[index] = newTabletChunkList;

                    // TODO(savrus): make a helper to replace a tablet chunk list.
                    newTabletChunkList->AddParent(oldRootChunkList);
                    objectManager->RefObject(newTabletChunkList);
                    oldTabletChunkList->RemoveParent(oldRootChunkList);
                    objectManager->UnrefObject(oldTabletChunkList);
                }
            }

            YCHECK(oldRootChunkList->Statistics() == statistics);
        }
    }

    void HydraPrepareUpdateTabletStores(TTransaction* transaction, TReqUpdateTabletStores* request, bool persistent)
    {
        YCHECK(persistent);

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = GetTabletOrThrow(tabletId);

        if (tablet->GetStoresUpdatePreparedTransaction()) {
            THROW_ERROR_EXCEPTION("Stores update for tablet %v is already prepared by transaction %v",
                tabletId,
                tablet->GetStoresUpdatePreparedTransaction()->GetId());
        }

        auto mountRevision = request->mount_revision();
        tablet->ValidateMountRevision(mountRevision);

        auto state = tablet->GetState();
        if (state != ETabletState::Mounted &&
            state != ETabletState::Unmounting &&
            state != ETabletState::Freezing)
        {
            THROW_ERROR_EXCEPTION("Cannot update stores while tablet %v is in %Qlv state",
                tabletId,
                state);
        }

        const auto* table = tablet->GetTable();
        if (!table->IsPhysicallySorted()) {
            auto* tabletChunkList = tablet->GetChunkList();

            if (request->stores_to_add_size() > 0) {
                if (request->stores_to_add_size() > 1) {
                    THROW_ERROR_EXCEPTION("Cannot attach more than one store to an ordered tablet %v at once",
                        tabletId);
                }

                const auto& descriptor = request->stores_to_add(0);
                auto storeId = FromProto<TStoreId>(descriptor.store_id());
                YCHECK(descriptor.has_starting_row_index());
                if (tabletChunkList->Statistics().LogicalRowCount != descriptor.starting_row_index()) {
                    THROW_ERROR_EXCEPTION("Invalid starting row index of store %v in tablet %v: expected %v, got %v",
                        storeId,
                        tabletId,
                        tabletChunkList->Statistics().LogicalRowCount,
                        descriptor.starting_row_index());
                }
            }

            if (request->stores_to_remove_size() > 0) {
                int childIndex = tabletChunkList->GetTrimmedChildCount();
                const auto& children = tabletChunkList->Children();
                for (const auto& descriptor : request->stores_to_remove()) {
                    auto storeId = FromProto<TStoreId>(descriptor.store_id());
                    if (TypeFromId(storeId) == EObjectType::OrderedDynamicTabletStore) {
                        continue;
                    }

                    if (childIndex >= children.size()) {
                        THROW_ERROR_EXCEPTION("Attempted to trim store %v which is not part of tablet %v",
                            storeId,
                            tabletId);
                    }
                    if (children[childIndex]->GetId() != storeId) {
                        THROW_ERROR_EXCEPTION("Invalid store to trim in tablet %v: expected %v, got %v",
                            tabletId,
                            children[childIndex]->GetId(),
                            storeId);
                    }
                    ++childIndex;
                }
            }
        }

        tablet->SetStoresUpdatePreparedTransaction(transaction);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet stores update prepared (TransactionId: %v, TableId: %v, TabletId: %v)",
            transaction->GetId(),
            table->GetId(),
            tabletId);
    }

    void HydraCommitUpdateTabletStores(TTransaction* transaction, TReqUpdateTabletStores* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            LOG_ERROR_UNLESS(IsRecovery(), "Unexpected error: invalid mount revision on tablet stores update commit; ignored "
                "(TabletId: %v, TransactionId: %v, ExpectedMountRevision: %v, ActualMountRevision: %v)",
                tabletId,
                transaction->GetId(),
                mountRevision,
                tablet->GetMountRevision());
            return;
        }

        if (tablet->GetStoresUpdatePreparedTransaction() != transaction) {
            LOG_ERROR_UNLESS(IsRecovery(), "Unexpected error: tablet stores update commit for an improperly unprepared tablet; ignored "
                "(TabletId: %v, ExpectedTransactionId: %v, ActualTransactionId: %v)",
                tabletId,
                transaction->GetId(),
                GetObjectId(tablet->GetStoresUpdatePreparedTransaction()));
            return;
        }

        auto* table = tablet->GetTable();
        if (!IsObjectAlive(table)) {
            return;
        }

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        cypressManager->SetModified(table, nullptr);

        // Collect all changes first.
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        std::vector<TChunkTree*> chunksToAttach;
        i64 attachedRowCount = 0;
        auto lastCommitTimestamp = table->GetLastCommitTimestamp();
        for (const auto& descriptor : request->stores_to_add()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            if (TypeFromId(storeId) == EObjectType::Chunk ||
                TypeFromId(storeId) == EObjectType::ErasureChunk)
            {
                auto* chunk = chunkManager->GetChunkOrThrow(storeId);
                if (!chunk->Parents().empty()) {
                    THROW_ERROR_EXCEPTION("Chunk %v cannot be attached since it already has a parent",
                        chunk->GetId());
                }
                const auto& miscExt = chunk->MiscExt();
                if (miscExt.has_max_timestamp()) {
                    lastCommitTimestamp = std::max(lastCommitTimestamp, static_cast<TTimestamp>(miscExt.max_timestamp()));
                }
                attachedRowCount += miscExt.row_count();
                chunksToAttach.push_back(chunk);
            }
        }

        std::vector<TChunkTree*> chunksToDetach;
        i64 detachedRowCount = 0;
        for (const auto& descriptor : request->stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            if (TypeFromId(storeId) == EObjectType::Chunk ||
                TypeFromId(storeId) == EObjectType::ErasureChunk)
            {
                auto* chunk = chunkManager->GetChunkOrThrow(storeId);
                const auto& miscExt = chunk->MiscExt();
                detachedRowCount += miscExt.row_count();
                chunksToDetach.push_back(chunk);
            }
        }

        // Update last commit timestamp.
        table->SetLastCommitTimestamp(lastCommitTimestamp);

        // Update retained timestamp.
        auto retainedTimestamp = std::max(
            tablet->GetRetainedTimestamp(),
            static_cast<TTimestamp>(request->retained_timestamp()));
        tablet->SetRetainedTimestamp(retainedTimestamp);

        // Copy chunk tree if somebody holds a reference.
        CopyChunkListIfShared(table, tablet->GetIndex(), tablet->GetIndex());

        // Save old tabet resource usage.
        auto oldMemorySize = tablet->GetTabletStaticMemorySize();
        auto oldStatistics = GetTabletStatistics(tablet);

        // Apply all requested changes.
        auto* tabletChunkList = tablet->GetChunkList();
        auto* cell = tablet->GetCell();
        chunkManager->AttachToChunkList(tabletChunkList, chunksToAttach);
        chunkManager->DetachFromChunkList(tabletChunkList, chunksToDetach);
        table->SnapshotStatistics() = table->GetChunkList()->Statistics().ToDataStatistics();

        // Get new tabet resource usage.
        auto newMemorySize = tablet->GetTabletStaticMemorySize();
        auto newStatistics = GetTabletStatistics(tablet);
        auto deltaStatistics = newStatistics - oldStatistics;

        // Update cell statistics.
        cell->TotalStatistics() += deltaStatistics;

        // Unstage just attached chunks.
        // Update table resource usage.
        for (auto* chunk : chunksToAttach) {
            chunkManager->UnstageChunk(chunk->AsChunk());
        }

        if (tablet->GetStoresUpdatePreparedTransaction() == transaction) {
            tablet->SetStoresUpdatePreparedTransaction(nullptr);
        }

        // Update node resource usage.
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        auto deltaResources = TClusterResources()
            .SetChunkCount(deltaStatistics.ChunkCount)
            .SetTabletStaticMemory(newMemorySize - oldMemorySize);
        std::copy(
            std::begin(deltaStatistics.DiskSpace),
            std::end(deltaStatistics.DiskSpace),
            std::begin(deltaResources.DiskSpace));
        securityManager->IncrementAccountNodeUsage(table, deltaResources);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet stores update committed (TransactionId: %v, TableId: %v, TabletId: %v, "
            "AttachedChunkIds: %v, DetachedChunkIds: %v, "
            "AttachedRowCount: %v, DetachedRowCount: %v, RetainedTimestamp: %llx)",
            transaction->GetId(),
            table->GetId(),
            tabletId,
            MakeFormattableRange(chunksToAttach, TObjectIdFormatter()),
            MakeFormattableRange(chunksToDetach, TObjectIdFormatter()),
            attachedRowCount,
            detachedRowCount,
            retainedTimestamp);
    }

    void HydraAbortUpdateTabletStores(TTransaction* transaction, TReqUpdateTabletStores* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        if (tablet->GetStoresUpdatePreparedTransaction() != transaction) {
            return;
        }

        const auto* table = tablet->GetTable();

        tablet->SetStoresUpdatePreparedTransaction(nullptr);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet stores update aborted (TransactionId: %v, TableId: %v, TabletId: %v)",
            transaction->GetId(),
            table->GetId(),
            tabletId);
    }

    void HydraUpdateTabletTrimmedRowCount(TReqUpdateTabletTrimmedRowCount* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        auto trimmedRowCount = request->trimmed_row_count();

        tablet->SetTrimmedRowCount(trimmedRowCount);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet trimmed row count updated (TabletId: %v, TrimmedRowCount: %v)",
            tabletId,
            trimmedRowCount);
    }

    void HydraCreateTabletAction(TReqCreateTabletAction* request)
    {
        auto kind = ETabletActionKind(request->kind());
        auto tabletIds = FromProto<std::vector<TTabletId>>(request->tablet_ids());
        auto cellIds = FromProto<std::vector<TTabletId>>(request->cell_ids());
        auto pivotKeys = FromProto<std::vector<TOwningKey>>(request->pivot_keys());
        bool keepFinished = request->keep_finished();
        TNullable<int> tabletCount = request->has_tablet_count()
            ? MakeNullable(request->tablet_count())
            : Null;
        std::vector<TTablet*> tablets;
        std::vector<TTabletCell*> cells;

        for (const auto& tabletId : tabletIds) {
            tablets.push_back(GetTabletOrThrow(tabletId));
        }

        for (const auto& cellId : cellIds) {
            cells.push_back(GetTabletCellOrThrow(cellId));
        }

        try {
            CreateTabletAction(NullObjectId, kind, tablets, cells, pivotKeys, tabletCount, false, Null, keepFinished);
        } catch (const std::exception& ex) {
            LOG_DEBUG_UNLESS(IsRecovery(), TError(ex), "Error creating tablet action (Kind: %v, Tablets: %v, TabletCellsL %v, PivotKeys %v, TabletCount %v)",
                kind,
                tablets,
                cells,
                pivotKeys,
                tabletCount,
                TError(ex));
        }
    }

    
    virtual void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnLeaderActive();

        if (Bootstrap_->IsPrimaryMaster()) {
            TabletTracker_->Start();
            TabletBalancer_->Start();
        }

        CleanupExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(),
            BIND(&TImpl::OnCleanup, MakeWeak(this)),
            CleanupPeriod);
        CleanupExecutor_->Start();
    }

    virtual void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnStopLeading();

        TabletTracker_->Stop();
        TabletBalancer_->Stop();

        if (CleanupExecutor_) {
            CleanupExecutor_->Stop();
            CleanupExecutor_.Reset();
        }
    }


    void ReconfigureCell(TTabletCell* cell)
    {
        cell->SetConfigVersion(cell->GetConfigVersion() + 1);

        auto config = cell->GetConfig();
        config->Addresses.clear();
        for (const auto& peer : cell->Peers()) {
            if (peer.Descriptor.IsNull()) {
                config->Addresses.push_back(Null);
            } else {
                config->Addresses.push_back(peer.Descriptor.GetAddress(Bootstrap_->GetConfig()->Networks));
            }
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell reconfigured (CellId: %v, Version: %v)",
            cell->GetId(),
            cell->GetConfigVersion());
    }


    void ValidateHasHealthyCells(TTabletCellBundle* cellBundle)
    {
        for (const auto& pair : TabletCellMap_) {
            auto* cell = pair.second;
            if (!IsObjectAlive(cell)) {
                continue;
            }
            if (cell->GetCellBundle() == cellBundle &&
                cell->GetHealth() == ETabletCellHealth::Good)
            {
                return;
            }
        }
        THROW_ERROR_EXCEPTION("No healthy tablet cells in bundle %Qv",
            cellBundle->GetName());
    }

    std::vector<std::pair<TTablet*, TTabletCell*>> ComputeTabletAssignment(
        TTableNode* table,
        TTableMountConfigPtr mountConfig,
        TTabletCell* hintCell,
        std::vector<TTablet*> tabletsToMount)
    {
        if (hintCell) {
            std::vector<std::pair<TTablet*, TTabletCell*>> assignment;
            for (auto* tablet : tabletsToMount) {
                assignment.emplace_back(tablet, hintCell);
            }
            return assignment;
        }

        struct TCellKey
        {
            i64 Size;
            TTabletCell* Cell;

            //! Compares by |(size, cellId)|.
            bool operator < (const TCellKey& other) const
            {
                if (Size < other.Size) {
                    return true;
                } else if (Size > other.Size) {
                    return false;
                }
                return Cell->GetId() < other.Cell->GetId();
            }
        };

        auto getCellSize = [&] (const TTabletCell* cell) -> i64 {
            i64 result = 0;
            i64 tabletCount;
            switch (mountConfig->InMemoryMode) {
                case EInMemoryMode::None:
                    result += cell->TotalStatistics().UncompressedDataSize;
                    tabletCount = cell->Tablets().size();
                    break;
                case EInMemoryMode::Uncompressed:
                case EInMemoryMode::Compressed: {
                    result += cell->TotalStatistics().MemorySize;
                    tabletCount = cell->TotalStatistics().TabletCountPerMemoryMode[EInMemoryMode::Uncompressed] +
                        cell->TotalStatistics().TabletCountPerMemoryMode[EInMemoryMode::Compressed];
                    break;
                }
                default:
                    Y_UNREACHABLE();
            }
            result += tabletCount * Config_->TabletDataSizeFootprint;
            return result;
        };

        std::set<TCellKey> cellKeys;
        for (const auto& pair : TabletCellMap_) {
            auto* cell = pair.second;
            if (!IsObjectAlive(cell)) {
                continue;
            }
            if (cell->GetCellBundle() == table->GetTabletCellBundle() &&
                cell->GetHealth() == ETabletCellHealth::Good)
            {
                YCHECK(cellKeys.insert(TCellKey{getCellSize(cell), cell}).second);
            }
        }
        YCHECK(!cellKeys.empty());

        auto getTabletSize = [&] (const TTablet* tablet) -> i64 {
            i64 result = 0;
            auto statistics = GetTabletStatistics(tablet);
            switch (mountConfig->InMemoryMode) {
                case EInMemoryMode::None:
                case EInMemoryMode::Uncompressed:
                    result += statistics.UncompressedDataSize;
                    break;
                case EInMemoryMode::Compressed:
                    result += statistics.CompressedDataSize;
                    break;
                default:
                    Y_UNREACHABLE();
            }
            result += Config_->TabletDataSizeFootprint;
            return result;
        };

        // Sort tablets by decreasing size to improve greedy heuristic performance.
        std::sort(
            tabletsToMount.begin(),
            tabletsToMount.end(),
            [&] (const TTablet* lhs, const TTablet* rhs) {
                return
                    std::make_tuple(getTabletSize(lhs), lhs->GetId()) >
                    std::make_tuple(getTabletSize(rhs), rhs->GetId());
            });

        auto chargeCell = [&] (std::set<TCellKey>::iterator it, TTablet* tablet) {
            const auto& existingKey = *it;
            auto newKey = TCellKey{existingKey.Size + getTabletSize(tablet), existingKey.Cell};
            cellKeys.erase(it);
            YCHECK(cellKeys.insert(newKey).second);
        };

        // Iteratively assign tablets to least-loaded cells.
        std::vector<std::pair<TTablet*, TTabletCell*>> assignment;
        for (auto* tablet : tabletsToMount) {
            auto it = cellKeys.begin();
            assignment.emplace_back(tablet, it->Cell);
            chargeCell(it, tablet);
        }

        return assignment;
    }


    void RestartPrerequisiteTransaction(TTabletCell* cell)
    {
        AbortPrerequisiteTransaction(cell);
        AbortCellSubtreeTransactions(cell);
        StartPrerequisiteTransaction(cell);
    }

    void StartPrerequisiteTransaction(TTabletCell* cell)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        const auto& secondaryCellTags = multicellManager->GetRegisteredMasterCellTags();

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionManager->StartTransaction(
            nullptr,
            secondaryCellTags,
            secondaryCellTags,
            Null,
            Format("Prerequisite for cell %v", cell->GetId()),
            EmptyAttributes());

        YCHECK(!cell->GetPrerequisiteTransaction());
        cell->SetPrerequisiteTransaction(transaction);
        YCHECK(TransactionToCellMap_.insert(std::make_pair(transaction, cell)).second);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell prerequisite transaction started (CellId: %v, TransactionId: %v)",
            cell->GetId(),
            transaction->GetId());
    }

    void AbortCellSubtreeTransactions(TTabletCell* cell)
    {
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto cellNodeProxy = FindCellNode(cell->GetId());
        if (cellNodeProxy) {
            cypressManager->AbortSubtreeTransactions(cellNodeProxy);
        }
    }

    void AbortPrerequisiteTransaction(TTabletCell* cell)
    {
        auto* transaction = cell->GetPrerequisiteTransaction();
        if (!transaction) {
            return;
        }

        // Suppress calling OnTransactionFinished.
        YCHECK(TransactionToCellMap_.erase(transaction) == 1);
        cell->SetPrerequisiteTransaction(nullptr);

        // NB: Make a copy, transaction will die soon.
        auto transactionId = transaction->GetId();

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->AbortTransaction(transaction, true);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell prerequisite aborted (CellId: %v, TransactionId: %v)",
            cell->GetId(),
            transactionId);
    }

    void OnTransactionFinished(TTransaction* transaction)
    {
        auto it = TransactionToCellMap_.find(transaction);
        if (it == TransactionToCellMap_.end()) {
            return;
        }

        auto* cell = it->second;
        cell->SetPrerequisiteTransaction(nullptr);
        TransactionToCellMap_.erase(it);

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell prerequisite transaction aborted (CellId: %v, TransactionId: %v)",
            cell->GetId(),
            transaction->GetId());

        for (auto peerId = 0; peerId < cell->Peers().size(); ++peerId) {
            DoRevokePeer(cell, peerId);
        }
    }


    void DoRevokePeer(TTabletCell* cell, TPeerId peerId)
    {
        const auto& peer = cell->Peers()[peerId];
        const auto& descriptor = peer.Descriptor;
        if (descriptor.IsNull()) {
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell peer revoked (CellId: %v, Address: %v, PeerId: %v)",
            cell->GetId(),
            descriptor.GetDefaultAddress(),
            peerId);

        if (peer.Node) {
            peer.Node->DetachTabletCell(cell);
        }
        RemoveFromAddressToCellMap(descriptor, cell);
        cell->RevokePeer(peerId);
    }

    void DoUnmountTable(
        TTableNode* table,
        bool force,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            DoUnmountTablet(tablet, force);
        }
    }

    void DoUnmountTablet(
        TTablet* tablet,
        bool force)
    {
        auto state = tablet->GetState();
        if (state == ETabletState::Unmounted) {
            return;
        }
        if (!force) {
            YCHECK(state == ETabletState::Mounted ||
                state == ETabletState::Frozen ||
                state == ETabletState::Freezing ||
                state == ETabletState::Unmounting);
        }

        const auto& hiveManager = Bootstrap_->GetHiveManager();

        auto* table = tablet->GetTable();
        auto* cell = tablet->GetCell();
        YCHECK(cell);

        LOG_DEBUG_UNLESS(IsRecovery(), "Unmounting tablet (TableId: %v, TabletId: %v, CellId: %v, Force: %v)",
            table->GetId(),
            tablet->GetId(),
            cell->GetId(),
            force);

        tablet->SetState(ETabletState::Unmounting);

        TReqUnmountTablet request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        request.set_force(force);
        auto* mailbox = hiveManager->GetMailbox(cell->GetId());
        hiveManager->PostMessage(mailbox, request);

        if (force) {
            for (auto& pair : tablet->Replicas()) {
                auto replica = pair.first;
                auto& replicaInfo = pair.second;
                if (replicaInfo.GetState() != ETableReplicaState::Disabling) {
                    continue;
                }
                replicaInfo.SetState(ETableReplicaState::Disabled);
                CheckForReplicaDisabled(replica);
            }

            DoTabletUnmounted(tablet);
        }
    }

    void ValidateTabletStaticMemoryUpdate(
        const TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        const TTableMountConfigPtr& mountConfig,
        bool remount)
    {
        i64 oldMemorySize = 0;
        i64 newMemorySize = 0;

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            const auto* tablet = table->Tablets()[index];
            if (remount && !tablet->IsActive()) {
                continue;
            }
            if (remount) {
                oldMemorySize += tablet->GetTabletStaticMemorySize();
            }
            newMemorySize += tablet->GetTabletStaticMemorySize(mountConfig->InMemoryMode);
        }

        auto memorySize = newMemorySize - oldMemorySize;
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidateResourceUsageIncrease(
            table->GetAccount(),
            TClusterResources().SetTabletStaticMemory(memorySize));
    }

    void CommitTabletStaticMemoryUpdate(TTableNode* table)
    {
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->UpdateAccountNodeUsage(table);
    }

    void ValidateTableMountConfig(
        const TTableNode* table,
        const TTableMountConfigPtr& mountConfig)
    {
        if (table->IsReplicated() && mountConfig->InMemoryMode != EInMemoryMode::None) {
            THROW_ERROR_EXCEPTION("Cannot mount a replicated dynamic table in memory");
        }
        if (!table->IsPhysicallySorted() && mountConfig->EnableLookupHashTable) {
            THROW_ERROR_EXCEPTION("\"enable_lookup_hash_table\" can be \"true\" only for sorted dynamic table");
        }
    }

    void GetTableSettings(
        TTableNode* table,
        TTableMountConfigPtr* mountConfig,
        NTabletNode::TTabletChunkReaderConfigPtr *readerConfig,
        NTabletNode::TTabletChunkWriterConfigPtr* writerConfig,
        TTableWriterOptionsPtr* writerOptions)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto tableProxy = objectManager->GetProxy(table);
        const auto& tableAttributes = tableProxy->Attributes();

        // Parse and prepare mount config.
        try {
            *mountConfig = ConvertTo<TTableMountConfigPtr>(tableAttributes);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing table mount configuration")
                << ex;
        }

        // Prepare table reader config.
        *readerConfig = Config_->ChunkReader;

        // Parse and prepare table writer config.
        try {
            *writerConfig = UpdateYsonSerializable(
                Config_->ChunkWriter,
                tableAttributes.FindYson("chunk_writer"));
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing chunk writer config")
                << ex;
        }

        // Prepare tablet writer options.
        const auto& chunkProperties = table->Properties();
        auto primaryMediumIndex = table->GetPrimaryMediumIndex();
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* primaryMedium = chunkManager->GetMediumByIndex(primaryMediumIndex);
        *writerOptions = New<TTableWriterOptions>();
        (*writerOptions)->ReplicationFactor = chunkProperties[primaryMediumIndex].GetReplicationFactor();
        (*writerOptions)->MediumName = primaryMedium->GetName();
        (*writerOptions)->Account = table->GetAccount()->GetName();
        (*writerOptions)->CompressionCodec = table->GetCompressionCodec();
        (*writerOptions)->ErasureCodec = table->GetErasureCodec();
        (*writerOptions)->ChunksVital = chunkProperties.GetVital();
        (*writerOptions)->OptimizeFor = table->GetOptimizeFor();
    }

    static void ParseTabletRange(
        TTableNode* table,
        int* first,
        int* last)
    {
        auto& tablets = table->Tablets();
        if (*first == -1 && *last == -1) {
            *first = 0;
            *last = static_cast<int>(tablets.size() - 1);
        } else {
            if (*first < 0 || *first >= tablets.size()) {
                THROW_ERROR_EXCEPTION("First tablet index %v is out of range [%v, %v]",
                    *first,
                    0,
                    tablets.size() - 1);
            }
            if (*last < 0 || *last >= tablets.size()) {
                THROW_ERROR_EXCEPTION("Last tablet index %v is out of range [%v, %v]",
                    *last,
                    0,
                    tablets.size() - 1);
            }
            if (*first > *last) {
                THROW_ERROR_EXCEPTION("First tablet index is greater than last tablet index");
            }
        }
    }


    IMapNodePtr GetCellMapNode()
    {
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto resolver = cypressManager->CreateResolver();
        return resolver->ResolvePath("//sys/tablet_cells")->AsMap();
    }

    INodePtr FindCellNode(const TCellId& cellId)
    {
        auto cellMapNodeProxy = GetCellMapNode();
        return cellMapNodeProxy->FindChild(ToString(cellId));
    }


    void OnCleanup()
    {
        try {
            const auto& cypressManager = Bootstrap_->GetCypressManager();
            auto resolver = cypressManager->CreateResolver();
            for (const auto& pair : TabletCellMap_) {
                const auto& cellId = pair.first;
                const auto* cell = pair.second;
                if (!IsObjectAlive(cell)) {
                    continue;
                }

                auto snapshotsPath = Format("//sys/tablet_cells/%v/snapshots", cellId);
                IMapNodePtr snapshotsMap;
                try {
                    snapshotsMap = resolver->ResolvePath(snapshotsPath)->AsMap();
                } catch (const std::exception&) {
                    continue;
                }

                std::vector<int> snapshotIds;
                auto snapshotKeys = SyncYPathList(snapshotsMap, "");
                for (const auto& key : snapshotKeys) {
                    int snapshotId;
                    try {
                        snapshotId = FromString<int>(key);
                    } catch (const std::exception& ex) {
                        LOG_WARNING("Unrecognized item %Qv in tablet snapshot store (CellId: %v)",
                            key,
                            cellId);
                        continue;
                    }
                    snapshotIds.push_back(snapshotId);
                }

                if (snapshotIds.size() <= Config_->MaxSnapshotsToKeep)
                    continue;

                std::sort(snapshotIds.begin(), snapshotIds.end());
                int thresholdId = snapshotIds[snapshotIds.size() - Config_->MaxSnapshotsToKeep];

                const auto& objectManager = Bootstrap_->GetObjectManager();
                auto rootService = objectManager->GetRootService();

                for (const auto& key : snapshotKeys) {
                    try {
                        int snapshotId = FromString<int>(key);
                        if (snapshotId < thresholdId) {
                            LOG_INFO("Removing tablet cell snapshot %v (CellId: %v)",
                                snapshotId,
                                cellId);
                            auto req = TYPathProxy::Remove(snapshotsPath + "/" + key);
                            ExecuteVerb(rootService, req).Subscribe(BIND([=] (const TYPathProxy::TErrorOrRspRemovePtr& rspOrError) {
                                if (rspOrError.IsOK()) {
                                    LOG_INFO("Tablet cell snapshot %v removed successfully (CellId: %v)",
                                        snapshotId,
                                        cellId);
                                } else {
                                    LOG_INFO(rspOrError, "Error removing tablet cell snapshot %v (CellId: %v)",
                                        snapshotId,
                                        cellId);
                                }
                            }));
                        }
                    } catch (const std::exception& ex) {
                        // Ignore, cf. logging above.
                    }
                }

                auto changelogsPath = Format("//sys/tablet_cells/%v/changelogs", cellId);
                IMapNodePtr changelogsMap;
                try {
                    changelogsMap = resolver->ResolvePath(changelogsPath)->AsMap();
                } catch (const std::exception&) {
                    continue;
                }

                auto changelogKeys = SyncYPathList(changelogsMap, "");
                for (const auto& key : changelogKeys) {
                    int changelogId;
                    try {
                        changelogId = FromString<int>(key);
                    } catch (const std::exception& ex) {
                        LOG_WARNING("Unrecognized item %Qv in tablet changelog store (CellId: %v)",
                            key,
                            cellId);
                        continue;
                    }
                    if (changelogId < thresholdId) {
                        LOG_INFO("Removing tablet cell changelog %v (CellId: %v)",
                            changelogId,
                            cellId);
                        auto req = TYPathProxy::Remove(changelogsPath + "/" + key);
                        ExecuteVerb(rootService, req).Subscribe(BIND([=] (const TYPathProxy::TErrorOrRspRemovePtr& rspOrError) {
                            if (rspOrError.IsOK()) {
                                LOG_INFO("Tablet cell changelog %v removed successfully (CellId: %v)",
                                    changelogId,
                                    cellId);
                            } else {
                                LOG_INFO(rspOrError, "Error removing tablet cell changelog %v (CellId: %v)",
                                    changelogId,
                                    cellId);
                            }
                        }));;
                    }
                }
            }
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error performing tablets cleanup");
        }
    }

    std::pair<std::vector<TTablet*>::iterator, std::vector<TTablet*>::iterator> GetIntersectingTablets(
        std::vector<TTablet*>& tablets,
        const TOwningKey& minKey,
        const TOwningKey& maxKey)
    {
        auto beginIt = std::upper_bound(
            tablets.begin(),
            tablets.end(),
            minKey,
            [] (const TOwningKey& key, const TTablet* tablet) {
                return key < tablet->GetPivotKey();
            });

        if (beginIt != tablets.begin()) {
            --beginIt;
        }

        auto endIt = beginIt;
        while (endIt != tablets.end() && maxKey >= (*endIt)->GetPivotKey()) {
            ++endIt;
        }

        return std::make_pair(beginIt, endIt);
    }

    void OnReplicateKeysToSecondaryMaster(TCellTag cellTag)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();

        auto tabletCellBundles = GetValuesSortedByKey(TabletCellBundleMap_);
        for (auto* tabletCellBundle : tabletCellBundles) {
            objectManager->ReplicateObjectCreationToSecondaryMaster(tabletCellBundle, cellTag);
        }

        auto tabletCells = GetValuesSortedByKey(TabletCellMap_);
        for (auto* tabletCell : tabletCells) {
            objectManager->ReplicateObjectCreationToSecondaryMaster(tabletCell, cellTag);
        }
    }

    void OnReplicateValuesToSecondaryMaster(TCellTag cellTag)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();

        auto tabletCellBundles = GetValuesSortedByKey(TabletCellBundleMap_);
        for (auto* tabletCellBundle : tabletCellBundles) {
            objectManager->ReplicateObjectAttributesToSecondaryMaster(tabletCellBundle, cellTag);
        }

        auto tabletCells = GetValuesSortedByKey(TabletCellMap_);
        for (auto* tabletCell : tabletCells) {
            objectManager->ReplicateObjectAttributesToSecondaryMaster(tabletCell, cellTag);
        }
    }

    static void ValidateTabletCellBundleName(const TString& name)
    {
        if (name.empty()) {
            THROW_ERROR_EXCEPTION("Tablet cell bundle name cannot be empty");
        }
    }


    static void PopulateTableReplicaDescriptor(TTableReplicaDescriptor* descriptor, const TTableReplica* replica, const TTableReplicaInfo& info)
    {
        ToProto(descriptor->mutable_replica_id(), replica->GetId());
        descriptor->set_cluster_name(replica->GetClusterName());
        descriptor->set_replica_path(replica->GetReplicaPath());
        descriptor->set_start_replication_timestamp(replica->GetStartReplicationTimestamp());
        descriptor->set_mode(static_cast<int>(replica->GetMode()));
        PopulateTableReplicaStatisticsFromInfo(descriptor->mutable_statistics(), info);
    }

    static void PopulateTableReplicaStatisticsFromInfo(TTableReplicaStatistics* statistics, const TTableReplicaInfo& info)
    {
        statistics->set_current_replication_row_index(info.GetCurrentReplicationRowIndex());
        statistics->set_current_replication_timestamp(info.GetCurrentReplicationTimestamp());
    }

    static void PopulateTableReplicaInfoFromStatistics(TTableReplicaInfo* info, const TTableReplicaStatistics& statistics)
    {
        // Updates may be reordered but we can rely on monotonicity here.
        info->SetCurrentReplicationRowIndex(std::max(
            info->GetCurrentReplicationRowIndex(),
            statistics.current_replication_row_index()));
        info->SetCurrentReplicationTimestamp(std::max(
            info->GetCurrentReplicationTimestamp(),
            statistics.current_replication_timestamp()));
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, TabletCellBundle, TTabletCellBundle, TabletCellBundleMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, TabletCell, TTabletCell, TabletCellMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, Tablet, TTablet, TabletMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, TableReplica, TTableReplica, TableReplicaMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, TabletAction, TTabletAction, TabletActionMap_)

////////////////////////////////////////////////////////////////////////////////

TTabletManager::TTabletManager(
    TTabletManagerConfigPtr config,
    NCellMaster::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TTabletManager::~TTabletManager() = default;

void TTabletManager::Initialize()
{
    return Impl_->Initialize();
}

int TTabletManager::GetAssignedTabletCellCount(const TString& address) const
{
    return Impl_->GetAssignedTabletCellCount(address);
}

TTabletStatistics TTabletManager::GetTabletStatistics(const TTablet* tablet)
{
    return Impl_->GetTabletStatistics(tablet);
}

void TTabletManager::MountTable(
    TTableNode* table,
    int firstTabletIndex,
    int lastTabletIndex,
    TTabletCell* hintCell,
    bool freeze)
{
    Impl_->MountTable(
        table,
        firstTabletIndex,
        lastTabletIndex,
        hintCell,
        freeze);
}

void TTabletManager::UnmountTable(
    TTableNode* table,
    bool force,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->UnmountTable(
        table,
        force,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::RemountTable(
    TTableNode* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->RemountTable(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::FreezeTable(
    TTableNode* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->FreezeTable(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::UnfreezeTable(
    TTableNode* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->UnfreezeTable(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::DestroyTable(TTableNode* table)
{
    Impl_->DestroyTable(table);
}

void TTabletManager::ReshardTable(
    TTableNode* table,
    int firstTabletIndex,
    int lastTabletIndex,
    int newTabletCount,
    const std::vector<TOwningKey>& pivotKeys)
{
    Impl_->ReshardTable(
        table,
        firstTabletIndex,
        lastTabletIndex,
        newTabletCount,
        pivotKeys);
}

void TTabletManager::CloneTable(
    TTableNode* sourceTable,
    TTableNode* clonedTable,
    TTransaction* transaction,
    ENodeCloneMode mode)
{
    return Impl_->CloneTable(
        sourceTable,
        clonedTable,
        transaction,
        mode);
}

void TTabletManager::MakeTableDynamic(TTableNode* table)
{
    Impl_->MakeTableDynamic(table);
}

void TTabletManager::MakeTableStatic(TTableNode* table)
{
    Impl_->MakeTableStatic(table);
}

TTablet* TTabletManager::GetTabletOrThrow(const TTabletId& id)
{
    return Impl_->GetTabletOrThrow(id);
}

TTabletCell* TTabletManager::GetTabletCellOrThrow(const TTabletCellId& id)
{
    return Impl_->GetTabletCellOrThrow(id);
}

TTabletCellBundle* TTabletManager::FindTabletCellBundleByName(const TString& name)
{
    return Impl_->FindTabletCellBundleByName(name);
}

TTabletCellBundle* TTabletManager::GetTabletCellBundleByNameOrThrow(const TString& name)
{
    return Impl_->GetTabletCellBundleByNameOrThrow(name);
}

void TTabletManager::RenameTabletCellBundle(TTabletCellBundle* cellBundle, const TString& newName)
{
    return Impl_->RenameTabletCellBundle(cellBundle, newName);
}

TTabletCellBundle* TTabletManager::GetDefaultTabletCellBundle()
{
    return Impl_->GetDefaultTabletCellBundle();
}

void TTabletManager::SetTabletCellBundle(TTableNode* table, TTabletCellBundle* cellBundle)
{
    Impl_->SetTabletCellBundle(table, cellBundle);
}

void TTabletManager::DestroyTablet(TTablet* tablet)
{
    Impl_->DestroyTablet(tablet);
}

TTabletCell* TTabletManager::CreateTabletCell(TTabletCellBundle* cellBundle, const TObjectId& hintId)
{
    return Impl_->CreateTabletCell(cellBundle, hintId);
}

void TTabletManager::DestroyTabletCell(TTabletCell* cell)
{
    Impl_->DestroyTabletCell(cell);
}

TTabletCellBundle* TTabletManager::CreateTabletCellBundle(const TString& name, const TObjectId& hintId)
{
    return Impl_->CreateTabletCellBundle(name, hintId);
}

void TTabletManager::DestroyTabletCellBundle(TTabletCellBundle* cellBundle)
{
    Impl_->DestroyTabletCellBundle(cellBundle);
}

TTableReplica* TTabletManager::CreateTableReplica(
    TReplicatedTableNode* table,
    const TString& clusterName,
    const TYPath& replicaPath,
    ETableReplicaMode mode,
    TTimestamp startReplicationTimestamp)
{
    return Impl_->CreateTableReplica(
        table,
        clusterName,
        replicaPath,
        mode,
        startReplicationTimestamp);
}

void TTabletManager::DestroyTableReplica(TTableReplica* replica)
{
    Impl_->DestroyTableReplica(replica);
}

void TTabletManager::SetTableReplicaEnabled(TTableReplica* replica, bool enabled)
{
    Impl_->SetTableReplicaEnabled(replica, enabled);
}

void TTabletManager::SetTableReplicaMode(TTableReplica* replica, ETableReplicaMode mode)
{
    Impl_->SetTableReplicaMode(replica, mode);
}

TTabletAction* TTabletManager::CreateTabletAction(
    const NObjectClient::TObjectId& hintId,
    ETabletActionKind kind,
    const std::vector<TTablet*>& tabletIds,
    const std::vector<TTabletCell*>& cellIds,
    const std::vector<NTableClient::TOwningKey>& pivotKeys,
    const TNullable<int>& tabletCount,
    bool skipFreezing,
    const TNullable<bool>& freeze,
    bool keepFinished)
{
    return Impl_->CreateTabletAction(
        hintId,
        kind,
        tabletIds,
        cellIds,
        pivotKeys,
        tabletCount,
        skipFreezing,
        freeze,
        keepFinished);
}

void TTabletManager::DestroyTabletAction(TTabletAction* action)
{
    Impl_->DestroyTabletAction(action);
}

DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, TabletCellBundle, TTabletCellBundle, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, TabletCell, TTabletCell, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, Tablet, TTablet, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, TableReplica, TTableReplica, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, TabletAction, TTabletAction, *Impl_)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
