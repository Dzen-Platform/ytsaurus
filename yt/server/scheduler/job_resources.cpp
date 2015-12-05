#include "job_resources.h"
#include "config.h"

#include <yt/ytlib/chunk_client/private.h>

#include <yt/core/misc/common.h>

namespace NYT {
namespace NScheduler {

using namespace NChunkClient;
using namespace NNodeTrackerClient::NProto;

using NChunkClient::ChunkReaderMemorySize;

////////////////////////////////////////////////////////////////////

//! Additive term for each job memory usage.
//! Accounts for job proxy process and other lightweight stuff.
static const i64 FootprintMemorySize = (i64) 256 * 1024 * 1024;

//! Memory overhead caused by LFAlloc.
static const i64 LFAllocBufferSize = (i64) 64 * 1024 * 1024;

//! Nodes having less free memory are considered fully occupied,
//! thus no scheduling attempts will be made.
static const i64 LowWatermarkMemorySize = (i64) 256 * 1024 * 1024;

static const i64 ChunkSpecOverhead = (i64) 1000;

////////////////////////////////////////////////////////////////////

TNodeResources GetMinSpareResources()
{
    TNodeResources result;
    result.set_user_slots(1);
    result.set_cpu(1);
    result.set_memory(LowWatermarkMemorySize);
    return result;
}

const TNodeResources& MinSpareNodeResources()
{
    static auto result = GetMinSpareResources();
    return result;
}

i64 GetFootprintMemorySize()
{
    return FootprintMemorySize + GetLFAllocBufferSize();
}

i64 GetLFAllocBufferSize()
{
    return LFAllocBufferSize;
}

i64 GetOutputWindowMemorySize(TJobIOConfigPtr ioConfig)
{
    return
        ioConfig->TableWriter->SendWindowSize +
        ioConfig->TableWriter->EncodeWindowSize;
}

i64 GetIntermediateOutputIOMemorySize(TJobIOConfigPtr ioConfig)
{
    auto result = GetOutputWindowMemorySize(ioConfig) +
        ioConfig->TableWriter->MaxBufferSize;

    return result;
}

i64 GetInputIOMemorySize(
    TJobIOConfigPtr ioConfig,
    const TChunkStripeStatistics& stat)
{
    if (stat.ChunkCount == 0)
        return 0;

    int concurrentReaders = std::min(stat.ChunkCount, ioConfig->TableReader->MaxPrefetchWindow);

    // Group can be overcommited by one block.
    i64 groupSize = stat.MaxBlockSize + ioConfig->TableReader->GroupSize;
    i64 windowSize = std::max(stat.MaxBlockSize, ioConfig->TableReader->WindowSize);
    i64 bufferSize = std::min(stat.DataSize, concurrentReaders * (windowSize + groupSize));
    // One block for table chunk reader.
    bufferSize += concurrentReaders * (ChunkReaderMemorySize + stat.MaxBlockSize);

    i64 maxBufferSize = std::max(ioConfig->TableReader->MaxBufferSize, 2 * stat.MaxBlockSize);

    return std::min(bufferSize, maxBufferSize) + stat.ChunkCount * ChunkSpecOverhead;
}

i64 GetSortInputIOMemorySize(const TChunkStripeStatistics& stat)
{
    if (stat.ChunkCount == 0)
        return 0;

    return stat.DataSize + stat.ChunkCount * (ChunkReaderMemorySize + ChunkSpecOverhead);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

