#include "private.h"
#include "tablet.h"
#include "tablet_profiling.h"

#include <yt/server/lib/misc/profiling_helpers.h>

#include <yt/client/chunk_client/data_statistics.h>

#include <yt/client/table_client/versioned_reader.h>

#include <yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/ytlib/chunk_client/chunk_writer_base.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/table_client/versioned_chunk_writer.h>
#include <yt/ytlib/table_client/config.h>

#include <yt/core/profiling/profile_manager.h>
#include <yt/core/profiling/profiler.h>

#include <yt/core/misc/farm_hash.h>
#include <yt/core/misc/singleton.h>

#include <yt/yt/library/profiling/sensor.h>
#include <yt/yt/library/syncmap/map.h>

namespace NYT::NTabletNode {

using namespace NProfiling;
using namespace NChunkClient;
using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////////////////

void TWriterProfiler::Profile(
    const TTabletSnapshotPtr& tabletSnapshot,
    EChunkWriteProfilingMethod method,
    bool failed)
{
    auto diskSpace = CalculateDiskSpaceUsage(
        tabletSnapshot->WriterOptions->ReplicationFactor,
        DataStatistics_.regular_disk_space(),
        DataStatistics_.erasure_disk_space());
    auto compressionCpuTime = CodecStatistics_.GetTotalDuration();

    auto counters = tabletSnapshot->TableProfiler->GetWriteCounters(method, failed);

    counters->DiskSpace.Increment(diskSpace);
    counters->DataWeight.Increment(DataStatistics_.data_weight());
    counters->CompressionCpuTime.Add(compressionCpuTime);
}

void TWriterProfiler::Update(const NTableClient::IVersionedMultiChunkWriterPtr& writer)
{
    if (writer) {
        DataStatistics_ += writer->GetDataStatistics();
        CodecStatistics_ += writer->GetCompressionStatistics();
    }
}

void TWriterProfiler::Update(const IChunkWriterBasePtr& writer)
{
    if (writer) {
        DataStatistics_ += writer->GetDataStatistics();
        CodecStatistics_ += writer->GetCompressionStatistics();
    }
}

////////////////////////////////////////////////////////////////////////////////

void TReaderProfiler::Profile(
    const TTabletSnapshotPtr& tabletSnapshot,
    EChunkReadProfilingMethod method,
    bool failed)
{
    auto compressionCpuTime = CodecStatistics_.GetTotalDuration();

    auto counters = tabletSnapshot->TableProfiler->GetReadCounters(method, failed);

    counters->CompressedDataSize.Increment(DataStatistics_.compressed_data_size());
    counters->UnmergedDataWeight.Increment(DataStatistics_.data_weight());
    counters->DecompressionCpuTime.Add(compressionCpuTime);

    counters->ChunkReaderStatisticsCounters.Increment(ChunkReaderStatistics_);
}

void TReaderProfiler::Update(
    const NTableClient::IVersionedReaderPtr& reader,
    const NChunkClient::TChunkReaderStatisticsPtr& chunkReaderStatistics)
{
    if (reader) {
        DataStatistics_ += reader->GetDataStatistics();
        CodecStatistics_ += reader->GetDecompressionStatistics();
    }
    ChunkReaderStatistics_ = chunkReaderStatistics;
}

void TReaderProfiler::SetCompressedDataSize(i64 compressedDataSize)
{
    DataStatistics_.set_compressed_data_size(compressedDataSize);
}

void TReaderProfiler::SetCodecStatistics(const TCodecStatistics& codecStatistics)
{
    CodecStatistics_ = codecStatistics;
}

void TReaderProfiler::SetChunkReaderStatistics(const TChunkReaderStatisticsPtr& chunkReaderStatistics)
{
    ChunkReaderStatistics_ = chunkReaderStatistics;
}

////////////////////////////////////////////////////////////////////////////////

class TTabletProfilerManager
{
public:
    TTabletProfilerManager()
        : ConsumedTableTags_(TabletNodeProfiler.Gauge("/consumed_table_tags"))
    { }

    TTableProfilerPtr CreateTabletProfiler(
        EDynamicTableProfilingMode profilingMode,
        const TString& bundle,
        const TString& tablePath,
        const TString& tableTag,
        const TString& account,
        const TString& medium)
    {
        auto guard = Guard(Lock_);

        TProfilerKey key;
        switch (profilingMode) {
            case EDynamicTableProfilingMode::Disabled:
                key = {profilingMode, bundle, "", account, medium};
                break;
                
            case EDynamicTableProfilingMode::Path:
                key = {profilingMode, bundle, tablePath, account, medium};
                AllTables_.insert(tablePath);
                ConsumedTableTags_.Update(AllTables_.size());
                break;

            case EDynamicTableProfilingMode::Tag:
                key = {profilingMode, bundle, tableTag, account, medium};
                break;

            default:
                YT_VERIFY(false);
        }

        auto& profiler = Tables_[key];
        auto p = profiler.Lock();
        if (p) {
            return p;
        }

        auto tableProfiler = TabletNodeProfiler
            .WithHot()
            .WithSparse()
            .WithRequiredTag("tablet_cell_bundle", bundle);
        switch (profilingMode) {
            case EDynamicTableProfilingMode::Disabled:
                break;
                
            case EDynamicTableProfilingMode::Path:
                tableProfiler = tableProfiler.WithTag("table_path", tablePath, -1);
                break;

            case EDynamicTableProfilingMode::Tag:
                tableProfiler = tableProfiler.WithTag("table_tag", tableTag, -1);
                break;

            default:
                YT_VERIFY(false);
        }

        auto diskProfiler = tableProfiler
            .WithTag("account", account, -1)
            .WithRequiredTag("medium", medium, -1);

        p = New<TTableProfiler>(tableProfiler, diskProfiler);
        profiler = p;
        return p;
    }

private:
    TSpinLock Lock_;

    THashSet<TString> AllTables_;
    TGauge ConsumedTableTags_;

    using TProfilerKey = std::tuple<EDynamicTableProfilingMode, TString, TString, TString, TString>;

    THashMap<TProfilerKey, TWeakPtr<TTableProfiler>> Tables_;
};

////////////////////////////////////////////////////////////////////////////////

TTableProfilerPtr CreateTableProfiler(
    EDynamicTableProfilingMode profilingMode,
    const TString& tabletCellBundle,
    const TString& tablePath,
    const TString& tableTag,
    const TString& account,
    const TString& medium)
{
    return Singleton<TTabletProfilerManager>()->CreateTabletProfiler(
        profilingMode,
        tabletCellBundle,
        tablePath,
        tableTag,
        account,
        medium);
}

template <class TCounter>
TCounter* TTableProfiler::TUserTaggedCounter<TCounter>::Get(
    bool disabled,
    const std::optional<TString>& userTag,
    const NProfiling::TRegistry& profiler)
{
    if (disabled) {
        static TCounter counter;
        return &counter;
    }

    return Counters.FindOrInsert(userTag, [&] {
        if (userTag) {
            return TCounter{profiler.WithTag("user", *userTag)};
        } else {
            return TCounter{profiler};
        }
    }).first;
}


TTableProfiler::TTableProfiler(
    const NProfiling::TRegistry& profiler,
    const NProfiling::TRegistry& diskProfiler)
    : Disabled_(false)
    , Profiler_(profiler)
    , TabletCounters_(profiler)
{
    for (auto method : TEnumTraits<EChunkWriteProfilingMethod>::GetDomainValues()) {
        ChunkWriteCounters_[method] = {
            TChunkWriteCounters{diskProfiler.WithTag("method", FormatEnum(method))},
            TChunkWriteCounters{diskProfiler.WithTag("method", FormatEnum(method) + "_failed")}
        };
    }

    for (auto method : TEnumTraits<EChunkReadProfilingMethod>::GetDomainValues()) {
        ChunkReadCounters_[method] = {
            TChunkReadCounters{diskProfiler.WithTag("method", FormatEnum(method))},
            TChunkReadCounters{diskProfiler.WithTag("method", FormatEnum(method) + "_failed")}
        };
    }
}

TTableProfilerPtr TTableProfiler::GetDisabled()
{
    return RefCountedSingleton<TTableProfiler>();
}

TTabletCounters* TTableProfiler::GetTabletCounters()
{
    return &TabletCounters_;
}

TLookupCounters* TTableProfiler::GetLookupCounters(const std::optional<TString>& userTag)
{
    return LookupCounters_.Get(Disabled_, userTag, Profiler_);
}

TWriteCounters* TTableProfiler::GetWriteCounters(const std::optional<TString>& userTag)
{
    return WriteCounters_.Get(Disabled_, userTag, Profiler_);
}

TCommitCounters* TTableProfiler::GetCommitCounters(const std::optional<TString>& userTag)
{
    return CommitCounters_.Get(Disabled_, userTag, Profiler_);
}

TSelectCpuCounters* TTableProfiler::GetSelectCpuCounters(const std::optional<TString>& userTag)
{
    return SelectCpuCounters_.Get(Disabled_, userTag, Profiler_);
}

TSelectReadCounters* TTableProfiler::GetSelectReadCounters(const std::optional<TString>& userTag)
{
    return SelectReadCounters_.Get(Disabled_, userTag, Profiler_);
}

TRemoteDynamicStoreReadCounters* TTableProfiler::GetRemoteDynamicStoreReadCounters(const std::optional<TString>& userTag)
{
    return DynamicStoreReadCounters_.Get(Disabled_, userTag, Profiler_);
}

TQueryServiceCounters* TTableProfiler::GetQueryServiceCounters(const std::optional<TString>& userTag)
{
    return QueryServiceCounters_.Get(Disabled_, userTag, Profiler_);
}

TReplicaCounters TTableProfiler::GetReplicaCounters(
    bool enableProfiling,
    const TString& cluster,
    const NYPath::TYPath& path,
    const TTableReplicaId& replicaId)
{
    if (Disabled_) {
        return {};
    }

    auto key = std::make_tuple(enableProfiling, cluster, path, replicaId);
    return *ReplicaCounters_.FindOrInsert(key, [&] {
        if (!enableProfiling) {
            return TReplicaCounters{Profiler_.WithTag("replica_cluster", cluster, -1)};
        }

        return TReplicaCounters{Profiler_
            .WithTag("replica_cluster", cluster, -1)
            .WithTag("replica_path", path, -1)
            .WithRequiredTag("replica_id", ToString(replicaId), -1)};
    }).first;
}

TChunkWriteCounters* TTableProfiler::GetWriteCounters(EChunkWriteProfilingMethod method, bool failed)
{
    return &ChunkWriteCounters_[method][failed ? 1 : 0];
}

TChunkReadCounters* TTableProfiler::GetReadCounters(EChunkReadProfilingMethod method, bool failed)
{
    return &ChunkReadCounters_[method][failed ? 1 : 0];    
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
