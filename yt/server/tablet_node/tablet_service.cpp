#include "stdafx.h"
#include "tablet_service.h"
#include "tablet.h"
#include "tablet_slot.h"
#include "tablet_manager.h"
#include "slot_manager.h"
#include "transaction_manager.h"
#include "transaction.h"
#include "store_manager.h"
#include "security_manager.h"
#include "private.h"

#include <core/compression/helpers.h>

#include <ytlib/tablet_client/tablet_service_proxy.h>
#include <ytlib/tablet_client/wire_protocol.h>

#include <server/hydra/hydra_service.h>

#include <server/hydra/hydra_manager.h>
#include <server/hydra/mutation.h>
#include <server/hydra/rpc_helpers.h>

#include <server/query_agent/helpers.h>

#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>

namespace NYT {
namespace NTabletNode {

using namespace NRpc;
using namespace NCompression;
using namespace NChunkClient;
using namespace NTabletClient;
using namespace NVersionedTableClient;
using namespace NHydra;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

class TTabletService
    : public THydraServiceBase
{
public:
    TTabletService(
        TTabletSlotPtr slot,
        NCellNode::TBootstrap* bootstrap)
        : THydraServiceBase(
            slot->GetHydraManager(),
            slot->GetAutomatonInvoker(),
            TServiceId(TTabletServiceProxy::GetServiceName(), slot->GetCellId()),
            TabletNodeLogger,
            TTabletServiceProxy::GetProtocolVersion())
        , Slot_(slot)
        , Bootstrap_(bootstrap)
    {
        YCHECK(Slot_);
        YCHECK(Bootstrap_);

        RegisterMethod(RPC_SERVICE_METHOD_DESC(StartTransaction));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Read)
            .SetCancelable(true)
            .SetInvoker(Bootstrap_->GetQueryPoolInvoker()));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Write)
            .SetInvoker(Slot_->GetAutomatonInvoker(EAutomatonThreadQueue::Write)));
    }

private:
    const TTabletSlotPtr Slot_;
    NCellNode::TBootstrap* const Bootstrap_;


    DECLARE_RPC_SERVICE_METHOD(NTabletClient::NProto, StartTransaction)
    {
        ValidateActiveLeader();

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto startTimestamp = TTimestamp(request->start_timestamp());
        auto timeout = TDuration::MilliSeconds(request->timeout());

        auto config = Bootstrap_->GetConfig()->TabletNode->TransactionManager;
        auto actualTimeout = std::min(timeout, config->MaxTransactionTimeout);
        request->set_timeout(actualTimeout.MilliSeconds());

        context->SetRequestInfo("TransactionId: %v, StartTimestamp: %v, Timeout: %v",
            transactionId,
            startTimestamp,
            actualTimeout);

        auto transactionManager = Slot_->GetTransactionManager();
        transactionManager
            ->CreateStartTransactionMutation(*request)
            ->Commit()
             .Subscribe(CreateRpcResponseHandler(context));
    }

    DECLARE_RPC_SERVICE_METHOD(NTabletClient::NProto, Read)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto timestamp = TTimestamp(request->timestamp());
        auto requestData = DecompressWithEnvelope(request->Attachments());

        context->SetRequestInfo("TabletId: %v, Timestamp: %v",
            tabletId,
            timestamp);

        auto user = GetAuthenticatedUserOrThrow(context);
        auto securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager, user);

        NQueryAgent::ExecuteRequestWithRetries(
            Bootstrap_->GetConfig()->QueryAgent->MaxQueryRetries,
            Logger,
            [&] () {
                ValidateActiveLeader();

                auto slotManager = Bootstrap_->GetTabletSlotManager();
                auto tabletSnapshot = slotManager->GetTabletSnapshotOrThrow(tabletId);

                TWireProtocolReader reader(requestData);
                TWireProtocolWriter writer;

                auto tabletManager = tabletSnapshot->Slot->GetTabletManager();
                tabletManager->Read(
                    tabletSnapshot,
                    timestamp,
                    &reader,
                    &writer);

                auto responseData = writer.Flush();
                auto responseCodec = request->has_response_codec()
                    ? ECodec(request->response_codec())
                    : ECodec(ECodec::None);
                response->Attachments() = CompressWithEnvelope(responseData,  responseCodec);
                context->Reply();
            });
    }

    DECLARE_RPC_SERVICE_METHOD(NTabletClient::NProto, Write)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto tabletId = FromProto<TTabletId>(request->tablet_id());

        context->SetRequestInfo("TransactionId: %v, TabletId: %v",
            transactionId,
            tabletId);

        auto requestData = NCompression::DecompressWithEnvelope(request->Attachments());
        TWireProtocolReader reader(requestData);

        auto user = GetAuthenticatedUserOrThrow(context);
        auto securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager, user);

        while (!reader.IsFinished()) {
            ValidateActiveLeader();

            // NB: May yield in Write, need to re-fetch tablet and transaction on every iteration.
            auto tabletManager = Slot_->GetTabletManager();
            auto* tablet = tabletManager->GetTabletOrThrow(tabletId);

            auto transactionManager = Slot_->GetTransactionManager();
            auto* transaction = transactionManager->GetTransactionOrThrow(transactionId);

            tabletManager->Write(
                tablet,
                transaction,
                &reader);
        }

        context->Reply();
    }

};

IServicePtr CreateTabletService(TTabletSlotPtr slot, NCellNode::TBootstrap* bootstrap)
{
    return New<TTabletService>(slot, bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
