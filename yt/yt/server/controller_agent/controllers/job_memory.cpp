#include "job_memory.h"

#include <yt/yt/ytlib/scheduler/config.h>

namespace NYT::NControllerAgent::NControllers {

using namespace NChunkClient;
using namespace NChunkPools;
using namespace NScheduler;

using NChunkClient::ChunkReaderMemorySize;

////////////////////////////////////////////////////////////////////////////////

//! Additive term for each job memory usage.
//! Accounts for job proxy process and other lightweight stuff.
static const i64 FootprintMemorySize = 64_MB;

//! Min memory overhead caused by YTAlloc.
static const i64 YTAllocMinLargeUnreclaimableBytes = 32_MB;

//! Max memory overhead caused by YTAlloc.
static const i64 YTAllocMaxLargeUnreclaimableBytes = 64_MB;

static const i64 ChunkSpecOverhead = 1000;

////////////////////////////////////////////////////////////////////////////////

i64 GetFootprintMemorySize()
{
    return FootprintMemorySize + YTAllocMaxLargeUnreclaimableBytes;
}

i64 GetYTAllocMinLargeUnreclaimableBytes()
{
    return YTAllocMinLargeUnreclaimableBytes;
}

i64 GetYTAllocMaxLargeUnreclaimableBytes()
{
    return YTAllocMaxLargeUnreclaimableBytes;
}

i64 GetOutputWindowMemorySize(const TJobIOConfigPtr& ioConfig)
{
    return
        ioConfig->TableWriter->SendWindowSize +
        ioConfig->TableWriter->EncodeWindowSize;
}

i64 GetIntermediateOutputIOMemorySize(const TJobIOConfigPtr& ioConfig)
{
    auto result = GetOutputWindowMemorySize(ioConfig) +
        ioConfig->TableWriter->MaxBufferSize;

    return result;
}

i64 GetInputIOMemorySize(
    const TJobIOConfigPtr& ioConfig,
    const TChunkStripeStatistics& stat)
{
    if (stat.ChunkCount == 0)
        return 0;

    int concurrentReaders = std::min(stat.ChunkCount, ioConfig->TableReader->MaxParallelReaders);

    // Group can be overcommitted by one block.
    i64 groupSize = stat.MaxBlockSize + ioConfig->TableReader->GroupSize;
    i64 windowSize = std::max(stat.MaxBlockSize, ioConfig->TableReader->WindowSize);

    // Data weight here is upper bound on the cumulative size of uncompressed blocks.
    i64 bufferSize = std::min(stat.DataWeight, concurrentReaders * (windowSize + groupSize));
    // One block for table chunk reader.
    bufferSize += concurrentReaders * (ChunkReaderMemorySize + stat.MaxBlockSize);

    i64 maxBufferSize = std::max(ioConfig->TableReader->MaxBufferSize, 2 * stat.MaxBlockSize);

    i64 blockCacheSize = ioConfig->BlockCache->CompressedData->Capacity + ioConfig->BlockCache->UncompressedData->Capacity;

    return std::min(bufferSize, maxBufferSize) + stat.ChunkCount * ChunkSpecOverhead + blockCacheSize;
}

i64 GetSortInputIOMemorySize(const TChunkStripeStatistics& stat)
{
    static const double dataOverheadFactor = 0.05;

    if (stat.ChunkCount == 0)
        return 0;

    return static_cast<i64>(
        stat.DataWeight * (1 + dataOverheadFactor) +
        stat.ChunkCount * (ChunkReaderMemorySize + ChunkSpecOverhead));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent::NControllers
