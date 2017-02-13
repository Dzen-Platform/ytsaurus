#include "job_memory.h"
#include "config.h"

namespace NYT {
namespace NScheduler {

using namespace NChunkClient;

using NChunkClient::ChunkReaderMemorySize;

////////////////////////////////////////////////////////////////////

//! Additive term for each job memory usage.
//! Accounts for job proxy process and other lightweight stuff.
static const i64 FootprintMemorySize = (i64) 64 * 1024 * 1024;

//! Memory overhead caused by LFAlloc.
static const i64 LFAllocBufferSize = (i64) 64 * 1024 * 1024;

static const i64 ChunkSpecOverhead = (i64) 1000;

////////////////////////////////////////////////////////////////////

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
    static const double dataOverheadFactor = 0.05;

    if (stat.ChunkCount == 0)
        return 0;

    return static_cast<i64>(
        stat.DataSize * (1 + dataOverheadFactor) +
        stat.ChunkCount * (ChunkReaderMemorySize + ChunkSpecOverhead));
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

