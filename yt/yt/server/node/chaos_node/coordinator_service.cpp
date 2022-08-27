#include "coordinator_service.h"

#include "bootstrap.h"
#include "chaos_slot.h"
#include "coordinator_manager.h"
#include "private.h"
#include "shortcut_snapshot_store.h"
#include "transaction_manager.h"

#include <yt/yt/server/lib/hydra/distributed_hydra_manager.h>
#include <yt/yt/server/lib/hydra_common/hydra_service.h>

#include <yt/yt/ytlib/chaos_client/coordinator_service_proxy.h>

namespace NYT::NChaosNode {

using namespace NRpc;
using namespace NChaosClient;
using namespace NHydra;

////////////////////////////////////////////////////////////////////////////////

class TCoordinatorService
    : public THydraServiceBase
{
public:
    explicit TCoordinatorService(IChaosSlotPtr slot)
        : THydraServiceBase(
            slot->GetHydraManager(),
            slot->GetGuardedAutomatonInvoker(EAutomatonThreadQueue::Default),
            TCoordinatorServiceProxy::GetDescriptor(),
            ChaosNodeLogger,
            slot->GetCellId(),
            CreateHydraManagerUpstreamSynchronizer(slot->GetHydraManager()))
        , Slot_(std::move(slot))
    {
        YT_VERIFY(Slot_);

        RegisterMethod(RPC_SERVICE_METHOD_DESC(SuspendCoordinator));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ResumeCoordinator));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(RegisterTransactionActions));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetReplicationCardEra)
            .SetInvoker(Slot_->GetSnapshotStoreReadPoolInvoker()));
    }

private:
    const IChaosSlotPtr Slot_;

    DECLARE_RPC_SERVICE_METHOD(NChaosClient::NProto, SuspendCoordinator)
    {
        context->SetRequestInfo();

        const auto& coordinatorManager = Slot_->GetCoordinatorManager();
        coordinatorManager->SuspendCoordinator(std::move(context));
    }

    DECLARE_RPC_SERVICE_METHOD(NChaosClient::NProto, ResumeCoordinator)
    {
        context->SetRequestInfo();

        const auto& coordinatorManager = Slot_->GetCoordinatorManager();
        coordinatorManager->ResumeCoordinator(std::move(context));
    }

    DECLARE_RPC_SERVICE_METHOD(NChaosClient::NProto, RegisterTransactionActions)
    {
        ValidatePeer(EPeerKind::Leader);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto transactionStartTimestamp = request->transaction_start_timestamp();
        auto transactionTimeout = FromProto<TDuration>(request->transaction_timeout());

        context->SetRequestInfo("TransactionId: %v, TransactionStartTimestamp: %x, TransactionTimeout: %v, ActionCount: %v",
            transactionId,
            transactionStartTimestamp,
            transactionTimeout,
            request->actions_size());

        const auto& transactionManager = Slot_->GetTransactionManager();
        transactionManager
            ->CreateRegisterTransactionActionsMutation(context)
            ->CommitAndReply(context);
    }

    DECLARE_RPC_SERVICE_METHOD(NChaosClient::NProto, GetReplicationCardEra)
    {
        auto replicationCardId = FromProto<TReplicationCardId>(request->replication_card_id());
        context->SetRequestInfo("ReplicationCardId: %v",
            replicationCardId);

        ValidateLeader();

        const auto& shortcutSnapshotStore = Slot_->GetShortcutSnapshotStore();
        auto shortcut = shortcutSnapshotStore->GetShortcutOrThrow(replicationCardId);

        response->set_replication_era(shortcut.Era);

        context->SetResponseInfo("Era: %v",
            shortcut.Era);
        context->Reply();
    }

    void ValidateLeader()
    {
        if (!Slot_->GetHydraManager()->IsActiveLeader()) {
            THROW_ERROR_EXCEPTION(
                NRpc::EErrorCode::Unavailable,
                "Not an active leader");
        }
    }
};

IServicePtr CreateCoordinatorService(IChaosSlotPtr slot)
{
    return New<TCoordinatorService>(std::move(slot));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosNode
