#pragma once

#include "rowset.h"

#include <yt/core/ypath/public.h>

namespace NYT {
namespace NApi {

///////////////////////////////////////////////////////////////////////////////

//! Represents a rowset extracted from a persistent queue via
//! TPersistentQueue::Poll.
struct IPersistentQueueRowset
    : public IUnversionedRowset
{
    //! Confirms that the rowset has been successfully processed
    //! within #transaction and must not be consumed again.
    virtual TFuture<void> Confirm(const ITransactionPtr& transaction) = 0;
};

DEFINE_REFCOUNTED_TYPE(IPersistentQueueRowset)

///////////////////////////////////////////////////////////////////////////////

//! Enables polling and consuming a subset of tablets of an ordered dynamic table.
/*!
 *  The best practices for using TPersistentQueuePoller are as follows:
 *
 *  - Create as many as |N * K| tablets within the queue, where |N| is the number of
 *  consumer processes and |K| is a small constant allowing for tuning |N| afterwards.
 *
 *  - Within each consumer process, create a single instance of TPersistentQueuePoller
 *  and assign it a unique subset of |K| tablets. Assigning the same tablet to multiple
 *  instances will not lead to any data corruption but will cause lock conflicts and
 *  performance degradation.
 *
 *  - Within each consumer process, spawn a number of worker fibers, possibly
 *  within a thread pool. Each fiber must do the following:
 *    - poll the queue (via #TPersistentQueuePoller::Poll); wait until rows arrive;
 *    - start a transaction
 *    - process the data within the transaction; make any writes necessary;
 *    - mark the dequeued rows as consumed (via IPersistentQueueRowset::Confirm)
 *    - commit the transaction
 *
 *  Thread affinity: any
 */
class TPersistentQueuePoller
    : public TRefCounted
{
public:
    //! Constructs a poller.
    /*
     *  \param config poller configuration
     *  \param dataTablePath points to an ordered table with queue data
     *  \param stateTablePath points to a sorted per-consumer table holding the state of the consumer
     *  \param tabletIndexes contains the indexes of the set of tablets to be polled
     */
    TPersistentQueuePoller(
        TPersistentQueuePollerConfigPtr config,
        IClientPtr client,
        const NYPath::TYPath& dataTablePath,
        const NYPath::TYPath& stateTablePath,
        const std::vector<int>& tabletIndexes);

    //! Polls the tablets of the queue.
    /*!
     *  When unconsumed rows become available, the returned future gets
     *  populated with the queue rows. At most #TPersistentQueuePollerConfig::MaxRowsPerPoll
     *  rows are returned.
     *
     *  This does not constitute a dequeue operation yet,
     *  however as long the the returned IPersistentQueueRowset instance is alive,
     *  the client is assumed to be holding a (transient) lock for these rows.
     *
     *  Is is assumed that upon receiving IPersistentQueueRowset the client initiates
     *  a transaction to process these rows, carries out all the required updates within this
     *  transaction and marks the rows are dequeued by calling IPersistentQueueRowset::Confirm.
     *  When this transaction commits, these rows are persistently marked as consumed.
     *
     *  Under any circumstances, it is guaranteed that any queued row is processed at most once
     *  by a consumer transaction that was able to commit successfully.
     */
    TFuture<IPersistentQueueRowsetPtr> Poll();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TPersistentQueuePoller)

///////////////////////////////////////////////////////////////////////////////

//! Creates an empty table holding the state of a consumer.
TFuture<void> CreatePersistentQueueStateTable(
    IClientPtr client,
    const NYPath::TYPath& path);

///////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

