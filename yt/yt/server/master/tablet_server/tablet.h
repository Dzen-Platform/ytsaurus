#pragma once

#include "public.h"

#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/server/master/object_server/object.h>

#include <yt/yt/server/master/table_server/public.h>

#include <yt/yt/server/master/transaction_server/public.h>

#include <yt/yt/server/master/chunk_server/public.h>

#include <yt/yt/server/lib/tablet_node/public.h>

#include <yt/yt/server/lib/tablet_node/proto/tablet_manager.pb.h>

#include <yt/yt/ytlib/tablet_client/backup.h>

#include <yt/yt/ytlib/tablet_client/proto/heartbeat.pb.h>

#include <yt/yt/client/chaos_client/replication_card.h>

#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/core/misc/aggregate_property.h>
#include <yt/yt/core/misc/enum.h>
#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/property.h>
#include <yt/yt/core/misc/ref_tracked.h>

#include <yt/yt/core/ytree/yson_serializable.h>

namespace NYT::NTabletServer {

////////////////////////////////////////////////////////////////////////////////

struct TTabletCellStatisticsBase
{
    i64 UnmergedRowCount = 0;
    i64 UncompressedDataSize = 0;
    i64 CompressedDataSize = 0;
    i64 HunkUncompressedDataSize = 0;
    i64 HunkCompressedDataSize = 0;
    i64 MemorySize = 0;
    i64 DynamicMemoryPoolSize = 0;
    NChunkClient::TMediumMap<i64> DiskSpacePerMedium = NChunkClient::TMediumMap<i64>();
    int ChunkCount = 0;
    int PartitionCount = 0;
    int StoreCount = 0;
    int PreloadPendingStoreCount = 0;
    int PreloadCompletedStoreCount = 0;
    int PreloadFailedStoreCount = 0;
    int TabletCount = 0;
    TEnumIndexedVector<NTabletClient::EInMemoryMode, int> TabletCountPerMemoryMode;

    void Persist(const NCellMaster::TPersistenceContext& context);
};

struct TTabletCellStatistics
    : public TTabletCellStatisticsBase
{
    void Persist(const NCellMaster::TPersistenceContext& context);
};

struct TTabletStatisticsBase
{
    int OverlappingStoreCount = 0;

    void Persist(const NCellMaster::TPersistenceContext& context);
};

struct TTabletStatistics
    : public TTabletCellStatisticsBase
    , public TTabletStatisticsBase
{
    void Persist(const NCellMaster::TPersistenceContext& context);
};

class TTabletStatisticsAggregate
    : public TNonCopyable
{
public:
    TTabletStatistics Get() const;

    void Account(const TTabletStatistics& tabletStatistics);
    void Discount(const TTabletStatistics& tabletStatistics);
    void AccountDelta(const TTabletStatistics& tabletStatistics);

    void Reset();

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

private:
    TSumAggregate<TTabletStatistics> CellStatistics_;
    TMaxAggregate<int> OverlappingStoreCount_{0};
};

////////////////////////////////////////////////////////////////////////////////

TTabletCellStatisticsBase& operator += (TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs);
TTabletCellStatisticsBase  operator +  (const TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs);

TTabletCellStatisticsBase& operator -= (TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs);
TTabletCellStatisticsBase  operator -  (const TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs);

bool operator == (const TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs);
bool operator != (const TTabletCellStatisticsBase& lhs, const TTabletCellStatisticsBase& rhs);

TTabletCellStatistics& operator += (TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs);
TTabletCellStatistics  operator +  (const TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs);

TTabletCellStatistics& operator -= (TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs);
TTabletCellStatistics  operator -  (const TTabletCellStatistics& lhs, const TTabletCellStatistics& rhs);

TTabletStatistics& operator += (TTabletStatistics& lhs, const TTabletStatistics& rhs);
TTabletStatistics  operator +  (const TTabletStatistics& lhs, const TTabletStatistics& rhs);

TTabletStatistics& operator += (TTabletStatistics& lhs, const TTabletStatistics& rhs);
TTabletStatistics  operator +  (const TTabletStatistics& lhs, const TTabletStatistics& rhs);

bool operator == (const TTabletStatistics& lhs, const TTabletStatistics& rhs);
bool operator != (const TTabletStatistics& lhs, const TTabletStatistics& rhs);

void ToProto(NProto::TTabletCellStatistics* protoStatistics, const TTabletCellStatistics& statistics);
void FromProto(TTabletCellStatistics* statistics, const NProto::TTabletCellStatistics& protoStatistics);

TString ToString(const TTabletStatistics& statistics, const NChunkServer::TChunkManagerPtr& chunkManager);

////////////////////////////////////////////////////////////////////////////////

// COMPAT(akozhikhov)
class TSerializableTabletCellStatisticsBase
    : public virtual NYTree::TYsonSerializable
    , public TTabletCellStatisticsBase
{
public:
    TSerializableTabletCellStatisticsBase();

    TSerializableTabletCellStatisticsBase(
        const TTabletCellStatisticsBase& statistics,
        const NChunkServer::TChunkManagerPtr& chunkManager);

private:
    i64 DiskSpace_ = 0;
    THashMap<TString, i64> DiskSpacePerMediumMap_;

    void InitParameters();
};

class TSerializableTabletStatisticsBase
    : public virtual NYTree::TYsonSerializable
    , public TTabletStatisticsBase
{
public:
    TSerializableTabletStatisticsBase();

    explicit TSerializableTabletStatisticsBase(const TTabletStatisticsBase& statistics);

private:
    void InitParameters();
};

class TSerializableTabletCellStatistics
    : public TSerializableTabletCellStatisticsBase
{
public:
    TSerializableTabletCellStatistics();

    TSerializableTabletCellStatistics(
        const TTabletCellStatistics& statistics,
        const NChunkServer::TChunkManagerPtr& chunkManager);
};

class TSerializableTabletStatistics
    : public TSerializableTabletCellStatisticsBase
    , public TSerializableTabletStatisticsBase
{
public:
    TSerializableTabletStatistics();

    TSerializableTabletStatistics(
        const TTabletStatistics& statistics,
        const NChunkServer::TChunkManagerPtr& chunkManager);
};

////////////////////////////////////////////////////////////////////////////////

struct TTabletPerformanceCounter
{
    i64 Count = 0;
    double Rate = 0.0;
    double Rate10 = 0.0;
    double Rate60 = 0.0;
};

#define ITERATE_TABLET_PERFORMANCE_COUNTERS(XX) \
    XX(dynamic_row_read,                        DynamicRowRead) \
    XX(dynamic_row_read_data_weight,            DynamicRowReadDataWeight) \
    XX(dynamic_row_lookup,                      DynamicRowLookup) \
    XX(dynamic_row_lookup_data_weight,          DynamicRowLookupDataWeight) \
    XX(dynamic_row_write,                       DynamicRowWrite) \
    XX(dynamic_row_write_data_weight,           DynamicRowWriteDataWeight) \
    XX(dynamic_row_delete,                      DynamicRowDelete) \
    XX(static_chunk_row_read,                   StaticChunkRowRead) \
    XX(static_chunk_row_read_data_weight,       StaticChunkRowReadDataWeight) \
    XX(static_chunk_row_lookup,                 StaticChunkRowLookup) \
    XX(static_chunk_row_lookup_true_negative,   StaticChunkRowLookupTrueNegative) \
    XX(static_chunk_row_lookup_false_positive,  StaticChunkRowLookupFalsePositive) \
    XX(static_chunk_row_lookup_data_weight,     StaticChunkRowLookupDataWeight) \
    XX(unmerged_row_read,                       UnmergedRowRead) \
    XX(merged_row_read,                         MergedRowRead) \
    XX(compaction_data_weight,                  CompactionDataWeight) \
    XX(partitioning_data_weight,                PartitioningDataWeight) \
    XX(lookup_error,                            LookupErrorCount) \
    XX(write_error,                             WriteErrorCount)

struct TTabletPerformanceCounters
{
    TInstant Timestamp;
    #define XX(name, Name) TTabletPerformanceCounter Name;
    ITERATE_TABLET_PERFORMANCE_COUNTERS(XX)
    #undef XX
};

void Serialize(const TTabletPerformanceCounters& counters, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

class TTableReplicaInfo
{
public:
    DEFINE_BYVAL_RW_PROPERTY(ETableReplicaState, State, ETableReplicaState::None);
    DEFINE_BYVAL_RW_PROPERTY(i64, CurrentReplicationRowIndex, 0);
    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::TTimestamp, CurrentReplicationTimestamp, NTransactionClient::NullTimestamp);
    DEFINE_BYVAL_RW_PROPERTY(bool, HasError);

public:
    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

};

////////////////////////////////////////////////////////////////////////////////

using TTabletErrors = TEnumIndexedVector<
    NTabletClient::ETabletBackgroundActivity,
    TError
>;

////////////////////////////////////////////////////////////////////////////////

class TTablet
    : public NObjectServer::TObject
    , public TRefTracked<TTablet>
{
public:
    DEFINE_BYVAL_RW_PROPERTY(int, Index);
    DEFINE_BYVAL_RW_PROPERTY(NHydra::TRevision, MountRevision);
    DEFINE_BYVAL_RW_PROPERTY(NTransactionServer::TTransaction*, StoresUpdatePreparedTransaction);
    DEFINE_BYVAL_RW_PROPERTY(TTabletCell*, Cell);
    DEFINE_BYVAL_RW_PROPERTY(TTabletAction*, Action);
    DEFINE_BYVAL_RW_PROPERTY(NTableClient::TLegacyOwningKey, PivotKey);
    DEFINE_BYREF_RW_PROPERTY(NTabletClient::NProto::TTabletStatistics, NodeStatistics);
    DEFINE_BYREF_RW_PROPERTY(TTabletPerformanceCounters, PerformanceCounters);
    //! Only makes sense for mounted tablets.
    DEFINE_BYVAL_RW_PROPERTY(NTabletClient::EInMemoryMode, InMemoryMode);
    //! Only used for ordered tablets.
    DEFINE_BYVAL_RW_PROPERTY(i64, TrimmedRowCount);
    //! Only makes sense for unmounted tablets.
    DEFINE_BYVAL_RW_PROPERTY(bool, WasForcefullyUnmounted);

    DEFINE_BYVAL_RW_PROPERTY(i64, ReplicationErrorCount);

    using TReplicaMap = THashMap<TTableReplica*, TTableReplicaInfo>;
    DEFINE_BYREF_RW_PROPERTY(TReplicaMap, Replicas);

    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::TTimestamp, RetainedTimestamp);

    using TUnconfirmedDynamicTableLocksSet = THashSet<NTransactionClient::TTransactionId>;
    DEFINE_BYREF_RW_PROPERTY(TUnconfirmedDynamicTableLocksSet, UnconfirmedDynamicTableLocks);

    DEFINE_BYREF_RW_PROPERTY(std::vector<TStoreId>, EdenStoreIds);
    DEFINE_BYREF_RW_PROPERTY(THashSet<NChunkServer::TDynamicStore*>, DynamicStores);

    DECLARE_BYVAL_RW_PROPERTY(ETabletState, State);
    DECLARE_BYVAL_RW_PROPERTY(ETabletState, ExpectedState);
    DECLARE_BYVAL_RW_PROPERTY(ETabletBackupState, BackupState);
    DECLARE_BYVAL_RW_PROPERTY(NTableServer::TTableNode*, Table);

    DEFINE_BYREF_RW_PROPERTY(NChaosClient::TReplicationProgress, ReplicationProgress);

public:
    using TObject::TObject;
    explicit TTablet(TTabletId id);

    TString GetLowercaseObjectName() const override;
    TString GetCapitalizedObjectName() const override;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    void CopyFrom(const TTablet& other);

    void ValidateMountRevision(NHydra::TRevision mountRevision);

    TTableReplicaInfo* FindReplicaInfo(const TTableReplica* replica);
    TTableReplicaInfo* GetReplicaInfo(const TTableReplica* replica);
    TDuration ComputeReplicationLagTime(
        NTransactionClient::TTimestamp latestTimestamp,
        const TTableReplicaInfo& replicaInfo) const;

    bool IsActive() const;

    NChunkServer::TChunkList* GetChunkList();
    const NChunkServer::TChunkList* GetChunkList() const;

    i64 GetTabletStaticMemorySize(NTabletClient::EInMemoryMode mode) const;
    i64 GetTabletStaticMemorySize() const;
    i64 GetTabletMasterMemoryUsage() const;

    i64 GetHunkUncompressedDataSize() const;
    i64 GetHunkCompressedDataSize() const;

    int GetTabletErrorCount() const;
    void SetTabletErrorCount(int tabletErrorCount);

    void CheckedSetBackupState(ETabletBackupState previous, ETabletBackupState next);

private:
    ETabletState State_ = ETabletState::Unmounted;
    ETabletState ExpectedState_ = ETabletState::Unmounted;
    ETabletBackupState BackupState_ = ETabletBackupState::None;
    NTableServer::TTableNode* Table_ = nullptr;
    int TabletErrorCount_ = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
