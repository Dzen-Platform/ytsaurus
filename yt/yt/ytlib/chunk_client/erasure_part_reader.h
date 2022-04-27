#pragma once

#include "public.h"
#include "client_block_cache.h"

#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/core/concurrency/throughput_throttler.h>

#include <yt/yt/library/erasure/impl/public.h>

#include <yt/yt/core/misc/error.h>

#include <yt/yt/core/rpc/public.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

std::vector<IChunkReaderAllowingRepairPtr> CreateErasurePartReaders(
    TReplicationReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    NApi::NNative::IClientPtr client,
    TChunkId chunkId,
    const TChunkReplicaList& seedReplicas,
    const NErasure::ICodec* codec,
    const NErasure::TPartIndexList& partIndexList,
    IBlockCachePtr blockCache,
    IClientChunkMetaCachePtr chunkMetaCache,
    TTrafficMeterPtr trafficMeter = nullptr,
    NConcurrency::IThroughputThrottlerPtr bandwidthThrottler = NConcurrency::GetUnlimitedThrottler(),
    NConcurrency::IThroughputThrottlerPtr rpsThrottler = NConcurrency::GetUnlimitedThrottler());

std::vector<IChunkReaderAllowingRepairPtr> CreateAllErasurePartReaders(
    TReplicationReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    NApi::NNative::IClientPtr client,
    TChunkId chunkId,
    const TChunkReplicaList& seedReplicas,
    const NErasure::ICodec* codec,
    IBlockCachePtr blockCache,
    IClientChunkMetaCachePtr chunkMetaCache,
    TTrafficMeterPtr trafficMeter = nullptr,
    NConcurrency::IThroughputThrottlerPtr bandwidthThrottler = NConcurrency::GetUnlimitedThrottler(),
    NConcurrency::IThroughputThrottlerPtr rpsThrottler = NConcurrency::GetUnlimitedThrottler());

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient

