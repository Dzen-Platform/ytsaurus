#pragma once

#include "public.h"

#include <yt/server/controller_agent/chunk_pools/chunk_pool.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

i64 GetFootprintMemorySize();
i64 GetYTAllocLargeUnreclaimableBytes();

i64 GetInputIOMemorySize(
    NScheduler::TJobIOConfigPtr ioConfig,
    const NChunkPools::TChunkStripeStatistics& stat);

i64 GetSortInputIOMemorySize(const NChunkPools::TChunkStripeStatistics& stat);

i64 GetIntermediateOutputIOMemorySize(NScheduler::TJobIOConfigPtr ioConfig);

i64 GetOutputWindowMemorySize(NScheduler::TJobIOConfigPtr ioConfig);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent

