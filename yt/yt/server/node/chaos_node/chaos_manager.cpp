#include "chaos_manager.h"

#include "automaton.h"
#include "bootstrap.h"
#include "chaos_cell_synchronizer.h"
#include "chaos_slot.h"
#include "private.h"
#include "replication_card.h"
#include "replication_card_observer.h"
#include "slot_manager.h"

#include <yt/yt/server/node/chaos_node/transaction_manager.h>

#include <yt/server/node/chaos_node/chaos_manager.pb.h>

#include <yt/yt/server/lib/hydra/distributed_hydra_manager.h>
#include <yt/yt/server/lib/hydra_common/entity_map.h>

#include <yt/yt/server/lib/hive/hive_manager.h>
#include <yt/yt/server/lib/hive/mailbox.h>

#include <yt/yt/server/lib/chaos_node/config.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/server/lib/hive/helpers.h>

#include <yt/yt/server/lib/transaction_supervisor/helpers.h>

#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/client/chaos_client/helpers.h>
#include <yt/yt/client/chaos_client/replication_card_serialization.h>

#include <yt/yt/client/tablet_client/helpers.h>

#include <yt/yt/client/transaction_client/timestamp_provider.h>

#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/virtual.h>

namespace NYT::NChaosNode {

using namespace NYson;
using namespace NYTree;
using namespace NHydra;
using namespace NClusterNode;
using namespace NHiveServer;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NChaosClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletNode;
using namespace NTransactionSupervisor;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

class TChaosManager
    : public IChaosManager
    , public TChaosAutomatonPart
{
public:
    TChaosManager(
        TChaosManagerConfigPtr config,
        IChaosSlotPtr slot,
        IBootstrap* bootstrap)
        : TChaosAutomatonPart(
            slot,
            bootstrap)
        , Config_(config)
        , OrchidService_(CreateOrchidService())
        , ChaosCellSynchronizer_(CreateChaosCellSynchronizer(Config_->ChaosCellSynchronizer, slot, bootstrap))
        , CommencerExecutor_(New<TPeriodicExecutor>(
            slot->GetAutomatonInvoker(NChaosNode::EAutomatonThreadQueue::EraCommencer),
            BIND(&TChaosManager::PeriodicCurrentTimestampPropagation, MakeWeak(this)),
            Config_->EraCommencingPeriod))
        , ReplicationCardObserver_(CreateReplicationCardObserver(Config_->ReplicationCardObserver, slot))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Slot_->GetAutomatonInvoker(), AutomatonThread);

        RegisterLoader(
            "ChaosManager.Keys",
            BIND(&TChaosManager::LoadKeys, Unretained(this)));
        RegisterLoader(
            "ChaosManager.Values",
            BIND(&TChaosManager::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "ChaosManager.Keys",
            BIND(&TChaosManager::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "ChaosManager.Values",
            BIND(&TChaosManager::SaveValues, Unretained(this)));

        RegisterMethod(BIND(&TChaosManager::HydraGenerateReplicationCardId, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraCreateReplicationCard, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraRemoveReplicationCard, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraChaosNodeRemoveReplicationCard, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraUpdateCoordinatorCells, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraCreateTableReplica, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraRemoveTableReplica, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraAlterTableReplica, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraUpdateTableReplicaProgress, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraCommenceNewReplicationEra, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraPropagateCurrentTimestamps, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraRspGrantShortcuts, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraRspRevokeShortcuts, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraSuspendCoordinator, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraResumeCoordinator, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraRemoveExpiredReplicaHistory, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraMigrateReplicationCards, Unretained(this)));
        RegisterMethod(BIND(&TChaosManager::HydraChaosNodeMigrateReplicationCards, Unretained(this)));
    }

    void Initialize() override
    {
        const auto& transactionManager = Slot_->GetTransactionManager();
        transactionManager->RegisterTransactionActionHandlers(
            MakeTransactionActionHandlerDescriptor(BIND(&TChaosManager::HydraPrepareCreateReplicationCard, MakeStrong(this))),
            MakeTransactionActionHandlerDescriptor(BIND(&TChaosManager::HydraCommitCreateReplicationCard, MakeStrong(this))),
            MakeTransactionActionHandlerDescriptor(BIND(&TChaosManager::HydraAbortCreateReplicationCard, MakeStrong(this))));
    }

    IYPathServicePtr GetOrchidService() const override
    {
        return OrchidService_;
    }


    void GenerateReplicationCardId(const TCtxGenerateReplicationCardIdPtr& context) override
    {
        auto mutation = CreateMutation(
            HydraManager_,
            context,
            &TChaosManager::HydraGenerateReplicationCardId,
            this);
        mutation->CommitAndReply(context);
    }

    void CreateReplicationCard(const TCtxCreateReplicationCardPtr& context) override
    {
        auto mutation = CreateMutation(
            HydraManager_,
            context,
            &TChaosManager::HydraCreateReplicationCard,
            this);
        mutation->CommitAndReply(context);
    }

    void RemoveReplicationCard(const TCtxRemoveReplicationCardPtr& context) override
    {
        auto mutation = CreateMutation(
            HydraManager_,
            context,
            &TChaosManager::HydraRemoveReplicationCard,
            this);
        mutation->CommitAndReply(context);
    }

    void CreateTableReplica(const TCtxCreateTableReplicaPtr& context) override
    {
        auto mutation = CreateMutation(
            HydraManager_,
            context,
            &TChaosManager::HydraCreateTableReplica,
            this);
        mutation->CommitAndReply(context);
    }

    void RemoveTableReplica(const TCtxRemoveTableReplicaPtr& context) override
    {
        auto mutation = CreateMutation(
            HydraManager_,
            context,
            &TChaosManager::HydraRemoveTableReplica,
            this);
        mutation->CommitAndReply(context);
    }

    void AlterTableReplica(const TCtxAlterTableReplicaPtr& context) override
    {
        auto mutation = CreateMutation(
            HydraManager_,
            context,
            &TChaosManager::HydraAlterTableReplica,
            this);
        mutation->CommitAndReply(context);
    }

    void UpdateTableReplicaProgress(const TCtxUpdateTableReplicaProgressPtr& context) override
    {
        auto mutation = CreateMutation(
            HydraManager_,
            context,
            &TChaosManager::HydraUpdateTableReplicaProgress,
            this);
        mutation->CommitAndReply(context);
    }

    void MigrateReplicationCards(const TCtxMigrateReplicationCardsPtr& context) override
    {
        auto mutation = CreateMutation(
            HydraManager_,
            context,
            &TChaosManager::HydraMigrateReplicationCards,
            this);
        mutation->CommitAndReply(context);
    }


    const std::vector<TCellId>& CoordinatorCellIds() override
    {
        return CoordinatorCellIds_;
    }

    bool IsCoordinatorSuspended(TCellId coordinatorCellId) override
    {
        return SuspendedCoordinators_.contains(coordinatorCellId);
    }

    DECLARE_ENTITY_MAP_ACCESSORS_OVERRIDE(ReplicationCard, TReplicationCard);

    TReplicationCard* GetReplicationCardOrThrow(TReplicationCardId replicationCardId) override
    {
        auto* replicationCard = ReplicationCardMap_.Find(replicationCardId);

        if (!replicationCard) {
            // Only replication card origin cell can answer if replication card exists.
            if (IsDomesticReplicationCard(replicationCardId)) {
                THROW_ERROR_EXCEPTION(NYTree::EErrorCode::ResolveError, "No such replication card")
                    << TErrorAttribute("replication_card_id", replicationCardId);
            } else {
                THROW_ERROR_EXCEPTION(NRpc::EErrorCode::Unavailable, "Replication card is not known")
                    << TErrorAttribute("replication_card_id", replicationCardId);
            }
        }

        if (IsReplicationCardMigrated(replicationCard)) {
            THROW_ERROR_EXCEPTION(NRpc::EErrorCode::Unavailable, "Replication card has been migrated")
                << TErrorAttribute("replication_card_id", replicationCardId)
                << TErrorAttribute("immigrated_to_cell_id", replicationCard->Migration().ImmigratedToCellId)
                << TErrorAttribute("immigration_time", replicationCard->Migration().ImmigrationTime);
        }

        return replicationCard;
    }

private:
    class TReplicationCardOrchidService
        : public TVirtualMapBase
    {
    public:
        static IYPathServicePtr Create(TWeakPtr<TChaosManager> impl, IInvokerPtr invoker)
        {
            return New<TReplicationCardOrchidService>(std::move(impl))
                ->Via(invoker);
        }

        std::vector<TString> GetKeys(i64 limit) const override
        {
            std::vector<TString> keys;
            if (auto owner = Owner_.Lock()) {
                for (const auto& [replicationCardId, _] : owner->ReplicationCards()) {
                    if (std::ssize(keys) >= limit) {
                        break;
                    }
                    keys.push_back(ToString(replicationCardId));
                }
            }
            return keys;
        }

        i64 GetSize() const override
        {
            if (auto owner = Owner_.Lock()) {
                return owner->ReplicationCards().size();
            }
            return 0;
        }

        IYPathServicePtr FindItemService(TStringBuf key) const override
        {
            if (auto owner = Owner_.Lock()) {
                if (auto replicationCard = owner->FindReplicationCard(TReplicationCardId::FromString(key))) {
                    auto producer = BIND(&TChaosManager::BuildReplicationCardOrchidYson, owner, replicationCard);
                    return ConvertToNode(producer);
                }
            }
            return nullptr;
        }

    private:
        const TWeakPtr<TChaosManager> Owner_;

        explicit TReplicationCardOrchidService(TWeakPtr<TChaosManager> impl)
            : Owner_(std::move(impl))
        { }

        DECLARE_NEW_FRIEND();
    };

    const TChaosManagerConfigPtr Config_;
    const IYPathServicePtr OrchidService_;
    const IChaosCellSynchronizerPtr ChaosCellSynchronizer_;
    const TPeriodicExecutorPtr CommencerExecutor_;
    const IReplicationCardObserverPtr ReplicationCardObserver_;

    TEntityMap<TReplicationCard> ReplicationCardMap_;
    std::vector<TCellId> CoordinatorCellIds_;
    THashMap<TCellId, TInstant> SuspendedCoordinators_;

    bool NeedRecomputeReplicationCardState_ = false;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void SaveKeys(TSaveContext& context) const
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        ReplicationCardMap_.SaveKeys(context);
    }

    void SaveValues(TSaveContext& context) const
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        using NYT::Save;

        ReplicationCardMap_.SaveValues(context);
        Save(context, CoordinatorCellIds_);
        Save(context, SuspendedCoordinators_);
    }

    void LoadKeys(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        ReplicationCardMap_.LoadKeys(context);
    }

    void LoadValues(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        using NYT::Load;

        ReplicationCardMap_.LoadValues(context);
        Load(context, CoordinatorCellIds_);
        Load(context, SuspendedCoordinators_);

        NeedRecomputeReplicationCardState_ = context.GetVersion() < EChaosReign::Migration;
    }

    void OnAfterSnapshotLoaded() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TChaosAutomatonPart::OnAfterSnapshotLoaded();

        if (NeedRecomputeReplicationCardState_) {
            for (auto& [_, replicationCard] : ReplicationCardMap_) {
                bool alterInProgress = false;
                for (const auto& [_, replica] : replicationCard->Replicas()) {
                    if (!IsStableReplicaState(replica.State) || !IsStableReplicaMode(replica.Mode)) {
                        alterInProgress = true;
                        break;
                    }
                }

                replicationCard->SetState(alterInProgress
                    ? EReplicationCardState::RevokingShortcutsForAlter
                    : EReplicationCardState::Normal);
            }
        }
    }

    void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TChaosAutomatonPart::Clear();

        ReplicationCardMap_.Clear();
        CoordinatorCellIds_.clear();
        SuspendedCoordinators_.clear();
        NeedRecomputeReplicationCardState_ = false;
    }


    void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TChaosAutomatonPart::OnLeaderActive();

        ChaosCellSynchronizer_->Start();
        CommencerExecutor_->Start();
        ReplicationCardObserver_->Start();
    }

    void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TChaosAutomatonPart::OnStopLeading();

        ChaosCellSynchronizer_->Stop();
        CommencerExecutor_->Stop();
        ReplicationCardObserver_->Stop();
    }


    void HydraGenerateReplicationCardId(
        const TCtxGenerateReplicationCardIdPtr& context,
        NChaosClient::NProto::TReqGenerateReplicationCardId* /*request*/,
        NChaosClient::NProto::TRspGenerateReplicationCardId* response)
    {
        auto replicationCardId = GenerateNewReplicationCardId();

        ToProto(response->mutable_replication_card_id(), replicationCardId);

        if (context) {
            context->SetResponseInfo("ReplicationCardId: %v",
                replicationCardId);
        }
    }

    TReplicationCardId CreateReplicationCardImpl(NChaosClient::NProto::TReqCreateReplicationCard* request)
    {
        auto hintId = FromProto<TReplicationCardId>(request->hint_id());
        auto replicationCardId = hintId ? hintId : GenerateNewReplicationCardId();

        auto tableId = FromProto<TTableId>(request->table_id());
        if (tableId && TypeFromId(tableId) != EObjectType::ChaosReplicatedTable) {
            THROW_ERROR_EXCEPTION("Malformed chaos replicated table id %v",
                tableId);
        }

        if (CellTagFromId(replicationCardId) != CellTagFromId(Slot_->GetCellId())) {
            THROW_ERROR_EXCEPTION("Could not create replication card with id %v: expected cell tag %v, got %v",
                replicationCardId,
                CellTagFromId(Slot_->GetCellId()),
                CellTagFromId(replicationCardId));
        }

        auto replicationCardHolder = std::make_unique<TReplicationCard>(replicationCardId);

        auto* replicationCard = replicationCardHolder.get();
        replicationCard->SetTableId(tableId);
        replicationCard->SetTablePath(request->table_path());
        replicationCard->SetTableClusterName(request->table_cluster_name());

        ReplicationCardMap_.Insert(replicationCardId, std::move(replicationCardHolder));

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Replication card created (ReplicationCardId: %v, ReplicationCard: %v)",
            replicationCardId,
            *replicationCard);

        return replicationCardId;
    }

    void HydraCreateReplicationCard(
        const TCtxCreateReplicationCardPtr& context,
        NChaosClient::NProto::TReqCreateReplicationCard* request,
        NChaosClient::NProto::TRspCreateReplicationCard* response)
    {
        auto replicationCardId = CreateReplicationCardImpl(request);

        ToProto(response->mutable_replication_card_id(), replicationCardId);

        if (context) {
            context->SetResponseInfo("ReplicationCardId: %v",
                replicationCardId);
        }
    }

    void HydraPrepareCreateReplicationCard(
        TTransaction* /*transaction*/,
        NChaosClient::NProto::TReqCreateReplicationCard* /*request*/,
        const TTransactionPrepareOptions& /*options*/)
    { }

    void HydraCommitCreateReplicationCard(
        TTransaction* /*transaction*/,
        NChaosClient::NProto::TReqCreateReplicationCard* request,
        const TTransactionCommitOptions& /*options*/)
    {
        CreateReplicationCardImpl(request);
    }

    void HydraAbortCreateReplicationCard(
        TTransaction* /*transaction*/,
        NChaosClient::NProto::TReqCreateReplicationCard* /*request*/,
        const TTransactionAbortOptions& /*options*/)
    { }

    void HydraRemoveReplicationCard(
        const TCtxRemoveReplicationCardPtr& /*context*/,
        NChaosClient::NProto::TReqRemoveReplicationCard* request,
        NChaosClient::NProto::TRspRemoveReplicationCard* /*response*/)
    {
        auto replicationCardId = FromProto<TReplicationCardId>(request->replication_card_id());

        auto* replicationCard = GetReplicationCardOrThrow(replicationCardId);
        RevokeShortcuts(replicationCard);

        if (!IsDomesticReplicationCard(replicationCardId)) {
            const auto& hiveManager = Slot_->GetHiveManager();
            NChaosNode::NProto::TReqRemoveReplicationCard req;
            ToProto(req.mutable_replication_card_id(), replicationCard->GetId());

            auto* mailbox = hiveManager->GetMailbox(replicationCard->Migration().OriginCellId);
            hiveManager->PostMessage(mailbox, req);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Removing migrated replication card at origin cell (ReplicationCardId: %v, OriginCellId: %v)",
                replicationCardId,
                replicationCard->Migration().OriginCellId);
        }

        ReplicationCardMap_.Remove(replicationCardId);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Replication card removed (ReplicationCardId: %v)",
            replicationCardId);
    }

    void HydraChaosNodeRemoveReplicationCard(NChaosNode::NProto::TReqRemoveReplicationCard *request)
    {
        auto replicationCardId = FromProto<TReplicationCardId>(request->replication_card_id());
        auto* replicationCard = FindReplicationCard(replicationCardId);

        if (!replicationCard) {
            YT_LOG_ALERT("Trying to remove emmigrated replication card but it does not exist"
                "(ReplicationCardId: %v)",
                replicationCardId);
            return;
        }

        if (replicationCard->GetState() != EReplicationCardState::Migrated) {
            YT_LOG_ALERT("Trying to remove emmigrated replication card in unexpected state "
                "(ReplicationCardId: %v, ReplicationCardState: %v)",
                replicationCardId,
                replicationCard->GetState());

            return;
        }

        if (!IsDomesticReplicationCard(replicationCard->GetId())) {
            YT_LOG_ALERT("Trying to remove emmigrated replication card but it is not domestic "
                "(ReplicationCardId: %v, OriginCellId: %v)",
                replicationCardId,
                replicationCard->Migration().OriginCellId);

            return;
        }

        ReplicationCardMap_.Remove(replicationCardId);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Replication card removed (ReplicationCardId: %v)",
            replicationCardId);
    }

    void HydraCreateTableReplica(
        const TCtxCreateTableReplicaPtr& context,
        NChaosClient::NProto::TReqCreateTableReplica* request,
        NChaosClient::NProto::TRspCreateTableReplica* response)
    {
        auto replicationCardId = FromProto<TReplicationCardId>(request->replication_card_id());
        const auto& clusterName = request->cluster_name();
        const auto& replicaPath = request->replica_path();
        auto contentType = FromProto<ETableReplicaContentType>(request->content_type());
        auto mode = FromProto<ETableReplicaMode>(request->mode());
        auto enabled = request->enabled();
        auto catchup = request->catchup();
        auto replicationProgress = request->has_replication_progress()
            ? std::make_optional(FromProto<TReplicationProgress>(request->replication_progress()))
            : std::nullopt;

        if (!IsStableReplicaMode(mode)) {
            THROW_ERROR_EXCEPTION("Invalid replica mode %Qlv", mode);
        }

        auto* replicationCard = GetReplicationCardOrThrow(replicationCardId);

        if (std::ssize(replicationCard->Replicas()) >= MaxReplicasPerReplicationCard) {
            THROW_ERROR_EXCEPTION("Replication card already has too many replicas")
                << TErrorAttribute("replication_card_id", replicationCardId)
                << TErrorAttribute("limit", MaxReplicasPerReplicationCard);
        }

        for (const auto& [replicaId, replicaInfo] : replicationCard->Replicas()) {
            if (replicaInfo.ClusterName == clusterName && replicaInfo.ReplicaPath == replicaPath) {
                THROW_ERROR_EXCEPTION("Replica already exists")
                    << TErrorAttribute("replica_id", replicaId)
                    << TErrorAttribute("cluster_name", replicaInfo.ClusterName)
                    << TErrorAttribute("replica_path", replicaInfo.ReplicaPath);
            }
        }

        if (!catchup && replicationProgress) {
            THROW_ERROR_EXCEPTION("Replication progress specified while replica is not to be catched up")
                << TErrorAttribute("replication_progress", *replicationProgress);
        }

        if (!replicationProgress) {
            replicationProgress = TReplicationProgress{
                .Segments = {{EmptyKey(), MinTimestamp}},
                .UpperKey = MaxKey()
            };
        }

        auto isWaitingReplica = [&] {
            for (const auto& [replicaId, replicaInfo] : replicationCard->Replicas()) {
                if (!replicaInfo.History.empty() &&
                    IsReplicationProgressGreaterOrEqual(*replicationProgress, replicaInfo.ReplicationProgress))
                {
                    return true;
                }
            }
            return false;
        };

        // Validate that old data is actually present at queues.
        // To do this we check that at least one replica is as far behind as the new one (as should be in case of replica copying).
        // This is correct since a) data replica first updates its progress at the replication card
        // b) queue only removes data that is older than overall replication card progress (e.g. data 'invisible' to other replicas)

        if (catchup && replicationCard->GetEra() != InitialReplicationEra && !isWaitingReplica()) {
            THROW_ERROR_EXCEPTION("Could not create replica since all other replicas already left it behind")
                << TErrorAttribute("replication_progress", *replicationProgress);
        }

        auto newReplicaId = GenerateNewReplicaId(replicationCard);

        auto& replicaInfo = EmplaceOrCrash(replicationCard->Replicas(), newReplicaId, TReplicaInfo())->second;
        replicaInfo.ClusterName = clusterName;
        replicaInfo.ReplicaPath = replicaPath;
        replicaInfo.ContentType = contentType;
        replicaInfo.State = enabled ? ETableReplicaState::Enabling : ETableReplicaState::Disabled;
        replicaInfo.Mode = mode;
        replicaInfo.ReplicationProgress = std::move(*replicationProgress);

        if (catchup) {
            replicaInfo.History.push_back({
                .Era = replicationCard->GetEra(),
                .Timestamp = MinTimestamp,
                .Mode = mode,
                .State = enabled && replicationCard->GetEra() == InitialReplicationEra
                    ? ETableReplicaState::Enabled
                    : ETableReplicaState::Disabled
            });
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica created (ReplicationCardId: %v, ReplicaId: %v, ReplicaInfo: %v)",
            replicationCardId,
            newReplicaId,
            replicaInfo);

        if (replicaInfo.State == ETableReplicaState::Enabling) {
            UpdateReplicationCardState(replicationCard, EReplicationCardState::RevokingShortcutsForAlter);
        }

        ToProto(response->mutable_replica_id(), newReplicaId);

        if (context) {
            context->SetResponseInfo("ReplicaId: %v",
                newReplicaId);
        }
    }

    void HydraRemoveTableReplica(
        const TCtxRemoveTableReplicaPtr& /*context*/,
        NChaosClient::NProto::TReqRemoveTableReplica* request,
        NChaosClient::NProto::TRspRemoveTableReplica* /*response*/)
    {
        auto replicationCardId = FromProto<TReplicationCardId>(request->replication_card_id());
        auto replicaId = FromProto<NChaosClient::TReplicaId>(request->replica_id());

        auto* replicationCard = GetReplicationCardOrThrow(replicationCardId);
        auto* replicaInfo = replicationCard->GetReplicaOrThrow(replicaId);

        if (replicaInfo->State != ETableReplicaState::Disabled) {
            THROW_ERROR_EXCEPTION("Could not remove replica since it is not disabled")
                << TErrorAttribute("replication_card_id", replicationCardId)
                << TErrorAttribute("replica_id", replicaId)
                << TErrorAttribute("state", replicaInfo->State);
        }

        EraseOrCrash(replicationCard->Replicas(), replicaId);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica removed (ReplicationCardId: %v, ReplicaId: %v)",
            replicationCardId,
            replicaId);
    }

    void HydraAlterTableReplica(
        const TCtxAlterTableReplicaPtr& /*context*/,
        NChaosClient::NProto::TReqAlterTableReplica* request,
        NChaosClient::NProto::TRspAlterTableReplica* /*response*/)
    {
        auto replicationCardId = FromProto<TReplicationCardId>(request->replication_card_id());
        auto replicaId = FromProto<TTableId>(request->replica_id());

        std::optional<ETableReplicaMode> mode;
        if (request->has_mode()) {
            mode = FromProto<ETableReplicaMode>(request->mode());
            if (!IsStableReplicaMode(*mode)) {
                THROW_ERROR_EXCEPTION("Invalid replica mode %Qlv", *mode);
            }
        }

        auto enabled = request->has_enabled()
            ? std::make_optional(request->enabled())
            : std::nullopt;

        auto* replicationCard = GetReplicationCardOrThrow(replicationCardId);
        auto* replicaInfo = replicationCard->GetReplicaOrThrow(replicaId);

        if (!IsStableReplicaMode(replicaInfo->Mode)) {
            THROW_ERROR_EXCEPTION("Replica mode is transitioning")
                << TErrorAttribute("replication_card_id", replicationCardId)
                << TErrorAttribute("replica_id", replicaId)
                << TErrorAttribute("mode", replicaInfo->Mode);
        }

        if (!IsStableReplicaState(replicaInfo->State)) {
            THROW_ERROR_EXCEPTION("Replica state is transitioning")
                << TErrorAttribute("replication_card_id", replicationCardId)
                << TErrorAttribute("replica_id", replicaId)
                << TErrorAttribute("state", replicaInfo->State);
        }

        bool revoke = false;

        if (mode && replicaInfo->Mode != *mode) {
            if (replicaInfo->Mode == ETableReplicaMode::Sync) {
                replicaInfo->Mode = ETableReplicaMode::SyncToAsync;
                revoke = true;
            } else if (replicaInfo->Mode == ETableReplicaMode::Async) {
                replicaInfo->Mode = ETableReplicaMode::AsyncToSync;
                revoke = true;
            }
        }

        bool currentlyEnabled = replicaInfo->State == ETableReplicaState::Enabled;
        if (enabled && *enabled != currentlyEnabled) {
            if (replicaInfo->State == ETableReplicaState::Disabled) {
                replicaInfo->State = ETableReplicaState::Enabling;
                revoke = true;
            } else if (replicaInfo->State == ETableReplicaState::Enabled) {
                replicaInfo->State = ETableReplicaState::Disabling;
                revoke = true;
            }
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica altered (ReplicationCardId: %v, ReplicaId: %v, Replica: %v)",
            replicationCardId,
            replicaId,
            *replicaInfo);

        if (revoke) {
            UpdateReplicationCardState(replicationCard, EReplicationCardState::RevokingShortcutsForAlter);
        }
    }

    void HydraRspGrantShortcuts(NChaosNode::NProto::TRspGrantShortcuts* request)
    {
        auto coordinatorCellId = FromProto<TCellId>(request->coordinator_cell_id());
        bool suspended = request->suspended();
        std::vector<TReplicationCardId> replicationCardIds;

        for (const auto& shortcut : request->shortcuts()) {
            auto replicationCardId = FromProto<TReplicationCardId>(shortcut.replication_card_id());
            auto era = shortcut.era();

            auto* replicationCard = ReplicationCardMap_.Find(replicationCardId);
            if (!replicationCard) {
                YT_LOG_WARNING_IF(IsMutationLoggingEnabled(), "Got grant shortcut response for an unknown replication card (ReplicationCardId: %v)",
                    replicationCardId);
                continue;
            }

            if (replicationCard->GetEra() != era) {
                YT_LOG_ALERT("Got grant shortcut response with invalid era (ReplicationCardId: %v, Era: %v, ResponseEra: %v)",
                    replicationCardId,
                    replicationCard->GetEra(),
                    era);
                continue;
            }

            if (auto it = replicationCard->Coordinators().find(coordinatorCellId); !it || it->second.State != EShortcutState::Granting) {
                YT_LOG_WARNING_IF(IsMutationLoggingEnabled(), "Got grant shortcut response but shortcut is not waiting for it"
                    "(ReplicationCardId: %v, Era: %v, CoordinatorCellId: %v, ShortcutState: %v)",
                    replicationCardId,
                    era,
                    coordinatorCellId,
                    it ? std::make_optional(it->second.State) : std::nullopt);

                continue;
            }

            replicationCardIds.push_back(replicationCardId);
            replicationCard->Coordinators()[coordinatorCellId].State = EShortcutState::Granted;
        }

        if (suspended) {
            SuspendCoordinator(coordinatorCellId);
        } else {
            ResumeCoordinator(coordinatorCellId);
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Shortcuts granted (CoordinatorCellId: %v, Suspended: %v, ReplicationCardIds: %v)",
            coordinatorCellId,
            suspended,
            replicationCardIds);
    }

    void HydraRspRevokeShortcuts(NChaosNode::NProto::TRspRevokeShortcuts* request)
    {
        auto coordinatorCellId = FromProto<TCellId>(request->coordinator_cell_id());
        std::vector<TReplicationCardId> replicationCardIds;

        for (const auto& shortcut : request->shortcuts()) {
            auto replicationCardId = FromProto<TReplicationCardId>(shortcut.replication_card_id());
            auto era = shortcut.era();

            auto* replicationCard = ReplicationCardMap_.Find(replicationCardId);
            if (!replicationCard) {
                YT_LOG_WARNING_IF(IsMutationLoggingEnabled(), "Got revoke shortcut response for an unknown replication card (ReplicationCardId: %v)",
                    replicationCardId);
                continue;
            }

            if (replicationCard->GetEra() != era) {
                YT_LOG_ALERT("Got revoke shortcut response with invalid era (ReplicationCardId: %v, Era: %v, ResponseEra: %v)",
                    replicationCardId,
                    replicationCard->GetEra(),
                    era);
                continue;
            }

            if (auto it = replicationCard->Coordinators().find(coordinatorCellId); it && it->second.State != EShortcutState::Revoking) {
                YT_LOG_WARNING_IF(IsMutationLoggingEnabled(), "Got revoke shortcut response but shortcut is not waiting for it"
                    "(ReplicationCardId: %v, Era: %v CoordinatorCellId: %v, ShortcutState: %v)",
                    replicationCard->GetId(),
                    replicationCard->GetEra(),
                    coordinatorCellId,
                    it->second.State);

                continue;
            }

            replicationCardIds.push_back(replicationCardId);
            EraseOrCrash(replicationCard->Coordinators(), coordinatorCellId);
            HandleReplicationCardStateTransition(replicationCard);
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Shortcuts revoked (CoordinatorCellId: %v, ReplicationCardIds: %v)",
            coordinatorCellId,
            replicationCardIds);
    }

    void RevokeShortcuts(TReplicationCard* replicationCard)
    {
        YT_VERIFY(HasMutationContext());

        const auto& hiveManager = Slot_->GetHiveManager();
        NChaosNode::NProto::TReqRevokeShortcuts req;
        ToProto(req.mutable_chaos_cell_id(), Slot_->GetCellId());
        auto* shortcut = req.add_shortcuts();
        ToProto(shortcut->mutable_replication_card_id(), replicationCard->GetId());
        shortcut->set_era(replicationCard->GetEra());

        for (auto& [cellId, coordinator] : replicationCard->Coordinators()) {
            if (coordinator.State == EShortcutState::Revoking) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Will not revoke shortcut since it already is revoking "
                    "(ReplicationCardId: %v, Era: %v CoordinatorCellId: %v)",
                    replicationCard->GetId(),
                    replicationCard->GetEra(),
                    cellId);

                continue;
            }

            coordinator.State = EShortcutState::Revoking;
            auto* mailbox = hiveManager->GetMailbox(cellId);
            hiveManager->PostMessage(mailbox, req);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Revoking shortcut (ReplicationCardId: %v, Era: %v CoordinatorCellId: %v)",
                replicationCard->GetId(),
                replicationCard->GetEra(),
                cellId);
        }
    }

    void GrantShortcuts(TReplicationCard* replicationCard, const std::vector<TCellId> coordinatorCellIds, bool strict = true)
    {
        YT_VERIFY(HasMutationContext());

        const auto& hiveManager = Slot_->GetHiveManager();
        NChaosNode::NProto::TReqGrantShortcuts req;
        ToProto(req.mutable_chaos_cell_id(), Slot_->GetCellId());
        auto* shortcut = req.add_shortcuts();
        ToProto(shortcut->mutable_replication_card_id(), replicationCard->GetId());
        shortcut->set_era(replicationCard->GetEra());

        for (auto cellId : coordinatorCellIds) {
            // TODO(savrus) This could happen in case if coordinator cell id has been removed from CoordinatorCellIds_ and then added.
            // Need to make a better protocol (YT-16072).
            if (replicationCard->Coordinators().contains(cellId)) {
                if (strict) {
                    YT_LOG_ALERT("Will not grant shortcut since it already is in replication card "
                        "(ReplicationCardId: %v, Era: %v, CoordinatorCellId: %v, CoordinatorState: %v)",
                        replicationCard->GetId(),
                        replicationCard->GetEra(),
                        cellId,
                        replicationCard->Coordinators()[cellId].State);
                }

                continue;
            }

            replicationCard->Coordinators().insert(std::make_pair(cellId, TCoordinatorInfo{EShortcutState::Granting}));
            auto* mailbox = hiveManager->GetOrCreateMailbox(cellId);
            hiveManager->PostMessage(mailbox, req);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Granting shortcut to coordinator (ReplicationCardId: %v, Era: %v, CoordinatorCellId: %v",
                replicationCard->GetId(),
                replicationCard->GetEra(),
                cellId);
        }
    }

    void HydraMigrateReplicationCards(
        const TCtxMigrateReplicationCardsPtr& /*context*/,
        NChaosClient::NProto::TReqMigrateReplicationCards* request,
        NChaosClient::NProto::TRspMigrateReplicationCards* /*response*/)
    {
        auto migrateToCellId = FromProto<TCellId>(request->migrate_to_cell_id());
        auto replicationCardIds = FromProto<std::vector<TReplicationCardId>>(request->replication_card_ids());

        if (std::find(CoordinatorCellIds_.begin(), CoordinatorCellIds_.end(), migrateToCellId) == CoordinatorCellIds_.end()) {
            THROW_ERROR_EXCEPTION("Trying to migrate replication card to unknown cell %v",
                migrateToCellId);
        }

        for (auto replicationCardId : replicationCardIds) {
            auto replicationCard = GetReplicationCardOrThrow(replicationCardId);
            if (replicationCard->GetState() != EReplicationCardState::Normal) {
                THROW_ERROR_EXCEPTION("Trying to migrate replication card %v while it is in %v state",
                    replicationCardId,
                    replicationCard->GetState());
            }
        }

        for (auto replicationCardId : replicationCardIds) {
            auto replicationCard = GetReplicationCardOrThrow(replicationCardId);
            replicationCard->Migration().ImmigratedToCellId = migrateToCellId;
            UpdateReplicationCardState(replicationCard, EReplicationCardState::RevokingShortcutsForMigration);
        }
    }

    void HydraChaosNodeMigrateReplicationCards(NChaosNode::NProto::TReqMigrateReplicationCards* request)
    {
        auto emmigratedFromCellId = FromProto<TCellId>(request->emmigrated_from_cell_id());
        auto now = GetCurrentMutationContext()->GetTimestamp();

        for (const auto& protoMigrationCard : request->migration_cards()) {
            auto replicationCardId = FromProto<TReplicationCardId>(protoMigrationCard.replication_card_id());
            const auto& protoReplicationCard = protoMigrationCard.replication_card();

            auto* replicationCard = FindReplicationCard(replicationCardId);
            if (!replicationCard) {
                if (IsDomesticReplicationCard(replicationCardId)) {
                    // Seems like card has been removed.
                    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Unexpected replication card returned from emmigration (ReplicationCardId: %v)",
                        replicationCardId);
                    continue;
                }

                auto replicationCardHolder = std::make_unique<TReplicationCard>(replicationCardId);
                replicationCard = replicationCardHolder.get();

                ReplicationCardMap_.Insert(replicationCardId, std::move(replicationCardHolder));

                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Replication card created for immigration (ReplicationCardId: %v)",
                    replicationCardId);
            }

            replicationCard->SetTableId(FromProto<TTableId>(protoReplicationCard.table_id()));
            replicationCard->SetTablePath(protoReplicationCard.table_path());
            replicationCard->SetTableClusterName(protoReplicationCard.table_cluster_name());
            replicationCard->SetEra(protoReplicationCard.era());

            YT_VERIFY(replicationCard->Coordinators().empty());

            replicationCard->Replicas().clear();
            for (auto protoReplica : protoReplicationCard.replicas()) {
                auto replicaId = FromProto<TReplicaId>(protoReplica.id());
                auto replicaInfo = FromProto<TReplicaInfo>(protoReplica.info());
                EmplaceOrCrash(replicationCard->Replicas(), replicaId, replicaInfo);
            }

            auto& migration = replicationCard->Migration();
            if (IsDomesticReplicationCard(replicationCardId)) {
                migration.ImmigratedToCellId = TCellId();
                migration.ImmigrationTime = TInstant();
            } else {
                migration.OriginCellId = FromProto<TCellId>(protoMigrationCard.origin_cell_id());
                migration.EmmigratedFromCellId = emmigratedFromCellId;
                migration.EmmigrationTime = now;
            }

            replicationCard->SetState(EReplicationCardState::GeneratingTimestampForNewEra);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Replication card migration started (ReplicationCardId: %v, Domestic: %v, ReplicationCard: %v)",
                replicationCardId,
                IsDomesticReplicationCard(replicationCardId),
                *replicationCard);

            HandleReplicationCardStateTransition(replicationCard);
        }
    }

    void MigrateReplicationCard(TReplicationCard* replicationCard)
    {
        YT_VERIFY(HasMutationContext());
        YT_VERIFY(replicationCard->Coordinators().empty());
        auto immigratedToCellId = replicationCard->Migration().ImmigratedToCellId;

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Migrating replication card to different cell "
            "(ReplicationCardId: %v, ImmigratedToCellId: %v, Domestic: %v)",
            replicationCard->GetId(),
            immigratedToCellId,
            IsDomesticReplicationCard(replicationCard->GetId()));

        NChaosNode::NProto::TReqMigrateReplicationCards req;
        ToProto(req.mutable_emmigrated_from_cell_id(), Slot_->GetCellId());
        auto protoMigrationCard = req.add_migration_cards();
        auto originCellId = IsDomesticReplicationCard(replicationCard->GetId())
            ? Slot_->GetCellId()
            : replicationCard->Migration().OriginCellId;
        ToProto(protoMigrationCard->mutable_origin_cell_id(), originCellId);
        ToProto(protoMigrationCard->mutable_replication_card_id(), replicationCard->GetId());
        auto* protoReplicationCard = protoMigrationCard->mutable_replication_card();

        ToProto(protoReplicationCard->mutable_table_id(), replicationCard->GetTableId());
        protoReplicationCard->set_table_path(replicationCard->GetTablePath());
        protoReplicationCard->set_table_cluster_name(replicationCard->GetTableClusterName());
        protoReplicationCard->set_era(replicationCard->GetEra());

        TReplicationCardFetchOptions fetchOptions{
            .IncludeProgress = true,
            .IncludeHistory = true
        };

        for (const auto& [replicaId, replicaInfo] : replicationCard->Replicas()) {
            auto* protoEntry = protoReplicationCard->add_replicas();
            ToProto(protoEntry->mutable_id(), replicaId);
            ToProto(protoEntry->mutable_info(), replicaInfo, fetchOptions);
        }

        const auto& hiveManager = Slot_->GetHiveManager();
        auto* mailbox = hiveManager->GetMailbox(immigratedToCellId);
        hiveManager->PostMessage(mailbox, req);

        replicationCard->SetState(EReplicationCardState::Migrated);
        replicationCard->Migration().ImmigrationTime = GetCurrentMutationContext()->GetTimestamp();
    }

    bool IsDomesticReplicationCard(TReplicationCardId replicationCardId)
    {
        return CellTagFromId(replicationCardId) == CellTagFromId(Slot_->GetCellId());
    }

    bool IsReplicationCardMigrated(TReplicationCard* replicationCard)
    {
        return replicationCard->GetState() == EReplicationCardState::Migrated;
    }

    void PeriodicCurrentTimestampPropagation(void)
    {
        if (!IsLeader()) {
            return;
        }

        Slot_->GetTimestampProvider()->GenerateTimestamps()
            .Subscribe(BIND(
                &TChaosManager::OnCurrentTimestampPropagationGenerated,
                MakeWeak(this))
                .Via(AutomatonInvoker_));
    }

    void OnCurrentTimestampPropagationGenerated(const TErrorOr<TTimestamp>& timestampOrError)
    {
        if (!IsLeader()) {
            return;
        }

        if (!timestampOrError.IsOK()) {
            YT_LOG_DEBUG(timestampOrError, "Error generating new current timestamp");
            return;
        }

        auto timestamp = timestampOrError.Value();
        YT_LOG_DEBUG("New current timestamp generated (Timestamp: %llx)",
            timestamp);

        NChaosNode::NProto::TReqPropagateCurrentTimestamp request;
        request.set_timestamp(timestamp);
        CreateMutation(HydraManager_, request)
            ->CommitAndLog(Logger);
    }

    void HydraPropagateCurrentTimestamps(NChaosNode::NProto::TReqPropagateCurrentTimestamp* request)
    {
        auto timestamp = request->timestamp();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Started periodic current timestamp propagation (Timestamp: %llx)",
            timestamp);

        for (auto* replicationCard : GetValuesSortedByKey(ReplicationCardMap_)) {
            if (IsReplicationCardMigrated(replicationCard)) {
                continue;
            }

            MaybeCommenceNewReplicationEra(replicationCard, timestamp);
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Finished periodic current timestamp propagation (Timestamp: %llx)",
            timestamp);
    }

    void UpdateReplicationCardState(TReplicationCard* replicationCard, EReplicationCardState newState)
    {
        switch (newState) {
            case EReplicationCardState::RevokingShortcutsForMigration:
                YT_VERIFY(replicationCard->GetState() == EReplicationCardState::Normal);
                replicationCard->SetState(EReplicationCardState::RevokingShortcutsForMigration);
                RevokeShortcuts(replicationCard);
                HandleReplicationCardStateTransition(replicationCard);
                break;

            case EReplicationCardState::RevokingShortcutsForAlter:
                if (replicationCard->GetState() == EReplicationCardState::Normal) {
                    replicationCard->SetState(EReplicationCardState::RevokingShortcutsForAlter);
                    RevokeShortcuts(replicationCard);
                    HandleReplicationCardStateTransition(replicationCard);
                } else {
                    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Skipping replication card state update (ReplicationCardId: %v, State: %v, NewState: %v)",
                        replicationCard->GetId(),
                        replicationCard->GetState(),
                        newState);
                }
                break;

            default:
                YT_ABORT();
        }
    }

    void HandleReplicationCardStateTransition(TReplicationCard* replicationCard)
    {
        while (true) {
            switch(replicationCard->GetState()) {
                case EReplicationCardState::RevokingShortcutsForMigration:
                    if (replicationCard->Coordinators().empty()) {
                        MigrateReplicationCard(replicationCard);
                    }
                    return;

                case EReplicationCardState::RevokingShortcutsForAlter:
                    if (replicationCard->Coordinators().empty()) {
                        replicationCard->SetState(EReplicationCardState::GeneratingTimestampForNewEra);
                        continue;
                    }
                    return;

                case EReplicationCardState::GeneratingTimestampForNewEra:
                    GenerateTimestampForNewEra(replicationCard);
                    return;

                default:
                    YT_ABORT();
            }
        }
    }

    void GenerateTimestampForNewEra(TReplicationCard* replicationCard)
    {
        if (!IsLeader()) {
            return;
        }

        Slot_->GetTimestampProvider()->GenerateTimestamps()
            .Subscribe(BIND(
                &TChaosManager::OnNewReplicationEraTimestampGenerated,
                MakeWeak(this),
                replicationCard->GetId(),
                replicationCard->GetEra())
                .Via(AutomatonInvoker_));
    }

    void OnNewReplicationEraTimestampGenerated(
        TReplicationCardId replicationCardId,
        TReplicationEra era,
        const TErrorOr<TTimestamp>& timestampOrError)
    {
        if (!IsLeader()) {
            return;
        }

        if (!timestampOrError.IsOK()) {
            YT_LOG_DEBUG(timestampOrError, "Error generating new era timestamp (ReplicationCardId: %v, Era: %v)",
                replicationCardId,
                era);
            return;
        }

        auto timestamp = timestampOrError.Value();
        YT_LOG_DEBUG("New era timestamp generated (ReplicationCardId: %v, Era: %v, Timestamp: %llx)",
            replicationCardId,
            era,
            timestamp);

        NChaosNode::NProto::TReqCommenceNewReplicationEra request;
        ToProto(request.mutable_replication_card_id(), replicationCardId);
        request.set_timestamp(timestamp);
        request.set_replication_era(era);
        CreateMutation(HydraManager_, request)
            ->CommitAndLog(Logger);
    }

    void HydraCommenceNewReplicationEra(NChaosNode::NProto::TReqCommenceNewReplicationEra* request)
    {
        auto timestamp = static_cast<TTimestamp>(request->timestamp());
        auto replicationCardId = FromProto<NChaosClient::TReplicationCardId>(request->replication_card_id());
        auto era = static_cast<TReplicationEra>(request->replication_era());

        auto* replicationCard = FindReplicationCard(replicationCardId);
        if (!replicationCard) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Will not commence new replication era because replication card is not found (ReplicationCardId: %v)",
                replicationCardId);
            return;
        }

        if (IsReplicationCardMigrated(replicationCard)) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Will not commence new replication card era since replication card has been migrated (ReplicationCardId: %v)",
                replicationCardId);
            return;
        }

        if (replicationCard->GetEra() != era) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Will not commence new replication card era because of era mismatch (ReplicationCardId: %v, ExpectedEra: %v, ActualEra: %v)",
                era,
                replicationCard->GetEra(),
                replicationCardId);
            return;
        }

        MaybeCommenceNewReplicationEra(replicationCard, timestamp);
    }

    void MaybeCommenceNewReplicationEra(TReplicationCard *replicationCard, TTimestamp timestamp)
    {
        YT_VERIFY(HasMutationContext());

        bool willUpdate = timestamp > replicationCard->GetCurrentTimestamp();
        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Updating replication card current timestamp "
            "(ReplicationCardId: %v, Era: %v, State: %v, CurrentTimestamp: %llx, NewTimestamp: %llx, WillUpdate: %v)",
            replicationCard->GetId(),
            replicationCard->GetEra(),
            replicationCard->GetState(),
            replicationCard->GetCurrentTimestamp(),
            timestamp,
            willUpdate);

        if (!willUpdate) {
            return;
        }

        replicationCard->SetCurrentTimestamp(timestamp);

        if (replicationCard->GetState() != EReplicationCardState::GeneratingTimestampForNewEra) {
            return;
        }

        auto hasSyncQueue = [&] {
            for (const auto& [replicaId, replicaInfo] : replicationCard->Replicas()) {
                if (replicaInfo.ContentType == ETableReplicaContentType::Queue &&
                    (replicaInfo.Mode == ETableReplicaMode::Sync || replicaInfo.Mode == ETableReplicaMode::AsyncToSync))
                {
                    return true;
                }
            }
            return false;
        }();

        if (!hasSyncQueue) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Will not commence new replication era since there would be no sync queue replicas (ReplicationCard: %v)",
                *replicationCard);
            return;
        }

        auto newEra = replicationCard->GetEra() + 1;
        replicationCard->SetEra(newEra);

        for (auto& [replicaId, replicaInfo] : replicationCard->Replicas()) {
            bool updated = false;

            if (replicaInfo.Mode == ETableReplicaMode::SyncToAsync) {
                replicaInfo.Mode = ETableReplicaMode::Async;
                updated = true;
            } else if (replicaInfo.Mode == ETableReplicaMode::AsyncToSync) {
                replicaInfo.Mode = ETableReplicaMode::Sync;
                updated = true;
            }

            if (replicaInfo.State == ETableReplicaState::Disabling) {
                replicaInfo.State = ETableReplicaState::Disabled;
                updated = true;
            } else if (replicaInfo.State == ETableReplicaState::Enabling) {
                replicaInfo.State = ETableReplicaState::Enabled;
                updated = true;
            }

            if (updated) {
                if (replicaInfo.History.empty()) {
                    replicaInfo.ReplicationProgress = TReplicationProgress{
                        .Segments = {{EmptyKey(), timestamp}},
                        .UpperKey = MaxKey()
                    };
                }

                replicaInfo.History.push_back({newEra, timestamp, replicaInfo.Mode, replicaInfo.State});
            }
        }

        replicationCard->SetState(EReplicationCardState::Normal);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Starting new replication era (ReplicationCard: %v, Era: %v, Timestamp: %llx)",
            *replicationCard,
            newEra,
            timestamp);

        GrantShortcuts(replicationCard, CoordinatorCellIds_);
    }

    void HydraSuspendCoordinator(NChaosNode::NProto::TReqSuspendCoordinator* request)
    {
        SuspendCoordinator(FromProto<TCellId>(request->coordinator_cell_id()));
    }

    void HydraResumeCoordinator(NChaosNode::NProto::TReqResumeCoordinator* request)
    {
        ResumeCoordinator(FromProto<TCellId>(request->coordinator_cell_id()));
    }

    void SuspendCoordinator(TCellId coordinatorCellId)
    {
        auto [_, inserted] = SuspendedCoordinators_.emplace(coordinatorCellId, GetCurrentMutationContext()->GetTimestamp());
        if (inserted) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Coordinator suspended (CoordinatorCellId: %v)",
                coordinatorCellId);
        }
    }

    void ResumeCoordinator(TCellId coordinatorCellId)
    {
        auto removed = SuspendedCoordinators_.erase(coordinatorCellId);
        if (removed > 0) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Coordinator resumed (CoordinatorCellId: %v)",
                coordinatorCellId);
        }
    }


    void HydraUpdateCoordinatorCells(NChaosNode::NProto::TReqUpdateCoordinatorCells* request)
    {
        auto newCells = FromProto<std::vector<TCellId>>(request->add_coordinator_cell_ids());
        auto oldCells = FromProto<std::vector<TCellId>>(request->remove_coordinator_cell_ids());
        auto oldCellsSet = THashSet<TCellId>(oldCells.begin(), oldCells.end());
        auto newCellsSet = THashSet<TCellId>(newCells.begin(), newCells.end());
        std::vector<TCellId> removedCells;

        int current = 0;
        for (int index = 0; index < std::ssize(CoordinatorCellIds_); ++index) {
            const auto& cellId = CoordinatorCellIds_[index];

            if (auto it = newCellsSet.find(cellId)) {
                newCellsSet.erase(it);
            }

            if (!oldCellsSet.contains(cellId)) {
                if (current != index) {
                    CoordinatorCellIds_[current] = cellId;
                }
                ++current;
            } else {
                removedCells.push_back(cellId);
            }
        }

        CoordinatorCellIds_.resize(current);
        newCells = std::vector<TCellId>(newCellsSet.begin(), newCellsSet.end());
        std::sort(newCells.begin(), newCells.end());

        for (auto* replicationCard : GetValuesSortedByKey(ReplicationCardMap_)) {
            if (replicationCard->GetState() == EReplicationCardState::Normal) {
                GrantShortcuts(replicationCard, newCells, /*strict*/ false);
            }
        }

        CoordinatorCellIds_.insert(CoordinatorCellIds_.end(), newCells.begin(), newCells.end());

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Coordinator cells updated (AddedCoordinatorCellIds: %v, RemovedCoordinatorCellIds: %v)",
            newCells,
            removedCells);
    }

    void HydraUpdateTableReplicaProgress(
        const TCtxUpdateTableReplicaProgressPtr& /*context*/,
        NChaosClient::NProto::TReqUpdateTableReplicaProgress* request,
        NChaosClient::NProto::TRspUpdateTableReplicaProgress* /*response*/)
    {
        auto replicationCardId = FromProto<TReplicationCardId>(request->replication_card_id());
        auto replicaId = FromProto<TTableId>(request->replica_id());
        auto newProgress = FromProto<TReplicationProgress>(request->replication_progress());

        auto* replicationCard = GetReplicationCardOrThrow(replicationCardId);
        auto* replicaInfo = replicationCard->GetReplicaOrThrow(replicaId);

        if (replicaInfo->History.empty()) {
            THROW_ERROR_EXCEPTION("Replication progress update is prohibited because replica history has not been started yet")
                << TErrorAttribute("replication_card_id", replicationCardId)
                << TErrorAttribute("replica_id", replicaId);
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Updating replication progress (ReplicationCardId: %v, ReplicaId: %v, OldProgress: %v, NewProgress: %v)",
            replicationCardId,
            replicaId,
            replicaInfo->ReplicationProgress,
            newProgress);

        NChaosClient::UpdateReplicationProgress(&replicaInfo->ReplicationProgress, newProgress);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Replication progress updated (ReplicationCardId: %v, ReplicaId: %v, Progress: %v)",
            replicationCardId,
            replicaId,
            replicaInfo->ReplicationProgress);
    }

    void HydraRemoveExpiredReplicaHistory(NProto::TReqRemoveExpiredReplicaHistory *request)
    {
        auto expires = FromProto<std::vector<TExpiredReplicaHistory>>(request->expired_replica_histories());

        for (const auto [replicaId, retainTimestamp] : expires) {
            auto replicationCardId = ReplicationCardIdFromReplicaId(replicaId);
            auto* replicationCard = FindReplicationCard(replicationCardId);
            if (!replicationCard || IsReplicationCardMigrated(replicationCard)) {
                continue;
            }

            auto* replica = replicationCard->FindReplica(replicaId);
            if (!replica) {
                continue;
            }

            auto historyIndex = replica->FindHistoryItemIndex(retainTimestamp);
            if (historyIndex > 0) {
                replica->History.erase(
                    replica->History.begin(),
                    replica->History.begin() + historyIndex);

                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Forsaken old replica history items (RepliationCardId: %v, ReplicaId: %v, RetainTimestamp: %v, HistoryItemIndex: %v)",
                    replicationCardId,
                    replicaId,
                    retainTimestamp,
                    historyIndex);
            }
        }
    }


    TReplicationCardId GenerateNewReplicationCardId()
    {
        return MakeReplicationCardId(Slot_->GenerateId(EObjectType::ReplicationCard));
    }

    TReplicaId GenerateNewReplicaId(TReplicationCard* replicationCard)
    {
        while (true) {
            auto index = replicationCard->GetCurrentReplicaIdIndex();
            // NB: Wrap-around is possible.
            replicationCard->SetCurrentReplicaIdIndex(index + 1);
            auto replicaId = MakeReplicaId(replicationCard->GetId(), index);
            if (!replicationCard->Replicas().contains(replicaId)) {
                return replicaId;
            }
        }
    }


    TCompositeMapServicePtr CreateOrchidService()
    {
        return New<TCompositeMapService>()
            ->AddAttribute(EInternedAttributeKey::Opaque, BIND([] (IYsonConsumer* consumer) {
                    BuildYsonFluently(consumer)
                        .Value(true);
                }))
            ->AddChild("coordinators", IYPathService::FromMethod(
                &TChaosManager::BuildCoordinatorsOrchid,
                MakeWeak(this))
                ->Via(Slot_->GetAutomatonInvoker()))
            ->AddChild("suspended_coordinators", IYPathService::FromMethod(
                &TChaosManager::BuildSuspendedCoordinatorsOrchid,
                MakeWeak(this))
                ->Via(Slot_->GetAutomatonInvoker()))
            ->AddChild("replication_cards", TReplicationCardOrchidService::Create(MakeWeak(this), Slot_->GetGuardedAutomatonInvoker()));
    }

    void BuildCoordinatorsOrchid(IYsonConsumer* consumer) const
    {
        BuildYsonFluently(consumer)
            .DoListFor(CoordinatorCellIds_, [] (TFluentList fluent, const auto& coordinatorCellId) {
                fluent
                    .Item().Value(coordinatorCellId);
                });
    }

    void BuildSuspendedCoordinatorsOrchid(IYsonConsumer* consumer) const
    {
        BuildYsonFluently(consumer)
            .DoListFor(SuspendedCoordinators_, [] (TFluentList fluent, const auto& suspended) {
                fluent
                    .Item().BeginMap()
                        .Item("coordinator_cell_id").Value(suspended.first)
                        .Item("suspension_time").Value(suspended.second)
                    .EndMap();
                });
    }

    void BuildReplicationCardOrchidYson(TReplicationCard* card, IYsonConsumer* consumer)
    {
        const auto& migration = card->Migration();
        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("replication_card_id").Value(card->GetId())
                .Item("era").Value(card->GetEra())
                .Item("state").Value(card->GetState())
                .Item("coordinators").DoMapFor(card->Coordinators(), [] (TFluentMap fluent, const auto& pair) {
                    fluent
                        .Item(ToString(pair.first)).Value(pair.second.State);
                })
                .Item("replicas").DoListFor(card->Replicas(), [] (TFluentList fluent, const auto& replicaInfo) {
                    Serialize(replicaInfo, fluent.GetConsumer());
                })
                .Item("migration")
                    .BeginMap()
                        .DoIf(static_cast<bool>(migration.OriginCellId), [&] (TFluentMap fluent) {
                            fluent.Item("origin_cell_id").Value(migration.OriginCellId);
                        })
                        .DoIf(static_cast<bool>(migration.ImmigratedToCellId), [&] (TFluentMap fluent) {
                            fluent.Item("immigrated_to_cell_id").Value(migration.ImmigratedToCellId);
                        })
                        .DoIf(static_cast<bool>(migration.EmmigratedFromCellId), [&] (TFluentMap fluent) {
                            fluent.Item("emmigrated_from_cell_id").Value(migration.EmmigratedFromCellId);
                        })
                        .DoIf(static_cast<bool>(migration.ImmigrationTime), [&] (TFluentMap fluent) {
                            fluent.Item("immigration_time").Value(migration.ImmigrationTime);
                        })
                        .DoIf(static_cast<bool>(migration.EmmigrationTime), [&] (TFluentMap fluent) {
                            fluent.Item("emmigration_time").Value(migration.EmmigrationTime);
                        })
                    .EndMap()
            .EndMap();
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TChaosManager, ReplicationCard, TReplicationCard, ReplicationCardMap_)

////////////////////////////////////////////////////////////////////////////////

IChaosManagerPtr CreateChaosManager(
    TChaosManagerConfigPtr config,
    IChaosSlotPtr slot,
    IBootstrap* bootstrap)
{
    return New<TChaosManager>(
        config,
        slot,
        bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosNode
