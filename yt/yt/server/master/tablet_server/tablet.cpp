#include "tablet.h"
#include "tablet_cell.h"
#include "table_replica.h"
#include "tablet_action.h"

#include <yt/yt/server/lib/tablet_server/proto/tablet_manager.pb.h>

#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/server/master/table_server/table_node.h>

#include <yt/yt/server/master/chunk_server/chunk_list.h>
#include <yt/yt/server/master/chunk_server/chunk_manager.h>
#include <yt/yt/server/master/chunk_server/medium.h>
#include <yt/yt/server/master/chunk_server/dynamic_store.h>

#include <yt/yt/client/transaction_client/helpers.h>

#include <yt/yt/core/misc/protobuf_helpers.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NTabletServer {

using namespace NCellMaster;
using namespace NChunkClient;
using namespace NChunkServer;
using namespace NTableServer;
using namespace NTabletClient;
using namespace NTransactionClient;
using namespace NYTree;
using namespace NYson;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

void TTabletCellStatisticsBase::Persist(const NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, UnmergedRowCount);
    Persist(context, UncompressedDataSize);
    Persist(context, CompressedDataSize);
    Persist(context, HunkUncompressedDataSize);
    Persist(context, HunkCompressedDataSize);
    Persist(context, MemorySize);
    Persist(context, DiskSpacePerMedium);
    Persist(context, ChunkCount);
    Persist(context, PartitionCount);
    Persist(context, StoreCount);
    Persist(context, PreloadPendingStoreCount);
    Persist(context, PreloadCompletedStoreCount);
    Persist(context, PreloadFailedStoreCount);
    Persist(context, TabletCount);
    Persist(context, TabletCountPerMemoryMode);
    Persist(context, DynamicMemoryPoolSize);
}

////////////////////////////////////////////////////////////////////////////////

void TTabletCellStatistics::Persist(const NCellMaster::TPersistenceContext& context)
{
    TTabletCellStatisticsBase::Persist(context);
}

////////////////////////////////////////////////////////////////////////////////

void TTabletStatisticsBase::Persist(const NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, OverlappingStoreCount);
}

////////////////////////////////////////////////////////////////////////////////

void TTabletStatistics::Persist(const NCellMaster::TPersistenceContext& context)
{
    TTabletCellStatisticsBase::Persist(context);
    TTabletStatisticsBase::Persist(context);
}

////////////////////////////////////////////////////////////////////////////////

TTabletStatistics TTabletStatisticsAggregate::Get() const
{
    auto statistics = CellStatistics_.Get();
    statistics.OverlappingStoreCount = OverlappingStoreCount_.Get();
    return statistics;
}

void TTabletStatisticsAggregate::Account(const TTabletStatistics& tabletStatistics)
{
    CellStatistics_.Account(tabletStatistics);
    OverlappingStoreCount_.Account(tabletStatistics.OverlappingStoreCount);
}

void TTabletStatisticsAggregate::Discount(const TTabletStatistics& tabletStatistics)
{
    CellStatistics_.Discount(tabletStatistics);
    OverlappingStoreCount_.Discount(tabletStatistics.OverlappingStoreCount);
}

void TTabletStatisticsAggregate::AccountDelta(const TTabletStatistics& tabletStatistics)
{
    CellStatistics_.AccountDelta(tabletStatistics);

    YT_VERIFY(tabletStatistics.OverlappingStoreCount == 0);
}

void TTabletStatisticsAggregate::Reset()
{
    CellStatistics_.Reset();
    OverlappingStoreCount_.Reset();
}

void TTabletStatisticsAggregate::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;

    Save(context, CellStatistics_);
    Save(context, OverlappingStoreCount_);
}

void TTabletStatisticsAggregate::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, CellStatistics_);
    Load(context, OverlappingStoreCount_);
}

////////////////////////////////////////////////////////////////////////////////

TTabletCellStatisticsBase& operator += (TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs)
{
    lhs.UnmergedRowCount += rhs.UnmergedRowCount;
    lhs.UncompressedDataSize += rhs.UncompressedDataSize;
    lhs.CompressedDataSize += rhs.CompressedDataSize;
    lhs.HunkUncompressedDataSize += rhs.HunkUncompressedDataSize;
    lhs.HunkCompressedDataSize += rhs.HunkCompressedDataSize;
    lhs.MemorySize += rhs.MemorySize;
    for (const auto& [mediumIndex, diskSpace] : rhs.DiskSpacePerMedium) {
        lhs.DiskSpacePerMedium[mediumIndex] += diskSpace;
    }
    lhs.ChunkCount += rhs.ChunkCount;
    lhs.PartitionCount += rhs.PartitionCount;
    lhs.StoreCount += rhs.StoreCount;
    lhs.PreloadPendingStoreCount += rhs.PreloadPendingStoreCount;
    lhs.PreloadCompletedStoreCount += rhs.PreloadCompletedStoreCount;
    lhs.PreloadFailedStoreCount += rhs.PreloadFailedStoreCount;
    lhs.DynamicMemoryPoolSize += rhs.DynamicMemoryPoolSize;
    lhs.TabletCount += rhs.TabletCount;
    std::transform(
        std::begin(lhs.TabletCountPerMemoryMode),
        std::end(lhs.TabletCountPerMemoryMode),
        std::begin(rhs.TabletCountPerMemoryMode),
        std::begin(lhs.TabletCountPerMemoryMode),
        std::plus<i64>());
    return lhs;
}

TTabletCellStatisticsBase operator + (const TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs)
{
    auto result = lhs;
    result += rhs;
    return result;
}

TTabletCellStatisticsBase& operator -= (TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs)
{
    lhs.UnmergedRowCount -= rhs.UnmergedRowCount;
    lhs.UncompressedDataSize -= rhs.UncompressedDataSize;
    lhs.CompressedDataSize -= rhs.CompressedDataSize;
    lhs.HunkUncompressedDataSize -= rhs.HunkUncompressedDataSize;
    lhs.HunkCompressedDataSize -= rhs.HunkCompressedDataSize;
    lhs.MemorySize -= rhs.MemorySize;
    for (const auto& [mediumIndex, diskSpace] : rhs.DiskSpacePerMedium) {
        lhs.DiskSpacePerMedium[mediumIndex] -= diskSpace;
    }
    lhs.ChunkCount -= rhs.ChunkCount;
    lhs.PartitionCount -= rhs.PartitionCount;
    lhs.StoreCount -= rhs.StoreCount;
    lhs.PreloadPendingStoreCount -= rhs.PreloadPendingStoreCount;
    lhs.PreloadCompletedStoreCount -= rhs.PreloadCompletedStoreCount;
    lhs.PreloadFailedStoreCount -= rhs.PreloadFailedStoreCount;
    lhs.DynamicMemoryPoolSize -= rhs.DynamicMemoryPoolSize;
    lhs.TabletCount -= rhs.TabletCount;
    std::transform(
        std::begin(lhs.TabletCountPerMemoryMode),
        std::end(lhs.TabletCountPerMemoryMode),
        std::begin(rhs.TabletCountPerMemoryMode),
        std::begin(lhs.TabletCountPerMemoryMode),
        std::minus<i64>());
    return lhs;
}

TTabletCellStatisticsBase operator - (const TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs)
{
    auto result = lhs;
    result -= rhs;
    return result;
}

bool operator == (const TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs)
{
    return
        lhs.UnmergedRowCount == rhs.UnmergedRowCount &&
        lhs.UncompressedDataSize == rhs.UncompressedDataSize &&
        lhs.CompressedDataSize == rhs.CompressedDataSize &&
        lhs.HunkUncompressedDataSize == rhs.HunkUncompressedDataSize &&
        lhs.HunkCompressedDataSize == rhs.HunkCompressedDataSize &&
        lhs.MemorySize == rhs.MemorySize &&
        lhs.DynamicMemoryPoolSize == rhs.DynamicMemoryPoolSize &&
        lhs.ChunkCount == rhs.ChunkCount &&
        lhs.PartitionCount == rhs.PartitionCount &&
        lhs.StoreCount == rhs.StoreCount &&
        lhs.PreloadPendingStoreCount == rhs.PreloadPendingStoreCount &&
        lhs.PreloadCompletedStoreCount == rhs.PreloadCompletedStoreCount &&
        lhs.PreloadFailedStoreCount == rhs.PreloadFailedStoreCount &&
        lhs.TabletCount == rhs.TabletCount &&
        std::equal(
            lhs.TabletCountPerMemoryMode.begin(),
            lhs.TabletCountPerMemoryMode.end(),
            rhs.TabletCountPerMemoryMode.begin()) &&
        lhs.DiskSpacePerMedium.size() == rhs.DiskSpacePerMedium.size() &&
        std::all_of(
            lhs.DiskSpacePerMedium.begin(),
            lhs.DiskSpacePerMedium.end(),
            [&] (const TMediumMap<i64>::value_type& value) {
                auto it = rhs.DiskSpacePerMedium.find(value.first);
                return it != rhs.DiskSpacePerMedium.end() && it->second == value.second;
            });
}

bool operator != (const TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs)
{
    return !(lhs == rhs);
}

TTabletCellStatistics& operator += (TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs)
{
    static_cast<TTabletCellStatisticsBase&>(lhs) += rhs;
    return lhs;
}

TTabletCellStatistics operator + (const TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs)
{
    auto result = lhs;
    result += rhs;
    return result;
}

TTabletCellStatistics& operator -= (TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs)
{
    static_cast<TTabletCellStatisticsBase&>(lhs) += rhs;
    return lhs;
}

TTabletCellStatistics operator - (const TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs)
{
    auto result = lhs;
    result -= rhs;
    return result;
}

TTabletStatistics& operator += (TTabletStatistics& lhs, const TTabletStatistics& rhs)
{
    static_cast<TTabletCellStatisticsBase&>(lhs) += rhs;

    lhs.OverlappingStoreCount = std::max(lhs.OverlappingStoreCount, rhs.OverlappingStoreCount);
    return lhs;
}

TTabletStatistics operator + (const TTabletStatistics& lhs, const TTabletStatistics& rhs)
{
    auto result = lhs;
    result += rhs;
    return result;
}

TTabletStatistics& operator -= (TTabletStatistics& lhs, const TTabletStatistics& rhs)
{
    static_cast<TTabletCellStatisticsBase&>(lhs) -= rhs;

    // Overlapping store count cannot be subtracted.

    return lhs;
}

TTabletStatistics operator - (const TTabletStatistics& lhs, const TTabletStatistics& rhs)
{
    auto result = lhs;
    result -= rhs;
    return result;
}

bool operator == (const TTabletStatistics& lhs, const TTabletStatistics& rhs)
{
    return static_cast<const TTabletCellStatisticsBase&>(lhs) == static_cast<const TTabletCellStatisticsBase&>(rhs) &&
        lhs.OverlappingStoreCount == rhs.OverlappingStoreCount;
}

bool operator != (const TTabletStatistics& lhs, const TTabletStatistics& rhs)
{
    return !(lhs == rhs);
}

void ToProto(NProto::TTabletCellStatistics* protoStatistics, const TTabletCellStatistics& statistics)
{
    protoStatistics->set_unmerged_row_count(statistics.UnmergedRowCount);
    protoStatistics->set_uncompressed_data_size(statistics.UncompressedDataSize);
    protoStatistics->set_compressed_data_size(statistics.CompressedDataSize);
    protoStatistics->set_hunk_uncompressed_data_size(statistics.HunkUncompressedDataSize);
    protoStatistics->set_hunk_compressed_data_size(statistics.HunkCompressedDataSize);
    protoStatistics->set_memory_size(statistics.MemorySize);
    protoStatistics->set_chunk_count(statistics.ChunkCount);
    protoStatistics->set_partition_count(statistics.PartitionCount);
    protoStatistics->set_store_count(statistics.StoreCount);
    protoStatistics->set_preload_pending_store_count(statistics.PreloadPendingStoreCount);
    protoStatistics->set_preload_completed_store_count(statistics.PreloadCompletedStoreCount);
    protoStatistics->set_preload_failed_store_count(statistics.PreloadFailedStoreCount);
    protoStatistics->set_dynamic_memory_pool_size(statistics.DynamicMemoryPoolSize);
    protoStatistics->set_tablet_count(statistics.TabletCount);

    // COMPAT(aozeritsky)
    const auto oldMaxMediumCount = 7;
    i64 oldDiskSpacePerMedium[oldMaxMediumCount] = {};
    for (const auto& [mediumIndex, diskSpace] : statistics.DiskSpacePerMedium) {
        if (mediumIndex < oldMaxMediumCount) {
            oldDiskSpacePerMedium[mediumIndex] = diskSpace;
        }

        auto* item = protoStatistics->add_disk_space_per_medium();
        item->set_medium_index(mediumIndex);
        item->set_disk_space(diskSpace);
    }

    ToProto(protoStatistics->mutable_disk_space_per_medium_old(), TRange<i64>(oldDiskSpacePerMedium, oldMaxMediumCount));
    ToProto(protoStatistics->mutable_tablet_count_per_memory_mode(), statistics.TabletCountPerMemoryMode);
}

void FromProto(TTabletCellStatistics* statistics, const NProto::TTabletCellStatistics& protoStatistics)
{
    statistics->UnmergedRowCount = protoStatistics.unmerged_row_count();
    statistics->UncompressedDataSize = protoStatistics.uncompressed_data_size();
    statistics->CompressedDataSize = protoStatistics.compressed_data_size();
    statistics->HunkUncompressedDataSize = protoStatistics.hunk_uncompressed_data_size();
    statistics->HunkCompressedDataSize = protoStatistics.hunk_compressed_data_size();
    statistics->MemorySize = protoStatistics.memory_size();
    statistics->ChunkCount = protoStatistics.chunk_count();
    statistics->PartitionCount = protoStatistics.partition_count();
    statistics->StoreCount = protoStatistics.store_count();
    statistics->PreloadPendingStoreCount = protoStatistics.preload_pending_store_count();
    statistics->PreloadCompletedStoreCount = protoStatistics.preload_completed_store_count();
    statistics->PreloadFailedStoreCount = protoStatistics.preload_failed_store_count();
    statistics->DynamicMemoryPoolSize = protoStatistics.dynamic_memory_pool_size();
    statistics->TabletCount = protoStatistics.tablet_count();

    // COMPAT(aozeritsky)
    const auto oldMaxMediumCount = 7;
    i64 oldDiskSpacePerMedium[oldMaxMediumCount] = {};
    auto diskSpacePerMedium = TMutableRange<i64>(oldDiskSpacePerMedium, oldMaxMediumCount);
    FromProto(&diskSpacePerMedium, protoStatistics.disk_space_per_medium_old());
    for (auto i = 0; i < oldMaxMediumCount; ++i) {
        if (oldDiskSpacePerMedium[i] > 0) {
            statistics->DiskSpacePerMedium[i] = oldDiskSpacePerMedium[i];
        }
    }
    for (auto& item : protoStatistics.disk_space_per_medium()) {
        statistics->DiskSpacePerMedium[item.medium_index()] = item.disk_space();
    }
    FromProto(&statistics->TabletCountPerMemoryMode, protoStatistics.tablet_count_per_memory_mode());
}

TString ToString(const TTabletStatistics& tabletStatistics, const TChunkManagerPtr& chunkManager)
{
    TStringStream output;
    TYsonWriter writer(&output, EYsonFormat::Text);
    New<TSerializableTabletStatistics>(tabletStatistics, chunkManager)->Save(&writer);
    writer.Flush();
    return output.Str();
}

////////////////////////////////////////////////////////////////////////////////

TSerializableTabletCellStatisticsBase::TSerializableTabletCellStatisticsBase()
{
    InitParameters();
}

TSerializableTabletCellStatisticsBase::TSerializableTabletCellStatisticsBase(
    const TTabletCellStatisticsBase& statistics,
    const NChunkServer::TChunkManagerPtr& chunkManager)
    : TTabletCellStatisticsBase(statistics)
{
    InitParameters();

    DiskSpace_ = 0;
    for (const auto& [mediumIndex, mediumDiskSpace] : DiskSpacePerMedium) {
        const auto* medium = chunkManager->FindMediumByIndex(mediumIndex);
        if (medium->GetCache()) {
            continue;
        }
        YT_VERIFY(DiskSpacePerMediumMap_.emplace(medium->GetName(), mediumDiskSpace).second);
        DiskSpace_ += mediumDiskSpace;
    }
}

void TSerializableTabletCellStatisticsBase::InitParameters()
{
    RegisterParameter("unmerged_row_count", UnmergedRowCount);
    RegisterParameter("uncompressed_data_size", UncompressedDataSize);
    RegisterParameter("compressed_data_size", CompressedDataSize);
    RegisterParameter("hunk_uncompressed_data_size", HunkUncompressedDataSize);
    RegisterParameter("hunk_compressed_data_size", HunkCompressedDataSize);
    RegisterParameter("memory_size", MemorySize);
    RegisterParameter("disk_space", DiskSpace_);
    RegisterParameter("disk_space_per_medium", DiskSpacePerMediumMap_);
    RegisterParameter("chunk_count", ChunkCount);
    RegisterParameter("partition_count", PartitionCount);
    RegisterParameter("store_count", StoreCount);
    RegisterParameter("preload_pending_store_count", PreloadPendingStoreCount);
    RegisterParameter("preload_completed_store_count", PreloadCompletedStoreCount);
    RegisterParameter("preload_failed_store_count", PreloadFailedStoreCount);
    RegisterParameter("dynamic_memory_pool_size", DynamicMemoryPoolSize);
    RegisterParameter("tablet_count", TabletCount);
    RegisterParameter("tablet_count_per_memory_mode", TabletCountPerMemoryMode);
}

TSerializableTabletStatisticsBase::TSerializableTabletStatisticsBase()
{
    InitParameters();
}

TSerializableTabletStatisticsBase::TSerializableTabletStatisticsBase(
    const TTabletStatisticsBase& statistics)
    : TTabletStatisticsBase(statistics)
{
    InitParameters();
}

void TSerializableTabletStatisticsBase::InitParameters()
{
    RegisterParameter("overlapping_store_count", OverlappingStoreCount);
}

TSerializableTabletCellStatistics::TSerializableTabletCellStatistics()
    : TSerializableTabletCellStatisticsBase()
{ }

TSerializableTabletCellStatistics::TSerializableTabletCellStatistics(
    const TTabletCellStatistics& statistics,
    const NChunkServer::TChunkManagerPtr& chunkManager)
    : TSerializableTabletCellStatisticsBase(statistics, chunkManager)
{ }

TSerializableTabletStatistics::TSerializableTabletStatistics()
    : TSerializableTabletCellStatisticsBase()
    , TSerializableTabletStatisticsBase()
{ }

TSerializableTabletStatistics::TSerializableTabletStatistics(
    const TTabletStatistics& statistics,
    const NChunkServer::TChunkManagerPtr& chunkManager)
    : TSerializableTabletCellStatisticsBase(statistics, chunkManager)
    , TSerializableTabletStatisticsBase(statistics)
{ }

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TTabletPerformanceCounters& counters, NYson::IYsonConsumer* consumer)
{
    #define XX(name, Name) \
        .Item(#name "_count").Value(counters.Name.Count) \
        .Item(#name "_rate").Value(counters.Name.Rate) \
        .Item(#name "_10m_rate").Value(counters.Name.Rate10) \
        .Item(#name "_1h_rate").Value(counters.Name.Rate60)
    BuildYsonFluently(consumer)
        .BeginMap()
            ITERATE_TABLET_PERFORMANCE_COUNTERS(XX)
        .EndMap();
    #undef XX
}

////////////////////////////////////////////////////////////////////////////////

void TTableReplicaInfo::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;

    Save(context, State_);
    Save(context, CurrentReplicationRowIndex_);
    Save(context, CurrentReplicationTimestamp_);
    Save(context, HasError_);
}

void TTableReplicaInfo::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, State_);
    Load(context, CurrentReplicationRowIndex_);
    Load(context, CurrentReplicationTimestamp_);
    Load(context, HasError_);
}

////////////////////////////////////////////////////////////////////////////////

TTablet::TTablet(TTabletId id)
    : TObject(id)
    , Index_(-1)
    , InMemoryMode_(EInMemoryMode::None)
    , RetainedTimestamp_(MinTimestamp)
{ }

TString TTablet::GetLowercaseObjectName() const
{
    return Format("tablet %v", GetId());
}

TString TTablet::GetCapitalizedObjectName() const
{
    return Format("Tablet %v", GetId());
}

void TTablet::Save(TSaveContext& context) const
{
    TObject::Save(context);

    using NYT::Save;
    Save(context, Index_);
    Save(context, State_);
    Save(context, MountRevision_);
    Save(context, StoresUpdatePreparedTransaction_);
    Save(context, Table_);
    Save(context, Cell_);
    Save(context, Action_);
    Save(context, PivotKey_);
    Save(context, NodeStatistics_);
    Save(context, InMemoryMode_);
    Save(context, TrimmedRowCount_);
    Save(context, WasForcefullyUnmounted_);
    Save(context, Replicas_);
    Save(context, RetainedTimestamp_);
    Save(context, TabletErrorCount_);
    Save(context, ReplicationErrorCount_);
    Save(context, ExpectedState_);
    Save(context, UnconfirmedDynamicTableLocks_);
    Save(context, EdenStoreIds_);
    Save(context, BackupState_);
    Save(context, DynamicStores_);
    Save(context, ReplicationProgress_);
}

void TTablet::Load(TLoadContext& context)
{
    TObject::Load(context);

    using NYT::Load;
    Load(context, Index_);
    Load(context, State_);
    Load(context, MountRevision_);
    Load(context, StoresUpdatePreparedTransaction_);
    Load(context, Table_);
    Load(context, Cell_);
    Load(context, Action_);
    Load(context, PivotKey_);
    Load(context, NodeStatistics_);
    Load(context, InMemoryMode_);
    Load(context, TrimmedRowCount_);
    // COMPAT(ifsmirnov)
    if (context.GetVersion() >= EMasterReign::SaveForcefullyUnmountedTablets) {
        Load(context, WasForcefullyUnmounted_);
    }
    Load(context, Replicas_);
    Load(context, RetainedTimestamp_);
    Load(context, TabletErrorCount_);
    Load(context, ReplicationErrorCount_);
    Load(context, ExpectedState_);
    Load(context, UnconfirmedDynamicTableLocks_);
    Load(context, EdenStoreIds_);
    // COMPAT(ifsmirnov)
    if (context.GetVersion() >= EMasterReign::BackupsInitial) {
        Load(context, BackupState_);
    }
    // COMPAT(ifsmirnov)
    if (context.GetVersion() >= EMasterReign::RefFromTabletToDynamicStore) {
        Load(context, DynamicStores_);
    }
    // COMPAT(savrus)
    if (context.GetVersion() >= EMasterReign::ChaosDataTransfer) {
        Load(context, ReplicationProgress_);
    }
}

void TTablet::CopyFrom(const TTablet& other)
{
    Index_ = other.Index_;
    YT_VERIFY(State_ == ETabletState::Unmounted);
    MountRevision_ = other.MountRevision_;
    YT_VERIFY(!Cell_);
    PivotKey_ = other.PivotKey_;
    InMemoryMode_ = other.InMemoryMode_;
    TrimmedRowCount_ = other.TrimmedRowCount_;
    EdenStoreIds_ = other.EdenStoreIds_;
}

void TTablet::ValidateMountRevision(NHydra::TRevision mountRevision)
{
    if (MountRevision_ != mountRevision) {
        THROW_ERROR_EXCEPTION(
            NRpc::EErrorCode::Unavailable,
            "Invalid mount revision of tablet %v: expected %llx, received %llx",
            Id_,
            MountRevision_,
            mountRevision);
    }
}

TTableReplicaInfo* TTablet::FindReplicaInfo(const TTableReplica* replica)
{
    auto it = Replicas_.find(const_cast<TTableReplica*>(replica));
    return it == Replicas_.end() ? nullptr : &it->second;
}

TTableReplicaInfo* TTablet::GetReplicaInfo(const TTableReplica* replica)
{
    auto* replicaInfo = FindReplicaInfo(replica);
    YT_VERIFY(replicaInfo);
    return replicaInfo;
}

TDuration TTablet::ComputeReplicationLagTime(TTimestamp latestTimestamp, const TTableReplicaInfo& replicaInfo) const
{
    auto lastWriteTimestamp = NodeStatistics_.last_write_timestamp();
    if (lastWriteTimestamp == NullTimestamp) {
        return TDuration::Zero();
    }
    auto replicationTimestamp = replicaInfo.GetCurrentReplicationTimestamp();
    if (replicationTimestamp >= lastWriteTimestamp || replicationTimestamp >= latestTimestamp) {
        return TDuration::Zero();
    }
    return TimestampToInstant(latestTimestamp).second - TimestampToInstant(replicationTimestamp).first;
}

bool TTablet::IsActive() const
{
    return
        State_ == ETabletState::Mounting ||
        State_ == ETabletState::FrozenMounting ||
        State_ == ETabletState::Mounted ||
        State_ == ETabletState::Freezing ||
        State_ == ETabletState::Frozen ||
        State_ == ETabletState::Unfreezing;
}

TChunkList* TTablet::GetChunkList()
{
    return Table_->GetTrunkNode()->GetChunkList()->Children()[Index_]->AsChunkList();
}

const TChunkList* TTablet::GetChunkList() const
{
    return const_cast<TTablet*>(this)->GetChunkList();
}

i64 TTablet::GetTabletStaticMemorySize(EInMemoryMode mode) const
{
    // TODO(savrus) consider lookup hash table.

    const auto& statistics = GetChunkList()->Statistics();
    switch (mode) {
        case EInMemoryMode::Compressed:
            return statistics.CompressedDataSize - GetHunkCompressedDataSize();
        case EInMemoryMode::Uncompressed:
            return statistics.UncompressedDataSize - GetHunkUncompressedDataSize();
        case EInMemoryMode::None:
            return 0;
        default:
            YT_ABORT();
    }
}

i64 TTablet::GetTabletStaticMemorySize() const
{
    return GetTabletStaticMemorySize(GetInMemoryMode());
}

i64 TTablet::GetTabletMasterMemoryUsage() const
{
    return sizeof(TTablet) + GetDataWeight(GetPivotKey()) + EdenStoreIds_.size() * sizeof(TStoreId);
}

i64 TTablet::GetHunkUncompressedDataSize() const
{
    const auto* hunkChunkList = GetChunkList()->GetHunkRootChild();
    return hunkChunkList ? hunkChunkList->Statistics().UncompressedDataSize : 0;
}

i64 TTablet::GetHunkCompressedDataSize() const
{
    const auto* hunkChunkList = GetChunkList()->GetHunkRootChild();
    return hunkChunkList ? hunkChunkList->Statistics().CompressedDataSize : 0;
}

ETabletState TTablet::GetState() const
{
    return State_;
}

void TTablet::SetState(ETabletState state)
{
    if (Table_) {
        auto* table = Table_->GetTrunkNode();
        YT_VERIFY(table->TabletCountByState()[State_] > 0);
        --table->MutableTabletCountByState()[State_];
        ++table->MutableTabletCountByState()[state];
    }

    if (!Action_) {
        SetExpectedState(state);
    }

    State_ = state;
}

ETabletBackupState TTablet::GetBackupState() const
{
    return BackupState_;
}

void TTablet::SetBackupState(ETabletBackupState state)
{
    if (Table_) {
        auto* table = Table_->GetTrunkNode();
        YT_VERIFY(table->TabletCountByBackupState()[BackupState_] > 0);
        --table->MutableTabletCountByBackupState()[BackupState_];
        ++table->MutableTabletCountByBackupState()[state];
    }

    BackupState_ = state;
}

void TTablet::CheckedSetBackupState(ETabletBackupState previous, ETabletBackupState next)
{
    YT_VERIFY(BackupState_ == previous);
    SetBackupState(next);
}

ETabletState TTablet::GetExpectedState() const
{
    return ExpectedState_;
}

void TTablet::SetExpectedState(ETabletState state)
{
    if (Table_) {
        auto* table = Table_->GetTrunkNode();
        YT_VERIFY(table->TabletCountByExpectedState()[ExpectedState_] > 0);
        --table->MutableTabletCountByExpectedState()[ExpectedState_];
        ++table->MutableTabletCountByExpectedState()[state];
    }
    ExpectedState_ = state;
}

TTableNode* TTablet::GetTable() const
{
    return Table_;
}

void TTablet::SetTable(TTableNode* table)
{
    if (Table_) {
        YT_VERIFY(Table_->GetTrunkNode()->TabletCountByState()[State_] > 0);
        YT_VERIFY(Table_->GetTrunkNode()->TabletCountByExpectedState()[ExpectedState_] > 0);
        YT_VERIFY(Table_->GetTrunkNode()->TabletCountByBackupState()[BackupState_] > 0);
        --Table_->GetTrunkNode()->MutableTabletCountByState()[State_];
        --Table_->GetTrunkNode()->MutableTabletCountByExpectedState()[ExpectedState_];
        --Table_->GetTrunkNode()->MutableTabletCountByBackupState()[BackupState_];

        int restTabletErrorCount = Table_->GetTabletErrorCount() - GetTabletErrorCount();
        YT_ASSERT(restTabletErrorCount >= 0);
        Table_->SetTabletErrorCount(restTabletErrorCount);
    }
    if (table) {
        YT_VERIFY(table->IsTrunk());
        ++table->MutableTabletCountByState()[State_];
        ++table->MutableTabletCountByExpectedState()[ExpectedState_];
        ++table->MutableTabletCountByBackupState()[BackupState_];

        table->SetTabletErrorCount(table->GetTabletErrorCount() + GetTabletErrorCount());
    }
    Table_ = table;
}

void TTablet::SetTabletErrorCount(int tabletErrorCount)
{
    if (Table_) {
        int restTabletErrorCount = Table_->GetTabletErrorCount() - GetTabletErrorCount();
        YT_ASSERT(restTabletErrorCount >= 0);
        Table_->SetTabletErrorCount(restTabletErrorCount + tabletErrorCount);
    }

    TabletErrorCount_ = tabletErrorCount;
}

int TTablet::GetTabletErrorCount() const
{
    return TabletErrorCount_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer

