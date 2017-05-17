#pragma once

#include "public.h"

#include <yt/core/misc/error.h>
#include <yt/core/misc/nullable.h>

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/concurrency/config.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunkManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! When the number of online nodes drops below this margin,
    //! replicator gets disabled.
    int SafeOnlineNodeCount;

    //! When the fraction of lost chunks grows above this margin,
    //! replicator gets disabled.
    double SafeLostChunkFraction;

    //! When the number of lost chunks grows above this margin,
    //! replicator gets disabled.
    int SafeLostChunkCount;

    //! Minimum difference in fill coefficient (between the most and the least loaded nodes) to start balancing.
    double MinBalancingFillFactorDiff;

    //! Minimum fill coefficient of the most loaded node to start balancing.
    double MinBalancingFillFactor;

    //! Maximum duration a job can run before it is considered dead.
    TDuration JobTimeout;

    //! Maximum number of replication/balancing jobs writing to each target node.
    /*!
     *  This limit is approximate and is only maintained when scheduling balancing jobs.
     *  This makes sense since balancing jobs specifically target nodes with lowest fill factor
     *  and thus risk overloading them.
     *  Replication jobs distribute data evenly across the cluster and thus pose no threat.
     */
    int MaxReplicationWriteSessions;

    //! Memory usage assigned to every repair job.
    i64 RepairJobMemoryUsage;

    //! Graceful delay before chunk refresh.
    TDuration ChunkRefreshDelay;

    //! Interval between consequent chunk refresh iterations.
    TDuration ChunkRefreshPeriod;

    //! Maximum number of chunks to process during a refresh iteration.
    int MaxChunksPerRefresh;

    //! Maximum amount of time allowed to spend during a refresh iteration.
    TDuration MaxTimePerRefresh;

    //! Interval between consequent chunk properties update iterations.
    TDuration ChunkPropertiesUpdatePeriod;

    //! Maximum number of chunks to process during a properties update iteration.
    int MaxChunksPerPropertiesUpdate;

    //! Maximum amount of time allowed to spend during a properties update iteration.
    TDuration MaxTimePerPropertiesUpdate;

    //! Interval between consequent seal attempts.
    TDuration ChunkSealBackoffTime;

    //! Timeout for RPC requests to nodes during journal operations.
    TDuration JournalRpcTimeout;

    //! Maximum number of chunks to process during a seal scan.
    int MaxChunksPerSeal;

    //! Maximum number of chunks that can be sealed concurrently.
    int MaxConcurrentChunkSeals;

    //! Maximum number of chunks to report per single fetch request.
    int MaxChunksPerFetch;

    //! Maximum number of cached replicas to be returned on fetch request.
    int MaxCachedReplicasPerFetch;

    //! Provides an additional bound for the number of replicas per rack for every chunk.
    //! Currently used to simulate DC awareness.
    int MaxReplicasPerRack;

    //! Same as #MaxReplicasPerRack but only applies to regular chunks.
    int MaxRegularReplicasPerRack;

    //! Same as #MaxReplicasPerRack but only applies to journal chunks.
    int MaxJournalReplicasPerRack;

    //! Same as #MaxReplicasPerRack but only applies to erasure chunks.
    int MaxErasureReplicasPerRack;

    //! Interval between consequent replicator state checks.
    TDuration ReplicatorEnabledCheckPeriod;

    //! Throttles chunk jobs.
    NConcurrency::TThroughputThrottlerConfigPtr JobThrottler;

    //! Controls the maximum number of unsuccessful attempts to schedule a replication job.
    int MaxMisscheduledReplicationJobsPerHeartbeat;
    //! Controls the maximum number of unsuccessful attempts to schedule a repair job.
    int MaxMisscheduledRepairJobsPerHeartbeat;
    //! Controls the maximum number of unsuccessful attempts to schedule a removal job.
    int MaxMisscheduledRemovalJobsPerHeartbeat;
    //! Controls the maximum number of unsuccessful attempts to schedule a seal job.
    int MaxMisscheduledSealJobsPerHeartbeat;


    TChunkManagerConfig()
    {
        RegisterParameter("safe_online_node_count", SafeOnlineNodeCount)
            .GreaterThanOrEqual(0)
            .Default(0);
        RegisterParameter("safe_lost_chunk_fraction", SafeLostChunkFraction)
            .InRange(0.0, 1.0)
            .Default(0.5);
        RegisterParameter("safe_lost_chunk_count", SafeLostChunkCount)
            .GreaterThan(0)
            .Default(1000);

        RegisterParameter("min_chunk_balancing_fill_factor_diff", MinBalancingFillFactorDiff)
            .InRange(0.0, 1.0)
            .Default(0.2);
        RegisterParameter("min_chunk_balancing_fill_factor", MinBalancingFillFactor)
            .InRange(0.0, 1.0)
            .Default(0.1);

        RegisterParameter("job_timeout", JobTimeout)
            .Default(TDuration::Minutes(5));

        RegisterParameter("max_replication_write_sessions", MaxReplicationWriteSessions)
            .GreaterThanOrEqual(1)
            .Default(128);
        RegisterParameter("repair_job_memory_usage", RepairJobMemoryUsage)
            .Default((i64) 256 * 1024 * 1024)
            .GreaterThanOrEqual(0);

        RegisterParameter("chunk_refresh_delay", ChunkRefreshDelay)
            .Default(TDuration::Seconds(30));
        RegisterParameter("chunk_refresh_period", ChunkRefreshPeriod)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("max_chunks_per_refresh", MaxChunksPerRefresh)
            .Default(10000);
        RegisterParameter("max_time_per_refresh", MaxTimePerRefresh)
            .Default(TDuration::MilliSeconds(100));

        RegisterParameter("chunk_properties_update_period", ChunkPropertiesUpdatePeriod)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("max_chunks_per_properties_update", MaxChunksPerPropertiesUpdate)
            .Default(10000);
        RegisterParameter("max_time_per_properties_update", MaxTimePerPropertiesUpdate)
            .Default(TDuration::MilliSeconds(100));

        RegisterParameter("max_chunks_per_seal", MaxChunksPerSeal)
            .GreaterThan(0)
            .Default(10000);
        RegisterParameter("chunk_seal_backoff_time", ChunkSealBackoffTime)
            .Default(TDuration::Seconds(30));
        RegisterParameter("journal_rpc_timeout", JournalRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("max_concurrent_chunk_seals", MaxConcurrentChunkSeals)
            .GreaterThan(0)
            .Default(10);

        RegisterParameter("max_chunks_per_fetch", MaxChunksPerFetch)
            .GreaterThan(0)
            .Default(1000000);
        RegisterParameter("max_cached_replicas_per_fetch", MaxCachedReplicasPerFetch)
            .GreaterThanOrEqual(0)
            .Default(20);

        RegisterParameter("max_replicas_per_rack", MaxReplicasPerRack)
            .GreaterThanOrEqual(0)
            .Default(std::numeric_limits<int>::max());
        RegisterParameter("max_regular_replicas_per_rack", MaxRegularReplicasPerRack)
            .GreaterThanOrEqual(0)
            .Default(std::numeric_limits<int>::max());
        RegisterParameter("max_journal_replicas_per_rack", MaxJournalReplicasPerRack)
            .GreaterThanOrEqual(0)
            .Default(std::numeric_limits<int>::max());
        RegisterParameter("max_erasure_replicas_per_rack", MaxErasureReplicasPerRack)
            .GreaterThanOrEqual(0)
            .Default(std::numeric_limits<int>::max());

        RegisterParameter("replicator_enabled_check_period", ReplicatorEnabledCheckPeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("job_throttler", JobThrottler)
            .DefaultNew();

        RegisterParameter("max_misscheduled_replication_jobs_per_heartbeat", MaxMisscheduledReplicationJobsPerHeartbeat)
            .Default(128);
        RegisterParameter("max_misscheduled_repair_jobs_per_heartbeat", MaxMisscheduledRepairJobsPerHeartbeat)
            .Default(128);
        RegisterParameter("max_misscheduled_removal_jobs_per_heartbeat", MaxMisscheduledRemovalJobsPerHeartbeat)
            .Default(128);
        RegisterParameter("max_misscheduled_seal_jobs_per_heartbeat", MaxMisscheduledSealJobsPerHeartbeat)
            .Default(128);

        RegisterInitializer([&] () {
            JobThrottler->Limit = 10000;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TChunkManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
