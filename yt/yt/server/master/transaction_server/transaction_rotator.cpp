#include "transaction_rotator.h"

#include "transaction.h"
#include "transaction_manager.h"

#include <yt/yt/server/master/cell_master/serialize.h>

namespace NYT::NTransactionServer {

using namespace NCellMaster;
using namespace NYTree;

using NHydra::HasHydraContext;

////////////////////////////////////////////////////////////////////////////////

TTransactionRotator::TTransactionRotator(
    TBootstrap* bootstrap,
    TString transactionTitle)
    : Bootstrap_(bootstrap)
    , TransactionTitle_(std::move(transactionTitle))
{
    YT_VERIFY(Bootstrap_);
}

bool TTransactionRotator::IsTransactionAlive() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return Transaction_.IsAlive();
}

void TTransactionRotator::Clear()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    PreviousTransaction_.Reset();
    Transaction_.Reset();
}

void TTransactionRotator::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    // COMPAT(kvk1920)
    if (context.IsLoad() && context.GetVersion() < EMasterReign::TransactionRotator) {
        Persist(context, CompatTransactionId_);
        Persist(context, CompatPreviousTransactionId_);
        NeedInitializeTransactionPtr_ = true;
        return;
    }

    Persist(context, Transaction_);
    Persist(context, PreviousTransaction_);
}

void TTransactionRotator::OnAfterSnapshotLoaded()
{
    if (!NeedInitializeTransactionPtr_) {
        return;
    }

    const auto& transactionManager = Bootstrap_->GetTransactionManager();

    Transaction_.Assign(transactionManager->FindTransaction(CompatTransactionId_));
    PreviousTransaction_.Assign(transactionManager->FindTransaction(CompatPreviousTransactionId_));
}

void TTransactionRotator::Rotate()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YT_VERIFY(HasHydraContext());

    const auto& transactionManager = Bootstrap_->GetTransactionManager();

    if (PreviousTransaction_.IsAlive()) {
        transactionManager->CommitTransaction(
            PreviousTransaction_.Get(),
            /*commitOptions*/ {});
    }
    PreviousTransaction_.Reset();

    PreviousTransaction_ = std::move(Transaction_);

    Transaction_.Assign(transactionManager->StartTransaction(
        /*parent*/ nullptr,
        /*prerequisiteTransactions*/ {},
        /*replicatedToCellTags*/ {},
        /*timeout*/ std::nullopt,
        /*deadline*/ std::nullopt,
        TransactionTitle_,
        EmptyAttributes()));
}

TTransactionId TTransactionRotator::TransactionIdFromPtr(const TTransactionWeakPtr& ptr)
{
    return ptr.IsAlive() ? ptr->GetId() : NullTransactionId;
}

TTransactionId TTransactionRotator::GetTransactionId() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return TransactionIdFromPtr(Transaction_);
}

TTransaction* TTransactionRotator::GetTransaction() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return Transaction_.Get();
}

TTransactionId TTransactionRotator::GetPreviousTransactionId() const
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return TransactionIdFromPtr(PreviousTransaction_);
}

bool TTransactionRotator::OnTransactionFinished(TTransaction* transaction)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (PreviousTransaction_.Get() != transaction &&
        Transaction_.Get() != transaction)
    {
        return false;
    }

    if (Transaction_.Get() == transaction) {
        Transaction_.Reset();
    } else {
        PreviousTransaction_.Reset();
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
