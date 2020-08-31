#pragma once

#include "public.h"
#include "chunk_lookup_hash_table.h"

#include <yt/ytlib/chunk_client/chunk_spec.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

//! Extracted chunk state to avoid unnecessary reference counting.
struct TChunkState
    : public TRefCounted
{
    TChunkState() = default;
    TChunkState(
        NChunkClient::IBlockCachePtr preloadedBlockCache,
        const NChunkClient::NProto::TChunkSpec& chunkSpec,
        TCachedVersionedChunkMetaPtr chunkMeta,
        NTransactionClient::TTimestamp chunkTimestamp,
        IChunkLookupHashTablePtr lookupHashTable,
        TChunkReaderPerformanceCountersPtr performanceCounters,
        TKeyComparer keyComparer)
        : BlockCache(std::move(preloadedBlockCache))
        , ChunkSpec(chunkSpec)
        , ChunkMeta(std::move(chunkMeta))
        , ChunkTimestamp(chunkTimestamp)
        , LookupHashTable(std::move(lookupHashTable))
        , PerformanceCounters(std::move(performanceCounters))
        , KeyComparer(std::move(keyComparer))
    { }

    TChunkState(const TChunkState& other) = default;
    TChunkState(TChunkState&& other) = default;

    NChunkClient::IBlockCachePtr BlockCache;
    NChunkClient::NProto::TChunkSpec ChunkSpec;
    TCachedVersionedChunkMetaPtr ChunkMeta;
    NTransactionClient::TTimestamp ChunkTimestamp;
    IChunkLookupHashTablePtr LookupHashTable;
    TChunkReaderPerformanceCountersPtr PerformanceCounters;
    TKeyComparer KeyComparer;
};

DEFINE_REFCOUNTED_TYPE(TChunkState)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient

