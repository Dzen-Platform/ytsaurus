#include "transaction_supervisor.h"
#include "commit.h"
#include "config.h"
#include "hive_manager.h"
#include "transaction_manager.h"

#include <yt/server/election/election_manager.h>

#include <yt/server/hive/transaction_supervisor.pb.h>

#include <yt/server/hydra/composite_automaton.h>
#include <yt/server/hydra/entity_map.h>
#include <yt/server/hydra/hydra_manager.h>
#include <yt/server/hydra/hydra_service.h>
#include <yt/server/hydra/mutation.h>

#include <yt/ytlib/hive/transaction_supervisor_service_proxy.h>
#include <yt/ytlib/hive/private.h>

#include <yt/ytlib/transaction_client/timestamp_provider.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/rpc/message.h>
#include <yt/core/rpc/response_keeper.h>
#include <yt/core/rpc/rpc.pb.h>
#include <yt/core/rpc/server.h>
#include <yt/core/rpc/service_detail.h>

#include <yt/core/ytree/attribute_helpers.h>

namespace NYT {
namespace NHive {

using namespace NRpc;
using namespace NRpc::NProto;
using namespace NHydra;
using namespace NHive::NProto;
using namespace NTransactionClient;
using namespace NYTree;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TTransactionSupervisor::TImpl
    : public THydraServiceBase
    , public TCompositeAutomatonPart
{
public:
    TImpl(
        TTransactionSupervisorConfigPtr config,
        IInvokerPtr automatonInvoker,
        IInvokerPtr trackerInvoker,
        IHydraManagerPtr hydraManager,
        TCompositeAutomatonPtr automaton,
        TResponseKeeperPtr responseKeeper,
        THiveManagerPtr hiveManager,
        ITransactionManagerPtr transactionManager,
        ITimestampProviderPtr timestampProvider)
        : THydraServiceBase(
            hydraManager->CreateGuardedAutomatonInvoker(automatonInvoker),
            TServiceId(TTransactionSupervisorServiceProxy::GetServiceName(), hiveManager->GetSelfCellId()),
            HiveLogger,
            TTransactionSupervisorServiceProxy::GetProtocolVersion())
        , TCompositeAutomatonPart(
            hydraManager,
            automaton,
            automatonInvoker)
        , Config_(config)
        , TrackerInvoker_(trackerInvoker)
        , HydraManager_(hydraManager)
        , ResponseKeeper_(responseKeeper)
        , HiveManager_(hiveManager)
        , TransactionManager_(transactionManager)
        , TimestampProvider_(timestampProvider)
    {
        YCHECK(Config_);
        YCHECK(TrackerInvoker_);
        YCHECK(ResponseKeeper_);
        YCHECK(HiveManager_);
        YCHECK(TransactionManager_);
        YCHECK(TimestampProvider_);

        Logger.AddTag("CellId: %v", hiveManager->GetSelfCellId());

        TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(CommitTransaction));
        TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(AbortTransaction));
        TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(PingTransaction)
            .SetInvoker(TrackerInvoker_));

        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCommitSimpleTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCommitDistributedTransactionPhaseOne, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraPrepareTransactionCommit, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraOnTransactionCommitPrepared, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCommitDistributedTransactionPhaseTwo, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCommitPreparedTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraAbortTransaction, Unretained(this)));

        RegisterLoader(
            "TransactionSupervisor.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "TransactionSupervisor.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "TransactionSupervisor.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "TransactionSupervisor.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));
    }

    IServicePtr GetRpcService()
    {
        return this;
    }

    TFuture<void> CommitTransaction(
        const TTransactionId& transactionId,
        const std::vector<TCellId>& participantCellIds)
    {
        return MessageToError(DoCommitTransaction(
            transactionId,
            participantCellIds,
            NullMutationId));
    }

    TFuture<void> AbortTransaction(
        const TTransactionId& transactionId,
        bool force)
    {
        return MessageToError(DoAbortTransaction(
            transactionId,
            NullMutationId,
            force));
    }

private:
    const TTransactionSupervisorConfigPtr Config_;
    const IInvokerPtr TrackerInvoker_;
    const IHydraManagerPtr HydraManager_;
    const TResponseKeeperPtr ResponseKeeper_;
    const THiveManagerPtr HiveManager_;
    const ITransactionManagerPtr TransactionManager_;
    const ITimestampProviderPtr TimestampProvider_;

    TEntityMap<TTransactionId, TCommit> TransientCommitMap_;
    TEntityMap<TTransactionId, TCommit> PersistentCommitMap_;


    // RPC handlers.

    DECLARE_RPC_SERVICE_METHOD(NProto, CommitTransaction)
    {
        ValidatePeer(EPeerKind::Leader);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto participantCellIds = FromProto<std::vector<TCellId>>(request->participant_cell_ids());

        context->SetRequestInfo("TransactionId: %v, ParticipantCellIds: %v",
            transactionId,
            participantCellIds);

        if (ResponseKeeper_->TryReplyFrom(context))
            return;

        auto asyncResponseMessage = DoCommitTransaction(
            transactionId,
            participantCellIds,
            GetMutationId(context));
        context->ReplyFrom(asyncResponseMessage);
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, AbortTransaction)
    {
        ValidatePeer(EPeerKind::Leader);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        bool force = request->force();

        context->SetRequestInfo("TransactionId: %v, Force: %v",
            transactionId,
            force);

        if (ResponseKeeper_->TryReplyFrom(context))
            return;

        auto asyncResponseMessage = DoAbortTransaction(
            transactionId,
            GetMutationId(context),
            force);
        context->ReplyFrom(asyncResponseMessage);
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, PingTransaction)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        bool pingAncestors = request->ping_ancestors();

        context->SetRequestInfo("TransactionId: %v, PingAncestors: %v",
            transactionId,
            pingAncestors);

        // Any exception thrown here is replied to the client.
        TransactionManager_->PingTransaction(transactionId, pingAncestors);

        context->Reply();
    }


    // Facade implementation.

    TFuture<TSharedRefArray> DoCommitTransaction(
        const TTransactionId& transactionId,
        const std::vector<TCellId>& participantCellIds,
        const TMutationId& mutationId)
    {
        YASSERT(!HasMutationContext());

        auto* commit = FindCommit(transactionId);
        if (commit) {
            // NB: Even Response Keeper cannot protect us from this.
            return commit->GetAsyncResponseMessage();
        }

        auto commitHolder = std::make_unique<TCommit>(
            transactionId,
            mutationId,
            participantCellIds);
        commit = TransientCommitMap_.Insert(transactionId, std::move(commitHolder));

        // Commit instance may die below.
        auto asyncResponseMessage = commit->GetAsyncResponseMessage();

        if (participantCellIds.empty()) {
            DoCommitSimpleTransaction(commit);
        } else {
            DoCommitDistributedTransaction(commit);
        }

        return asyncResponseMessage;
    }

    void DoCommitSimpleTransaction(TCommit* commit)
    {
        YCHECK(!commit->GetPersistent());

        auto prepareTimestamp = TimestampProvider_->GetLatestTimestamp();
        const auto& transactionId = commit->GetTransactionId();

        try {
            // Any exception thrown here is replied to the client.
            TransactionManager_->PrepareTransactionCommit(
                transactionId,
                false,
                prepareTimestamp);
        } catch (const std::exception& ex) {
            LOG_DEBUG(ex, "Error preparing simple transaction commit (TransactionId: %v)",
                transactionId);
            SetCommitFailed(commit, ex);
            TransientCommitMap_.Remove(transactionId);
            // Best effort, fire-and-forget.
            AbortTransaction(transactionId, true);
            return;
        }

        GenerateCommitTimestamp(commit);
    }

    void DoCommitDistributedTransaction(TCommit* commit)
    {
        YCHECK(!commit->GetPersistent());

        auto prepareTimestamp = TimestampProvider_->GetLatestTimestamp();
        const auto& transactionId = commit->GetTransactionId();

        try {
            // Any exception thrown here is replied to the client.
            TransactionManager_->PrepareTransactionCommit(
                transactionId,
                false,
                prepareTimestamp);
        } catch (const std::exception& ex) {
            LOG_DEBUG(ex, "Error preparing distributed transaction commit at coordinator (TransactionId: %v)",
                transactionId);
            SetCommitFailed(commit, ex);
            TransientCommitMap_.Remove(transactionId);
            // Best effort, fire-and-forget.
            AbortTransaction(transactionId, true);
            return;
        }

        // Distributed commit.
        TReqCommitDistributedTransactionPhaseOne hydraRequest;
        ToProto(hydraRequest.mutable_transaction_id(), commit->GetTransactionId());
        ToProto(hydraRequest.mutable_mutation_id(), commit->GetMutationId());
        ToProto(hydraRequest.mutable_participant_cell_ids(), commit->ParticipantCellIds());
        hydraRequest.set_prepare_timestamp(prepareTimestamp);
        CreateMutation(
            HydraManager_,
            hydraRequest,
            &TImpl::HydraCommitDistributedTransactionPhaseOne,
            this)
            ->CommitAndLog(Logger);
    }

    TFuture<TSharedRefArray> DoAbortTransaction(
        const TTransactionId& transactionId,
        const TMutationId& mutationId,
        bool force)
    {
        YASSERT(!HasMutationContext());

        try {
            // Any exception thrown here is caught below..
            TransactionManager_->PrepareTransactionAbort(transactionId, force);
        } catch (const std::exception& ex) {
            LOG_DEBUG(ex, "Error preparing transaction abort (TransactionId: %v, Force: %v)",
                transactionId,
                force);
            auto responseMessage = CreateErrorResponseMessage(ex);
            if (mutationId) {
                ResponseKeeper_->EndRequest(mutationId, responseMessage);
            }
            return MakeFuture(responseMessage);
        }

        TReqHydraAbortTransaction hydraRequest;
        ToProto(hydraRequest.mutable_transaction_id(), transactionId);
        ToProto(hydraRequest.mutable_mutation_id(), mutationId);
        hydraRequest.set_force(force);
        return
            CreateMutation(
                HydraManager_,
                hydraRequest,
                &TImpl::HydraAbortTransaction,
                this)
            ->Commit()
            .Apply(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TMutationResponse>& result) -> TSharedRefArray {
                if (result.IsOK()) {
                    return result.Value().Data;
                } else {
                    LOG_WARNING(result, "Error committing transaction abort mutation");
                    return CreateErrorResponseMessage(result);
                }
            }));
    }

    static TFuture<void> MessageToError(TFuture<TSharedRefArray> asyncMessage)
    {
        return asyncMessage.Apply(BIND([] (const TSharedRefArray& message) -> TFuture<void> {
            TResponseHeader header;
            YCHECK(ParseResponseHeader(message, &header));
            return header.has_error()
                ? MakeFuture<void>(FromProto<TError>(header.error()))
                : VoidFuture;
        }));
    }

    // Hydra handlers.

    void HydraCommitSimpleTransaction(TReqCommitSimpleTransaction* request)
    {
        auto mutationId = FromProto<TMutationId>(request->mutation_id());
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto commitTimestamp = TTimestamp(request->commit_timestamp());

        auto* commit = FindCommit(transactionId);
            
        try {
            // Any exception thrown here is caught below.
            TransactionManager_->CommitTransaction(transactionId, commitTimestamp);
        } catch (const std::exception& ex) {
            if (commit) {
                YCHECK(!commit->GetPersistent());
                SetCommitSucceeded(commit, commitTimestamp);
                TransientCommitMap_.Remove(transactionId);
            }
            LOG_DEBUG_UNLESS(IsRecovery(), ex, "Error committing simple transaction (TransactionId: %v)",
                transactionId);
            return;
        }

        if (!commit) {
            // Commit could be missing (e.g. at followers or during recovery).
            // Let's recreate it since it's needed below in SetCommitSucceeded.
            auto commitHolder = std::make_unique<TCommit>(
                transactionId,
                mutationId,
                std::vector<TCellId>());
            commit = TransientCommitMap_.Insert(transactionId, std::move(commitHolder));
        }

        YCHECK(!commit->GetPersistent());
        SetCommitSucceeded(commit, commitTimestamp);
        TransientCommitMap_.Remove(transactionId);
    }

    void HydraCommitDistributedTransactionPhaseOne(TReqCommitDistributedTransactionPhaseOne* request)
    {
        auto mutationId = FromProto<TMutationId>(request->mutation_id());
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto participantCellIds = FromProto<std::vector<TCellId>>(request->participant_cell_ids());
        auto prepareTimestamp = TTimestamp(request->prepare_timestamp());

        // Ensure commit existence (possibly moving it from transient to persistent).
        std::unique_ptr<TCommit> commitHolder;
        auto* commit = FindCommit(transactionId);
        if (commit) {
            YCHECK(!commit->GetPersistent());
            commitHolder = TransientCommitMap_.Release(transactionId);
        } else {
            commitHolder = std::make_unique<TCommit>(
                transactionId,
                mutationId,
                participantCellIds);
            commit = commitHolder.get();
        }
        commit->SetPersistent(true);
        PersistentCommitMap_.Insert(transactionId, std::move(commitHolder));

        const auto& coordinatorCellId = HiveManager_->GetSelfCellId();

        LOG_DEBUG_UNLESS(IsRecovery(),
            "Distributed commit phase one started "
            "(TransactionId: %v, ParticipantCellIds: %v, PrepareTimestamp: %v, CoordinatorCellId: %v)",
            transactionId,
            participantCellIds,
            prepareTimestamp,
            coordinatorCellId);

        // Prepare at coordinator.
        try {
            // Any exception thrown here is caught below.
            TransactionManager_->PrepareTransactionCommit(
                transactionId,
                true,
                prepareTimestamp);
        } catch (const std::exception& ex) {
            LOG_DEBUG_UNLESS(IsRecovery(), ex, "Error preparing distributed transaction commit at coordinator (TransactionId: %v)",
                transactionId);
            SetCommitFailed(commit, ex);
            PersistentCommitMap_.Remove(transactionId);
            if (IsLeader()) {
                // Best effort, fire-and-forget.
                EpochAutomatonInvoker_->Invoke(BIND([=, this_ = MakeStrong(this)] () {
                    AbortTransaction(transactionId, true);
                }));
            }
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Coordinator has prepared transaction (TransactionId: %v)",
            transactionId);

        // Prepare at participants.
        {
            TReqPrepareTransactionCommit hydraRequest;
            ToProto(hydraRequest.mutable_transaction_id(), transactionId);
            hydraRequest.set_prepare_timestamp(prepareTimestamp);
            ToProto(hydraRequest.mutable_coordinator_cell_id(), coordinatorCellId);
            PostToParticipants(commit, hydraRequest);
        }
    }

    void HydraPrepareTransactionCommit(TReqPrepareTransactionCommit* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto prepareTimestamp = TTimestamp(request->prepare_timestamp());
        auto coordinatorCellId = FromProto<TCellId>(request->coordinator_cell_id());

        YCHECK(!FindCommit(transactionId));

        TError error; // initially OK
        try {
            // Any exception thrown here is caught below and replied to the coordinator.
            TransactionManager_->PrepareTransactionCommit(
                transactionId,
                true,
                prepareTimestamp);

            LOG_DEBUG_UNLESS(IsRecovery(), "Participant has prepared transaction (TransactionId: %v, CoordinatorCellId: %v)",
                transactionId,
                coordinatorCellId);
        } catch (const std::exception& ex) {
            LOG_DEBUG_UNLESS(IsRecovery(), ex, "Error preparing transaction commit at participant (TransactionId: %v)",
                transactionId);
            error = ex;
        }

        {
            TReqOnTransactionCommitPrepared hydraResponse;
            ToProto(hydraResponse.mutable_transaction_id(), transactionId);
            ToProto(hydraResponse.mutable_participant_cell_id(), HiveManager_->GetSelfCellId());
            ToProto(hydraResponse.mutable_error(), error.Sanitize());
            PostToCoordinator(coordinatorCellId, hydraResponse);
        }
    }

    void HydraOnTransactionCommitPrepared(TReqOnTransactionCommitPrepared* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto participantCellId = FromProto<TCellId>(request->participant_cell_id());

        auto* commit = FindCommit(transactionId);
        if (!commit) {
            LOG_DEBUG_UNLESS(IsRecovery(),
                "Invalid or expired transaction has been prepared, ignoring "
                "(TransactionId: %v, ParticipantCellId: %v)",
                transactionId,
                participantCellId);
            return;
        }

        YCHECK(commit->GetPersistent());

        auto error = FromProto<TError>(request->error());
        if (!error.IsOK()) {
            LOG_DEBUG_UNLESS(IsRecovery(), error, "Participant response: transaction has failed to prepare (TransactionId: %v, ParticipantCellId: %v)",
                transactionId,
                participantCellId);

            SetCommitFailed(commit, error);

            // Transaction is already prepared at coordinator and (possibly) at some participants.
            // We _must_ forcefully abort it.
            try {
                TransactionManager_->AbortTransaction(transactionId, true);
            } catch (const std::exception& ex) {
                LOG_DEBUG_UNLESS(IsRecovery(), ex, "Error aborting transaction at coordinator, ignored (TransactionId: %v)",
                    transactionId);
            }

            TReqHydraAbortTransaction hydraRequest;
            ToProto(hydraRequest.mutable_transaction_id(), transactionId);
            ToProto(hydraRequest.mutable_mutation_id(), NullMutationId);
            hydraRequest.set_force(true);
            PostToParticipants(commit, hydraRequest);

            PersistentCommitMap_.Remove(transactionId);
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Participant response: transaction prepared (TransactionId: %v, ParticipantCellId: %v)",
            transactionId,
            participantCellId);

        YCHECK(commit->PreparedParticipantCellIds().insert(participantCellId).second);

        if (IsLeader()) {
            CheckForPhaseTwo(commit);
        }
    }

    void HydraCommitDistributedTransactionPhaseTwo(TReqCommitDistributedTransactionPhaseTwo* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto commitTimestamp = TTimestamp(request->commit_timestamp());

        auto* commit = FindCommit(transactionId);
        if (!commit) {
            LOG_ERROR_UNLESS(IsRecovery(), "Requested to start phase two for an unknown transaction, ignored (TransactionId: %v)",
                transactionId);
            return;
        }

        YCHECK(commit->IsDistributed());
        YCHECK(commit->GetPersistent());

        try {
            // Any exception thrown here is caught below.
            TransactionManager_->CommitTransaction(transactionId, commitTimestamp);
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error committing transaction at coordinator (TransactionId: %v)",
                transactionId);
            SetCommitFailed(commit, ex);
            PersistentCommitMap_.Remove(transactionId);
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Coordinator has committed transaction (TransactionId: %v)",
            transactionId);

        // Commit at participants.
        {
            TReqCommitPreparedTransaction hydraRequest;
            ToProto(hydraRequest.mutable_transaction_id(), transactionId);
            hydraRequest.set_commit_timestamp(commitTimestamp);
            PostToParticipants(commit, hydraRequest);
        }

        SetCommitSucceeded(commit, commitTimestamp);
        PersistentCommitMap_.Remove(transactionId);
    }

    void HydraCommitPreparedTransaction(TReqCommitPreparedTransaction* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto commitTimestamp = TTimestamp(request->commit_timestamp());

        try {
            // Any exception thrown here is caught below.
            TransactionManager_->CommitTransaction(transactionId, commitTimestamp);
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error committing transaction at participant (TransactionId: %v)",
                transactionId);
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Participant has committed transaction (TransactionId: %v)",
            transactionId);
    }

    void HydraAbortTransaction(TReqHydraAbortTransaction* request)
    {
        auto mutationId = FromProto<TMutationId>(request->mutation_id());
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto force = request->force();

        try {
            // All exceptions thrown here are caught below.
            TransactionManager_->AbortTransaction(transactionId, force);
        } catch (const std::exception& ex) {
            LOG_DEBUG_UNLESS(IsRecovery(), ex, "Error aborting transaction, ignored (TransactionId: %v)",
                transactionId);
        }

        auto* commit = FindCommit(transactionId);
        if (commit) {
            if (commit->GetPersistent()) {
                TReqHydraAbortTransaction hydraRequest;
                ToProto(hydraRequest.mutable_transaction_id(), transactionId);
                ToProto(hydraRequest.mutable_mutation_id(), NullMutationId);
                hydraRequest.set_force(true);
                PostToParticipants(commit, hydraRequest);
            }
            
            auto error = TError("Transaction %v was aborted", transactionId);
            SetCommitFailed(commit, error);

            RemoveCommit(transactionId);
        }

        {
            TRspAbortTransaction response;
            auto responseMessage = CreateResponseMessage(response);

            auto* mutationContext = GetCurrentMutationContext();
            mutationContext->Response().Data = responseMessage;

            if (mutationId) {
                ResponseKeeper_->EndRequest(mutationId, responseMessage);
            }
        }
    }


    // Accessors for the combined collection.
    
    TCommit* FindCommit(const TTransactionId& transactionId)
    {
        if (auto* commit = TransientCommitMap_.Find(transactionId)) {
            return commit;
        }
        if (auto* commit = PersistentCommitMap_.Find(transactionId)) {
            return commit;
        }
        return nullptr;
    }

    void RemoveCommit(const TTransactionId& transactionId)
    {
        YCHECK(TransientCommitMap_.TryRemove(transactionId) || PersistentCommitMap_.TryRemove(transactionId));
    }


    void SetCommitFailed(TCommit* commit, const TError& error)
    {
        LOG_DEBUG_UNLESS(IsRecovery(), error, "Transaction commit failed (TransactionId: %v)",
            commit->GetTransactionId());

        auto responseMessage = CreateErrorResponseMessage(error);
        SetCommitResponse(commit, responseMessage);
    }

    void SetCommitSucceeded(TCommit* commit, TTimestamp commitTimestamp)
    {
        LOG_DEBUG_UNLESS(IsRecovery(), "Transaction commit succeeded (TransactionId: %v, CommitTimestamp: %v)",
            commit->GetTransactionId(),
            commitTimestamp);

        TRspCommitTransaction response;
        response.set_commit_timestamp(commitTimestamp);

        auto responseMessage = CreateResponseMessage(response);
        SetCommitResponse(commit, responseMessage);
    }

    void SetCommitResponse(TCommit* commit, TSharedRefArray responseMessage)
    {
        const auto& mutationId = commit->GetMutationId();
        if (mutationId) {
            ResponseKeeper_->EndRequest(mutationId, responseMessage);
        }

        commit->SetResponseMessage(std::move(responseMessage));
    }


    template <class TMessage>
    void PostToParticipants(TCommit* commit, const TMessage& message)
    {
        for (const auto& cellId : commit->ParticipantCellIds()) {
            auto* mailbox = HiveManager_->GetOrCreateMailbox(cellId);
            HiveManager_->PostMessage(mailbox, message);
        }
    }

    template <class TMessage>
    void PostToCoordinator(const TCellId& coordinatorCellId, const TMessage& message)
    {
        auto* mailbox = HiveManager_->GetOrCreateMailbox(coordinatorCellId);
        HiveManager_->PostMessage(mailbox, message);
    }


    void GenerateCommitTimestamp(TCommit* commit)
    {
        LOG_DEBUG("Generating commit timestamp (TransactionId: %v)",
            commit->GetTransactionId());

        TimestampProvider_->GenerateTimestamps()
            .Subscribe(BIND(&TImpl::OnCommitTimestampGenerated, MakeStrong(this), commit->GetTransactionId())
                .Via(EpochAutomatonInvoker_));
    }

    void OnCommitTimestampGenerated(const TTransactionId& transactionId, TErrorOr<TTimestamp> timestampOrError)
    {
        auto* commit = FindCommit(transactionId);
        if (!commit) {
            LOG_DEBUG("Commit timestamp generated for an invalid or expired transaction, ignored (TransactionId: %v)",
                transactionId);
            return;
        }

        if (!timestampOrError.IsOK()) {
            // If this is a distributed transaction then it's already prepared at coordinator and
            // at all participants. We _must_ forcefully abort it.
            LOG_DEBUG(timestampOrError, "Error generating commit timestamp (TransactionId: %v)",
                transactionId);
            AbortTransaction(transactionId, true);
            return;
        }

        auto timestamp = timestampOrError.Value();
        if (commit->IsDistributed()) {
            TReqCommitDistributedTransactionPhaseTwo hydraRequest;
            ToProto(hydraRequest.mutable_transaction_id(), transactionId);
            hydraRequest.set_commit_timestamp(timestamp);
            CreateMutation(
                HydraManager_,
                hydraRequest,
                &TImpl::HydraCommitDistributedTransactionPhaseTwo,
                this)
                ->CommitAndLog(Logger);
        } else {
            TReqCommitSimpleTransaction hydraRequest;
            ToProto(hydraRequest.mutable_transaction_id(), transactionId);
            ToProto(hydraRequest.mutable_mutation_id(), commit->GetMutationId());
            hydraRequest.set_commit_timestamp(timestamp);
            CreateMutation(
                HydraManager_,
                hydraRequest,
                &TImpl::HydraCommitSimpleTransaction,
                this)
                ->CommitAndLog(Logger);
        }
    }


    void CheckForPhaseTwo(TCommit* commit)
    {
        if (!commit->IsDistributed())
            return;

        if (commit->PreparedParticipantCellIds().size() != commit->ParticipantCellIds().size())
            // Some participants are not prepared yet.
            return;

        const auto& transactionId = commit->GetTransactionId();

        LOG_DEBUG_UNLESS(IsRecovery(), "Distributed commit phase two started (TransactionId: %v)",
            transactionId);

        GenerateCommitTimestamp(commit);
    }

    
    virtual bool ValidateSnapshotVersion(int version) override
    {
        return version == 1;
    }

    virtual int GetCurrentSnapshotVersion() override
    {
        return 1;
    }


    virtual void OnLeaderActive() override
    {
        TCompositeAutomatonPart::OnLeaderActive();

        for (const auto& pair : PersistentCommitMap_) {
            auto* commit = pair.second;
            CheckForPhaseTwo(commit);
        }
    }

    virtual void OnStopLeading() override
    {
        TCompositeAutomatonPart::OnStopLeading();

        TransientCommitMap_.Clear();
    }


    virtual void Clear() override
    {
        TCompositeAutomatonPart::Clear();

        PersistentCommitMap_.Clear();
        TransientCommitMap_.Clear();
    }

    void SaveKeys(TSaveContext& context) const
    {
        PersistentCommitMap_.SaveKeys(context);
    }

    void SaveValues(TSaveContext& context) const
    {
        PersistentCommitMap_.SaveValues(context);
    }

    void LoadKeys(TLoadContext& context)
    {
        PersistentCommitMap_.LoadKeys(context);
    }

    void LoadValues(TLoadContext& context)
    {
        PersistentCommitMap_.LoadValues(context);
    }


    // THydraServiceBase overrides.
    virtual IHydraManagerPtr GetHydraManager() override
    {
        return HydraManager_;
    }

};

////////////////////////////////////////////////////////////////////////////////

TTransactionSupervisor::TTransactionSupervisor(
    TTransactionSupervisorConfigPtr config,
    IInvokerPtr automatonInvoker,
    IInvokerPtr trackerInvoker,
    IHydraManagerPtr hydraManager,
    TCompositeAutomatonPtr automaton,
    TResponseKeeperPtr responseKeeper,
    THiveManagerPtr hiveManager,
    ITransactionManagerPtr transactionManager,
    ITimestampProviderPtr timestampProvider)
    : Impl_(New<TImpl>(
        config,
        automatonInvoker,
        trackerInvoker,
        hydraManager,
        automaton,
        responseKeeper,
        hiveManager,
        transactionManager,
        timestampProvider))
{ }

TTransactionSupervisor::~TTransactionSupervisor()
{ }

IServicePtr TTransactionSupervisor::GetRpcService()
{
    return Impl_->GetRpcService();
}

TFuture<void> TTransactionSupervisor::CommitTransaction(
    const TTransactionId& transactionId,
    const std::vector<TCellId>& participantCellIds)
{
    return Impl_->CommitTransaction(
        transactionId,
        participantCellIds);
}

TFuture<void> TTransactionSupervisor::AbortTransaction(
    const TTransactionId& transactionId,
    bool force)
{
    return Impl_->AbortTransaction(
        transactionId,
        force);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHive
} // namespace NYT
