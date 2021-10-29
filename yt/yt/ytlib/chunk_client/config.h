#pragma once

#include "public.h"

#include <yt/yt/client/misc/config.h>
#include <yt/yt/client/misc/workload.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/client/api/public.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>
#include <yt/yt/client/chunk_client/config.h>

#include <yt/yt/client/table_client/schema.h>

#include <yt/yt/core/compression/public.h>

#include <yt/yt/library/erasure/public.h>

#include <yt/yt/core/misc/cache_config.h>
#include <yt/yt/core/misc/error.h>

#include <yt/yt/core/rpc/config.h>

#include <yt/yt/core/ytree/yson_serializable.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TRemoteReaderOptions
    : public virtual NYTree::TYsonSerializable
{
public:
    //! If |true| then the master may be asked for seeds.
    bool AllowFetchingSeedsFromMaster;

    //! Advertise current host as a P2P peer.
    bool EnableP2P;

    TRemoteReaderOptions()
    {
        RegisterParameter("allow_fetching_seeds_from_master", AllowFetchingSeedsFromMaster)
            .Default(true);

        RegisterParameter("enable_p2p", EnableP2P)
            .Default(false);
    }
};

DEFINE_REFCOUNTED_TYPE(TRemoteReaderOptions)

////////////////////////////////////////////////////////////////////////////////

class TRemoteWriterOptions
    : public virtual NYTree::TYsonSerializable
{
public:
    bool AllowAllocatingNewTargetNodes;
    TString MediumName;
    TPlacementId PlacementId;

    TRemoteWriterOptions()
    {
        RegisterParameter("allow_allocating_new_target_nodes", AllowAllocatingNewTargetNodes)
            .Default(true);
        RegisterParameter("medium_name", MediumName)
            .Default(DefaultStoreMediumName);
        RegisterParameter("placement_id", PlacementId)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TRemoteWriterOptions)

////////////////////////////////////////////////////////////////////////////////

class TDispatcherDynamicConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    std::optional<int> ChunkReaderPoolSize;

    TDispatcherDynamicConfig()
    {
        RegisterParameter("chunk_reader_pool_size", ChunkReaderPoolSize)
            .Optional();
    }
};

DEFINE_REFCOUNTED_TYPE(TDispatcherDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

class TDispatcherConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    static constexpr int DefaultChunkReaderPoolSize = 8;
    int ChunkReaderPoolSize;

    TDispatcherConfig()
    {
        RegisterParameter("chunk_reader_pool_size", ChunkReaderPoolSize)
            .Default(DefaultChunkReaderPoolSize);
    }

    TDispatcherConfigPtr ApplyDynamic(const TDispatcherDynamicConfigPtr& dynamicConfig) const
    {
        auto mergedConfig = New<TDispatcherConfig>();
        mergedConfig->ChunkReaderPoolSize = dynamicConfig->ChunkReaderPoolSize.value_or(ChunkReaderPoolSize);
        mergedConfig->Postprocess();
        return mergedConfig;
    }
};

DEFINE_REFCOUNTED_TYPE(TDispatcherConfig)

////////////////////////////////////////////////////////////////////////////////

class TMultiChunkWriterOptions
    : public virtual TEncodingWriterOptions
    , public virtual TRemoteWriterOptions
{
public:
    static constexpr int InvalidTableIndex = -1;

public:
    int ReplicationFactor;
    TString Account;
    bool ChunksVital;
    bool ChunksMovable;
    bool ValidateResourceUsageIncrease;

    //! This field doesn't affect the behavior of writer.
    //! It is stored in table_index field of output_chunk_specs.
    int TableIndex;

    NErasure::ECodec ErasureCodec;

    //! Table and chunk schema might differ. By default they are assumed
    //! to be equal, this value overrides table schema, if set. Table schema
    //! cannot be stricter than chunk schema.
    NTableClient::TTableSchemaPtr TableSchema;

    NChunkClient::TConsistentReplicaPlacementHash ConsistentChunkReplicaPlacementHash;

    TMultiChunkWriterOptions()
    {
        RegisterParameter("replication_factor", ReplicationFactor)
            .GreaterThanOrEqual(1)
            .Default(DefaultReplicationFactor);
        RegisterParameter("account", Account);
        RegisterParameter("chunks_vital", ChunksVital)
            .Default(true);
        RegisterParameter("chunks_movable", ChunksMovable)
            .Default(true);
        RegisterParameter("validate_resource_usage_increase", ValidateResourceUsageIncrease)
            .Default(true);
        RegisterParameter("erasure_codec", ErasureCodec)
            .Default(NErasure::ECodec::None);
        RegisterParameter("table_index", TableIndex)
            .Default(InvalidTableIndex);
        RegisterParameter("table_schema", TableSchema)
            .Default();
        RegisterParameter("chunk_consistent_replica_placement_hash", ConsistentChunkReplicaPlacementHash)
            .Default(NChunkClient::NullConsistentReplicaPlacementHash);
    }
};

DEFINE_REFCOUNTED_TYPE(TMultiChunkWriterOptions)

////////////////////////////////////////////////////////////////////////////////

class TMultiChunkReaderOptions
    : public TRemoteReaderOptions
{
public:
    bool KeepInMemory;

    TMultiChunkReaderOptions()
    {
        RegisterParameter("keep_in_memory", KeepInMemory)
            .Default(false);
    }
};

DEFINE_REFCOUNTED_TYPE(TMultiChunkReaderOptions)

////////////////////////////////////////////////////////////////////////////////

class TMetaAggregatingWriterOptions
    : public TMultiChunkWriterOptions
{
public:
    bool EnableSkynetSharing;
    int MaxHeavyColumns;
    bool AllowUnknownExtensions;
    std::optional<i64> MaxBlockCount;

    TMetaAggregatingWriterOptions()
    {
        RegisterParameter("enable_skynet_sharing", EnableSkynetSharing)
            .Default(false);
        RegisterParameter("max_heavy_columns", MaxHeavyColumns)
            .Default(0);
        RegisterParameter("allow_unknown_extensions", AllowUnknownExtensions)
            .Default(false);
        RegisterParameter("max_block_count", MaxBlockCount)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TMetaAggregatingWriterOptions)

////////////////////////////////////////////////////////////////////////////////

class TBlockCacheConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    TSlruCacheConfigPtr CompressedData;
    TSlruCacheConfigPtr UncompressedData;

    TBlockCacheConfig()
    {
        RegisterParameter("compressed_data", CompressedData)
            .DefaultNew();
        RegisterParameter("uncompressed_data", UncompressedData)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TBlockCacheConfig)

////////////////////////////////////////////////////////////////////////////////

class TClientChunkMetaCacheConfig
    : public TSlruCacheConfig
{
public:
    TClientChunkMetaCacheConfig(i64 capacity = 0)
        : TSlruCacheConfig(capacity)
    { }
};

DEFINE_REFCOUNTED_TYPE(TClientChunkMetaCacheConfig)

////////////////////////////////////////////////////////////////////////////////

class TBlockCacheDynamicConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    TSlruCacheDynamicConfigPtr CompressedData;
    TSlruCacheDynamicConfigPtr UncompressedData;

    TBlockCacheDynamicConfig()
    {
        RegisterParameter("compressed_data", CompressedData)
            .DefaultNew();
        RegisterParameter("uncompressed_data", UncompressedData)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TBlockCacheDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

class TChunkScraperConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Number of chunks scratched per one LocateChunks.
    int MaxChunksPerRequest;

    TChunkScraperConfig()
    {
        RegisterParameter("max_chunks_per_request", MaxChunksPerRequest)
            .Default(10000)
            .GreaterThan(0)
            .LessThan(100000);
    }
};

DEFINE_REFCOUNTED_TYPE(TChunkScraperConfig)

////////////////////////////////////////////////////////////////////////////////

class TChunkTeleporterConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Maximum number of chunks to export/import per request.
    int MaxTeleportChunksPerRequest;

    TChunkTeleporterConfig()
    {
        RegisterParameter("max_teleport_chunks_per_request", MaxTeleportChunksPerRequest)
            .GreaterThan(0)
            .Default(5000);
    }
};

DEFINE_REFCOUNTED_TYPE(TChunkTeleporterConfig)

////////////////////////////////////////////////////////////////////////////////

class TMediumDirectorySynchronizerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent directory updates.
    TDuration SyncPeriod;

    TMediumDirectorySynchronizerConfig()
    {
        RegisterParameter("sync_period", SyncPeriod)
            .Default(TDuration::Seconds(60));
    }
};

DEFINE_REFCOUNTED_TYPE(TMediumDirectorySynchronizerConfig)

////////////////////////////////////////////////////////////////////////////////

class TChunkFragmentReaderConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Expiration timeouts of corresponding sync expiring caches.
    TDuration ChunkReplicaLocatorExpirationTimeout;
    TDuration PeerInfoExpirationTimeout;

    //! Minimal delay between sequential chunk replica locations.
    TDuration SeedsExpirationTimeout;

    //! Delay between background cache updates.
    TDuration PeriodicUpdateDelay;

    //! Factors to calculate peer load as linear combination of disk queue and net queue.
    double NetQueueSizeFactor;
    double DiskQueueSizeFactor;

    //! Rpc timeouts of ProbeChunkSet and GetChunkFragmentSet.
    TDuration ProbeChunkSetRpcTimeout;
    TDuration GetChunkFragmentSetRpcTimeout;

    //! Channel multiplexing parallelism for GetChunkFragmentSet.
    int GetChunkFragmentSetMultiplexingParallelism;

    //! Limit on retry count.
    int MaxRetryCount;
    //! Time between retries.
    TDuration RetryBackoffTime;

    //! Chunk that was not accessed for the time by user
    //! will stop being accessed within periodic updates and then will be evicted via expiring cache logic.
    TDuration EvictAfterSuccessfulAccessTime;

    //! Will locate new replicas from master
    //! if node was suspicious for at least the period (unless null).
    std::optional<TDuration> SuspiciousNodeGracePeriod;

    TChunkFragmentReaderConfig()
    {
        RegisterParameter("chunk_replica_locator_expiration_timeout", ChunkReplicaLocatorExpirationTimeout)
            .Default(TDuration::Minutes(30));
        RegisterParameter("peer_info_expiration_timeout", PeerInfoExpirationTimeout)
            .Default(TDuration::Minutes(30));

        RegisterParameter("seeds_expiration_timeout", SeedsExpirationTimeout)
            .Default(TDuration::Seconds(3));

        RegisterParameter("periodic_update_delay", PeriodicUpdateDelay)
            .GreaterThan(TDuration::Zero())
            .Default(TDuration::Seconds(10));

        RegisterParameter("net_queue_size_factor", NetQueueSizeFactor)
            .Default(0.5);
        RegisterParameter("disk_queue_size_factor", DiskQueueSizeFactor)
            .Default(1.0);

        RegisterParameter("probe_chunk_set_rpc_timeout", ProbeChunkSetRpcTimeout)
            .Default(TDuration::Seconds(5));
        RegisterParameter("get_chunk_fragment_set_rpc_timeout", GetChunkFragmentSetRpcTimeout)
            .Default(TDuration::Seconds(15));

        RegisterParameter("get_chunk_fragment_multiplexing_parallelism", GetChunkFragmentSetMultiplexingParallelism)
            .GreaterThan(0)
            .Default(1);

        RegisterParameter("max_retry_count", MaxRetryCount)
            .GreaterThanOrEqual(1)
            .Default(3);
        RegisterParameter("retry_backoff_time", RetryBackoffTime)
            .Default(TDuration::MilliSeconds(10));

        RegisterParameter("evict_after_successful_access_time", EvictAfterSuccessfulAccessTime)
            .Default(TDuration::Seconds(30));

        RegisterParameter("suspicious_node_grace_period", SuspiciousNodeGracePeriod)
            .Default(TDuration::Minutes(5));
    }
};

DEFINE_REFCOUNTED_TYPE(TChunkFragmentReaderConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
