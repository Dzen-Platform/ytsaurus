#pragma once

#include "store_manager_detail.h"
#include "dynamic_store_bits.h"

#include <yt/server/cell_node/public.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/core/misc/chunked_memory_pool.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TReplicatedStoreManager
    : public ISortedStoreManager
{
public:
    TReplicatedStoreManager(
        TTabletManagerConfigPtr config,
        TTablet* tablet,
        ITabletContext* tabletContext,
        NHydra::IHydraManagerPtr hydraManager = nullptr,
        TInMemoryManagerPtr inMemoryManager = nullptr,
        NApi::INativeClientPtr client = nullptr);

    // IStoreManager overrides.
    virtual bool HasActiveLocks() const override;

    virtual bool HasUnflushedStores() const override;

    virtual void StartEpoch(TTabletSlotPtr slot) override;
    virtual void StopEpoch() override;

    virtual void ExecuteAtomicWrite(
        TTransaction* transaction,
        NTabletClient::TWireProtocolReader* reader,
        bool prelock) override;
    virtual void ExecuteNonAtomicWrite(
        const TTransactionId& transactionId,
        NTabletClient::TWireProtocolReader* reader) override;

    virtual bool IsOverflowRotationNeeded() const override;
    virtual bool IsPeriodicRotationNeeded() const override;
    virtual bool IsRotationPossible() const override;
    virtual bool IsForcedRotationPossible() const override;
    virtual bool IsRotationScheduled() const override;
    virtual void ScheduleRotation() override;
    virtual void Rotate(bool createNewStore) override;

    virtual void AddStore(IStorePtr store, bool onMount) override;

    virtual void RemoveStore(IStorePtr store) override;
    virtual void BackoffStoreRemoval(IStorePtr store) override;

    virtual bool IsStoreLocked(IStorePtr store) const override;
    virtual std::vector<IStorePtr> GetLockedStores() const override;

    virtual IChunkStorePtr PeekStoreForPreload() override;
    virtual void BeginStorePreload(
        IChunkStorePtr store,
        TCallback<TFuture<void>()> callbackFuture) override;
    virtual void EndStorePreload(IChunkStorePtr store) override;
    virtual void BackoffStorePreload(IChunkStorePtr store) override;

    virtual bool IsStoreFlushable(IStorePtr store) const override;
    virtual TStoreFlushCallback BeginStoreFlush(
        IDynamicStorePtr store,
        TTabletSnapshotPtr tabletSnapshot) override;
    virtual void EndStoreFlush(IDynamicStorePtr store) override;
    virtual void BackoffStoreFlush(IDynamicStorePtr store) override;

    virtual bool IsStoreCompactable(IStorePtr store) const override;
    virtual void BeginStoreCompaction(IChunkStorePtr store) override;
    virtual void EndStoreCompaction(IChunkStorePtr store) override;
    virtual void BackoffStoreCompaction(IChunkStorePtr store) override;

    virtual void Mount(
        const std::vector<NTabletNode::NProto::TAddStoreDescriptor>& storeDescriptors) override;
    virtual void Remount(
        TTableMountConfigPtr mountConfig,
        TTabletChunkReaderConfigPtr readerConfig,
        TTabletChunkWriterConfigPtr writerConfig,
        TTabletWriterOptionsPtr writerOptions) override;

    virtual ISortedStoreManagerPtr AsSorted() override;
    virtual IOrderedStoreManagerPtr AsOrdered() override;

    // ISortedStoreManager overrides.
    virtual bool SplitPartition(
        int partitionIndex,
        const std::vector<TOwningKey>& pivotKeys) override;
    virtual void MergePartitions(
        int firstPartitionIndex,
        int lastPartitionIndex) override;
    virtual void UpdatePartitionSampleKeys(
        TPartition* partition,
        const TSharedRange<TKey>& keys) override;


    TOrderedDynamicRowRef WriteRow(
        TTransaction* transaction,
        TUnversionedRow row,
        bool prelock);
    TOrderedDynamicRowRef DeleteRow(
        TTransaction* transaction,
        TKey key,
        bool prelock);

private:
    const TTabletManagerConfigPtr Config_;
    TTablet* const Tablet_;
    ITabletContext* const TabletContext_;
    const NHydra::IHydraManagerPtr HydraManager_;
    const TInMemoryManagerPtr InMemoryManager_;
    const NApi::INativeClientPtr Client_;

    const NLogging::TLogger Logger;
    const TOrderedStoreManagerPtr Underlying_;

    NTableClient::TUnversionedRowBuilder LogRowBuilder_;


    TUnversionedRow BuildLogRow(TUnversionedRow row, NApi::ERowModificationType changeType);

};

DEFINE_REFCOUNTED_TYPE(TReplicatedStoreManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
