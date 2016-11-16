#include "transaction_manager.h"
#include "private.h"
#include "automaton.h"
#include "config.h"
#include "tablet_slot.h"
#include "transaction.h"

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/server/hive/transaction_supervisor.h>
#include <yt/server/hive/transaction_lease_tracker.h>
#include <yt/server/hive/transaction_manager_detail.h>

#include <yt/server/hydra/hydra_manager.h>
#include <yt/server/hydra/mutation.h>

#include <yt/server/tablet_node/transaction_manager.pb.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/timestamp_provider.h>
#include <yt/ytlib/transaction_client/action.h>

#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/native_client.h>

#include <yt/ytlib/tablet_client/tablet_service.pb.h>

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/logging/log.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/misc/heap.h>

#include <set>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NTransactionClient;
using namespace NHydra;
using namespace NHiveServer;
using namespace NCellNode;
using namespace NTabletClient::NProto;

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager::TImpl
    : public TTabletAutomatonPart
    , public TTransactionManagerBase<TTransaction>
{
public:
    DEFINE_SIGNAL(void(TTransaction*), TransactionStarted);
    DEFINE_SIGNAL(void(TTransaction*), TransactionPrepared);
    DEFINE_SIGNAL(void(TTransaction*), TransactionCommitted);
    DEFINE_SIGNAL(void(TTransaction*), TransactionSerialized);
    DEFINE_SIGNAL(void(TTransaction*), TransactionAborted);
    DEFINE_SIGNAL(void(TTransaction*), TransactionTransientReset);

public:
    TImpl(
        TTransactionManagerConfigPtr config,
        TTabletSlotPtr slot,
        NCellNode::TBootstrap* bootstrap)
        : TCompositeAutomatonPart(
            slot->GetHydraManager(),
            slot->GetAutomaton(),
            slot->GetAutomatonInvoker())
        , TTabletAutomatonPart(
            slot,
            bootstrap)
        , TTransactionManagerBase(
            slot->GetHydraManager(),
            slot->GetAutomaton(),
            slot->GetAutomatonInvoker())
        , Config_(config)
        , LeaseTracker_(New<TTransactionLeaseTracker>(
            Slot_->GetAutomatonInvoker(),
            Logger))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Slot_->GetAutomatonInvoker(), AutomatonThread);

        RegisterLoader(
            "TransactionManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "TransactionManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));
        RegisterLoader(
            "TransactionManager.Async",
            BIND(&TImpl::LoadAsync, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "TransactionManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "TransactionManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));
        RegisterSaver(
            EAsyncSerializationPriority::Default,
            "TransactionManager.Async",
            BIND(&TImpl::SaveAsync, Unretained(this)));

        RegisterMethod(BIND(&TImpl::HydraRegisterTransactionActions, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraHandleTransactionBarrier, Unretained(this)));

        OrchidService_ = IYPathService::FromProducer(BIND(&TImpl::BuildOrchidYson, MakeWeak(this)))
            ->Via(Slot_->GetGuardedAutomatonInvoker())
            ->Cached(TDuration::Seconds(1));
    }

    TMutationPtr CreateRegisterTransactionActionsMutation(
        TCtxRegisterTransactionActionsPtr context)
    {
        return CreateMutation(HydraManager_, std::move(context));
    }

    TTransaction* GetTransactionOrThrow(const TTransactionId& transactionId)
    {
        if (auto* transaction = TransientTransactionMap_.Find(transactionId)) {
            return transaction;
        }
        if (auto* transaction = PersistentTransactionMap_.Find(transactionId)) {
            return transaction;
        }
        THROW_ERROR_EXCEPTION(
            NTransactionClient::EErrorCode::NoSuchTransaction,
            "No such transaction %v",
            transactionId);
    }

    TTransaction* GetOrCreateTransaction(
        const TTransactionId& transactionId,
        TTimestamp startTimestamp,
        TDuration timeout,
        bool transient)
    {
        if (auto* transaction = TransientTransactionMap_.Find(transactionId)) {
            return transaction;
        }
        if (auto* transaction = PersistentTransactionMap_.Find(transactionId)) {
            return transaction;
        }

        auto transactionHolder = std::make_unique<TTransaction>(transactionId);
        transactionHolder->SetTimeout(timeout);
        transactionHolder->SetStartTimestamp(startTimestamp);
        transactionHolder->SetState(ETransactionState::Active);
        transactionHolder->SetTransient(transient);

        auto& map = transient ? TransientTransactionMap_ : PersistentTransactionMap_;
        auto* transaction = map.Insert(transactionId, std::move(transactionHolder));

        if (!transient && IsLeader()) {
            CreateLeases(transaction);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Transaction started (TransactionId: %v, StartTimestamp: %v, StartTime: %v, "
            "Timeout: %v, Transient: %v)",
            transactionId,
            startTimestamp,
            TimestampToInstant(startTimestamp).first,
            timeout,
            transient);

        return transaction;
    }

    TTransaction* MakeTransactionPersistent(const TTransactionId& transactionId)
    {
        if (auto* transaction = TransientTransactionMap_.Find(transactionId)) {
            transaction->SetTransient(false);
            CreateLeases(transaction);
            auto transactionHolder = TransientTransactionMap_.Release(transactionId);
            PersistentTransactionMap_.Insert(transactionId, std::move(transactionHolder));
            LOG_DEBUG_UNLESS(IsRecovery(), "Transaction became persistent (TransactionId: %v)",
                transactionId);
            return transaction;
        }

        if (auto* transaction = PersistentTransactionMap_.Find(transactionId)) {
            YCHECK(!transaction->GetTransient());
            return transaction;
        }

        Y_UNREACHABLE();
    }

    std::vector<TTransaction*> GetTransactions()
    {
        std::vector<TTransaction*> transactions;
        for (const auto& pair : TransientTransactionMap_) {
            transactions.push_back(pair.second);
        }
        for (const auto& pair : PersistentTransactionMap_) {
            transactions.push_back(pair.second);
        }
        return transactions;
    }

    IYPathServicePtr GetOrchidService()
    {
        return OrchidService_;
    }


    // ITransactionManager implementation.
    void PrepareTransactionCommit(
        const TTransactionId& transactionId,
        bool persistent,
        TTimestamp prepareTimestamp)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTransaction* transaction;
        ETransactionState state;
        TTransactionSignature signature;
        if (persistent) {
            transaction = GetPersistentTransactionOrThrow(transactionId);
            state = transaction->GetPersistentState();
            signature = transaction->GetPersistentSignature();
        } else {
            transaction = GetTransactionOrThrow(transactionId);
            state = transaction->GetState();
            signature = transaction->GetTransientSignature();
        }

        // Allow preparing transactions in Active and TransientCommitPrepared (for persistent mode) states.
        if (state != ETransactionState::Active &&
            !(persistent && state == ETransactionState::TransientCommitPrepared))
        {
            transaction->ThrowInvalidState();
        }

        if (signature != FinalTransactionSignature) {
            THROW_ERROR_EXCEPTION("Transaction %v is incomplete: expected signature %x, actual signature %x",
                transactionId,
                FinalTransactionSignature,
                signature);
        }

        if (state == ETransactionState::Active) {
            UpdatePrepareTimestamp(transaction, prepareTimestamp);

            transaction->SetState(persistent
                ? ETransactionState::PersistentCommitPrepared
                : ETransactionState::TransientCommitPrepared);

            TransactionPrepared_.Fire(transaction);
            RunPrepareTransactionActions(transaction, persistent);

            LOG_DEBUG_UNLESS(IsRecovery(), "Transaction commit prepared (TransactionId: %v, Persistent: %v, "
                "PrepareTimestamp: %v)",
                transactionId,
                persistent,
                prepareTimestamp);
        }
    }

    void PrepareTransactionAbort(const TTransactionId& transactionId, bool force)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetTransactionOrThrow(transactionId);

        if (!transaction->IsActive() && !force) {
            transaction->ThrowInvalidState();
        }

        if (transaction->IsActive()) {
            transaction->SetState(ETransactionState::TransientAbortPrepared);

            LOG_DEBUG("Transaction abort prepared (TransactionId: %v)",
                transactionId);
        }
    }

    void CommitTransaction(const TTransactionId& transactionId, TTimestamp commitTimestamp)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetPersistentTransactionOrThrow(transactionId);

        auto state = transaction->GetPersistentState();
        if (state == ETransactionState::Committed) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Transaction is already committed (TransactionId: %v)",
                transactionId);
            return;
        }

        if (state != ETransactionState::Active &&
            state != ETransactionState::PersistentCommitPrepared)
        {
            transaction->ThrowInvalidState();
        }

        if (IsLeader()) {
            CloseLeases(transaction);
        }

        transaction->SetCommitTimestamp(commitTimestamp);
        transaction->SetState(ETransactionState::Committed);

        TransactionCommitted_.Fire(transaction);
        RunCommitTransactionActions(transaction);

        LOG_DEBUG_UNLESS(IsRecovery(), "Transaction committed (TransactionId: %v, CommitTimestamp: %v)",
            transactionId,
            commitTimestamp);

        SerializingTransactionHeap_.push_back(transaction);
        AdjustHeapBack(SerializingTransactionHeap_.begin(), SerializingTransactionHeap_.end(), SerializingTransactionHeapComparer);

        FinishTransaction(transaction);
    }

    void AbortTransaction(const TTransactionId& transactionId, bool force)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetPersistentTransactionOrThrow(transactionId);

        auto state = transaction->GetPersistentState();
        if (state == ETransactionState::PersistentCommitPrepared && !force) {
            transaction->ThrowInvalidState();
        }

        if (IsLeader()) {
            CloseLeases(transaction);
        }

        transaction->SetState(ETransactionState::Aborted);

        TransactionAborted_.Fire(transaction);
        RunAbortTransactionActions(transaction);

        LOG_DEBUG_UNLESS(IsRecovery(), "Transaction aborted (TransactionId: %v, Force: %v)",
            transactionId,
            force);

        FinishTransaction(transaction);
        PersistentTransactionMap_.Remove(transactionId);
    }

    void PingTransaction(const TTransactionId& transactionId, bool pingAncestors)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        LeaseTracker_->PingTransaction(transactionId, pingAncestors);
    }

    TTimestamp GetMinPrepareTimestamp() const
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (PreparedTransactions_.empty()) {
            const auto& timestampProvider = Bootstrap_
                ->GetMasterClient()
                ->GetConnection()
                ->GetTimestampProvider();
            return timestampProvider->GetLatestTimestamp();
        } else {
            return PreparedTransactions_.begin()->first;
        }
    }

private:
    const TTransactionManagerConfigPtr Config_;
    const TTransactionLeaseTrackerPtr LeaseTracker_;

    TEntityMap<TTransaction> TransientTransactionMap_;
    std::vector<TTransaction*> SerializingTransactionHeap_;
    TTimestamp LastSerializedCommitTimestamp_ = MinTimestamp;
    TTimestamp TransientBarrierTimestamp_ = MinTimestamp;

    IYPathServicePtr OrchidService_;

    std::set<std::pair<TTimestamp, TTransaction*>> PreparedTransactions_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void BuildOrchidYson(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto dumpTransaction = [&] (TFluentMap fluent, const std::pair<TTransactionId, TTransaction*>& pair) {
            auto* transaction = pair.second;
            fluent
                .Item(ToString(transaction->GetId())).BeginMap()
                    .Item("transient").Value(transaction->GetTransient())
                    .Item("timeout").Value(transaction->GetTimeout())
                    .Item("state").Value(transaction->GetState())
                    .Item("start_timestamp").Value(transaction->GetStartTimestamp())
                    .Item("prepare_timestamp").Value(transaction->GetPrepareTimestamp())
                    // Omit CommitTimestamp, it's typically null.
                    .Item("locked_row_count").Value(transaction->LockedSortedRows().size())
                .EndMap();
        };
        BuildYsonFluently(consumer)
            .BeginMap()
                .DoFor(TransientTransactionMap_, dumpTransaction)
                .DoFor(PersistentTransactionMap_, dumpTransaction)
            .EndMap();
    }

    void CreateLeases(TTransaction* transaction)
    {
        YCHECK(!transaction->GetTransient());

        auto invoker = Slot_->GetEpochAutomatonInvoker();

        LeaseTracker_->RegisterTransaction(
            transaction->GetId(),
            NullTransactionId,
            transaction->GetTimeout(),
            BIND(&TImpl::OnTransactionExpired, MakeStrong(this))
                .Via(invoker));

        auto startInstants = TimestampToInstant(transaction->GetStartTimestamp());
        auto deadline = startInstants.first + Config_->MaxTransactionDuration;

        transaction->TimeoutCookie() = TDelayedExecutor::Submit(
            BIND(&TImpl::OnTransactionTimedOut, MakeStrong(this), transaction->GetId())
                .Via(invoker),
            deadline);
    }

    void CloseLeases(TTransaction* transaction)
    {
        LeaseTracker_->UnregisterTransaction(transaction->GetId());

        TDelayedExecutor::CancelAndClear(transaction->TimeoutCookie());
    }


    void OnTransactionExpired(const TTransactionId& id)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = FindPersistentTransaction(id);
        if (!transaction) {
            return;
        }

        if (transaction->GetState() != ETransactionState::Active) {
            return;
        }

        const auto& transactionSupervisor = Slot_->GetTransactionSupervisor();
        transactionSupervisor->AbortTransaction(id).Subscribe(BIND([=] (const TError& error) {
            if (!error.IsOK()) {
                LOG_DEBUG(error, "Error aborting expired transaction (TransactionId: %v)",
                    id);
            }
        }));
    }

    void OnTransactionTimedOut(const TTransactionId& id)
    {
        auto* transaction = FindPersistentTransaction(id);
        if (!transaction) {
            return;
        }

        if (transaction->GetState() != ETransactionState::Active) {
            return;
        }

        LOG_DEBUG("Transaction timed out (TransactionId: %v)",
            id);

        const auto& transactionSupervisor = Slot_->GetTransactionSupervisor();
        transactionSupervisor->AbortTransaction(id).Subscribe(BIND([=] (const TError& error) {
            if (!error.IsOK()) {
                LOG_DEBUG(error, "Error aborting timed out transaction (TransactionId: %v)",
                    id);
            }
        }));
    }

    void FinishTransaction(TTransaction* transaction)
    {
        ErasePrepareTimestamp(transaction);
    }


    virtual void OnAfterSnapshotLoaded() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnAfterSnapshotLoaded();

        SerializingTransactionHeap_.clear();
        for (const auto& pair : PersistentTransactionMap_) {
            auto* transaction = pair.second;
            if (transaction->GetState() == ETransactionState::Committed) {
                SerializingTransactionHeap_.push_back(transaction);
            }
            InitPrepareTimestamp(transaction);
        }
        MakeHeap(SerializingTransactionHeap_.begin(), SerializingTransactionHeap_.end(), SerializingTransactionHeapComparer);
    }

    virtual void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnLeaderActive();

        YCHECK(TransientTransactionMap_.GetSize() == 0);

        // Recreate leases for all active transactions.
        for (const auto& pair : PersistentTransactionMap_) {
            auto* transaction = pair.second;
            if (transaction->GetState() == ETransactionState::Active ||
                transaction->GetState() == ETransactionState::PersistentCommitPrepared)
            {
                CreateLeases(transaction);
            }
        }

        TransientBarrierTimestamp_ = MinTimestamp;
        MaybeCommitBarrier();

        LeaseTracker_->Start();
    }

    virtual void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnStopLeading();

        LeaseTracker_->Stop();

        for (const auto& pair : TransientTransactionMap_) {
            auto* transaction = pair.second;
            transaction->ResetFinished();
            TransactionTransientReset_.Fire(transaction);
        }
        TransientTransactionMap_.Clear();

        // Reset all transiently prepared persistent transactions back into active state.
        // Mark all transactions as finished to release pending readers.
        for (const auto& pair : PersistentTransactionMap_) {
            auto* transaction = pair.second;
            transaction->SetState(transaction->GetPersistentState());
            transaction->SetTransientSignature(transaction->GetPersistentSignature());
            transaction->ResetFinished();
            TransactionTransientReset_.Fire(transaction);
        }
    }


    void SaveKeys(TSaveContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        PersistentTransactionMap_.SaveKeys(context);
    }

    void SaveValues(TSaveContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        using NYT::Save;
        PersistentTransactionMap_.SaveValues(context);
        Save(context, LastSerializedCommitTimestamp_);
    }

    TCallback<void(TSaveContext&)> SaveAsync()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        std::vector<std::pair<TTransactionId, TCallback<void(TSaveContext&)>>> capturedTransactions;
        for (const auto& pair : PersistentTransactionMap_) {
            auto* transaction = pair.second;
            capturedTransactions.push_back(std::make_pair(transaction->GetId(), transaction->AsyncSave()));
        }

        return BIND([capturedTransactions = std::move(capturedTransactions)] (TSaveContext& context) {
                using NYT::Save;
                // NB: This is not stable.
                for (const auto& pair : capturedTransactions) {
                    Save(context, pair.first);
                    pair.second.Run(context);
                }
            });
    }


    void LoadKeys(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        PersistentTransactionMap_.LoadKeys(context);
    }

    void LoadValues(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        using NYT::Load;
        PersistentTransactionMap_.LoadValues(context);
        Load(context, LastSerializedCommitTimestamp_);
    }

    void LoadAsync(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        SERIALIZATION_DUMP_WRITE(context, "transactions[%v]", PersistentTransactionMap_.size());
        SERIALIZATION_DUMP_INDENT(context) {
            for (int index = 0; index < PersistentTransactionMap_.size(); ++index) {
                auto transactionId = Load<TTransactionId>(context);
                SERIALIZATION_DUMP_WRITE(context, "%v =>", transactionId);
                SERIALIZATION_DUMP_INDENT(context) {
                    auto* transaction = GetPersistentTransaction(transactionId);
                    transaction->AsyncLoad(context);
                }
            }
        }
    }


    virtual void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::Clear();

        TransientTransactionMap_.Clear();
        PersistentTransactionMap_.Clear();
        SerializingTransactionHeap_.clear();
        PreparedTransactions_.clear();
        LastSerializedCommitTimestamp_ = MinTimestamp;
    }


    void HydraRegisterTransactionActions(NTabletClient::NProto::TReqRegisterTransactionActions* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto transactionStartTimestamp = request->transaction_start_timestamp();
        auto transactionTimeout = FromProto<TDuration>(request->transaction_timeout());
        auto signature = request->signature();

        auto* transaction = GetOrCreateTransaction(
            transactionId,
            transactionStartTimestamp,
            transactionTimeout,
            false);

        auto state = transaction->GetPersistentState();
        if (state != ETransactionState::Active) {
            transaction->ThrowInvalidState();
        }

        for (const auto& protoData : request->actions()) {
            auto data = FromProto<TTransactionActionData>(protoData);
            transaction->Actions().push_back(data);

            LOG_DEBUG_UNLESS(IsRecovery(), "Transaction action registered (TransactionId: %v, ActionType: %v)",
                transactionId,
                data.Type);
        }

        transaction->SetPersistentSignature(transaction->GetPersistentSignature() + signature);
    }

    void HydraHandleTransactionBarrier(NTabletNode::NProto::TReqHandleTransactionBarrier* request)
    {
        auto barrierTimestamp = request->timestamp();

        LOG_DEBUG("Handling transaction barrier (Timestamp: %v)",
            barrierTimestamp);

        while (!SerializingTransactionHeap_.empty()) {
            auto* transaction = SerializingTransactionHeap_.front();
            auto commitTimestamp = transaction->GetCommitTimestamp();
            if (commitTimestamp > barrierTimestamp) {
                break;
            }

            YCHECK(commitTimestamp > LastSerializedCommitTimestamp_);
            LastSerializedCommitTimestamp_ = commitTimestamp;

            const auto& transactionId = transaction->GetId();
            LOG_DEBUG_UNLESS(IsRecovery(), "Transaction serialized (TransactionId: %v, CommitTimestamp: %v)",
                transaction->GetId(),
                commitTimestamp);

            transaction->SetState(ETransactionState::Serialized);
            TransactionSerialized_.Fire(transaction);

            PersistentTransactionMap_.Remove(transactionId);

            ExtractHeap(SerializingTransactionHeap_.begin(), SerializingTransactionHeap_.end(), SerializingTransactionHeapComparer);
            SerializingTransactionHeap_.pop_back();
        }
    }


    void MaybeCommitBarrier()
    {
        if (!IsLeader()) {
            return;
        }

        auto minPrepareTimestamp = GetMinPrepareTimestamp();
        if (minPrepareTimestamp <= TransientBarrierTimestamp_) {
            return;
        }

        TransientBarrierTimestamp_ = minPrepareTimestamp;

        LOG_DEBUG("Committing transaction barrier (Timestamp: %v)",
            TransientBarrierTimestamp_);

        NTabletNode::NProto::TReqHandleTransactionBarrier request;
        request.set_timestamp(TransientBarrierTimestamp_);
        CreateMutation(HydraManager_, request)
            ->CommitAndLog(Logger);
    }

    void InitPrepareTimestamp(TTransaction* transaction)
    {
        if (transaction->IsPrepared() && !transaction->IsCommitted()) {
            PreparedTransactions_.emplace(transaction->GetPrepareTimestamp(), transaction);
        }
    }

    void UpdatePrepareTimestamp(TTransaction* transaction, TTimestamp prepareTimestamp)
    {
        ErasePrepareTimestamp(transaction);
        transaction->SetPrepareTimestamp(prepareTimestamp);
        PreparedTransactions_.emplace(transaction->GetPrepareTimestamp(), transaction);
    }

    void ErasePrepareTimestamp(TTransaction* transaction)
    {
        auto pair = std::make_pair(transaction->GetPrepareTimestamp(), transaction);
        auto it = PreparedTransactions_.find(pair);
        if (it != PreparedTransactions_.end()) {
            PreparedTransactions_.erase(it);
        } else {
            YCHECK(transaction->GetPrepareTimestamp() == NullTimestamp);
        }
        MaybeCommitBarrier();
    }

    static bool SerializingTransactionHeapComparer(
        const TTransaction* lhs,
        const TTransaction* rhs)
    {
        Y_ASSERT(lhs->IsCommitted());
        Y_ASSERT(rhs->IsCommitted());
        return lhs->GetCommitTimestamp() < rhs->GetCommitTimestamp();
    }
};

