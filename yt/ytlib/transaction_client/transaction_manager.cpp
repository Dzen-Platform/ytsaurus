#include "transaction_manager.h"
#include "private.h"
#include "config.h"
#include "helpers.h"
#include "timestamp_provider.h"
#include "action.h"

#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/transaction_supervisor_service_proxy.h>
#include <yt/ytlib/hive/transaction_participant_service_proxy.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/master_ypath_proxy.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/tablet_client/tablet_service_proxy.h>

#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/rpc/helpers.h>
#include <yt/core/rpc/retrying_channel.h>

#include <yt/core/ytree/public.h>

#include <atomic>

namespace NYT {
namespace NTransactionClient {

using namespace NYTree;
using namespace NHydra;
using namespace NHiveClient;
using namespace NRpc;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NTabletClient;

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TransactionClientLogger;
static std::atomic<ui32> TabletTransactionHashCounter; // used as a part of transaction id

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TTransactionManagerConfigPtr config,
        const TCellId& cellId,
        IChannelPtr channel,
        ITimestampProviderPtr timestampProvider,
        TCellDirectoryPtr cellDirectory);

    TFuture<TTransactionPtr> Start(
        ETransactionType type,
        const TTransactionStartOptions& options);

    TTransactionPtr Attach(
        const TTransactionId& id,
        const TTransactionAttachOptions& options);

    void AbortAll();

private:
    friend class TTransaction;

    const TTransactionManagerConfigPtr Config_;
    const IChannelPtr MasterChannel_;
    const TCellId CellId_;
    const ITimestampProviderPtr TimestampProvider_;
    const TCellDirectoryPtr CellDirectory_;

    TSpinLock SpinLock_;
    yhash_set<TTransaction::TImpl*> AliveTransactions_;


    TTransactionSupervisorServiceProxy MakeSupervisorProxy(IChannelPtr channel, bool retry)
    {
        if (retry) {
            channel = CreateRetryingChannel(Config_, std::move(channel));
        }
        TTransactionSupervisorServiceProxy proxy(std::move(channel));
        proxy.SetDefaultTimeout(Config_->RpcTimeout);
        return proxy;
    }

    TTransactionParticipantServiceProxy MakeParticipantProxy(IChannelPtr channel)
    {
        TTransactionParticipantServiceProxy proxy(std::move(channel));
        proxy.SetDefaultTimeout(Config_->RpcTimeout);
        return proxy;
    }
};

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETransactionState,
    (Initializing)
    (Active)
    (Aborted)
    (Committing)
    (Committed)
    (Detached)
);

class TTransaction::TImpl
    : public TRefCounted
{
public:
    explicit TImpl(TIntrusivePtr<TTransactionManager::TImpl> owner)
        : Owner_(owner)
    { }

    ~TImpl()
    {
        Unregister();
    }


    TFuture<void> Start(
        ETransactionType type,
        const TTransactionStartOptions& options)
    {
        try {
            ValidateStartOptions(type, options);
        } catch (const std::exception& ex) {
            return MakeFuture<void>(ex);
        }

        Type_ = type;
        AutoAbort_ = options.AutoAbort;
        Sticky_ = options.Sticky;
        PingPeriod_ = options.PingPeriod;
        Ping_ = options.Ping;
        PingAncestors_ = options.PingAncestors;
        Timeout_ = options.Timeout;
        Atomicity_ = options.Atomicity;
        Durability_ = options.Durability;

        switch (Atomicity_) {
            case EAtomicity::Full:
                if (options.Id) {
                    auto startTimestamp = TimestampFromTransactionId(options.Id);
                    OnGotStartTimestamp(options, startTimestamp);
                } else {
                    return Owner_->TimestampProvider_->GenerateTimestamps()
                        .Apply(BIND(&TImpl::OnGotStartTimestamp, MakeStrong(this), options));
                }

            case EAtomicity::None:
                return StartNonAtomicTabletTransaction();

            default:
                Y_UNREACHABLE();
        }
    }

    void Attach(
        const TTransactionId& id,
        const TTransactionAttachOptions& options)
    {
        ValidateAttachOptions(id, options);

        Type_ = ETransactionType::Master;
        Id_ = id;
        AutoAbort_ = options.AutoAbort;
        YCHECK(!options.Sticky);
        PingPeriod_ = options.PingPeriod;
        Ping_ = options.Ping;
        PingAncestors_ = options.PingAncestors;
        State_ = ETransactionState::Active;

        {
            auto guard = Guard(SpinLock_);
            FindOrAddParticipant(Owner_->CellId_);
        }

        Register();

        LOG_DEBUG("Master transaction attached (TransactionId: %v, AutoAbort: %v, Ping: %v, PingAncestors: %v)",
            Id_,
            AutoAbort_,
            Ping_,
            PingAncestors_);

        if (Ping_) {
            RunPeriodicPings();
        }
    }

    TFuture<void> Commit(const TTransactionCommitOptions& options)
    {
        try {
            {
                auto guard = Guard(SpinLock_);
                Error_.ThrowOnError();
                switch (State_) {
                    case ETransactionState::Committing:
                        THROW_ERROR_EXCEPTION("Transaction is already being committed");

                    case ETransactionState::Committed:
                        THROW_ERROR_EXCEPTION("Transaction is already committed");

                    case ETransactionState::Aborted:
                        THROW_ERROR_EXCEPTION("Transaction is already aborted");

                    case ETransactionState::Active:
                        State_ = ETransactionState::Committing;
                        break;

                    default:
                        Y_UNREACHABLE();
                }
            }

            switch (Atomicity_) {
                case EAtomicity::Full:
                    return DoCommitAtomic(options);

                case EAtomicity::None:
                    return DoCommitNonAtomic();

                default:
                    Y_UNREACHABLE();
            }
        } catch (const std::exception& ex) {
            return MakeFuture<void>(ex);
        }
    }

    TFuture<void> Abort(const TTransactionAbortOptions& options = TTransactionAbortOptions())
    {
        VERIFY_THREAD_AFFINITY_ANY();

        if (Atomicity_ != EAtomicity::Full) {
            return VoidFuture;
        }

        return SendAbort(options).Apply(BIND([=, this_ = MakeStrong(this)] () {
            DoAbort(TError("Transaction aborted by user request"));
        }));
    }

    TFuture<void> Ping()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        try {
            if (Atomicity_ != EAtomicity::Full) {
                THROW_ERROR_EXCEPTION("Cannot ping a transaction with %Qlv atomicity",
                    Atomicity_);
            }

            return SendPing();
        } catch (const std::exception& ex) {
            return MakeFuture<void>(ex);
        }
    }

    void Detach()
    {
        if (Type_ != ETransactionType::Master) {
            THROW_ERROR_EXCEPTION("Cannot detach a %Qlv transaction",
                Type_);
        }

        if (Sticky_) {
            THROW_ERROR_EXCEPTION("Cannot detach a sticky transaction");
        }

        YCHECK(Atomicity_ == EAtomicity::Full);

        {
            auto guard = Guard(SpinLock_);
            switch (State_) {
                case ETransactionState::Committed:
                    THROW_ERROR_EXCEPTION("Transaction %v is already committed", Id_);
                    break;

                case ETransactionState::Aborted:
                    THROW_ERROR_EXCEPTION("Transaction %v is already aborted", Id_);
                    break;

                case ETransactionState::Active:
                    State_ = ETransactionState::Detached;
                    break;

                case ETransactionState::Detached:
                    return;

                default:
                    Y_UNREACHABLE();
            }
        }

        LOG_DEBUG("Transaction detached (TransactionId: %v)",
            Id_);
    }


    ETransactionType GetType() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Type_;
    }

    const TTransactionId& GetId() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Id_;
    }

    TTimestamp GetStartTimestamp() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return StartTimestamp_;
    }

    ETransactionState GetState()
    {
        auto guard = Guard(SpinLock_);
        return State_;
    }

    EAtomicity GetAtomicity() const
    {
        return Atomicity_;
    }

    EDurability GetDurability() const
    {
        return Durability_;
    }

    TDuration GetTimeout() const
    {
        return Timeout_.Get(Owner_->Config_->DefaultTransactionTimeout);
    }


    void AddParticipant(const TCellId& cellId)
    {
        YCHECK(TypeFromId(cellId) == EObjectType::TabletCell);


        if (Atomicity_ != EAtomicity::Full) {
            return;
        }

        {
            auto guard = Guard(SpinLock_);

            if (State_ != ETransactionState::Active) {
                return;
            }

            FindOrAddParticipant(cellId);
        }
    }

    void AddAction(const TCellId& cellId, const TTransactionActionData& data)
    {
        YCHECK(TypeFromId(cellId) == EObjectType::TabletCell);


        if (Atomicity_ != EAtomicity::Full) {
            THROW_ERROR_EXCEPTION("Atomicity must be %Qlv for custom actions",
                EAtomicity::Full);
        }

        {
            auto guard = Guard(SpinLock_);

            if (State_ != ETransactionState::Active) {
                return;
            }

            auto* info = FindOrAddParticipant(cellId);
            info->Actions.push_back(data);
        }

        LOG_DEBUG("Transaction action added (TransactionId: %v, CellId: %v, ActionType: %v)",
            Id_,
            cellId,
            data.Type);
    }


    void SubscribeCommitted(const TCallback<void()>& handler)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        Committed_.Subscribe(handler);
    }

    void UnsubscribeCommitted(const TCallback<void()>& handler)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        Committed_.Unsubscribe(handler);
    }


    void SubscribeAborted(const TCallback<void()>& handler)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        Aborted_.Subscribe(handler);
    }

    void UnsubscribeAborted(const TCallback<void()>& handler)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        Aborted_.Unsubscribe(handler);
    }

private:
    friend class TTransactionManager::TImpl;

    const TIntrusivePtr<TTransactionManager::TImpl> Owner_;
    ETransactionType Type_;
    bool AutoAbort_ = false;
    bool Sticky_ = false;
    TNullable<TDuration> PingPeriod_;
    bool Ping_ = false;
    bool PingAncestors_ = false;
    TNullable<TDuration> Timeout_;
    EAtomicity Atomicity_ = EAtomicity::Full;
    EDurability Durability_ = EDurability::Sync;

    TSpinLock SpinLock_;

    ETransactionState State_ = ETransactionState::Initializing;

    TSingleShotCallbackList<void()> Committed_;
    TSingleShotCallbackList<void()> Aborted_;

    struct TParticipantInfo
    {
        std::vector<TTransactionActionData> Actions;
    };
    yhash_map<TCellId, TParticipantInfo> ParticiapantMap_;

    TError Error_;

    TTimestamp StartTimestamp_ = NullTimestamp;
    TTransactionId Id_;



    static void ValidateStartOptions(
        ETransactionType type,
        const TTransactionStartOptions& options)
    {
        switch (type) {
            case ETransactionType::Master:
                ValidateMasterStartOptions(options);
                break;
            case ETransactionType::Tablet:
                ValidateTabletStartOptions(options);
                break;
            default:
                Y_UNREACHABLE();
        }
    }

    static void ValidateMasterStartOptions(const TTransactionStartOptions& options)
    {
        if (options.Id) {
            THROW_ERROR_EXCEPTION("Cannot use externally provided id for master transactions");
        }
        if (options.Atomicity != EAtomicity::Full) {
            THROW_ERROR_EXCEPTION("Atomicity must be %Qlv for master transactions",
                EAtomicity::Full);
        }
        if (options.Durability != EDurability::Sync) {
            THROW_ERROR_EXCEPTION("Durability must be %Qlv for master transactions",
                EDurability::Sync);
        }
    }

    static void ValidateTabletStartOptions(const TTransactionStartOptions& options)
    {
        if (options.ParentId) {
            THROW_ERROR_EXCEPTION("Tablet transaction cannot have a parent");
        }
        if (options.Id) {
            auto type = TypeFromId(options.Id);
            if (type != EObjectType::AtomicTabletTransaction) {
                THROW_ERROR_EXCEPTION("Externally provided transaction id %v has invalid type",
                    options.Id);
            }
        }
        if (!options.Ping) {
            THROW_ERROR_EXCEPTION("Cannot switch off pings for a tablet transaction");
        }
        if (options.Atomicity == EAtomicity::Full && options.Durability != EDurability::Sync) {
            THROW_ERROR_EXCEPTION("Durability must be %Qlv for tablet transactions with %Qlv atomicity",
                EDurability::Sync,
                EAtomicity::Full);
        }
        if (options.Sticky && options.Atomicity != EAtomicity::Full) {
            THROW_ERROR_EXCEPTION("Atomicity must be %Qlv for sticky transactions",
                EAtomicity::Full);
        }
    }

    static void ValidateAttachOptions(
        const TTransactionId& id,
        const TTransactionAttachOptions& options)
    {
        ValidateMasterTransactionId(id);
        // NB: Sticky transactions are handled in TNativeClient.
        YCHECK(!options.Sticky);
    }


    void Register()
    {
        if (AutoAbort_) {
            TGuard<TSpinLock> guard(Owner_->SpinLock_);
            YCHECK(Owner_->AliveTransactions_.insert(this).second);
        }
    }

    void Unregister()
    {
        if (AutoAbort_) {
            {
                TGuard<TSpinLock> guard(Owner_->SpinLock_);
                // NB: Instance is not necessarily registered.
                Owner_->AliveTransactions_.erase(this);
            }

            if (State_ == ETransactionState::Active) {
                SendAbort();
            }
        }
    }


    TFuture<void> OnGotStartTimestamp(const TTransactionStartOptions& options, TTimestamp timestamp)
    {
        StartTimestamp_ = timestamp;

        Register();

        LOG_DEBUG("Starting transaction (StartTimestamp: %v, Type: %v)",
            StartTimestamp_,
            Type_);

        switch (Type_) {
            case ETransactionType::Master:
                return StartMasterTransaction(options);
            case ETransactionType::Tablet:
                return StartAtomicTabletTransaction(options);
            default:
                Y_UNREACHABLE();
        }
    }

    TFuture<void> StartMasterTransaction(const TTransactionStartOptions& options)
    {
        TObjectServiceProxy proxy(Owner_->MasterChannel_);
        auto req = TMasterYPathProxy::CreateObject();
        req->set_type(static_cast<int>(EObjectType::Transaction));

        auto attributes = options.Attributes ? options.Attributes->Clone() : CreateEphemeralAttributes();
        attributes->Set("timeout", GetTimeout());
        if (options.ParentId) {
            attributes->Set("parent_id", options.ParentId);
        }
        ToProto(req->mutable_object_attributes(), *attributes);

        SetOrGenerateMutationId(req, options.MutationId, options.Retry);

        return proxy.Execute(req).Apply(
            BIND(&TImpl::OnMasterTransactionStarted, MakeStrong(this)));
    }

    void OnMasterTransactionStarted(const TMasterYPathProxy::TErrorOrRspCreateObjectPtr& rspOrError)
    {
        if (!rspOrError.IsOK()) {
            State_ = ETransactionState::Aborted;
            THROW_ERROR rspOrError;
        }

        State_ = ETransactionState::Active;

        const auto& rsp = rspOrError.Value();
        Id_ = FromProto<TTransactionId>(rsp->object_id());

        {
            auto guard = Guard(SpinLock_);
            FindOrAddParticipant(Owner_->CellId_);
        }

        LOG_DEBUG("Master transaction started (TransactionId: %v, StartTimestamp: %v, AutoAbort: %v, Ping: %v, PingAncestors: %v)",
            Id_,
            StartTimestamp_,
            AutoAbort_,
            Ping_,
            PingAncestors_);

        if (Ping_) {
            RunPeriodicPings();
        }
    }

    TFuture<void> StartAtomicTabletTransaction(const TTransactionStartOptions& options)
    {
        YCHECK(Atomicity_ == EAtomicity::Full);
        YCHECK(Durability_ == EDurability::Sync);

        Id_ = options.Id
            ? options.Id
            : MakeTabletTransactionId(
                Atomicity_,
                CellTagFromId(Owner_->CellId_),
                StartTimestamp_,
                TabletTransactionHashCounter++);

        State_ = ETransactionState::Active;

        LOG_DEBUG("Atomic tablet transaction started (TransactionId: %v, StartTimestamp: %v, AutoAbort: %v)",
            Id_,
            StartTimestamp_,
            AutoAbort_);

        // Start ping scheduling.
        // Participants will be added into it upon arrival.
        YCHECK(Ping_);
        RunPeriodicPings();

        return VoidFuture;
    }

    TFuture<void> StartNonAtomicTabletTransaction()
    {
        YCHECK(Atomicity_ == EAtomicity::None);

        StartTimestamp_ = InstantToTimestamp(TInstant::Now()).first;

        Id_ = MakeTabletTransactionId(
            Atomicity_,
            CellTagFromId(Owner_->CellId_),
            StartTimestamp_,
            TabletTransactionHashCounter++);

        State_ = ETransactionState::Active;

        LOG_DEBUG("Non-atomic tablet transaction started (TransactionId: %v, Durability: %v)",
            Id_,
            Durability_);

        return VoidFuture;
    }

    void FireCommitted()
    {
        Committed_.Fire();
    }

    void SetTransactionCommitted()
    {
        {
            auto guard = Guard(SpinLock_);
            if (State_ != ETransactionState::Committing) {
                THROW_ERROR Error_;
            }
            State_ = ETransactionState::Committed;
        }

        FireCommitted();

        LOG_DEBUG("Transaction committed (TransactionId: %v)",
            Id_);
    }

    TFuture<void> DoCommitAtomic(const TTransactionCommitOptions& options)
    {
        if (ParticiapantMap_.empty()) {
            SetTransactionCommitted();
            return VoidFuture;
        }

        std::vector<TFuture<void>> registerActionsAsyncResults;
        for (const auto& pair : ParticiapantMap_) {
            const auto& cellId = pair.first;
            const auto& participant = pair.second;
            if (participant.Actions.empty()) {
                continue;
            }

            auto channel = Owner_->CellDirectory_->GetChannelOrThrow(cellId);
            auto proxy = Owner_->MakeParticipantProxy(std::move(channel));
            auto req = proxy.RegisterTransactionActions();
            ToProto(req->mutable_transaction_id(), Id_);
            ToProto(req->mutable_actions(), participant.Actions);

            LOG_DEBUG("Registering transaction actions (TransactionId: %v, CellId: %v, ActionCount: %v)",
                Id_,
                cellId,
                participant.Actions.size());

            registerActionsAsyncResults.push_back(req->Invoke().As<void>());
        }

        auto registerActionsAsyncResult = Combine(registerActionsAsyncResults);
        return registerActionsAsyncResult.Apply(BIND(
            &TImpl::OnTransactionActionsRegistered,
            MakeStrong(this),
            options));
    }

    TFuture<void> DoCommitNonAtomic()
    {
        SetTransactionCommitted();
        return VoidFuture;
    }

    TCellId ChooseCoordinator(const TTransactionCommitOptions& options)
    {
        if (Type_ == ETransactionType::Master) {
            return Owner_->CellId_;
        }

        if (options.CoordinatorCellId) {
            if (ParticiapantMap_.find(options.CoordinatorCellId) == ParticiapantMap_.end()) {
                THROW_ERROR_EXCEPTION("Cell %v is not a participant",
                    options.CoordinatorCellId);
            }
            return options.CoordinatorCellId;
        }

        auto participantCellIds = GetKeys(ParticiapantMap_);
        return participantCellIds[RandomNumber(participantCellIds.size())];
    }

    TFuture<void> OnTransactionActionsRegistered(const TTransactionCommitOptions& options)
    {
        try {
            auto coordinatorCellId = ChooseCoordinator(options);

            LOG_DEBUG("Committing transaction (TransactionId: %v, CoordinatorCellId: %v)",
                Id_,
                coordinatorCellId);

            auto coordinatorChannel = Owner_->CellDirectory_->GetChannelOrThrow(coordinatorCellId);
            auto proxy = Owner_->MakeSupervisorProxy(std::move(coordinatorChannel), true);
            auto req = proxy.CommitTransaction();
            ToProto(req->mutable_transaction_id(), Id_);
            for (const auto& pair : ParticiapantMap_) {
                const auto& cellId = pair.first;
                if (cellId != coordinatorCellId) {
                    ToProto(req->add_participant_cell_ids(), cellId);
                }
            }
            req->set_force_2pc(options.Force2PC);
            SetOrGenerateMutationId(req, options.MutationId, options.Retry);

            return req->Invoke().Apply(
                BIND(&TImpl::OnAtomicTransactionCommitted, MakeStrong(this), coordinatorCellId));
        } catch (const std::exception& ex) {
            return MakeFuture(TError(ex));
        }
    }

    void OnAtomicTransactionCommitted(
        const TCellId& cellId,
        const TTransactionSupervisorServiceProxy::TErrorOrRspCommitTransactionPtr& rspOrError)
    {
        if (!rspOrError.IsOK()) {
            auto error = TError("Error committing transaction %v at cell %v",
                Id_,
                cellId)
                << rspOrError;
            DoAbort(error);
            THROW_ERROR error;
        }

        SetTransactionCommitted();
    }


    TFuture<void> SendPing()
    {
        std::vector<TFuture<void>> asyncResults;
        auto participantIds = GetParticipantIds();
        for (const auto& cellId : participantIds) {
            LOG_DEBUG("Pinging transaction (TransactionId: %v, CellId: %v)",
                Id_,
                cellId);

            auto channel = Owner_->CellDirectory_->FindChannel(cellId);
            if (!channel) {
                continue;
            }

            auto proxy = Owner_->MakeSupervisorProxy(std::move(channel), false);
            auto req = proxy.PingTransaction();
            ToProto(req->mutable_transaction_id(), Id_);
            if (cellId == Owner_->CellId_) {
                req->set_ping_ancestors(PingAncestors_);
            }

            auto asyncRspOrError = req->Invoke();
            asyncResults.push_back(asyncRspOrError.Apply(
                BIND([=, this_ = MakeStrong(this)] (const TTransactionSupervisorServiceProxy::TErrorOrRspPingTransactionPtr& rspOrError) {
                    if (rspOrError.IsOK()) {
                        LOG_DEBUG("Transaction pinged (TransactionId: %v, CellId: %v)",
                            Id_,
                            cellId);
                    } else if (rspOrError.GetCode() == NTransactionClient::EErrorCode::NoSuchTransaction && GetState() == ETransactionState::Active) {
                        // Hard error.
                        LOG_WARNING("Transaction has expired or was aborted (TransactionId: %v, CellId: %v)",
                            Id_,
                            cellId);
                        auto error = TError("Transaction %v has expired or was aborted at cell %v",
                            Id_,
                            cellId);
                        DoAbort(error);
                        THROW_ERROR error;
                    } else {
                        // Soft error.
                        LOG_WARNING(rspOrError, "Error pinging transaction (TransactionId: %v, CellId: %v)",
                            Id_,
                            cellId);
                        THROW_ERROR_EXCEPTION("Failed to ping transaction %v at cell %v",
                            Id_,
                            cellId)
                            << rspOrError;
                    }
                })));
        }

        return Combine(asyncResults);
    }

    void RunPeriodicPings()
    {
        if (!IsPingableState()) {
            return;
        }

        SendPing().Subscribe(BIND([=, this_ = MakeStrong(this)] (const TError& error) {
            if (!IsPingableState()) {
                return;
            }

            if (error.FindMatching(NYT::EErrorCode::Timeout)) {
                RunPeriodicPings();
                return;
            }

            LOG_DEBUG("Transaction ping scheduled (TransactionId: %v)",
                Id_);

            TDelayedExecutor::Submit(
                BIND(IgnoreResult(&TImpl::RunPeriodicPings), MakeWeak(this)),
                PingPeriod_.Get(Owner_->Config_->DefaultPingPeriod));
        }));
    }

    bool IsPingableState()
    {
        auto state = GetState();
        // NB: We have to continue pinging the transaction while committing.
        return state == ETransactionState::Active || state == ETransactionState::Committing;
    }


    TFuture<void> SendAbort(const TTransactionAbortOptions& options = TTransactionAbortOptions())
    {
        std::vector<TFuture<void>> asyncResults;
        auto participantIds = GetParticipantIds();
        for (const auto& cellId : participantIds) {
            LOG_DEBUG("Aborting transaction (TransactionId: %v, CellId: %v)",
                Id_,
                cellId);

            auto channel = Owner_->CellDirectory_->FindChannel(cellId);
            if (!channel) {
                continue;
            }

            auto proxy = Owner_->MakeSupervisorProxy(std::move(channel), true);
            auto req = proxy.AbortTransaction();
            ToProto(req->mutable_transaction_id(), Id_);
            req->set_force(options.Force);
            SetMutationId(req, options.MutationId, options.Retry);

            auto asyncRspOrError = req->Invoke();
            // NB: "this" could be dying; can't capture it.
            auto transactionId = Id_;
            asyncResults.push_back(asyncRspOrError.Apply(
                BIND([=] (const TTransactionSupervisorServiceProxy::TErrorOrRspAbortTransactionPtr& rspOrError) {
                    if (rspOrError.IsOK()) {
                        LOG_DEBUG("Transaction aborted (TransactionId: %v, CellId: %v)",
                            transactionId,
                            cellId);
                    } else if (rspOrError.GetCode() == NTransactionClient::EErrorCode::NoSuchTransaction) {
                        LOG_DEBUG("Transaction has expired or was already aborted, ignored (TransactionId: %v, CellId: %v)",
                            transactionId,
                            cellId);
                    } else {
                        LOG_WARNING(rspOrError, "Error aborting transaction (TransactionId: %v, CellId: %v)",
                            transactionId,
                            cellId);
                        THROW_ERROR_EXCEPTION("Error aborting transaction %v at cell %v",
                            transactionId,
                            cellId)
                            << rspOrError;
                    }
                })));
        }

        return Combine(asyncResults);
    }


    void FireAborted()
    {
        Aborted_.Fire();
    }

    void DoAbort(const TError& error)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        {
            auto guard = Guard(SpinLock_);
            if (State_ == ETransactionState::Aborted) {
                return;
            }
            State_ = ETransactionState::Aborted;
            Error_ = error;
        }

        FireAborted();
    }


    TParticipantInfo* FindOrAddParticipant(const TCellId& cellId)
    {
        VERIFY_SPINLOCK_AFFINITY(SpinLock_);

        auto it = ParticiapantMap_.find(cellId);
        if (it != ParticiapantMap_.end()) {
            auto pair = ParticiapantMap_.emplace(cellId, TParticipantInfo());
            YCHECK(pair.second);
            it = pair.first;

            LOG_DEBUG("Transaction participant added (TransactionId: %v, CellId: %v)",
                Id_,
                cellId);
        }

        return &it->second;
    }

    std::vector<TCellId> GetParticipantIds()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        return GetKeys(ParticiapantMap_);
    }
};

