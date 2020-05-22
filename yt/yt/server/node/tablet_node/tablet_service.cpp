#include "tablet_service.h"
#include "private.h"
#include "security_manager.h"
#include "slot_manager.h"
#include "store_manager.h"
#include "tablet.h"
#include "tablet_manager.h"
#include "tablet_slot.h"
#include "transaction.h"
#include "transaction_manager.h"

#include <yt/server/node/cluster_node/bootstrap.h>
#include <yt/server/node/cluster_node/config.h>

#include <yt/server/lib/hydra/hydra_manager.h>
#include <yt/server/lib/hydra/hydra_service.h>
#include <yt/server/lib/hydra/mutation.h>

#include <yt/server/lib/misc/profiling_helpers.h>

#include <yt/ytlib/tablet_client/config.h>
#include <yt/ytlib/tablet_client/tablet_service_proxy.h>

#include <yt/ytlib/transaction_client/helpers.h>

#include <yt/client/security_client/public.h>

#include <yt/client/table_client/row_buffer.h>
#include <yt/client/table_client/wire_protocol.h>

#include <yt/core/compression/codec.h>

namespace NYT::NTabletNode {

using namespace NYTree;
using namespace NRpc;
using namespace NCompression;
using namespace NChunkClient;
using namespace NTabletClient;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NHydra;
using namespace NClusterNode;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TTabletService
    : public THydraServiceBase
{
public:
    TTabletService(
        TTabletSlotPtr slot,
        NClusterNode::TBootstrap* bootstrap)
        : THydraServiceBase(
            slot->GetGuardedAutomatonInvoker(EAutomatonThreadQueue::Write),
            TTabletServiceProxy::GetDescriptor(),
            TabletNodeLogger,
            slot->GetCellId())
        , Slot_(slot)
        , Bootstrap_(bootstrap)
    {
        YT_VERIFY(Slot_);
        YT_VERIFY(Bootstrap_);

        RegisterMethod(RPC_SERVICE_METHOD_DESC(Write));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(RegisterTransactionActions));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Trim));
    }

private:
    const TTabletSlotPtr Slot_;
    NClusterNode::TBootstrap* const Bootstrap_;


    DECLARE_RPC_SERVICE_METHOD(NTabletClient::NProto, Write)
    {
        ValidatePeer(EPeerKind::Leader);

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto mountRevision = request->mount_revision();
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto transactionStartTimestamp = request->transaction_start_timestamp();
        auto transactionTimeout = FromProto<TDuration>(request->transaction_timeout());
        auto signature = request->signature();
        auto rowCount = request->row_count();
        auto dataWeight = request->data_weight();
        auto requestCodecId = CheckedEnumCast<NCompression::ECodec>(request->request_codec());
        auto versioned = request->versioned();
        auto syncReplicaIds = FromProto<TSyncReplicaIdList>(request->sync_replica_ids());
        auto upstreamReplicaId = FromProto<TTableReplicaId>(request->upstream_replica_id());

        ValidateTabletTransactionId(transactionId);

        auto atomicity = AtomicityFromTransactionId(transactionId);
        auto durability = CheckedEnumCast<EDurability>(request->durability());

        context->SetRequestInfo("TabletId: %v, TransactionId: %v, TransactionStartTimestamp: %llx, "
            "TransactionTimeout: %v, Atomicity: %v, Durability: %v, Signature: %x, RowCount: %v, DataWeight: %v, "
            "RequestCodec: %v, Versioned: %v, SyncReplicaIds: %v, UpstreamReplicaId: %v",
            tabletId,
            transactionId,
            transactionStartTimestamp,
            transactionTimeout,
            atomicity,
            durability,
            signature,
            rowCount,
            dataWeight,
            requestCodecId,
            versioned,
            syncReplicaIds,
            upstreamReplicaId);

        // NB: Must serve the whole request within a single epoch.
        TCurrentInvokerGuard invokerGuard(Slot_->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Write));

        auto tabletSnapshot = GetTabletSnapshotOrThrow(tabletId, mountRevision);

        if (tabletSnapshot->Atomicity != atomicity) {
            THROW_ERROR_EXCEPTION("Invalid atomicity mode: %Qlv instead of %Qlv",
                atomicity,
                tabletSnapshot->Atomicity);
        }

        if (versioned && context->GetAuthenticationIdentity().User != NSecurityClient::ReplicatorUserName) {
            THROW_ERROR_EXCEPTION("Versioned writes are only allowed for %Qv user",
                NSecurityClient::ReplicatorUserName);
        }

        auto checkUpstreamReplicaId = versioned || tabletSnapshot->UpstreamReplicaId;

        if (checkUpstreamReplicaId) {
            if (upstreamReplicaId && !tabletSnapshot->UpstreamReplicaId) {
                THROW_ERROR_EXCEPTION("Table is not bound to any upstream replica but replica %v was given",
                    upstreamReplicaId);
            } else if (!upstreamReplicaId && tabletSnapshot->UpstreamReplicaId) {
                THROW_ERROR_EXCEPTION("Table is bound to upstream replica %v; direct modifications are forbidden",
                    tabletSnapshot->UpstreamReplicaId);
            } else if (upstreamReplicaId != tabletSnapshot->UpstreamReplicaId) {
                THROW_ERROR_EXCEPTION("Mismatched upstream replica: expected %v, got %v",
                    tabletSnapshot->UpstreamReplicaId,
                    upstreamReplicaId);
            }
        }

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidateResourceLimits(
            tabletSnapshot->WriterOptions->Account,
            tabletSnapshot->WriterOptions->MediumName,
            tabletSnapshot->Config->InMemoryMode);

        auto slotOptions = Slot_->GetOptions();
        securityManager->ValidateResourceLimits(
            slotOptions->ChangelogAccount,
            slotOptions->ChangelogPrimaryMedium,
            EInMemoryMode::None);
        securityManager->ValidateResourceLimits(
            slotOptions->SnapshotAccount,
            slotOptions->SnapshotPrimaryMedium,
            EInMemoryMode::None);

        tabletSnapshot->WaitOnLocks(0); // XXX fix timestamp

        auto* requestCodec = NCompression::GetCodec(requestCodecId);
        auto requestData = requestCodec->Decompress(request->Attachments()[0]);
        struct TWriteBufferTag { };
        TWireProtocolReader reader(requestData, New<TRowBuffer>(TWriteBufferTag()));

        const auto& tabletManager = Slot_->GetTabletManager();

        TFuture<void> commitResult;
        try {
            while (!reader.IsFinished()) {
                // Due to possible row blocking, serving the request may involve a number of write attempts.
                // Each attempt causes a mutation to be enqueued to Hydra.
                // Since all these mutations are enqueued within a single epoch, only the last commit outcome is
                // actually relevant.
                // Note that we're passing signature to every such call but only the last one actually uses it.
                tabletManager->Write(
                    tabletSnapshot,
                    transactionId,
                    transactionStartTimestamp,
                    transactionTimeout,
                    signature,
                    rowCount,
                    dataWeight,
                    versioned,
                    syncReplicaIds,
                    &reader,
                    &commitResult);
            }
        } catch (const TErrorException&) {
            ++tabletSnapshot->PerformanceCounters->WriteErrorCount;
            throw;
        }

        if (atomicity == EAtomicity::None && durability == EDurability::Sync) {
            context->ReplyFrom(commitResult);
        } else {
            context->Reply();
        }
    }

    DECLARE_RPC_SERVICE_METHOD(NTabletClient::NProto, RegisterTransactionActions)
    {
        ValidatePeer(EPeerKind::Leader);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto transactionStartTimestamp = request->transaction_start_timestamp();
        auto transactionTimeout = FromProto<TDuration>(request->transaction_timeout());
        auto signature = request->signature();

        context->SetRequestInfo("TransactionId: %v, TransactionStartTimestamp: %llx, TransactionTimeout: %v, "
            "ActionCount: %v, Signature: %x",
            transactionId,
            transactionStartTimestamp,
            transactionTimeout,
            request->actions_size(),
            signature);

        const auto& transactionManager = Slot_->GetTransactionManager();
        auto future = transactionManager->RegisterTransactionActions(
            transactionId,
            transactionStartTimestamp,
            transactionTimeout,
            signature,
            std::move(*request->mutable_actions()));

        context->ReplyFrom(std::move(future));
    }

    DECLARE_RPC_SERVICE_METHOD(NTabletClient::NProto, Trim)
    {
        ValidatePeer(EPeerKind::Leader);

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto mountRevision = request->mount_revision();
        auto trimmedRowCount = request->trimmed_row_count();

        context->SetRequestInfo("TabletId: %v, TrimmedRowCount: %v",
            tabletId,
            trimmedRowCount);

        auto tabletSnapshot = GetTabletSnapshotOrThrow(tabletId, mountRevision);

        const auto& tabletManager = Slot_->GetTabletManager();
        auto future = tabletManager->Trim(tabletSnapshot, trimmedRowCount);

        context->ReplyFrom(std::move(future));
    }


    TTabletSnapshotPtr GetTabletSnapshotOrThrow(
        TTabletId tabletId,
        NHydra::TRevision mountRevision)
    {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        auto tabletSnapshot = slotManager->GetTabletSnapshotOrThrow(tabletId);
        tabletSnapshot->ValidateCellId(Slot_->GetCellId());
        tabletSnapshot->ValidateMountRevision(mountRevision);
        return tabletSnapshot;
    }

    // THydraServiceBase overrides.
    virtual IHydraManagerPtr GetHydraManager() override
    {
        return Slot_->GetHydraManager();
    }
};

IServicePtr CreateTabletService(TTabletSlotPtr slot, NClusterNode::TBootstrap* bootstrap)
{
    return New<TTabletService>(slot, bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