////////////////////////////////////////////////////////////////////////////////

TTransactionManager::TTransactionManager(
    TTransactionManagerConfigPtr config,
    TTabletSlotPtr slot,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(
        config,
        slot,
        bootstrap))
{ }

TTransactionManager::~TTransactionManager() = default;

TMutationPtr TTransactionManager::CreateRegisterTransactionActionsMutation(
    TCtxRegisterTransactionActionsPtr context)
{
    return Impl_->CreateRegisterTransactionActionsMutation(std::move(context));
}

IYPathServicePtr TTransactionManager::GetOrchidService()
{
    return Impl_->GetOrchidService();
}

TTransaction* TTransactionManager::GetOrCreateTransaction(
    const TTransactionId& transactionId,
    TTimestamp startTimestamp,
    TDuration timeout,
    bool transient)
{
    return Impl_->GetOrCreateTransaction(
        transactionId,
        startTimestamp,
        timeout,
        transient);
}

TTransaction* TTransactionManager::MakeTransactionPersistent(const TTransactionId& transactionId)
{
    return Impl_->MakeTransactionPersistent(transactionId);
}

std::vector<TTransaction*> TTransactionManager::GetTransactions()
{
    return Impl_->GetTransactions();
}

void TTransactionManager::RegisterPrepareActionHandler(
    const TTransactionPrepareActionHandlerDescriptor<TTransaction>& descriptor)
{
    Impl_->RegisterPrepareActionHandler(descriptor);
}

void TTransactionManager::RegisterCommitActionHandler(
    const TTransactionCommitActionHandlerDescriptor<TTransaction>& descriptor)
{
    Impl_->RegisterCommitActionHandler(descriptor);
}

void TTransactionManager::RegisterAbortActionHandler(
    const TTransactionAbortActionHandlerDescriptor<TTransaction>& descriptor)
{
    Impl_->RegisterAbortActionHandler(descriptor);
}

void TTransactionManager::PrepareTransactionCommit(
    const TTransactionId& transactionId,
    bool persistent,
    TTimestamp prepareTimestamp)
{
    Impl_->PrepareTransactionCommit(transactionId, persistent, prepareTimestamp);
}

void TTransactionManager::PrepareTransactionAbort(const TTransactionId& transactionId, bool force)
{
    Impl_->PrepareTransactionAbort(transactionId, force);
}

void TTransactionManager::CommitTransaction(const TTransactionId& transactionId, TTimestamp commitTimestamp)
{
    Impl_->CommitTransaction(transactionId, commitTimestamp);
}

void TTransactionManager::AbortTransaction(const TTransactionId& transactionId, bool force)
{
    Impl_->AbortTransaction(transactionId, force);
}

void TTransactionManager::PingTransaction(const TTransactionId& transactionId, bool pingAncestors)
{
    Impl_->PingTransaction(transactionId, pingAncestors);
}

TTimestamp TTransactionManager::GetMinPrepareTimestamp()
{
    return Impl_->GetMinPrepareTimestamp();
}

DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionStarted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionPrepared, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionCommitted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionSerialized, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionAborted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionTransientReset, *Impl_)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
