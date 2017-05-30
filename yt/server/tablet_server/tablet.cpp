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

using namespace NTableServer;
using namespace NChunkServer;
using namespace NCellMaster;
using namespace NYTree;
using namespace NTransactionClient;

using NTabletNode::EInMemoryMode;

////////////////////////////////////////////////////////////////////////////////

void TTabletCellStatistics::Persist(NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, UnmergedRowCount);
    Persist(context, UncompressedDataSize);
    Persist(context, CompressedDataSize);
    Persist(context, MemorySize);
    Persist(context, DiskSpace);
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
}

void TTabletStatistics::Persist(NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;

    TTabletCellStatistics::Persist(context);

    Persist(context, OverlappingStoreCount);
}

TTabletCellStatistics& operator +=(TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs)
{
    lhs.UnmergedRowCount += rhs.UnmergedRowCount;
    lhs.UncompressedDataSize += rhs.UncompressedDataSize;
    lhs.CompressedDataSize += rhs.CompressedDataSize;
    lhs.MemorySize += rhs.MemorySize;
    std::transform(
        std::begin(lhs.DiskSpace),
        std::end(lhs.DiskSpace),
        std::begin(rhs.DiskSpace),
        std::begin(lhs.DiskSpace),
        std::plus<i64>());
    lhs.ChunkCount += rhs.ChunkCount;
    lhs.PartitionCount += rhs.PartitionCount;
    lhs.StoreCount += rhs.StoreCount;
    lhs.PreloadPendingStoreCount += rhs.PreloadPendingStoreCount;
    lhs.PreloadCompletedStoreCount += rhs.PreloadCompletedStoreCount;
    lhs.PreloadFailedStoreCount += rhs.PreloadFailedStoreCount;
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
        std::begin(lhs.DiskSpace),
        std::end(lhs.DiskSpace),
        std::begin(rhs.DiskSpace),
        std::begin(lhs.DiskSpace),
        std::minus<i64>());
    lhs.ChunkCount -= rhs.ChunkCount;
    lhs.PartitionCount -= rhs.PartitionCount;
    lhs.StoreCount -= rhs.StoreCount;
    lhs.PreloadPendingStoreCount -= rhs.PreloadPendingStoreCount;
    lhs.PreloadCompletedStoreCount -= rhs.PreloadCompletedStoreCount;
    lhs.PreloadFailedStoreCount -= rhs.PreloadFailedStoreCount;
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

void SerializeMembers(const TTabletCellStatistics& statistics, TFluentMap fluent)
{
    fluent
        .Item("unmerged_row_count").Value(statistics.UnmergedRowCount)
        .Item("uncompressed_data_size").Value(statistics.UncompressedDataSize)
        .Item("compressed_data_size").Value(statistics.CompressedDataSize)
        .Item("memory_size").Value(statistics.MemorySize)
        .Item("disk_space").Value(statistics.DiskSpace)
        .Item("chunk_count").Value(statistics.ChunkCount)
        .Item("partition_count").Value(statistics.PartitionCount)
        .Item("store_count").Value(statistics.StoreCount)
        .Item("preload_pending_store_count").Value(statistics.PreloadPendingStoreCount)
        .Item("preload_completed_store_count").Value(statistics.PreloadCompletedStoreCount)
        .Item("preload_failed_store_count").Value(statistics.PreloadFailedStoreCount)
        .Item("tablet_count").Value(std::accumulate(
            statistics.TabletCountPerMemoryMode.begin(),
            statistics.TabletCountPerMemoryMode.end(),
            0))
        .Item("tablet_count_per_memory_mode").Value(statistics.TabletCountPerMemoryMode);
}

void Serialize(const TTabletCellStatistics& statistics, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Do([&] (TFluentMap fluent) {
                SerializeMembers(statistics, fluent);
            })
        .EndMap();
}

void Serialize(const TTabletStatistics& statistics, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Do([&] (TFluentMap fluent) {
                SerializeMembers(statistics, fluent);
            })
            .Item("overlapping_store_count").Value(statistics.OverlappingStoreCount)
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TTabletPerformanceCounters& counters, NYson::IYsonConsumer* consumer)
{
    #define XX(name, Name) \
        .Item(#name "_count").Value(counters.Name.Count) \
        .Item(#name "_rate").Value(counters.Name.Rate)
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
}

void TTableReplicaInfo::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    Load(context, State_);
    Load(context, CurrentReplicationRowIndex_);
    Load(context, CurrentReplicationTimestamp_);
}

////////////////////////////////////////////////////////////////////////////////

TTablet::TTablet(const TTabletId& id)
    : TNonversionedObjectBase(id)
    , Index_(-1)
    , State_(ETabletState::Unmounted)
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

TDuration TTablet::ComputeReplicationLagTime(const TTableReplicaInfo& replicaInfo) const
{
    auto lastCommitTimestamp = NodeStatistics_.last_commit_timestamp();
    if (lastCommitTimestamp == NullTimestamp) {
        return TDuration::Zero();
    }
    auto replicationTimestamp = replicaInfo.GetCurrentReplicationTimestamp();
    if (replicationTimestamp >= lastCommitTimestamp) {
        return TDuration::Zero();
    }
    return TimestampToInstant(lastCommitTimestamp).second - TimestampToInstant(replicationTimestamp).first;
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

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

