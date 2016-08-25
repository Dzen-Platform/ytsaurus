#pragma once

#include "store_manager_detail.h"
#include "dynamic_store_bits.h"
#include "sorted_dynamic_store.h"

#include <yt/server/cell_node/public.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/tablet_client/public.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TSortedStoreManager
    : public TStoreManagerBase
    , public ISortedStoreManager
{
public:
    TSortedStoreManager(
        TTabletManagerConfigPtr config,
        TTablet* tablet,
        ITabletContext* tabletContext,
        NHydra::IHydraManagerPtr hydraManager = nullptr,
        TInMemoryManagerPtr inMemoryManager = nullptr,
        NApi::IClientPtr client = nullptr);

    // IStoreManager overrides.
    virtual void ExecuteAtomicWrite(
        TTablet* tablet,
        TTransaction* transaction,
        NTabletClient::TWireProtocolReader* reader,
        bool prelock) override;
    virtual void ExecuteNonAtomicWrite(
        TTablet* tablet,
        NTransactionClient::TTimestamp commitTimestamp,
        NTabletClient::TWireProtocolReader* reader) override;

    TSortedDynamicRowRef WriteRowAtomic(
        TTransaction* transaction,
        TUnversionedRow row,
        bool prelock);
    void WriteRowNonAtomic(
        TTimestamp commitTimestamp,
        TUnversionedRow row);
    TSortedDynamicRowRef DeleteRowAtomic(
        TTransaction* transaction,
        TKey key,
        bool prelock);
    void DeleteRowNonAtomic(
        TTimestamp commitTimestamp,
        TKey key);

    virtual void StartEpoch(TTabletSlotPtr slot) override;
    virtual void StopEpoch() override;

    static void LockRow(TTransaction* transaction, bool prelock, const TSortedDynamicRowRef& rowRef);
    void ConfirmRow(TTransaction* transaction, const TSortedDynamicRowRef& rowRef);
    void PrepareRow(TTransaction* transaction, const TSortedDynamicRowRef& rowRef);
    void CommitRow(TTransaction* transaction, const TSortedDynamicRowRef& rowRef);
    void AbortRow(TTransaction* transaction, const TSortedDynamicRowRef& rowRef);

    virtual void Mount(
        const std::vector<NTabletNode::NProto::TAddStoreDescriptor>& storeDescriptors) override;
    virtual void Remount(
        TTableMountConfigPtr mountConfig,
        TTabletWriterOptionsPtr writerOptions) override;

    virtual void AddStore(IStorePtr store, bool onMount) override;
    virtual void RemoveStore(IStorePtr store) override;

    virtual bool IsStoreCompactable(IStorePtr store) const override;

    virtual ISortedStoreManagerPtr AsSorted() override;

    // ISortedStoreManager overrides.
    virtual bool SplitPartition(
        int partitionIndex,
        const std::vector<TOwningKey>& pivotKeys) override;
    virtual void MergePartitions(
        int firstPartitionIndex,
        int lastPartitionIndex) override;
    virtual void UpdatePartitionSampleKeys(
        TPartition* partition,
        const std::vector<TOwningKey>& keys) override;

private:
    const int KeyColumnCount_;

    TSortedDynamicStorePtr ActiveStore_;
    std::multimap<TTimestamp, ISortedStorePtr> MaxTimestampToStore_;

    IInvokerPtr EpochInvoker_;

    virtual IDynamicStore* GetActiveStore() const override;
    virtual void ResetActiveStore() override;
    virtual void OnActiveStoreRotated() override;

    virtual TStoreFlushCallback MakeStoreFlushCallback(
        IDynamicStorePtr store,
        TTabletSnapshotPtr tabletSnapshot) override;

    virtual void CreateActiveStore() override;

    ui32 ComputeLockMask(TUnversionedRow row);

    void CheckInactiveStoresLocks(
        TTransaction* transaction,
        TUnversionedRow row,
        ui32 lockMask);

    void ValidateOnWrite(const TTransactionId& transactionId, TUnversionedRow row);
    void ValidateOnDelete(const TTransactionId& transactionId, TKey key);

    void SchedulePartitionSampling(TPartition* partition);
    void SchedulePartitionsSampling(int beginPartitionIndex, int endPartitionIndex);

    void DoSplitPartition(
        int partitionIndex,
        const std::vector<TOwningKey>& pivotKeys);
    void DoMergePartitions(
        int firstPartitionIndex,
        int lastPartitionIndex);

    TSortedDynamicStore::TRowBlockedHandler CreateRowBlockedHandler(
        const IStorePtr& store);
    void OnRowBlocked(
        IStore* store,
        IInvokerPtr invoker,
        TSortedDynamicRow row,
        int lockIndex);
    void WaitOnBlockedRow(
        IStorePtr store,
        TSortedDynamicRow row,
        int lockIndex);
};

DEFINE_REFCOUNTED_TYPE(TSortedStoreManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
