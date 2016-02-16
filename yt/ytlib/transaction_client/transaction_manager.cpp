#include "transaction_manager.h"
#include "private.h"
#include "config.h"
#include "helpers.h"
#include "timestamp_provider.h"

#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/transaction_supervisor_service_proxy.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/master_ypath_proxy.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/tablet_client/tablet_service_proxy.h>

#include <yt/ytlib/transaction_client/transaction_ypath.pb.h>

#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/rpc/helpers.h>

#include <yt/core/ytree/public.h>

#include <atomic>

namespace NYT {
namespace NTransactionClient {

using namespace NYTree;
using namespace NHydra;
using namespace NHive;
using namespace NRpc;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NTabletClient;

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


    std::unique_ptr<TTransactionSupervisorServiceProxy> MakeSupervisorProxy(IChannelPtr channel)
    {
        auto proxy = std::make_unique<TTransactionSupervisorServiceProxy>(channel);
        proxy->SetDefaultTimeout(Config_->RpcTimeout);
        return proxy;
    }

    std::unique_ptr<TTabletServiceProxy> MakeTabletProxy(IChannelPtr channel)
    {
        auto proxy = std::make_unique<TTabletServiceProxy>(channel);
        proxy->SetDefaultTimeout(Config_->RpcTimeout);
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
        , AutoAbort_(false)
        , Ping_(false)
        , PingAncestors_(false)
        , State_(ETransactionState::Initializing)
        , Aborted_(NewPromise<void>())
        , StartTimestamp_(NullTimestamp)
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
            return MakeFuture(TError(ex));
        }

        Type_ = type;
        AutoAbort_ = options.AutoAbort;
        PingPeriod_ = options.PingPeriod;
        Ping_ = options.Ping;
        PingAncestors_ = options.PingAncestors;
        Timeout_ = options.Timeout;
        Atomicity_ = options.Atomicity;
        Durability_ = options.Durability;

        switch (Atomicity_) {
            case EAtomicity::Full:
                return Owner_->TimestampProvider_->GenerateTimestamps()
                    .Apply(BIND(&TImpl::OnGotStartTimestamp, MakeStrong(this), options));

            case EAtomicity::None:
                return StartNonAtomicTabletTransaction();

            default:
                YUNREACHABLE();
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
        PingPeriod_ = options.PingPeriod;
        Ping_ = options.Ping;
        PingAncestors_ = options.PingAncestors;
        State_ = ETransactionState::Active;

        YCHECK(CellIdToStartTransactionResult_.insert(std::make_pair(
            Owner_->CellId_,
            MakePromise<void>(TError()))).second);

        Register();

        LOG_INFO("Master transaction attached (TransactionId: %v, AutoAbort: %v, Ping: %v, PingAncestors: %v)",
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
        VERIFY_THREAD_AFFINITY(ClientThread);

        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (!Error_.IsOK()) {
                return MakeFuture(Error_);
            }
            switch (State_) {
                case ETransactionState::Committing:
                    return MakeFuture(TError("Transaction is already being committed"));

                case ETransactionState::Committed:
                    return MakeFuture(TError("Transaction is already committed"));

                case ETransactionState::Aborted:
                    return MakeFuture(TError("Transaction is already aborted"));

                case ETransactionState::Active:
                    State_ = ETransactionState::Committing;
                    break;

                default:
                    YUNREACHABLE();
            }
        }

        switch (Atomicity_) {
            case EAtomicity::Full: {
                auto participantGuids = GetParticipantIds();
                if (participantGuids.empty()) {
                    {
                        TGuard<TSpinLock> guard(SpinLock_);
                        if (State_ != ETransactionState::Committing) {
                            return MakeFuture(Error_);
                        }
                        State_ = ETransactionState::Committed;
                    }

                    LOG_INFO("Trivial transaction committed (TransactionId: %v)",
                        Id_);
                    return VoidFuture;
                }

                auto coordinatorCellId = Type_ == ETransactionType::Master
                    ? Owner_->CellId_
                    : participantGuids[RandomNumber(participantGuids.size())];

                LOG_INFO("Committing transaction (TransactionId: %v, CoordinatorCellId: %v)",
                    Id_,
                    coordinatorCellId);

                auto channel = Owner_->CellDirectory_->GetChannelOrThrow(coordinatorCellId);
                auto proxy = Owner_->MakeSupervisorProxy(channel);

                auto req = proxy->CommitTransaction();
                ToProto(req->mutable_transaction_id(), Id_);
                for (const auto& cellId : participantGuids) {
                    if (cellId != coordinatorCellId) {
                        ToProto(req->add_participant_cell_ids(), cellId);
                    }
                }
                SetOrGenerateMutationId(req, options.MutationId, options.Retry);

                return req->Invoke().Apply(
                    BIND(&TImpl::OnAtomicTransactionCommitted, MakeStrong(this), coordinatorCellId));
            }

            case EAtomicity::None:
                SetTransactionCommitted();
                return VoidFuture;

            default:
                YUNREACHABLE();
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

        if (Atomicity_ != EAtomicity::Full) {
            return MakeFuture(TError("Cannot ping a transaction with %Qlv atomicity",
                Atomicity_));
        }

        return SendPing();
    }

    void Detach()
    {
        VERIFY_THREAD_AFFINITY(ClientThread);

        if (Type_ != ETransactionType::Master) {
            THROW_ERROR_EXCEPTION("Cannot detach a %Qlv transaction",
                Type_);
        }

        YCHECK(Atomicity_ == EAtomicity::Full);

        {
            TGuard<TSpinLock> guard(SpinLock_);
            switch (State_) {
                case ETransactionState::Committed:
                    THROW_ERROR_EXCEPTION("Transaction is already committed (TransactionId: %v)", Id_);
                    break;

                case ETransactionState::Aborted:
                    THROW_ERROR_EXCEPTION("Transaction is already aborted (TransactionId: %v)", Id_);
                    break;

                case ETransactionState::Active:
                    State_ = ETransactionState::Detached;
                    break;

                case ETransactionState::Detached:
                    return;

                default:
                    YUNREACHABLE();
            }
        }

        LOG_INFO("Transaction detached (TransactionId: %v)",
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
        TGuard<TSpinLock> guard(SpinLock_);
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


    TFuture<void> AddTabletParticipant(const TCellId& cellId)
    {
        VERIFY_THREAD_AFFINITY(ClientThread);
        YCHECK(TypeFromId(cellId) == EObjectType::TabletCell);

        if (Atomicity_ != EAtomicity::Full) {
            return VoidFuture;
        }

        if (TypeFromId(Id_) == EObjectType::NestedTransaction) {
            return MakeFuture(TError("Nested master transactions cannot be used at tablets"));
        }

        TPromise<void> promise;
        {
            TGuard<TSpinLock> guard(SpinLock_);

            if (State_ != ETransactionState::Active) {
                return MakeFuture(TError("Transaction is not active"));
            }

            if (!Error_.IsOK()) {
                THROW_ERROR Error_;
            }

            auto it = CellIdToStartTransactionResult_.find(cellId);
            if (it != CellIdToStartTransactionResult_.end()) {
                return it->second;
            }

            promise = NewPromise<void>();
            YCHECK(CellIdToStartTransactionResult_.insert(std::make_pair(cellId, promise)).second);
        }

        LOG_DEBUG("Adding transaction tablet participant (TransactionId: %v, CellId: %v)",
            Id_,
            cellId);

        auto channel = Owner_->CellDirectory_->GetChannelOrThrow(cellId);
        auto proxy = Owner_->MakeTabletProxy(channel);

        auto req = proxy->StartTransaction();
        ToProto(req->mutable_transaction_id(), Id_);
        req->set_start_timestamp(StartTimestamp_);
        req->set_timeout(ToProto(Timeout_.Get(Owner_->Config_->DefaultTransactionTimeout)));

        req->Invoke().Subscribe(
            BIND(&TImpl::OnTabletParticipantAdded, MakeStrong(this), cellId, promise));

        return promise;
    }


    void SubscribeAborted(const TCallback<void()>& handler)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        Aborted_.ToFuture().Subscribe(BIND([=] (const TError& error) {
            if (error.IsOK()) {
                handler.Run();
            }
        }));
    }

    void UnsubscribeAborted(const TCallback<void()>& handler)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        YUNREACHABLE();
    }

private:
    friend class TTransactionManager::TImpl;

    TIntrusivePtr<TTransactionManager::TImpl> Owner_;
    ETransactionType Type_;
    bool AutoAbort_;
    TNullable<TDuration> PingPeriod_;
    bool Ping_;
    bool PingAncestors_;
    TNullable<TDuration> Timeout_;
    EAtomicity Atomicity_ = EAtomicity::Full;
    EDurability Durability_ = EDurability::Sync;

    TSpinLock SpinLock_;
    ETransactionState State_;
    TPromise<void> Aborted_;
    yhash_map<TCellId, TPromise<void>> CellIdToStartTransactionResult_;
    TError Error_;

    TTimestamp StartTimestamp_ = NullTimestamp;
    TTransactionId Id_;

    DECLARE_THREAD_AFFINITY_SLOT(ClientThread);



    static void ValidateStartOptions(
        ETransactionType type,
        const TTransactionStartOptions& options)
    {
        switch (type)
        {
            case ETransactionType::Master:
                ValidateMasterStartOptions(options);
                break;
            case ETransactionType::Tablet:
                ValidateTabletStartOptions(options);
                break;
            default:
                YUNREACHABLE();
        }
    }

    static void ValidateMasterStartOptions(const TTransactionStartOptions& options)
    {
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
        if (!options.Ping) {
            THROW_ERROR_EXCEPTION("Cannot switch off pings for a tablet transaction");
        }
        if (options.Atomicity == EAtomicity::Full && options.Durability != EDurability::Sync) {
            THROW_ERROR_EXCEPTION("Durability must be %Qlv for tablet transactions with %Qlv atomicity",
                EDurability::Sync,
                EAtomicity::Full);
        }
    }

    static void ValidateAttachOptions(
        const TTransactionId& id,
        const TTransactionAttachOptions& /*options*/)
    {
        ValidateMasterTransactionId(id);
        // No option checks for now.
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

        LOG_INFO("Starting transaction (StartTimestamp: %v, Type: %v)",
            StartTimestamp_,
            Type_);

        switch (Type_) {
            case ETransactionType::Master:
                return StartMasterTransaction(options);
            case ETransactionType::Tablet:
                return StartAtomicTabletTransaction();
            default:
                YUNREACHABLE();
        }
    }

    TFuture<void> StartMasterTransaction(const TTransactionStartOptions& options)
    {
        TObjectServiceProxy proxy(Owner_->MasterChannel_);
        auto req = TMasterYPathProxy::CreateObject();
        req->set_type(static_cast<int>(EObjectType::Transaction));
        if (options.Attributes) {
            ToProto(req->mutable_object_attributes(), *options.Attributes);
        }
        if (options.ParentId) {
            ToProto(req->mutable_transaction_id(), options.ParentId);
        }

        auto* reqExt = req->mutable_extensions()->MutableExtension(NTransactionClient::NProto::TTransactionCreationExt::transaction_creation_ext);
        reqExt->set_timeout(ToProto(options.Timeout.Get(Owner_->Config_->DefaultTransactionTimeout)));

        if (options.ParentId) {
            SetOrGenerateMutationId(req, options.MutationId, options.Retry);
        }

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

        YCHECK(CellIdToStartTransactionResult_.insert(std::make_pair(
            Owner_->CellId_,
            MakePromise<void>(TError()))).second);

        LOG_INFO("Master transaction started (TransactionId: %v, StartTimestamp: %v, AutoAbort: %v, Ping: %v, PingAncestors: %v)",
            Id_,
            StartTimestamp_,
            AutoAbort_,
            Ping_,
            PingAncestors_);

        if (Ping_) {
            RunPeriodicPings();
        }
    }

    TFuture<void> StartAtomicTabletTransaction()
    {
        YCHECK(Atomicity_ == EAtomicity::Full);
        YCHECK(Durability_ == EDurability::Sync);

        Id_ = MakeTabletTransactionId(
            Atomicity_,
            CellTagFromId(Owner_->CellId_),
            StartTimestamp_,
            TabletTransactionHashCounter++);

        State_ = ETransactionState::Active;

        LOG_INFO("Atomic tablet transaction started (TransactionId: %v, StartTimestamp: %v, AutoAbort: %v)",
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

        LOG_INFO("Non-atomic tablet transaction started (TransactionId: %v, Durability: %v)",
            Id_,
            Durability_);

        return VoidFuture;
    }

    void OnTabletParticipantAdded(
        const TCellId& cellId,
        TPromise<void> promise,
        const TTabletServiceProxy::TErrorOrRspStartTransactionPtr& rspOrError)
    {
        if (rspOrError.IsOK()) {
            LOG_DEBUG("Transaction tablet participant added (TransactionId: %v, CellId: %v)",
                Id_,
                cellId);
        } else {
            LOG_DEBUG(rspOrError, "Error adding transaction tablet participant (TransactionId: %v, CellId: %v)",
                Id_,
                cellId);

            DoAbort(TError("Error adding participant %v to transaction %v",
                cellId,
                Id_)
                << rspOrError);
        }

        promise.Set(rspOrError);
    }

    void SetTransactionCommitted()
    {
        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (State_ != ETransactionState::Committing) {
                THROW_ERROR Error_;
            }
            State_ = ETransactionState::Committed;
        }

        LOG_INFO("Transaction committed (TransactionId: %v)",
            Id_);
    }

    void OnAtomicTransactionCommitted(
        const TCellId& cellId,
        const TTransactionSupervisorServiceProxy::TErrorOrRspCommitTransactionPtr& rspOrError)
    {
        if (!rspOrError.IsOK()) {
            auto error = TError("Error committing transaction at cell %v",
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

            auto channel = Owner_->CellDirectory_->GetChannelOrThrow(cellId);
            auto proxy = Owner_->MakeSupervisorProxy(channel);

            auto req = proxy->PingTransaction();
            ToProto(req->mutable_transaction_id(), Id_);

            if (cellId == Owner_->CellId_) {
                auto* reqExt = req->MutableExtension(NProto::TReqPingTransactionExt::ping_transaction_ext);
                reqExt->set_ping_ancestors(PingAncestors_);
            }

            auto asyncRspOrError = req->Invoke();
            asyncResults.push_back(asyncRspOrError.Apply(
                BIND([=, this_ = MakeStrong(this)] (const TTransactionSupervisorServiceProxy::TErrorOrRspPingTransactionPtr& rspOrError) {
                    if (rspOrError.IsOK()) {
                        LOG_DEBUG("Transaction pinged (TransactionId: %v, CellId: %v)",
                            Id_,
                            cellId);
                    } else if (rspOrError.GetCode() == NYTree::EErrorCode::ResolveError && GetState() == ETransactionState::Active) {
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
            if (!channel)
                continue; // better skip
            auto proxy = Owner_->MakeSupervisorProxy(channel);

            auto req = proxy->AbortTransaction();
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
                    } else if (rspOrError.GetCode() == NYTree::EErrorCode::ResolveError) {
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
        Aborted_.Set();
    }

    void DoAbort(const TError& error)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (State_ == ETransactionState::Aborted)
                return;
            State_ = ETransactionState::Aborted;
            Error_ = error;
        }

        FireAborted();
    }


    std::vector<TCellId> GetParticipantIds()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        std::vector<TCellId> result;
        for (const auto& pair : CellIdToStartTransactionResult_) {
            if (pair.second.IsSet()) {
                result.push_back(pair.first);
            }
        }
        return result;
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
    : Impl_(impl)
{ }

TTransactionPtr TTransaction::Create(TIntrusivePtr<TImpl> impl)
{
    return New<TTransaction>(impl);
}

TTransaction::~TTransaction()
{ }

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

TFuture<void> TTransaction::AddTabletParticipant(const TCellId& cellId)
{
    return Impl_->AddTabletParticipant(cellId);
}

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
