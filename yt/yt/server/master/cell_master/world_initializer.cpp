#include "world_initializer.h"
#include "private.h"
#include "config.h"
#include "hydra_facade.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>

#include <yt/yt/server/master/cypress_server/node_detail.h>

#include <yt/yt/server/master/security_server/acl.h>
#include <yt/yt/server/master/security_server/group.h>

#include <yt/yt/server/lib/hive/transaction_supervisor.h>

#include <yt/yt/server/lib/scheduler/public.h>

#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>
#include <yt/yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/yt/ytlib/election/cell_manager.h>

#include <yt/yt/ytlib/transaction_client/transaction_service_proxy.h>

#include <yt/yt/ytlib/tablet_client/helpers.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/concurrency/scheduler.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/misc/atomic_object.h>
#include <yt/yt/core/misc/collection_helpers.h>

#include <yt/yt/core/ypath/token.h>

#include <yt/yt/core/ytree/ypath_client.h>

namespace NYT::NCellMaster {

using namespace NConcurrency;
using namespace NCypressClient;
using namespace NCypressServer;
using namespace NHiveClient::NProto;
using namespace NHiveClient;
using namespace NHydra;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NRpc;
using namespace NSecurityServer;
using namespace NTransactionClient;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;

using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CellMasterLogger;

////////////////////////////////////////////////////////////////////////////////

class TWorldInitializer::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TCellMasterConfigPtr config,
        TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
    {
        YT_VERIFY(Config_);
        YT_VERIFY(Bootstrap_);

        const auto& hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
        hydraManager->SubscribeLeaderActive(BIND(&TImpl::OnLeaderActive, MakeWeak(this)));
    }


    bool IsInitialized()
    {
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* rootNode = cypressManager->GetRootNode();
        return !rootNode->KeyToChild().empty();
    }

    void ValidateInitialized()
    {
        if (!IsInitialized()) {
            THROW_ERROR_EXCEPTION(NRpc::EErrorCode::Unavailable, "Cluster is not initialized");
        }
    }

    bool HasProvisionLock()
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsPrimaryMaster());

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto sysNode = cypressManager->ResolvePathToNodeProxy("//sys");
        return sysNode->Attributes().Get<bool>("provision_lock", false);
    }

private:
    const TCellMasterConfigPtr Config_;
    TBootstrap* const Bootstrap_;

    std::vector<TFuture<void>> ScheduledMutations_;

    TAtomicObject<std::vector<TYPath>> OrchidAddresses_;
    TAtomicObject<THashMap<TYPath, TYsonString>> OrchidAddressToAnnotations_;

    void OnLeaderActive()
    {
        // NB: Initialization cannot be carried out here since not all subsystems
        // are fully initialized yet.
        // We'll post an initialization callback to the automaton invoker instead.
        ScheduleInitialize();

        ScheduleUpdateAnnotations();
    }

    void ScheduleInitialize(TDuration delay = TDuration::Zero())
    {
        if (!Bootstrap_->GetHydraFacade()->GetHydraManager()->IsLeader()) {
            YT_LOG_INFO("Master is not leading anymore, ignore world initialization schedule request");
            return;
        }

        YT_LOG_DEBUG("Schedule world initialization (Delay: %v)",
            delay);
        TDelayedExecutor::Submit(
            BIND(&TImpl::Initialize, MakeStrong(this))
                .Via(Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::Periodic)),
            delay);
    }

    void ScheduleUpdateAnnotations(TDuration delay = TDuration::Zero())
    {
        if (!Bootstrap_->GetHydraFacade()->GetHydraManager()->IsLeader()) {
            YT_LOG_INFO("Master is not leading anymore, ignore annotations update schedule request");
            return;
        }

        YT_LOG_DEBUG("Schedule annotations update (Delay: %v)",
            delay);
        TDelayedExecutor::Submit(
            BIND(&TImpl::UpdateAnnotations, MakeStrong(this))
                .Via(Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::Periodic)),
            delay);
    }

    void Initialize()
    {
        if (IsInitialized()) {
            YT_LOG_INFO("World update started");
        } else {
            YT_LOG_INFO("World initialization started");
        }

        auto traceContext = NTracing::TTraceContext::NewRoot("WorldInitializer");
        traceContext->SetSampled();
        NTracing::TTraceContextGuard contextGuard(traceContext);

        TTransactionId transactionId;

        try {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            const auto& multicellManager = Bootstrap_->GetMulticellManager();

            // All initialization will be happening within this transaction.
            transactionId = StartTransaction();

            // Level 1
            ScheduleCreateNode(
                "//sys",
                transactionId,
                EObjectType::SysNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .DoIf(Config_->EnableProvisionLock && multicellManager->IsPrimaryMaster(), [&] (TFluentMap fluent) {
                            fluent.Item("provision_lock").Value(true);
                        })
                    .EndMap());

            // "//tmp" directory is frequently created and removed in tests.
            // Let's not touch it during update to prevent transactions conflicts.
            if (!IsInitialized()) {
                ScheduleCreateNode(
                    "//tmp",
                    transactionId,
                    EObjectType::MapNode,
                    BuildYsonStringFluently()
                        .BeginMap()
                            .Item("opaque").Value(true)
                            .Item("account").Value("tmp")
                            .Item("acl").BeginList()
                                .Item().Value(TAccessControlEntry(
                                    ESecurityAction::Allow,
                                    securityManager->GetUsersGroup(),
                                    EPermissionSet(EPermission::Read | EPermission::Write | EPermission::Remove)))
                            .EndList()
                        .EndMap());
            }

            FlushScheduled();

            // Level 2
            ScheduleCreateNode(
                "//sys/schemas",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/scheduler",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/controller_agents",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                NScheduler::PoolTreesRootCypressPath,
                transactionId,
                EObjectType::SchedulerPoolTreeMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("acl").BeginList()
                        .Item().Value(TAccessControlEntry(
                            ESecurityAction::Allow,
                            securityManager->GetUsersGroup(),
                            EPermissionSet(EPermission::Use)))
                        .EndList()
                    .EndMap());

            ScheduleCreateNode(
                "//sys/tokens",
                transactionId,
                EObjectType::Document,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("value").BeginMap()
                        .EndMap()
                    .EndMap());

            ScheduleCreateNode(
                NTabletClient::GetCypressClustersPath(),
                transactionId,
                EObjectType::Document,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("value").BeginMap()
                        .EndMap()
                    .EndMap());

            ScheduleCreateNode(
                "//sys/scheduler/instances",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/scheduler/orchid",
                transactionId,
                EObjectType::Orchid);

            ScheduleCreateNode(
                "//sys/scheduler/event_log",
                transactionId,
                EObjectType::Table,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("external").Value(false)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/controller_agents/instances",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/controller_agents/orchid",
                transactionId,
                EObjectType::Orchid);

            // As well as //tmp, this is often a portal. Attempting to create the
            // node when it's already a portal will forward that request to another
            // cell. Which isn't the intention here. Also, that other cell may be
            // unavailable (when tearing down testing envs, for example), which
            // will lead to long stalls while we're waiting for timeout to happen.
            // TODO(shakurov): fix this. Redirection suppression?
            if (!IsInitialized()) {
                ScheduleCreateNode(
                    "//sys/operations",
                    transactionId,
                    EObjectType::MapNode,
                    BuildYsonStringFluently()
                        .BeginMap()
                            .Item("opaque").Value(true)
                        .EndMap());
            }

            ScheduleCreateNode(
                "//sys/proxies",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/rpc_proxies",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/cluster_nodes",
                transactionId,
                EObjectType::ClusterNodeMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                        .Item("config").BeginMap()
                            .Item("%true").BeginMap()
                                .Item("config_annotation").Value("default")
                            .EndMap()
                        .EndMap()
                    .EndMap());

            ScheduleCreateNode(
                "//sys/data_nodes",
                transactionId,
                EObjectType::DataNodeMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/exec_nodes",
                transactionId,
                EObjectType::ExecNodeMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/tablet_nodes",
                transactionId,
                EObjectType::TabletNodeMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/chaos_nodes",
                transactionId,
                EObjectType::ChaosNodeMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/racks",
                transactionId,
                EObjectType::RackMap);

            ScheduleCreateNode(
                "//sys/data_centers",
                transactionId,
                EObjectType::DataCenterMap);

            ScheduleCreateNode(
                "//sys/primary_masters",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/secondary_masters",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/timestamp_providers",
                transactionId,
                EObjectType::MapNode,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/locks",
                transactionId,
                EObjectType::LockMap);

            ScheduleCreateNode(
                "//sys/chunks",
                transactionId,
                EObjectType::ChunkMap);

            ScheduleCreateNode(
                "//sys/lost_chunks",
                transactionId,
                EObjectType::LostChunkMap);

            ScheduleCreateNode(
                "//sys/lost_vital_chunks",
                transactionId,
                EObjectType::LostVitalChunkMap);

            ScheduleCreateNode(
                "//sys/precarious_chunks",
                transactionId,
                EObjectType::PrecariousChunkMap);

            ScheduleCreateNode(
                "//sys/precarious_vital_chunks",
                transactionId,
                EObjectType::PrecariousVitalChunkMap);

            ScheduleCreateNode(
                "//sys/overreplicated_chunks",
                transactionId,
                EObjectType::OverreplicatedChunkMap);

            ScheduleCreateNode(
                "//sys/underreplicated_chunks",
                transactionId,
                EObjectType::UnderreplicatedChunkMap);

            ScheduleCreateNode(
                "//sys/data_missing_chunks",
                transactionId,
                EObjectType::DataMissingChunkMap);

            ScheduleCreateNode(
                "//sys/parity_missing_chunks",
                transactionId,
                EObjectType::ParityMissingChunkMap);

            ScheduleCreateNode(
                "//sys/oldest_part_missing_chunks",
                transactionId,
                EObjectType::OldestPartMissingChunkMap);

            ScheduleCreateNode(
                "//sys/quorum_missing_chunks",
                transactionId,
                EObjectType::QuorumMissingChunkMap);

            ScheduleCreateNode(
                "//sys/unsafely_placed_chunks",
                transactionId,
                EObjectType::UnsafelyPlacedChunkMap);

            ScheduleCreateNode(
                "//sys/foreign_chunks",
                transactionId,
                EObjectType::ForeignChunkMap);

            ScheduleCreateNode(
                "//sys/chunk_views",
                transactionId,
                EObjectType::ChunkViewMap);

            ScheduleCreateNode(
                "//sys/chunk_lists",
                transactionId,
                EObjectType::ChunkListMap);

            ScheduleCreateNode(
                "//sys/master_table_schemas",
                transactionId,
                EObjectType::MasterTableSchemaMap);

            ScheduleCreateNode(
                "//sys/media",
                transactionId,
                EObjectType::MediumMap);

            ScheduleCreateNode(
                "//sys/transactions",
                transactionId,
                EObjectType::TransactionMap);

            ScheduleCreateNode(
                "//sys/topmost_transactions",
                transactionId,
                EObjectType::TopmostTransactionMap);

            ScheduleCreateNode(
                "//sys/accounts",
                transactionId,
                EObjectType::AccountMap);

            ScheduleCreateNode(
                NSecurityClient::RootAccountCypressPath,
                transactionId,
                EObjectType::Link,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("target_path").Value(
                            FromObjectId(Bootstrap_->GetSecurityManager()->GetRootAccount()->GetId()))
                    .EndMap());

            ScheduleCreateNode(
                "//sys/account_resource_usage_leases",
                transactionId,
                EObjectType::AccountResourceUsageLeaseMap);

            ScheduleCreateNode(
                "//sys/users",
                transactionId,
                EObjectType::UserMap);

            ScheduleCreateNode(
                "//sys/groups",
                transactionId,
                EObjectType::GroupMap);

            ScheduleCreateNode(
                "//sys/network_projects",
                transactionId,
                EObjectType::NetworkProjectMap);

            ScheduleCreateNode(
                "//sys/chaos_cell_bundles",
                transactionId,
                EObjectType::ChaosCellBundleMap);

            ScheduleCreateNode(
                "//sys/chaos_cells",
                transactionId,
                EObjectType::ChaosCellMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/tablet_cell_bundles",
                transactionId,
                EObjectType::TabletCellBundleMap);

            ScheduleCreateNode(
                "//sys/tablet_cells",
                transactionId,
                EObjectType::TabletCellMap,
                BuildYsonStringFluently()
                    .BeginMap()
                        .Item("opaque").Value(true)
                    .EndMap());

            ScheduleCreateNode(
                "//sys/tablets",
                transactionId,
                EObjectType::TabletMap);

            ScheduleCreateNode(
                "//sys/tablet_actions",
                transactionId,
                EObjectType::TabletActionMap);

            ScheduleCreateNode(
                "//sys/areas",
                transactionId,
                EObjectType::AreaMap);

            ScheduleCreateNode(
                "//sys/portal_entrances",
                transactionId,
                EObjectType::PortalEntranceMap);

            ScheduleCreateNode(
                "//sys/portal_exits",
                transactionId,
                EObjectType::PortalExitMap);

            ScheduleCreateNode(
                "//sys/cypress_shards",
                transactionId,
                EObjectType::CypressShardMap);

            ScheduleCreateNode(
                "//sys/estimated_creation_time",
                transactionId,
                EObjectType::EstimatedCreationTimeMap);

            ScheduleCreateNode(
                "//sys/ql_pools",
                transactionId,
                EObjectType::MapNode);

            FlushScheduled();

            // Level 3

            for (auto type : objectManager->GetRegisteredTypes()) {
                if (HasSchema(type)) {
                    ScheduleCreateNode(
                        "//sys/schemas/" + ToYPathLiteral(FormatEnum(type)),
                        transactionId,
                        EObjectType::Link,
                        BuildYsonStringFluently()
                            .BeginMap()
                                .Item("target_path").Value(FromObjectId(objectManager->GetSchema(type)->GetId()))
                            .EndMap());
                }
            }

            ScheduleCreateNode(
                "//sys/scheduler/lock",
                transactionId,
                EObjectType::MapNode);

            ScheduleCreateNode(
                "//sys/scheduler/pool_trees_lock",
                transactionId,
                EObjectType::MapNode);

            std::vector<TYPath> orchidAddresses;

            auto createOrchidNode = [&] (const TYPath& addressPath, const TString& address) {
                orchidAddresses.push_back(addressPath);

                ScheduleCreateNode(
                    addressPath + "/orchid",
                    transactionId,
                    EObjectType::Orchid,
                    BuildYsonStringFluently()
                        .BeginMap()
                            .Item("remote_addresses").Value(NNodeTrackerClient::TAddressMap{
                                {NNodeTrackerClient::DefaultNetworkName, address}
                            })
                        .EndMap());
            };

            auto createMasters = [&] (const TYPath& rootPath, NElection::TCellConfigPtr cellConfig) {
                for (const auto& peer : cellConfig->Peers) {
                    const auto& address = *peer.Address;
                    auto addressPath = rootPath + "/" + ToYPathLiteral(address);
                    createOrchidNode(addressPath, address);
                }
            };

            createMasters("//sys/primary_masters", Config_->PrimaryMaster);

            for (auto cellConfig : Config_->SecondaryMasters) {
                auto cellTag = CellTagFromId(cellConfig->CellId);
                auto cellPath = "//sys/secondary_masters/" + ToYPathLiteral(cellTag);
                createMasters(cellPath, cellConfig);
            }

            // TODO(babenko): handle service discovery.
            if (Config_->TimestampProvider->Addresses) {
                for (const auto& timestampProviderAddress : *Config_->TimestampProvider->Addresses) {
                    auto addressPath = "//sys/timestamp_providers/" + ToYPathLiteral(timestampProviderAddress);
                    createOrchidNode(addressPath, timestampProviderAddress);
                }
            }

            auto createDiscoveryOrchid = [&] (const TYPath& path, NElection::TCellConfigPtr cellConfig) {
                ScheduleCreateNode(
                    path,
                    transactionId,
                    EObjectType::Orchid,
                    BuildYsonStringFluently()
                        .BeginMap()
                            .Item("remote_addresses")
                                .DoListFor(cellConfig->Peers, [&] (TFluentList fluent, const auto& peer) {
                                    fluent
                                        .Item().Value(*peer.Address);
                                })
                            .Item("remote_root").Value("//discovery_server")
                        .EndMap());
            };

            createDiscoveryOrchid("//sys/discovery/primary_master_cell", Config_->PrimaryMaster);
            for (auto cellConfig : Config_->SecondaryMasters) {
                auto cellTag = CellTagFromId(cellConfig->CellId);
                auto cellPath = "//sys/discovery/secondary_master_cells/" + ToYPathLiteral(cellTag);
                createDiscoveryOrchid(cellPath, cellConfig);
            }

            if (Config_->DiscoveryServer && Config_->DiscoveryServer->Addresses) {
                for (const auto& discoveryServerAddress : *Config_->DiscoveryServer->Addresses) {
                    auto addressPath = "//sys/discovery_servers/" + ToYPathLiteral(discoveryServerAddress);
                    createOrchidNode(addressPath, discoveryServerAddress);
                }
            }
            OrchidAddresses_.Store(orchidAddresses);

            FlushScheduled();

            // Level 4

            auto orchidAddressToAnnotations = OrchidAddressToAnnotations_.Load();
            for (const auto& orchidAddress : orchidAddresses) {
                auto annotationsPath = orchidAddress + "/@annotations";
                if (orchidAddressToAnnotations.contains(orchidAddress)) {
                    auto annotations = orchidAddressToAnnotations[orchidAddress];
                    ScheduleSetNode(
                        annotationsPath,
                        transactionId,
                        annotations);
                }
            }

            FlushScheduled();

            CommitTransaction(transactionId);

            YT_LOG_INFO("World initialization completed");
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "World initialization failed");
            AbandonScheduled();
            if (transactionId) {
                try {
                    AbortTransaction(transactionId);
                } catch (const std::exception& ex) {
                    YT_LOG_ERROR(ex, "Failed to abort world initialization transaction (TransactionId: %v)",
                        transactionId);
                }
            }
        }

        ScheduleInitialize(IsInitialized()
            ? Config_->WorldInitializer->UpdatePeriod
            : Config_->WorldInitializer->InitRetryPeriod);
    }

    void UpdateAnnotations()
    {
        YT_LOG_DEBUG("Updating annotations");

        auto orchidAddresses = OrchidAddresses_.Load();
        THashMap<TYPath, TYsonString> orchidAddressToAnnotations;

        for (const auto& orchidAddress : orchidAddresses) {
            try {
                auto annotations = GetNode(orchidAddress + "/orchid/config/cypress_annotations");
                YT_VERIFY(orchidAddressToAnnotations.emplace(orchidAddress, annotations).second);
            } catch (const std::exception& ex) {
                YT_LOG_DEBUG(ex, "Failed to get annotations (OrchidAddress: %v)", orchidAddress);
            }
        }

        OrchidAddressToAnnotations_.Store(std::move(orchidAddressToAnnotations));

        YT_LOG_DEBUG("Annotations updated");

        ScheduleUpdateAnnotations(Config_->WorldInitializer->UpdatePeriod);
    }

    TTransactionId StartTransaction()
    {
        TTransactionServiceProxy proxy(Bootstrap_->GetLocalRpcChannel());
        auto req = proxy.StartTransaction();
        req->set_timeout(ToProto<i64>(Config_->WorldInitializer->InitTransactionTimeout));
        req->set_title("World initialization");

        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();
        return FromProto<TTransactionId>(rsp->id());
    }

    void AbortTransaction(TTransactionId transactionId)
    {
        const auto& transactionSupervisor = Bootstrap_->GetTransactionSupervisor();
        WaitFor(transactionSupervisor->AbortTransaction(transactionId))
            .ThrowOnError();
    }

    void CommitTransaction(TTransactionId transactionId)
    {
        const auto& transactionSupervisor = Bootstrap_->GetTransactionSupervisor();
        WaitFor(transactionSupervisor->CommitTransaction(transactionId))
            .ThrowOnError();
    }

    void ScheduleCreateNode(
        const TYPath& path,
        TTransactionId transactionId,
        EObjectType type,
        const TYsonString& attributes = TYsonString(TStringBuf("{}")),
        bool force = false)
    {
        auto service = Bootstrap_->GetObjectManager()->GetRootService();
        auto req = TCypressYPathProxy::Create(path);
        SetTransactionId(req, transactionId);
        req->set_type(static_cast<int>(type));
        req->set_recursive(true);
        if (force) {
            req->set_force(true);
        } else {
            req->set_ignore_existing(true);
            req->set_ignore_type_mismatch(true);
        }
        ToProto(req->mutable_node_attributes(), *ConvertToAttributes(attributes));
        ScheduledMutations_.push_back(ExecuteVerb(service, req).AsVoid());
    }

    void ScheduleSetNode(
        const TYPath& path,
        TTransactionId transactionId,
        const TYsonString& value)
    {
        auto service = Bootstrap_->GetObjectManager()->GetRootService();
        auto req = TCypressYPathProxy::Set(path);
        SetTransactionId(req, transactionId);
        req->set_value(value.ToString());
        ScheduledMutations_.push_back(ExecuteVerb(service, req).AsVoid());
    }

    TYsonString GetNode(const TYPath& path)
    {
        auto service = Bootstrap_->GetObjectManager()->GetRootService();
        auto req = TCypressYPathProxy::Get(path);
        auto rsp = WaitFor(ExecuteVerb(service, req))
            .ValueOrThrow();
        return TYsonString(rsp->value());
    }

    void FlushScheduled()
    {
        std::vector<TFuture<void>> scheduledMutations;
        ScheduledMutations_.swap(scheduledMutations);
        WaitFor(AllSucceeded(scheduledMutations))
            .ThrowOnError();
    }

    void AbandonScheduled()
    {
        ScheduledMutations_.clear();
    }
};

////////////////////////////////////////////////////////////////////////////////

TWorldInitializer::TWorldInitializer(
    TCellMasterConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TWorldInitializer::~TWorldInitializer() = default;

bool TWorldInitializer::IsInitialized()
{
    return Impl_->IsInitialized();
}

void TWorldInitializer::ValidateInitialized()
{
    Impl_->ValidateInitialized();
}

bool TWorldInitializer::HasProvisionLock()
{
    return Impl_->HasProvisionLock();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster

