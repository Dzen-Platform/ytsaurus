#include "tablet.h"
#include "automaton.h"
#include "sorted_chunk_store.h"
#include "config.h"
#include "sorted_dynamic_store.h"
#include "partition.h"
#include "store_manager.h"
#include "tablet_manager.h"
#include "tablet_slot.h"
#include "transaction_manager.h"

#include <yt/ytlib/table_client/chunk_meta.pb.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/ytlib/tablet_client/config.h>

#include <yt/ytlib/transaction_client/timestamp_provider.h>
#include <yt/ytlib/transaction_client/helpers.h>

#include <yt/ytlib/query_client/column_evaluator.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/async_semaphore.h>

#include <yt/core/misc/collection_helpers.h>
#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/serialize.h>

namespace NYT {
namespace NTabletNode {

using namespace NHydra;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletClient::NProto;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NTransactionClient;
using namespace NQueryClient;

////////////////////////////////////////////////////////////////////////////////

void ValidateTabletRetainedTimestamp(const TTabletSnapshotPtr& tabletSnapshot, TTimestamp timestamp)
{
    if (timestamp < tabletSnapshot->RetainedTimestamp) {
        THROW_ERROR_EXCEPTION("Timestamp %v is less than tablet %v retained timestamp %v",
            timestamp,
            tabletSnapshot->TabletId,
            tabletSnapshot->RetainedTimestamp);
    }
}

////////////////////////////////////////////////////////////////////////////////

void TRuntimeTableReplicaData::Populate(TTableReplicaStatistics* statistics) const
{
    statistics->set_current_replication_row_index(CurrentReplicationRowIndex.load());
    statistics->set_current_replication_timestamp(CurrentReplicationTimestamp.load());
}

void TRuntimeTableReplicaData::MergeFrom(const TTableReplicaStatistics& statistics)
{
    CurrentReplicationRowIndex = statistics.current_replication_row_index();
    CurrentReplicationTimestamp = statistics.current_replication_timestamp();
}

////////////////////////////////////////////////////////////////////////////////

std::pair<TTabletSnapshot::TPartitionListIterator, TTabletSnapshot::TPartitionListIterator>
TTabletSnapshot::GetIntersectingPartitions(
    const TKey& lowerBound,
    const TKey& upperBound)
{
    auto beginIt = std::upper_bound(
        PartitionList.begin(),
        PartitionList.end(),
        lowerBound,
        [] (const TKey& key, const TPartitionSnapshotPtr& partition) {
            return key < partition->PivotKey;
        });

    if (beginIt != PartitionList.begin()) {
        --beginIt;
    }

    auto endIt = beginIt;
    while (endIt != PartitionList.end() && upperBound > (*endIt)->PivotKey) {
        ++endIt;
    }

    return std::make_pair(beginIt, endIt);
}

TPartitionSnapshotPtr TTabletSnapshot::FindContainingPartition(TKey key)
{
    auto it = std::upper_bound(
        PartitionList.begin(),
        PartitionList.end(),
        key,
        [] (TKey key, const TPartitionSnapshotPtr& partition) {
            return key < partition->PivotKey;
        });

    return it == PartitionList.begin() ? nullptr : *(--it);
}

std::vector<ISortedStorePtr> TTabletSnapshot::GetEdenStores()
{
    std::vector<ISortedStorePtr> stores;
    stores.reserve(Eden->Stores.size() + LockedStores.size());
    for (auto store : Eden->Stores) {
        stores.emplace_back(std::move(store));
    }
    for (const auto& weakStore : LockedStores) {
        auto store = weakStore.Lock();
        if (store) {
            stores.emplace_back(std::move(store));
        }
    }
    return stores;
}

TTableReplicaSnapshotPtr TTabletSnapshot::FindReplicaSnapshot(const TTableReplicaId& replicaId)
{
    auto it = Replicas.find(replicaId);
    return it == Replicas.end() ? nullptr : it->second;
}

void TTabletSnapshot::ValidateCellId(const TCellId& cellId)
{
    if (CellId != cellId) {
        THROW_ERROR_EXCEPTION("Wrong cell id: expected %v, got %v",
            CellId,
            cellId);
    }
}

void TTabletSnapshot::ValidateMountRevision(i64 mountRevision)
{
    if (MountRevision != mountRevision) {
        THROW_ERROR_EXCEPTION(
            NTabletClient::EErrorCode::InvalidMountRevision,
            "Invalid mount revision of tablet %v: expected %x, received %x",
            TabletId,
            MountRevision,
            mountRevision)
            << TErrorAttribute("tablet_id", TabletId);
    }
}

////////////////////////////////////////////////////////////////////////////////

TTableReplicaInfo::TTableReplicaInfo()
{ }

TTableReplicaInfo::TTableReplicaInfo(const TTableReplicaId& id)
    : Id_(id)
{ }

void TTableReplicaInfo::Save(TSaveContext& context) const
{
    using NYT::Save;
    Save(context, Id_);
    Save(context, ClusterName_);
    Save(context, ReplicaPath_);
    Save(context, StartReplicationTimestamp_);
    Save(context, PreparedReplicationTransactionId_);
    Save(context, State_);
    Save(context, RuntimeData_->CurrentReplicationRowIndex);
    Save(context, RuntimeData_->CurrentReplicationTimestamp);
    Save(context, RuntimeData_->PreparedReplicationRowIndex);
}

void TTableReplicaInfo::Load(TLoadContext& context)
{
    using NYT::Load;
    Load(context, Id_);
    Load(context, ClusterName_);
    Load(context, ReplicaPath_);
    Load(context, StartReplicationTimestamp_);
    // COMPAT(babenko)
    if (context.GetVersion() >= 100001) {
        Load(context, PreparedReplicationTransactionId_);
    }
    Load(context, State_);
    Load(context, RuntimeData_->CurrentReplicationRowIndex);
    Load(context, RuntimeData_->CurrentReplicationTimestamp);
    Load(context, RuntimeData_->PreparedReplicationRowIndex);
}

i64 TTableReplicaInfo::GetCurrentReplicationRowIndex() const
{
    return RuntimeData_->CurrentReplicationRowIndex;
}

void TTableReplicaInfo::SetCurrentReplicationRowIndex(i64 value)
{
    RuntimeData_->CurrentReplicationRowIndex = value;
}

TTimestamp TTableReplicaInfo::GetCurrentReplicationTimestamp() const
{
    return RuntimeData_->CurrentReplicationTimestamp;
}

void TTableReplicaInfo::SetCurrentReplicationTimestamp(TTimestamp value)
{
    RuntimeData_->CurrentReplicationTimestamp = value;
}

i64 TTableReplicaInfo::GetPreparedReplicationRowIndex() const
{
    return RuntimeData_->PreparedReplicationRowIndex;
}

void TTableReplicaInfo::SetPreparedReplicationRowIndex(i64 value)
{
    RuntimeData_->PreparedReplicationRowIndex = value;
}

TTableReplicaSnapshotPtr TTableReplicaInfo::BuildSnapshot() const
{
    auto snapshot = New<TTableReplicaSnapshot>();
    snapshot->StartReplicationTimestamp = StartReplicationTimestamp_;
    snapshot->RuntimeData = RuntimeData_;
    return snapshot;
}

void TTableReplicaInfo::PopulateStatistics(TTableReplicaStatistics* statistics) const
{
    RuntimeData_->Populate(statistics);
}

void TTableReplicaInfo::MergeFromStatistics(const TTableReplicaStatistics& statistics)
{
    RuntimeData_->MergeFrom(statistics);
}

////////////////////////////////////////////////////////////////////////////////

TTablet::TTablet(
    const TTabletId& tabletId,
    ITabletContext* context)
    : TObjectBase(tabletId)
    , Config_(New<TTableMountConfig>())
    , ReaderConfig_(New<TTabletChunkReaderConfig>())
    , WriterConfig_(New<TTabletChunkWriterConfig>())
    , WriterOptions_(New<TTabletWriterOptions>())
    , Context_(context)
{ }

TTablet::TTablet(
    TTableMountConfigPtr config,
    TTabletChunkReaderConfigPtr readerConfig,
    TTabletChunkWriterConfigPtr writerConfig,
    TTabletWriterOptionsPtr writerOptions,
    const TTabletId& tabletId,
    i64 mountRevision,
    const TObjectId& tableId,
    ITabletContext* context,
    const TTableSchema& schema,
    TOwningKey pivotKey,
    TOwningKey nextPivotKey,
    EAtomicity atomicity,
    ECommitOrdering commitOrdering)
    : TObjectBase(tabletId)
    , MountRevision_(mountRevision)
    , TableId_(tableId)
    , TableSchema_(schema)
    , PivotKey_(std::move(pivotKey))
    , NextPivotKey_(std::move(nextPivotKey))
    , State_(ETabletState::Mounted)
    , Atomicity_(atomicity)
    , CommitOrdering_(commitOrdering)
    , HashTableSize_(config->EnableLookupHashTable ? config->MaxDynamicStoreRowCount : 0)
    , RetainedTimestamp_(MinTimestamp)
    , Config_(config)
    , ReaderConfig_(readerConfig)
    , WriterConfig_(writerConfig)
    , WriterOptions_(writerOptions)
    , Eden_(std::make_unique<TPartition>(
        this,
        context->GenerateId(EObjectType::TabletPartition),
        EdenIndex,
        PivotKey_,
        NextPivotKey_))
    , Context_(context)
{
    Initialize();
}

ETabletState TTablet::GetPersistentState() const
{
    switch (State_) {
        case ETabletState::UnmountFlushPending:
            return ETabletState::UnmountWaitingForLocks;
        case ETabletState::UnmountPending:
            return ETabletState::UnmountFlushing;
        case ETabletState::FreezeFlushPending:
            return ETabletState::FreezeWaitingForLocks;
        case ETabletState::FreezePending:
            return ETabletState::FreezeFlushing;
        default:
            return State_;
    }
}

const TTableMountConfigPtr& TTablet::GetConfig() const
{
    return Config_;
}

void TTablet::SetConfig(TTableMountConfigPtr config)
{
    Config_ = config;
}

const TTabletChunkReaderConfigPtr& TTablet::GetReaderConfig() const
{
    return ReaderConfig_;
}

void TTablet::SetReaderConfig(TTabletChunkReaderConfigPtr config)
{
    ReaderConfig_ = config;
}

const TTabletChunkWriterConfigPtr& TTablet::GetWriterConfig() const
{
    return WriterConfig_;
}

void TTablet::SetWriterConfig(TTabletChunkWriterConfigPtr config)
{
    WriterConfig_ = config;
}

const TTabletWriterOptionsPtr& TTablet::GetWriterOptions() const
{
    return WriterOptions_;
}

void TTablet::SetWriterOptions(TTabletWriterOptionsPtr options)
{
    WriterOptions_ = options;
}

const IStoreManagerPtr& TTablet::GetStoreManager() const
{
    return StoreManager_;
}

void TTablet::SetStoreManager(IStoreManagerPtr storeManager)
{
    StoreManager_ = std::move(storeManager);
}

const TTabletPerformanceCountersPtr& TTablet::GetPerformanceCounters() const
{
    return PerformanceCounters_;
}

void TTablet::Save(TSaveContext& context) const
{
    using NYT::Save;

    Save(context, TableId_);
    Save(context, MountRevision_);
    Save(context, GetPersistentState());
    Save(context, TableSchema_);
    Save(context, Atomicity_);
    Save(context, CommitOrdering_);
    Save(context, HashTableSize_);
    Save(context, RuntimeData_->TotalRowCount);
    Save(context, RuntimeData_->TrimmedRowCount);
    Save(context, RuntimeData_->LastCommitTimestamp);
    Save(context, Replicas_);
    Save(context, RetainedTimestamp_);

    TSizeSerializer::Save(context, StoreIdMap_.size());
    // NB: This is not stable.
    for (const auto& pair : StoreIdMap_) {
        const auto& store = pair.second;
        Save(context, store->GetType());
        Save(context, store->GetId());
        store->Save(context);
    }

    Save(context, ActiveStore_ ? ActiveStore_->GetId() : NullStoreId);

    auto savePartition = [&] (const TPartition& partition) {
        Save(context, partition.GetId());
        partition.Save(context);
    };

    savePartition(*Eden_);

    TSizeSerializer::Save(context, PartitionList_.size());
    for (const auto& partition : PartitionList_) {
        savePartition(*partition);
    }
}

void TTablet::Load(TLoadContext& context)
{
    using NYT::Load;

    Load(context, TableId_);
    Load(context, MountRevision_);
    Load(context, State_);
    Load(context, TableSchema_);
    Load(context, Atomicity_);
    Load(context, CommitOrdering_);
    Load(context, HashTableSize_);
    Load(context, RuntimeData_->TotalRowCount);
    Load(context, RuntimeData_->TrimmedRowCount);
    Load(context, RuntimeData_->LastCommitTimestamp);
    Load(context, Replicas_);
    Load(context, RetainedTimestamp_);

    // NB: Stores that we're about to create may request some tablet properties (e.g. column lock count)
    // during construction. Initialize() will take care of this.
    Initialize();

    int storeCount = TSizeSerializer::LoadSuspended(context);
    SERIALIZATION_DUMP_WRITE(context, "stores[%v]", storeCount);
    SERIALIZATION_DUMP_INDENT(context) {
        for (int index = 0; index < storeCount; ++index) {
            auto storeType = Load<EStoreType>(context);
            auto storeId = Load<TStoreId> (context);
            auto store = Context_->CreateStore(this, storeType, storeId, nullptr);
            YCHECK(StoreIdMap_.insert(std::make_pair(store->GetId(), store)).second);
            store->Load(context);
        }
    }

    if (IsPhysicallyOrdered()) {
        for (const auto& pair : StoreIdMap_) {
            auto orderedStore = pair.second->AsOrdered();
            YCHECK(StoreRowIndexMap_.insert(std::make_pair(orderedStore->GetStartingRowIndex(), orderedStore)).second);
        }
    }

    auto activeStoreId = Load<TStoreId>(context);
    if (activeStoreId) {
        ActiveStore_ = GetStore(activeStoreId)->AsDynamic();
    }

    auto loadPartition = [&] (int index) -> std::unique_ptr<TPartition> {
        auto partitionId = LoadSuspended<TPartitionId>(context);
        SERIALIZATION_DUMP_WRITE(context, "%v =>", partitionId);
        SERIALIZATION_DUMP_INDENT(context) {
            auto partition = std::make_unique<TPartition>(
                this,
                partitionId,
                index);
            Load(context, *partition);
            for (const auto& store : partition->Stores()) {
                store->SetPartition(partition.get());
            }
            return partition;
        }
    };

    SERIALIZATION_DUMP_WRITE(context, "partitions");
    SERIALIZATION_DUMP_INDENT(context) {
        Eden_ = loadPartition(EdenIndex);

        int partitionCount = TSizeSerializer::LoadSuspended(context);
        for (int index = 0; index < partitionCount; ++index) {
            auto partition = loadPartition(index);
            YCHECK(PartitionMap_.insert(std::make_pair(partition->GetId(), partition.get())).second);
            PartitionList_.push_back(std::move(partition));
        }
    }
}

TCallback<void(TSaveContext&)> TTablet::AsyncSave()
{
    std::vector<std::pair<TStoreId, TCallback<void(TSaveContext&)>>> capturedStores;
    for (const auto& pair : StoreIdMap_) {
        const auto& store = pair.second;
        capturedStores.push_back(std::make_pair(store->GetId(), store->AsyncSave()));
    }

    auto capturedEden = Eden_->AsyncSave();

    std::vector<TCallback<void(TSaveContext&)>> capturedPartitions;
    for (const auto& partition : PartitionList_) {
        capturedPartitions.push_back(partition->AsyncSave());
    }

    return BIND(
        [
            snapshot = BuildSnapshot(nullptr),
            capturedStores = std::move(capturedStores),
            capturedEden = std::move(capturedEden),
            capturedPartitions = std::move(capturedPartitions)
        ] (TSaveContext& context) {
            using NYT::Save;

            Save(context, *snapshot->Config);
            Save(context, *snapshot->WriterConfig);
            Save(context, *snapshot->WriterOptions);
            Save(context, snapshot->PivotKey);
            Save(context, snapshot->NextPivotKey);

            capturedEden.Run(context);
            for (const auto& callback : capturedPartitions) {
                callback.Run(context);
            }

            // NB: This is not stable.
            for (const auto& pair : capturedStores) {
                Save(context, pair.first);
                pair.second.Run(context);
            }
        });
}

void TTablet::AsyncLoad(TLoadContext& context)
{
    using NYT::Load;

    Load(context, *Config_);
    Load(context, *WriterConfig_);
    Load(context, *WriterOptions_);
    Load(context, PivotKey_);
    Load(context, NextPivotKey_);

    auto loadPartition = [&] (const std::unique_ptr<TPartition>& partition) {
        SERIALIZATION_DUMP_WRITE(context, "%v =>", partition->GetId());
        SERIALIZATION_DUMP_INDENT(context) {
            partition->AsyncLoad(context);
        }
    };

    SERIALIZATION_DUMP_WRITE(context, "partitions");
    SERIALIZATION_DUMP_INDENT(context) {
        loadPartition(Eden_);
        for (const auto& partition : PartitionList_) {
            loadPartition(partition);
        }
    }

    SERIALIZATION_DUMP_WRITE(context, "stores[%v]", StoreIdMap_.size());
    SERIALIZATION_DUMP_INDENT(context) {
        for (int index = 0; index < StoreIdMap_.size(); ++index) {
            auto storeId = Load<TStoreId>(context);
            SERIALIZATION_DUMP_WRITE(context, "%v =>", storeId);
            SERIALIZATION_DUMP_INDENT(context) {
                auto store = GetStore(storeId);
                store->AsyncLoad(context);
            }
        }
    }
}

const std::vector<std::unique_ptr<TPartition>>& TTablet::PartitionList() const
{
    YCHECK(IsPhysicallySorted());
    return PartitionList_;
}

TPartition* TTablet::GetEden() const
{
    YCHECK(IsPhysicallySorted());
    return Eden_.get();
}

void TTablet::CreateInitialPartition()
{
    YCHECK(IsPhysicallySorted());
    YCHECK(PartitionList_.empty());
    auto partition = std::make_unique<TPartition>(
        this,
        Context_->GenerateId(EObjectType::TabletPartition),
        static_cast<int>(PartitionList_.size()),
        PivotKey_,
        NextPivotKey_);
    YCHECK(PartitionMap_.insert(std::make_pair(partition->GetId(), partition.get())).second);
    PartitionList_.push_back(std::move(partition));
}

TPartition* TTablet::FindPartition(const TPartitionId& partitionId)
{
    YCHECK(IsPhysicallySorted());
    const auto& it = PartitionMap_.find(partitionId);
    return it == PartitionMap_.end() ? nullptr : it->second;
}

TPartition* TTablet::GetPartition(const TPartitionId& partitionId)
{
    YCHECK(IsPhysicallySorted());
    auto* partition = FindPartition(partitionId);
    YCHECK(partition);
    return partition;
}

void TTablet::MergePartitions(int firstIndex, int lastIndex)
{
    YCHECK(IsPhysicallySorted());

    for (int i = lastIndex + 1; i < static_cast<int>(PartitionList_.size()); ++i) {
        PartitionList_[i]->SetIndex(i - (lastIndex - firstIndex));
    }

    auto mergedPartition = std::make_unique<TPartition>(
        this,
        Context_->GenerateId(EObjectType::TabletPartition),
        firstIndex,
        PartitionList_[firstIndex]->GetPivotKey(),
        PartitionList_[lastIndex]->GetNextPivotKey());

    std::vector<TKey> mergedSampleKeys;
    auto rowBuffer = New<TRowBuffer>(TSampleKeyListTag());

    for (int index = firstIndex; index <= lastIndex; ++index) {
        const auto& existingPartition = PartitionList_[index];
        const auto& existingSampleKeys = existingPartition->GetSampleKeys()->Keys;
        if (index > firstIndex) {
            mergedSampleKeys.push_back(rowBuffer->Capture(existingPartition->GetPivotKey()));
        }
        for (auto key : existingSampleKeys) {
            mergedSampleKeys.push_back(rowBuffer->Capture(key));
        }

        for (const auto& store : existingPartition->Stores()) {
            YCHECK(store->GetPartition() == existingPartition.get());
            store->SetPartition(mergedPartition.get());
            YCHECK(mergedPartition->Stores().insert(store).second);
        }
    }

    mergedPartition->GetSampleKeys()->Keys = MakeSharedRange(std::move(mergedSampleKeys), std::move(rowBuffer));

    auto firstPartitionIt = PartitionList_.begin() + firstIndex;
    auto lastPartitionIt = PartitionList_.begin() + lastIndex;
    for (auto it = firstPartitionIt; it !=  lastPartitionIt; ++it) {
        PartitionMap_.erase((*it)->GetId());
    }
    YCHECK(PartitionMap_.insert(std::make_pair(mergedPartition->GetId(), mergedPartition.get())).second);
    PartitionList_.erase(firstPartitionIt, lastPartitionIt + 1);
    PartitionList_.insert(firstPartitionIt, std::move(mergedPartition));

    UpdateOverlappingStoreCount();
}

void TTablet::SplitPartition(int index, const std::vector<TOwningKey>& pivotKeys)
{
    YCHECK(IsPhysicallySorted());

    auto existingPartition = std::move(PartitionList_[index]);
    YCHECK(existingPartition->GetPivotKey() == pivotKeys[0]);

    for (int partitionIndex = index + 1; partitionIndex < PartitionList_.size(); ++partitionIndex) {
        PartitionList_[partitionIndex]->SetIndex(partitionIndex + pivotKeys.size() - 1);
    }

    std::vector<std::unique_ptr<TPartition>> splitPartitions;
    const auto& existingSampleKeys = existingPartition->GetSampleKeys()->Keys;
    int sampleKeyIndex = 0;
    for (int pivotKeyIndex = 0; pivotKeyIndex < pivotKeys.size(); ++pivotKeyIndex) {
        auto thisPivotKey = pivotKeys[pivotKeyIndex];
        auto nextPivotKey = (pivotKeyIndex == pivotKeys.size() - 1)
            ? existingPartition->GetNextPivotKey()
            : pivotKeys[pivotKeyIndex + 1];
        auto partition = std::make_unique<TPartition>(
            this,
            Context_->GenerateId(EObjectType::TabletPartition),
            index + pivotKeyIndex,
            thisPivotKey,
            nextPivotKey);

        if (sampleKeyIndex < existingSampleKeys.Size() && existingSampleKeys[sampleKeyIndex] == thisPivotKey) {
            ++sampleKeyIndex;
        }

        YCHECK(sampleKeyIndex >= existingSampleKeys.Size() || existingSampleKeys[sampleKeyIndex] > thisPivotKey);

        std::vector<TKey> sampleKeys;
        auto rowBuffer = New<TRowBuffer>(TSampleKeyListTag());

        while (sampleKeyIndex < existingSampleKeys.Size() && existingSampleKeys[sampleKeyIndex] < nextPivotKey) {
            sampleKeys.push_back(rowBuffer->Capture(existingSampleKeys[sampleKeyIndex]));
            ++sampleKeyIndex;
        }

        partition->GetSampleKeys()->Keys = MakeSharedRange(std::move(sampleKeys), std::move(rowBuffer));
        splitPartitions.push_back(std::move(partition));
    }

    PartitionMap_.erase(existingPartition->GetId());
    for (const auto& partition : splitPartitions) {
        YCHECK(PartitionMap_.insert(std::make_pair(partition->GetId(), partition.get())).second);
    }
    PartitionList_.erase(PartitionList_.begin() + index);
    PartitionList_.insert(
        PartitionList_.begin() + index,
        std::make_move_iterator(splitPartitions.begin()),
        std::make_move_iterator(splitPartitions.end()));

    for (const auto& store : existingPartition->Stores()) {
        YCHECK(store->GetPartition() == existingPartition.get());
        auto* newPartition = GetContainingPartition(store);
        store->SetPartition(newPartition);
        YCHECK(newPartition->Stores().insert(store).second);
    }

    UpdateOverlappingStoreCount();
}

TPartition* TTablet::GetContainingPartition(
    const TOwningKey& minKey,
    const TOwningKey& maxKey)
{
    YCHECK(IsPhysicallySorted());

    auto it = std::upper_bound(
        PartitionList_.begin(),
        PartitionList_.end(),
        minKey,
        [] (const TOwningKey& key, const std::unique_ptr<TPartition>& partition) {
            return key < partition->GetPivotKey();
        });

    if (it != PartitionList_.begin()) {
        --it;
    }

    if (it + 1 == PartitionList().end()) {
        return it->get();
    }

    if ((*(it + 1))->GetPivotKey() > maxKey) {
        return it->get();
    }

    return Eden_.get();
}

const yhash_map<TStoreId, IStorePtr>& TTablet::StoreIdMap() const
{
    return StoreIdMap_;
}

const std::map<i64, IOrderedStorePtr>& TTablet::StoreRowIndexMap() const
{
    YCHECK(IsPhysicallyOrdered());
    return StoreRowIndexMap_;
}

void TTablet::AddStore(IStorePtr store)
{
    YCHECK(StoreIdMap_.insert(std::make_pair(store->GetId(), store)).second);
    if (IsPhysicallySorted()) {
        auto sortedStore = store->AsSorted();
        auto* partition = GetContainingPartition(sortedStore);
        YCHECK(partition->Stores().insert(sortedStore).second);
        sortedStore->SetPartition(partition);
        UpdateOverlappingStoreCount();
    } else {
        auto orderedStore = store->AsOrdered();
        YCHECK(StoreRowIndexMap_.insert(std::make_pair(orderedStore->GetStartingRowIndex(), orderedStore)).second);
    }
}

void TTablet::RemoveStore(IStorePtr store)
{
    YCHECK(StoreIdMap_.erase(store->GetId()) == 1);
    if (IsPhysicallySorted()) {
        auto sortedStore = store->AsSorted();
        auto* partition = sortedStore->GetPartition();
        YCHECK(partition->Stores().erase(sortedStore) == 1);
        sortedStore->SetPartition(nullptr);
        UpdateOverlappingStoreCount();
    } else {
        auto orderedStore = store->AsOrdered();
        YCHECK(StoreRowIndexMap_.erase(orderedStore->GetStartingRowIndex()) == 1);
    }
}

IStorePtr TTablet::FindStore(const TStoreId& id)
{
    auto it = StoreIdMap_.find(id);
    return it == StoreIdMap_.end() ? nullptr : it->second;
}

IStorePtr TTablet::GetStore(const TStoreId& id)
{
    auto store = FindStore(id);
    YCHECK(store);
    return store;
}

IStorePtr TTablet::GetStoreOrThrow(const TStoreId& id)
{
    auto store = FindStore(id);
    if (!store) {
        THROW_ERROR_EXCEPTION("No such store %v", id);
    }
    return store;
}

TTableReplicaInfo* TTablet::FindReplicaInfo(const TTableReplicaId& id)
{
    auto it = Replicas_.find(id);
    return it == Replicas_.end() ? nullptr : &it->second;
}

TTableReplicaInfo* TTablet::GetReplicaInfoOrThrow(const TTableReplicaId& id)
{
    auto* info = FindReplicaInfo(id);
    if (!info) {
        THROW_ERROR_EXCEPTION("No such replica %v", id);
    }
    return info;
}

bool TTablet::IsPhysicallySorted() const
{
    return PhysicalSchema_.GetKeyColumnCount() > 0;
}

bool TTablet::IsPhysicallyOrdered() const
{
    return PhysicalSchema_.GetKeyColumnCount() == 0;
}

bool TTablet::IsReplicated() const
{
    return TypeFromId(TableId_) == EObjectType::ReplicatedTable;
}

int TTablet::GetColumnLockCount() const
{
    return ColumnLockCount_;
}

i64 TTablet::GetTotalRowCount() const
{
    return RuntimeData_->TotalRowCount;
}

void TTablet::SetTotalRowCount(i64 value)
{
    RuntimeData_->TotalRowCount = value;
}

i64 TTablet::GetTrimmedRowCount() const
{
    return RuntimeData_->TrimmedRowCount;
}

void TTablet::SetTrimmedRowCount(i64 value)
{
    RuntimeData_->TrimmedRowCount = value;
}

TTimestamp TTablet::GetLastCommitTimestamp() const
{
    return RuntimeData_->LastCommitTimestamp;
}

void TTablet::SetLastCommitTimestamp(TTimestamp value)
{
    RuntimeData_->LastCommitTimestamp = value;
}

TTimestamp TTablet::GenerateMonotonicCommitTimestamp(TTimestamp hintTimestamp) const
{
    return 0;
}

void TTablet::UpdateLastCommitTimestamp(TTimestamp timestamp)
{
}

TTimestamp TTablet::GetUnflushedTimestamp() const
{
    return RuntimeData_->UnflushedTimestamp;
}

void TTablet::StartEpoch(TTabletSlotPtr slot)
{
    CancelableContext_ = New<TCancelableContext>();

    for (auto queue : TEnumTraits<EAutomatonThreadQueue>::GetDomainValues()) {
        EpochAutomatonInvokers_[queue] = CancelableContext_->CreateInvoker(
            // NB: Slot can be null in tests.
            slot
            ? slot->GetEpochAutomatonInvoker(queue)
            : GetSyncInvoker());
    }

    Eden_->StartEpoch();
    for (const auto& partition : PartitionList_) {
        partition->StartEpoch();
    }
}

void TTablet::StopEpoch()
{
    if (CancelableContext_) {
        CancelableContext_->Cancel();
        CancelableContext_.Reset();
    }

    std::fill(EpochAutomatonInvokers_.begin(), EpochAutomatonInvokers_.end(), GetNullInvoker());

    SetState(GetPersistentState());

    Eden_->StopEpoch();
    for (const auto& partition : PartitionList_) {
        partition->StopEpoch();
    }
}

IInvokerPtr TTablet::GetEpochAutomatonInvoker(EAutomatonThreadQueue queue)
{
    return EpochAutomatonInvokers_[queue];
}

TTabletSnapshotPtr TTablet::BuildSnapshot(TTabletSlotPtr slot) const
{
    auto snapshot = New<TTabletSnapshot>();

    if (slot) {
        snapshot->CellId = slot->GetCellId();
        snapshot->HydraManager = slot->GetHydraManager();
        snapshot->TabletManager = slot->GetTabletManager();
    }

    snapshot->TabletId = Id_;
    snapshot->MountRevision = MountRevision_;
    snapshot->TableId = TableId_;
    snapshot->Config = Config_;
    snapshot->WriterConfig = WriterConfig_;
    snapshot->WriterOptions = WriterOptions_;
    snapshot->PivotKey = PivotKey_;
    snapshot->NextPivotKey = NextPivotKey_;
    snapshot->TableSchema = TableSchema_;
    snapshot->PhysicalSchema = PhysicalSchema_;
    snapshot->QuerySchema = PhysicalSchema_.ToQuery();
    snapshot->Atomicity = Atomicity_;
    snapshot->HashTableSize = HashTableSize_;
    snapshot->OverlappingStoreCount = OverlappingStoreCount_;
    snapshot->RetainedTimestamp = RetainedTimestamp_;

    auto addPartitionStatistics = [&] (const TPartitionSnapshotPtr& partitionSnapshot) {
        snapshot->StoreCount += partitionSnapshot->Stores.size();
        for (const auto& store : partitionSnapshot->Stores) {
            if (store->IsChunk()) {
                auto chunkStore = store->AsChunk();
                auto preloadState = chunkStore->GetPreloadState();
                switch (preloadState) {
                    case EStorePreloadState::Scheduled:
                    case EStorePreloadState::Running:
                        ++snapshot->PreloadPendingStoreCount;
                        break;
                    case EStorePreloadState::Complete:
                        ++snapshot->PreloadCompletedStoreCount;
                        break;
                    default:
                        break;
                }
            }
        }
    };

    snapshot->Eden = Eden_->BuildSnapshot();
    addPartitionStatistics(snapshot->Eden);

    snapshot->PartitionList.reserve(PartitionList_.size());
    for (const auto& partition : PartitionList_) {
        auto partitionSnapshot = partition->BuildSnapshot();
        snapshot->PartitionList.push_back(partitionSnapshot);
        addPartitionStatistics(partitionSnapshot);
    }

    if (IsPhysicallyOrdered()) {
        // TODO(babenko): optimize
        snapshot->OrderedStores.reserve(StoreRowIndexMap_.size());
        for (const auto& pair : StoreRowIndexMap_) {
            snapshot->OrderedStores.push_back(pair.second);
        }
    }

    if (IsPhysicallySorted() && StoreManager_) {
        auto lockedStores = StoreManager_->GetLockedStores();
        for (const auto& store : lockedStores) {
            snapshot->LockedStores.push_back(store->AsSorted());
        }
    }

    snapshot->RowKeyComparer = RowKeyComparer_;
    snapshot->PerformanceCounters = PerformanceCounters_;
    snapshot->ColumnEvaluator = ColumnEvaluator_;
    snapshot->RuntimeData = RuntimeData_;

    for (const auto& pair : Replicas_) {
        YCHECK(snapshot->Replicas.emplace(pair.first, pair.second.BuildSnapshot()).second);
    }

    UpdateUnflushedTimestamp();

    return snapshot;
}

void TTablet::Initialize()
{
    PerformanceCounters_ = New<TTabletPerformanceCounters>();

    PhysicalSchema_ = IsReplicated() ? TableSchema_.ToReplicationLog() : TableSchema_;

    int keyColumnCount = PhysicalSchema_.GetKeyColumnCount();

    RowKeyComparer_ = TSortedDynamicRowKeyComparer::Create(
        keyColumnCount,
        PhysicalSchema_);

    ColumnIndexToLockIndex_.resize(PhysicalSchema_.Columns().size());
    LockIndexToName_.push_back(PrimaryLockName);

    // Assign dummy lock indexes to key components.
    for (int index = 0; index < keyColumnCount; ++index) {
        ColumnIndexToLockIndex_[index] = -1;
    }

    // Assign lock indexes to data components.
    yhash_map<Stroka, int> groupToIndex;
    for (int index = keyColumnCount; index < PhysicalSchema_.Columns().size(); ++index) {
        const auto& columnSchema = PhysicalSchema_.Columns()[index];
        int lockIndex = TSortedDynamicRow::PrimaryLockIndex;
        // No locking supported for non-atomic tablets, however we still need the primary
        // lock descriptor to maintain last commit timestamps.
        if (columnSchema.Lock && Atomicity_ == EAtomicity::Full) {
            auto it = groupToIndex.find(*columnSchema.Lock);
            if (it == groupToIndex.end()) {
                lockIndex = groupToIndex.size() + 1;
                YCHECK(groupToIndex.insert(std::make_pair(*columnSchema.Lock, lockIndex)).second);
                LockIndexToName_.push_back(*columnSchema.Lock);
            } else {
                lockIndex = it->second;
            }
        } else {
            lockIndex = TSortedDynamicRow::PrimaryLockIndex;
        }
        ColumnIndexToLockIndex_[index] = lockIndex;
    }

    ColumnLockCount_ = groupToIndex.size() + 1;

    ColumnEvaluator_ = Context_->GetColumnEvaluatorCache()->Find(PhysicalSchema_);

    StoresUpdateCommitSemaphore_ = New<NConcurrency::TAsyncSemaphore>(1);
}

TPartition* TTablet::GetContainingPartition(const ISortedStorePtr& store)
{
    // Dynamic stores must reside in Eden.
    if (store->GetStoreState() == EStoreState::ActiveDynamic ||
        store->GetStoreState() == EStoreState::PassiveDynamic)
    {
        return Eden_.get();
    }

    return GetContainingPartition(store->GetMinKey(), store->GetMaxKey());
}

const TSortedDynamicRowKeyComparer& TTablet::GetRowKeyComparer() const
{
    return RowKeyComparer_;
}

void TTablet::ValidateMountRevision(i64 mountRevision)
{
    if (MountRevision_ != mountRevision) {
        THROW_ERROR_EXCEPTION(
            NTabletClient::EErrorCode::InvalidMountRevision,
            "Invalid mount revision of tablet %v: expected %x, received %x",
            Id_,
            MountRevision_,
            mountRevision)
            << TErrorAttribute("tablet_id", Id_);
    }
}

void TTablet::UpdateOverlappingStoreCount()
{
    OverlappingStoreCount_ = 0;
    for (const auto& partition : PartitionList_) {
        OverlappingStoreCount_ = std::max(
            OverlappingStoreCount_,
            static_cast<int>(partition->Stores().size()));
    }
    OverlappingStoreCount_ += Eden_->Stores().size();
}

void TTablet::UpdateUnflushedTimestamp() const
{
    auto unflushedTimestamp = MaxTimestamp;

    for (const auto& pair : StoreIdMap()) {
        if (pair.second->IsDynamic()) {
            auto timestamp = pair.second->GetMinTimestamp();
            unflushedTimestamp = std::min(unflushedTimestamp, timestamp);
        }
    }

    if (Context_) {
        auto transactionManager = Context_->GetTransactionManager();
        if (transactionManager) {
            auto prepareTimestamp = transactionManager->GetMinPrepareTimestamp();
            auto commitTimestamp = transactionManager->GetMinCommitTimestamp();
            unflushedTimestamp = std::min({
                unflushedTimestamp,
                prepareTimestamp,
                commitTimestamp});
        }
    }

    RuntimeData_->UnflushedTimestamp = unflushedTimestamp;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT

