#pragma once

#include "public.h"
#include "client_block_cache.h"

#include <yt/ytlib/api/native/public.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/concurrency/throughput_throttler.h>

#include <yt/core/rpc/public.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

IChunkWriterPtr CreateReplicationWriter(
    TReplicationWriterConfigPtr config,
    TRemoteWriterOptionsPtr options,
    TSessionId sessionId,
    const TChunkReplicaList& targets,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    NApi::NNative::IClientPtr client,
    IBlockCachePtr blockCache = GetNullBlockCache(),
    TTrafficMeterPtr trafficMeter = nullptr,
    NConcurrency::IThroughputThrottlerPtr throttler = NConcurrency::GetUnlimitedThrottler());

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
