#pragma once

#include "public.h"
#include "yt/yt/library/profiling/sensor.h"

#include <yt/yt/server/lib/misc/profiling_helpers.h>

#include <yt/yt/server/lib/lsm/public.h>

#include <yt/yt/ytlib/table_client/hunks.h>
#include <yt/yt/ytlib/table_client/versioned_chunk_reader.h>

#include <yt/yt/ytlib/chunk_client/public.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>

#include <yt/yt/client/chunk_client/data_statistics.h>

#include <yt/yt/core/profiling/public.h>

#include <yt/yt/library/syncmap/map.h>

#include <library/cpp/ytalloc/core/misc/enum.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

struct TLookupCounters
{
    TLookupCounters() = default;

    TLookupCounters(
        const NProfiling::TProfiler& profiler,
        const NTableClient::TTableSchemaPtr& schema);

    NProfiling::TCounter CacheHits;
    NProfiling::TCounter CacheOutdated;
    NProfiling::TCounter CacheMisses;
    NProfiling::TCounter CacheInserts;

    NProfiling::TCounter RowCount;
    NProfiling::TCounter MissingKeyCount;
    NProfiling::TCounter DataWeight;
    NProfiling::TCounter UnmergedRowCount;
    NProfiling::TCounter UnmergedDataWeight;

    NProfiling::TTimeCounter CpuTime;
    NProfiling::TTimeCounter DecompressionCpuTime;
    NYT::NProfiling::TEventTimer LookupDuration;

    NChunkClient::TChunkReaderStatisticsCounters ChunkReaderStatisticsCounters;

    NTableClient::THunkChunkReaderCounters HunkChunkReaderCounters;
};

////////////////////////////////////////////////////////////////////////////////

struct TSelectCpuCounters
{
    TSelectCpuCounters() = default;

    TSelectCpuCounters(
        const NProfiling::TProfiler& profiler,
        const NTableClient::TTableSchemaPtr& schema);

    NProfiling::TTimeCounter CpuTime;
    NChunkClient::TChunkReaderStatisticsCounters ChunkReaderStatisticsCounters;
    NTableClient::THunkChunkReaderCounters HunkChunkReaderCounters;
};

struct TSelectReadCounters
{
    TSelectReadCounters() = default;

    explicit TSelectReadCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TCounter RowCount;
    NProfiling::TCounter DataWeight;
    NProfiling::TCounter UnmergedRowCount;
    NProfiling::TCounter UnmergedDataWeight;
    NProfiling::TTimeCounter DecompressionCpuTime;
    NYT::NProfiling::TEventTimer SelectDuration;
};

////////////////////////////////////////////////////////////////////////////////

struct TWriteCounters
{
    TWriteCounters() = default;

    explicit TWriteCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TCounter RowCount;
    NProfiling::TCounter DataWeight;
};

////////////////////////////////////////////////////////////////////////////////

struct TCommitCounters
{
    TCommitCounters() = default;

    explicit TCommitCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TCounter RowCount;
    NProfiling::TCounter DataWeight;
};

////////////////////////////////////////////////////////////////////////////////

struct TRemoteDynamicStoreReadCounters
{
    TRemoteDynamicStoreReadCounters() = default;

    explicit TRemoteDynamicStoreReadCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TCounter RowCount;
    NProfiling::TCounter DataWeight;
    NProfiling::TTimeCounter CpuTime;
    NProfiling::TSummary SessionRowCount;
    NProfiling::TSummary SessionDataWeight;
    NProfiling::TEventTimer SessionWallTime;
};

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EChunkReadProfilingMethod,
    (Preload)
    (Partitioning)
    (Compaction)
);

struct TChunkReadCounters
{
    TChunkReadCounters() = default;

    TChunkReadCounters(
        const NProfiling::TProfiler& profiler,
        const NTableClient::TTableSchemaPtr& schema);

    NProfiling::TCounter CompressedDataSize;
    NProfiling::TCounter UnmergedDataWeight;
    NProfiling::TTimeCounter DecompressionCpuTime;

    NChunkClient::TChunkReaderStatisticsCounters ChunkReaderStatisticsCounters;
    NTableClient::THunkChunkReaderCounters HunkChunkReaderCounters;
};

DEFINE_ENUM(EChunkWriteProfilingMethod,
    (StoreFlush)
    (Partitioning)
    (Compaction)
);

struct TChunkWriteCounters
{
    TChunkWriteCounters() = default;

    TChunkWriteCounters(
        const NProfiling::TProfiler& profiler,
        const NTableClient::TTableSchemaPtr& schema);

    NChunkClient::TChunkWriterCounters ChunkWriterCounters;
    NTableClient::THunkChunkWriterCounters HunkChunkWriterCounters;
};

////////////////////////////////////////////////////////////////////////////////

struct TTabletCounters
{
    TTabletCounters() = default;

    explicit TTabletCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TGauge OverlappingStoreCount;
    NProfiling::TGauge EdenStoreCount;
};

////////////////////////////////////////////////////////////////////////////////

struct TReplicaCounters
{
    TReplicaCounters() = default;

    explicit TReplicaCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TGauge LagRowCount;
    NProfiling::TTimeGauge LagTime;
    NProfiling::TEventTimer ReplicationThrottleTime;
    NProfiling::TEventTimer ReplicationTransactionStartTime;
    NProfiling::TEventTimer ReplicationTransactionCommitTime;
    NProfiling::TEventTimer ReplicationRowsReadTime;
    NProfiling::TEventTimer ReplicationRowsWriteTime;
    NProfiling::TSummary ReplicationBatchRowCount;
    NProfiling::TSummary ReplicationBatchDataWeight;

    NProfiling::TCounter ReplicationRowCount;
    NProfiling::TCounter ReplicationDataWeight;
    NProfiling::TCounter ReplicationErrorCount;
};

////////////////////////////////////////////////////////////////////////////////

struct TQueryServiceCounters
{
    TQueryServiceCounters() = default;

    explicit TQueryServiceCounters(const NProfiling::TProfiler& profiler);

    TMethodCounters Execute;
    TMethodCounters Multiread;
};

////////////////////////////////////////////////////////////////////////////////

struct TStoreRotationCounters
{
    TStoreRotationCounters() = default;

    explicit TStoreRotationCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TCounter RotationCount;
    NProfiling::TSummary RotatedRowCount;
    NProfiling::TSummary RotatedMemoryUsage;
};

struct TStoreCompactionCounters
{
    TStoreCompactionCounters() = default;

    explicit TStoreCompactionCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TCounter InDataWeight;
    NProfiling::TCounter OutDataWeight;
    NProfiling::TCounter InStoreCount;
    NProfiling::TCounter OutStoreCount;
};

struct TPartitionBalancingCounters
{
    TPartitionBalancingCounters() = default;

    explicit TPartitionBalancingCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TCounter PartitionSplits;
    NProfiling::TCounter PartitionMerges;
};

class TLsmCounters
{
public:
    TLsmCounters() = default;

    explicit TLsmCounters(const NProfiling::TProfiler& profiler);

    void ProfileRotation(NLsm::EStoreRotationReason reason, i64 rowCount, i64 memoryUsage);

    void ProfileCompaction(
        NLsm::EStoreCompactionReason reason,
        bool isEden,
        const NChunkClient::NProto::TDataStatistics readerStatistics,
        const NChunkClient::NProto::TDataStatistics writerStatistics);

    void ProfilePartitioning(
        NLsm::EStoreCompactionReason reason,
        const NChunkClient::NProto::TDataStatistics readerStatistics,
        const NChunkClient::NProto::TDataStatistics writerStatistics);

    void ProfilePartitionSplit();
    void ProfilePartitionMerge();

private:
    TEnumIndexedVector<
        NLsm::EStoreRotationReason,
        TStoreRotationCounters> RotationCounters_;

    // Counters[reason][eden][compaction/partitioning].
    TEnumIndexedVector<
        NLsm::EStoreCompactionReason,
        std::array<
            TEnumIndexedVector<
                NLsm::EStoreCompactorActivityKind,
                TStoreCompactionCounters>, 2>> CompactionCounters_;

    TPartitionBalancingCounters PartitionBalancingCounters_;

    void DoProfileCompaction(
        TStoreCompactionCounters* counters,
        const NChunkClient::NProto::TDataStatistics readerStatistics,
        const NChunkClient::NProto::TDataStatistics writerStatistics);
};

////////////////////////////////////////////////////////////////////////////////

TTableProfilerPtr CreateTableProfiler(
    EDynamicTableProfilingMode profilingMode,
    const TString& tabletCellBundle,
    const TString& tablePath,
    const TString& tableTag,
    const TString& account,
    const TString& medium,
    NObjectClient::TObjectId schemaId,
    const NTableClient::TTableSchemaPtr& schema);

////////////////////////////////////////////////////////////////////////////////

using TChunkWriteCountersVector = TEnumIndexedVector<
    EChunkWriteProfilingMethod,
    std::array<TChunkWriteCounters, 2>>;

using TChunkReadCountersVector = TEnumIndexedVector<
    EChunkReadProfilingMethod,
    std::array<TChunkReadCounters, 2>>;

using TTabletDistributedThrottlerTimersVector = TEnumIndexedVector<
    ETabletDistributedThrottlerKind,
    NProfiling::TEventTimer>;

using TTabletDistributedThrottlerCounters = TEnumIndexedVector<
    ETabletDistributedThrottlerKind,
    NProfiling::TCounter>;

class TTableProfiler
    : public TRefCounted
{
public:
    TTableProfiler() = default;

    TTableProfiler(
        const NProfiling::TProfiler& profiler,
        const NProfiling::TProfiler& diskProfiler,
        NTableClient::TTableSchemaPtr schema);

    static TTableProfilerPtr GetDisabled();

    TTabletCounters GetTabletCounters();

    TLookupCounters* GetLookupCounters(const std::optional<TString>& userTag);
    TWriteCounters* GetWriteCounters(const std::optional<TString>& userTag);
    TCommitCounters* GetCommitCounters(const std::optional<TString>& userTag);
    TSelectCpuCounters* GetSelectCpuCounters(const std::optional<TString>& userTag);
    TSelectReadCounters* GetSelectReadCounters(const std::optional<TString>& userTag);
    TRemoteDynamicStoreReadCounters* GetRemoteDynamicStoreReadCounters(const std::optional<TString>& userTag);
    TQueryServiceCounters* GetQueryServiceCounters(const std::optional<TString>& userTag);

    TReplicaCounters GetReplicaCounters(const TString& cluster);

    TChunkWriteCounters* GetWriteCounters(EChunkWriteProfilingMethod method, bool failed);
    TChunkReadCounters* GetReadCounters(EChunkReadProfilingMethod method, bool failed);
    NProfiling::TEventTimer* GetThrottlerTimer(ETabletDistributedThrottlerKind kind);
    NProfiling::TCounter* GetThrottlerCounter(ETabletDistributedThrottlerKind kind);
    TLsmCounters* GetLsmCounters();

private:
    const bool Disabled_ = true;
    const NProfiling::TProfiler Profiler_ = {};
    const NTableClient::TTableSchemaPtr Schema_;

    template <class TCounter>
    struct TUserTaggedCounter
    {
        NConcurrency::TSyncMap<std::optional<TString>, TCounter> Counters;

        TCounter* Get(
            bool disabled,
            const std::optional<TString>& userTag,
            const NProfiling::TProfiler& profiler);
        TCounter* Get(
            bool disabled,
            const std::optional<TString>& userTag,
            const NProfiling::TProfiler& profiler,
            const NTableClient::TTableSchemaPtr& schema);
    };

    TUserTaggedCounter<TLookupCounters> LookupCounters_;
    TUserTaggedCounter<TWriteCounters> WriteCounters_;
    TUserTaggedCounter<TCommitCounters> CommitCounters_;
    TUserTaggedCounter<TSelectCpuCounters> SelectCpuCounters_;
    TUserTaggedCounter<TSelectReadCounters> SelectReadCounters_;
    TUserTaggedCounter<TRemoteDynamicStoreReadCounters> DynamicStoreReadCounters_;
    TUserTaggedCounter<TQueryServiceCounters> QueryServiceCounters_;

    TChunkWriteCountersVector ChunkWriteCounters_;
    TChunkReadCountersVector ChunkReadCounters_;
    TTabletDistributedThrottlerTimersVector ThrottlerWaitTimers_;
    TTabletDistributedThrottlerCounters ThrottlerCounters_;
    TLsmCounters LsmCounters_;
};

DEFINE_REFCOUNTED_TYPE(TTableProfiler)

////////////////////////////////////////////////////////////////////////////////

class TWriterProfiler
    : public TRefCounted
{
public:
    TWriterProfiler() = default;

    void Profile(
        const TTabletSnapshotPtr& tabletSnapshot,
        EChunkWriteProfilingMethod method,
        bool failed);

    void Update(const NChunkClient::IMultiChunkWriterPtr& writer);
    void Update(const NChunkClient::IChunkWriterBasePtr& writer);
    void Update(
        const NTableClient::IHunkChunkPayloadWriterPtr& hunkChunkWriter,
        const NTableClient::IHunkChunkWriterStatisticsPtr& hunkChunkWriterStatistics);

private:
    NChunkClient::NProto::TDataStatistics DataStatistics_;
    NChunkClient::TCodecStatistics CodecStatistics_;

    NChunkClient::NProto::TDataStatistics HunkChunkDataStatistics_;
    NTableClient::IHunkChunkWriterStatisticsPtr HunkChunkWriterStatistics_;
};

DEFINE_REFCOUNTED_TYPE(TWriterProfiler)

/////////////////////////////////////////////////////////////////////////////

class TReaderProfiler
    : public TRefCounted
{
public:
    TReaderProfiler() = default;

    void Profile(
        const TTabletSnapshotPtr& tabletSnapshot,
        EChunkReadProfilingMethod method,
        bool failed);

    void Update(
        const NTableClient::IVersionedReaderPtr& reader,
        const NChunkClient::TChunkReaderStatisticsPtr& chunkReaderStatistics,
        const NTableClient::IHunkChunkReaderStatisticsPtr& hunkChunkReaderStatistics);

    void SetCompressedDataSize(i64 compressedDataSize);
    void SetCodecStatistics(const NChunkClient::TCodecStatistics& codecStatistics);
    void SetChunkReaderStatistics(const NChunkClient::TChunkReaderStatisticsPtr& chunkReaderStatistics);

private:
    NChunkClient::NProto::TDataStatistics DataStatistics_;
    NChunkClient::TCodecStatistics CodecStatistics_;

    NChunkClient::TChunkReaderStatisticsPtr ChunkReaderStatistics_;
    NTableClient::IHunkChunkReaderStatisticsPtr HunkChunkReaderStatistics_;
};

DEFINE_REFCOUNTED_TYPE(TReaderProfiler)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
