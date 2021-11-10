#pragma once

#include "public.h"

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/server/lib/hive/config.h>

#include <yt/yt/ytlib/table_client/config.h>

#include <yt/yt/core/ytree/yson_serializable.h>

#include <yt/yt/core/misc/arithmetic_formula.h>

namespace NYT::NTabletServer {

////////////////////////////////////////////////////////////////////////////////

class TTabletBalancerConfig
    : public NYTree::TYsonSerializable
{
public:
    bool EnableInMemoryCellBalancer;
    bool EnableCellBalancer;
    bool EnableTabletSizeBalancer;

    bool EnableTabletCellSmoothing;

    double HardInMemoryCellBalanceThreshold;
    double SoftInMemoryCellBalanceThreshold;

    i64 MinTabletSize;
    i64 MaxTabletSize;
    i64 DesiredTabletSize;

    i64 MinInMemoryTabletSize;
    i64 MaxInMemoryTabletSize;
    i64 DesiredInMemoryTabletSize;

    double TabletToCellRatio;

    TTimeFormula TabletBalancerSchedule;

    bool EnableVerboseLogging;

    TTabletBalancerConfig()
    {
        RegisterParameter("enable_in_memory_cell_balancer", EnableInMemoryCellBalancer)
            .Default(true)
            .Alias("enable_in_memory_balancer");

        RegisterParameter("enable_cell_balancer", EnableCellBalancer)
            .Default(false);

        RegisterParameter("enable_tablet_size_balancer", EnableTabletSizeBalancer)
            .Default(true);

        // COMPAT(savrus) Only for compatibility purpose.
        RegisterParameter("compat_enable_tablet_cell_smoothing", EnableTabletCellSmoothing)
            .Default(true)
            .Alias("enable_tablet_cell_smoothing");

        RegisterParameter("soft_in_memory_cell_balance_threshold", SoftInMemoryCellBalanceThreshold)
            .Default(0.05)
            .Alias("cell_balance_factor");

        RegisterParameter("hard_in_memory_cell_balance_threshold", HardInMemoryCellBalanceThreshold)
            .Default(0.15);

        RegisterParameter("min_tablet_size", MinTabletSize)
            .Default(128_MB);

        RegisterParameter("max_tablet_size", MaxTabletSize)
            .Default(20_GB);

        RegisterParameter("desired_tablet_size", DesiredTabletSize)
            .Default(10_GB);

        RegisterParameter("min_in_memory_tablet_size", MinInMemoryTabletSize)
            .Default(512_MB);

        RegisterParameter("max_in_memory_tablet_size", MaxInMemoryTabletSize)
            .Default(2_GB);

        RegisterParameter("desired_in_memory_tablet_size", DesiredInMemoryTabletSize)
            .Default(1_GB);

        RegisterParameter("tablet_to_cell_ratio", TabletToCellRatio)
            .GreaterThan(0)
            .Default(5.0);

        RegisterParameter("tablet_balancer_schedule", TabletBalancerSchedule)
            .Default();

        RegisterParameter("enable_verbose_logging", EnableVerboseLogging)
            .Default(false);

        RegisterPostprocessor([&] () {
            if (MinTabletSize > DesiredTabletSize) {
                THROW_ERROR_EXCEPTION("\"min_tablet_size\" must be less than or equal to \"desired_tablet_size\"");
            }
            if (DesiredTabletSize > MaxTabletSize) {
                THROW_ERROR_EXCEPTION("\"desired_tablet_size\" must be less than or equal to \"max_tablet_size\"");
            }
            if (MinInMemoryTabletSize >= DesiredInMemoryTabletSize) {
                THROW_ERROR_EXCEPTION("\"min_in_memory_tablet_size\" must be less than \"desired_in_memory_tablet_size\"");
            }
            if (DesiredInMemoryTabletSize >= MaxInMemoryTabletSize) {
                THROW_ERROR_EXCEPTION("\"desired_in_memory_tablet_size\" must be less than \"max_in_memory_tablet_size\"");
            }
            if (SoftInMemoryCellBalanceThreshold > HardInMemoryCellBalanceThreshold) {
                THROW_ERROR_EXCEPTION("\"soft_in_memory_cell_balance_threshold\" must less than or equal to "
                    "\"hard_in_memory_cell_balance_threshold\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TTabletBalancerConfig)

////////////////////////////////////////////////////////////////////////////////

class TTabletBalancerMasterConfig
    : public NYTree::TYsonSerializable
{
public:
    bool EnableTabletBalancer;
    TTimeFormula TabletBalancerSchedule;

    TDuration ConfigCheckPeriod;
    TDuration BalancePeriod;

    TTabletBalancerMasterConfig()
    {
        RegisterParameter("enable_tablet_balancer", EnableTabletBalancer)
            .Default(true);
        RegisterParameter("tablet_balancer_schedule", TabletBalancerSchedule)
            .Default(DefaultTabletBalancerSchedule);
        RegisterParameter("config_check_period", ConfigCheckPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("balance_period", BalancePeriod)
            .Default(TDuration::Minutes(5));

        RegisterPostprocessor([&] () {
            if (TabletBalancerSchedule.IsEmpty()) {
                THROW_ERROR_EXCEPTION("tablet_balancer_schedule cannot be empty in master config");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TTabletBalancerMasterConfig)

////////////////////////////////////////////////////////////////////////////////

class TTabletCellDecommissionerConfig
    : public NYTree::TYsonSerializable
{
public:
    bool EnableTabletCellDecommission;
    bool EnableTabletCellRemoval;

    TDuration DecommissionCheckPeriod;
    TDuration OrphansCheckPeriod;

    NConcurrency::TThroughputThrottlerConfigPtr DecommissionThrottler;
    NConcurrency::TThroughputThrottlerConfigPtr KickOrphansThrottler;

    TTabletCellDecommissionerConfig()
    {
        RegisterParameter("enable_tablet_cell_decommission", EnableTabletCellDecommission)
            .Default(true);
        RegisterParameter("enable_tablet_cell_removal", EnableTabletCellRemoval)
            .Default(true);
        RegisterParameter("decommission_check_period", DecommissionCheckPeriod)
            .Default(TDuration::Seconds(30));
        RegisterParameter("orphans_check_period", OrphansCheckPeriod)
            .Default(TDuration::Seconds(30));
        RegisterParameter("decommission_throttler", DecommissionThrottler)
            .DefaultNew();
        RegisterParameter("kick_orphans_throttler", KickOrphansThrottler)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TTabletCellDecommissionerConfig)

////////////////////////////////////////////////////////////////////////////////

class TTabletActionManagerMasterConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration TabletActionsCleanupPeriod;

    TTabletActionManagerMasterConfig()
    {
        RegisterParameter("tablet_actions_cleanup_period", TabletActionsCleanupPeriod)
            .Default(TDuration::Minutes(1));
    }
};

DEFINE_REFCOUNTED_TYPE(TTabletActionManagerMasterConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicTablesMulticellGossipConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Multicell tablet cell statistics gossip period.
    TDuration TabletCellStatisticsGossipPeriod;

    //! Multicell tablet cell status full gossip period.
    // COMPAT(gritukan): If not set, #TabletCellStatisticsGossipPeriod is used instead.
    // Set this option at all clusters and drop optional.
    std::optional<TDuration> TabletCellStatusFullGossipPeriod;

    //! Multicell tablet cell status incremental gossip period.
    //! If not set, only full tablet cell status gossip is performed.
    std::optional<TDuration> TabletCellStatusIncrementalGossipPeriod;

    //! Multicell table (e.g. chunk owner) statistics gossip period.
    TDuration TableStatisticsGossipPeriod;

    //! Throttler for table statistics gossip.
    NConcurrency::TThroughputThrottlerConfigPtr TableStatisticsGossipThrottler;

    //! Bundle resource usage gossip period.
    TDuration BundleResourceUsageGossipPeriod;

    bool EnableUpdateStatisticsOnHeartbeat;

    TDynamicTablesMulticellGossipConfig()
    {
        RegisterParameter("tablet_cell_statistics_gossip_period", TabletCellStatisticsGossipPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("tablet_cell_status_full_gossip_period", TabletCellStatusFullGossipPeriod)
            .Default();
        RegisterParameter("tablet_cell_status_incremental_gossip_period", TabletCellStatusIncrementalGossipPeriod)
            .Default();
        RegisterParameter("table_statistics_gossip_period", TableStatisticsGossipPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("table_statistics_gossip_throttler", TableStatisticsGossipThrottler)
            .DefaultNew();
        RegisterParameter("bundle_resource_usage_gossip_period", BundleResourceUsageGossipPeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("enable_update_statistics_on_heartbeat", EnableUpdateStatisticsOnHeartbeat)
            .Default(true);
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicTablesMulticellGossipConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicTabletCellBalancerMasterConfig
    : public NYTree::TYsonSerializable
{
public:
    bool EnableTabletCellSmoothing;
    bool EnableVerboseLogging;
    TDuration RebalanceWaitTime;

    TDynamicTabletCellBalancerMasterConfig()
    {
        RegisterParameter("enable_tablet_cell_smoothing", EnableTabletCellSmoothing)
            .Default(true);
        RegisterParameter("enable_verbose_logging", EnableVerboseLogging)
            .Default(false);
        RegisterParameter("rebalance_wait_time", RebalanceWaitTime)
            .Default(TDuration::Minutes(1));
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicTabletCellBalancerMasterConfig)

////////////////////////////////////////////////////////////////////////////////

class TReplicatedTableTrackerExpiringCacheConfig
    : public TAsyncExpiringCacheConfig
{
public:
    TReplicatedTableTrackerExpiringCacheConfig()
    {
        RegisterPreprocessor([this] () {
            RefreshTime = std::nullopt;
            ExpireAfterAccessTime = TDuration::Seconds(1);
            ExpireAfterSuccessfulUpdateTime = TDuration::Seconds(1);
            ExpireAfterFailedUpdateTime = TDuration::Seconds(1);
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TReplicatedTableTrackerExpiringCacheConfig);

////////////////////////////////////////////////////////////////////////////////

class TReplicatedTableTrackerConfig
    : public NYTree::TYsonSerializable
{
public:
    int CheckerThreadCount;

    TReplicatedTableTrackerConfig()
    {
        RegisterParameter("checker_thread_count", CheckerThreadCount)
            .Default(1);
    }
};

DEFINE_REFCOUNTED_TYPE(TReplicatedTableTrackerConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicReplicatedTableTrackerConfig
    : public NYTree::TYsonSerializable
{
public:
    bool EnableReplicatedTableTracker;

    TDuration CheckPeriod;
    TDuration UpdatePeriod;

    TDuration GeneralCheckTimeout;

    NTabletNode::TReplicatorHintConfigPtr ReplicatorHint;
    TAsyncExpiringCacheConfigPtr BundleHealthCache;
    TAsyncExpiringCacheConfigPtr ClusterStateCache;
    NHiveServer::TClusterDirectorySynchronizerConfigPtr ClusterDirectorySynchronizer;

    i64 MaxIterationsWithoutAcceptableBundleHealth;

    TDynamicReplicatedTableTrackerConfig()
    {
        RegisterParameter("enable_replicated_table_tracker", EnableReplicatedTableTracker)
            .Default(true);
        RegisterParameter("check_period", CheckPeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("update_period", UpdatePeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("general_check_timeout", GeneralCheckTimeout)
            .Default(TDuration::Minutes(1))
            .DontSerializeDefault();
        RegisterParameter("replicator_hint", ReplicatorHint)
            .DefaultNew();
        RegisterParameter("bundle_health_cache", BundleHealthCache)
            .DefaultNew();
        RegisterParameter("cluster_state_cache", ClusterStateCache)
            .DefaultNew();
        RegisterParameter("cluster_directory_synchronizer", ClusterDirectorySynchronizer)
            .DefaultNew();
        RegisterParameter("max_iterations_without_acceptable_bundle_health", MaxIterationsWithoutAcceptableBundleHealth)
            .Default(1)
            .DontSerializeDefault();

        RegisterPreprocessor([&] {
            ClusterStateCache->RefreshTime = CheckPeriod;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicReplicatedTableTrackerConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicTabletNodeTrackerConfig
    : public NYTree::TYsonSerializable
{
public:
    int MaxConcurrentHeartbeats;

    TDynamicTabletNodeTrackerConfig()
    {
        RegisterParameter("max_concurrent_heartbeats", MaxConcurrentHeartbeats)
            .Default(10)
            .GreaterThan(0);
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicTabletNodeTrackerConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicTabletManagerConfig
    : public NHydra::THydraJanitorConfig
{
public:
    //! Time to wait for a node to be back online before revoking it from all
    //! tablet cells.
    TDuration PeerRevocationTimeout;

    //! Time to wait before resetting leader to another peer.
    TDuration LeaderReassignmentTimeout;

    //! Maximum number of snapshots to keep for a tablet cell.
    std::optional<int> MaxSnapshotCountToKeep;

    //! Maximum total size of snapshots to keep for a tablet cell.
    std::optional<i64> MaxSnapshotSizeToKeep;

    //! Maximum number of snapshots to remove per a single check.
    int MaxSnapshotCountToRemovePerCheck;

    //! Maximum number of snapshots to remove per a single check.
    int MaxChangelogCountToRemovePerCheck;

    //! When the number of online nodes drops below this margin,
    //! tablet cell peers are no longer assigned and revoked.
    int SafeOnlineNodeCount;

    //! Internal between tablet cell examinations.
    TDuration CellScanPeriod;

    bool EnableCellTracker;

    //! Additional number of bytes per tablet to charge each cell
    //! for balancing purposes.
    i64 TabletDataSizeFootprint;

    //! Store chunk reader config for all dynamic tables.
    NTabletNode::TTabletStoreReaderConfigPtr StoreChunkReader;

    //! Hunk chunk reader config for all dynamic tables.
    NTabletNode::TTabletHunkReaderConfigPtr HunkChunkReader;

    //! Store chunk writer config for all dynamic tables.
    NTabletNode::TTabletStoreWriterConfigPtr StoreChunkWriter;

    //! Hunk chunk writer config for all dynamic tables.
    NTabletNode::TTabletHunkWriterConfigPtr HunkChunkWriter;

    //! Tablet balancer config.
    TTabletBalancerMasterConfigPtr TabletBalancer;

    //! Tablet cell decommissioner config.
    TTabletCellDecommissionerConfigPtr TabletCellDecommissioner;

    //! Tablet action manager config.
    TTabletActionManagerMasterConfigPtr TabletActionManager;

    //! Dynamic tables multicell gossip config.
    TDynamicTablesMulticellGossipConfigPtr MulticellGossip;

    TDuration TabletCellsCleanupPeriod;

    NTabletNode::EDynamicTableProfilingMode DynamicTableProfilingMode;

    TDynamicTabletCellBalancerMasterConfigPtr TabletCellBalancer;

    TDynamicReplicatedTableTrackerConfigPtr ReplicatedTableTracker;

    bool EnableBulkInsert;

    bool DecommissionThroughExtraPeers;

    // TODO(gritukan): Move it to node dynamic config when it will be ready.
    bool AbandonLeaderLeaseDuringRecovery;

    //! This parameter is used only for testing purposes.
    std::optional<TDuration> DecommissionedLeaderReassignmentTimeout;

    bool EnableDynamicStoreReadByDefault;

    //! Peer revocation reason is reset after this period of time.
    TDuration PeerRevocationReasonExpirationTime;

    //! If set, tablet statistics will be validated upon each attributes request
    //! to the table node.
    bool EnableAggressiveTabletStatisticsValidation;

    //! If set, tablet statistics will be validated upon each @tablet_statistics request
    //! to the table node.
    bool EnableRelaxedTabletStatisticsValidation;

    //! Time to wait before peer count update after new leader assignment
    //! during decommission through extra peers.
    TDuration ExtraPeerDropDelay;

    // COMPAT(ifsmirnov): YT-13541
    bool AccumulatePreloadPendingStoreCountCorrectly;

    // COMPAT(akozhikhov): YT-14187
    bool IncreaseUploadReplicationFactor;

    //! If set, tablet resource limit violation will be validated per-bundle.
    // TODO(ifsmirnov): remove and set default to true.
    bool EnableTabletResourceValidation;

    TDynamicTabletNodeTrackerConfigPtr TabletNodeTracker;

    // COMPAT(babenko)
    bool EnableHunks;

    TDynamicTabletManagerConfig()
    {
        RegisterParameter("peer_revocation_timeout", PeerRevocationTimeout)
            .Default(TDuration::Minutes(1));
        RegisterParameter("leader_reassignment_timeout", LeaderReassignmentTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("max_snapshot_count_to_remove_per_check", MaxSnapshotCountToRemovePerCheck)
            .GreaterThan(0)
            .Default(100);
        RegisterParameter("max_changelog_count_to_remove_per_check", MaxChangelogCountToRemovePerCheck)
            .GreaterThan(0)
            .Default(100);
        RegisterParameter("safe_online_node_count", SafeOnlineNodeCount)
            .GreaterThanOrEqual(0)
            .Default(0);
        RegisterParameter("cell_scan_period", CellScanPeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("enable_cell_tracker", EnableCellTracker)
            .Default(true);
        RegisterParameter("tablet_data_size_footprint", TabletDataSizeFootprint)
            .GreaterThanOrEqual(0)
            .Default(64_MB);
        RegisterParameter("store_chunk_reader", StoreChunkReader)
            .Alias("chunk_reader")
            .DefaultNew();
        RegisterParameter("hunk_chunk_reader", HunkChunkReader)
            .DefaultNew();
        RegisterParameter("store_chunk_writer", StoreChunkWriter)
            .Alias("chunk_writer")
            .DefaultNew();
        RegisterParameter("hunk_chunk_writer", HunkChunkWriter)
            .DefaultNew();
        RegisterParameter("tablet_balancer", TabletBalancer)
            .DefaultNew();
        RegisterParameter("tablet_cell_decommissioner", TabletCellDecommissioner)
            .DefaultNew();
        RegisterParameter("tablet_action_manager", TabletActionManager)
            .DefaultNew();
        RegisterParameter("multicell_gossip", MulticellGossip)
            // COMPAT(babenko)
            .Alias("multicell_gossip_config")
            .DefaultNew();
        RegisterParameter("tablet_cells_cleanup_period", TabletCellsCleanupPeriod)
            .Default(TDuration::Seconds(60));
        RegisterParameter("dynamic_table_profiling_mode", DynamicTableProfilingMode)
            .Default(NTabletNode::EDynamicTableProfilingMode::Path);
        RegisterParameter("tablet_cell_balancer", TabletCellBalancer)
            .DefaultNew();
        RegisterParameter("replicated_table_tracker", ReplicatedTableTracker)
            .DefaultNew();
        RegisterParameter("enable_bulk_insert", EnableBulkInsert)
            .Default(false);
        RegisterParameter("decommission_through_extra_peers", DecommissionThroughExtraPeers)
            .Default(false);
        RegisterParameter("decommissioned_leader_reassignment_timeout", DecommissionedLeaderReassignmentTimeout)
            .Default();
        RegisterParameter("abandon_leader_lease_during_recovery", AbandonLeaderLeaseDuringRecovery)
            .Default(false);
        RegisterParameter("enable_dynamic_store_read_by_default", EnableDynamicStoreReadByDefault)
            .Default(false);
        RegisterParameter("peer_revocation_reason_expiration_time", PeerRevocationReasonExpirationTime)
            .Default(TDuration::Minutes(15));
        RegisterParameter("enable_relaxed_tablet_statistics_validation", EnableRelaxedTabletStatisticsValidation)
            .Default(false);
        RegisterParameter("enable_aggressive_tablet_statistics_validation", EnableAggressiveTabletStatisticsValidation)
            .Default(false);
        RegisterParameter("extra_peer_drop_delay", ExtraPeerDropDelay)
            .Default(TDuration::Minutes(1));
        RegisterParameter("accumulate_preload_pending_store_count_correctly", AccumulatePreloadPendingStoreCountCorrectly)
            .Default(false)
            .DontSerializeDefault();
        RegisterParameter("increase_upload_replication_factor", IncreaseUploadReplicationFactor)
            .Default(false);
        RegisterParameter("enable_tablet_resource_validation", EnableTabletResourceValidation)
            .Default(false);

        RegisterParameter("tablet_node_tracker", TabletNodeTracker)
            .DefaultNew();

        RegisterParameter("enable_hunks", EnableHunks)
            .Default(false);

        RegisterPreprocessor([&] {
            StoreChunkReader->SuspiciousNodeGracePeriod = TDuration::Minutes(5);
            StoreChunkReader->BanPeersPermanently = false;

            StoreChunkWriter->BlockSize = 256_KB;
            StoreChunkWriter->SampleRate = 0.0005;
        });

        RegisterPostprocessor([&] {
            MaxSnapshotCountToKeep = 2;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicTabletManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
