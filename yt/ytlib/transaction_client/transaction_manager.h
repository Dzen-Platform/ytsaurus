#pragma once

#include "public.h"

#include <core/actions/signal.h>

#include <core/rpc/public.h>

#include <ytlib/hydra/public.h>

#include <ytlib/hive/public.h>

#include <ytlib/api/client.h>

namespace NYT {
namespace NTransactionClient {

////////////////////////////////////////////////////////////////////////////////

struct TTransactionStartOptions
    : public NApi::TTransactionStartOptions
{
    TTransactionStartOptions() = default;
    TTransactionStartOptions(const TTransactionStartOptions& other) = default;
    TTransactionStartOptions(const NApi::TTransactionStartOptions& other)
        : NApi::TTransactionStartOptions(other)
    { }

    bool EnableUncommittedAccounting = true;
    bool EnableStagedAccounting = true;
};

struct TTransactionAttachOptions
{
    bool AutoAbort = false;
    bool Ping = true;
    bool PingAncestors = false;
};

using TTransactionCommitOptions = NApi::TTransactionCommitOptions;
using TTransactionAbortOptions = NApi::TTransactionAbortOptions;

////////////////////////////////////////////////////////////////////////////////

//! Represents a transaction within a client.
class TTransaction
    : public TRefCounted
{
public:
    ~TTransaction();

    //! Commits the transaction asynchronously.
    /*!
     *  Should not be called more than once.
     *
     *  \note Thread affinity: ClientThread
     */
    TFuture<void> Commit(const TTransactionCommitOptions& options = TTransactionCommitOptions());

    //! Aborts the transaction asynchronously.
    /*!
     *  \note Thread affinity: any
     */
    TFuture<void> Abort(const TTransactionAbortOptions& options = TTransactionAbortOptions());

    //! Detaches the transaction, i.e. stops pings.
    /*!
     *  This call does not block and does not throw.
     *  Safe to call multiple times.
     *
     *  \note Thread affinity: ClientThread
     */
    void Detach();

    //! Sends an asynchronous ping.
    /*!
     *  \note Thread affinity: any
     */
    TFuture<void> Ping();


    //! Returns the transaction type.
    /*!
     *  \note Thread affinity: any
     */
    ETransactionType GetType() const;

    //! Returns the transaction id.
    /*!
     *  \note Thread affinity: any
     */
    const TTransactionId& GetId() const;

    //! Returns the transaction start timestamp.
    /*!
     *  \note Thread affinity: any
     */
    TTimestamp GetStartTimestamp() const;


    //! Called to mark a given cell as a transaction participant.
    //! Starts the corresponding transaction and returns the async result.
    /*!
     *  \note Thread affinity: ClientThread
     */
    TFuture<void> AddTabletParticipant(const NElection::TCellId& cellId);


    //! Raised when the transaction is aborted.
    /*!
     *  \note Thread affinity: any
     */
    DECLARE_SIGNAL(void(), Aborted);

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl_;

    friend class TTransactionManager;
    DECLARE_NEW_FRIEND();

    static TTransactionPtr Create(TIntrusivePtr<TImpl> impl);
    explicit TTransaction(TIntrusivePtr<TImpl> impl);

};

DEFINE_REFCOUNTED_TYPE(TTransaction)

////////////////////////////////////////////////////////////////////////////////

//! Controls transactions at client-side.
/*!
 *  Provides a factory for all client-side transactions.
 *  Keeps track of all active transactions and sends pings to master servers periodically.
 *
 * /note Thread affinity: any
 */
class TTransactionManager
    : public TRefCounted
{
public:
    //! Initializes an instance.
    /*!
     * \param config A configuration.
     * \param channel A channel used for communicating with masters.
     */
    TTransactionManager(
        TTransactionManagerConfigPtr config,
        const NHive::TCellId& cellId,
        NRpc::IChannelPtr masterChannel,
        ITimestampProviderPtr timestampProvider,
        NHive::TCellDirectoryPtr cellDirectory);

    ~TTransactionManager();

    //! Asynchronously starts a new transaction.
    /*!
     *  If |options.Ping| is |true| then transaction's lease will be renewed periodically.
     *
     *  If |options.PingAncestors| is |true| then the above renewal will also apply to all
     *  ancestor transactions.
     */
    TFuture<TTransactionPtr> Start(
        ETransactionType type,
        const TTransactionStartOptions& options = TTransactionStartOptions());
    
    //! Attaches to an existing transaction.
    /*!
     *  If |options.AutoAbort| is True then the transaction will be aborted
     *  (if not already committed) at the end of its lifetime.
     *
     *  If |options.Ping| is True then Transaction Manager will be renewing
     *  the lease of this transaction.
     *
     *  If |options.PingAncestors| is True then Transaction Manager will be renewing
     *  the leases of all ancestors of this transaction.
     *
     *  \note
     *  This call does not block.
     */
    TTransactionPtr Attach(
        const TTransactionId& id,
        const TTransactionAttachOptions& options = TTransactionAttachOptions());

    //! Asynchronously aborts all active transactions.
    void AbortAll();

private:
    friend class TTransaction;
    class TImpl;
    TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TTransactionManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionClient
} // namespace NYT
