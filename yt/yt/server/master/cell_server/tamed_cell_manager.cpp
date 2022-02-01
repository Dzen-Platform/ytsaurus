#include "bundle_node_tracker.h"
#include "config.h"
#include "private.h"
#include "cell_base.h"
#include "cell_bundle.h"
#include "cell_bundle_type_handler.h"
#include "cell_type_handler_base.h"
#include "cellar_node_tracker.h"
#include "tamed_cell_manager.h"
#include "cell_tracker.h"
#include "area.h"
#include "area_type_handler.h"

#include <yt/yt/server/master/tablet_server/tablet_cell_decommissioner.h>
#include <yt/yt/server/master/tablet_server/cypress_integration.h>
#include <yt/yt/server/master/tablet_server/tablet_manager.h>

#include <yt/yt/server/master/cell_master/config.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>
#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/server/master/chunk_server/chunk_list.h>
#include <yt/yt/server/master/chunk_server/chunk_view.h>
#include <yt/yt/server/master/chunk_server/chunk_manager.h>
#include <yt/yt/server/master/chunk_server/chunk_tree_traverser.h>
#include <yt/yt/server/master/chunk_server/helpers.h>
#include <yt/yt/server/master/chunk_server/medium.h>

#include <yt/yt/server/master/cypress_server/cypress_manager.h>

#include <yt/yt/server/master/table_server/table_node.h>

#include <yt/yt/server/lib/hive/hive_manager.h>
#include <yt/yt/server/lib/hive/helpers.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/server/master/node_tracker_server/node.h>
#include <yt/yt/server/master/node_tracker_server/node_tracker.h>

#include <yt/yt/server/master/object_server/object_manager.h>

#include <yt/yt/server/master/security_server/security_manager.h>
#include <yt/yt/server/master/security_server/group.h>
#include <yt/yt/server/master/security_server/subject.h>

#include <yt/yt/server/lib/cellar_agent/helpers.h>

#include <yt/yt/server/lib/tablet_server/proto/tablet_manager.pb.h>

#include <yt/yt/server/lib/cell_server/proto/cell_manager.pb.h>

#include <yt/yt/server/lib/hydra_common/mutation_context.h>

#include <yt/yt/ytlib/cell_balancer/proto/cell_tracker_service.pb.h>

#include <yt/yt/ytlib/cellar_client/public.h>

#include <yt/yt/ytlib/election/config.h>

#include <yt/yt/ytlib/hive/cell_directory.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/yt/ytlib/tablet_client/config.h>

#include <yt/yt/ytlib/transaction_client/helpers.h>

#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/throughput_throttler.h>

#include <yt/yt/core/misc/collection_helpers.h>
#include <yt/yt/core/misc/random_access_queue.h>
#include <yt/yt/core/misc/string.h>

#include <yt/yt/core/ypath/token.h>

#include <algorithm>

namespace NYT::NCellServer {

using namespace NCellBalancerClient::NProto;
using namespace NCellMaster;
using namespace NCellarClient;
using namespace NCellarAgent;
using namespace NChunkClient::NProto;
using namespace NChunkClient;
using namespace NChunkServer;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NCypressServer;
using namespace NHiveClient;
using namespace NHiveServer;
using namespace NHydra;
using namespace NNodeTrackerClient::NProto;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerServer::NProto;
using namespace NNodeTrackerServer;
using namespace NObjectClient::NProto;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NProfiling;
using namespace NSecurityServer;
using namespace NTableServer;
using namespace NCellarNodeTrackerClient::NProto;
using namespace NTabletServer::NProto;
using namespace NTransactionClient;
using namespace NTransactionServer;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;

using NNodeTrackerClient::TNodeDescriptor;
using NTransactionServer::TTransaction;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CellServerLogger;

static const auto ProfilingPeriod = TDuration::Seconds(10);

////////////////////////////////////////////////////////////////////////////////

class TTamedCellManager
    : public ITamedCellManager
    , public TMasterAutomatonPart
{
public:
    DEFINE_SIGNAL_OVERRIDE(void(TCellBundle* bundle), CellBundleDestroyed);
    DEFINE_SIGNAL_OVERRIDE(void(TArea* area), AreaCreated);
    DEFINE_SIGNAL_OVERRIDE(void(TArea* area), AreaDestroyed);
    DEFINE_SIGNAL_OVERRIDE(void(TArea* area), AreaNodeTagFilterChanged);
    DEFINE_SIGNAL_OVERRIDE(void(TCellBase* cell), CellCreated);
    DEFINE_SIGNAL_OVERRIDE(void(TCellBase* cell), CellDecommissionStarted);
    DEFINE_SIGNAL_OVERRIDE(void(), CellPeersAssigned);
    DEFINE_SIGNAL_OVERRIDE(void(), AfterSnapshotLoaded);

public:
    explicit TTamedCellManager(NCellMaster::TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap, NCellMaster::EAutomatonThreadQueue::TamedCellManager)
        , CellTracker_(New<TCellTracker>(Bootstrap_))
        , BundleNodeTracker_(New<TBundleNodeTracker>(Bootstrap_))
        , CellBundleMap_(TEntityMapTypeTraits<TCellBundle>(Bootstrap_))
        , CellMap_(TEntityMapTypeTraits<TCellBase>(Bootstrap_))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(
            Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(EAutomatonThreadQueue::Default), AutomatonThread);

        RegisterLoader(
            "CellManager.Keys",
            BIND(&TTamedCellManager::LoadKeys, Unretained(this)));
        RegisterLoader(
            "CellManager.Values",
            BIND(&TTamedCellManager::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "CellManager.Keys",
            BIND(&TTamedCellManager::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "CellManager.Values",
            BIND(&TTamedCellManager::SaveValues, Unretained(this)));

        // COMPAT(alexkolodezny)
        RegisterMethod(BIND(&TTamedCellManager::HydraAssignPeers, Unretained(this)), /*aliases*/ {"NYT.NTabletServer.NProto.TReqAssignPeers"});
        RegisterMethod(BIND(&TTamedCellManager::HydraRevokePeers, Unretained(this)), /*aliases*/ {"NYT.NTabletServer.NProto.TReqRevokePeers"});
        RegisterMethod(BIND(&TTamedCellManager::HydraReassignPeers, Unretained(this)), /*aliases*/ {"NYT.NTabletServer.NProto.TReqReassignPeers"});
        RegisterMethod(BIND(&TTamedCellManager::HydraSetLeadingPeer, Unretained(this)), /*aliases*/ {"NYT.NTabletServer.NProto.TReqSetLeadingPeer"});
        RegisterMethod(BIND(&TTamedCellManager::HydraStartPrerequisiteTransaction, Unretained(this)));
        RegisterMethod(BIND(&TTamedCellManager::HydraAbortPrerequisiteTransaction, Unretained(this)));
        RegisterMethod(BIND(&TTamedCellManager::HydraDecommissionCellOnMaster, Unretained(this)));
        RegisterMethod(BIND(&TTamedCellManager::HydraOnCellDecommissionedOnNode, Unretained(this)));
        RegisterMethod(BIND(&TTamedCellManager::HydraOnCellDecommissionedOnMaster, Unretained(this)));
        RegisterMethod(BIND(&TTamedCellManager::HydraSetCellConfigVersion, Unretained(this)));
        RegisterMethod(BIND(&TTamedCellManager::HydraSetCellStatus, Unretained(this)));
        RegisterMethod(BIND(&TTamedCellManager::HydraUpdateCellHealth, Unretained(this)));
        RegisterMethod(BIND(&TTamedCellManager::HydraUpdatePeerCount, Unretained(this)), /*aliases*/ {"NYT.NTabletServer.NProto.TReqUpdatePeerCount"});
    }

    void Initialize() override
    {
        const auto& nodeTracker = Bootstrap_->GetNodeTracker();
        nodeTracker->SubscribeNodeUnregistered(BIND(&TTamedCellManager::OnNodeUnregistered, MakeWeak(this)));

        const auto& cellarNodeTracker = Bootstrap_->GetCellarNodeTracker();
        cellarNodeTracker->SubscribeHeartbeat(BIND(&TTamedCellManager::OnCellarNodeHeartbeat, MakeWeak(this)));

        const auto& configManager = Bootstrap_->GetConfigManager();
        configManager->SubscribeConfigChanged(BIND(&TTamedCellManager::OnDynamicConfigChanged, MakeWeak(this)));

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(CreateAreaTypeHandler(Bootstrap_, &AreaMap_));

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->SubscribeTransactionCommitted(BIND(&TTamedCellManager::OnTransactionFinished, MakeWeak(this)));
        transactionManager->SubscribeTransactionAborted(BIND(&TTamedCellManager::OnTransactionFinished, MakeWeak(this)));

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            multicellManager->SubscribeReplicateKeysToSecondaryMaster(
                BIND(&TTamedCellManager::OnReplicateKeysToSecondaryMaster, MakeWeak(this)));
            multicellManager->SubscribeReplicateValuesToSecondaryMaster(
                BIND(&TTamedCellManager::OnReplicateValuesToSecondaryMaster, MakeWeak(this)));
        }

        BundleNodeTracker_->Initialize();

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(EAutomatonThreadQueue::Periodic),
            BIND(&TTamedCellManager::OnProfiling, MakeWeak(this)),
            ProfilingPeriod);
        ProfilingExecutor_->Start();
    }

    TCellBundle* CreateCellBundle(
        const TString& name,
        std::unique_ptr<TCellBundle> holder,
        TTabletCellOptionsPtr options) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        ValidateCellBundleName(name);

        if (FindCellBundleByName(name, holder->GetCellarType(), false /*activeLifeStageOnly*/)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Cell bundle %Qv already exists",
                name);
        }

        auto cellBundleId = holder->GetId();
        auto* cellBundle = CellBundleMap_.Insert(cellBundleId, std::move(holder));

        cellBundle->SetName(name);

        EmplaceOrCrash(NameToCellBundleMap_[cellBundle->GetCellarType()], cellBundle->GetName(), cellBundle);
        InsertOrCrash(CellBundlesPerTypeMap_[cellBundle->GetCellarType()], cellBundle);
        cellBundle->SetOptions(std::move(options));

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(cellBundle);

        CreateDefaultArea(cellBundle);

        return cellBundle;
    }

    void ZombifyCellBundle(TCellBundle* cellBundle) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        YT_VERIFY(cellBundle->Cells().empty());

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto areas = cellBundle->Areas();
        for (const auto& [_, area] : areas) {
            // NB: It is forbidden to remove bundle with active areas. This branch is possible only when bundle creation failed.
            YT_VERIFY(area == cellBundle->GetDefaultArea());
            objectManager->UnrefObject(area);
        }
        YT_VERIFY(cellBundle->Areas().empty());

        // Remove cell bundle from maps.
        EraseOrCrash(NameToCellBundleMap_[cellBundle->GetCellarType()], cellBundle->GetName());
        EraseOrCrash(CellBundlesPerTypeMap_[cellBundle->GetCellarType()], cellBundle);

        CellBundleDestroyed_.Fire(cellBundle);
    }

    void DestroyCellBundle(TCellBundle* cellBundle) override
    {
        Y_UNUSED(CellBundleMap_.Release(cellBundle->GetId()).release());
    }

    void SetCellBundleOptions(TCellBundle* cellBundle, TTabletCellOptionsPtr newOptions) override
    {
        Bootstrap_->GetSecurityManager()->ValidatePermission(cellBundle, EPermission::Use);

        const auto& currentOptions = cellBundle->GetOptions();
        if (newOptions->PeerCount != currentOptions->PeerCount && !cellBundle->Cells().empty()) {
            THROW_ERROR_EXCEPTION("Cannot change peer count since cell bundle has %v cell(s)",
                cellBundle->Cells().size());
        }
        if (newOptions->IndependentPeers != currentOptions->IndependentPeers && !cellBundle->Cells().empty()) {
            THROW_ERROR_EXCEPTION("Cannot change peer independency since bundle has %v cell(s)",
                cellBundle->Cells().size());
        }
        if (cellBundle->GetType() == EObjectType::ChaosCellBundle) {
            if (!newOptions->IndependentPeers) {
                THROW_ERROR_EXCEPTION("Chaos cells must always have independent peers");
            }
            if (newOptions->PeerCount != currentOptions->PeerCount) {
                THROW_ERROR_EXCEPTION("Cannot change peer count for chaos cell bundle");
            }
        }

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        if (currentOptions->SnapshotAccount != newOptions->SnapshotAccount) {
            auto* account = securityManager->GetAccountByNameOrThrow(newOptions->SnapshotAccount, /*activeLifeStageOnly*/ true);
            securityManager->ValidatePermission(account, EPermission::Use);
        }
        if (currentOptions->ChangelogAccount != newOptions->ChangelogAccount) {
            auto* account = securityManager->GetAccountByNameOrThrow(newOptions->ChangelogAccount, /*activeLifeStageOnly*/ true);
            securityManager->ValidatePermission(account, EPermission::Use);
        }

        auto snapshotAcl = ConvertToYsonString(newOptions->SnapshotAcl, EYsonFormat::Binary).ToString();
        auto changelogAcl = ConvertToYsonString(newOptions->ChangelogAcl, EYsonFormat::Binary).ToString();

        cellBundle->SetOptions(std::move(newOptions));

        auto* rootUser = securityManager->GetRootUser();

        for (auto* cell : GetValuesSortedByKey(cellBundle->Cells())) {
            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            if (multicellManager->IsPrimaryMaster()) {
                if (auto node = FindCellNode(cell->GetId())) {
                    TAuthenticatedUserGuard userGuard(securityManager, rootUser);

                    auto cellNode = node->AsMap();
                    try {
                        {
                            auto req = TCypressYPathProxy::Set("/snapshots/@acl");
                            req->set_value(snapshotAcl);
                            SyncExecuteVerb(cellNode, req);
                        }
                        {
                            auto req = TCypressYPathProxy::Set("/changelogs/@acl");
                            req->set_value(changelogAcl);
                            SyncExecuteVerb(cellNode, req);
                        }
                    } catch (const std::exception& ex) {
                        YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), ex,
                            "Caught exception while changing ACL (Bundle: %v, TabletCellId: %v)",
                            cellBundle->GetName(),
                            cell->GetId());
                    }
                }

                RestartAllPrerequisiteTransactions(cell);
            }

            ReconfigureCell(cell);
        }
    }

    TArea* CreateDefaultArea(TCellBundle* cellBundle)
    {
        auto areaId = ReplaceTypeInId(cellBundle->GetId(), EObjectType::Area);
        auto* area = CreateArea(DefaultAreaName, cellBundle, areaId);
        if (area->GetNativeCellTag() != Bootstrap_->GetCellTag()) {
            area->SetForeign();
        }
        return area;
    }

    TArea* CreateArea(
        const TString& name,
        TCellBundle* cellBundle,
        TObjectId hintId) override
    {
        ValidateAreaName(name);

        if (cellBundle->Areas().contains(name)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Area %Qv already exists at cell bundle %Qv",
                name,
                cellBundle->GetName());
        }

        if (cellBundle->Areas().size() >= MaxAreaCount) {
            THROW_ERROR_EXCEPTION("Area count limit %v is reached",
                MaxAreaCount);
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto areaId = objectManager->GenerateId(EObjectType::Area, hintId);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Creating area (CellBundle: %v, Area: %v, AreaId: %v)",
            cellBundle->GetName(),
            name,
            areaId);

        auto areaHolder = TPoolAllocator::New<TArea>(areaId);
        auto* area = AreaMap_.Insert(areaId, std::move(areaHolder));

        area->SetName(name);
        area->SetCellBundle(cellBundle);

        EmplaceOrCrash(cellBundle->Areas(), name, area);
        if (name == DefaultAreaName) {
            cellBundle->SetDefaultArea(area);
        }

        // Make the fake reference.
        YT_VERIFY(area->RefObject() == 1);

        AreaCreated_.Fire(area);

        return area;
    }

    void ZombifyArea(TArea* area) override
    {
        YT_VERIFY(area->Cells().empty());

        AreaDestroyed_.Fire(area);

        auto* cellBundle = area->GetCellBundle();
        if (cellBundle->GetDefaultArea() == area) {
            cellBundle->SetDefaultArea(nullptr);
        }
        area->SetCellBundle(nullptr);
        EraseOrCrash(cellBundle->Areas(), area->GetName());
    }

    void CreateSnapshotAndChangelogNodes(
        const TString& path,
        const IMapNodePtr& cellMapNodeProxy,
        const IAttributeDictionaryPtr& snapshotAttributes,
        const IAttributeDictionaryPtr& changelogAttributes)
    {
        // Create "snapshots" child.
        {
            auto req = TCypressYPathProxy::Create(path + "/snapshots");
            req->set_type(ToProto<int>(EObjectType::MapNode));
            ToProto(req->mutable_node_attributes(), *snapshotAttributes);
            SyncExecuteVerb(cellMapNodeProxy, req);
        }

        // Create "changelogs" child.
        {
            auto req = TCypressYPathProxy::Create(path + "/changelogs");
            req->set_type(ToProto<int>(EObjectType::MapNode));
            ToProto(req->mutable_node_attributes(), *changelogAttributes);
            SyncExecuteVerb(cellMapNodeProxy, req);
        }
    }

    TCellBase* CreateCell(
        TCellBundle* cellBundle,
        TArea* area,
        std::unique_ptr<TCellBase> holder) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(cellBundle, EPermission::Use);

        auto cellId = holder->GetId();
        auto cellTag = CellTagFromId(cellId);

        if (IsGlobalCellId(cellId) && CellTagToCell_.contains(cellTag)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Cell with tag %v already exists",
                cellTag);
        }

        if (FindCell(cellId)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Cell with id %v already exists",
                cellId);
        }

        int peerCount = cellBundle->GetOptions()->PeerCount;
        if (peerCount <= 0) {
            THROW_ERROR_EXCEPTION("Peer count must be positive");
        }

        auto* cell = CellMap_.Insert(cellId, std::move(holder));

        cell->Peers().resize(peerCount);
        cell->SetCellBundle(cellBundle);
        InsertOrCrash(cellBundle->Cells(), cell);
        InsertOrCrash(CellsPerTypeMap_[cell->GetCellarType()], cell);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(cellBundle);

        cell->SetArea(area);
        InsertOrCrash(area->Cells(), cell);

        if (!cell->IsIndependent()) {
            cell->SetLeadingPeerId(0);
        }

        cell->GossipStatus().Initialize(Bootstrap_);

        MaybeRegisterGlobalCell(cell);
        ReconfigureCell(cell);

        // Make the fake reference.
        YT_VERIFY(cell->RefObject() == 1);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        hiveManager->CreateMailbox(cellId);

        auto cellMapNodeProxy = GetCellMapNode(cellId);
        auto cellNodePath = "/" + ToString(cellId);

        try {
            // NB: Users typically are not allowed to create these types.
            auto* rootUser = securityManager->GetRootUser();
            TAuthenticatedUserGuard userGuard(securityManager, rootUser);

            // Create Cypress node.
            {
                auto req = TCypressYPathProxy::Create(cellNodePath);
                req->set_type(ToProto<int>(EObjectType::TabletCellNode));

                auto attributes = CreateEphemeralAttributes();
                attributes->Set("opaque", true);
                ToProto(req->mutable_node_attributes(), *attributes);

                SyncExecuteVerb(cellMapNodeProxy, req);
            }

            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            if (multicellManager->IsPrimaryMaster()) {
                auto createAttributes = [&] (const auto& acl) {
                    auto attributes = CreateEphemeralAttributes();
                    attributes->Set("inherit_acl", false);
                    attributes->Set("acl", acl);
                    return attributes;
                };

                auto snapshotAttributes = createAttributes(cellBundle->GetOptions()->SnapshotAcl);
                auto changelogAttributes = createAttributes(cellBundle->GetOptions()->ChangelogAcl);

                if (cell->IsIndependent()) {
                    for (TPeerId peerId = 0; peerId < peerCount; ++peerId) {
                        if (cell->IsAlienPeer(peerId)) {
                            continue;
                        }

                        auto peerNodePath = Format("%v/%v", cellNodePath, peerId);

                        {
                            auto req = TCypressYPathProxy::Create(peerNodePath);
                            req->set_type(ToProto<int>(EObjectType::MapNode));
                            SyncExecuteVerb(cellMapNodeProxy, req);
                        }

                        CreateSnapshotAndChangelogNodes(
                            peerNodePath,
                            cellMapNodeProxy,
                            snapshotAttributes,
                            changelogAttributes);
                    }
                } else {
                    CreateSnapshotAndChangelogNodes(
                        cellNodePath,
                        cellMapNodeProxy,
                        snapshotAttributes,
                        changelogAttributes);
                }
            }
        } catch (const std::exception& ex) {
            YT_LOG_ERROR_IF(
                IsMutationLoggingEnabled(),
                ex,
                "Error registering cell in Cypress (CellId: %v)",
                cell->GetId());

            objectManager->UnrefObject(cell);
            THROW_ERROR_EXCEPTION("Error registering cell in Cypress")
                << ex;
        }

        CellCreated_.Fire(cell);

        return cell;
    }

    void ZombifyCell(TCellBase* cell) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = cell->GetId();

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        if (auto* mailbox = hiveManager->FindMailbox(cellId)) {
            hiveManager->RemoveMailbox(mailbox);
        }

        for (TPeerId peerId = 0; peerId < std::ssize(cell->Peers()); ++peerId) {
            const auto& peer = cell->Peers()[peerId];
            if (cell->IsAlienPeer(peerId)) {
                continue;
            }
            if (peer.Node) {
                peer.Node->DetachCell(cell);
            }
            if (!peer.Descriptor.IsNull()) {
                RemoveFromAddressToCellMap(peer.Descriptor, cell);
            }
        }

        // NB: Code below interacts with other master parts and may require root permissions
        // (for example, when aborting a transaction).
        // We want this code to always succeed.
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        auto* rootUser = securityManager->GetRootUser();
        TAuthenticatedUserGuard userGuard(securityManager, rootUser);

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            AbortAllCellTransactions(cell);
        }

        if (auto cellNodeProxy = FindCellNode(cellId)) {
            try {
                // NB: Subtree transactions were already aborted above.
                cellNodeProxy->GetParent()->RemoveChild(cellNodeProxy);
            } catch (const std::exception& ex) {
                YT_LOG_ALERT_IF(
                    IsMutationLoggingEnabled(),
                    ex,
                    "Error unregistering cell from Cypress (CellId: %v)",
                    cellId);
            }
        }

        auto* cellBundle = cell->GetCellBundle();
        EraseOrCrash(cellBundle->Cells(), cell);
        EraseOrCrash(CellsPerTypeMap_[cell->GetCellarType()], cell);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->UnrefObject(cellBundle);
        cell->SetCellBundle(nullptr);

        auto* area = cell->GetArea();
        EraseOrCrash(area->Cells(), cell);
        cell->SetArea(nullptr);

        cell->Peers().clear();
    }

    void DestroyCell(TCellBase* cell) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        Y_UNUSED(CellMap_.Release(cell->GetId()).release());
        MaybeUnregisterGlobalCell(cell);
    }

    void UpdatePeerCount(TCellBase* cell, std::optional<int> peerCount) override
    {
        if (cell->IsIndependent()) {
            YT_LOG_WARNING_IF(
                IsMutationLoggingEnabled(),
                "Attempted to update peer count of independent cell (CellId: %v)",
                cell->GetId());
            return;
        }

        cell->PeerCount() = peerCount;
        cell->LastPeerCountUpdateTime() = TInstant::Now();

        int oldPeerCount = std::ssize(cell->Peers());
        int newPeerCount = cell->GetCellBundle()->GetOptions()->PeerCount;
        if (cell->PeerCount()) {
            newPeerCount = *cell->PeerCount();
        }

        if (oldPeerCount == newPeerCount) {
            return;
        }

        YT_LOG_DEBUG("Updating cell peer count (CellId: %v, OldPeerCount: %v, NewPeerCount: %v)",
            cell->GetId(),
            oldPeerCount,
            newPeerCount);

        bool leaderChanged = false;
        if (newPeerCount > oldPeerCount) {
            cell->Peers().resize(newPeerCount);
        } else {
            // Move leader to the first place to prevent its removing.
            int leaderId = cell->GetLeadingPeerId();
            if (leaderId != 0) {
                leaderChanged = true;
                auto& leaderPeer = cell->Peers()[leaderId];
                auto& firstPeer = cell->Peers()[0];
                if (!leaderPeer.Descriptor.IsNull()) {
                    RemoveFromAddressToCellMap(leaderPeer.Descriptor, cell);
                }
                if (!firstPeer.Descriptor.IsNull()) {
                    RemoveFromAddressToCellMap(firstPeer.Descriptor, cell);
                }
                std::swap(leaderPeer, firstPeer);
                if (!leaderPeer.Descriptor.IsNull()) {
                    YT_VERIFY(AddToAddressToCellMap(leaderPeer.Descriptor, cell, leaderId));
                }
                if (!firstPeer.Descriptor.IsNull()) {
                    YT_VERIFY(AddToAddressToCellMap(firstPeer.Descriptor, cell, 0));
                }
                cell->SetLeadingPeerId(0);
            }

            // Revoke extra peers.
            auto revocationReason = TError("Peer count reduced from %v to %v",
                oldPeerCount,
                newPeerCount);
            for (TPeerId peerId = newPeerCount; peerId < oldPeerCount; ++peerId) {
                DoRevokePeer(cell, peerId, revocationReason);
            }

            cell->Peers().resize(newPeerCount);
        }

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (leaderChanged && multicellManager->IsPrimaryMaster()) {
            RestartAllPrerequisiteTransactions(cell);
        }

        ReconfigureCell(cell);

        // Notify new quorum as soon as possible via heartbeat requests.
        if (multicellManager->IsPrimaryMaster() && IsLeader()) {
            for (const auto& peer : cell->Peers()) {
                if (peer.Node) {
                    Bootstrap_->GetNodeTracker()->RequestCellarHeartbeat(peer.Node->GetId());
                }
            }
        }
    }

    const TCellSet* FindAssignedCells(const TString& address) const override
    {
        auto it = AddressToCell_.find(address);
        return it != AddressToCell_.end()
           ? &it->second
           : nullptr;
    }

    const TBundleNodeTrackerPtr& GetBundleNodeTracker() override
    {
        return BundleNodeTracker_;
    }

    const THashSet<TCellBase*>& Cells(ECellarType cellarType) override
    {
        return CellsPerTypeMap_[cellarType];
    }

    TCellBase* GetCellOrThrow(TTamedCellId cellId) override
    {
        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No cell with id %v is known",
                cellId);
        }
        return cell;
    }

    TCellBase* FindCellByCellTag(TCellTag cellTag) override
    {
        auto it = CellTagToCell_.find(cellTag);
        return it == CellTagToCell_.end() ? nullptr : it->second;
    }

    TCellBase* GetCellByCellTagOrThrow(TCellTag cellTag) override
    {
        auto* cell = FindCellByCellTag(cellTag);
        if (!IsObjectAlive(cell)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No cell with tag %v is known",
                cellTag);
        }
        return cell;
    }

    void RemoveCell(TCellBase* cell, bool force) override
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsPrimaryMaster());

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Removing cell (CellId: %v, Force: %v)",
            cell->GetId(),
            force);

        switch (cell->GetCellLifeStage()) {
            case ECellLifeStage::Running: {
                // Decommission cell on primary master.
                DecommissionCell(cell);

                // Decommission cell on secondary masters.
                NTabletServer::NProto::TReqDecommissionTabletCellOnMaster req;
                ToProto(req.mutable_cell_id(), cell->GetId());
                multicellManager->PostToMasters(req, multicellManager->GetRegisteredMasterCellTags());

                // Decommission cell on node.
                if (force) {
                    OnCellDecommissionedOnNode(cell);
                }

                break;
            }

            case ECellLifeStage::DecommissioningOnMaster:
            case ECellLifeStage::DecommissioningOnNode:
                if (force) {
                    OnCellDecommissionedOnNode(cell);
                }

                break;

            default:
                YT_ABORT();
        }
    }

    void HydraOnCellDecommissionedOnMaster(TReqOnTabletCellDecommisionedOnMaster* request)
    {
        auto cellId = FromProto<TTamedCellId>(request->cell_id());
        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell)) {
            return;
        }

        if (cell->GetCellLifeStage() != ECellLifeStage::DecommissioningOnMaster) {
            return;
        }

        // Decommission cell on node.

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Requesting cell decommission on node (CellId: %v)",
            cell->GetId());

        cell->SetCellLifeStage(ECellLifeStage::DecommissioningOnNode);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        auto* mailbox = hiveManager->GetMailbox(cell->GetId());
        hiveManager->PostMessage(mailbox, TReqDecommissionTabletCellOnNode());
    }

    void HydraDecommissionCellOnMaster(TReqDecommissionTabletCellOnMaster* request)
    {
        auto cellId = FromProto<TTamedCellId>(request->cell_id());
        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell)) {
            return;
        }
        DecommissionCell(cell);
        OnCellDecommissionedOnNode(cell);
    }

    void HydraUpdatePeerCount(TReqUpdatePeerCount* request)
    {
        auto cellId = FromProto<TTamedCellId>(request->cell_id());
        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell)) {
            YT_LOG_WARNING_IF(
                IsMutationLoggingEnabled(),
                "Attempted to update peer count of non-existing cell (CellId: %v)",
                cellId);
            return;
        }

        if (request->has_peer_count()) {
            if (request->peer_count() >= 1) {
                UpdatePeerCount(cell, request->peer_count());
            } else {
                YT_LOG_WARNING_IF(
                    IsMutationLoggingEnabled(),
                    "Attempted to update cell with incorrect peer count (CellId: %v, PeerCount: %v)",
                    cellId,
                    request->peer_count());
            }
        } else {
            UpdatePeerCount(cell, std::nullopt);
        }

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            multicellManager->PostToMasters(*request, multicellManager->GetRegisteredMasterCellTags());
        }
    }

    void DecommissionCell(TCellBase* cell)
    {
        if (cell->IsDecommissionStarted()) {
            return;
        }

        cell->SetCellLifeStage(ECellLifeStage::DecommissioningOnMaster);

        CellDecommissionStarted_.Fire(cell);
    }

    void HydraOnCellDecommissionedOnNode(TRspDecommissionTabletCellOnNode* response)
    {
        auto cellId = FromProto<TTamedCellId>(response->cell_id());
        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell)) {
            return;
        }
        OnCellDecommissionedOnNode(cell);
    }

    void OnCellDecommissionedOnNode(TCellBase* cell)
    {
        if (cell->IsDecommissionCompleted()) {
            return;
        }

        cell->SetCellLifeStage(ECellLifeStage::Decommissioned);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell decommissioned (CellId: %v)",
            cell->GetId());
    }

    const THashSet<TCellBundle*>& CellBundles(ECellarType cellarType) override
    {
        return CellBundlesPerTypeMap_[cellarType];
    }

    TCellBundle* FindCellBundleByName(const TString& name, ECellarType cellarType, bool activeLifeStageOnly) override
    {
        auto* cellBundle = DoFindCellBundleByName(name, cellarType);
        if (!cellBundle) {
            return cellBundle;
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            return objectManager->IsObjectLifeStageValid(cellBundle)
                ? cellBundle
                : nullptr;
        } else {
            return cellBundle;
        }
    }

    TCellBundle* GetCellBundleByNameOrThrow(const TString& name, ECellarType cellarType, bool activeLifeStageOnly) override
    {
        auto* cellBundle = DoFindCellBundleByName(name, cellarType);
        if (!cellBundle) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No such %Qlv cell bundle %Qlv",
                cellarType,
                name);
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->ValidateObjectLifeStage(cellBundle);
        }

        return cellBundle;
    }

    TCellBundle* GetCellBundleByIdOrThrow(TCellBundleId cellBundleId, bool activeLifeStageOnly) override
    {
        auto* cellBundle = FindCellBundle(cellBundleId);
        if (!cellBundle) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No such cell bundle %v",
                cellBundleId);
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->ValidateObjectLifeStage(cellBundle);
        }

        return cellBundle;
    }

    void RenameCellBundle(TCellBundle* cellBundle, const TString& newName) override
    {
        if (newName == cellBundle->GetName()) {
            return;
        }

        ValidateCellBundleName(newName);

        if (FindCellBundleByName(newName, cellBundle->GetCellarType(), false)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Cell bundle %Qv already exists",
                newName);
        }

        EraseOrCrash(NameToCellBundleMap_[cellBundle->GetCellarType()], cellBundle->GetName());
        EmplaceOrCrash(NameToCellBundleMap_[cellBundle->GetCellarType()], newName, cellBundle);
        cellBundle->SetName(newName);
    }

    void RenameArea(TArea* area, const TString& newName) override
    {
        if (newName == area->GetName()) {
            return;
        }

        if (area->GetCellBundle()->GetDefaultArea() == area) {
            // NB: Restrict default area name change to avoid attribute replication problems.
            THROW_ERROR_EXCEPTION("Cannot change default area name");
        }

        ValidateAreaName(newName);

        auto* cellBundle = area->GetCellBundle();

        if (cellBundle->Areas().contains(newName)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Area %Qv already exists at cell bundle %Qv",
                newName,
                cellBundle->GetName());
        }

        EraseOrCrash(cellBundle->Areas(), area->GetName());
        EmplaceOrCrash(cellBundle->Areas(), newName, area);
        area->SetName(newName);
    }

    void SetAreaNodeTagFilter(TArea* area, const TString& formula) override
    {
        if (area->NodeTagFilter().GetFormula() != formula) {
            area->NodeTagFilter() = MakeBooleanFormula(formula);
            AreaNodeTagFilterChanged_.Fire(area);
        }
    }

    DECLARE_ENTITY_MAP_ACCESSORS_OVERRIDE(CellBundle, TCellBundle);
    DECLARE_ENTITY_MAP_ACCESSORS_OVERRIDE(Cell, TCellBase);
    DECLARE_ENTITY_MAP_ACCESSORS_OVERRIDE(Area, TArea);

private:
    template <class T>
    class TEntityMapTypeTraits
    {
    public:
        explicit TEntityMapTypeTraits(TBootstrap* bootstrap)
            : Bootstrap_(bootstrap)
        { }

        std::unique_ptr<T> Create(TObjectId id) const
        {
            auto type = TypeFromId(id);
            const auto& objectManager = Bootstrap_->GetObjectManager();
            const auto& handler = objectManager->FindHandler(type);
            auto objectHolder = handler->InstantiateObject(id);
            return std::unique_ptr<T>(objectHolder.release()->As<T>());
        }

    private:
        TBootstrap* const Bootstrap_;
    };

    const TCellTrackerPtr CellTracker_;
    const TBundleNodeTrackerPtr BundleNodeTracker_;

    TEntityMap<TCellBundle, TEntityMapTypeTraits<TCellBundle>> CellBundleMap_;
    TEntityMap<TCellBase, TEntityMapTypeTraits<TCellBase>> CellMap_;
    TEntityMap<TArea> AreaMap_;

    THashMap<ECellarType, THashSet<TCellBundle*>> CellBundlesPerTypeMap_;
    THashMap<ECellarType, THashSet<TCellBase*>> CellsPerTypeMap_;

    THashMap<ECellarType, THashMap<TString, TCellBundle*>> NameToCellBundleMap_;

    THashMap<TCellTag, TCellBase*> CellTagToCell_;
    THashMap<TString, TCellSet> AddressToCell_;
    THashMap<TTransaction*, std::pair<TCellBase*, std::optional<TPeerId>>> TransactionToCellMap_;

    TPeriodicExecutorPtr CellStatusIncrementalGossipExecutor_;
    TPeriodicExecutorPtr CellStatusFullGossipExecutor_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

    TPeriodicExecutorPtr ProfilingExecutor_;


    const NTabletServer::TDynamicTabletManagerConfigPtr& GetDynamicConfig()
    {
        return Bootstrap_->GetConfigManager()->GetConfig()->TabletManager;
    }

    void OnDynamicConfigChanged(TDynamicClusterConfigPtr /*oldConfig*/ = nullptr)
    {
        const auto& config = GetDynamicConfig();
        const auto& gossipConfig = config->MulticellGossip;

        if (CellStatusFullGossipExecutor_) {
            auto gossipPeriod = gossipConfig->TabletCellStatusFullGossipPeriod.value_or(gossipConfig->TabletCellStatisticsGossipPeriod);
            CellStatusFullGossipExecutor_->SetPeriod(gossipPeriod);
        }
        if (CellStatusIncrementalGossipExecutor_) {
            CellStatusIncrementalGossipExecutor_->SetPeriod(gossipConfig->TabletCellStatusIncrementalGossipPeriod);
        }
    }

    void SaveKeys(NCellMaster::TSaveContext& context) const
    {
        CellBundleMap_.SaveKeys(context);
        CellMap_.SaveKeys(context);
        AreaMap_.SaveKeys(context);
    }

    void SaveValues(NCellMaster::TSaveContext& context) const
    {
        CellBundleMap_.SaveValues(context);
        CellMap_.SaveValues(context);
        AreaMap_.SaveValues(context);
    }

    void LoadKeys(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        CellBundleMap_.LoadKeys(context);
        CellMap_.LoadKeys(context);
        AreaMap_.LoadKeys(context);
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        CellBundleMap_.LoadValues(context);
        CellMap_.LoadValues(context);
        AreaMap_.LoadValues(context);
    }

    void MaybeRegisterGlobalCell(TCellBase* cell)
    {
        auto cellId = cell->GetId();
        if (IsGlobalCellId(cellId)) {
            EmplaceOrCrash(CellTagToCell_, CellTagFromId(cellId), cell);
        }
    }

    void MaybeUnregisterGlobalCell(TCellBase* cell)
    {
        auto cellId = cell->GetId();
        if (IsGlobalCellId(cellId)) {
            // NB: Missing cell is fine.
            CellTagToCell_.erase(CellTagFromId(cellId));
        }
    }

    void OnAfterSnapshotLoaded() override
    {
        TMasterAutomatonPart::OnAfterSnapshotLoaded();

        NameToCellBundleMap_.clear();
        CellBundlesPerTypeMap_.clear();
        for (auto [_, bundle] : CellBundleMap_) {
            if (!IsObjectAlive(bundle)) {
                continue;
            }

            EmplaceOrCrash(NameToCellBundleMap_[bundle->GetCellarType()], bundle->GetName(), bundle);
            InsertOrCrash(CellBundlesPerTypeMap_[bundle->GetCellarType()], bundle);
        }

        for (auto [_, area] : AreaMap_) {
            if (!IsObjectAlive(area)) {
                continue;
            }

            EmplaceOrCrash(area->GetCellBundle()->Areas(), area->GetName(), area);
            if (area->GetName() == DefaultAreaName) {
                YT_VERIFY(!area->GetCellBundle()->GetDefaultArea());
                area->GetCellBundle()->SetDefaultArea(area);
            }
        }

        AddressToCell_.clear();
        CellsPerTypeMap_.clear();
        for (auto [cellId, cell] : CellMap_) {
            if (!IsObjectAlive(cell)) {
                continue;
            }

            MaybeRegisterGlobalCell(cell);

            InsertOrCrash(cell->GetCellBundle()->Cells(), cell);
            InsertOrCrash(cell->GetArea()->Cells(), cell);
            InsertOrCrash(CellsPerTypeMap_[cell->GetCellarType()], cell);

            for (TPeerId peerId = 0; peerId < std::ssize(cell->Peers()); ++peerId) {
                if (cell->IsAlienPeer(peerId)) {
                    continue;
                }
                const auto& peer = cell->Peers()[peerId];
                if (!peer.Descriptor.IsNull()) {
                    YT_VERIFY(AddToAddressToCellMap(peer.Descriptor, cell, peerId));
                }
            }

            if (cell->IsIndependent()) {
                for (TPeerId peerId = 0; peerId < std::ssize(cell->Peers()); ++peerId) {
                    auto* transaction = cell->Peers()[peerId].PrerequisiteTransaction;
                    if (transaction) {
                        EmplaceOrCrash(TransactionToCellMap_, transaction, std::make_pair(cell, peerId));
                    }
                }
            } else {
                if (auto* transaction = cell->GetPrerequisiteTransaction(); transaction) {
                    EmplaceOrCrash(TransactionToCellMap_, transaction, std::make_pair(cell, std::nullopt));
                }
            }

            cell->GossipStatus().Initialize(Bootstrap_);
        }

        AfterSnapshotLoaded_.Fire();
    }

    void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::Clear();

        CellBundleMap_.Clear();
        CellMap_.Clear();
        AreaMap_.Clear();
        NameToCellBundleMap_.clear();
        CellTagToCell_.clear();
        AddressToCell_.clear();
        TransactionToCellMap_.clear();
        CellBundlesPerTypeMap_.clear();
        CellsPerTypeMap_.clear();
        BundleNodeTracker_->Clear();
    }

    void OnCellStatusGossip(bool incremental)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (!multicellManager->IsLocalMasterCellRegistered()) {
            return;
        }

        YT_LOG_INFO("Sending cell status gossip message (Incremental: %v)",
            incremental);

        NProto::TReqSetCellStatus request;
        request.set_cell_tag(Bootstrap_->GetCellTag());

        for (auto [cellId, cell] : CellMap_) {
            if (!IsObjectAlive(cell)) {
                continue;
            }

            TCellStatus cellStatus;
            if (multicellManager->IsPrimaryMaster()) {
                cellStatus = cell->GossipStatus().Cluster();
            } else {
                cellStatus = cell->GossipStatus().Local();
            }

            if (incremental && cell->LastGossipStatus() == cellStatus) {
                continue;
            }
            cell->LastGossipStatus() = cellStatus;

            auto* entry = request.add_entries();
            ToProto(entry->mutable_cell_id(), cell->GetId());
            ToProto(entry->mutable_status(), cellStatus);
        }

        const auto& hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
        CreateMutation(hydraManager, TReqUpdateTabletCellHealthStatistics())
            ->CommitAndLog(Logger);

        if (multicellManager->IsPrimaryMaster()) {
            multicellManager->PostToSecondaryMasters(request, false);
        } else {
            multicellManager->PostToPrimaryMaster(request, false);
        }
    }

    void HydraSetCellStatus(NProto::TReqSetCellStatus* request)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();

        auto cellTag = request->cell_tag();
        YT_VERIFY(multicellManager->IsPrimaryMaster() || cellTag == Bootstrap_->GetPrimaryCellTag());

        if (!multicellManager->IsRegisteredMasterCell(cellTag)) {
            YT_LOG_ERROR_IF(IsMutationLoggingEnabled(), "Received cell status gossip message from unknown cell (CellTag: %v)",
                cellTag);
            return;
        }

        YT_LOG_INFO_IF(IsMutationLoggingEnabled(), "Received cell status gossip message (CellTag: %v, EntryCount: %v)",
            cellTag,
            request->entries_size());

        THashSet<TCellBundle*> updatedBundles;
        for (const auto& entry : request->entries()) {
            auto cellId = FromProto<TTamedCellId>(entry.cell_id());
            auto* cell = FindCell(cellId);
            if (!IsObjectAlive(cell)) {
                continue;
            }

            auto newStatus = FromProto<TCellStatus>(entry.status());
            if (multicellManager->IsPrimaryMaster()) {
                *cell->GossipStatus().Remote(cellTag) = newStatus;
            } else {
                cell->GossipStatus().Cluster() = newStatus;
            }

            updatedBundles.insert(cell->GetCellBundle());
        }

        UpdateBundlesHealth(updatedBundles);
    }

    void HydraUpdateCellHealth(TReqUpdateTabletCellHealthStatistics* /*request*/)
    {
        UpdateCellsHealth();

        THashSet<TCellBundle*> allBundles;
        for (const auto& [bundleId, bundle] : CellBundleMap_) {
            allBundles.insert(bundle);
        }
        UpdateBundlesHealth(allBundles);
    }

    void UpdateCellsHealth()
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        auto peerRevocationReasonDeadline =
            NHydra::GetCurrentMutationContext()->GetTimestamp() -
            GetDynamicConfig()->PeerRevocationReasonExpirationTime;

        for (auto [cellId, cell] : CellMap_) {
            if (!IsObjectAlive(cell)) {
                continue;
            }

            auto& health = cell->GossipStatus().Local().Health;
            auto newHealth = cell->GetHealth();

            if (health != newHealth) {
                YT_LOG_DEBUG("Cell health changed (CellId: %v, OldHealth: %v, NewHealth: %v)",
                    cell->GetId(),
                    health,
                    newHealth);
                health = newHealth;
            }

            if (multicellManager->IsMulticell() && multicellManager->IsPrimaryMaster()) {
                cell->RecomputeClusterStatus();
                tabletManager->RecomputeTabletCellStatistics(cell);
            }

            cell->ExpirePeerRevocationReasons(peerRevocationReasonDeadline);
        }
    }

    void UpdateBundlesHealth(const THashSet<TCellBundle*>& bundles)
    {
        for (auto* bundle : bundles) {
            if (!IsObjectAlive(bundle)) {
                continue;
            }

            auto oldHealth = bundle->Health();
            bundle->Health() = ECellHealth::Good;
            for (auto cell : bundle->Cells()) {
                bundle->Health() = TCellBase::CombineHealths(cell->GossipStatus().Local().Health, bundle->Health());
            }

            YT_LOG_DEBUG_IF(bundle->Health() != oldHealth,
                "Bundle health changed (Bundle: %v, OldHealth: %v, NewHealth: %v)",
                bundle->GetName(),
                oldHealth,
                bundle->Health());
        }
    }

    void UpdateNodeCellarSize(TNode* node, ECellarType cellarType, int newSize)
    {
        auto oldSize = node->GetCellarSize(cellarType);

        if (oldSize == newSize) {
            return;
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Node cellar size changed (Address: %v, CellarType: %v, OldCellarSize: %v, NewCellarSize: %v)",
            node->GetDefaultAddress(),
            cellarType,
            oldSize,
            newSize);

        if (newSize < oldSize) {
            const auto& cellar = node->GetCellar(cellarType);

            for (int index = newSize; index < oldSize; ++index) {
                const auto& slot = cellar[index];
                auto* cell = slot.Cell;
                if (cell) {
                    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Slot destroyed, detaching cell peer (Address: %v, CellarType: %v, CellId: %v, PeerId: %v)",
                        node->GetDefaultAddress(),
                        cellarType,
                        cell->GetId(),
                        slot.PeerId);

                    cell->DetachPeer(node);
                }
            }
        }

        node->UpdateCellarSize(cellarType, newSize);
    }

    void OnNodeRegistered(TNode* node)
    {
        node->InitCellars();
    }

    void OnNodeUnregistered(TNode* node)
    {
        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Node unregistered (Address: %v)",
            node->GetDefaultAddress());

        for (auto cellarType : TEnumTraits<ECellarType>::GetDomainValues()) {
            UpdateNodeCellarSize(node, cellarType, 0);
        }
    }

    void OnCellarNodeHeartbeat(
        TNode* node,
        NCellarNodeTrackerClient::NProto::TReqHeartbeat* request,
        NCellarNodeTrackerClient::NProto::TRspHeartbeat* response)
    {
        THashSet<ECellarType> seenCellarTypes;

        for (auto cellarRequest : request->cellars()) {
            auto* cellarResponse = response->add_cellars();
            cellarResponse->set_type(cellarRequest.type());

            auto cellarType = FromProto<ECellarType>(cellarRequest.type());
            if (!seenCellarTypes.insert(cellarType).second) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Duplicate cellar type in heartbeat, skipped (CellarType: %v)",
                    cellarType);
                continue;
            }

            ProcessCellarHeartbeat(node, &cellarRequest, cellarResponse);
        }

        for (auto cellarType : TEnumTraits<ECellarType>::GetDomainValues()) {
            if (!seenCellarTypes.contains(cellarType)) {
                UpdateNodeCellarSize(node, cellarType, 0);
            }
        }
    }

    void ProcessCellarHeartbeat(
        TNode* node,
        NCellarNodeTrackerClient::NProto::TReqCellarHeartbeat* request,
        NCellarNodeTrackerClient::NProto::TRspCellarHeartbeat* response)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellarType = FromProto<ECellarType>(request->type());
        auto Logger = CellServerLogger.WithTag("CellarType: %v", cellarType);

        // Various request helpers.
        auto requestCreateSlot = [&] (const TCellBase* cell) {
            if (!response) {
                return;
            }

            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            if (!multicellManager->IsPrimaryMaster()) {
                return;
            }

            auto cellId = cell->GetId();
            auto peerId = cell->GetPeerId(node->GetDefaultAddress());
            if (!cell->GetPrerequisiteTransaction(peerId)) {
                return;
            }

            auto* protoInfo = response->add_slots_to_create();

            ToProto(protoInfo->mutable_cell_id(), cell->GetId());
            protoInfo->set_peer_id(peerId);

            const auto* cellBundle = cell->GetCellBundle();
            protoInfo->set_options(ConvertToYsonString(cellBundle->GetOptions()).ToString());

            protoInfo->set_cell_bundle(cellBundle->GetName());

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Occupant creation requested (Address: %v, CellId: %v, PeerId: %v)",
                node->GetDefaultAddress(),
                cellId,
                peerId);
        };

        auto requestConfigureSlot = [&] (const TCellBase* cell) {
            if (!response) {
                return;
            }

            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            if (!multicellManager->IsPrimaryMaster()) {
                return;
            }

            auto cellId = cell->GetId();
            auto peerId = cell->GetPeerId(node->GetDefaultAddress());
            if (!cell->GetPrerequisiteTransaction(peerId)) {
                return;
            }

            auto* protoInfo = response->add_slots_to_configure();

            auto cellDescriptor = cell->GetDescriptor();
            const auto& prerequisiteTransactionId = cell->GetPrerequisiteTransaction(peerId)->GetId();

            protoInfo->set_peer_id(peerId);
            protoInfo->set_config_version(cell->GetConfigVersion());
            ToProto(protoInfo->mutable_cell_descriptor(), cellDescriptor);
            ToProto(protoInfo->mutable_prerequisite_transaction_id(), prerequisiteTransactionId);
            protoInfo->set_abandon_leader_lease_during_recovery(GetDynamicConfig()->AbandonLeaderLeaseDuringRecovery);
            protoInfo->set_options(ConvertToYsonString(cell->GetCellBundle()->GetOptions()).ToString());

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Occupant configuration update requested "
                "(Address: %v, CellId: %v, PeerId: %v, Version: %v, PrerequisiteTransactionId: %v, AbandonLeaderLeaseDuringRecovery: %v)",
                node->GetDefaultAddress(),
                cellId,
                peerId,
                cell->GetConfigVersion(),
                prerequisiteTransactionId,
                protoInfo->abandon_leader_lease_during_recovery());
        };

        auto requestUpdateSlot = [&] (const TCellBase* cell) {
            if (!response) {
                return;
            }

            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            if (!multicellManager->IsPrimaryMaster()) {
                return;
            }

            auto* protoInfo = response->add_slots_to_update();

            auto cellId = cell->GetId();

            ToProto(protoInfo->mutable_cell_id(), cell->GetId());

            const auto* cellBundle = cell->GetCellBundle();
            protoInfo->set_dynamic_config_version(cellBundle->GetDynamicConfigVersion());
            protoInfo->set_dynamic_options(ConvertToYsonString(cellBundle->GetDynamicOptions()).ToString());

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Occupant update requested (Address: %v, CellId: %v, DynamicConfigVersion: %v)",
                node->GetDefaultAddress(),
                cellId,
                cellBundle->GetDynamicConfigVersion());
        };

        auto requestRemoveSlot = [&] (TTamedCellId cellId) {
            if (!response) {
                return;
            }

            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            if (!multicellManager->IsPrimaryMaster()) {
                return;
            }

            auto* protoInfo = response->add_slots_to_remove();
            ToProto(protoInfo->mutable_cell_id(), cellId);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Occupant removal requested (Address: %v, CellId: %v)",
                node->GetDefaultAddress(),
                cellId);
        };

        const auto* mutationContext = GetCurrentMutationContext();
        auto mutationTimestamp = mutationContext->GetTimestamp();

        const auto& address = node->GetDefaultAddress();

        UpdateNodeCellarSize(node, cellarType, request->cell_slots_size());

        auto* cellar = node->FindCellar(cellarType);
        if (!cellar) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Received heartbeat for unexisting cellar, skipped (Address: %v, CellarType: %v)",
                node->GetDefaultAddress(),
                cellarType);
            return;
        }
        YT_VERIFY(std::ssize(*cellar) == request->cell_slots_size());

        // Our expectations.
        THashSet<TCellBase*> expectedCells;
        for (const auto& slot : *cellar) {
            auto* cell = slot.Cell;
            if (!IsObjectAlive(cell)) {
                continue;
            }
            InsertOrCrash(expectedCells, cell);
        }

        // Figure out and analyze the reality.
        THashSet<const TCellBase*> actualCells;
        for (int slotIndex = 0; slotIndex < request->cell_slots_size(); ++slotIndex) {
            // Pre-erase slot.
            auto& slot = (*cellar)[slotIndex];
            slot = TNode::TCellSlot();

            const auto& slotInfo = request->cell_slots(slotIndex);

            auto state = EPeerState(slotInfo.peer_state());
            if (state == EPeerState::None)
                continue;

            auto cellInfo = FromProto<TCellInfo>(slotInfo.cell_info());
            auto cellId = cellInfo.CellId;
            auto* cell = FindCell(cellId);
            if (!IsObjectAlive(cell)) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Unknown cell is running (Address: %v, CellId: %v)",
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            if (GetCellarTypeFromCellId(cellId) != cellarType) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell with unexpected cellar type is running (Address: %v, CellId: %v, CellarType: %v, CellarType: %v)",
                    address,
                    cellId,
                    GetCellarTypeFromCellId(cellId),
                    cellarType);
                requestRemoveSlot(cellId);
                continue;
            }

            auto peerId = cell->FindPeerId(address);
            if (peerId == InvalidPeerId) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Unexpected cell is running (Address: %v, CellId: %v)",
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            if (CountVotingPeers(cell) > 1 && slotInfo.peer_id() != InvalidPeerId && slotInfo.peer_id() != peerId) {
                YT_LOG_DEBUG_IF(
                    IsMutationLoggingEnabled(),
                    "Invalid peer id for cell: %v instead of %v (Address: %v, CellId: %v)",
                    slotInfo.peer_id(),
                    peerId,
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            if (state == EPeerState::Stopped) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell peer is stopped, removing (PeerId: %v, Address: %v, CellId: %v)",
                    slotInfo.peer_id(),
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            auto expectedIt = expectedCells.find(cell);
            if (expectedIt == expectedCells.end()) {
                cell->AttachPeer(node, peerId);
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell peer online (Address: %v, CellId: %v, PeerId: %v)",
                    address,
                    cellId,
                    peerId);
            }

            cell->UpdatePeerSeenTime(peerId, mutationTimestamp);
            cell->UpdatePeerState(peerId, state);
            InsertOrCrash(actualCells, cell);

            // Populate slot.
            slot.Cell = cell;
            slot.PeerState = state;
            slot.PeerId = slot.Cell->GetPeerId(node); // don't trust peerInfo, it may still be InvalidPeerId
            slot.PreloadPendingStoreCount = slotInfo.preload_pending_store_count();
            slot.PreloadCompletedStoreCount = slotInfo.preload_completed_store_count();
            slot.PreloadFailedStoreCount = slotInfo.preload_failed_store_count();

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell is running (Address: %v, CellId: %v, PeerId: %v, State: %v, ConfigVersion: %v)",
                address,
                cell->GetId(),
                slot.PeerId,
                state,
                cellInfo.ConfigVersion);

            if (cellInfo.ConfigVersion != cell->GetConfigVersion()) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Occupant should be reconfigured "
                    "(CellId: %v, PeerId: %v, ExpectedConfingVersion: %v, ActualConfigVersion: %v)",
                    cell->GetId(),
                    slot.PeerId,
                    cell->GetConfigVersion(),
                    cellInfo.ConfigVersion);
                requestConfigureSlot(cell);
            }

            if (slotInfo.has_dynamic_config_version() &&
                slotInfo.dynamic_config_version() != cell->GetCellBundle()->GetDynamicConfigVersion())
            {
                requestUpdateSlot(cell);
            }
        }

        // Check for expected slots that are missing.
        for (auto* cell : expectedCells) {
            if (actualCells.find(cell) == actualCells.end()) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell peer offline: slot is missing (CellId: %v, Address: %v)",
                    cell->GetId(),
                    address);
                cell->DetachPeer(node);
            }
        }

        // Request slot starts.
        {
            int availableSlots = node->GetAvailableSlotCount(cellarType);
            auto it = AddressToCell_.find(address);
            if (it != AddressToCell_.end() && availableSlots > 0) {
                for (auto [cell, peerId] : it->second) {
                    if (!IsObjectAlive(cell)) {
                        continue;
                    }

                    if (actualCells.find(cell) == actualCells.end()) {
                        requestCreateSlot(cell);
                        requestConfigureSlot(cell);
                        requestUpdateSlot(cell);
                    }
                }
            }
        }
    }


    bool AddToAddressToCellMap(const TNodeDescriptor& descriptor, TCellBase* cell, TPeerId peerId)
    {
        const auto& address = descriptor.GetDefaultAddress();
        auto cellsIt = AddressToCell_.find(address);
        if (cellsIt == AddressToCell_.end()) {
            cellsIt = AddressToCell_.emplace(address, TCellSet()).first;
        }
        auto& set = cellsIt->second;
        auto it = std::find_if(set.begin(), set.end(), [&] (const auto& pair) {
            return pair.first == cell;
        });
        if (it != set.end()) {
            return false;
        }
        set.emplace_back(cell, peerId);
        return true;
    }

    void RemoveFromAddressToCellMap(const TNodeDescriptor& descriptor, TCellBase* cell)
    {
        const auto& address = descriptor.GetDefaultAddress();
        auto cellsIt = AddressToCell_.find(address);
        YT_VERIFY(cellsIt != AddressToCell_.end());
        auto& set = cellsIt->second;
        auto it = std::find_if(set.begin(), set.end(), [&] (const auto& pair) {
            return pair.first == cell;
        });
        YT_VERIFY(it != set.end());
        set.erase(it);
        if (set.empty()) {
            AddressToCell_.erase(cellsIt);
        }
    }

    void HydraAssignPeers(TReqAssignPeers* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TTamedCellId>(request->cell_id());
        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell)) {
            YT_LOG_WARNING_IF(
                IsMutationLoggingEnabled(),
                "Attempted to assigning peer on non-existing cell (CellId: %v)",
                cellId);
            return;
        }

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        const auto* mutationContext = GetCurrentMutationContext();
        auto mutationTimestamp = mutationContext->GetTimestamp();

        std::vector<TPeerId> assignedPeers;
        THashSet<TPeerId> assignedPeersSet;
        for (const auto& peerInfo : request->peer_infos()) {
            auto peerId = peerInfo.peer_id();
            auto descriptor = FromProto<TNodeDescriptor>(peerInfo.node_descriptor());

            if (!cell->IsValidPeer(peerId)) {
                YT_LOG_WARNING_IF(
                    IsMutationLoggingEnabled(),
                    "Attempted to assigning invalid peer (CellId: %v, PeerId: %v, PeerCount: %v)",
                    cellId,
                    peerId,
                    std::ssize(cell->Peers()));
                continue;
            }
            if (descriptor.IsNull() ||
                !Bootstrap_->GetNodeTracker()->FindNodeByAddress(descriptor.GetDefaultAddress()))
            {
                YT_LOG_WARNING_IF(
                    IsMutationLoggingEnabled(),
                    "Attempted to assign peer on non-existing node (CellId: %v, PeerId: %v, Address: %v)",
                    cellId,
                    peerId,
                    descriptor.GetDefaultAddress());
                continue;
            }
            if (assignedPeersSet.contains(peerId)) {
                YT_LOG_WARNING_IF(
                    IsMutationLoggingEnabled(),
                    "Peer is assigned multiple times (CellId: %v, PeerId: %v, PeerCount: %v)",
                    cellId,
                    peerId,
                    std::ssize(cell->Peers()));
                continue;
            }

            auto& peer = cell->Peers()[peerId];
            if (!peer.Descriptor.IsNull()) {
                YT_LOG_WARNING_IF(IsMutationLoggingEnabled(),
                    "Peer is already assigned to node (CellId: %v, PeerId: %v, CurrentAddress: %v, AssignedAddress: %v)",
                    cellId,
                    peerId,
                    peer.Descriptor.GetDefaultAddress(),
                    descriptor.GetDefaultAddress());
                continue;
            }

            if (!AddToAddressToCellMap(descriptor, cell, peerId)) {
                YT_LOG_WARNING_IF(IsMutationLoggingEnabled(),
                    "Cell already has peer on node (CellId: %v, PeerId: %v, Address: %v)",
                    cellId,
                    peerId,
                    descriptor.GetDefaultAddress());
                continue;
            }
            assignedPeersSet.insert(peerId);

            if (!cell->GetPrerequisiteTransaction(peerId)) {
                assignedPeers.push_back(peerId);
            }

            cell->AssignPeer(descriptor, peerId);
            cell->UpdatePeerSeenTime(peerId, mutationTimestamp);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell peer assigned (CellId: %v, PeerId: %v, Address: %v)",
                cellId,
                peerId,
                descriptor.GetDefaultAddress());
        }

        if (multicellManager->IsPrimaryMaster()) {
            RestartPrerequisiteTransactions(cell, assignedPeers);
            multicellManager->PostToMasters(*request, multicellManager->GetRegisteredMasterCellTags());
        }

        ReconfigureCell(cell);
    }

    void HydraRevokePeers(TReqRevokePeers* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TTamedCellId>(request->cell_id());
        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell)) {
            YT_LOG_WARNING_IF(
                IsMutationLoggingEnabled(),
                "Attempted to revoking peer of non-existing cell (CellId: %v)",
                cellId);
            return;
        }

        THashSet<int> revokedPeersSet;
        std::vector<int> revokedPeers;
        for (auto peerId : request->peer_ids()) {
            if (!cell->IsValidPeer(peerId)) {
                YT_LOG_WARNING_IF(
                    IsMutationLoggingEnabled(),
                    "Attempted to revoking invalid peer (CellId: %v, PeerId: %v, PeerCount: %v)",
                    cellId,
                    peerId,
                    std::ssize(cell->Peers()));
                continue;
            }
            if (revokedPeersSet.contains(peerId)) {
                YT_LOG_WARNING_IF(
                    IsMutationLoggingEnabled(),
                    "Peer is revoked multiple times (CellId: %v, PeerId: %v, PeerCount: %v)",
                    cellId,
                    peerId,
                    std::ssize(cell->Peers()));
                continue;
            }
            revokedPeersSet.insert(peerId);
            revokedPeers.push_back(peerId);
        }

        for (auto peerId : revokedPeers) {
            DoRevokePeer(cell, peerId, FromProto<TError>(request->reason()));
        }

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            AbortCellTransactions(cell, revokedPeers);
            multicellManager->PostToMasters(*request, multicellManager->GetRegisteredMasterCellTags());
        }

        ReconfigureCell(cell);
    }

    void HydraReassignPeers(TReqReassignPeers* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        for (auto& revocation : *request->mutable_revocations()) {
            HydraRevokePeers(&revocation);
        }

        for (auto& assignment : *request->mutable_assignments()) {
            HydraAssignPeers(&assignment);
        }

        for (auto& peerCountUpdate : *request->mutable_peer_count_updates()) {
            HydraUpdatePeerCount(&peerCountUpdate);
        }

        for (auto& leadingPearUpdates : *request->mutable_leading_peer_updates()) {
            HydraSetLeadingPeer(&leadingPearUpdates);
        }

        CellPeersAssigned_.Fire();

        // NB: Send individual revoke and assign requests to secondary masters to support old tablet tracker.
    }

    void HydraSetLeadingPeer(TReqSetLeadingPeer* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TTamedCellId>(request->cell_id());
        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell)) {
            YT_LOG_WARNING_IF(
                IsMutationLoggingEnabled(),
                "Attempted to set leading peer of non-existing cell (CellId: %v)",
                cellId);
            return;
        }
        if (cell->IsIndependent()) {
            YT_LOG_WARNING_IF(
                IsMutationLoggingEnabled(),
                "Attempted to set leading peer of independent cell (CellId: %v)",
                cell->GetId());
            return;
        }
        auto peerId = request->peer_id();
        if (!cell->IsValidPeer(peerId)) {
            YT_LOG_WARNING_IF(
                IsMutationLoggingEnabled(),
                "Attempted to set invalide peer to lead (CellId: %v, PeerId: %v, PeerCount: %v)",
                cellId,
                peerId,
                std::ssize(cell->Peers()));
            return;
        }

        const auto& oldLeader = cell->Peers()[cell->GetLeadingPeerId()];
        const auto& newLeader = cell->Peers()[peerId];

        cell->SetLeadingPeerId(peerId);

        const auto& descriptor = cell->Peers()[peerId].Descriptor;
        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell leading peer updated (CellId: %v, Address: %v, PeerId: %v)",
            cellId,
            descriptor.GetDefaultAddress(),
            peerId);

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            RestartAllPrerequisiteTransactions(cell);

            multicellManager->PostToMasters(*request, multicellManager->GetRegisteredMasterCellTags());
        }

        ReconfigureCell(cell);

        // Notify new leader as soon as possible via heartbeat request.
        if (multicellManager->IsPrimaryMaster() && IsLeader()) {
            if (oldLeader.Node) {
                Bootstrap_->GetNodeTracker()->RequestCellarHeartbeat(oldLeader.Node->GetId());
            }
            if (newLeader.Node) {
                Bootstrap_->GetNodeTracker()->RequestCellarHeartbeat(newLeader.Node->GetId());
            }
        }
    }

    void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnLeaderActive();

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            CellTracker_->Start();
        }

        CellStatusIncrementalGossipExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::TabletGossip),
            BIND(&TTamedCellManager::OnCellStatusGossip, MakeWeak(this), /* incremental */true));
        CellStatusIncrementalGossipExecutor_->Start();
        CellStatusFullGossipExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::TabletGossip),
            BIND(&TTamedCellManager::OnCellStatusGossip, MakeWeak(this), /* incremental */false));
        CellStatusFullGossipExecutor_->Start();
    }

    void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnStopLeading();

        CellTracker_->Stop();

        if (CellStatusIncrementalGossipExecutor_) {
            CellStatusIncrementalGossipExecutor_->Stop();
            CellStatusIncrementalGossipExecutor_.Reset();
        }
        if (CellStatusFullGossipExecutor_) {
            CellStatusFullGossipExecutor_->Stop();
            CellStatusFullGossipExecutor_.Reset();
        }
    }

    void ReconfigureCell(TCellBase* cell)
    {
        cell->SetConfigVersion(cell->GetConfigVersion() + 1);

        YT_LOG_DEBUG_IF(
            IsMutationLoggingEnabled(),
            "Cell reconfigured (CellId: %v, Version: %v)",
            cell->GetId(),
            cell->GetConfigVersion());
    }

    bool CheckHasHealthyCells(TCellBundle* bundle)
    {
        for (auto [cellId, cell] : CellMap_) {
            if (!IsCellActive(cell)) {
                continue;
            }
            if (cell->GetCellBundle() == bundle && cell->IsHealthy()) {
                return true;
            }
        }

        return false;
    }

    void ValidateHasHealthyCells(TCellBundle* bundle)
    {
        if (!CheckHasHealthyCells(bundle)) {
            THROW_ERROR_EXCEPTION("No healthy cells in bundle %Qv",
                bundle->GetName());
        }
    }

    bool IsCellActive(TCellBase* cell)
    {
        return IsObjectAlive(cell) && !cell->IsDecommissionStarted();
    }


    void RestartPrerequisiteTransactions(TCellBase* cell, const std::vector<TPeerId>& peerIds)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsPrimaryMaster());

        AbortCellTransactions(cell, peerIds);

        bool independent = cell->IsIndependent();
        for (auto peerId : peerIds) {
            if (independent || peerId == cell->GetLeadingPeerId()) {
                StartPrerequisiteTransaction(cell, independent ? std::make_optional(peerId) : std::nullopt);
            }
        }
    }

    void AbortCellTransactions(TCellBase* cell, const std::vector<TPeerId>& peerIds)
    {
        bool independent = cell->IsIndependent();
        for (auto peerId : peerIds) {
            if (independent || peerId == cell->GetLeadingPeerId()) {
                AbortPrerequisiteTransaction(cell, independent ? std::make_optional(peerId) : std::nullopt);
                AbortCellSubtreeTransactions(cell, independent ? std::make_optional(peerId) : std::nullopt);
            }
        }
    }

    void RestartAllPrerequisiteTransactions(TCellBase* cell)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsPrimaryMaster());

        AbortAllCellTransactions(cell);

        bool independent = cell->IsIndependent();
        if (independent) {
            for (TPeerId peerId = 0; peerId < std::ssize(cell->Peers()); ++peerId) {
                if (!cell->IsAlienPeer(peerId)) {
                    StartPrerequisiteTransaction(cell, peerId);
                }
            }
        } else {
            StartPrerequisiteTransaction(cell, std::nullopt);
        }
    }

    void AbortAllCellTransactions(TCellBase* cell)
    {
        bool independent = cell->IsIndependent();
        if (independent) {
            for (TPeerId peerId = 0; peerId < std::ssize(cell->Peers()); ++peerId) {
                if (!cell->IsAlienPeer(peerId)) {
                    AbortPrerequisiteTransaction(cell, peerId);
                    AbortCellSubtreeTransactions(cell, peerId);
                }
            }
        } else {
            AbortPrerequisiteTransaction(cell, std::nullopt);
            AbortCellSubtreeTransactions(cell, std::nullopt);
        }
    }

    void StartPrerequisiteTransaction(TCellBase* cell, std::optional<TPeerId> peerId)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsPrimaryMaster());

        const auto& secondaryCellTags = multicellManager->GetRegisteredMasterCellTags();

        auto title = peerId
            ? Format("Prerequisite for cell %v, peer %v", cell->GetId(), peerId)
            : Format("Prerequisite for cell %v", cell->GetId());

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionManager->StartTransaction(
            nullptr /*parent*/,
            {} /*prerequisiteTransactions*/,
            secondaryCellTags,
            std::nullopt /*timeout*/,
            std::nullopt /*deadline*/,
            title,
            EmptyAttributes());

        YT_VERIFY(!cell->GetPrerequisiteTransaction(peerId));
        EmplaceOrCrash(TransactionToCellMap_, transaction, std::make_pair(cell, peerId));
        cell->SetPrerequisiteTransaction(peerId, transaction);

        TReqStartPrerequisiteTransaction request;
        ToProto(request.mutable_cell_id(), cell->GetId());
        ToProto(request.mutable_transaction_id(), transaction->GetId());
        if (peerId) {
            request.set_peer_id(*peerId);
        }
        multicellManager->PostToMasters(request, multicellManager->GetRegisteredMasterCellTags());

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell prerequisite transaction started (CellId: %v, PeerId: %v, TransactionId: %v)",
            cell->GetId(),
            peerId,
            transaction->GetId());
    }

    void HydraStartPrerequisiteTransaction(TReqStartPrerequisiteTransaction* request)
    {
        YT_VERIFY(Bootstrap_->IsSecondaryMaster());

        auto cellId = FromProto<TTamedCellId>(request->cell_id());
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto peerId = request->has_peer_id() ? std::make_optional(request->peer_id()) : std::nullopt;

        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell)) {
            return;
        }

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto transaction = transactionManager->FindTransaction(transactionId);

        if (!IsObjectAlive(transaction)) {
            YT_LOG_INFO_IF(IsMutationLoggingEnabled(), "Prerequisite transaction is not found on secondary master (CellId: %v, PeerId: %v, TransactionId: %v)",
                cellId,
                peerId,
                transactionId);
            return;
        }

        EmplaceOrCrash(TransactionToCellMap_, transaction, std::make_pair(cell, peerId));

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell prerequisite transaction attached (CellId: %v, PeerId: %v, TransactionId: %v)",
            cell->GetId(),
            peerId,
            transaction->GetId());
    }

    void AbortCellSubtreeTransactions(TCellBase* cell, std::optional<int> peerId)
    {
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto nodeProxy = FindCellNode(cell->GetId());
        if (nodeProxy && peerId) {
            nodeProxy = nodeProxy->FindChild(ToString(*peerId))->AsMap();
        }
        if (nodeProxy) {
            cypressManager->AbortSubtreeTransactions(nodeProxy);
        }
    }

    void AbortPrerequisiteTransaction(TCellBase* cell, std::optional<int> peerId)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsPrimaryMaster());

        auto* transaction = cell->GetPrerequisiteTransaction(peerId);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Aborting cell prerequisite transaction (CellId: %v, PeerId: %v, transactionId: %v)",
            cell->GetId(),
            peerId,
            GetObjectId(transaction));

        if (!transaction) {
            return;
        }

        // Suppress calling OnTransactionFinished.
        EraseOrCrash(TransactionToCellMap_, transaction);

        cell->SetPrerequisiteTransaction(peerId, nullptr);

        // Suppress calling OnTransactionFinished on secondary masters.
        TReqAbortPrerequisiteTransaction request;
        ToProto(request.mutable_cell_id(), cell->GetId());
        ToProto(request.mutable_transaction_id(), transaction->GetId());
        if (peerId) {
            request.set_peer_id(*peerId);
        }
        multicellManager->PostToMasters(request, multicellManager->GetRegisteredMasterCellTags());

        // NB: Make a copy, transaction will die soon.
        auto transactionId = transaction->GetId();

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->AbortTransaction(transaction, true);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell prerequisite transaction aborted (CellId: %v, PeerId: %v, TransactionId: %v)",
            cell->GetId(),
            peerId,
            transactionId);
    }

    void HydraAbortPrerequisiteTransaction(TReqAbortPrerequisiteTransaction* request)
    {
        YT_VERIFY(Bootstrap_->IsSecondaryMaster());

        auto cellId = FromProto<TTamedCellId>(request->cell_id());
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto peerId = request->has_peer_id() ? std::make_optional(request->peer_id()) : std::nullopt;

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionManager->FindTransaction(transactionId);

        if (!IsObjectAlive(transaction)) {
            YT_LOG_ALERT("Cell prerequisite transaction not found at secondary master (CellId: %v, PeerId: %v, TransactionId: %v)",
                cellId,
                peerId,
                transactionId);
            return;
        }

        // COMPAT(savrus) Don't check since we didn't have them in earlier versions.
        TransactionToCellMap_.erase(transaction);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell prerequisite transaction aborted (CellId: %v, PeerId: %v, TransactionId: %v)",
            cellId,
            peerId,
            transactionId);
    }

    void OnTransactionFinished(TTransaction* transaction)
    {
        auto it = TransactionToCellMap_.find(transaction);
        if (it == TransactionToCellMap_.end()) {
            return;
        }

        auto [cell, peerId] = it->second;
        TransactionToCellMap_.erase(it);

        auto revocationReason = TError("Cell prerequisite transaction %v finished",
            transaction->GetId());

        cell->SetPrerequisiteTransaction(peerId, nullptr);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Cell prerequisite transaction finished (CellId: %v, PeerId: %v, TransactionId: %v)",
            cell->GetId(),
            peerId,
            transaction->GetId());

        if (peerId) {
            DoRevokePeer(cell, *peerId, revocationReason);
        } else {
            for (TPeerId peerId = 0; peerId < std::ssize(cell->Peers()); ++peerId) {
                if (!cell->IsAlienPeer(peerId)) {
                    DoRevokePeer(cell, peerId, revocationReason);
                }
            }
        }
    }

    void DoRevokePeer(TCellBase* cell, TPeerId peerId, const TError& reason)
    {
        const auto& peer = cell->Peers()[peerId];
        const auto& descriptor = peer.Descriptor;
        if (descriptor.IsNull()) {
            YT_LOG_WARNING_IF(
                IsMutationLoggingEnabled(),
                "Peer is not assigned to node (CellId: %v, PeerId: %v)",
                cell->GetId(),
                peerId);
            return;
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), reason, "Cell peer revoked (CellId: %v, Address: %v, PeerId: %v)",
            cell->GetId(),
            descriptor.GetDefaultAddress(),
            peerId);

        if (peer.Node) {
            peer.Node->DetachCell(cell);
        }

        RemoveFromAddressToCellMap(descriptor, cell);

        cell->RevokePeer(peerId, reason);
    }

    IMapNodePtr GetCellMapNode(TTamedCellId cellId)
    {
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        return cypressManager->ResolvePathToNodeProxy(GetCellCypressPrefix(cellId))->AsMap();
    }

    IMapNodePtr FindCellNode(TTamedCellId cellId)
    {
        auto cellMapNodeProxy = GetCellMapNode(cellId);
        return cellMapNodeProxy->FindChild(ToString(cellId))->AsMap();
    }

    void OnReplicateKeysToSecondaryMaster(TCellTag cellTag)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();

        auto cellBundles = GetValuesSortedByKey(CellBundleMap_);
        for (auto* cellBundle : cellBundles) {
            objectManager->ReplicateObjectCreationToSecondaryMaster(cellBundle, cellTag);
        }

        auto areas = GetValuesSortedByKey(AreaMap_);
        for (auto* area : areas) {
            if (area->GetId() == ReplaceTypeInId(area->GetCellBundle()->GetId(), EObjectType::Area)) {
                continue;
            }
            objectManager->ReplicateObjectCreationToSecondaryMaster(area, cellTag);
        }

        auto cells = GetValuesSortedByKey(CellMap_);
        for (auto* cell : cells) {
            objectManager->ReplicateObjectCreationToSecondaryMaster(cell, cellTag);
        }
    }

    void OnReplicateValuesToSecondaryMaster(TCellTag cellTag)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();

        auto cellBundles = GetValuesSortedByKey(CellBundleMap_);
        for (auto* cellBundle : cellBundles) {
            objectManager->ReplicateObjectAttributesToSecondaryMaster(cellBundle, cellTag);
        }

        auto areas = GetValuesSortedByKey(AreaMap_);
        for (auto* area : areas) {
            objectManager->ReplicateObjectAttributesToSecondaryMaster(area, cellTag);
        }

        auto cells = GetValuesSortedByKey(CellMap_);
        for (auto* cell : cells) {
            objectManager->ReplicateObjectAttributesToSecondaryMaster(cell, cellTag);
            ReplicateCellPropertiesToSecondaryMaster(cell, cellTag);
        }
    }

    void ReplicateCellPropertiesToSecondaryMaster(TCellBase* cell, TCellTag cellTag)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();

        {
            NTabletServer::NProto::TReqSetTabletCellConfigVersion req;
            ToProto(req.mutable_cell_id(), cell->GetId());
            req.set_config_version(cell->GetConfigVersion());
            multicellManager->PostToMaster(req, cellTag);
        }

        if (cell->IsDecommissionStarted()) {
            NTabletServer::NProto::TReqDecommissionTabletCellOnMaster req;
            ToProto(req.mutable_cell_id(), cell->GetId());
            multicellManager->PostToMaster(req, cellTag);
        }
    }

    void HydraSetCellConfigVersion(NTabletServer::NProto::TReqSetTabletCellConfigVersion* request)
    {
        auto cellId = FromProto<TTamedCellId>(request->cell_id());
        auto* cell = FindCell(cellId);
        if (!IsObjectAlive(cell))
            return;
        cell->SetConfigVersion(request->config_version());
    }

    void OnProfiling()
    {
        if (!IsLeader()) {
            for (const auto& [id, cellBundle] : CellBundleMap_) {
                cellBundle->ProfilingCounters().TabletCellCount.Update(0.0);
            }

            return;
        }

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (!multicellManager->IsPrimaryMaster()) {
            for (const auto& [id, cellBundle] : CellBundleMap_) {
                cellBundle->ProfilingCounters().TabletCellCount.Update(0.0);
            }

            return;
        }

        for (const auto& [id, cellBundle] : CellBundleMap_) {
            cellBundle->ProfilingCounters().TabletCellCount.Update(cellBundle->Cells().size());
        }
    }

    TCellBundle* DoFindCellBundleByName(const TString& name, ECellarType cellarType)
    {
        auto it = NameToCellBundleMap_[cellarType].find(name);
        return it == NameToCellBundleMap_[cellarType].end() ? nullptr : it->second;
    }

    static void ValidateCellBundleName(const TString& name)
    {
        if (name.empty()) {
            THROW_ERROR_EXCEPTION("Cell bundle name cannot be empty");
        }
    }

    static void ValidateAreaName(const TString& name)
    {
        if (name.empty()) {
            THROW_ERROR_EXCEPTION("Area name cannot be empty");
        }
    }

    static int CountVotingPeers(const TCellBase* cell)
    {
        int votingPeers = 0;
        for (const auto& peer : cell->GetDescriptor().Peers) {
            if (peer.GetVoting()) {
                ++votingPeers;
            }
        }

        return votingPeers;
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TTamedCellManager, CellBundle, TCellBundle, CellBundleMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTamedCellManager, Cell, TCellBase, CellMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTamedCellManager, Area, TArea, AreaMap_)

////////////////////////////////////////////////////////////////////////////////

ITamedCellManagerPtr CreateTamedCellManager(NCellMaster::TBootstrap* bootstrap)
{
    return New<TTamedCellManager>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellServer
