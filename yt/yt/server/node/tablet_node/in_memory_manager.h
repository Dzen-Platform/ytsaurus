#pragma once

#include "public.h"
#include "tablet_profiling.h"

#include <yt/server/node/cluster_node/public.h>

#include <yt/ytlib/api/native/public.h>

#include <yt/ytlib/chunk_client/public.h>
#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/client/chunk_client/proto/chunk_spec.pb.h>

#include <yt/ytlib/misc/memory_usage_tracker.h>

#include <yt/ytlib/table_client/cached_versioned_chunk_meta.h>
#include <yt/ytlib/table_client/versioned_chunk_reader.h>

#include <yt/core/misc/ref.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

NChunkClient::EBlockType MapInMemoryModeToBlockType(NTabletClient::EInMemoryMode mode);

////////////////////////////////////////////////////////////////////////////////

struct TPreloadedBlockTag { };

////////////////////////////////////////////////////////////////////////////////

//! Contains all relevant data (e.g. blocks) for in-memory chunks.
struct TInMemoryChunkData
    : public TRefCounted
{
    NTabletClient::EInMemoryMode InMemoryMode = NTabletClient::EInMemoryMode::None;
    int StartBlockIndex = 0;
    std::vector<NChunkClient::TBlock> Blocks;
    NTableClient::TCachedVersionedChunkMetaPtr ChunkMeta;
    NTableClient::IChunkLookupHashTablePtr LookupHashTable;
    TMemoryUsageTrackerGuard MemoryTrackerGuard;
    std::atomic<bool> Finalized = false;
};

DEFINE_REFCOUNTED_TYPE(TInMemoryChunkData)

////////////////////////////////////////////////////////////////////////////////

//! Manages in-memory tables served by the node.
/*!
 *  Ensures that chunk stores of in-memory tables are preloaded when a node starts.
 *
 *  Provides means for intercepting data write-out during flushes and compactions
 *  and thus enables new chunk stores to be created with all blocks already resident.
 */
struct IInMemoryManager
    : public TRefCounted
{
    virtual NChunkClient::IBlockCachePtr CreateInterceptingBlockCache(
        NTabletClient::EInMemoryMode mode) = 0;

    virtual TInMemoryChunkDataPtr EvictInterceptedChunkData(
        NChunkClient::TChunkId chunkId) = 0;

    virtual void FinalizeChunk(
        NChunkClient::TChunkId chunkId,
        const NChunkClient::TRefCountedChunkMetaPtr& chunkMeta,
        const TTabletSnapshotPtr& tablet) = 0;

    virtual const TInMemoryManagerConfigPtr& GetConfig() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IInMemoryManager)

IInMemoryManagerPtr CreateInMemoryManager(
    TInMemoryManagerConfigPtr config,
    NClusterNode::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

//! Preload specified store into memory.
TInMemoryChunkDataPtr PreloadInMemoryStore(
    const TTabletSnapshotPtr& tabletSnapshot,
    const IChunkStorePtr& store,
    NChunkClient::TReadSessionId readSessionId,
    const NClusterNode::TNodeMemoryTrackerPtr& memoryTracker,
    const IInvokerPtr& compressionInvoker,
    const NConcurrency::IThroughputThrottlerPtr& bandwidthThrottler,
    const TReaderProfilerPtr& readerProfiler);

////////////////////////////////////////////////////////////////////////////////

struct TChunkInfo
{
    TChunkInfo(
        NChunkClient::TChunkId chunkId,
        NChunkClient::NProto::TChunkMeta chunkMeta,
        TTabletId tabletId,
        NHydra::TRevision mountRevision)
        : ChunkId(chunkId)
        , ChunkMeta(std::move(chunkMeta))
        , TabletId(tabletId)
        , MountRevision(mountRevision)
    { }

    NChunkClient::TChunkId ChunkId;
    NChunkClient::NProto::TChunkMeta ChunkMeta;
    TTabletId TabletId;
    NHydra::TRevision MountRevision;
};

struct IRemoteInMemoryBlockCache
    : public NChunkClient::IBlockCache
{
    virtual TFuture<void> Finish(const std::vector<TChunkInfo>& chunkInfos) = 0;
};

DEFINE_REFCOUNTED_TYPE(IRemoteInMemoryBlockCache)

TFuture<IRemoteInMemoryBlockCachePtr> CreateRemoteInMemoryBlockCache(
    NApi::NNative::IClientPtr client,
    const NNodeTrackerClient::TNodeDescriptor& localDescriptor,
    NRpc::IServerPtr localRpcServer,
    const NHiveClient::TCellDescriptor& cellDescriptor,
    NTabletClient::EInMemoryMode inMemoryMode,
    TInMemoryManagerConfigPtr config);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
