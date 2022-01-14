#include "chunk_reader_statistics.h"

#include <yt/yt/core/misc/statistics.h>

namespace NYT::NChunkClient {

using namespace NProfiling;
using namespace NYPath;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TChunkReaderStatistics* protoChunkReaderStatistics, const TChunkReaderStatisticsPtr& chunkReaderStatistics)
{
    protoChunkReaderStatistics->set_data_bytes_read_from_disk(chunkReaderStatistics->DataBytesReadFromDisk);
    protoChunkReaderStatistics->set_data_io_requests(chunkReaderStatistics->DataIORequests);
    protoChunkReaderStatistics->set_data_bytes_transmitted(chunkReaderStatistics->DataBytesTransmitted);
    protoChunkReaderStatistics->set_data_bytes_read_from_cache(chunkReaderStatistics->DataBytesReadFromCache);
    protoChunkReaderStatistics->set_meta_bytes_read_from_disk(chunkReaderStatistics->MetaBytesReadFromDisk);
    protoChunkReaderStatistics->set_omitted_suspicious_node_count(chunkReaderStatistics->OmittedSuspiciousNodeCount);
    protoChunkReaderStatistics->set_data_wait_time(chunkReaderStatistics->DataWaitTime);
    protoChunkReaderStatistics->set_meta_wait_time(chunkReaderStatistics->MetaWaitTime);
    protoChunkReaderStatistics->set_meta_read_from_disk_time(chunkReaderStatistics->MetaReadFromDiskTime);
    protoChunkReaderStatistics->set_pick_peer_wait_time(chunkReaderStatistics->PickPeerWaitTime);
}

void FromProto(TChunkReaderStatisticsPtr* chunkReaderStatisticsPtr, const NProto::TChunkReaderStatistics& protoChunkReaderStatistics)
{
    auto& chunkReaderStatistics = *chunkReaderStatisticsPtr;
    chunkReaderStatistics = New<TChunkReaderStatistics>();
    chunkReaderStatistics->DataBytesReadFromDisk = protoChunkReaderStatistics.data_bytes_read_from_disk();
    chunkReaderStatistics->DataIORequests = protoChunkReaderStatistics.data_io_requests();
    chunkReaderStatistics->DataBytesTransmitted = protoChunkReaderStatistics.data_bytes_transmitted();
    chunkReaderStatistics->DataBytesReadFromCache = protoChunkReaderStatistics.data_bytes_read_from_cache();
    chunkReaderStatistics->MetaBytesReadFromDisk = protoChunkReaderStatistics.meta_bytes_read_from_disk();
    chunkReaderStatistics->OmittedSuspiciousNodeCount = protoChunkReaderStatistics.omitted_suspicious_node_count();
    chunkReaderStatistics->DataWaitTime = protoChunkReaderStatistics.data_wait_time();
    chunkReaderStatistics->MetaWaitTime = protoChunkReaderStatistics.meta_wait_time();
    chunkReaderStatistics->MetaReadFromDiskTime = protoChunkReaderStatistics.meta_read_from_disk_time();
    chunkReaderStatistics->PickPeerWaitTime = protoChunkReaderStatistics.pick_peer_wait_time();
}

void UpdateFromProto(const TChunkReaderStatisticsPtr* chunkReaderStatisticsPtr, const NProto::TChunkReaderStatistics& protoChunkReaderStatistics)
{
    const auto& chunkReaderStatistics = *chunkReaderStatisticsPtr;
    chunkReaderStatistics->DataBytesReadFromDisk += protoChunkReaderStatistics.data_bytes_read_from_disk();
    chunkReaderStatistics->DataIORequests += protoChunkReaderStatistics.data_io_requests();
    chunkReaderStatistics->DataBytesTransmitted += protoChunkReaderStatistics.data_bytes_transmitted();
    chunkReaderStatistics->DataBytesReadFromCache += protoChunkReaderStatistics.data_bytes_read_from_cache();
    chunkReaderStatistics->MetaBytesReadFromDisk += protoChunkReaderStatistics.meta_bytes_read_from_disk();
    chunkReaderStatistics->OmittedSuspiciousNodeCount += protoChunkReaderStatistics.omitted_suspicious_node_count();
    chunkReaderStatistics->DataWaitTime += protoChunkReaderStatistics.data_wait_time();
    chunkReaderStatistics->MetaWaitTime += protoChunkReaderStatistics.meta_wait_time();
    chunkReaderStatistics->MetaReadFromDiskTime += protoChunkReaderStatistics.meta_read_from_disk_time();
    chunkReaderStatistics->PickPeerWaitTime += protoChunkReaderStatistics.pick_peer_wait_time();
}

void DumpChunkReaderStatistics(
    TStatistics* jobStatisitcs,
    const TString& path,
    const TChunkReaderStatisticsPtr& chunkReaderStatisticsPtr)
{
    jobStatisitcs->AddSample(path + "/data_bytes_read_from_disk", chunkReaderStatisticsPtr->DataBytesReadFromDisk);
    jobStatisitcs->AddSample(path + "/data_bytes_transmitted", chunkReaderStatisticsPtr->DataBytesTransmitted);
    jobStatisitcs->AddSample(path + "/data_bytes_read_from_cache", chunkReaderStatisticsPtr->DataBytesReadFromCache);
    jobStatisitcs->AddSample(path + "/meta_bytes_read_from_disk", chunkReaderStatisticsPtr->MetaBytesReadFromDisk);
}

void DumpTimingStatistics(
    TStatistics* jobStatistics,
    const TString& path,
    const TTimingStatistics& timingStatistics)
{
    jobStatistics->AddSample(path + "/wait_time", timingStatistics.WaitTime);
    jobStatistics->AddSample(path + "/read_time", timingStatistics.ReadTime);
    jobStatistics->AddSample(path + "/idle_time", timingStatistics.IdleTime);
}

////////////////////////////////////////////////////////////////////////////////

TChunkReaderStatisticsCounters::TChunkReaderStatisticsCounters(const NProfiling::TProfiler& profiler)
    : DataBytesReadFromDisk_(profiler.Counter("/data_bytes_read_from_disk"))
    , DataIORequests_(profiler.Counter("/data_io_requests"))
    , DataBytesTransmitted_(profiler.Counter("/data_bytes_transmitted"))
    , DataBytesReadFromCache_(profiler.Counter("/data_bytes_read_from_cache"))
    , WastedDataBytesReadFromDisk_(profiler.Counter("/wasted_data_bytes_read_from_disk"))
    , WastedDataBytesTransmitted_(profiler.Counter("/wasted_data_bytes_transmitted"))
    , WastedDataBytesReadFromCache_(profiler.Counter("/wasted_data_bytes_read_from_cache"))
    , MetaBytesReadFromDisk_(profiler.Counter("/meta_bytes_read_from_disk"))
    , OmittedSuspiciousNodeCount_(profiler.Counter("/omitted_suspicious_node_count"))
    , DataWaitTime_(profiler.TimeCounter("/data_wait_time"))
    , MetaWaitTime_(profiler.TimeCounter("/meta_wait_time"))
    , MetaReadFromDiskTime_(profiler.TimeCounter("/meta_read_from_disk_time"))
    , PickPeerWaitTime_(profiler.TimeCounter("/pick_peer_wait_time"))
{ }

void TChunkReaderStatisticsCounters::Increment(
    const TChunkReaderStatisticsPtr& chunkReaderStatistics,
    bool failed)
{
    DataBytesReadFromDisk_.Increment(chunkReaderStatistics->DataBytesReadFromDisk);
    DataIORequests_.Increment(chunkReaderStatistics->DataIORequests);
    DataBytesTransmitted_.Increment(chunkReaderStatistics->DataBytesTransmitted);
    DataBytesReadFromCache_.Increment(chunkReaderStatistics->DataBytesReadFromCache);
    if (failed) {
        WastedDataBytesReadFromDisk_.Increment(chunkReaderStatistics->DataBytesReadFromDisk);
        WastedDataBytesTransmitted_.Increment(chunkReaderStatistics->DataBytesTransmitted);
        WastedDataBytesReadFromCache_.Increment(chunkReaderStatistics->DataBytesReadFromCache);
    }

    MetaBytesReadFromDisk_.Increment(chunkReaderStatistics->MetaBytesReadFromDisk);
    OmittedSuspiciousNodeCount_.Increment(chunkReaderStatistics->OmittedSuspiciousNodeCount);

    DataWaitTime_.Add(TDuration::FromValue(chunkReaderStatistics->DataWaitTime));
    MetaWaitTime_.Add(TDuration::FromValue(chunkReaderStatistics->MetaWaitTime));
    MetaReadFromDiskTime_.Add(TDuration::FromValue(chunkReaderStatistics->MetaReadFromDiskTime));
    PickPeerWaitTime_.Add(TDuration::FromValue(chunkReaderStatistics->PickPeerWaitTime));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
