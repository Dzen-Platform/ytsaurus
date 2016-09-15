#include "transaction_supervisor.h"
#include "commit.h"
#include "config.h"
#include "transaction_manager.h"
#include "transaction_participant_provider.h"
#include "private.h"

#include <yt/server/hive/transaction_supervisor.pb.h>

#include <yt/server/hydra/composite_automaton.h>
#include <yt/server/hydra/entity_map.h>
#include <yt/server/hydra/hydra_manager.h>
#include <yt/server/hydra/hydra_service.h>
#include <yt/server/hydra/mutation.h>

#include <yt/ytlib/hive/transaction_supervisor_service_proxy.h>
#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/transaction_participant.h>
#include <yt/ytlib/hive/transaction_participant_service_proxy.h>

#include <yt/ytlib/transaction_client/timestamp_provider.h>
#include <yt/ytlib/transaction_client/action.h>

#include <yt/ytlib/api/connection.h>

#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/rpc/message.h>
#include <yt/core/rpc/response_keeper.h>
#include <yt/core/rpc/rpc.pb.h>
#include <yt/core/rpc/server.h>
#include <yt/core/rpc/service_detail.h>

#include <yt/core/ytree/helpers.h>

namespace NYT {
namespace NHiveServer {

using namespace NRpc;
using namespace NRpc::NProto;
using namespace NHydra;
using namespace NHiveClient;
using namespace NTransactionClient;
using namespace NApi;
using namespace NYTree;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto ParticipantCleanupPeriod = TDuration::Seconds(15);

////////////////////////////////////////////////////////////////////////////////

class TTransactionSupervisor::TImpl
    : public TCompositeAutomatonPart
{
public:
    TImpl(
        TTransactionSupervisorConfigPtr config,
        IInvokerPtr automatonInvoker,
        IInvokerPtr trackerInvoker,
        IHydraManagerPtr hydraManager,
        TCompositeAutomatonPtr automaton,
        TResponseKeeperPtr responseKeeper,
        ITransactionManagerPtr transactionManager,
        const TCellId& selfCellId,
        ITimestampProviderPtr timestampProvider)
        : TCompositeAutomatonPart(
            hydraManager,
            automaton,
            automatonInvoker)
        , Config_(config)
        , TrackerInvoker_(trackerInvoker)
        , HydraManager_(hydraManager)
        , ResponseKeeper_(responseKeeper)
        , TransactionManager_(transactionManager)
        , SelfCellId_(selfCellId)
        , TimestampProvider_(timestampProvider)
        , Logger(NLogging::TLogger(HiveServerLogger)
            .AddTag("CellId: %v", SelfCellId_))
        , TransactionSupervisorService_(New<TTransactionSupervisorService>(this))
        , TransactionParticipantService_(New<TTransactionParticipantService>(this))
    {
        YCHECK(Config_);
        YCHECK(TrackerInvoker_);
        YCHECK(ResponseKeeper_);
        YCHECK(TransactionManager_);
        YCHECK(TimestampProvider_);

        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCoordinatorCommitSimpleTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCoordinatorCommitDistributedTransactionPhaseOne, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCoordinatorCommitDistributedTransactionPhaseTwo, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCoorindatorAbortTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraCoordinatorFinishDistributedTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraParticipantPrepareTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraParticipantCommitTransaction, Unretained(this)));
        TCompositeAutomatonPart::RegisterMethod(BIND(&TImpl::HydraParticipantAbortTransaction, Unretained(this)));

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

    std::vector<IServicePtr> GetRpcServices()
    {
        return std::vector<IServicePtr>{
            TransactionSupervisorService_,
            TransactionParticipantService_
        };
    }

    void RegisterParticipantProvider(ITransactionParticipantProviderPtr provider)
    {
        ParticipantProviders_.push_back(provider);
    }

    TFuture<void> CommitTransaction(
        const TTransactionId& transactionId,
        const std::vector<TCellId>& participantCellIds)
    {
        return MessageToError(
            CoordinatorCommitTransaction(
                transactionId,
                participantCellIds,
                false,
                NullMutationId));
    }

    TFuture<void> AbortTransaction(
        const TTransactionId& transactionId,
        bool force)
    {
        return MessageToError(
            CoordinatorAbortTransaction(
                transactionId,
                NullMutationId,
                force));
    }

private:
    const TTransactionSupervisorConfigPtr Config_;
    const IInvokerPtr TrackerInvoker_;
    const IHydraManagerPtr HydraManager_;
    const TResponseKeeperPtr ResponseKeeper_;
    const ITransactionManagerPtr TransactionManager_;
    const TCellId SelfCellId_;
    const ITimestampProviderPtr TimestampProvider_;

    const NLogging::TLogger Logger;

    TEntityMap<TCommit> TransientCommitMap_;
    TEntityMap<TCommit> PersistentCommitMap_;

    class TWrappedParticipant
        : public TRefCounted
    {
    public:
        TWrappedParticipant(
            const TCellId& cellId,
            TTransactionSupervisorConfigPtr config,
            const std::vector<ITransactionParticipantProviderPtr>& providers,
            const NLogging::TLogger logger)
            : CellId_(cellId)
            , Config_(std::move(config))
            , Providers_(providers)
            , ProbationExecutor_(New<TPeriodicExecutor>(
                NRpc::TDispatcher::Get()->GetLightInvoker(),
                BIND(&TWrappedParticipant::OnProbation, MakeWeak(this)),
                Config_->ParticipantProbationPeriod))
            , Logger(NLogging::TLogger(logger)
                .AddTag("ParticipantCellId: %v", CellId_))
        {
            ProbationExecutor_->Start();
        }

        const TCellId& GetCellId() const
        {
            return CellId_;
        }

        bool IsValid() const
        {
            auto guard = Guard(SpinLock_);
            return !Underlying_ || Underlying_->IsValid();
        }

        TFuture<void> PrepareTransaction(const TTransactionId& transactionId)
        {
            return EnqueueRequest(
                &ITransactionParticipant::PrepareTransaction,
                transactionId);
        }

        TFuture<void> CommitTransaction(const TTransactionId& transactionId, TTimestamp commitTimestamp)
        {
            return EnqueueRequest(
                &ITransactionParticipant::CommitTransaction,
                transactionId,
                commitTimestamp);
        }

        TFuture<void> AbortTransaction(const TTransactionId& transactionId)
        {
            return EnqueueRequest(
                &ITransactionParticipant::AbortTransaction,
                transactionId);
        }

        void SetUp()
        {
            auto guard = Guard(SpinLock_);

            if (Up_) {
                return;
            }

            decltype(PendingSenders_) senders;
            PendingSenders_.swap(senders);
            Up_ = true;

            guard.Release();

            LOG_DEBUG("Participant cell is up");

            for (const auto& sender : PendingSenders_) {
                sender.Run();
            }
        }

        void SetDown(const TError& error)
        {
            auto guard = Guard(SpinLock_);

            if (!Up_) {
                return;
            }

            Up_ = false;

            LOG_DEBUG(error, "Participant cell is down");
        }

    private:
        const TCellId CellId_;
        const TTransactionSupervisorConfigPtr Config_;
        const std::vector<ITransactionParticipantProviderPtr> Providers_;
        const TPeriodicExecutorPtr ProbationExecutor_;
        const NLogging::TLogger Logger;

        TSpinLock SpinLock_;
        ITransactionParticipantPtr Underlying_;
        std::vector<TClosure> PendingSenders_;
        bool Up_ = true;


        ITransactionParticipantPtr TryCreateUnderlying()
        {
            TTransactionParticipantOptions options;
            options.RpcTimeout = Config_->RpcTimeout;

            for (const auto& provider : Providers_) {
                auto participant = provider->TryCreate(CellId_, options);
                if (participant) {
                    return participant;
                }
            }
            return nullptr;
        }

        template <class TMethod, class... TArgs>
        TFuture<void> EnqueueRequest(TMethod method, TArgs... args)
        {
            auto promise = NewPromise<void>();

            auto guard = Guard(SpinLock_);

            if (!Underlying_) {
                Underlying_ = TryCreateUnderlying();
                if (!Underlying_) {
                    return MakeFuture(TError(
                        NRpc::EErrorCode::Unavailable,
                        "No connection info is available for participant cell %v",
                        CellId_));
                }
            }

            auto sender = BIND([=, underlying = Underlying_] () mutable {
                promise.SetFrom((underlying.Get()->*method)(args...));
            });
            if (!TrySendRequestImmediately(sender, &guard)) {
                PendingSenders_.emplace_back(std::move(sender));
            }

            return promise;
        }

        bool TrySendRequestImmediately(const TClosure& sender, TGuard<TSpinLock>* guard)
        {
            if (!Up_) {
                return false;
            }

            guard->Release();
            sender.Run();
            return true;
        }

        void OnProbation()
        {
            auto guard = Guard(SpinLock_);

            if (Up_ || PendingSenders_.empty()) {
                return;
            }

            auto sender = PendingSenders_.back();
            PendingSenders_.pop_back();

            guard.Release();

            sender.Run();
        }
    };

    using TWrappedParticipantPtr = TIntrusivePtr<TWrappedParticipant>;
    using TWrappedParticipantWeakPtr = TWeakPtr<TWrappedParticipant>;

    std::vector<ITransactionParticipantProviderPtr> ParticipantProviders_;
    yhash_map<TCellId, TWrappedParticipantPtr> StrongParticipantMap_;
    yhash_map<TCellId, TWrappedParticipantWeakPtr> WeakParticipantMap_;
    TPeriodicExecutorPtr ParticipantCleanupExecutor_;



    class TOwnedServiceBase
        : public THydraServiceBase
    {
    protected:
        explicit TOwnedServiceBase(
            TImplPtr owner,
            const Stroka& serviceName,
            int protocolVersion)
            : THydraServiceBase(
                owner->HydraManager_->CreateGuardedAutomatonInvoker(owner->AutomatonInvoker_),
                TServiceId(serviceName, owner->SelfCellId_),
                HiveServerLogger,
                protocolVersion)
            , Owner_(owner)
            , HydraManager_(owner->HydraManager_)
        { }

        TImplPtr GetOwnerOrThrow()
        {
            auto owner = Owner_.Lock();
            if (!owner) {
                THROW_ERROR_EXCEPTION(NRpc::EErrorCode::Unavailable, "Service is shutting down");
            }
            return owner;
        }

    private:
        const TWeakPtr<TImpl> Owner_;
        const IHydraManagerPtr HydraManager_;

        virtual IHydraManagerPtr GetHydraManager() override
        {
            return HydraManager_;
        }
    };


    class TTransactionSupervisorService
        : public TOwnedServiceBase
    {
    public:
        explicit TTransactionSupervisorService(TImplPtr owner)
            : TOwnedServiceBase(
                owner,
                TTransactionSupervisorServiceProxy::GetServiceName(),
                TTransactionSupervisorServiceProxy::GetProtocolVersion())
        {
            TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(CommitTransaction));
            TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(AbortTransaction));
            TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(PingTransaction)
                .SetInvoker(owner->TrackerInvoker_));
        }

    private:
        DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto::NTransactionSupervisor, CommitTransaction)
        {
            ValidatePeer(EPeerKind::Leader);

            auto transactionId = FromProto<TTransactionId>(request->transaction_id());
            auto participantCellIds = FromProto<std::vector<TCellId>>(request->participant_cell_ids());
            auto force2PC = request->force_2pc();

            context->SetRequestInfo("TransactionId: %v, ParticipantCellIds: %v, Force2PC: %v",
                transactionId,
                participantCellIds,
                force2PC);

            auto owner = GetOwnerOrThrow();

            if (owner->ResponseKeeper_->TryReplyFrom(context)) {
                return;
            }

            auto asyncResponseMessage = owner->CoordinatorCommitTransaction(
                transactionId,
                participantCellIds,
                force2PC,
                GetMutationId(context));
            context->ReplyFrom(asyncResponseMessage);
        }

        DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto::NTransactionSupervisor, AbortTransaction)
        {
            ValidatePeer(EPeerKind::Leader);

            auto transactionId = FromProto<TTransactionId>(request->transaction_id());
            bool force = request->force();

            context->SetRequestInfo("TransactionId: %v, Force: %v",
                transactionId,
                force);

            auto owner = GetOwnerOrThrow();

            if (owner->ResponseKeeper_->TryReplyFrom(context)) {
                return;
            }

            auto asyncResponseMessage = owner->CoordinatorAbortTransaction(
                transactionId,
                GetMutationId(context),
                force);
            context->ReplyFrom(asyncResponseMessage);
        }

        DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto::NTransactionSupervisor, PingTransaction)
        {
            auto transactionId = FromProto<TTransactionId>(request->transaction_id());
            bool pingAncestors = request->ping_ancestors();

            context->SetRequestInfo("TransactionId: %v, PingAncestors: %v",
                transactionId,
                pingAncestors);

            auto owner = GetOwnerOrThrow();

            // Any exception thrown here is replied to the client.
            owner->TransactionManager_->PingTransaction(transactionId, pingAncestors);

            context->Reply();
        }
    };

    const TIntrusivePtr<TTransactionSupervisorService> TransactionSupervisorService_;


    class TTransactionParticipantService
        : public TOwnedServiceBase
    {
    public:
        explicit TTransactionParticipantService(TImplPtr owner)
            : TOwnedServiceBase(
                owner,
                TTransactionParticipantServiceProxy::GetServiceName(),
                TTransactionParticipantServiceProxy::GetProtocolVersion())
        {
            TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(PrepareTransaction));
            TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(CommitTransaction));
            TServiceBase::RegisterMethod(RPC_SERVICE_METHOD_DESC(AbortTransaction));
        }

    private:
        DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto::NTransactionParticipant, PrepareTransaction)
        {
            ValidatePeer(EPeerKind::Leader);

            auto transactionId = FromProto<TTransactionId>(request->transaction_id());

            context->SetRequestInfo("TransactionId: %v",
                transactionId);

            auto owner = GetOwnerOrThrow();

            CreateMutation(owner->HydraManager_, context)
                ->CommitAndReply(context);
        }

        DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto::NTransactionParticipant, CommitTransaction)
        {
            ValidatePeer(EPeerKind::Leader);

            auto transactionId = FromProto<TTransactionId>(request->transaction_id());
            auto commitTimestamp = request->commit_timestamp();

            context->SetRequestInfo("TransactionId: %v, CommitTimestamp: %v",
                transactionId,
                commitTimestamp);

            auto owner = GetOwnerOrThrow();

            CreateMutation(owner->HydraManager_, context)
                ->CommitAndReply(context);
        }

        DECLARE_RPC_SERVICE_METHOD(NHiveClient::NProto::NTransactionParticipant, AbortTransaction)
        {
            ValidatePeer(EPeerKind::Leader);

            auto transactionId = FromProto<TTransactionId>(request->transaction_id());

            context->SetRequestInfo("TransactionId: %v",
                transactionId);

            auto owner = GetOwnerOrThrow();

            CreateMutation(owner->HydraManager_, context)
                ->CommitAndReply(context);
        }
    };

    const TIntrusivePtr<TTransactionParticipantService> TransactionParticipantService_;


    // Coordinator implementation.

    TFuture<TSharedRefArray> CoordinatorCommitTransaction(
        const TTransactionId& transactionId,
        const std::vector<TCellId>& participantCellIds,
        bool force2PC,
        const TMutationId& mutationId)
    {
        Y_ASSERT(!HasMutationContext());

        auto* commit = FindCommit(transactionId);
        if (commit) {
            // NB: Even Response Keeper cannot protect us from this.
            return commit->GetAsyncResponseMessage();
        }

        auto commitHolder = std::make_unique<TCommit>(
            transactionId,
            mutationId,
            participantCellIds,
            force2PC || !participantCellIds.empty());
        commit = TransientCommitMap_.Insert(transactionId, std::move(commitHolder));

        // Commit instance may die below.
        auto asyncResponseMessage = commit->GetAsyncResponseMessage();

        if (commit->GetDistributed()) {
            CommitDistributedTransaction(commit);
        } else {
            CommitSimpleTransaction(commit);
        }

        return asyncResponseMessage;
    }

    void CommitSimpleTransaction(TCommit* commit)
    {
        YCHECK(!commit->GetPersistent());

        const auto& transactionId = commit->GetTransactionId();

        try {
            // Any exception thrown here is replied to the client.
            TransactionManager_->PrepareTransactionCommit(transactionId, false);
        } catch (const std::exception& ex) {
            LOG_DEBUG(ex, "Error preparing simple transaction commit (TransactionId: %v)",
                transactionId);
            SetCommitFailed(commit, ex);
            RemoveTransientCommit(commit);
            // Best effort, fire-and-forget.
            AbortTransaction(transactionId, true);
            return;
        }

        GenerateCommitTimestamp(commit);
    }

    void CommitDistributedTransaction(TCommit* commit)
    {
        YCHECK(!commit->GetPersistent());

        NHiveServer::NProto::TReqCommitDistributedTransactionPhaseOne request;
        ToProto(request.mutable_transaction_id(), commit->GetTransactionId());
        ToProto(request.mutable_mutation_id(), commit->GetMutationId());
        ToProto(request.mutable_participant_cell_ids(), commit->ParticipantCellIds());
        CreateMutation(HydraManager_, request)
            ->CommitAndLog(Logger);
    }

    TFuture<TSharedRefArray> CoordinatorAbortTransaction(
        const TTransactionId& transactionId,
        const TMutationId& mutationId,
        bool force)
    {
        Y_ASSERT(!HasMutationContext());

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

        NHiveServer::NProto::TReqAbortTransaction request;
        ToProto(request.mutable_transaction_id(), transactionId);
        ToProto(request.mutable_mutation_id(), mutationId);
        request.set_force(force);
        return CreateMutation(HydraManager_, request)
            ->Commit()
            .Apply(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TMutationResponse>& result) -> TSharedRefArray {
                if (!result.IsOK()) {
                    LOG_WARNING(result, "Error committing transaction abort mutation");
                    return CreateErrorResponseMessage(result);
                }
                return result.Value().Data;
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

    void HydraCoordinatorCommitSimpleTransaction(NHiveServer::NProto::TReqCommitSimpleTransaction* request)
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
                SetCommitFailed(commit, ex);
                RemoveTransientCommit(commit);
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
                std::vector<TCellId>(),
                false);
            commit = TransientCommitMap_.Insert(transactionId, std::move(commitHolder));
            commit->SetCommitTimestamp(commitTimestamp);
        }

        SetCommitSucceeded(commit);
        RemoveTransientCommit(commit);
    }

    void HydraCoordinatorCommitDistributedTransactionPhaseOne(NHiveServer::NProto::TReqCommitDistributedTransactionPhaseOne* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto mutationId = FromProto<TMutationId>(request->mutation_id());
        auto participantCellIds = FromProto<std::vector<TCellId>>(request->participant_cell_ids());

        // Ensure commit existence (possibly moving it from transient to persistent).
        auto* commit = GetOrCreatePersistentCommit(
            transactionId,
            mutationId,
            participantCellIds,
            true);

        LOG_DEBUG_UNLESS(IsRecovery(),
            "Distributed commit phase one started "
            "(TransactionId: %v, ParticipantCellIds: %v)",
            transactionId,
            participantCellIds);

        // Prepare at coordinator.
        try {
            // Any exception thrown here is caught below.
            TransactionManager_->PrepareTransactionCommit(transactionId, true);
        } catch (const std::exception& ex) {
            LOG_DEBUG_UNLESS(IsRecovery(), ex, "Coordinator failure; will abort (TransactionId: %v, State: %v)",
                transactionId,
                ECommitState::Prepare);
            SetCommitFailed(commit, ex);
            RemovePersistentCommit(commit);
            TransactionManager_->AbortTransaction(transactionId, true);
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Coordinator success (TransactionId: %v, State: %v)",
            transactionId,
            ECommitState::Prepare);

        ChangeCommitPersistentState(commit, ECommitState::Prepare);
        ChangeCommitTransientState(commit, ECommitState::Prepare);
    }

    void HydraCoordinatorCommitDistributedTransactionPhaseTwo(NHiveServer::NProto::TReqCommitDistributedTransactionPhaseTwo* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto commitTimestamp = request->commit_timestamp();

        auto* commit = FindPersistentCommit(transactionId);
        if (!commit) {
            LOG_ERROR_UNLESS(IsRecovery(), "Requested to start phase two for a non-existing transaction commit, ignored (TransactionId: %v)",
                transactionId);
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(),
            "Distributed commit phase two started "
            "(TransactionId: %v, ParticipantCellIds: %v, CommitTimestamp: %v)",
            transactionId,
            commit->ParticipantCellIds(),
            commitTimestamp);

        YCHECK(commit->GetDistributed());
        YCHECK(commit->GetPersistent());

        if (commit->GetPersistentState() != ECommitState::Prepare) {
            LOG_ERROR_UNLESS(IsRecovery(), "Requested to start phase two for transaction commit in %Qlv state, ignored (TransactionId: %v)",
                commit->GetPersistentState(),
                transactionId);
            return;
        }

        commit->SetCommitTimestamp(commitTimestamp);
        ChangeCommitPersistentState(commit, ECommitState::Commit);
        ChangeCommitTransientState(commit, ECommitState::Commit);

        SetCommitSucceeded(commit);

        try {
            // Any exception thrown here is caught below.
            TransactionManager_->CommitTransaction(transactionId, commitTimestamp);
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Unexpected error: coordinator failure; ignored (TransactionId: %v, State: %v)",
                transactionId,
                ECommitState::Commit);
            return;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Coordinator success (TransactionId: %v, State: %v)",
            transactionId,
            ECommitState::Commit);
    }

    void HydraCoorindatorAbortTransaction(NHiveServer::NProto::TReqAbortTransaction* request)
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

            auto responseMessage = CreateErrorResponseMessage(ex);

            auto* mutationContext = GetCurrentMutationContext();
            mutationContext->Response().Data = responseMessage;

            if (mutationId) {
                ResponseKeeper_->EndRequest(mutationId, responseMessage);
            }

            return;
        }

        auto* commit = FindPersistentCommit(transactionId);
        if (commit) {
            auto error = TError("Transaction %v was aborted", transactionId);
            SetCommitFailed(commit, error);

            ChangeCommitTransientState(commit, ECommitState::Abort);
            ChangeCommitPersistentState(commit, ECommitState::Abort);
        }

        {
            NHiveClient::NProto::NTransactionSupervisor::TRspAbortTransaction response;
            auto responseMessage = CreateResponseMessage(response);

            auto* mutationContext = GetCurrentMutationContext();
            mutationContext->Response().Data = responseMessage;

            if (mutationId) {
                ResponseKeeper_->EndRequest(mutationId, responseMessage);
            }
        }
    }

    void HydraCoordinatorFinishDistributedTransaction(NHiveServer::NProto::TReqFinishDistributedTransaction* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto* commit = FindPersistentCommit(transactionId);
        if (!commit) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Requested to finish a non-existing transaction commit; ignored (TransactionId: %v)",
                transactionId);
            return;
        }

        RemovePersistentCommit(commit);

        LOG_DEBUG_UNLESS(IsRecovery(), "Distributed transaction commit finished (TransactionId: %v)",
            transactionId);
    }

    void HydraParticipantPrepareTransaction(NHiveClient::NProto::NTransactionParticipant::TReqPrepareTransaction* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        try {
            // Any exception thrown here is caught below.
            TransactionManager_->PrepareTransactionCommit(transactionId, true);
        } catch (const std::exception& ex) {
            LOG_DEBUG_UNLESS(IsRecovery(), ex, "Participant failure (TransactionId: %v, State: %v)",
                transactionId,
                ECommitState::Prepare);
            throw;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Participant success (TransactionId: %v, State: %v)",
            transactionId,
            ECommitState::Prepare);
    }

    void HydraParticipantCommitTransaction(NHiveClient::NProto::NTransactionParticipant::TReqCommitTransaction* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto commitTimestamp = request->commit_timestamp();

        try {
            // Any exception thrown here is caught below.
            TransactionManager_->CommitTransaction(transactionId, commitTimestamp);
        } catch (const std::exception& ex) {
            LOG_DEBUG_UNLESS(IsRecovery(), ex, "Participant failure (TransactionId: %v, State: %v)",
                transactionId,
                ECommitState::Commit);
            throw;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Participant success (TransactionId: %v, State: %v)",
            transactionId,
            ECommitState::Commit);
    }

    void HydraParticipantAbortTransaction(NHiveClient::NProto::NTransactionParticipant::TReqAbortTransaction* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        try {
            // Any exception thrown here is caught below.
            TransactionManager_->AbortTransaction(transactionId, true);
        } catch (const std::exception& ex) {
            LOG_DEBUG_UNLESS(IsRecovery(), ex, "Participant failure (TransactionId: %v, State: %v)",
                transactionId,
                ECommitState::Abort);
            throw;
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Participant success (TransactionId: %v, State: %v)",
            transactionId,
            ECommitState::Abort);
    }


    TCommit* FindTransientCommit(const TTransactionId& transactionId)
    {
        return TransientCommitMap_.Find(transactionId);
    }

    TCommit* FindPersistentCommit(const TTransactionId& transactionId)
    {
        return PersistentCommitMap_.Find(transactionId);
    }

    TCommit* FindCommit(const TTransactionId& transactionId)
    {
        if (auto* commit = FindTransientCommit(transactionId)) {
            return commit;
        }
        if (auto* commit = FindPersistentCommit(transactionId)) {
            return commit;
        }
        return nullptr;
    }

    TCommit* GetOrCreatePersistentCommit(
        const TTransactionId& transactionId,
        const TMutationId& mutationId,
        const std::vector<TCellId>& participantCellIds,
        bool distributed)
    {
        auto* commit = FindCommit(transactionId);
        std::unique_ptr<TCommit> commitHolder;
        if (commit) {
            YCHECK(!commit->GetPersistent());
            commitHolder = TransientCommitMap_.Release(transactionId);
        } else {
            commitHolder = std::make_unique<TCommit>(
                transactionId,
                mutationId,
                participantCellIds,
                distributed);
        }
        commitHolder->SetPersistent(true);
        return PersistentCommitMap_.Insert(transactionId, std::move(commitHolder));
    }


    void RemoveTransientCommit(TCommit* commit)
    {
        YCHECK(!commit->GetPersistent());
        TransientCommitMap_.Remove(commit->GetTransactionId());
    }

    void RemovePersistentCommit(TCommit* commit)
    {
        YCHECK(commit->GetPersistent());
        PersistentCommitMap_.Remove(commit->GetTransactionId());
    }


    void SetCommitFailed(TCommit* commit, const TError& error)
    {
        LOG_DEBUG_UNLESS(IsRecovery(), error, "Transaction commit failed (TransactionId: %v)",
            commit->GetTransactionId());

        auto responseMessage = CreateErrorResponseMessage(error);
        SetCommitResponse(commit, responseMessage);
    }

    void SetCommitSucceeded(TCommit* commit)
    {
        LOG_DEBUG_UNLESS(IsRecovery(), "Transaction commit succeeded (TransactionId: %v, CommitTimestamp: %v)",
            commit->GetTransactionId(),
            commit->GetCommitTimestamp());

        NHiveClient::NProto::NTransactionSupervisor::TRspCommitTransaction response;
        response.set_commit_timestamp(commit->GetCommitTimestamp());

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
            LOG_DEBUG("Commit timestamp generated for a non-existing transaction commit; ignored (TransactionId: %v)",
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
        LOG_DEBUG("Transaction commit timestamp generated (TransactionId: %v, CommitTimestamp: %v)",
            transactionId,
            timestamp);

        if (commit->GetDistributed()) {
            NHiveServer::NProto::TReqCommitDistributedTransactionPhaseTwo request;
            ToProto(request.mutable_transaction_id(), transactionId);
            request.set_commit_timestamp(timestamp);
            CreateMutation(HydraManager_, request)
                ->CommitAndLog(Logger);
        } else {
            NHiveServer::NProto::TReqCommitSimpleTransaction request;
            ToProto(request.mutable_transaction_id(), transactionId);
            ToProto(request.mutable_mutation_id(), commit->GetMutationId());
            request.set_commit_timestamp(timestamp);
            CreateMutation(HydraManager_, request)
                ->CommitAndLog(Logger);
        }
    }


    TWrappedParticipantPtr GetParticipant(const TCellId& cellId)
    {
        auto it = WeakParticipantMap_.find(cellId);
        if (it != WeakParticipantMap_.end()) {
            auto participant = it->second.Lock();
            if (participant) {
                return participant;
            }
            WeakParticipantMap_.erase(it);
        }

        auto wrappedParticipant = New<TWrappedParticipant>(
            cellId,
            Config_,
            ParticipantProviders_,
            Logger);

        YCHECK(StrongParticipantMap_.emplace(cellId, wrappedParticipant).second);
        YCHECK(WeakParticipantMap_.emplace(cellId, wrappedParticipant).second);

        LOG_DEBUG("Participant cell registered (ParticipantCellId: %v)",
            cellId);

        return wrappedParticipant;
    }

    void OnParticipantCleanup()
    {
        for (auto it = StrongParticipantMap_.begin(); it != StrongParticipantMap_.end(); ) {
            auto jt = it++;
            if (!jt->second->IsValid()) {
                LOG_DEBUG("Participant cell unregistered (ParticipantCellId: %v)",
                    jt->first);
                StrongParticipantMap_.erase(jt);
            }
        }

        for (auto it = WeakParticipantMap_.begin(); it != WeakParticipantMap_.end(); ) {
            auto jt = it++;
            if (jt->second.IsExpired()) {
                WeakParticipantMap_.erase(jt);
            }
        }
    }


    void ChangeCommitTransientState(TCommit* commit, ECommitState state)
    {
        if (!IsLeader()) {
            return;
        }

        LOG_DEBUG("Commit transient state changed (TransactionId: %v, State: %v -> %v)",
            commit->GetTransactionId(),
            commit->GetTransientState(),
            state);
        commit->SetTransientState(state);
        commit->RespondedCellIds().clear();

        switch (state) {
            case ECommitState::GenerateCommitTimestamp:
                GenerateCommitTimestamp(commit);
                break;

            case ECommitState::Prepare:
            case ECommitState::Commit:
            case ECommitState::Abort:
                SendParticipantRequests(commit);
                break;

            case ECommitState::Finish: {
                NHiveServer::NProto::TReqFinishDistributedTransaction request;
                ToProto(request.mutable_transaction_id(), commit->GetTransactionId());
                CreateMutation(HydraManager_, request)
                    ->CommitAndLog(Logger);
                break;
            }

            default:
                Y_UNREACHABLE();
        }
    }

    void ChangeCommitPersistentState(TCommit* commit, ECommitState state)
    {
        LOG_DEBUG("Commit persistent state changed (TransactionId: %v, State: %v -> %v)",
            commit->GetTransactionId(),
            commit->GetPersistentState(),
            state);
        commit->SetPersistentState(state);
    }

    void SendParticipantRequests(TCommit* commit)
    {
        YCHECK(commit->RespondedCellIds().empty());
        for (const auto& cellId : commit->ParticipantCellIds()) {
            SendParticipantRequest(commit, cellId);
        }
        CheckAllParticipantsResponded(commit);
    }

    void SendParticipantRequest(TCommit* commit, const TCellId& cellId)
    {
        auto participant = GetParticipant(cellId);

        TFuture<void> response;
        switch (commit->GetTransientState()) {
            case ECommitState::Prepare:
                response = participant->PrepareTransaction(commit->GetTransactionId());
                break;

            case ECommitState::Commit:
                response = participant->CommitTransaction(commit->GetTransactionId(), commit->GetCommitTimestamp());
                break;

            case ECommitState::Abort:
                response = participant->AbortTransaction(commit->GetTransactionId());
                break;

            default:
                Y_UNREACHABLE();
        }
        response.Subscribe(
            BIND(&TImpl::OnParticipantResponse, MakeWeak(this), commit->GetTransactionId(), participant)
                .Via(EpochAutomatonInvoker_));
    }

    bool IsParticipantResponseSuccessful(
        TCommit* commit,
        const TWrappedParticipantPtr& participant,
        const TError& error)
    {
        if (error.IsOK()) {
            return true;
        }

        if (error.FindMatching(NTransactionClient::EErrorCode::NoSuchTransaction) &&
            commit->GetTransientState() != ECommitState::Prepare)
        {
            LOG_DEBUG("Transaction is missing at participant; still consider this a success "
                "(TransactionId: %v, ParticipantCellId: %v, State: %v)",
                commit->GetTransactionId(),
                participant->GetCellId(),
                commit->GetTransientState());
            return true;
        }

        return false;
    }

    bool IsParticipantUp(const TError& error)
    {
        if (error.IsOK()) {
            return true;
        }

        if (error.FindMatching(NTransactionClient::EErrorCode::NoSuchTransaction)) {
            return true;
        }

        return false;
    }

    void OnParticipantResponse(
        const TTransactionId& transactionId,
        const TWrappedParticipantPtr& participant,
        const TError& error)
    {
        const auto& participantCellId = participant->GetCellId();

        auto* commit = FindPersistentCommit(transactionId);
        if (!commit) {
            LOG_DEBUG("Received participant response for a non-existing commit; ignored (TransactionId: %v, ParticipantCellId: %v)",
                transactionId,
                participantCellId);
            return;
        }

        if (IsParticipantUp(error)) {
            participant->SetUp();
        } else {
            participant->SetDown(error);
        }

        auto state = commit->GetTransientState();
        if (!IsParticipantResponseSuccessful(commit, participant, error)) {
            switch (state) {
                case ECommitState::Prepare:
                    LOG_DEBUG(error, "Coordinator observes participant failure; will abort "
                        "(TransactionId: %v, ParticipantCellId: %v, State: %v)",
                        commit->GetTransactionId(),
                        participantCellId,
                        state);
                    ChangeCommitTransientState(commit, ECommitState::Abort);
                    break;

                case ECommitState::Commit:
                case ECommitState::Abort:
                    LOG_DEBUG(error, "Coordinator observes participant failure; will retry "
                        "(TransactionId: %v, ParticipantCellId: %v, State: %v)",
                        commit->GetTransactionId(),
                        participantCellId,
                        state);
                    SendParticipantRequest(commit, participantCellId);
                    break;

                default:
                    LOG_DEBUG(error, "Coordinator observes participant failure observed; ignored "
                        "(TransactionId: %v, ParticipantCellId: %v, State: %v)",
                        commit->GetTransactionId(),
                        participantCellId,
                        state);
                    break;
            }
            return;
        }

        LOG_DEBUG("Coordinator observes participant success "
            "(TransactionId: %v, ParticipantCellId: %v, State: %v)",
            commit->GetTransactionId(),
            participantCellId,
            state);

        // NB: Duplicates are fine.
        commit->RespondedCellIds().insert(participantCellId);
        CheckAllParticipantsResponded(commit);
    }

    void CheckAllParticipantsResponded(TCommit* commit)
    {
        if (commit->RespondedCellIds().size() == commit->ParticipantCellIds().size()) {
            ChangeCommitTransientState(commit, GetNewCommitState(commit->GetTransientState()));
        }
    }


    static ECommitState GetNewCommitState(ECommitState state)
    {
        switch (state) {
            case ECommitState::Prepare:
                return ECommitState::GenerateCommitTimestamp;

            case ECommitState::GenerateCommitTimestamp:
                return ECommitState::Commit;

            case ECommitState::Commit:
            case ECommitState::Abort:
                return ECommitState::Finish;

            default:
                Y_UNREACHABLE();
        }
    }


    virtual bool ValidateSnapshotVersion(int version) override
    {
        return version == 2;
    }

    virtual int GetCurrentSnapshotVersion() override
    {
        return 2;
    }


    virtual void OnLeaderActive() override
    {
        TCompositeAutomatonPart::OnLeaderActive();

        ParticipantCleanupExecutor_ = New<TPeriodicExecutor>(
            EpochAutomatonInvoker_,
            BIND(&TImpl::OnParticipantCleanup, MakeWeak(this)),
            ParticipantCleanupPeriod);
        ParticipantCleanupExecutor_->Stop();

        YCHECK(TransientCommitMap_.GetSize() == 0);
        for (const auto& pair : PersistentCommitMap_) {
            auto* commit = pair.second;
            ChangeCommitTransientState(commit, commit->GetPersistentState());
        }
    }

    virtual void OnStopLeading() override
    {
        TCompositeAutomatonPart::OnStopLeading();

        if (ParticipantCleanupExecutor_) {
            ParticipantCleanupExecutor_->Stop();
        }
        ParticipantCleanupExecutor_.Reset();

        TransientCommitMap_.Clear();
        StrongParticipantMap_.clear();
        WeakParticipantMap_.clear();
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
};

////////////////////////////////////////////////////////////////////////////////

TTransactionSupervisor::TTransactionSupervisor(
    TTransactionSupervisorConfigPtr config,
    IInvokerPtr automatonInvoker,
    IInvokerPtr trackerInvoker,
    IHydraManagerPtr hydraManager,
    TCompositeAutomatonPtr automaton,
    TResponseKeeperPtr responseKeeper,
    ITransactionManagerPtr transactionManager,
    const TCellId& selfCellId,
    ITimestampProviderPtr timestampProvider)
    : Impl_(New<TImpl>(
        config,
        automatonInvoker,
        trackerInvoker,
        hydraManager,
        automaton,
        responseKeeper,
        transactionManager,
        selfCellId,
        timestampProvider))
{ }

TTransactionSupervisor::~TTransactionSupervisor() = default;

std::vector<NRpc::IServicePtr> TTransactionSupervisor::GetRpcServices()
{
    return Impl_->GetRpcServices();
}

void TTransactionSupervisor::RegisterParticipantProvider(ITransactionParticipantProviderPtr provider)
{
    return Impl_->RegisterParticipantProvider(std::move(provider));
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

} // namespace NHiveServer
} // namespace NYT
