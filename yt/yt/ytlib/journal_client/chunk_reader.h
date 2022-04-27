#pragma once

#include "public.h"

#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>

#include <yt/yt/library/erasure/public.h>

#include <yt/yt/core/concurrency/throughput_throttler.h>

namespace NYT::NJournalClient {

////////////////////////////////////////////////////////////////////////////////

NChunkClient::IChunkReaderPtr CreateChunkReader(
    TChunkReaderConfigPtr config,
    NApi::NNative::IClientPtr client,
    NChunkClient::TChunkId chunkId,
    NErasure::ECodec codecId,
    const NChunkClient::TChunkReplicaList& replicas,
    NChunkClient::IBlockCachePtr blockCache,
    NChunkClient::IClientChunkMetaCachePtr chunkMetaCache,
    NChunkClient::TTrafficMeterPtr trafficMeter = nullptr,
    NConcurrency::IThroughputThrottlerPtr bandwidthThrottler = NConcurrency::GetUnlimitedThrottler(),
    NConcurrency::IThroughputThrottlerPtr rpsThrottler = NConcurrency::GetUnlimitedThrottler());

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJournalClient
