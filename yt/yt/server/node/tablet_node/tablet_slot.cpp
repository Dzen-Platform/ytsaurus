#include "tablet_slot.h"

#include "automaton.h"
#include "bootstrap.h"
#include "master_connector.h"
#include "private.h"
#include "security_manager.h"
#include "serialize.h"
#include "slot_manager.h"
#include "tablet.h"
#include "tablet_manager.h"
#include "tablet_cell_write_manager.h"
#include "tablet_service.h"
#include "transaction_manager.h"
#include "tablet_snapshot_store.h"
#include "hint_manager.h"

#include <yt/yt/server/node/data_node/config.h>

#include <yt/yt/server/lib/cellar_agent/automaton_invoker_hood.h>
#include <yt/yt/server/lib/cellar_agent/occupant.h>

#include <yt/yt/server/lib/election/election_manager.h>

#include <yt/yt/server/lib/hive/hive_manager.h>
#include <yt/yt/server/lib/hive/mailbox.h>
#include <yt/yt/server/lib/hive/transaction_supervisor.h>
#include <yt/yt/server/lib/hive/transaction_lease_tracker.h>

#include <yt/yt/server/lib/hydra_common/remote_changelog_store.h>
#include <yt/yt/server/lib/hydra_common/remote_snapshot_store.h>

#include <yt/yt/server/lib/hydra/distributed_hydra_manager.h>

#include <yt/yt/server/lib/hive/transaction_participant_provider.h>

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/server/node/cellar_node/master_connector.h>

#include <yt/yt/server/node/cluster_node/bootstrap.h>
#include <yt/yt/server/node/cluster_node/master_connector.h>

#include <yt/yt/server/node/data_node/legacy_master_connector.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/ytlib/chunk_client/chunk_fragment_reader.h>

#include <yt/yt/ytlib/misc/memory_usage_tracker.h>

#include <yt/yt/client/api/client.h>
#include <yt/yt/client/api/transaction.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/security_client/public.h>

#include <yt/yt/client/transaction_client/timestamp_provider.h>

#include <yt/yt/ytlib/hive/cluster_directory_synchronizer.h>

#include <yt/yt/ytlib/tablet_client/config.h>

#include <yt/yt/ytlib/election/cell_manager.h>

#include <yt/yt/core/concurrency/fair_share_action_queue.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/rpc/response_keeper.h>

#include <yt/yt/core/ytree/virtual.h>
#include <yt/yt/core/ytree/helpers.h>

#include <yt/yt/core/misc/atomic_object.h>

namespace NYT::NTabletNode {

using namespace NApi;
using namespace NCellarClient;
using namespace NCellarAgent;
using namespace NConcurrency;
using namespace NElection;
using namespace NHiveClient;
using namespace NHiveServer;
using namespace NHydra;
using namespace NObjectClient;
using namespace NRpc;
using namespace NTabletClient::NProto;
using namespace NTabletClient;
using namespace NChunkClient;
using namespace NYTree;
using namespace NYson;

using NHydra::EPeerState;

////////////////////////////////////////////////////////////////////////////////

class TTabletSlot
    : public TAutomatonInvokerHood<EAutomatonThreadQueue>
    , public ITabletSlot
    , public ITransactionManagerHost
{
private:
    using THood = TAutomatonInvokerHood<EAutomatonThreadQueue>;

public:
    TTabletSlot(
        int slotIndex,
        TTabletNodeConfigPtr config,
        IBootstrap* bootstrap)
        : THood(Format("TabletSlot:%v", slotIndex))
        , Config_(config)
        , Bootstrap_(bootstrap)
        , SnapshotQueue_(New<TActionQueue>(
            Format("TabletSnap:%v", slotIndex)))
        , Logger(TabletNodeLogger)
    {
        VERIFY_INVOKER_THREAD_AFFINITY(GetAutomatonInvoker(), AutomatonThread);

        ResetEpochInvokers();
        ResetGuardedInvokers();
    }

    void SetOccupant(ICellarOccupantPtr occupant) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YT_VERIFY(!Occupant_);

        Occupant_ = std::move(occupant);
        Logger = GetLogger();
    }

    IInvokerPtr GetAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return THood::GetAutomatonInvoker(queue);
    }

    IInvokerPtr GetOccupierAutomatonInvoker() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return GetAutomatonInvoker(EAutomatonThreadQueue::Default);
    }

    IInvokerPtr GetMutationAutomatonInvoker() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return GetAutomatonInvoker(EAutomatonThreadQueue::Mutation);
    }

    IInvokerPtr GetEpochAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return THood::GetEpochAutomatonInvoker(queue);
    }

    IInvokerPtr GetGuardedAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return THood::GetGuardedAutomatonInvoker(queue);
    }

    TCellId GetCellId() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetCellId();
    }

    EPeerState GetAutomatonState() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto hydraManager = GetHydraManager();
        return hydraManager ? hydraManager->GetAutomatonState() : EPeerState::None;
    }

    const TString& GetTabletCellBundleName() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetCellBundleName();
    }

    IDistributedHydraManagerPtr GetHydraManager() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetHydraManager();
    }

    ISimpleHydraManagerPtr GetSimpleHydraManager() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetHydraManager();
    }

    const TCompositeAutomatonPtr& GetAutomaton() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Occupant_->GetAutomaton();
    }

    const THiveManagerPtr& GetHiveManager() override
    {
        return Occupant_->GetHiveManager();
    }

    TMailbox* GetMasterMailbox() override
    {
        return Occupant_->GetMasterMailbox();
    }

    void CommitTabletMutation(const ::google::protobuf::MessageLite& message) override
    {
        auto mutation = CreateMutation(GetHydraManager(), message);
        GetEpochAutomatonInvoker()->Invoke(BIND([=, this_ = MakeStrong(this), mutation = std::move(mutation)] {
            mutation->CommitAndLog(Logger);
        }));
    }

    void PostMasterMessage(TTabletId tabletId, const ::google::protobuf::MessageLite& message) override
    {
        YT_VERIFY(HasMutationContext());

        const auto& hiveManager = GetHiveManager();
        auto* mailbox = hiveManager->GetOrCreateMailbox(Bootstrap_->GetCellId(CellTagFromId(tabletId)));
        if (!mailbox) {
            mailbox = GetMasterMailbox();
        }
        hiveManager->PostMessage(mailbox, message);
    }

    const TTransactionManagerPtr& GetTransactionManager() override
    {
        return TransactionManager_;
    }

    ITransactionManagerPtr GetOccupierTransactionManager() override
    {
        return GetTransactionManager();
    }

    const ITransactionSupervisorPtr& GetTransactionSupervisor() override
    {
        return Occupant_->GetTransactionSupervisor();
    }

    const TTabletManagerPtr& GetTabletManager() override
    {
        return TabletManager_;
    }

    const ITabletCellWriteManagerPtr& GetTabletCellWriteManager() override
    {
        return TabletCellWriteManager_;
    }

    TObjectId GenerateId(EObjectType type) override
    {
        return Occupant_->GenerateId(type);
    }

    TCompositeAutomatonPtr CreateAutomaton() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return New<TTabletAutomaton>(
            SnapshotQueue_->GetInvoker(),
            GetCellId());
    }

    TCellTag GetNativeCellTag() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Bootstrap_->GetMasterClient()->GetNativeConnection()->GetPrimaryMasterCellTag();
    }

    const NNative::IConnectionPtr& GetNativeConnection() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Bootstrap_->GetMasterClient()->GetNativeConnection();
    }
        

    TFuture<TTabletCellMemoryStatistics> GetMemoryStatistics() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND(&TTabletSlot::DoGetMemoryStatistics, MakeStrong(this))
            .AsyncVia(GetAutomatonInvoker())
            .Run();
    }

    TTabletCellMemoryStatistics DoGetMemoryStatistics()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return TTabletCellMemoryStatistics{
            .CellId = GetCellId(),
            .BundleName = GetTabletCellBundleName(),
            .Tablets = TabletManager_->GetMemoryStatistics()
        };
    }

    TTimestamp GetLatestTimestamp() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Bootstrap_
            ->GetMasterConnection()
            ->GetTimestampProvider()
            ->GetLatestTimestamp();
    }

    void Configure(IDistributedHydraManagerPtr hydraManager) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        hydraManager->SubscribeStartLeading(BIND(&TTabletSlot::OnStartEpoch, MakeWeak(this)));
        hydraManager->SubscribeStartFollowing(BIND(&TTabletSlot::OnStartEpoch, MakeWeak(this)));

        hydraManager->SubscribeStopLeading(BIND(&TTabletSlot::OnStopEpoch, MakeWeak(this)));
        hydraManager->SubscribeStopFollowing(BIND(&TTabletSlot::OnStopEpoch, MakeWeak(this)));

        InitGuardedInvokers(hydraManager);

        // NB: Tablet Manager must register before Transaction Manager since the latter
        // will be writing and deleting rows during snapshot loading.
        TabletManager_ = New<TTabletManager>(
            Config_->TabletManager,
            this,
            Bootstrap_);

        TransactionManager_ = New<TTransactionManager>(
            Config_->TransactionManager,
            this,
            GetOptions()->ClockClusterTag,
            CreateTransactionLeaseTracker(
                Bootstrap_->GetTransactionTrackerInvoker(),
                Logger));

        Logger = GetLogger();

        TabletCellWriteManager_ = CreateTabletCellWriteManager(
            TabletManager_->GetTabletCellWriteManagerHost(),
            hydraManager,
            GetAutomaton(),
            TMemoryUsageTrackerGuard::Acquire(
                Bootstrap_
                ->GetMemoryUsageTracker()
                ->WithCategory(NNodeTrackerClient::EMemoryCategory::TabletDynamic),
                0 /*size*/,
                MemoryUsageGranularity),
            GetAutomatonInvoker());
    }

    void Initialize() override
    {
        TabletService_ = CreateTabletService(
            this,
            Bootstrap_);

        TabletManager_->Initialize();
        TabletCellWriteManager_->Initialize();
    }

    void RegisterRpcServices() override
    {
        const auto& rpcServer = Bootstrap_->GetRpcServer();
        rpcServer->RegisterService(TabletService_);
    }

    void Stop() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        const auto& snapshotStore = Bootstrap_->GetTabletSnapshotStore();
        snapshotStore->UnregisterTabletSnapshots(this);

        ResetEpochInvokers();
        ResetGuardedInvokers();
    }

    void Finalize() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        TabletManager_->Finalize();
        TabletManager_.Reset();

        TransactionManager_.Reset();
        TabletCellWriteManager_.Reset();

        if (TabletService_) {
            const auto& rpcServer = Bootstrap_->GetRpcServer();
            rpcServer->UnregisterService(TabletService_);
            TabletService_.Reset();
        }
    }

    ECellarType GetCellarType() override
    {
        return CellarType;
    }

    TCompositeMapServicePtr PopulateOrchidService(TCompositeMapServicePtr orchid) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return orchid
            ->AddChild("life_stage", IYPathService::FromMethod(
                &TTabletManager::GetTabletCellLifeStage,
                MakeWeak(TabletManager_))
                ->Via(GetAutomatonInvoker()))
            ->AddChild("transactions", TransactionManager_->GetOrchidService())
            ->AddChild("tablets", TabletManager_->GetOrchidService());
    }

    const TRuntimeTabletCellDataPtr& GetRuntimeData() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return RuntimeData_;
    }

    double GetUsedCpu(double cpuPerTabletSlot) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return GetDynamicOptions()->CpuPerTabletSlot.value_or(cpuPerTabletSlot);
    }

    TDynamicTabletCellOptionsPtr GetDynamicOptions() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetDynamicOptions();
    }

    TTabletCellOptionsPtr GetOptions() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Occupant_->GetOptions();
    }

    NProfiling::TProfiler GetProfiler() override
    {
        return TabletNodeProfiler;
    }

    IChunkFragmentReaderPtr CreateChunkFragmentReader(TTablet* tablet) override
    {
        return NChunkClient::CreateChunkFragmentReader(
            tablet->GetSettings().HunkReaderConfig,
            Bootstrap_->GetMasterClient(),
            Bootstrap_->GetHintManager(),
            tablet->GetTableProfiler()->GetProfiler().WithPrefix("/chunk_fragment_reader"));
    }

private:
    const TTabletNodeConfigPtr Config_;
    IBootstrap* const Bootstrap_;

    ICellarOccupantPtr Occupant_;

    const TActionQueuePtr SnapshotQueue_;

    NLogging::TLogger Logger;

    const TRuntimeTabletCellDataPtr RuntimeData_ = New<TRuntimeTabletCellData>();

    TTabletManagerPtr TabletManager_;

    ITabletCellWriteManagerPtr TabletCellWriteManager_;

    TTransactionManagerPtr TransactionManager_;

    ITransactionSupervisorPtr TransactionSupervisor_;

    NRpc::IServicePtr TabletService_;


    void OnStartEpoch()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        InitEpochInvokers(GetHydraManager());
    }

    void OnStopEpoch()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        ResetEpochInvokers();
    }

    NLogging::TLogger GetLogger() const
    {
        return TabletNodeLogger.WithTag("CellId: %v, PeerId: %v",
            Occupant_->GetCellId(),
            Occupant_->GetPeerId());
    }

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);
};

ITabletSlotPtr CreateTabletSlot(
    int slotIndex,
    TTabletNodeConfigPtr config,
    IBootstrap* bootstrap)
{
    return New<TTabletSlot>(
        slotIndex,
        std::move(config),
        bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode::NYT
