#include "transaction_service.h"
#include "transaction_manager.h"
#include "private.h"

#include <yt/server/master/cell_master/bootstrap.h>
#include <yt/server/master/cell_master/master_hydra_service.h>
#include <yt/server/master/cell_master/multicell_manager.h>

#include <yt/server/master/transaction_server/proto/transaction_manager.pb.h>

#include <yt/server/lib/hive/hive_manager.h>

#include <yt/ytlib/transaction_client/transaction_service_proxy.h>

#include <yt/client/object_client/helpers.h>

namespace NYT::NTransactionServer {

using namespace NRpc;
using namespace NTransactionClient;
using namespace NObjectClient;
using namespace NHydra;
using namespace NCellMaster;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TTransactionService
    : public TMasterHydraServiceBase
{
public:
    explicit TTransactionService(TBootstrap* bootstrap)
        : TMasterHydraServiceBase(
            bootstrap,
            TTransactionServiceProxy::GetDescriptor(),
            EAutomatonThreadQueue::TransactionSupervisor,
            TransactionServerLogger)
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(StartTransaction));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(RegisterTransactionActions));
    }

private:
    DECLARE_RPC_SERVICE_METHOD(NTransactionClient::NProto, StartTransaction)
    {
        ValidatePeer(EPeerKind::Leader);

        auto parentId = FromProto<TTransactionId>(request->parent_id());
        auto timeout = FromProto<TDuration>(request->timeout());
        auto deadline = request->has_deadline() ? std::make_optional(FromProto<TInstant>(request->deadline())) : std::nullopt;
        auto title = request->has_title() ? std::make_optional(request->title()) : std::nullopt;
        auto prerequisiteTransactionIds = FromProto<std::vector<TTransactionId>>(request->prerequisite_transaction_ids());

        context->SetRequestInfo("ParentId: %v, PrerequisiteTransactionIds: %v, Timeout: %v, Title: %v, Deadline: %v",
            parentId,
            prerequisiteTransactionIds,
            timeout,
            title,
            deadline);

        NTransactionServer::NProto::TReqStartTransaction hydraRequest;
        hydraRequest.mutable_attributes()->Swap(request->mutable_attributes());
        hydraRequest.mutable_parent_id()->Swap(request->mutable_parent_id());
        hydraRequest.mutable_prerequisite_transaction_ids()->Swap(request->mutable_prerequisite_transaction_ids());
        hydraRequest.set_timeout(request->timeout());
        if (request->has_deadline()) {
            hydraRequest.set_deadline(request->deadline());
        }
        hydraRequest.mutable_hint_id()->Swap(request->mutable_hint_id());
        hydraRequest.mutable_replicate_to_cell_tags()->Swap(request->mutable_replicate_to_cell_tags());
        hydraRequest.set_dont_replicate(request->dont_replicate());
        if (title) {
            hydraRequest.set_title(*title);
        }
        NRpc::WriteAuthenticationIdentityToProto(&hydraRequest, context->GetAuthenticationIdentity());

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager
            ->CreateStartTransactionMutation(context, hydraRequest)
            ->CommitAndReply(context);
    }

    DECLARE_RPC_SERVICE_METHOD(NTransactionClient::NProto, RegisterTransactionActions)
    {
        ValidatePeer(EPeerKind::Leader);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        context->SetRequestInfo("TransactionId: %v, ActionCount: %v",
            transactionId,
            request->actions_size());

        auto cellTag = CellTagFromId(transactionId);

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto cellId = multicellManager->GetCellId(cellTag);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        WaitFor(hiveManager->SyncWith(cellId, true))
            .ThrowOnError();

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager
            ->CreateRegisterTransactionActionsMutation(context)
            ->CommitAndReply(context);
    }
};

IServicePtr CreateTransactionService(TBootstrap* bootstrap)
{
    return New<TTransactionService>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionServer
