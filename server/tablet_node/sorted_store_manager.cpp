#include "sorted_store_manager.h"
#include "sorted_chunk_store.h"
#include "config.h"
#include "sorted_dynamic_store.h"
#include "tablet.h"
#include "tablet_slot.h"
#include "transaction_manager.h"
#include "chunk_writer_pool.h"

#include <yt/server/tablet_node/tablet_manager.pb.h>

#include <yt/ytlib/chunk_client/block_cache.h>

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/unversioned_row.h>
#include <yt/ytlib/table_client/versioned_chunk_writer.h>
#include <yt/ytlib/table_client/versioned_reader.h>
#include <yt/ytlib/table_client/versioned_row.h>
#include <yt/ytlib/table_client/versioned_writer.h>

#include <yt/ytlib/tablet_client/wire_protocol.h>
#include <yt/ytlib/tablet_client/wire_protocol.pb.h>

#include <yt/ytlib/transaction_client/helpers.h>

#include <yt/ytlib/api/transaction.h>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NYTree;
using namespace NApi;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NTabletClient;
using namespace NTabletClient::NProto;
using namespace NObjectClient;
using namespace NTabletNode::NProto;
using namespace NHydra;

using NTableClient::TKey;

////////////////////////////////////////////////////////////////////////////////

static const size_t MaxRowsPerFlushRead = 1024;

static const auto BlockedRowWaitQuantum = TDuration::MilliSeconds(100);

////////////////////////////////////////////////////////////////////////////////

TSortedStoreManager::TSortedStoreManager(
    TTabletManagerConfigPtr config,
    TTablet* tablet,
    ITabletContext* tabletContext,
    NHydra::IHydraManagerPtr hydraManager,
    TInMemoryManagerPtr inMemoryManager,
    IClientPtr client)
    : TStoreManagerBase(
        std::move(config),
        tablet,
        tabletContext,
        std::move(hydraManager),
        std::move(inMemoryManager),
        std::move(client))
    , KeyColumnCount_(Tablet_->GetKeyColumnCount())
{
    for (const auto& pair : Tablet_->StoreIdMap()) {
        auto store = pair.second->AsSorted();
        if (store->GetStoreState() != EStoreState::ActiveDynamic) {
            MaxTimestampToStore_.insert(std::make_pair(store->GetMaxTimestamp(), store));
        }
    }

    if (Tablet_->GetActiveStore()) {
        ActiveStore_ = Tablet_->GetActiveStore()->AsSortedDynamic();
    }
}

void TSortedStoreManager::ExecuteAtomicWrite(
    TTablet* tablet,
    TTransaction* transaction,
    TWireProtocolReader* reader,
    bool prelock)
{
    auto command = reader->ReadCommand();
    switch (command) {
        case EWireProtocolCommand::WriteRow: {
            TReqWriteRow req;
            reader->ReadMessage(&req);
            auto row = reader->ReadUnversionedRow();
            WriteRowAtomic(
                transaction,
                row,
                prelock);
            break;
        }

        case EWireProtocolCommand::DeleteRow: {
            TReqDeleteRow req;
            reader->ReadMessage(&req);
            auto key = reader->ReadUnversionedRow();
            DeleteRowAtomic(
                transaction,
                key,
                prelock);
            break;
        }

        default:
            THROW_ERROR_EXCEPTION("Unsupported write command %v",
                command);
    }
}

void TSortedStoreManager::ExecuteNonAtomicWrite(
    TTablet* tablet,
    TTimestamp commitTimestamp,
    TWireProtocolReader* reader)
{
    auto command = reader->ReadCommand();
    switch (command) {
        case EWireProtocolCommand::WriteRow: {
            TReqWriteRow req;
            reader->ReadMessage(&req);
            auto row = reader->ReadUnversionedRow();
            WriteRowNonAtomic(commitTimestamp, row);
            break;
        }

        case EWireProtocolCommand::DeleteRow: {
            TReqDeleteRow req;
            reader->ReadMessage(&req);
            auto key = reader->ReadUnversionedRow();
            DeleteRowNonAtomic(commitTimestamp, key);
            break;
        }

        default:
            THROW_ERROR_EXCEPTION("Unknown write command %v",
                command);
    }
}