////////////////////////////////////////////////////////////////////////////////

TTransactionManager::TImpl::TImpl(
    TTransactionManagerConfigPtr config,
    const TCellId& cellId,
    IChannelPtr masterChannel,
    ITimestampProviderPtr timestampProvider,
    TCellDirectoryPtr cellDirectory)
    : Config_(config)
    , MasterChannel_(masterChannel)
    , CellId_(cellId)
    , TimestampProvider_(timestampProvider)
    , CellDirectory_(cellDirectory)
{
    YCHECK(Config_);
    YCHECK(MasterChannel_);
    YCHECK(TimestampProvider_);
    YCHECK(CellDirectory_);
}

TFuture<TTransactionPtr> TTransactionManager::TImpl::Start(
    ETransactionType type,
    const TTransactionStartOptions& options)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto transaction = New<TTransaction::TImpl>(this);
    return transaction->Start(type, options).Apply(BIND([=] () {
        return TTransaction::Create(transaction);
    }));
}

TTransactionPtr TTransactionManager::TImpl::Attach(
    const TTransactionId& id,
    const TTransactionAttachOptions& options)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto transaction = New<TTransaction::TImpl>(this);
    transaction->Attach(id, options);
    return TTransaction::Create(transaction);
}

void TTransactionManager::TImpl::AbortAll()
{
    VERIFY_THREAD_AFFINITY_ANY();

    std::vector<TIntrusivePtr<TTransaction::TImpl>> transactions;
    {
        TGuard<TSpinLock> guard(SpinLock_);
        for (auto* rawTransaction : AliveTransactions_) {
            auto transaction = TRefCounted::DangerousGetPtr(rawTransaction);
            if (transaction) {
                transactions.push_back(transaction);
            }
        }
    }

    for (const auto& transaction : transactions) {
        transaction->Abort();
    }
}

////////////////////////////////////////////////////////////////////////////////

TTransaction::TTransaction(TIntrusivePtr<TImpl> impl)
    : Impl_(std::move(impl))
{ }

TTransactionPtr TTransaction::Create(TIntrusivePtr<TImpl> impl)
{
    return New<TTransaction>(std::move(impl));
}

TTransaction::~TTransaction() = default;

TFuture<void> TTransaction::Commit(const TTransactionCommitOptions& options)
{
    return Impl_->Commit(options);
}

TFuture<void> TTransaction::Abort(const TTransactionAbortOptions& options)
{
    return Impl_->Abort(options);
}

void TTransaction::Detach()
{
    Impl_->Detach();
}

TFuture<void> TTransaction::Ping()
{
    return Impl_->Ping();
}

ETransactionType TTransaction::GetType() const
{
    return Impl_->GetType();
}

const TTransactionId& TTransaction::GetId() const
{
    return Impl_->GetId();
}

TTimestamp TTransaction::GetStartTimestamp() const
{
    return Impl_->GetStartTimestamp();
}

EAtomicity TTransaction::GetAtomicity() const
{
    return Impl_->GetAtomicity();
}

EDurability TTransaction::GetDurability() const
{
    return Impl_->GetDurability();
}

TDuration TTransaction::GetTimeout() const
{
    return Impl_->GetTimeout();
}

void TTransaction::AddParticipant(const TCellId& cellId)
{
    Impl_->AddParticipant(cellId);
}

void TTransaction::AddAction(const TCellId& cellId, const TTransactionActionData& data)
{
    Impl_->AddAction(cellId, data);
}

DELEGATE_SIGNAL(TTransaction, void(), Committed, *Impl_);
DELEGATE_SIGNAL(TTransaction, void(), Aborted, *Impl_);

////////////////////////////////////////////////////////////////////////////////

TTransactionManager::TTransactionManager(
    TTransactionManagerConfigPtr config,
    const TCellId& cellId,
    IChannelPtr masterChannel,
    ITimestampProviderPtr timestampProvider,
    TCellDirectoryPtr cellDirectory)
    : Impl_(New<TImpl>(
        config,
        cellId,
        masterChannel,
        timestampProvider,
        cellDirectory))
{ }

TTransactionManager::~TTransactionManager()
{ }

TFuture<TTransactionPtr> TTransactionManager::Start(
    ETransactionType type,
    const TTransactionStartOptions& options)
{
    return Impl_->Start(type, options);
}

TTransactionPtr TTransactionManager::Attach(
    const TTransactionId& id,
    const TTransactionAttachOptions& options)
{
    return Impl_->Attach(id, options);
}

void TTransactionManager::AbortAll()
{
    Impl_->AbortAll();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionClient
} // namespace NYT
