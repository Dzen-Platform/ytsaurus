#pragma once

#include "public.h"

#include <core/misc/config.h>

#include <core/concurrency/throughput_throttler.h>

#include <ytlib/chunk_client/config.h>

#include <server/hydra/config.h>

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

class TLocationConfigBase
    : public NYTree::TYsonSerializable
{
public:
    //! Root directory for the location.
    Stroka Path;

    //! Maximum space chunks are allowed to occupy.
    //! (If not initialized then indicates to occupy all available space on drive).
    TNullable<i64> Quota;

    //! Minimum size the disk partition must have to make this location usable.
    TNullable<i64> MinDiskSpace;

    TLocationConfigBase()
    {
        RegisterParameter("path", Path)
            .NonEmpty();
        RegisterParameter("quota", Quota)
            .GreaterThanOrEqual(0)
            .Default(TNullable<i64>());
        RegisterParameter("min_disk_space", MinDiskSpace)
            .GreaterThanOrEqual(0)
            .Default(TNullable<i64>());
    }
};

DEFINE_REFCOUNTED_TYPE(TLocationConfigBase);

class TStoreLocationConfig
    : public TLocationConfigBase
{
public:
    //! The location is considered to be full when available space becomes less than #LowWatermark.
    i64 LowWatermark;

    //! All uploads to the location are aborted when available space becomes less than #HighWatermark.
    i64 HighWatermark;

    //! Maximum amount of time files of a deleted chunk could rest in trash directory before
    //! being permanently removed.
    TDuration MaxTrashTtl;

    //! When free space drops below this watermark, the system starts deleting files in trash directory,
    //! starting from the eldest ones.
    i64 TrashCleanupWatermark;

    //! Controls if new blob chunks are accpeted by this location.
    bool EnableBlobs;

    //! Controls if new journal chunks are accepted by this location.
    bool EnableJournals;

    TStoreLocationConfig()
    {
        RegisterParameter("low_watermark", LowWatermark)
            .GreaterThanOrEqual(0)
            .Default((i64) 20 * 1024 * 1024 * 1024); // 20 Gb
        RegisterParameter("high_watermark", HighWatermark)
            .GreaterThanOrEqual(0)
            .Default((i64) 10 * 1024 * 1024 * 1024); // 10 Gb
        RegisterParameter("max_trash_ttl", MaxTrashTtl)
            .Default(TDuration::Hours(1));
        RegisterParameter("trash_cleanup_watermark", TrashCleanupWatermark)
            .GreaterThanOrEqual(0)
            .Default((i64) 40 * 1024 * 1024 * 1024); // 40 Gb
        RegisterParameter("enable_blobs", EnableBlobs)
            .Default(true);
        RegisterParameter("enable_journals", EnableJournals)
            .Default(true);

        RegisterValidator([&] () {
            if (HighWatermark > LowWatermark) {
                THROW_ERROR_EXCEPTION("\"high_watermark\" must be less than or equal to \"low_watermark\"");
            }
            if (LowWatermark > TrashCleanupWatermark) {
                THROW_ERROR_EXCEPTION("\"low_watermark\" must be less than or equal to \"trash_cleanup_watermark\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TStoreLocationConfig);

class TCacheLocationConfig
    : public TLocationConfigBase
{ };

DEFINE_REFCOUNTED_TYPE(TCacheLocationConfig);

class TDiskHealthCheckerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Period between consequent checks.
    TDuration CheckPeriod;

    //! Size of the test file.
    i64 TestSize;

    //! Maximum time allowed for execution of a single check.
    TDuration Timeout;

    TDiskHealthCheckerConfig()
    {
        RegisterParameter("check_period", CheckPeriod)
            .Default(TDuration::Minutes(1));
        RegisterParameter("test_size", TestSize)
            .InRange(0, (i64) 1024 * 1024 * 1024)
            .Default((i64) 1024 * 1024);
        RegisterParameter("timeout", Timeout)
            .Default(TDuration::Seconds(60));
    }
};

DEFINE_REFCOUNTED_TYPE(TDiskHealthCheckerConfig)

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
            .Default((i64) 256 * 1024 * 1024)
            .GreaterThan(0);
        RegisterParameter("auto_rotation_period", AutoRotationPeriod)
            .Default(TDuration::Minutes(15));
        RegisterParameter("replay_buffer_size", ReplayBufferSize)
            .GreaterThan(0)
            .Default((i64) 256 * 1024 * 1024);
        RegisterParameter("max_clean_changelogs_to_keep", MaxCleanChangelogsToKeep)
            .GreaterThanOrEqual(0)
            .Default(3);
        RegisterParameter("clean_delay", CleanDelay)
            .Default(TDuration::Minutes(1));
    }
};

DEFINE_REFCOUNTED_TYPE(TMultiplexedChangelogConfig)

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

    //! Random delay before first heartbeat
    TDuration HeartbeatSplay;

    //! Timeout for IncrementalHeartbeat requests.
    /*!
     *  This must not be too long to prevent node lease from expiring.
     */
    TDuration IncrementalHeartbeatTimeout;

    //! Timeout for FullHeartbeat requests.
    /*!
     *  This is usually much larger then the default RPC timeout.
     */
    TDuration FullHeartbeatTimeout;

    //! Cache for all types of blocks.
    NChunkClient::TBlockCacheConfigPtr BlockCache;

    //! Opened blob chunks cache.
    TSlruCacheConfigPtr BlobReaderCache;

    //! Multiplexed changelog configuration.
    TMultiplexedChangelogConfigPtr MultiplexedChangelog;

    //! Split (per chunk) changelog configuration.
    NHydra::TFileChangelogConfigPtr SplitChangelog;

    //! Opened changelogs cache.
    TSlruCacheConfigPtr ChangelogReaderCache;

    //! Upload session timeout.
    /*!
     * Some activity must be happening in a session regularly (i.e. new
     * blocks uploaded or sent to other data nodes). Otherwise
     * the session expires.
     */
    TDuration SessionTimeout;

    //! Timeout for "PutBlock" requests to other data nodes.
    TDuration NodeRpcTimeout;

    //! Period between peer updates (see TPeerBlockUpdater).
    TDuration PeerUpdatePeriod;

    //! Updated expiration timeout (see TPeerBlockUpdater).
    TDuration PeerUpdateExpirationTimeout;

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

    //! Remote reader configuration used to download chunks into cache.
    NChunkClient::TReplicationReaderConfigPtr CacheRemoteReader;

    //! Sequential reader configuration used to download chunks into cache.
    NChunkClient::TSequentialReaderConfigPtr CacheSequentialReader;

    //! Writer configuration used to replicate chunks.
    NChunkClient::TReplicationWriterConfigPtr ReplicationWriter;

    //! Reader configuration used to repair chunks.
    NChunkClient::TReplicationReaderConfigPtr RepairReader;

    //! Writer configuration used to repair chunks.
    NChunkClient::TReplicationWriterConfigPtr RepairWriter;

    //! Reader configuration used to seal chunks.
    NChunkClient::TReplicationReaderConfigPtr SealReader;

    //! Controls incoming bandwidth used by replication jobs.
    NConcurrency::TThroughputThrottlerConfigPtr ReplicationInThrottler;

    //! Controls outcoming bandwidth used by replication jobs.
    NConcurrency::TThroughputThrottlerConfigPtr ReplicationOutThrottler;

    //! Controls incoming bandwidth used by repair jobs.
    NConcurrency::TThroughputThrottlerConfigPtr RepairInThrottler;

    //! Controls outcoming bandwidth used by repair jobs.
    NConcurrency::TThroughputThrottlerConfigPtr RepairOutThrottler;

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

    //! Enables block checksums validation.
    bool ValidateBlockChecksums;


    TDataNodeConfig()
    {
        RegisterParameter("lease_transaction_timeout", LeaseTransactionTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("lease_transaction_ping_period", LeaseTransactionPingPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("incremental_heartbeat_period", IncrementalHeartbeatPeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("full_heartbeat_period", FullHeartbeatPeriod)
            .Default();
        RegisterParameter("incremental_heartbeat_timeout", IncrementalHeartbeatTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("full_heartbeat_timeout", FullHeartbeatTimeout)
            .Default(TDuration::Seconds(60));
        
        RegisterParameter("block_cache", BlockCache)
            .DefaultNew();

        RegisterParameter("blob_reader_cache", BlobReaderCache)
            .DefaultNew();

        RegisterParameter("multiplexed_changelog", MultiplexedChangelog)
            .DefaultNew();
        RegisterParameter("split_changelog", SplitChangelog)
            .DefaultNew();
        RegisterParameter("changelog_reader_cache", ChangelogReaderCache)
            .DefaultNew();

        RegisterParameter("session_timeout", SessionTimeout)
            .Default(TDuration::Seconds(120));
        RegisterParameter("node_rpc_timeout", NodeRpcTimeout)
            .Default(TDuration::Seconds(120));
        RegisterParameter("peer_update_period", PeerUpdatePeriod)
            .Default(TDuration::Seconds(30));
        RegisterParameter("peer_update_expiration_timeout", PeerUpdateExpirationTimeout)
            .Default(TDuration::Seconds(40));

        RegisterParameter("net_out_throttling_limit", NetOutThrottlingLimit)
            .GreaterThan(0)
            .Default((i64) 512 * 1024 * 1024);
        RegisterParameter("disk_write_throttling_limit", DiskWriteThrottlingLimit)
            .GreaterThan(0)
            .Default((i64) 1024 * 1024 * 1024);
        RegisterParameter("disk_read_throttling_limit", DiskReadThrottlingLimit)
            .GreaterThan(0)
            .Default((i64) 512 * 1024 * 1024);

        RegisterParameter("store_locations", StoreLocations)
            .NonEmpty();
        RegisterParameter("cache_locations", CacheLocations)
            .NonEmpty();

        RegisterParameter("cache_remote_reader", CacheRemoteReader)
            .DefaultNew();
        RegisterParameter("cache_sequential_reader", CacheSequentialReader)
            .DefaultNew();
        RegisterParameter("replication_writer", ReplicationWriter)
            .DefaultNew();
        RegisterParameter("repair_reader", RepairReader)
            .DefaultNew();
        RegisterParameter("repair_writer", RepairWriter)
            .DefaultNew();

        RegisterParameter("seal_reader", SealReader)
            .DefaultNew();

        RegisterParameter("replication_in_throttler", ReplicationInThrottler)
            .DefaultNew();
        RegisterParameter("replication_out_throttler", ReplicationOutThrottler)
            .DefaultNew();
        RegisterParameter("repair_in_throttler", RepairInThrottler)
            .DefaultNew();
        RegisterParameter("repair_out_throttler", RepairOutThrottler)
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
            .Default((i64) 64 * 1024 * 1024);
        RegisterParameter("validate_block_checksums", ValidateBlockChecksums)
            .Default(true);

        RegisterInitializer([&] () {
            BlockCache->CompressedData->Capacity = (i64) 1024 * 1024 * 1024;
            BlockCache->UncompressedData->Capacity = (i64) 1024 * 1024 * 1024;

            BlobReaderCache->Capacity = 256;

            ChangelogReaderCache->Capacity = 256;

            // Expect many splits -- adjust configuration.
            SplitChangelog->FlushBufferSize = (i64) 16 * 1024 * 1024;
            SplitChangelog->FlushPeriod = TDuration::Seconds(15);

            // Disable target allocation from master.
            ReplicationWriter->UploadReplicationFactor = 1;
            RepairWriter->UploadReplicationFactor = 1;

            // Use proper workload descriptors.
            RepairReader->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemRepair);
            RepairWriter->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemRepair);
            SealReader->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemReplication);
            ReplicationWriter->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemReplication);
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
