#pragma once

#include "public.h"

#include <yt/server/hydra/config.h>

#include <yt/server/misc/config.h>

#include <yt/ytlib/chunk_client/config.h>

#include <yt/core/concurrency/config.h>

#include <yt/core/misc/config.h>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

class TPeerBlockTableConfig
    : public NYTree::TYsonSerializable
{
public:
    int MaxPeersPerBlock;
    TDuration SweepPeriod;

    TPeerBlockTableConfig()
    {
        RegisterParameter("max_peers_per_block", MaxPeersPerBlock)
            .GreaterThan(0)
            .Default(64);
        RegisterParameter("sweep_period", SweepPeriod)
            .Default(TDuration::Minutes(10));
    }
};

DEFINE_REFCOUNTED_TYPE(TPeerBlockTableConfig)

////////////////////////////////////////////////////////////////////////////////

class TStoreLocationConfigBase
    : public TDiskLocationConfig
{
public:
    //! Maximum space chunks are allowed to occupy.
    //! (If not initialized then indicates to occupy all available space on drive).
    TNullable<i64> Quota;

    // NB: actually registered as parameter by subclasses (because default value
    // is subclass-specific).
    TString MediumName;

    TStoreLocationConfigBase()
    {
        RegisterParameter("quota", Quota)
            .GreaterThanOrEqual(0)
            .Default(TNullable<i64>());
    }
};

DEFINE_REFCOUNTED_TYPE(TStoreLocationConfigBase)

////////////////////////////////////////////////////////////////////////////////

class TStoreLocationConfig
    : public TStoreLocationConfigBase
{
public:
    //! A currently full location is considered to be non-full again when available space grows
    //! above this limit.
    i64 LowWatermark;

    //! A location is considered to be full when available space becomes less than #HighWatermark.
    i64 HighWatermark;

    //! All writes to the location are aborted when available space becomes less than #DisableWritesWatermark.
    i64 DisableWritesWatermark;

    //! Maximum amount of time files of a deleted chunk could rest in trash directory before
    //! being permanently removed.
    TDuration MaxTrashTtl;

    //! When free space drops below this watermark, the system starts deleting files in trash directory,
    //! starting from the eldest ones.
    i64 TrashCleanupWatermark;

    //! Controls incoming location bandwidth used by repair jobs.
    NConcurrency::TThroughputThrottlerConfigPtr RepairInThrottler;

    //! Controls incoming location bandwidth used by replication jobs.
    NConcurrency::TThroughputThrottlerConfigPtr ReplicationInThrottler;

    TStoreLocationConfig()
    {
        RegisterParameter("low_watermark", LowWatermark)
            .GreaterThanOrEqual(0)
            .Default(50_GB);
        RegisterParameter("high_watermark", HighWatermark)
            .GreaterThanOrEqual(0)
            .Default(20_GB);
        RegisterParameter("disable_writes_watermark", DisableWritesWatermark)
            .GreaterThanOrEqual(0)
            .Default(10_GB);
        RegisterParameter("max_trash_ttl", MaxTrashTtl)
            .Default(TDuration::Hours(1));
        RegisterParameter("trash_cleanup_watermark", TrashCleanupWatermark)
            .GreaterThanOrEqual(0)
            .Default(40_GB);
        RegisterParameter("repair_in_throttler", RepairInThrottler)
            .DefaultNew();
        RegisterParameter("replication_in_throttler", ReplicationInThrottler)
            .DefaultNew();

        // NB: base class's field.
        RegisterParameter("medium_name", MediumName)
            .Default(NChunkClient::DefaultStoreMediumName);

        RegisterValidator([&] () {
            if (HighWatermark > LowWatermark) {
                THROW_ERROR_EXCEPTION("\"high_full_watermark\" must be less than or equal to \"low_watermark\"");
            }
            if (DisableWritesWatermark > HighWatermark) {
                THROW_ERROR_EXCEPTION("\"write_disable_watermark\" must be less than or equal to \"high_watermark\"");
            }
            if (DisableWritesWatermark > TrashCleanupWatermark) {
                THROW_ERROR_EXCEPTION("\"disable_writes_watermark\" must be less than or equal to \"trash_cleanup_watermark\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TStoreLocationConfig)

////////////////////////////////////////////////////////////////////////////////

class TCacheLocationConfig
    : public TStoreLocationConfigBase
{
public:
    //! Controls incoming location bandwidth used by cache.
    NConcurrency::TThroughputThrottlerConfigPtr InThrottler;

    TCacheLocationConfig()
    {
        RegisterParameter("in_throttler", InThrottler)
            .DefaultNew();

        // NB: base class's field.
        RegisterParameter("medium_name", MediumName)
            .Default(NChunkClient::DefaultCacheMediumName);
    }
};

DEFINE_REFCOUNTED_TYPE(TCacheLocationConfig)

////////////////////////////////////////////////////////////////////////////////

class TMultiplexedChangelogConfig
    : public NHydra::TFileChangelogConfig
    , public NHydra::TFileChangelogDispatcherConfig
{
public:
    //! Multiplexed changelog record count limit.
    /*!
     *  When this limit is reached, the current multiplexed changelog is rotated.
     */
    int MaxRecordCount;

    //! Multiplexed changelog data size limit, in bytes.
    /*!
     *  See #MaxRecordCount.
     */
    i64 MaxDataSize;

    //! Interval between automatic changelog rotation (to avoid keeping too many non-clean records
    //! and speed up starup).
    TDuration AutoRotationPeriod;

    //! Maximum bytes of multiplexed changelog to read during
    //! a single iteration of replay.
    i64 ReplayBufferSize;

    //! Maximum number of clean multiplexed changelogs to keep.
    int MaxCleanChangelogsToKeep;

    //! Time to wait before marking a multiplexed changelog as clean.
    TDuration CleanDelay;

    TMultiplexedChangelogConfig()
    {
        RegisterParameter("max_record_count", MaxRecordCount)
            .Default(1000000)
            .GreaterThan(0);
        RegisterParameter("max_data_size", MaxDataSize)
            .Default(256_MB)
            .GreaterThan(0);
        RegisterParameter("auto_rotation_period", AutoRotationPeriod)
            .Default(TDuration::Minutes(15));
        RegisterParameter("replay_buffer_size", ReplayBufferSize)
            .GreaterThan(0)
            .Default(256_MB);
        RegisterParameter("max_clean_changelogs_to_keep", MaxCleanChangelogsToKeep)
            .GreaterThanOrEqual(0)
            .Default(3);
        RegisterParameter("clean_delay", CleanDelay)
            .Default(TDuration::Minutes(1));
    }
};

DEFINE_REFCOUNTED_TYPE(TMultiplexedChangelogConfig)

////////////////////////////////////////////////////////////////////////////////

class TArtifactCacheReaderConfig
    : public virtual NChunkClient::TBlockFetcherConfig
    , public virtual NTableClient::TTableReaderConfig
    , public virtual NApi::TFileReaderConfig
{ };

DEFINE_REFCOUNTED_TYPE(TArtifactCacheReaderConfig)

////////////////////////////////////////////////////////////////////////////////

class TRepairReaderConfig
    : public NChunkClient::TReplicationReaderConfig
    , public TWorkloadConfig
{ };

DEFINE_REFCOUNTED_TYPE(TRepairReaderConfig)

////////////////////////////////////////////////////////////////////////////////

class TSealReaderConfig
    : public NChunkClient::TReplicationReaderConfig
    , public TWorkloadConfig
{ };

DEFINE_REFCOUNTED_TYPE(TSealReaderConfig)

////////////////////////////////////////////////////////////////////////////////

class TLayerLocationConfig
    : public TDiskLocationConfig
{
public:
    //! The location is considered to be full when available space becomes less than #LowWatermark.
    i64 LowWatermark;

    //! Maximum space layers are allowed to occupy.
    //! (If not initialized then indicates to occupy all available space on drive).
    TNullable<i64> Quota;

    TLayerLocationConfig()
    {
        RegisterParameter("low_watermark", LowWatermark)
            .Default(1_GB)
            .GreaterThan(0);

        RegisterParameter("quota", Quota)
            .Default(Null);
    }
};

DEFINE_REFCOUNTED_TYPE(TLayerLocationConfig)

////////////////////////////////////////////////////////////////////////////////

class TVolumeManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    std::vector<TLayerLocationConfigPtr> LayerLocations;
    TDuration PortoRetryTimeout;
    TDuration PortoPollPeriod;

    TVolumeManagerConfig()
    {
        RegisterParameter("layer_locations", LayerLocations);

        RegisterParameter("porto_retry_timeout", PortoRetryTimeout)
            .Default(TDuration::Seconds(1))
            .GreaterThan(TDuration::Zero());

        RegisterParameter("porto_poll_period", PortoPollPeriod)
            .Default(TDuration::MilliSeconds(200))
            .GreaterThan(TDuration::Zero());
    }

    i64 GetCacheCapacity() const
    {
        i64 result = 0;
        for (const auto& location : LayerLocations) {
            if (!location->Quota) {
                // Infinite capacity.
                return std::numeric_limits<i64>::max();
            } else {
                result += *location->Quota;
            }
        }
        return result;
    }
};

DEFINE_REFCOUNTED_TYPE(TVolumeManagerConfig)

////////////////////////////////////////////////////////////////////////////////

//! Describes a configuration of a data node.
class TDataNodeConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Timeout for lease transactions.
    TDuration LeaseTransactionTimeout;

    //! Period between consequent lease transaction pings.
    TDuration LeaseTransactionPingPeriod;

    //! Period between consequent incremental heartbeats.
    TDuration IncrementalHeartbeatPeriod;

    //! Period between consequent full heartbeats.
    TNullable<TDuration> FullHeartbeatPeriod;

    //! Period between consequent registration attempts.
    TDuration RegisterRetryPeriod;

    //! Timeout for RegisterNode requests.
    TDuration RegisterTimeout;

    //! Timeout for NodeTrackerService:IncrementalHeartbeat requests.
    TDuration IncrementalHeartbeatTimeout;

    //! Timeout for NodeTrackerService:FullHeartbeat requests.
    TDuration FullHeartbeatTimeout;

    //! Timeout for JobTrackerService:Heartbeat requests.
    TDuration JobHeartbeatTimeout;

    //! Cache for chunk metas.
    TSlruCacheConfigPtr ChunkMetaCache;

    //! Cache for all types of blocks.
    NChunkClient::TBlockCacheConfigPtr BlockCache;

    //! Opened blob chunks cache.
    TSlruCacheConfigPtr BlobReaderCache;

    //! Opened changelogs cache.
    TSlruCacheConfigPtr ChangelogReaderCache;

    //! Multiplexed changelog configuration.
    TMultiplexedChangelogConfigPtr MultiplexedChangelog;

    //! Configuration of per-chunk changelog that backs the multiplexed changelog.
    NHydra::TFileChangelogConfigPtr HighLatencySplitChangelog;

    //! Configuration of per-chunk changelog that is being written directly (w/o multiplexing).
    NHydra::TFileChangelogConfigPtr LowLatencySplitChangelog;

    //! Upload session timeout.
    /*!
     * Some activity must be happening in a session regularly (i.e. new
     * blocks uploaded or sent to other data nodes). Otherwise
     * the session expires.
     */
    TDuration SessionTimeout;

    //! Timeout for "PutBlocks" requests to other data nodes.
    TDuration NodeRpcTimeout;

    //! Period between peer updates (see TPeerBlockUpdater).
    TDuration PeerUpdatePeriod;

    //! Peer update expiration time (see TPeerBlockUpdater).
    TDuration PeerUpdateExpirationTime;

    //! Read requests are throttled when the number of bytes queued at Bus layer exceeds this limit.
    //! This is a global limit.
    //! Cf. TTcpDispatcherStatistics::PendingOutBytes
    i64 NetOutThrottlingLimit;

    //! Write requests are throttled when the number of bytes queued for write exceeds this limit.
    //! This is a per-location limit.
    i64 DiskWriteThrottlingLimit;

    //! Read requests are throttled when the number of bytes scheduled for read exceeds this limit.
    //! This is a per-location limit.
    i64 DiskReadThrottlingLimit;

    //! Regular storage locations.
    std::vector<TStoreLocationConfigPtr> StoreLocations;

    //! Cached chunks location.
    std::vector<TCacheLocationConfigPtr> CacheLocations;

    //! Manages layers and root volumes for porto job environment.
    TVolumeManagerConfigPtr VolumeManager;

    //! Reader configuration used to download chunks into cache.
    TArtifactCacheReaderConfigPtr ArtifactCacheReader;

    //! Writer configuration used to replicate chunks.
    NChunkClient::TReplicationWriterConfigPtr ReplicationWriter;

    //! Reader configuration used to repair chunks.
    TRepairReaderConfigPtr RepairReader;

    //! Writer configuration used to repair chunks.
    NChunkClient::TReplicationWriterConfigPtr RepairWriter;

    //! Reader configuration used to seal chunks.
    TSealReaderConfigPtr SealReader;

    //! Controls the total incoming bandwidth.
    NConcurrency::TThroughputThrottlerConfigPtr TotalInThrottler;

    //! Controls the total outcoming bandwidth.
    NConcurrency::TThroughputThrottlerConfigPtr TotalOutThrottler;

    //! Controls incoming bandwidth used by replication jobs.
    NConcurrency::TThroughputThrottlerConfigPtr ReplicationInThrottler;

    //! Controls outcoming bandwidth used by replication jobs.
    NConcurrency::TThroughputThrottlerConfigPtr ReplicationOutThrottler;

    //! Controls incoming bandwidth used by repair jobs.
    NConcurrency::TThroughputThrottlerConfigPtr RepairInThrottler;

    //! Controls outcoming bandwidth used by repair jobs.
    NConcurrency::TThroughputThrottlerConfigPtr RepairOutThrottler;

    //! Controls incoming bandwidth used by Artifact Cache downloads.
    NConcurrency::TThroughputThrottlerConfigPtr ArtifactCacheInThrottler;

    //! Controls outcoming bandwidth used by Artifact Cache downloads.
    NConcurrency::TThroughputThrottlerConfigPtr ArtifactCacheOutThrottler;

    //! Keeps chunk peering information.
    TPeerBlockTableConfigPtr PeerBlockTable;

    //! Runs periodic checks against disks.
    TDiskHealthCheckerConfigPtr DiskHealthChecker;

    //! The number of reader threads per location (for blob data only; meta reader always has a separate thread).
    int ReadThreadCount;

    //! Number of writer threads per location.
    int WriteThreadCount;

    //! Maximum number of concurrent balancing write sessions.
    int MaxWriteSessions;

    //! Maximum number of blocks to fetch via a single range request.
    int MaxBlocksPerRead;

    //! Maximum number of bytes to fetch via a single range request.
    i64 MaxBytesPerRead;

    //! Desired number of bytes per disk write in a blob chunks.
    i64 BytesPerWrite;

    //! Enables block checksums validation.
    bool ValidateBlockChecksums;

    //! Use DIRECT_IO flag when writing chunks data to disk.
    EDirectIOPolicy UseDirectIO;

    bool EnableExperimentalSkynetHttpApi;

    //! The time after which any registered placement info expires.
    TDuration PlacementExpirationTime;

    //! Controls if cluster and cell directories are to be synchronized on connect.
    //! Useful for tests.
    bool SyncDirectoriesOnConnect;

    TDataNodeConfig()
    {
        RegisterParameter("lease_transaction_timeout", LeaseTransactionTimeout)
            .Default(TDuration::Seconds(120));
        RegisterParameter("lease_transaction_ping_period", LeaseTransactionPingPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("incremental_heartbeat_period", IncrementalHeartbeatPeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("full_heartbeat_period", FullHeartbeatPeriod)
            .Default();
        RegisterParameter("register_retry_period", RegisterRetryPeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("register_timeout", RegisterTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("incremental_heartbeat_timeout", IncrementalHeartbeatTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("full_heartbeat_timeout", FullHeartbeatTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("job_heartbeat_timeout", JobHeartbeatTimeout)
            .Default(TDuration::Seconds(60));

        RegisterParameter("chunk_meta_cache", ChunkMetaCache)
            .DefaultNew();
        RegisterParameter("block_cache", BlockCache)
            .DefaultNew();
        RegisterParameter("blob_reader_cache", BlobReaderCache)
            .DefaultNew();
        RegisterParameter("changelog_reader_cache", ChangelogReaderCache)
            .DefaultNew();

        RegisterParameter("multiplexed_changelog", MultiplexedChangelog)
            .DefaultNew();
        RegisterParameter("high_latency_split_changelog", HighLatencySplitChangelog)
            .DefaultNew();
        RegisterParameter("low_latency_split_changelog", LowLatencySplitChangelog)
            .DefaultNew();

        RegisterParameter("session_timeout", SessionTimeout)
            .Default(TDuration::Seconds(120));
        RegisterParameter("node_rpc_timeout", NodeRpcTimeout)
            .Default(TDuration::Seconds(120));
        RegisterParameter("peer_update_period", PeerUpdatePeriod)
            .Default(TDuration::Seconds(30));
        RegisterParameter("peer_update_expiration_time", PeerUpdateExpirationTime)
            .Default(TDuration::Seconds(40));

        RegisterParameter("net_out_throttling_limit", NetOutThrottlingLimit)
            .GreaterThan(0)
            .Default(512_MB);
        RegisterParameter("disk_write_throttling_limit", DiskWriteThrottlingLimit)
            .GreaterThan(0)
            .Default(1_GB);
        RegisterParameter("disk_read_throttling_limit", DiskReadThrottlingLimit)
            .GreaterThan(0)
            .Default(512_MB);

        RegisterParameter("store_locations", StoreLocations)
            .NonEmpty();
        RegisterParameter("cache_locations", CacheLocations)
            .NonEmpty();

        RegisterParameter("volume_manager", VolumeManager)
            .DefaultNew();

        RegisterParameter("artifact_cache_reader", ArtifactCacheReader)
            .DefaultNew();
        RegisterParameter("replication_writer", ReplicationWriter)
            .DefaultNew();
        RegisterParameter("repair_reader", RepairReader)
            .DefaultNew();
        RegisterParameter("repair_writer", RepairWriter)
            .DefaultNew();

        RegisterParameter("seal_reader", SealReader)
            .DefaultNew();

        RegisterParameter("total_in_throttler", TotalInThrottler)
            .DefaultNew();
        RegisterParameter("total_out_throttler", TotalOutThrottler)
            .DefaultNew();
        RegisterParameter("replication_in_throttler", ReplicationInThrottler)
            .DefaultNew();
        RegisterParameter("replication_out_throttler", ReplicationOutThrottler)
            .DefaultNew();
        RegisterParameter("repair_in_throttler", RepairInThrottler)
            .DefaultNew();
        RegisterParameter("repair_out_throttler", RepairOutThrottler)
            .DefaultNew();
        RegisterParameter("artifact_cache_in_throttler", ArtifactCacheInThrottler)
            .DefaultNew();
        RegisterParameter("artifact_cache_out_throttler", ArtifactCacheOutThrottler)
            .DefaultNew();

        RegisterParameter("peer_block_table", PeerBlockTable)
            .DefaultNew();

        RegisterParameter("disk_health_checker", DiskHealthChecker)
            .DefaultNew();

        RegisterParameter("read_thread_count", ReadThreadCount)
            .Default(1)
            .GreaterThanOrEqual(1);
        RegisterParameter("write_thread_count", WriteThreadCount)
            .Default(1)
            .GreaterThanOrEqual(1);

        RegisterParameter("max_write_sessions", MaxWriteSessions)
            .Default(1000)
            .GreaterThanOrEqual(1);

        RegisterParameter("max_blocks_per_read", MaxBlocksPerRead)
            .GreaterThan(0)
            .Default(100000);
        RegisterParameter("max_bytes_per_read", MaxBytesPerRead)
            .GreaterThan(0)
            .Default(64_MB);
        RegisterParameter("bytes_per_write", BytesPerWrite)
            .GreaterThan(0)
            .Default(16_MB);

        RegisterParameter("validate_block_checksums", ValidateBlockChecksums)
            .Default(true);

        RegisterParameter("use_direct_io", UseDirectIO)
            .Default(EDirectIOPolicy::Never);

        RegisterParameter("enable_experimental_skynet_http_api", EnableExperimentalSkynetHttpApi)
            .Default(false);

        RegisterParameter("placement_expiration_time", PlacementExpirationTime)
            .Default(TDuration::Hours(1));

        RegisterParameter("sync_directories_on_connect", SyncDirectoriesOnConnect)
            .Default(false);

        RegisterInitializer([&] () {
            ChunkMetaCache->Capacity = 1_GB;

            BlockCache->CompressedData->Capacity = 1_GB;
            BlockCache->UncompressedData->Capacity = 1_GB;

            BlobReaderCache->Capacity = 256;

            ChangelogReaderCache->Capacity = 256;

            // Expect many splits -- adjust configuration.
            HighLatencySplitChangelog->FlushPeriod = TDuration::Seconds(15);

            // Disable target allocation from master.
            ReplicationWriter->UploadReplicationFactor = 1;
            RepairWriter->UploadReplicationFactor = 1;

            // Use proper workload descriptors.
            RepairReader->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemRepair);
            RepairWriter->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemRepair);
            SealReader->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemReplication);
            ReplicationWriter->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemReplication);
            ArtifactCacheReader->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemArtifactCacheDownload);

            // Don't populate caches in chunk jobs.
            RepairReader->PopulateCache = false;
            SealReader->PopulateCache = false;
        });
    }

    i64 GetCacheCapacity() const
    {
        bool unlimited = false;
        i64 capacity = 0;

        for (const auto& config : CacheLocations) {
            if (!unlimited) {
                if (config->Quota) {
                    capacity += *config->Quota;
                } else {
                    unlimited = true;
                }
            }
        }

        return unlimited ? std::numeric_limits<i64>::max() : capacity;
    }
};

DEFINE_REFCOUNTED_TYPE(TDataNodeConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
