#include "replicated_store_manager.h"
#include "ordered_store_manager.h"
#include "tablet.h"
#include "private.h"

#include <yt/ytlib/tablet_client/wire_protocol.h>
#include <yt/ytlib/tablet_client/wire_protocol.pb.h>

#include <yt/ytlib/table_client/unversioned_row.h>

namespace NYT {
namespace NTabletNode {

using namespace NApi;
using namespace NHydra;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletClient::NProto;

////////////////////////////////////////////////////////////////////////////////

TReplicatedStoreManager::TReplicatedStoreManager(
    TTabletManagerConfigPtr config,
    TTablet* tablet,
    ITabletContext* tabletContext,
    NHydra::IHydraManagerPtr hydraManager,
    TInMemoryManagerPtr inMemoryManager,
    INativeClientPtr client)
    : Config_(config)
    , Tablet_(tablet)
    , TabletContext_(tabletContext)
    , HydraManager_(std::move(hydraManager))
    , InMemoryManager_(std::move(inMemoryManager))
    , Client_(std::move(client))
    , Logger(NLogging::TLogger(TabletNodeLogger)
        .AddTag("TabletId: %v, CellId: %v",
            Tablet_->GetId(),
            TabletContext_->GetCellId()))
    , Underlying_(New<TOrderedStoreManager>(
        Config_,
        Tablet_,
        TabletContext_,
        HydraManager_,
        InMemoryManager_,
        Client_))
{ }

TTablet* TReplicatedStoreManager::GetTablet() const
{
    return Tablet_;
}

bool TReplicatedStoreManager::HasActiveLocks() const
{
    return Underlying_->HasActiveLocks();
}

bool TReplicatedStoreManager::HasUnflushedStores() const
{
    return Underlying_->HasUnflushedStores();
}

void TReplicatedStoreManager::StartEpoch(TTabletSlotPtr slot)
{
    Underlying_->StartEpoch(std::move(slot));
}

void TReplicatedStoreManager::StopEpoch()
{
    Underlying_->StopEpoch();
}

void TReplicatedStoreManager::ExecuteAtomicWrite(
    TTransaction* transaction,
    TWireProtocolReader* reader,
    bool prelock)
{
    auto command = reader->ReadCommand();
    switch (command) {
        case EWireProtocolCommand::WriteRow: {
            auto row = reader->ReadUnversionedRow();
            WriteRow(
                transaction,
                row,
                prelock);
            break;
        }

        case EWireProtocolCommand::DeleteRow: {
            auto key = reader->ReadUnversionedRow();
            DeleteRow(
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

void TReplicatedStoreManager::ExecuteNonAtomicWrite(
    const TTransactionId& /*transactionId*/,
    TWireProtocolReader* /*reader*/)
{
    THROW_ERROR_EXCEPTION("Non-atomic writes to replicated tables are not supported");
}

bool TReplicatedStoreManager::IsOverflowRotationNeeded() const
{
    return Underlying_->IsOverflowRotationNeeded();
}

bool TReplicatedStoreManager::IsPeriodicRotationNeeded() const
{
    return Underlying_->IsPeriodicRotationNeeded();
}

bool TReplicatedStoreManager::IsRotationPossible() const
{
    return Underlying_->IsRotationPossible();
}

bool TReplicatedStoreManager::IsForcedRotationPossible() const
{
    return Underlying_->IsForcedRotationPossible();
}

bool TReplicatedStoreManager::IsRotationScheduled() const
{
    return Underlying_->IsRotationScheduled();
}

void TReplicatedStoreManager::ScheduleRotation()
{
    Underlying_->ScheduleRotation();
}

void TReplicatedStoreManager::Rotate(bool createNewStore)
{
    Underlying_->Rotate(createNewStore);
}

void TReplicatedStoreManager::AddStore(IStorePtr store, bool onMount)
{
    Underlying_->AddStore(std::move(store), onMount);
}

void TReplicatedStoreManager::RemoveStore(IStorePtr store)
{
    Underlying_->RemoveStore(std::move(store));
}

void TReplicatedStoreManager::BackoffStoreRemoval(IStorePtr store)
{
    Underlying_->BackoffStoreRemoval(std::move(store));
}

bool TReplicatedStoreManager::IsStoreLocked(IStorePtr store) const
{
    return Underlying_->IsStoreLocked(std::move(store));
}

std::vector<IStorePtr> TReplicatedStoreManager::GetLockedStores() const
{
    return Underlying_->GetLockedStores();
}

IChunkStorePtr TReplicatedStoreManager::PeekStoreForPreload()
{
    return NYT::NTabletNode::IChunkStorePtr();
}

void TReplicatedStoreManager::BeginStorePreload(IChunkStorePtr store, TCallback<TFuture<void>()> callbackFuture)
{
    Underlying_->BeginStorePreload(std::move(store), std::move(callbackFuture));
}

void TReplicatedStoreManager::EndStorePreload(IChunkStorePtr store)
{
    Underlying_->EndStorePreload(std::move(store));
}

void TReplicatedStoreManager::BackoffStorePreload(IChunkStorePtr store)
{
    Underlying_->BackoffStorePreload(std::move(store));
}

bool TReplicatedStoreManager::IsStoreFlushable(IStorePtr store) const
{
    return Underlying_->IsStoreFlushable(std::move(store));
}

TStoreFlushCallback  TReplicatedStoreManager::BeginStoreFlush(
    IDynamicStorePtr store,
    TTabletSnapshotPtr tabletSnapshot)
{
    return Underlying_->BeginStoreFlush(std::move(store), std::move(tabletSnapshot));
}

void TReplicatedStoreManager::EndStoreFlush(IDynamicStorePtr store)
{
    Underlying_->EndStoreFlush(std::move(store));
}

void TReplicatedStoreManager::BackoffStoreFlush(IDynamicStorePtr store)
{
    Underlying_->BackoffStoreFlush(std::move(store));
}

bool TReplicatedStoreManager::IsStoreCompactable(IStorePtr store) const
{
    return Underlying_->IsStoreCompactable(std::move(store));
}

void TReplicatedStoreManager::BeginStoreCompaction(IChunkStorePtr store)
{
    Underlying_->BeginStoreCompaction(std::move(store));
}

void TReplicatedStoreManager::EndStoreCompaction(IChunkStorePtr store)
{
    Underlying_->EndStoreCompaction(std::move(store));
}

void TReplicatedStoreManager::BackoffStoreCompaction(IChunkStorePtr store)
{
    Underlying_->BackoffStoreCompaction(std::move(store));
}

void TReplicatedStoreManager::Mount(
    const std::vector<NTabletNode::NProto::TAddStoreDescriptor>& storeDescriptors)
{
    Underlying_->Mount(storeDescriptors);
}

void TReplicatedStoreManager::Remount(
    TTableMountConfigPtr mountConfig,
    TTabletWriterOptionsPtr writerOptions)
{
    Underlying_->Remount(std::move(mountConfig), std::move(writerOptions));
}

ISortedStoreManagerPtr TReplicatedStoreManager::AsSorted()
{
    return this;
}

IOrderedStoreManagerPtr TReplicatedStoreManager::AsOrdered()
{
    return Underlying_;
}

bool TReplicatedStoreManager::SplitPartition(
    int /*partitionIndex*/,
    const std::vector<TOwningKey>& /*pivotKeys*/)
{
    Y_UNREACHABLE();
}

void TReplicatedStoreManager::MergePartitions(
    int /*firstPartitionIndex*/,
    int /*lastPartitionIndex*/)
{
    Y_UNREACHABLE();
}

void TReplicatedStoreManager::UpdatePartitionSampleKeys(
    TPartition* /*partition*/,
    const std::vector<TOwningKey>& /*keys*/)
{
    Y_UNREACHABLE();
}

TOrderedDynamicRowRef  TReplicatedStoreManager::WriteRow(
    TTransaction* transaction,
    TUnversionedRow row,
    bool prelock)
{
    return Underlying_->WriteRow(
        transaction,
        BuildLogRow(row, ERowModificationType::Write),
        prelock);
}

TOrderedDynamicRowRef TReplicatedStoreManager::DeleteRow(
    TTransaction* transaction,
    TKey key,
    bool prelock)
{
    return Underlying_->WriteRow(
        transaction,
        BuildLogRow(key, ERowModificationType::Delete),
        prelock);
}

TUnversionedRow TReplicatedStoreManager::BuildLogRow(
    TUnversionedRow row,
    ERowModificationType changeType)
{
    LogRowBuilder_.Reset();
    LogRowBuilder_.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 0));
    LogRowBuilder_.AddValue(MakeUnversionedInt64Value(static_cast<int>(changeType), 1));

    int keyColumnCount = Tablet_->TableSchema().GetKeyColumnCount();
    int valueColumnCount = Tablet_->TableSchema().GetValueColumnCount();

    YCHECK(row.GetCount() >= keyColumnCount);
    for (int index = 0; index < keyColumnCount; ++index) {
        auto value = row[index];
        value.Id += 2;
        LogRowBuilder_.AddValue(value);
    }

    if (changeType == ERowModificationType::Write) {
        for (int index = 0; index < valueColumnCount; ++index) {
            LogRowBuilder_.AddValue(MakeUnversionedSentinelValue(
                EValueType::Null,
                index * 2 + keyColumnCount + 2));
            LogRowBuilder_.AddValue(MakeUnversionedUint64Value(
                static_cast<ui64>(EReplicationLogDataFlags::Missing),
                index * 2 + keyColumnCount + 3));
        }
        auto logRow = LogRowBuilder_.GetRow();
        for (int index = keyColumnCount; index < row.GetCount(); ++index) {
            auto value = row[index];
            value.Id = (value.Id - keyColumnCount) * 2 + keyColumnCount + 2;
            logRow[value.Id] = value;
            logRow[value.Id + 1].Data.Uint64 &= ~static_cast<ui64>(EReplicationLogDataFlags::Missing);
        }
    }

    return LogRowBuilder_.GetRow();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
