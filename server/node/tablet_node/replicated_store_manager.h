#pragma once

#include "store_manager_detail.h"
#include "dynamic_store_bits.h"

#include <yt/server/node/cluster_node/public.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/client/table_client/unversioned_row.h>

#include <yt/core/misc/chunked_memory_pool.h>

namespace NYT::NTabletNode {

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
        IInMemoryManagerPtr inMemoryManager = nullptr,
        NApi::NNative::IClientPtr client = nullptr);

    // IStoreManager overrides.
    virtual bool HasActiveLocks() const override;

    virtual bool HasUnflushedStores() const override;

    virtual void StartEpoch(TTabletSlotPtr slot) override;
    virtual void StopEpoch() override;

    virtual bool ExecuteWrites(
        NTableClient::TWireProtocolReader* reader,
        TWriteContext* context) override;

    virtual bool IsOverflowRotationNeeded() const override;
    virtual TError CheckOverflow() const override;
    virtual bool IsPeriodicRotationNeeded() const override;
    virtual bool IsRotationPossible() const override;
    virtual bool IsForcedRotationPossible() const override;
    virtual bool IsRotationScheduled() const override;
    virtual bool IsFlushNeeded() const override;
    virtual void InitializeRotation() override;
    virtual void ScheduleRotation() override;
    virtual void UnscheduleRotation() override;
    virtual void Rotate(bool createNewStore) override;

    virtual void AddStore(IStorePtr store, bool onMount) override;
    virtual void BulkAddStores(TRange<IStorePtr> stores, bool onMount) override;

    virtual void DiscardAllStores() override;
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

    virtual NTabletClient::EInMemoryMode GetInMemoryMode() const override;

    virtual bool IsStoreFlushable(IStorePtr store) const override;
    virtual TStoreFlushCallback BeginStoreFlush(
        IDynamicStorePtr store,
        TTabletSnapshotPtr tabletSnapshot,
        bool isUnmountWorkflow) override;
    virtual void EndStoreFlush(IDynamicStorePtr store) override;
    virtual void BackoffStoreFlush(IDynamicStorePtr store) override;

    virtual bool IsStoreCompactable(IStorePtr store) const override;
    virtual void BeginStoreCompaction(IChunkStorePtr store) override;
    virtual void EndStoreCompaction(IChunkStorePtr store) override;
    virtual void BackoffStoreCompaction(IChunkStorePtr store) override;

    virtual void Mount(
        const std::vector<NTabletNode::NProto::TAddStoreDescriptor>& storeDescriptors,
        bool createDynamicStore) override;
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

private:
    const TTabletManagerConfigPtr Config_;
    TTablet* const Tablet_;
    ITabletContext* const TabletContext_;
    const NHydra::IHydraManagerPtr HydraManager_;
    const IInMemoryManagerPtr InMemoryManager_;
    const NApi::NNative::IClientPtr Client_;

    const NLogging::TLogger Logger;
    const TOrderedStoreManagerPtr LogStoreManager_;

    NTableClient::TUnversionedRowBuilder LogRowBuilder_;


    TUnversionedRow BuildLogRow(TUnversionedRow row, NApi::ERowModificationType changeType);
    TUnversionedRow BuildSortedLogRow(TUnversionedRow row, NApi::ERowModificationType changeType);
    TUnversionedRow BuildOrderedLogRow(TUnversionedRow row, NApi::ERowModificationType changeType);

};

DEFINE_REFCOUNTED_TYPE(TReplicatedStoreManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
