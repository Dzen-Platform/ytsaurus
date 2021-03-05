#include "sticky_transaction_pool.h"

#include "transaction.h"

#include <yt/yt/core/concurrency/lease_manager.h>
#include <yt/yt/core/concurrency/spinlock.h>

namespace NYT::NApi {

////////////////////////////////////////////////////////////////////////////////

using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

ITransactionPtr IStickyTransactionPool::GetTransactionAndRenewLeaseOrThrow(
    TTransactionId transactionId)
{
    auto transaction = FindTransactionAndRenewLease(transactionId);
    if (!transaction) {
        THROW_ERROR_EXCEPTION(
            NTransactionClient::EErrorCode::NoSuchTransaction,
            "Sticky transaction %v is not found, "
            "this usually means that you use tablet transactions within HTTP API; "
            "consider using RPC API instead",
            transactionId);
    }

    return transaction;
}

////////////////////////////////////////////////////////////////////////////////

class TStickyTransactionPool
    : public IStickyTransactionPool
{
public:
    explicit TStickyTransactionPool(const NLogging::TLogger& logger)
        : Logger(logger)
    { }

    virtual ITransactionPtr RegisterTransaction(ITransactionPtr transaction) override
    {
        auto transactionId = transaction->GetId();
        TStickyTransactionEntry entry{
            transaction,
            NConcurrency::TLeaseManager::CreateLease(
                transaction->GetTimeout(),
                BIND(&TStickyTransactionPool::OnStickyTransactionLeaseExpired, MakeWeak(this), transactionId))
        };

        {
            auto guard = WriterGuard(StickyTransactionLock_);
            YT_VERIFY(IdToStickyTransactionEntry_.emplace(transactionId, entry).second);
        }

        transaction->SubscribeCommitted(
            BIND(&TStickyTransactionPool::OnStickyTransactionFinished, MakeWeak(this), transactionId));
        transaction->SubscribeAborted(
            BIND(&TStickyTransactionPool::OnStickyTransactionFinished, MakeWeak(this), transactionId));

        YT_LOG_DEBUG("Sticky transaction registered (TransactionId: %v)",
            transactionId);

        return transaction;
    }

    virtual void UnregisterTransaction(TTransactionId transactionId) override
    {
        TStickyTransactionEntry entry;
        {
            auto guard = WriterGuard(StickyTransactionLock_);
            auto it = IdToStickyTransactionEntry_.find(transactionId);
            if (it == IdToStickyTransactionEntry_.end()) {
                return;
            }
            entry = std::move(it->second);
            IdToStickyTransactionEntry_.erase(it);
        }

        YT_LOG_DEBUG("Sticky transaction unregistered (TransactionId: %v)",
            transactionId);
    }

    virtual ITransactionPtr FindTransactionAndRenewLease(TTransactionId transactionId) override
    {
        ITransactionPtr transaction;
        NConcurrency::TLease lease;
        {
            auto guard = ReaderGuard(StickyTransactionLock_);
            auto it = IdToStickyTransactionEntry_.find(transactionId);
            if (it == IdToStickyTransactionEntry_.end()) {
                return nullptr;
            }
            const auto& entry = it->second;
            transaction = entry.Transaction;
            lease = entry.Lease;
        }
        NConcurrency::TLeaseManager::RenewLease(lease);
        YT_LOG_DEBUG("Sticky transaction lease renewed (TransactionId: %v)",
            transactionId);
        return transaction;
    }

private:
    const NLogging::TLogger Logger;

    struct TStickyTransactionEntry
    {
        ITransactionPtr Transaction;
        NConcurrency::TLease Lease;
    };

    YT_DECLARE_SPINLOCK(NConcurrency::TReaderWriterSpinLock, StickyTransactionLock_);
    THashMap<TTransactionId, TStickyTransactionEntry> IdToStickyTransactionEntry_;

    void OnStickyTransactionLeaseExpired(TTransactionId transactionId)
    {
        ITransactionPtr transaction;
        {
            auto guard = WriterGuard(StickyTransactionLock_);
            auto it = IdToStickyTransactionEntry_.find(transactionId);
            if (it == IdToStickyTransactionEntry_.end()) {
                return;
            }
            transaction = it->second.Transaction;
            IdToStickyTransactionEntry_.erase(it);
        }

        YT_LOG_DEBUG("Sticky transaction lease expired (TransactionId: %v)",
            transactionId);

        transaction->Abort();
    }
    void OnStickyTransactionFinished(TTransactionId transactionId)
    {
        NConcurrency::TLease lease;
        {
            auto guard = WriterGuard(StickyTransactionLock_);
            auto it = IdToStickyTransactionEntry_.find(transactionId);
            if (it == IdToStickyTransactionEntry_.end()) {
                return;
            }
            lease = it->second.Lease;
            IdToStickyTransactionEntry_.erase(it);
        }

        YT_LOG_DEBUG("Sticky transaction unregistered (TransactionId: %v)",
            transactionId);

        NConcurrency::TLeaseManager::CloseLease(lease);
    }
};

////////////////////////////////////////////////////////////////////////////////

IStickyTransactionPoolPtr CreateStickyTransactionPool(
    const NLogging::TLogger& logger)
{
    return New<TStickyTransactionPool>(logger);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi
