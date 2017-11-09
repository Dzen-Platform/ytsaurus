#include "tablet.h"
#include "tablet_cell.h"
#include "table_replica.h"
#include "tablet_action.h"
// COMPAT(babenko)
#include "private.h"

#include <yt/server/cell_master/serialize.h>

#include <yt/server/table_server/table_node.h>

#include <yt/server/chunk_server/chunk_list.h>

#include <yt/ytlib/transaction_client/helpers.h>

#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NTabletServer {

using namespace NCellMaster;
using namespace NChunkClient;
using namespace NChunkServer;
using namespace NTableServer;
using namespace NTabletClient;
using namespace NTransactionClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

void TTabletCellStatistics::Persist(NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, UnmergedRowCount);
    Persist(context, UncompressedDataSize);
    Persist(context, CompressedDataSize);
    Persist(context, MemorySize);
    Persist(context, DiskSpacePerMedium);
    Persist(context, ChunkCount);
    Persist(context, PartitionCount);
    Persist(context, StoreCount);
    Persist(context, PreloadPendingStoreCount);
    Persist(context, PreloadCompletedStoreCount);
    Persist(context, PreloadFailedStoreCount);
    // COMPAT(savrus)
    if (context.GetVersion() >= 600) {
        Persist(context, TabletCountPerMemoryMode);
    }
    // COMPAT(savrus)
    if (context.GetVersion() >= 623) {
        Persist(context, DynamicMemoryPoolSize);
    }
}

void TTabletStatisticsBase::Persist(NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, OverlappingStoreCount);
}

void TTabletStatistics::Persist(NCellMaster::TPersistenceContext& context)
{
    TTabletCellStatistics::Persist(context);
    TTabletStatisticsBase::Persist(context);
}

TTabletCellStatistics& operator +=(TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs)
{
    lhs.UnmergedRowCount += rhs.UnmergedRowCount;
    lhs.UncompressedDataSize += rhs.UncompressedDataSize;
    lhs.CompressedDataSize += rhs.CompressedDataSize;
    lhs.MemorySize += rhs.MemorySize;
    std::transform(
        std::begin(lhs.DiskSpacePerMedium),
        std::end(lhs.DiskSpacePerMedium),
        std::begin(rhs.DiskSpacePerMedium),
        std::begin(lhs.DiskSpacePerMedium),
        std::plus<i64>());
    lhs.ChunkCount += rhs.ChunkCount;
    lhs.PartitionCount += rhs.PartitionCount;
    lhs.StoreCount += rhs.StoreCount;
    lhs.PreloadPendingStoreCount += rhs.PreloadPendingStoreCount;
    lhs.PreloadCompletedStoreCount += rhs.PreloadCompletedStoreCount;
    lhs.PreloadFailedStoreCount += rhs.PreloadFailedStoreCount;
    lhs.DynamicMemoryPoolSize += rhs.DynamicMemoryPoolSize;
    std::transform(
        std::begin(lhs.TabletCountPerMemoryMode),
        std::end(lhs.TabletCountPerMemoryMode),
        std::begin(rhs.TabletCountPerMemoryMode),
        std::begin(lhs.TabletCountPerMemoryMode),
        std::plus<i64>());
    return lhs;
}

TTabletCellStatistics operator +(const TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs)
{
    auto result = lhs;
    result += rhs;
    return result;
}

TTabletStatistics& operator +=(TTabletStatistics& lhs, const TTabletStatistics& rhs)
{
    static_cast<TTabletCellStatistics&>(lhs) += rhs;

    lhs.OverlappingStoreCount = std::max(lhs.OverlappingStoreCount, rhs.OverlappingStoreCount);
    return lhs;
}

TTabletStatistics operator +(const TTabletStatistics& lhs, const TTabletStatistics& rhs)
{
    auto result = lhs;
    result += rhs;
    return result;
}

TTabletCellStatistics& operator -=(TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs)
{
    lhs.UnmergedRowCount -= rhs.UnmergedRowCount;
    lhs.UncompressedDataSize -= rhs.UncompressedDataSize;
    lhs.CompressedDataSize -= rhs.CompressedDataSize;
    lhs.MemorySize -= rhs.MemorySize;
    std::transform(
        std::begin(lhs.DiskSpacePerMedium),
        std::end(lhs.DiskSpacePerMedium),
        std::begin(rhs.DiskSpacePerMedium),
        std::begin(lhs.DiskSpacePerMedium),
        std::minus<i64>());
    lhs.ChunkCount -= rhs.ChunkCount;
    lhs.PartitionCount -= rhs.PartitionCount;
    lhs.StoreCount -= rhs.StoreCount;
    lhs.PreloadPendingStoreCount -= rhs.PreloadPendingStoreCount;
    lhs.PreloadCompletedStoreCount -= rhs.PreloadCompletedStoreCount;
    lhs.PreloadFailedStoreCount -= rhs.PreloadFailedStoreCount;
    lhs.DynamicMemoryPoolSize -= rhs.DynamicMemoryPoolSize;
    std::transform(
        std::begin(lhs.TabletCountPerMemoryMode),
        std::end(lhs.TabletCountPerMemoryMode),
        std::begin(rhs.TabletCountPerMemoryMode),
        std::begin(lhs.TabletCountPerMemoryMode),
        std::minus<i64>());
    return lhs;
}

TTabletCellStatistics operator -(const TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs)
{
    auto result = lhs;
    result -= rhs;
    return result;
}

////////////////////////////////////////////////////////////////////////////////

TSerializableTabletCellStatistics::TSerializableTabletCellStatistics()
{
    InitParameters();
}

TSerializableTabletCellStatistics::TSerializableTabletCellStatistics(
    const TTabletCellStatistics& statistics,
    const NChunkServer::TChunkManagerPtr& chunkManager)
    : TTabletCellStatistics(statistics)
{
    InitParameters();

    TabletCount_ = std::accumulate(
        TabletCountPerMemoryMode.begin(),
        TabletCountPerMemoryMode.end(),
        0);

    DiskSpace_ = 0;
    for (const auto& pair : chunkManager->Media()) {
        const auto* medium = pair.second;
        if (medium->GetCache()) {
            continue;
        }
        int mediumIndex = medium->GetIndex();
        i64 mediumDiskSpace = DiskSpacePerMedium[mediumIndex];
        YCHECK(DiskSpacePerMediumMap_.insert(std::make_pair(medium->GetName(), mediumDiskSpace)).second);
        DiskSpace_ += mediumDiskSpace;
    }
}

void TSerializableTabletCellStatistics::InitParameters()
{
    RegisterParameter("unmerged_row_count", UnmergedRowCount);
    RegisterParameter("uncompressed_data_size", UncompressedDataSize);
    RegisterParameter("compressed_data_size", CompressedDataSize);
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
    RegisterParameter("tablet_count", TabletCount_);
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

TSerializableTabletStatistics::TSerializableTabletStatistics()
    : TSerializableTabletCellStatistics()
    , TSerializableTabletStatisticsBase()
{ }

TSerializableTabletStatistics::TSerializableTabletStatistics(
    const TTabletStatistics& statistics,
    const NChunkServer::TChunkManagerPtr& chunkManager)
    : TSerializableTabletCellStatistics(statistics, chunkManager)
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
    Save(context, Error_);
}

void TTableReplicaInfo::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, State_);
    Load(context, CurrentReplicationRowIndex_);
    Load(context, CurrentReplicationTimestamp_);
    if (context.GetVersion() >= 610) {
        Load(context, Error_);
    }
}

////////////////////////////////////////////////////////////////////////////////

TTablet::TTablet(const TTabletId& id)
    : TNonversionedObjectBase(id)
    , Index_(-1)
    , InMemoryMode_(EInMemoryMode::None)
    , RetainedTimestamp_(MinTimestamp)
{ }

void TTablet::Save(TSaveContext& context) const
{
    TNonversionedObjectBase::Save(context);

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
    Save(context, Replicas_);
    Save(context, RetainedTimestamp_);
}

void TTablet::Load(TLoadContext& context)
{
    TNonversionedObjectBase::Load(context);

    using NYT::Load;
    Load(context, Index_);
    Load(context, State_);
    Load(context, MountRevision_);
    // COMPAT(babenko)
    bool brokenPrepare = false;
    if (context.GetVersion() >= 500) {
        if (context.GetVersion() < 503) {
            if (Load<bool>(context)) {
                brokenPrepare = true;
            }
        } else {
            Load(context, StoresUpdatePreparedTransaction_);
        }
    }
    Load(context, Table_);
    Load(context, Cell_);
    // COMPAT(savrus)
    if (context.GetVersion() >= 600) {
        Load(context, Action_);
    }
    Load(context, PivotKey_);
    Load(context, NodeStatistics_);
    Load(context, InMemoryMode_);
    // COMPAT(babenko)
    if (context.GetVersion() >= 400) {
        Load(context, TrimmedRowCount_);
        Load(context, Replicas_);
        Load(context, RetainedTimestamp_);
    }
    // COMPAT(babenko)
    if (brokenPrepare) {
        const auto& Logger = TabletServerLogger;
        LOG_ERROR("Broken prepared tablet found (TabletId: %v, TableId: %v)",
            Id_,
            Table_->GetId());
    }
}

void TTablet::CopyFrom(const TTablet& other)
{
    Index_ = other.Index_;
    YCHECK(State_ == ETabletState::Unmounted);
    MountRevision_ = other.MountRevision_;
    YCHECK(!Cell_);
    PivotKey_ = other.PivotKey_;
    NodeStatistics_ = other.NodeStatistics_;
    InMemoryMode_ = other.InMemoryMode_;
    TrimmedRowCount_ = other.TrimmedRowCount_;
}

void TTablet::ValidateMountRevision(i64 mountRevision)
{
    if (MountRevision_ != mountRevision) {
        THROW_ERROR_EXCEPTION(
            NRpc::EErrorCode::Unavailable,
            "Invalid mount revision of tablet %v: expected %x, received %x",
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
    YCHECK(replicaInfo);
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
            return statistics.CompressedDataSize;
        case EInMemoryMode::Uncompressed:
            return statistics.UncompressedDataSize;
        case EInMemoryMode::None:
            return 0;
        default:
            Y_UNREACHABLE();
    }
}

i64 TTablet::GetTabletStaticMemorySize() const
{
    return GetTabletStaticMemorySize(GetInMemoryMode());
}

ETabletState TTablet::GetState() const
{
    return State_;
}

void TTablet::SetState(ETabletState state)
{
    if (Table_) {
        auto* table = Table_->GetTrunkNode();
        --table->MutableTabletCountByState()[State_];
        ++table->MutableTabletCountByState()[state];
    }
    State_ = state;
}

TTableNode* TTablet::GetTable() const
{
    return Table_;
}

void TTablet::SetTable(TTableNode* table)
{
    if (Table_) {
        --Table_->GetTrunkNode()->MutableTabletCountByState()[State_];
    }
    if (table) {
        YCHECK(table->IsTrunk());
        ++table->MutableTabletCountByState()[State_];
    }
    Table_ = table;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

