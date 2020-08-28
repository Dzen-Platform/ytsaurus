#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/proto/chunk_reader_statistics.pb.h>

#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/core/profiling/profiler.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

struct TChunkReaderStatistics
    : public TRefCounted
{
    std::atomic<i64> DataBytesReadFromDisk{0};
    std::atomic<i64> DataBytesTransmitted{0};
    std::atomic<i64> DataBytesReadFromCache{0};
    std::atomic<i64> MetaBytesReadFromDisk{0};
    std::atomic<NProfiling::TValue> DataWaitTime{0};
    std::atomic<NProfiling::TValue> MetaWaitTime{0};
    std::atomic<NProfiling::TValue> PickPeerWaitTime{0};
};

DEFINE_REFCOUNTED_TYPE(TChunkReaderStatistics)

void ToProto(
    NProto::TChunkReaderStatistics* protoChunkReaderStatistics,
    const TChunkReaderStatisticsPtr& chunkReaderStatistics);
void FromProto(
    TChunkReaderStatisticsPtr chunkReaderStatistics,
    NProto::TChunkReaderStatistics* protoChunkReaderStatistics);

void UpdateFromProto(
    const TChunkReaderStatisticsPtr* chunkReaderStatisticsPtr,
    const NProto::TChunkReaderStatistics& protoChunkReaderStatistics);

void DumpChunkReaderStatistics(
    TStatistics* jobStatisitcs,
    const TString& path,
    const TChunkReaderStatisticsPtr& chunkReaderStatisticsPtr);

////////////////////////////////////////////////////////////////////////////////

class TChunkReaderStatisticsCounters
{
public:
    explicit TChunkReaderStatisticsCounters(
        const NYPath::TYPath& path = {},
        const NProfiling::TTagIdList& tagIds = {});

    void Increment(
        const NProfiling::TProfiler& profiler,
        const TChunkReaderStatisticsPtr& chunkReaderStatistics);

private:
    NProfiling::TShardedMonotonicCounter DataBytesReadFromDisk;
    NProfiling::TShardedMonotonicCounter DataBytesTransmitted;
    NProfiling::TShardedMonotonicCounter DataBytesReadFromCache;
    NProfiling::TShardedMonotonicCounter MetaBytesReadFromDisk;
    NProfiling::TShardedMonotonicCounter DataWaitTime;
    NProfiling::TShardedMonotonicCounter MetaWaitTime;
    NProfiling::TShardedMonotonicCounter PickPeerWaitTime;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient

