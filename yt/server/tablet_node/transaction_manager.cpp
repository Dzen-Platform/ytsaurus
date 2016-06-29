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

#include <yt/server/hydra/hydra_manager.h>
#include <yt/server/hydra/mutation.h>

#include <yt/ytlib/transaction_client/helpers.h>

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/logging/log.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NTransactionClient;
using namespace NHydra;
using namespace NHive;
using namespace NHive::NProto;
using namespace NCellNode;
using namespace NTabletClient::NProto;

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager::TImpl
    : public TTabletAutomatonPart
{
public:
    DEFINE_SIGNAL(void(TTransaction*), TransactionStarted);
    DEFINE_SIGNAL(void(TTransaction*), TransactionPrepared);
    DEFINE_SIGNAL(void(TTransaction*), TransactionCommitted);
    DEFINE_SIGNAL(void(TTransaction*), TransactionAborted);

public:
    TImpl(
        TTransactionManagerConfigPtr config,
        TTabletSlotPtr slot,
        NCellNode::TBootstrap* bootstrap)
        : TTabletAutomatonPart(
            slot,
            bootstrap)
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

        RegisterMethod(BIND(&TImpl::HydraStartTransaction, Unretained(this)));

        OrchidService_ = IYPathService::FromProducer(BIND(&TImpl::BuildOrchidYson, MakeStrong(this)))
            ->Via(Slot_->GetAutomatonInvoker())
            ->Cached(TDuration::Seconds(1));
    }


    TMutationPtr CreateStartTransactionMutation(const TReqStartTransaction& request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return CreateMutation(
            Slot_->GetHydraManager(),
            request,
            &TImpl::HydraStartTransaction,
            this);
    }

    TTransaction* GetTransactionOrThrow(const TTransactionId& id)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = FindTransaction(id);
        if (!transaction) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No such transaction %v",
                id);
        }
        return transaction;
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

        auto* transaction = GetTransactionOrThrow(transactionId);

        // Allow preparing transactions in Active and TransientCommitPrepared (for persistent mode) states.
        auto state = persistent ? transaction->GetPersistentState() : transaction->GetState();
        if (state != ETransactionState::Active &&
            (!persistent || state != ETransactionState::TransientCommitPrepared))
        {
            transaction->ThrowInvalidState();
        }

        transaction->SetState(persistent
            ? ETransactionState::PersistentCommitPrepared
            : ETransactionState::TransientCommitPrepared);

        if (state == ETransactionState::Active) {
            transaction->SetPrepareTimestamp(prepareTimestamp);
            TransactionPrepared_.Fire(transaction);

            LOG_DEBUG_UNLESS(IsRecovery(), "Transaction commit prepared (TransactionId: %v, Persistent: %v, PrepareTimestamp: %v)",
                transactionId,
                persistent,
                prepareTimestamp);
        }
    }

    void PrepareTransactionAbort(const TTransactionId& transactionId, bool force)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetTransactionOrThrow(transactionId);

        auto state = transaction->GetState();
        if (state != ETransactionState::Active && !force) {
            transaction->ThrowInvalidState();
        }

        if (state == ETransactionState::Active) {
            transaction->SetState(ETransactionState::TransientAbortPrepared);

            LOG_DEBUG("Transaction abort prepared (TransactionId: %v)",
                transactionId);
        }
    }

    void CommitTransaction(const TTransactionId& transactionId, TTimestamp commitTimestamp)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetTransactionOrThrow(transactionId);

        auto state = transaction->GetPersistentState();
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

        FinishTransaction(transaction);

        LOG_DEBUG_UNLESS(IsRecovery(), "Transaction committed (TransactionId: %v, CommitTimestamp: %v)",
            transactionId,
            commitTimestamp);
    }

    void AbortTransaction(const TTransactionId& transactionId, bool force)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetTransactionOrThrow(transactionId);

        auto state = transaction->GetPersistentState();
        if (state == ETransactionState::PersistentCommitPrepared && !force) {
            transaction->ThrowInvalidState();
        }

        if (IsLeader()) {
            CloseLeases(transaction);
        }

        transaction->SetState(ETransactionState::Aborted);

        TransactionAborted_.Fire(transaction);

        FinishTransaction(transaction);

        LOG_DEBUG_UNLESS(IsRecovery(), "Transaction aborted (TransactionId: %v, Force: %v)",
            transactionId,
            force);
    }

    void PingTransaction(const TTransactionId& transactionId, bool pingAncestors)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        LeaseTracker_->PingTransaction(transactionId, pingAncestors);
    }


    DECLARE_ENTITY_MAP_ACCESSORS(Transaction, TTransaction);

private:
    const TTransactionManagerConfigPtr Config_;
    const TTransactionLeaseTrackerPtr LeaseTracker_;

    TEntityMap<TTransaction> TransactionMap_;

    IYPathServicePtr OrchidService_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void BuildOrchidYson(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        BuildYsonFluently(consumer)
            .DoMapFor(TransactionMap_, [&] (TFluentMap fluent, const std::pair<TTransactionId, TTransaction*>& pair) {
                auto* transaction = pair.second;
                fluent
                    .Item(ToString(transaction->GetId())).BeginMap()
                        .Item("timeout").Value(transaction->GetTimeout())
                        .Item("register_time").Value(transaction->GetRegisterTime())
                        .Item("state").Value(transaction->GetState())
                        .Item("start_timestamp").Value(transaction->GetStartTimestamp())
                        .Item("prepare_timestamp").Value(transaction->GetPrepareTimestamp())
                        // Omit CommitTimestamp, it's typically null.
                        .Item("locked_row_count").Value(transaction->LockedSortedRows().size())
                    .EndMap();
            });
    }

    void CreateLeases(TTransaction* transaction)
    {
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

        auto* transaction = FindTransaction(id);
        if (!transaction)
            return;

        if (transaction->GetState() != ETransactionState::Active)
            return;

        LOG_DEBUG("Transaction lease expired (TransactionId: %v)",
            id);

        auto transactionSupervisor = Slot_->GetTransactionSupervisor();
        transactionSupervisor->AbortTransaction(id).Subscribe(BIND([=] (const TError& error) {
            if (!error.IsOK()) {
                LOG_DEBUG(error, "Error aborting expired transaction (TransactionId: %v)",
                    id);
            }
        }));
    }

    void OnTransactionTimedOut(const TTransactionId& id)
    {
        auto* transaction = FindTransaction(id);
        if (!transaction)
            return;

        if (transaction->GetState() != ETransactionState::Active)
            return;

        LOG_DEBUG("Transaction timed out (TransactionId: %v)",
            id);

        auto transactionSupervisor = Slot_->GetTransactionSupervisor();
        transactionSupervisor->AbortTransaction(id).Subscribe(BIND([=] (const TError& error) {
            if (!error.IsOK()) {
                LOG_DEBUG(error, "Error aborting timed out transaction (TransactionId: %v)",
                    id);
            }
        }));
    }

    void FinishTransaction(TTransaction* transaction)
    {
        transaction->SetFinished();
        TransactionMap_.Remove(transaction->GetId());
    }


    // Hydra handlers.
    void HydraStartTransaction(TReqStartTransaction* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        if (TransactionMap_.Contains(transactionId)) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Transaction is already started, request ignored (TransactionId: %v)",
                transactionId);
            return;
        }

        auto startTimestamp = TTimestamp(request->start_timestamp());
        auto timeout = FromProto<TDuration>(request->timeout());

        auto transactionHolder = std::make_unique<TTransaction>(transactionId);
        auto* transaction = TransactionMap_.Insert(transactionId, std::move(transactionHolder));

        const auto* mutationContext = GetCurrentMutationContext();

        transaction->SetTimeout(timeout);
        transaction->SetStartTimestamp(startTimestamp);
        transaction->SetRegisterTime(mutationContext->GetTimestamp());
        transaction->SetState(ETransactionState::Active);

        LOG_DEBUG_UNLESS(IsRecovery(), "Transaction started (TransactionId: %v, StartTimestamp: %v, StartTime: %v, Timeout: %v)",
            transactionId,
            startTimestamp,
            TimestampToInstant(startTimestamp).first,
            timeout);

        if (IsLeader()) {
            CreateLeases(transaction);
        }
    }


    virtual void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnLeaderActive();

        // Recreate leases for all active transactions.
        for (const auto& pair : TransactionMap_) {
            auto* transaction = pair.second;
            if (transaction->GetState() == ETransactionState::Active ||
                transaction->GetState() == ETransactionState::PersistentCommitPrepared)
            {
                CreateLeases(transaction);
            }
        }

        LeaseTracker_->Start();
    }

    virtual void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::OnStopLeading();

        LeaseTracker_->Stop();

        // Reset all transiently prepared transactions back into active state.
        // Mark all transactions as finished to release pending readers.
        for (const auto& pair : TransactionMap_) {
            auto* transaction = pair.second;
            transaction->SetState(transaction->GetPersistentState());
            transaction->ResetFinished();
        }
    }


    void SaveKeys(TSaveContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TransactionMap_.SaveKeys(context);
    }

    void SaveValues(TSaveContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TransactionMap_.SaveValues(context);
    }

    TCallback<void(TSaveContext&)> SaveAsync()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        std::vector<std::pair<TTransactionId, TCallback<void(TSaveContext&)>>> capturedTransactions;
        for (const auto& pair : TransactionMap_) {
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

        TransactionMap_.LoadKeys(context);
    }

    void LoadValues(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TransactionMap_.LoadValues(context);
    }

    void LoadAsync(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        SERIALIZATION_DUMP_WRITE(context, "transactions[%v]", TransactionMap_.size());
        SERIALIZATION_DUMP_INDENT(context) {
            for (int index = 0; index < TransactionMap_.size(); ++index) {
                auto transactionId = Load<TTransactionId>(context);
                SERIALIZATION_DUMP_WRITE(context, "%v =>", transactionId);
                SERIALIZATION_DUMP_INDENT(context) {
                    auto* transaction = GetTransaction(transactionId);
                    transaction->AsyncLoad(context);
                }
            }
        }
    }


    virtual void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TTabletAutomatonPart::Clear();

        TransactionMap_.Clear();
    }

};

DEFINE_ENTITY_MAP_ACCESSORS(TTransactionManager::TImpl, Transaction, TTransaction, TransactionMap_)

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

TTransactionManager::~TTransactionManager()
{ }

TMutationPtr TTransactionManager::CreateStartTransactionMutation(
    const TReqStartTransaction& request)
{
    return Impl_->CreateStartTransactionMutation(request);
}

TTransaction* TTransactionManager::GetTransactionOrThrow(const TTransactionId& id)
{
    return Impl_->GetTransactionOrThrow(id);
}

IYPathServicePtr TTransactionManager::GetOrchidService()
{
    return Impl_->GetOrchidService();
}

void TTransactionManager::PrepareTransactionCommit(
    const TTransactionId& transactionId,
    bool persistent,
    TTimestamp prepareTimestamp)
{
    Impl_->PrepareTransactionCommit(
        transactionId,
        persistent,
        prepareTimestamp);
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

DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionStarted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionPrepared, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionCommitted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionAborted, *Impl_);
DELEGATE_ENTITY_MAP_ACCESSORS(TTransactionManager, Transaction, TTransaction, *Impl_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
