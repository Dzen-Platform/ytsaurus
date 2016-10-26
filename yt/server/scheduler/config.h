#pragma once

#include "private.h"

#include <yt/server/job_proxy/config.h>

#include <yt/ytlib/api/config.h>

#include <yt/ytlib/chunk_client/config.h>

#include <yt/ytlib/table_client/config.h>

#include <yt/ytlib/ypath/public.h>

#include <yt/core/concurrency/config.h>

#include <yt/core/rpc/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TFairShareStrategyConfig
    : virtual public NYTree::TYsonSerializable
{
public:
    // The following settings can be overridden in operation spec.
    TDuration MinSharePreemptionTimeout;
    TDuration FairSharePreemptionTimeout;
    double FairShareStarvationTolerance;

    TDuration MinSharePreemptionTimeoutLimit;
    TDuration FairSharePreemptionTimeoutLimit;
    double FairShareStarvationToleranceLimit;

    TDuration FairShareUpdatePeriod;
    TDuration FairShareProfilingPeriod;
    TDuration FairShareLogPeriod;

    //! Any operation with usage less than this cannot be preempted.
    double MinPreemptableRatio;

    //! Limit on number of operations in cluster.
    int MaxRunningOperationCount;
    int MaxOperationCount;

    //! Limit on number of operations in pool.
    int MaxOperationCountPerPool;
    int MaxRunningOperationCountPerPool;

    //! If enabled, pools will be able to starve and provoke preemption.
    bool EnablePoolStarvation;

    //! Default parent pool for operations with unknown pool.
    Stroka DefaultParentPool;

    // Preemption timeout for operations with small number of jobs will be
    // discounted proportionally to this coefficient.
    double JobCountPreemptionTimeoutCoefficient;

    //! Limit on number of concurrent calls to ScheduleJob of single controller.
    int MaxConcurrentControllerScheduleJobCalls;

    //! Maximum allowed time for single job scheduling.
    TDuration ControllerScheduleJobTimeLimit;

    //! Backoff time after controller schedule job failure.
    TDuration ControllerScheduleJobFailBackoffTime;

    //! Thresholds to partition jobs of operation
    //! to preemptable, aggressively preemptable and non-preemptable lists.
    double PreemptionSatisfactionThreshold;
    double AggressivePreemptionSatisfactionThreshold;

    TFairShareStrategyConfig()
    {
        RegisterParameter("min_share_preemption_timeout", MinSharePreemptionTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("fair_share_preemption_timeout", FairSharePreemptionTimeout)
            .Default(TDuration::Seconds(30));
        RegisterParameter("fair_share_starvation_tolerance", FairShareStarvationTolerance)
            .InRange(0.0, 1.0)
            .Default(0.8);

        RegisterParameter("min_share_preemption_timeout_limit", MinSharePreemptionTimeoutLimit)
            .Default(TDuration::Seconds(15));
        RegisterParameter("fair_share_preemption_timeout_limit", FairSharePreemptionTimeoutLimit)
            .Default(TDuration::Seconds(30));
        RegisterParameter("fair_share_starvation_tolerance_limit", FairShareStarvationToleranceLimit)
            .InRange(0.0, 1.0)
            .Default(0.8);

        RegisterParameter("fair_share_update_period", FairShareUpdatePeriod)
            .InRange(TDuration::MilliSeconds(10), TDuration::Seconds(60))
            .Default(TDuration::MilliSeconds(1000));

        RegisterParameter("fair_share_profiling_period", FairShareProfilingPeriod)
            .InRange(TDuration::MilliSeconds(10), TDuration::Seconds(60))
            .Default(TDuration::MilliSeconds(5000));

        RegisterParameter("fair_share_log_period", FairShareLogPeriod)
            .InRange(TDuration::MilliSeconds(10), TDuration::Seconds(60))
            .Default(TDuration::MilliSeconds(1000));

        RegisterParameter("min_preemptable_ratio", MinPreemptableRatio)
            .InRange(0.0, 1.0)
            .Default(0.05);

        RegisterParameter("max_running_operation_count", MaxRunningOperationCount)
            .Alias("max_running_operations")
            .Default(200)
            .GreaterThan(0);

        RegisterParameter("max_running_operation_count_per_pool", MaxRunningOperationCountPerPool)
            .Alias("max_running_operations_per_pool")
            .Default(50)
            .GreaterThan(0);

        RegisterParameter("max_operation_count_per_pool", MaxOperationCountPerPool)
            .Alias("max_operations_per_pool")
            .Default(50)
            .GreaterThan(0);

        RegisterParameter("max_operation_count", MaxOperationCount)
            .Default(1000)
            .GreaterThan(0);

        RegisterParameter("enable_pool_starvation", EnablePoolStarvation)
            .Default(true);

        RegisterParameter("default_parent_pool", DefaultParentPool)
            .Default(RootPoolName);

        RegisterParameter("job_count_preemption_timeout_coefficient", JobCountPreemptionTimeoutCoefficient)
            .Default(1.0)
            .GreaterThanOrEqual(1.0);

        RegisterParameter("max_concurrent_controller_schedule_job_calls", MaxConcurrentControllerScheduleJobCalls)
            .Default(10)
            .GreaterThan(0);

        RegisterParameter("schedule_job_time_limit", ControllerScheduleJobTimeLimit)
            .Default(TDuration::Seconds(60));

        RegisterParameter("schedule_job_fail_backoff_time", ControllerScheduleJobFailBackoffTime)
            .Default(TDuration::MilliSeconds(100));

        RegisterParameter("preemption_satisfaction_threshold", PreemptionSatisfactionThreshold)
            .Default(1.0)
            .GreaterThan(0);

        RegisterParameter("aggressive_preemption_satisfaction_threshold", AggressivePreemptionSatisfactionThreshold)
            .Default(0.5)
            .GreaterThan(0);

        RegisterValidator([&] () {
            if (AggressivePreemptionSatisfactionThreshold > PreemptionSatisfactionThreshold) {
                THROW_ERROR_EXCEPTION("Aggressive preemption satisfaction threshold must be less than preemption satisfaction threshold")
                    << TErrorAttribute("aggressive_threshold", AggressivePreemptionSatisfactionThreshold)
                    << TErrorAttribute("threshold", PreemptionSatisfactionThreshold);
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TFairShareStrategyConfig)

////////////////////////////////////////////////////////////////////////////////

class TEventLogConfig
    : public NTableClient::TBufferedTableWriterConfig
{
public:
    NYPath::TYPath Path;

    TEventLogConfig()
    {
        RegisterParameter("path", Path)
            .Default("//sys/scheduler/event_log");
    }
};

DEFINE_REFCOUNTED_TYPE(TEventLogConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobSizeManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration MinJobTime;
    double ExecToPrepareTimeRatio;

    TJobSizeManagerConfig()
    {
        RegisterParameter("min_job_time", MinJobTime)
            .Default(TDuration::Seconds(60));

        RegisterParameter("exec_to_prepare_time_ratio", ExecToPrepareTimeRatio)
            .Default(20.0);
    }
};

DEFINE_REFCOUNTED_TYPE(TJobSizeManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TOperationOptions
    : public NYTree::TYsonSerializable
{
public:
    NYTree::INodePtr SpecTemplate;

    TOperationOptions()
    {
        RegisterParameter("spec_template", SpecTemplate)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TOperationOptions)

class TSimpleOperationOptions
    : public TOperationOptions
{
public:
    int MaxJobCount;
    i64 JobMaxSliceDataSize;
    i64 DataSizePerJob;
    TJobSizeManagerConfigPtr JobSizeManager;

    TSimpleOperationOptions()
    {
        RegisterParameter("max_job_count", MaxJobCount)
            .Default(100000);

        RegisterParameter("job_max_slice_data_size", JobMaxSliceDataSize)
            .Default((i64)256 * 1024 * 1024)
            .GreaterThan(0);

        RegisterParameter("data_size_per_job", DataSizePerJob)
            .Default((i64) 256 * 1024 * 1024)
            .GreaterThan(0);

        RegisterParameter("job_size_manager", JobSizeManager)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TSimpleOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TMapOperationOptions
    : public TSimpleOperationOptions
{
public:
    TMapOperationOptions()
    {
        RegisterInitializer([&] () {
            DataSizePerJob = (i64) 128 * 1024 * 1024;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TMapOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TUnorderedMergeOperationOptions
    : public TSimpleOperationOptions
{ };

DEFINE_REFCOUNTED_TYPE(TUnorderedMergeOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TOrderedMergeOperationOptions
    : public TSimpleOperationOptions
{ };

DEFINE_REFCOUNTED_TYPE(TOrderedMergeOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TSortedMergeOperationOptions
    : public TSimpleOperationOptions
{ };

DEFINE_REFCOUNTED_TYPE(TSortedMergeOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TReduceOperationOptions
    : public TSortedMergeOperationOptions
{
public:
    TReduceOperationOptions()
    {
        RegisterInitializer([&] () {
            DataSizePerJob = (i64) 128 * 1024 * 1024;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TReduceOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TJoinReduceOperationOptions
    : public TReduceOperationOptions
{ };

DEFINE_REFCOUNTED_TYPE(TJoinReduceOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TEraseOperationOptions
    : public TOrderedMergeOperationOptions
{ };

DEFINE_REFCOUNTED_TYPE(TEraseOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TSortOperationOptionsBase
    : public TOperationOptions
{
public:
    int MaxPartitionJobCount;
    int MaxPartitionCount;
    i64 SortJobMaxSliceDataSize;
    i64 PartitionJobMaxSliceDataSize;
    i32 MaxSampleSize;
    i64 CompressedBlockSize;
    i64 MinPartitionSize;
    i64 MinUncompressedBlockSize;
    TJobSizeManagerConfigPtr PartitionJobSizeManager;

    TSortOperationOptionsBase()
    {
        RegisterParameter("max_partition_job_count", MaxPartitionJobCount)
            .Default(100000)
            .GreaterThan(0);

        RegisterParameter("max_partition_count", MaxPartitionCount)
            .Default(10000)
            .GreaterThan(0);

        RegisterParameter("partition_job_max_slice_data_size", PartitionJobMaxSliceDataSize)
            .Default((i64)256 * 1024 * 1024)
            .GreaterThan(0);

        RegisterParameter("sort_job_max_slice_data_size", SortJobMaxSliceDataSize)
            .Default((i64)256 * 1024 * 1024)
            .GreaterThan(0);

        RegisterParameter("max_sample_size", MaxSampleSize)
            .Default(10 * 1024)
            .GreaterThanOrEqual(1024)
            // NB(psushin): removing this validator may lead to weird errors in sorting.
            .LessThanOrEqual(NTableClient::MaxSampleSize);

        RegisterParameter("compressed_block_size", CompressedBlockSize)
            .Default(1 * 1024 * 1024)
            .GreaterThanOrEqual(1024);

        RegisterParameter("min_partition_size", MinPartitionSize)
            .Default(256 * 1024 * 1024)
            .GreaterThanOrEqual(1024);

        // Minimum is 1 for tests.
        RegisterParameter("min_uncompressed_block_size", MinUncompressedBlockSize)
            .Default(1024 * 1024)
            .GreaterThanOrEqual(1);

        RegisterParameter("partition_job_size_manager", PartitionJobSizeManager)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TSortOperationOptionsBase)

////////////////////////////////////////////////////////////////////////////////

class TSortOperationOptions
    : public TSortOperationOptionsBase
{ };

DEFINE_REFCOUNTED_TYPE(TSortOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TMapReduceOperationOptions
    : public TSortOperationOptionsBase
{ };

DEFINE_REFCOUNTED_TYPE(TMapReduceOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TRemoteCopyOperationOptions
    : public TOperationOptions
{
public:
    int MaxJobCount;
    i64 DataSizePerJob;

    TRemoteCopyOperationOptions()
    {
        RegisterParameter("max_job_count", MaxJobCount)
            .Default(100000)
            .GreaterThan(0);

        RegisterParameter("data_size_per_job", DataSizePerJob)
            .Default((i64) 1024 * 1024 * 1024)
            .GreaterThan(0);
    }
};

DEFINE_REFCOUNTED_TYPE(TRemoteCopyOperationOptions)

////////////////////////////////////////////////////////////////////////////////

class TSchedulerConfig
    : public TFairShareStrategyConfig
    , public NChunkClient::TChunkScraperConfig
    , public NChunkClient::TChunkTeleporterConfig
{
public:
    //! Number of threads for running controllers invokers.
    int ControllerThreadCount;

    //! Number of threads for retrieving important fields from job statistics.
    int StatisticsAnalyzerThreadCount;

    //! Number of threads for building job specs.
    int JobSpecBuilderThreadCount;

    //! Number of parallel operation snapshot builders.
    int ParallelSnapshotBuilderCount;

    //! Number of shards the nodes are split into.
    int NodeShardCount;

    TDuration ConnectRetryBackoffTime;

    //! Timeout for node expiration.
    TDuration NodeHeartbeatTimeout;

    TDuration TransactionsRefreshPeriod;

    TDuration OperationsUpdatePeriod;

    TDuration WatchersUpdatePeriod;

    TDuration ProfilingUpdatePeriod;

    TDuration AlertsUpdatePeriod;

    TDuration ClusterDirectoryUpdatePeriod;

    TDuration ResourceDemandSanityCheckPeriod;

    TDuration LockTransactionTimeout;

    TDuration OperationTransactionTimeout;

    TDuration JobProberRpcTimeout;

    TDuration ClusterInfoLoggingPeriod;

    TDuration PendingEventLogRowsFlushPeriod;

    TDuration UpdateExecNodeDescriptorsPeriod;

    TDuration OperationTimeLimitCheckPeriod;

    TDuration TaskUpdatePeriod;

    //! Jobs running on node are logged periodically or when they change their state.
    TDuration JobsLoggingPeriod;

    //! Statistics and resource usages of jobs running on a node are updated
    //! not more often then this period.
    TDuration RunningJobsUpdatePeriod;

    //! Missing jobs are checked not more often then this period.
    TDuration CheckMissingJobsPeriod;

    //! Maximum allowed running time of operation. Null value is interpreted as infinity.
    TNullable<TDuration> OperationTimeLimit;

    //! Maximum number of job nodes per operation.
    int MaxJobNodesPerOperation;

    //! Number of chunk lists to be allocated when an operation starts.
    int ChunkListPreallocationCount;

    //! Maximum number of chunk lists to request via a single request.
    int MaxChunkListAllocationCount;

    //! Better keep the number of spare chunk lists above this threshold.
    int ChunkListWatermarkCount;

    //! Each time the number of spare chunk lists drops below #ChunkListWatermarkCount or
    //! the controller requests more chunk lists than we currently have,
    //! another batch is allocated. Each time we allocate #ChunkListAllocationMultiplier times
    //! more chunk lists than previously.
    double ChunkListAllocationMultiplier;

    //! Minimum time between two consecutive chunk list release requests
    //! until number of chunk lists to release less that desired chunk lists to release.
    //! This option necessary to prevent chunk list release storm.
    TDuration ChunkListReleaseBatchDelay;

    //! Desired number of chunks to release in one batch.
    int DesiredChunkListsPerRelease;

    //! Maximum number of chunks per single fetch.
    int MaxChunksPerFetch;

    //! Maximum number of chunk stripes per job.
    int MaxChunkStripesPerJob;

    //! Maximum number of chunk trees to attach per request.
    int MaxChildrenPerAttachRequest;

    //! Controls finer initial slicing of input data to ensure even distribution of data split sizes among jobs.
    double SliceDataSizeMultiplier;

    //! Maximum size of file allowed to be passed to jobs.
    i64 MaxFileSize;

    //! Maximum number of output tables times job count an operation can have.
    int MaxOutputTablesTimesJobsCount;

    //! Maximum number of input tables an operation can have.
    int MaxInputTableCount;

    //! Maximum number of files per user job.
    int MaxUserFileCount;

    //! Maximum number of jobs to start within a single heartbeat.
    TNullable<int> MaxStartedJobsPerHeartbeat;

    //! Don't check resource demand for sanity if the number of online
    //! nodes is less than this bound.
    // TODO(ignat): rename to SafeExecNodeCount.
    int SafeOnlineNodeCount;

    //! Time between two consecutive calls in operation controller to get exec nodes information from scheduler.
    TDuration GetExecNodesInformationDelay;

    //! Maximum number of foreign chunks to locate per request.
    int MaxChunksPerLocateRequest;

    //! Patch for all operation options.
    NYT::NYTree::INodePtr OperationOptions;

    //! Specific operation options.
    TMapOperationOptionsPtr MapOperationOptions;
    TReduceOperationOptionsPtr ReduceOperationOptions;
    TJoinReduceOperationOptionsPtr JoinReduceOperationOptions;
    TEraseOperationOptionsPtr EraseOperationOptions;
    TOrderedMergeOperationOptionsPtr OrderedMergeOperationOptions;
    TUnorderedMergeOperationOptionsPtr UnorderedMergeOperationOptions;
    TSortedMergeOperationOptionsPtr SortedMergeOperationOptions;
    TMapReduceOperationOptionsPtr MapReduceOperationOptions;
    TSortOperationOptionsPtr SortOperationOptions;
    TRemoteCopyOperationOptionsPtr RemoteCopyOperationOptions;

    //! Default environment variables set for every job.
    yhash_map<Stroka, Stroka> Environment;

    //! Interval between consequent snapshots.
    TDuration SnapshotPeriod;

    //! Timeout for snapshot construction.
    TDuration SnapshotTimeout;

    //! If |true|, snapshots are periodically constructed and uploaded into the system.
    bool EnableSnapshotBuilding;

    //! If |true|, snapshots are loaded during revival.
    bool EnableSnapshotLoading;

    Stroka SnapshotTempPath;
    NApi::TFileReaderConfigPtr SnapshotReader;
    NApi::TFileWriterConfigPtr SnapshotWriter;

    NChunkClient::TFetcherConfigPtr Fetcher;

    TEventLogConfigPtr EventLog;

    //! Limits the rate (measured in chunks) of location requests issued by all active chunk scrapers.
    NConcurrency::TThroughputThrottlerConfigPtr ChunkLocationThrottler;

    TNullable<NYPath::TYPath> UdfRegistryPath;

    // Backoff for processing successive heartbeats.
    TDuration HeartbeatProcessBackoff;
    // Number of heartbeats that can be processed without applying backoff.
    int SoftConcurrentHeartbeatLimit;
    // Maximum number of simultaneously processed heartbeats.
    int HardConcurrentHeartbeatLimit;

    bool EnableTmpfs;
    // Enable dynamic change of job sizes.
    bool EnableJobSizeManager;

    double UserJobMemoryDigestPrecision;
    double UserJobMemoryReserveQuantile;
    double JobProxyMemoryReserveQuantile;

    // Duration of no activity by job to be considered as suspicious.
    TDuration SuspiciousInactivityTimeout;

    // Cpu usage delta that is considered insignificant when checking if job is suspicious.
    i64 SuspiciousCpuUsageThreshold;
    // Time fraction spent in idle state enough for job to be considered suspicious.
    double SuspiciousInputPipeIdleTimeFraction;

    // Testing option that enables snapshot build/load cycle after operation materialization.
    bool EnableSnapshotCycleAfterMaterialization;

    // Testing option that enables sleeping between intermediate and final states of operation.
    TNullable<TDuration> FinishOperationTransitionDelay;

    // Testing option that enables sleeping during master disconnect.
    TNullable<TDuration> MasterDisconnectDelay;

    // If user job iops threshold is exceeded, iops throttling is enabled via cgroups.
    TNullable<i32> IopsThreshold;
    TNullable<i32> IopsThrottlerLimit;

    TDuration StaticOrchidCacheUpdatePeriod;

    TSchedulerConfig()
    {
        RegisterParameter("controller_thread_count", ControllerThreadCount)
            .Default(4)
            .GreaterThan(0);
        RegisterParameter("statistics_analyzer_thread_count", StatisticsAnalyzerThreadCount)
            .Default(2)
            .GreaterThan(0);
        RegisterParameter("job_spec_builder_thread_count", JobSpecBuilderThreadCount)
            .Default(8)
            .GreaterThan(0);
        RegisterParameter("parallel_snapshot_builder_count", ParallelSnapshotBuilderCount)
            .Default(4)
            .GreaterThan(0);
        RegisterParameter("node_shard_count", NodeShardCount)
            .Default(4)
            .GreaterThan(0);

        RegisterParameter("connect_retry_backoff_time", ConnectRetryBackoffTime)
            .Default(TDuration::Seconds(15));
        RegisterParameter("node_heartbeat_timeout", NodeHeartbeatTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("transactions_refresh_period", TransactionsRefreshPeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("operations_update_period", OperationsUpdatePeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("watchers_update_period", WatchersUpdatePeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("profiling_update_period", ProfilingUpdatePeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("alerts_update_period", AlertsUpdatePeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("cluster_directory_update_period", ClusterDirectoryUpdatePeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("resource_demand_sanity_check_period", ResourceDemandSanityCheckPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("lock_transaction_timeout", LockTransactionTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("operation_transaction_timeout", OperationTransactionTimeout)
            .Default(TDuration::Minutes(60));
        RegisterParameter("job_prober_rpc_timeout", JobProberRpcTimeout)
            .Default(TDuration::Seconds(300));

        RegisterParameter("task_update_period", TaskUpdatePeriod)
            .Default(TDuration::Seconds(3));

        RegisterParameter("cluster_info_logging_period", ClusterInfoLoggingPeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("pending_event_log_rows_flush_period", PendingEventLogRowsFlushPeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("update_exec_node_descriptors_period", UpdateExecNodeDescriptorsPeriod)
            .Default(TDuration::Seconds(1));


        RegisterParameter("operation_time_limit_check_period", OperationTimeLimitCheckPeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("jobs_logging_period", JobsLoggingPeriod)
            .Default(TDuration::Seconds(30));

        RegisterParameter("running_jobs_update_period", RunningJobsUpdatePeriod)
            .Default(TDuration::Seconds(10));

        RegisterParameter("check_missing_jobs_period", CheckMissingJobsPeriod)
            .Default(TDuration::Seconds(10));

        RegisterParameter("operation_time_limit", OperationTimeLimit)
            .Default();

        RegisterParameter("max_job_nodes_per_operation", MaxJobNodesPerOperation)
            .Default(200)
            .GreaterThanOrEqual(0)
            .LessThanOrEqual(200);

        RegisterParameter("chunk_list_preallocation_count", ChunkListPreallocationCount)
            .Default(128)
            .GreaterThanOrEqual(0);
        RegisterParameter("max_chunk_list_allocation_count", MaxChunkListAllocationCount)
            .Default(16384)
            .GreaterThanOrEqual(0);
        RegisterParameter("chunk_list_watermark_count", ChunkListWatermarkCount)
            .Default(50)
            .GreaterThanOrEqual(0);
        RegisterParameter("chunk_list_allocation_multiplier", ChunkListAllocationMultiplier)
            .Default(2.0)
            .GreaterThan(1.0);
        RegisterParameter("chunk_list_release_batch_delay", ChunkListReleaseBatchDelay)
            .Default(TDuration::Seconds(30));
        RegisterParameter("desired_chunk_lists_per_release", DesiredChunkListsPerRelease)
            .Default(1000);

        RegisterParameter("max_chunks_per_fetch", MaxChunksPerFetch)
            .Default(100000)
            .GreaterThan(0);

        RegisterParameter("max_chunk_stripes_per_job", MaxChunkStripesPerJob)
            .Default(50000)
            .GreaterThan(0);

        RegisterParameter("max_children_per_attach_request", MaxChildrenPerAttachRequest)
            .Default(10000)
            .GreaterThan(0);

        RegisterParameter("slice_data_size_multiplier", SliceDataSizeMultiplier)
            .Default(0.51)
            .GreaterThan(0.0);

        RegisterParameter("max_file_size", MaxFileSize)
            .Default((i64) 10 * 1024 * 1024 * 1024);

        RegisterParameter("max_input_table_count", MaxInputTableCount)
            .Default(1000)
            .GreaterThan(0);

        RegisterParameter("max_user_file_count", MaxUserFileCount)
            .Default(1000)
            .GreaterThan(0);

        RegisterParameter("max_output_tables_times_jobs_count", MaxOutputTablesTimesJobsCount)
            .Default(20 * 100000)
            .GreaterThanOrEqual(100000);

        RegisterParameter("max_started_jobs_per_heartbeat", MaxStartedJobsPerHeartbeat)
            .Default()
            .GreaterThan(0);

        RegisterParameter("safe_online_node_count", SafeOnlineNodeCount)
            .GreaterThanOrEqual(0)
            .Default(1);

        RegisterParameter("get_exec_nodes_information_delay", GetExecNodesInformationDelay)
            .Default(TDuration::Seconds(1));

        RegisterParameter("max_chunks_per_locate_request", MaxChunksPerLocateRequest)
            .GreaterThan(0)
            .Default(10000);

        RegisterParameter("operation_options", OperationOptions)
            .Default();
        RegisterParameter("map_operation_options", MapOperationOptions)
            .DefaultNew();
        RegisterParameter("reduce_operation_options", ReduceOperationOptions)
            .DefaultNew();
        RegisterParameter("join_reduce_operation_options", JoinReduceOperationOptions)
            .DefaultNew();
        RegisterParameter("erase_operation_options", EraseOperationOptions)
            .DefaultNew();
        RegisterParameter("ordered_merge_operation_options", OrderedMergeOperationOptions)
            .DefaultNew();
        RegisterParameter("unordered_merge_operation_options", UnorderedMergeOperationOptions)
            .DefaultNew();
        RegisterParameter("sorted_merge_operation_options", SortedMergeOperationOptions)
            .DefaultNew();
        RegisterParameter("map_reduce_operation_options", MapReduceOperationOptions)
            .DefaultNew();
        RegisterParameter("sort_operation_options", SortOperationOptions)
            .DefaultNew();
        RegisterParameter("remote_copy_operation_options", RemoteCopyOperationOptions)
            .DefaultNew();

        RegisterParameter("environment", Environment)
            .Default(yhash_map<Stroka, Stroka>());

        RegisterParameter("snapshot_timeout", SnapshotTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("snapshot_period", SnapshotPeriod)
            .Default(TDuration::Seconds(300));
        RegisterParameter("enable_snapshot_building", EnableSnapshotBuilding)
            .Default(true);
        RegisterParameter("enable_snapshot_loading", EnableSnapshotLoading)
            .Default(false);
        RegisterParameter("snapshot_temp_path", SnapshotTempPath)
            .NonEmpty()
            .Default("/tmp/yt/scheduler/snapshots");
        RegisterParameter("snapshot_reader", SnapshotReader)
            .DefaultNew();
        RegisterParameter("snapshot_writer", SnapshotWriter)
            .DefaultNew();

        RegisterParameter("fetcher", Fetcher)
            .DefaultNew();
        RegisterParameter("event_log", EventLog)
            .DefaultNew();

        RegisterParameter("chunk_location_throttler", ChunkLocationThrottler)
            .DefaultNew();

        RegisterParameter("udf_registry_path", UdfRegistryPath)
            .Default(Null);

        RegisterParameter("heartbeat_process_backoff", HeartbeatProcessBackoff)
            .Default(TDuration::MilliSeconds(5000));

        RegisterParameter("soft_concurrent_heartbeat_limit", SoftConcurrentHeartbeatLimit)
            .Default(50)
            .GreaterThanOrEqual(1);

        RegisterParameter("hard_concurrent_heartbeat_limit", HardConcurrentHeartbeatLimit)
            .Default(100)
            .GreaterThanOrEqual(1);

        RegisterParameter("enable_tmpfs", EnableTmpfs)
            .Default(true);
        RegisterParameter("enable_job_size_manager", EnableJobSizeManager)
            .Default(true);

        RegisterParameter("user_job_memory_digest_precision", UserJobMemoryDigestPrecision)
            .Default(0.01)
            .GreaterThan(0);
        RegisterParameter("user_job_memory_reserve_quantile", UserJobMemoryReserveQuantile)
            .InRange(0.0, 1.0)
            .Default(0.95);
        RegisterParameter("job_proxy_memory_reserve_quantile", JobProxyMemoryReserveQuantile)
            .InRange(0.0, 1.0)
            .Default(0.95);

        RegisterParameter("suspicious_inactivity_timeout", SuspiciousInactivityTimeout)
            .Default(TDuration::Minutes(1));
        RegisterParameter("suspicious_cpu_usage_threshold", SuspiciousCpuUsageThreshold)
            .Default(300);
        RegisterParameter("suspicious_input_pipe_time_idle_fraction", SuspiciousInputPipeIdleTimeFraction)
            .Default(0.95);

        RegisterParameter("enable_snapshot_cycle_after_materialization", EnableSnapshotCycleAfterMaterialization)
            .Default(false);
        RegisterParameter("static_orchid_cache_update_period", StaticOrchidCacheUpdatePeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("finish_operation_transition_delay", FinishOperationTransitionDelay)
            .Default(Null);

        RegisterParameter("master_disconnect_delay", MasterDisconnectDelay)
            .Default(Null);

        RegisterParameter("iops_threshold", IopsThreshold)
            .Default(Null);
        RegisterParameter("iops_throttler_limit", IopsThrottlerLimit)
            .Default(Null);

        RegisterInitializer([&] () {
            ChunkLocationThrottler->Limit = 10000;

            EventLog->MaxRowWeight = (i64) 128 * 1024 * 1024;
        });

        RegisterValidator([&] () {
            if (SoftConcurrentHeartbeatLimit > HardConcurrentHeartbeatLimit) {
                THROW_ERROR_EXCEPTION("Soft limit on concurrent heartbeats must be less than or equal to hard limit on concurrent heartbeats")
                    << TErrorAttribute("soft_limit", SoftConcurrentHeartbeatLimit)
                    << TErrorAttribute("hard_limit", HardConcurrentHeartbeatLimit);
            }
        });
    }

    virtual void OnLoaded() override
    {
        UpdateOptions(&MapOperationOptions, OperationOptions);
        UpdateOptions(&ReduceOperationOptions, OperationOptions);
        UpdateOptions(&JoinReduceOperationOptions, OperationOptions);
        UpdateOptions(&EraseOperationOptions, OperationOptions);
        UpdateOptions(&OrderedMergeOperationOptions, OperationOptions);
        UpdateOptions(&UnorderedMergeOperationOptions, OperationOptions);
        UpdateOptions(&SortedMergeOperationOptions, OperationOptions);
        UpdateOptions(&MapReduceOperationOptions, OperationOptions);
        UpdateOptions(&SortOperationOptions, OperationOptions);
        UpdateOptions(&RemoteCopyOperationOptions, OperationOptions);
    }

private:
    template <class TOptions>
    void UpdateOptions(TOptions* options, NYT::NYTree::INodePtr patch)
    {
        using NYTree::INodePtr;
        using NYTree::ConvertTo;

        if (!patch) {
            return;
        }

        if (*options) {
            *options = ConvertTo<TOptions>(UpdateNode(patch, ConvertTo<INodePtr>(*options)));
        } else {
            *options = ConvertTo<TOptions>(patch);
        }
    }
};

DEFINE_REFCOUNTED_TYPE(TSchedulerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