TSortedDynamicRowRef TSortedStoreManager::WriteRowAtomic(
    TTransaction* transaction,
    TUnversionedRow row,
    bool prelock)
{
    if (prelock) {
        ValidateOnWrite(transaction->GetId(), row);
    }

    ui32 lockMask = ComputeLockMask(row);

    if (prelock) {
        CheckInactiveStoresLocks(
            transaction,
            row,
            lockMask);
    }

    auto dynamicRow = ActiveStore_->WriteRowAtomic(transaction, row, lockMask);
    auto dynamicRowRef = TSortedDynamicRowRef(ActiveStore_.Get(), this, dynamicRow);
    LockRow(transaction, prelock, dynamicRowRef);
    return dynamicRowRef;
}

void TSortedStoreManager::WriteRowNonAtomic(
    TTimestamp commitTimestamp,
    TUnversionedRow row)
{
    // TODO(sandello): YT-4148
    // ValidateOnWrite(transactionId, row);

    ActiveStore_->WriteRowNonAtomic(row, commitTimestamp);
}

TSortedDynamicRowRef TSortedStoreManager::DeleteRowAtomic(
    TTransaction* transaction,
    NTableClient::TKey key,
    bool prelock)
{
    if (prelock) {
        ValidateOnDelete(transaction->GetId(), key);

        CheckInactiveStoresLocks(
            transaction,
            key,
            TSortedDynamicRow::PrimaryLockMask);
    }

    auto dynamicRow = ActiveStore_->DeleteRowAtomic(transaction, key);
    auto dynamicRowRef = TSortedDynamicRowRef(ActiveStore_.Get(), this, dynamicRow);
    LockRow(transaction, prelock, dynamicRowRef);
    return dynamicRowRef;
}

void TSortedStoreManager::DeleteRowNonAtomic(
    TTimestamp commitTimestamp,
    NTableClient::TKey key)
{
    // TODO(sandello): YT-4148
    // ValidateOnDelete(transactionId, key);

    ActiveStore_->DeleteRowNonAtomic(key, commitTimestamp);
}

void TSortedStoreManager::LockRow(TTransaction* transaction, bool prelock, const TSortedDynamicRowRef& rowRef)
{
    if (prelock) {
        transaction->PrelockedSortedRows().push(rowRef);
    } else {
        transaction->LockedSortedRows().push_back(rowRef);
    }
}

void TSortedStoreManager::ConfirmRow(TTransaction* transaction, const TSortedDynamicRowRef& rowRef)
{
    transaction->LockedSortedRows().push_back(rowRef);
}

void TSortedStoreManager::PrepareRow(TTransaction* transaction, const TSortedDynamicRowRef& rowRef)
{
    rowRef.Store->PrepareRow(transaction, rowRef.Row);
}

void TSortedStoreManager::CommitRow(TTransaction* transaction, const TSortedDynamicRowRef& rowRef)
{
    if (rowRef.Store == ActiveStore_) {
        ActiveStore_->CommitRow(transaction, rowRef.Row);
    } else {
        auto migratedRow = ActiveStore_->MigrateRow(transaction, rowRef.Row);
        rowRef.Store->CommitRow(transaction, rowRef.Row);
        CheckForUnlockedStore(rowRef.Store);
        ActiveStore_->CommitRow(transaction, migratedRow);
    }
}

void TSortedStoreManager::AbortRow(TTransaction* transaction, const TSortedDynamicRowRef& rowRef)
{
    rowRef.Store->AbortRow(transaction, rowRef.Row);
    CheckForUnlockedStore(rowRef.Store);
}

IDynamicStore* TSortedStoreManager::GetActiveStore() const
{
    return ActiveStore_.Get();
}

ui32 TSortedStoreManager::ComputeLockMask(TUnversionedRow row)
{
    const auto& columnIndexToLockIndex = Tablet_->ColumnIndexToLockIndex();
    ui32 lockMask = 0;
    for (int index = KeyColumnCount_; index < row.GetCount(); ++index) {
        const auto& value = row[index];
        int lockIndex = columnIndexToLockIndex[value.Id];
        lockMask |= (1 << lockIndex);
    }
    Y_ASSERT(lockMask != 0);
    return lockMask;
}

void TSortedStoreManager::CheckInactiveStoresLocks(
    TTransaction* transaction,
    TUnversionedRow row,
    ui32 lockMask)
{
    for (const auto& store : LockedStores_) {
        store->AsSortedDynamic()->CheckRowLocks(
            row,
            transaction,
            lockMask);
    }

    for (auto it = MaxTimestampToStore_.rbegin();
         it != MaxTimestampToStore_.rend() && it->first > transaction->GetStartTimestamp();
         ++it)
    {
        const auto& store = it->second;
        // Avoid checking locked stores twice.
        if (store->GetType() == EStoreType::SortedDynamic &&
            store->AsSortedDynamic()->GetLockCount() > 0)
            continue;
        store->CheckRowLocks(
            row,
            transaction,
            lockMask);
    }
}

void TSortedStoreManager::Mount(const std::vector<TAddStoreDescriptor>& storeDescriptors)
{
    Tablet_->CreateInitialPartition();

    std::vector<std::tuple<TOwningKey, int, int>> chunkBoundaries;
    int descriptorIndex = 0;
    const auto& schema = Tablet_->Schema();
    for (const auto& descriptor : storeDescriptors) {
        const auto& extensions = descriptor.chunk_meta().extensions();
        auto miscExt = GetProtoExtension<NChunkClient::NProto::TMiscExt>(extensions);
        if (!miscExt.eden()) {
            auto boundaryKeysExt = GetProtoExtension<NTableClient::NProto::TBoundaryKeysExt>(extensions);
            auto minKey = WidenKey(FromProto<TOwningKey>(boundaryKeysExt.min()), schema.GetKeyColumnCount());
            auto maxKey = WidenKey(FromProto<TOwningKey>(boundaryKeysExt.max()), schema.GetKeyColumnCount());
            chunkBoundaries.push_back(std::make_tuple(minKey, -1, descriptorIndex));
            chunkBoundaries.push_back(std::make_tuple(maxKey, 1, descriptorIndex));
        }
        ++descriptorIndex;
    }

    if (!chunkBoundaries.empty()) {
        std::sort(chunkBoundaries.begin(), chunkBoundaries.end());
        std::vector<TOwningKey> pivotKeys{Tablet_->GetPivotKey()};
        int depth = 0;
        for (const auto& boundary : chunkBoundaries) {
            if (std::get<1>(boundary) == -1 && depth == 0 && std::get<0>(boundary) > Tablet_->GetPivotKey()) {
                pivotKeys.push_back(std::get<0>(boundary));
            }
            depth -= std::get<1>(boundary);
        }

        YCHECK(Tablet_->PartitionList().size() == 1);
        DoSplitPartition(0, pivotKeys);
    }

    TStoreManagerBase::Mount(storeDescriptors);
}

void TSortedStoreManager::Remount(
    TTableMountConfigPtr mountConfig,
    TTabletWriterOptionsPtr writerOptions)
{
    int oldSamplesPerPartition = Tablet_->GetConfig()->SamplesPerPartition;
    int newSamplesPerPartition = mountConfig->SamplesPerPartition;

    TStoreManagerBase::Remount(mountConfig, writerOptions);

    if (oldSamplesPerPartition != newSamplesPerPartition) {
        SchedulePartitionsSampling(0, Tablet_->PartitionList().size());
    }
}

void TSortedStoreManager::AddStore(IStorePtr store, bool onMount)
{
    TStoreManagerBase::AddStore(store, onMount);

    auto sortedStore = store->AsSorted();
    MaxTimestampToStore_.insert(std::make_pair(sortedStore->GetMaxTimestamp(), sortedStore));

    SchedulePartitionSampling(sortedStore->GetPartition());
}

void TSortedStoreManager::RemoveStore(IStorePtr store)
{
    // The range is likely to contain at most one element.
    auto sortedStore = store->AsSorted();
    auto range = MaxTimestampToStore_.equal_range(sortedStore->GetMaxTimestamp());
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == sortedStore) {
            MaxTimestampToStore_.erase(it);
            break;
        }
    }

    SchedulePartitionSampling(sortedStore->GetPartition());

    TStoreManagerBase::RemoveStore(store);
}

void TSortedStoreManager::CreateActiveStore()
{
    auto storeId = TabletContext_->GenerateId(EObjectType::SortedDynamicTabletStore);
    ActiveStore_ = TabletContext_
        ->CreateStore(Tablet_, EStoreType::SortedDynamic, storeId, nullptr)
        ->AsSortedDynamic();

    ActiveStore_->SetRowBlockedHandler(CreateRowBlockedHandler(ActiveStore_));

    Tablet_->AddStore(ActiveStore_);
    Tablet_->SetActiveStore(ActiveStore_);

    LOG_INFO_UNLESS(IsRecovery(), "Active store created (StoreId: %v)",
        storeId);
}

void TSortedStoreManager::ResetActiveStore()
{
    ActiveStore_.Reset();
}

void TSortedStoreManager::OnActiveStoreRotated()
{
    MaxTimestampToStore_.insert(std::make_pair(ActiveStore_->GetMaxTimestamp(), ActiveStore_));
}

TStoreFlushCallback TSortedStoreManager::MakeStoreFlushCallback(
    IDynamicStorePtr store,
    TTabletSnapshotPtr tabletSnapshot)
{
    auto reader = store->AsSortedDynamic()->CreateFlushReader();
    // NB: Memory store reader is always synchronous.
    YCHECK(reader->Open().Get().IsOK());

    return BIND([=, this_ = MakeStrong(this)] (ITransactionPtr transaction) {
        auto writerOptions = CloneYsonSerializable(tabletSnapshot->WriterOptions);
        writerOptions->ChunksEden = true;

        TChunkWriterPool writerPool(
            InMemoryManager_,
            tabletSnapshot,
            1,
            Config_->ChunkWriter,
            writerOptions,
            Client_,
            transaction->GetId());
        auto writer = writerPool.AllocateWriter();

        WaitFor(writer->Open())
            .ThrowOnError();

        std::vector<TVersionedRow> rows;
        rows.reserve(MaxRowsPerFlushRead);

        while (true) {
            // NB: Memory store reader is always synchronous.
            reader->Read(&rows);
            if (rows.empty()) {
                break;
            }
            if (!writer->Write(rows)) {
                WaitFor(writer->GetReadyEvent())
                    .ThrowOnError();
            }
        }

        WaitFor(writer->Close())
            .ThrowOnError();

        std::vector<TAddStoreDescriptor> result;
        for (const auto& chunkSpec : writer->GetWrittenChunksMasterMeta()) {
            TAddStoreDescriptor descriptor;
            descriptor.set_store_type(static_cast<int>(EStoreType::SortedChunk));
            descriptor.mutable_store_id()->CopyFrom(chunkSpec.chunk_id());
            descriptor.mutable_chunk_meta()->CopyFrom(chunkSpec.chunk_meta());
            ToProto(descriptor.mutable_backing_store_id(), store->GetId());
            result.push_back(descriptor);
        }
        return result;
    });
}

bool TSortedStoreManager::IsStoreCompactable(IStorePtr store) const
{
    if (store->GetStoreState() != EStoreState::Persistent) {
        return false;
    }

    // NB: Partitioning chunk stores with backing ones may interfere with conflict checking.
    auto sortedChunkStore = store->AsSortedChunk();
    if (sortedChunkStore->HasBackingStore()) {
        return false;
    }

    if (sortedChunkStore->GetCompactionState() != EStoreCompactionState::None) {
        return false;
    }

    return true;
}

ISortedStoreManagerPtr TSortedStoreManager::AsSorted()
{
    return this;
}

bool TSortedStoreManager::SplitPartition(
    int partitionIndex,
    const std::vector<TOwningKey>& pivotKeys)
{
    auto* partition = Tablet_->PartitionList()[partitionIndex].get();

    // NB: Set the state back to normal; otherwise if some of the below checks fail, we might get
    // a partition stuck in splitting state forever.
    partition->SetState(EPartitionState::Normal);

    if (Tablet_->PartitionList().size() >= Tablet_->GetConfig()->MaxPartitionCount) {
        return false;
    }

    DoSplitPartition(partitionIndex, pivotKeys);

    // NB: Initial partition is split into new ones with indexes |[partitionIndex, partitionIndex + pivotKeys.size())|.
    SchedulePartitionsSampling(partitionIndex, partitionIndex + pivotKeys.size());

    return true;
}

void TSortedStoreManager::MergePartitions(
    int firstPartitionIndex,
    int lastPartitionIndex)
{
    for (int index = firstPartitionIndex; index <= lastPartitionIndex; ++index) {
        const auto& partition = Tablet_->PartitionList()[index];
        // See SplitPartition.
        // Currently this code is redundant since there's no escape path below,
        // but we prefer to keep it to make things look symmetric.
        partition->SetState(EPartitionState::Normal);
    }

    DoMergePartitions(firstPartitionIndex, lastPartitionIndex);

    // NB: Initial partitions are merged into a single one with index |firstPartitionIndex|.
    SchedulePartitionsSampling(firstPartitionIndex, firstPartitionIndex + 1);
}

void TSortedStoreManager::UpdatePartitionSampleKeys(
    TPartition* partition,
    const std::vector<TOwningKey>& keys)
{
    YCHECK(keys.empty() || keys[0] > partition->GetPivotKey());

    auto keyList = New<TKeyList>();
    keyList->Keys = keys;
    partition->SetSampleKeys(keyList);

    const auto* mutationContext = GetCurrentMutationContext();
    partition->SetSamplingTime(mutationContext->GetTimestamp());
}

void TSortedStoreManager::ValidateOnWrite(
    const TTransactionId& transactionId,
    TUnversionedRow row)
{
    try {
        ValidateServerDataRow(row, Tablet_->Schema());
        if (row.GetCount() == KeyColumnCount_) {
            THROW_ERROR_EXCEPTION("Empty writes are not allowed");
        }
    } catch (TErrorException& ex) {
        auto& errorAttributes = ex.Error().Attributes();
        errorAttributes.Set("transaction_id", transactionId);
        errorAttributes.Set("tablet_id", Tablet_->GetId());
        errorAttributes.Set("row", row);
        throw ex;
    }
}

void TSortedStoreManager::ValidateOnDelete(
    const TTransactionId& transactionId,
    TKey key)
{
    try {
        ValidateServerKey(key, Tablet_->Schema());
    } catch (TErrorException& ex) {
        auto& errorAttributes = ex.Error().Attributes();
        errorAttributes.Set("transaction_id", transactionId);
        errorAttributes.Set("tablet_id", Tablet_->GetId());
        errorAttributes.Set("key", key);
        throw ex;
    }
}

