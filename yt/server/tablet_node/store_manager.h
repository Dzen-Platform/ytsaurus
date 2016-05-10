#pragma once

#include "public.h"

#include <yt/server/cell_node/public.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/ytlib/api/public.h>

#include <yt/core/actions/future.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

using TStoreFlushResult = std::vector<NTabletNode::NProto::TAddStoreDescriptor>;
using TStoreFlushCallback = TCallback<TStoreFlushResult(NApi::ITransactionPtr transaction)>;

//! Provides a facade for modifying data within a given tablet.
/*!
 *  Each tablet has an instance of IStoreManager, which is attached to the tablet
 *  upon its construction.
 *
 *  IStoreManager instances are not bound to any specific epoch and are reused.
 */
struct IStoreManager
    : public virtual TRefCounted
{
    //! Returns the tablet this instance is bound to.
    virtual TTablet* GetTablet() const = 0;

    //! Returns |true| if there are outstanding locks to any of dynamic memory stores.
    //! Used to determine when it is safe to unmount the tablet.
    virtual bool HasActiveLocks() const = 0;

    //! Returns |true| if there are some dynamic memory stores that are not flushed yet.
    virtual bool HasUnflushedStores() const = 0;

    virtual void StartEpoch(TTabletSlotPtr slot) = 0;
    virtual void StopEpoch() = 0;

    virtual void ExecuteAtomicWrite(
        TTablet* tablet,
        TTransaction* transaction,
        NTabletClient::TWireProtocolReader* reader,
        bool prelock) = 0;
    virtual void ExecuteNonAtomicWrite(
        TTablet* tablet,
        NTransactionClient::TTimestamp commitTimestamp,
        NTabletClient::TWireProtocolReader* reader) = 0;

    virtual bool IsOverflowRotationNeeded() const = 0;
    virtual bool IsPeriodicRotationNeeded() const = 0;
    virtual bool IsRotationPossible() const = 0;
    virtual bool IsForcedRotationPossible() const = 0;
    virtual bool IsRotationScheduled() const = 0;
    virtual void ScheduleRotation() = 0;
    virtual void Rotate(bool createNewStore) = 0;

    virtual void AddStore(IStorePtr store, bool onMount) = 0;

    virtual void RemoveStore(IStorePtr store) = 0;
    virtual void BackoffStoreRemoval(IStorePtr store) = 0;

    //! Creates and sets an empty dynamic store.
    virtual void CreateActiveStore() = 0;

    virtual bool IsStoreLocked(IStorePtr store) const = 0;
    virtual std::vector<IStorePtr> GetLockedStores() const = 0;

    virtual IChunkStorePtr PeekStoreForPreload() = 0;
    virtual void BeginStorePreload(
        IChunkStorePtr store,
        TFuture<void> future) = 0;
    virtual void EndStorePreload(IChunkStorePtr store) = 0;
    virtual void BackoffStorePreload(IChunkStorePtr store) = 0;

    virtual bool IsStoreFlushable(IStorePtr store) const = 0;
    virtual TStoreFlushCallback BeginStoreFlush(
        IDynamicStorePtr store,
        TTabletSnapshotPtr tabletSnapshot) = 0;
    virtual void EndStoreFlush(IDynamicStorePtr store) = 0;
    virtual void BackoffStoreFlush(IDynamicStorePtr store) = 0;

    virtual bool IsStoreCompactable(IStorePtr store) const = 0;
    virtual void BeginStoreCompaction(IChunkStorePtr store) = 0;
    virtual void EndStoreCompaction(IChunkStorePtr store) = 0;
    virtual void BackoffStoreCompaction(IChunkStorePtr store) = 0;

    virtual void Remount(
        TTableMountConfigPtr mountConfig,
        TTabletWriterOptionsPtr writerOptions) = 0;


    virtual ISortedStoreManagerPtr AsSorted() = 0;
    virtual IOrderedStoreManagerPtr AsOrdered() = 0;

};

DEFINE_REFCOUNTED_TYPE(IStoreManager)

////////////////////////////////////////////////////////////////////////////////

//! A refinement of IStoreManager for sorted tablets.
struct ISortedStoreManager
    : public virtual IStoreManager
{ };

DEFINE_REFCOUNTED_TYPE(ISortedStoreManager)

////////////////////////////////////////////////////////////////////////////////

//! A refinement of IStoreManager for ordered tablets.
struct IOrderedStoreManager
    : public virtual IStoreManager
{ };

DEFINE_REFCOUNTED_TYPE(IOrderedStoreManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
