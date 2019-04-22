#pragma once

#include "public.h"

#include <yt/core/misc/error.h>
#include <yt/core/misc/optional.h>

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/concurrency/config.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TInterDCLimitsConfig
    : public NYTree::TYsonSerializable
{
public:
    explicit TInterDCLimitsConfig()
    {
        RegisterParameter("default_capacity", DefaultCapacity_)
            .Default(std::numeric_limits<i64>::max())
            .GreaterThanOrEqual(0);

        RegisterParameter("capacities", Capacities_)
            .Default();

        RegisterPostprocessor([&] () {
            for (const auto& pair : Capacities_) {
                for (const auto& pair2 : pair.second) {
                    if (pair2.second < 0) {
                        THROW_ERROR_EXCEPTION(
                            "Negative capacity %v for inter-DC edge %v->%v",
                            pair2.second,
                            pair.first,
                            pair2.first);
                    }
                }
            }

            CpuUpdateInterval_ = NProfiling::DurationToCpuDuration(UpdateInterval_);
        });
    }

    THashMap<std::optional<TString>, THashMap<std::optional<TString>, i64>> GetCapacities() const
    {
        THashMap<std::optional<TString>, THashMap<std::optional<TString>, i64>> result;
        for (const auto& pair : Capacities_) {
            auto srcDataCenter = pair.first.empty() ? std::nullopt : std::make_optional(pair.first);
            auto& srcDataCenterCapacities = result[srcDataCenter];
            for (const auto& pair2 : pair.second) {
                auto dstDataCenter = pair2.first.empty() ? std::nullopt : std::make_optional(pair2.first);
                srcDataCenterCapacities.emplace(dstDataCenter, pair2.second);
            }
        }
        return result;
    }

    i64 GetDefaultCapacity() const
    {
        return DefaultCapacity_;
    }

    NProfiling::TCpuDuration GetUpdateInterval() const
    {
        return CpuUpdateInterval_;
    }

private:
    // src DC -> dst DC -> data size.
    // NB: that null DC is encoded as an empty string here.
    THashMap<TString, THashMap<TString, i64>> Capacities_;
    i64 DefaultCapacity_;
    TDuration UpdateInterval_;
    NProfiling::TCpuDuration CpuUpdateInterval_ = {};
};

DEFINE_REFCOUNTED_TYPE(TInterDCLimitsConfig)

////////////////////////////////////////////////////////////////////////////////

class TChunkManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! A default value for an additional bound for the global replication
    //! factor cap. The value is used when a new medium is created to initialize
    //! corresponding medium-specific setting.
    int MaxReplicationFactor;
    //! A default value for an additional bound for the number of replicas per
    //! rack for every chunk. The value is used when a new medium is created to
    //! initialize corresponding medium-specific setting.
    //! Currently used to simulate DC awareness.
    int MaxReplicasPerRack;
    //! Same as #MaxReplicasPerRack but only applies to regular chunks.
    int MaxRegularReplicasPerRack;
    //! Same as #MaxReplicasPerRack but only applies to journal chunks.
    int MaxJournalReplicasPerRack;
    //! Same as #MaxReplicasPerRack but only applies to erasure chunks.
    int MaxErasureReplicasPerRack;

    //! Enables storing more than one chunk part per node.
    //! Should only be used in local mode to enable writing erasure chunks in a cluster with just one node.
    bool AllowMultipleErasurePartsPerNode;

    //! Interval between consequent replicator state checks.
    TDuration ReplicatorEnabledCheckPeriod;

    //! When balancing chunk repair queues for multiple media, how often do
    //! their weights decay. (Weights are essentially repaired data sizes.)
    TDuration RepairQueueBalancerWeightDecayInterval;
    //! The number by which chunk repair queue weights are multiplied during decay.
    double RepairQueueBalancerWeightDecayFactor;

    TChunkManagerConfig()
    {
        RegisterParameter("max_replication_factor", MaxReplicationFactor)
            .GreaterThanOrEqual(NChunkClient::DefaultReplicationFactor)
            .Default(NChunkClient::MaxReplicationFactor);
        RegisterParameter("max_replicas_per_rack", MaxReplicasPerRack)
            .GreaterThan(0)
            .Default(std::numeric_limits<int>::max());
        RegisterParameter("max_regular_replicas_per_rack", MaxRegularReplicasPerRack)
            .GreaterThan(0)
            .Default(std::numeric_limits<int>::max());
        RegisterParameter("max_journal_replicas_per_rack", MaxJournalReplicasPerRack)
            .GreaterThan(0)
            .Default(std::numeric_limits<int>::max());
        RegisterParameter("max_erasure_replicas_per_rack", MaxErasureReplicasPerRack)
            .GreaterThan(0)
            .Default(std::numeric_limits<int>::max());

        RegisterParameter("allow_multiple_erasure_parts_per_node", AllowMultipleErasurePartsPerNode)
            .Default(false);

        RegisterParameter("replicator_enabled_check_period", ReplicatorEnabledCheckPeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("repair_queue_balancer_weight_decay_interval", RepairQueueBalancerWeightDecayInterval)
            .Default(TDuration::Seconds(60));
        RegisterParameter("repair_queue_balancer_weight_decay_factor", RepairQueueBalancerWeightDecayFactor)
            .Default(0.5);
    }
};

DEFINE_REFCOUNTED_TYPE(TChunkManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TMediumConfig
    : public NYTree::TYsonSerializable
{
public:
    //! An additional bound for the global replication factor cap.
    //! Useful when the number of racks is too low to interoperate meaningfully
    //! with the default cap.
    int MaxReplicationFactor;

    //! Provides an additional bound for the number of replicas per rack for every chunk.
    //! Currently used to simulate DC awareness.
    int MaxReplicasPerRack;

    //! Same as #MaxReplicasPerRack but only applies to regular chunks.
    int MaxRegularReplicasPerRack;

    //! Same as #MaxReplicasPerRack but only applies to journal chunks.
    int MaxJournalReplicasPerRack;

    //! Same as #MaxReplicasPerRack but only applies to erasure chunks.
    int MaxErasureReplicasPerRack;

    //! Default behavior for dynamic tables, living on this medium.
    bool PreferLocalHostForDynamicTables;

    TMediumConfig()
    {
        RegisterParameter("max_replication_factor", MaxReplicationFactor)
            .GreaterThanOrEqual(NChunkClient::DefaultReplicationFactor)
            .Default(NChunkClient::MaxReplicationFactor);
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
        RegisterParameter("prefer_local_host_for_dynamic_tables", PreferLocalHostForDynamicTables)
            .Default(true);
    }
};

DEFINE_REFCOUNTED_TYPE(TMediumConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicChunkManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! If set to false, disables scheduling new chunk jobs (replication, removal).
    bool EnableChunkReplicator;

    //! If set to false, disables scheduling new chunk seal jobs.
    bool EnableChunkSealer;

    TDuration ReplicaApproveTimeout;

    //! Controls the maximum number of unsuccessful attempts to schedule a replication job.
    int MaxMisscheduledReplicationJobsPerHeartbeat;
    //! Controls the maximum number of unsuccessful attempts to schedule a repair job.
    int MaxMisscheduledRepairJobsPerHeartbeat;
    //! Controls the maximum number of unsuccessful attempts to schedule a removal job.
    int MaxMisscheduledRemovalJobsPerHeartbeat;
    //! Controls the maximum number of unsuccessful attempts to schedule a seal job.
    int MaxMisscheduledSealJobsPerHeartbeat;

    //! Minimum difference in fill coefficient (between the most and the least loaded nodes) to start balancing.
    double MinChunkBalancingFillFactorDiff;
    //! Minimum fill coefficient of the most loaded node to start balancing.
    double MinChunkBalancingFillFactor;

    //! If set to false, fully disables background chunk refresh.
    //! Only use during bulk node restarts to save leaders' CPU.
    //! Don't forget to turn it on afterwards.
    bool EnableChunkRefresh;
    //! Graceful delay before chunk refresh.
    TDuration ChunkRefreshDelay;
    //! Interval between consequent chunk refresh iterations.
    std::optional<TDuration> ChunkRefreshPeriod;
    //! Maximum number of chunks to process during a refresh iteration.
    int MaxChunksPerRefresh;
    //! Maximum amount of time allowed to spend during a refresh iteration.
    TDuration MaxTimePerRefresh;

    //! If set to false, fully disables background chunk requisition updates;
    //! see #EnableChunkRefresh for a rationale.
    bool EnableChunkRequisitionUpdate;
    //! Interval between consequent chunk requisition update iterations.
    std::optional<TDuration> ChunkRequisitionUpdatePeriod;
    //! Maximum number of chunks to process during a requisition update iteration.
    int MaxChunksPerRequisitionUpdate;
    //! Maximum amount of time allowed to spend during a requisition update iteration.
    TDuration MaxTimePerRequisitionUpdate;
    //! Chunk requisition update finish mutations are batched within this period.
    TDuration FinishedChunkListsRequisitionTraverseFlushPeriod;

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

    //! Maximum duration a job can run before it is considered dead.
    TDuration JobTimeout;

    //! When the number of online nodes drops below this margin,
    //! replicator gets disabled.
    int SafeOnlineNodeCount;
    //! When the fraction of lost chunks grows above this margin,
    //! replicator gets disabled.
    double SafeLostChunkFraction;
    //! When the number of lost chunks grows above this margin,
    //! replicator gets disabled.
    int SafeLostChunkCount;

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

    //! Throttles chunk jobs.
    NConcurrency::TThroughputThrottlerConfigPtr JobThrottler;

    //! Limits data size to be replicated/repaired along an inter-DC edge at any given moment.
    TInterDCLimitsConfigPtr InterDCLimits;

    TDynamicChunkManagerConfig()
    {
        RegisterParameter("enable_chunk_replicator", EnableChunkReplicator)
            .Default(true);

        RegisterParameter("enable_chunk_sealer", EnableChunkSealer)
            .Default(true);

        RegisterParameter("replica_approve_timeout", ReplicaApproveTimeout)
            .Default(TDuration::Seconds(60));

        RegisterParameter("max_misscheduled_replication_jobs_per_heartbeat", MaxMisscheduledReplicationJobsPerHeartbeat)
            .Default(128);
        RegisterParameter("max_misscheduled_repair_jobs_per_heartbeat", MaxMisscheduledRepairJobsPerHeartbeat)
            .Default(128);
        RegisterParameter("max_misscheduled_removal_jobs_per_heartbeat", MaxMisscheduledRemovalJobsPerHeartbeat)
            .Default(128);
        RegisterParameter("max_misscheduled_seal_jobs_per_heartbeat", MaxMisscheduledSealJobsPerHeartbeat)
            .Default(128);

        RegisterParameter("min_chunk_balancing_fill_factor_diff", MinChunkBalancingFillFactorDiff)
            .InRange(0.0, 1.0)
            .Default(1.0);
        RegisterParameter("min_chunk_balancing_fill_factor", MinChunkBalancingFillFactor)
            .InRange(0.0, 1.0)
            .Default(0.1);

        RegisterParameter("enable_chunk_refresh", EnableChunkRefresh)
            .Default(true);
        RegisterParameter("chunk_refresh_delay", ChunkRefreshDelay)
            .Default(TDuration::Seconds(30));
        RegisterParameter("chunk_refresh_period", ChunkRefreshPeriod)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("max_chunks_per_refresh", MaxChunksPerRefresh)
            .Default(10000);
        RegisterParameter("max_time_per_refresh", MaxTimePerRefresh)
            .Default(TDuration::MilliSeconds(100));

        RegisterParameter("enable_chunk_requisition_update", EnableChunkRequisitionUpdate)
            .Default(true);
        RegisterParameter("chunk_requisition_update_period", ChunkRequisitionUpdatePeriod)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("max_chunks_per_requisition_update", MaxChunksPerRequisitionUpdate)
            .Default(10000);
        RegisterParameter("max_time_per_requisition_update", MaxTimePerRequisitionUpdate)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("finished_chunk_lists_requisition_traverse_flush_period", FinishedChunkListsRequisitionTraverseFlushPeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("chunk_seal_backoff_time", ChunkSealBackoffTime)
            .Default(TDuration::Seconds(30));
        RegisterParameter("journal_rpc_timeout", JournalRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("max_chunks_per_seal", MaxChunksPerSeal)
            .GreaterThan(0)
            .Default(10000);
        RegisterParameter("max_concurrent_chunk_seals", MaxConcurrentChunkSeals)
            .GreaterThan(0)
            .Default(10);

        RegisterParameter("max_chunks_per_fetch", MaxChunksPerFetch)
            .GreaterThan(0)
            .Default(1000000);
        RegisterParameter("max_cached_replicas_per_fetch", MaxCachedReplicasPerFetch)
            .GreaterThan(0)
            .Default(20);

        RegisterParameter("job_timeout", JobTimeout)
            .Default(TDuration::Minutes(5));

        RegisterParameter("safe_online_node_count", SafeOnlineNodeCount)
            .GreaterThanOrEqual(0)
            .Default(0);
        RegisterParameter("safe_lost_chunk_fraction", SafeLostChunkFraction)
            .InRange(0.0, 1.0)
            .Default(0.5);
        RegisterParameter("safe_lost_chunk_count", SafeLostChunkCount)
            .GreaterThan(0)
            .Default(1000);

        RegisterParameter("max_replication_write_sessions", MaxReplicationWriteSessions)
            .GreaterThanOrEqual(1)
            .Default(128);

        RegisterParameter("repair_job_memory_usage", RepairJobMemoryUsage)
            .Default(256_MB)
            .GreaterThanOrEqual(0);

        RegisterParameter("job_throttler", JobThrottler)
            .DefaultNew();

        RegisterParameter("inter_dc_limits", InterDCLimits)
            .DefaultNew();

        RegisterPreprocessor([&] () {
            JobThrottler->Limit = 10000;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicChunkManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