void TSortedStoreManager::SchedulePartitionSampling(TPartition* partition)
{
    if (!HasMutationContext()) {
        return;
    }

    if (partition->IsEden()) {
        return;
    }

    const auto* mutationContext = GetCurrentMutationContext();
    partition->SetSamplingRequestTime(mutationContext->GetTimestamp());
}

void TSortedStoreManager::SchedulePartitionsSampling(int beginPartitionIndex, int endPartitionIndex)
{
    if (!HasMutationContext()) {
        return;
    }

    const auto* mutationContext = GetCurrentMutationContext();
    for (int index = beginPartitionIndex; index < endPartitionIndex; ++index) {
        Tablet_->PartitionList()[index]->SetSamplingRequestTime(mutationContext->GetTimestamp());
    }
}

void TSortedStoreManager::DoSplitPartition(int partitionIndex, const std::vector<TOwningKey>& pivotKeys)
{
    Tablet_->SplitPartition(partitionIndex, pivotKeys);
    if (!IsRecovery()) {
        for (int currentIndex = partitionIndex; currentIndex < partitionIndex + pivotKeys.size(); ++currentIndex) {
            Tablet_->PartitionList()[currentIndex]->StartEpoch();
        }
    }
}

void TSortedStoreManager::DoMergePartitions(int firstPartitionIndex, int lastPartitionIndex)
{
    Tablet_->MergePartitions(firstPartitionIndex, lastPartitionIndex);
    if (!IsRecovery()) {
        Tablet_->PartitionList()[firstPartitionIndex]->StartEpoch();
    }
}

void TSortedStoreManager::StartEpoch(TTabletSlotPtr slot)
{
    TStoreManagerBase::StartEpoch(slot);

    if (slot) {
        EpochInvoker_ = slot->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Read);
    }

    for (const auto& pair : Tablet_->StoreIdMap()) {
        const auto& store = pair.second;
        if (store->GetType() == EStoreType::SortedDynamic) {
            auto sortedDynamicStore = store->AsSortedDynamic();
            sortedDynamicStore->SetRowBlockedHandler(CreateRowBlockedHandler(store));
        }
    }
}

void TSortedStoreManager::StopEpoch()
{
    for (const auto& pair : Tablet_->StoreIdMap()) {
        const auto& store = pair.second;
        if (store->GetType() == EStoreType::SortedDynamic) {
            store->AsSortedDynamic()->ResetRowBlockedHandler();
        }
    }

    EpochInvoker_.Reset();

    TStoreManagerBase::StopEpoch();
}

TSortedDynamicStore::TRowBlockedHandler TSortedStoreManager::CreateRowBlockedHandler(
    const IStorePtr& store)
{
    if (!EpochInvoker_) {
        return TSortedDynamicStore::TRowBlockedHandler();
    }

    return BIND(
        &TSortedStoreManager::OnRowBlocked,
        MakeWeak(this),
        Unretained(store.Get()),
        EpochInvoker_);
}

void TSortedStoreManager::OnRowBlocked(
    IStore* store,
    IInvokerPtr invoker,
    TSortedDynamicRow row,
    int lockIndex)
{
    WaitFor(
        BIND(
            &TSortedStoreManager::WaitOnBlockedRow,
            MakeStrong(this),
            MakeStrong(store),
            row,
            lockIndex)
        .AsyncVia(invoker)
        .Run());
}

void TSortedStoreManager::WaitOnBlockedRow(
    IStorePtr /*store*/,
    TSortedDynamicRow row,
    int lockIndex)
{
    const auto& lock = row.BeginLocks(Tablet_->GetKeyColumnCount())[lockIndex];
    const auto* transaction = lock.Transaction;
    if (!transaction) {
        return;
    }

    LOG_DEBUG("Waiting on blocked row (Key: %v, LockIndex: %v, TransactionId: %v)",
        RowToKey(Tablet_->Schema(), row),
        lockIndex,
        transaction->GetId());

    WaitFor(transaction->GetFinished().WithTimeout(BlockedRowWaitQuantum));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT

