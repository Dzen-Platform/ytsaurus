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

#include <yt/yt/core/concurrency/config.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TRemoteReaderOptions
    : public virtual NYTree::TYsonStruct
{
public:
    //! If |true| then the master may be asked for seeds.
    bool AllowFetchingSeedsFromMaster;

    //! Advertise current host as a P2P peer.
    bool EnableP2P;

    REGISTER_YSON_STRUCT(TRemoteReaderOptions);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TRemoteReaderOptions)

////////////////////////////////////////////////////////////////////////////////

class TRemoteWriterOptions
    : public virtual TMemoryTrackedWriterOptions
{
public:
    bool AllowAllocatingNewTargetNodes;
    TString MediumName;
    TPlacementId PlacementId;

    REGISTER_YSON_STRUCT(TRemoteWriterOptions);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TRemoteWriterOptions)

////////////////////////////////////////////////////////////////////////////////

class TDispatcherDynamicConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    std::optional<int> ChunkReaderPoolSize;

    TDispatcherDynamicConfig();
};

DEFINE_REFCOUNTED_TYPE(TDispatcherDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

class TDispatcherConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    static constexpr int DefaultChunkReaderPoolSize = 8;
    int ChunkReaderPoolSize;

    TDispatcherConfig();

    TDispatcherConfigPtr ApplyDynamic(const TDispatcherDynamicConfigPtr& dynamicConfig) const;
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
    bool EnableStripedErasure;

    //! Table and chunk schema might differ. By default they are assumed
    //! to be equal, this value overrides table schema, if set. Table schema
    //! cannot be stricter than chunk schema.
    NTableClient::TTableSchemaPtr TableSchema;

    NChunkClient::TConsistentReplicaPlacementHash ConsistentChunkReplicaPlacementHash;

    REGISTER_YSON_STRUCT(TMultiChunkWriterOptions);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TMultiChunkWriterOptions)

////////////////////////////////////////////////////////////////////////////////

class TMultiChunkReaderOptions
    : public TRemoteReaderOptions
{
public:
    bool KeepInMemory;

    REGISTER_YSON_STRUCT(TMultiChunkReaderOptions);

    static void Register(TRegistrar registrar);
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

    REGISTER_YSON_STRUCT(TMetaAggregatingWriterOptions);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TMetaAggregatingWriterOptions)

////////////////////////////////////////////////////////////////////////////////

class TBlockCacheConfig
    : public virtual NYTree::TYsonStruct
{
public:
    TSlruCacheConfigPtr CompressedData;
    TSlruCacheConfigPtr UncompressedData;

    REGISTER_YSON_STRUCT(TBlockCacheConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TBlockCacheConfig)

////////////////////////////////////////////////////////////////////////////////

class TClientChunkMetaCacheConfig
    : public TSlruCacheConfig
{
public:
    REGISTER_YSON_STRUCT(TClientChunkMetaCacheConfig);

    static void Register(TRegistrar)
    { }
};

DEFINE_REFCOUNTED_TYPE(TClientChunkMetaCacheConfig)

////////////////////////////////////////////////////////////////////////////////

class TBlockCacheDynamicConfig
    : public virtual NYTree::TYsonStruct
{
public:
    TSlruCacheDynamicConfigPtr CompressedData;
    TSlruCacheDynamicConfigPtr UncompressedData;

    REGISTER_YSON_STRUCT(TBlockCacheDynamicConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TBlockCacheDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

class TChunkScraperConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Number of chunks scratched per one LocateChunks.
    int MaxChunksPerRequest;

    TChunkScraperConfig();
};

DEFINE_REFCOUNTED_TYPE(TChunkScraperConfig)

////////////////////////////////////////////////////////////////////////////////

class TChunkTeleporterConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Maximum number of chunks to export/import per request.
    int MaxTeleportChunksPerRequest;

    TChunkTeleporterConfig();
};

DEFINE_REFCOUNTED_TYPE(TChunkTeleporterConfig)

////////////////////////////////////////////////////////////////////////////////

class TMediumDirectorySynchronizerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent directory updates.
    TDuration SyncPeriod;

    TMediumDirectorySynchronizerConfig();
};

DEFINE_REFCOUNTED_TYPE(TMediumDirectorySynchronizerConfig)

////////////////////////////////////////////////////////////////////////////////

class TChunkFragmentReaderConfig
    : public virtual NYTree::TYsonStruct
{
public:
    //! Expiration timeout of corresponding sync expiring cache.
    TDuration PeerInfoExpirationTimeout;

    //! Minimal delay between sequential chunk replica locations.
    TDuration SeedsExpirationTimeout;

    //! Delay between background cache updates.
    TDuration PeriodicUpdateDelay;

    //! Factors to calculate peer load as linear combination of disk queue and net queue.
    double NetQueueSizeFactor;
    double DiskQueueSizeFactor;

    //! RPC timeouts of ProbeChunkSet and GetChunkFragmentSet.
    TDuration ProbeChunkSetRpcTimeout;
    TDuration GetChunkFragmentSetRpcTimeout;

    //! Delay before sending a hedged request. If null then hedging is disabled.
    //! NB: This option may be overriden via hedging manager.
    std::optional<TDuration> FragmentReadHedgingDelay;

    //! Limit on retry count.
    int RetryCountLimit;
    //! Time between retries.
    TDuration RetryBackoffTime;
    //! Maximum time to serve fragments read request.
    TDuration ReadTimeLimit;

    //! Chunk that was not accessed for the time by user
    //! will stop being accessed within periodic updates and then will be evicted via expiring cache logic.
    TDuration ChunkInfoCacheExpirationTimeout;

    //! Will locate new replicas from master
    //! if node was suspicious for at least the period (unless null).
    std::optional<TDuration> SuspiciousNodeGracePeriod;

    //! Will open and read with DirectIO (unless already opened w/o DirectIO or disabled via location config).
    bool UseDirectIO;

    REGISTER_YSON_STRUCT(TChunkFragmentReaderConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TChunkFragmentReaderConfig)

////////////////////////////////////////////////////////////////////////////////

class TChunkReplicaCacheConfig
    : public virtual NYTree::TYsonStruct
{
public:
    TDuration ExpirationTime;
    TDuration ExpirationSweepPeriod;
    int MaxChunksPerLocate;

    REGISTER_YSON_STRUCT(TChunkReplicaCacheConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TChunkReplicaCacheConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
