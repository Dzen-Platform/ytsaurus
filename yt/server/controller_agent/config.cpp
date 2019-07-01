#include "config.h"

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

TJobSizeAdjusterConfig::TJobSizeAdjusterConfig()
{
    RegisterParameter("min_job_time", MinJobTime)
        .Default(TDuration::Seconds(60));

    RegisterParameter("max_job_time", MaxJobTime)
        .Default(TDuration::Minutes(10));

    RegisterParameter("exec_to_prepare_time_ratio", ExecToPrepareTimeRatio)
        .Default(20.0);
}

TIntermediateChunkScraperConfig::TIntermediateChunkScraperConfig()
{
    RegisterParameter("restart_timeout", RestartTimeout)
        .Default(TDuration::Seconds(10));
}

TTestingOptions::TTestingOptions()
{
    RegisterParameter("enable_snapshot_cycle_after_materialization", EnableSnapshotCycleAfterMaterialization)
        .Default(false);
}

TOperationAlertsConfig::TOperationAlertsConfig()
{
    RegisterParameter("tmpfs_alert_max_unused_space_ratio", TmpfsAlertMaxUnusedSpaceRatio)
        .InRange(0.0, 1.0)
        .Default(0.2);

    RegisterParameter("tmpfs_alert_min_unused_space_threshold", TmpfsAlertMinUnusedSpaceThreshold)
        .Default(512_MB)
        .GreaterThan(0);

    RegisterParameter("aborted_jobs_alert_max_aborted_time", AbortedJobsAlertMaxAbortedTime)
        .Default((i64) 10 * 60 * 1000)
        .GreaterThan(0);

    RegisterParameter("aborted_jobs_alert_max_aborted_time_ratio", AbortedJobsAlertMaxAbortedTimeRatio)
        .InRange(0.0, 1.0)
        .Default(0.25);

    RegisterParameter("short_jobs_alert_min_job_duration", ShortJobsAlertMinJobDuration)
        .Default(TDuration::Minutes(1));

    RegisterParameter("short_jobs_alert_min_job_count", ShortJobsAlertMinJobCount)
        .Default(1000);

    RegisterParameter("intermediate_data_skew_alert_min_partition_size", IntermediateDataSkewAlertMinPartitionSize)
        .Default(10_GB)
        .GreaterThan(0);

    RegisterParameter("intermediate_data_skew_alert_min_interquartile_range", IntermediateDataSkewAlertMinInterquartileRange)
        .Default(1_GB)
        .GreaterThan(0);

    RegisterParameter("job_spec_throttling_alert_activation_count_threshold", JobSpecThrottlingAlertActivationCountThreshold)
        .Default(1000)
        .GreaterThan(0);

    RegisterParameter("low_cpu_usage_alert_min_execution_time", LowCpuUsageAlertMinExecTime)
        .Default(TDuration::Minutes(10));

    RegisterParameter("low_cpu_usage_alert_min_average_job_time", LowCpuUsageAlertMinAverageJobTime)
        .Default(TDuration::Minutes(1));

    RegisterParameter("low_cpu_usage_alert_cpu_usage_threshold", LowCpuUsageAlertCpuUsageThreshold)
        .Default(0.5)
        .GreaterThan(0);

    RegisterParameter("operation_too_long_alert_min_wall_time", OperationTooLongAlertMinWallTime)
        .Default(TDuration::Minutes(5));

    RegisterParameter("operation_too_long_alert_estimate_duration_threshold", OperationTooLongAlertEstimateDurationThreshold)
        .Default(TDuration::Days(7));
}

TJobSplitterConfig::TJobSplitterConfig()
{
    RegisterParameter("min_job_time", MinJobTime)
        .Default(TDuration::Seconds(60));

    RegisterParameter("exec_to_prepare_time_ratio", ExecToPrepareTimeRatio)
        .Default(20.0);

    RegisterParameter("no_progress_job_total_to_prepare_time_ratio", NoProgressJobTotalToPrepareTimeRatio)
        .Default(20.0);

    RegisterParameter("min_total_data_weight", MinTotalDataWeight)
        .Alias("min_total_data_size")
        .Default(1_GB);

    RegisterParameter("update_period", UpdatePeriod)
        .Default(TDuration::Seconds(60));

    RegisterParameter("candidate_percentile", CandidatePercentile)
        .GreaterThanOrEqual(0.5)
        .LessThanOrEqual(1.0)
        .Default(0.8);

    RegisterParameter("late_jobs_percentile", LateJobsPercentile)
        .GreaterThanOrEqual(0.5)
        .LessThanOrEqual(1.0)
        .Default(0.95);

    RegisterParameter("residual_job_factor", ResidualJobFactor)
        .GreaterThan(0)
        .LessThanOrEqual(1.0)
        .Default(0.8);

    RegisterParameter("residual_job_count_min_threshold", ResidualJobCountMinThreshold)
        .GreaterThan(0)
        .Default(10);

    RegisterParameter("max_jobs_per_split", MaxJobsPerSplit)
        .GreaterThan(0)
        .Default(5);

    RegisterParameter("max_input_table_count", MaxInputTableCount)
        .GreaterThan(0)
        .Default(100);

    RegisterParameter("split_timeout_before_speculate", SplitTimeoutBeforeSpeculate)
        .Default(TDuration::Minutes(5));

    RegisterParameter("job_logging_period", JobLoggingPeriod)
        .Default(TDuration::Minutes(3));
}

TSuspiciousJobsOptions::TSuspiciousJobsOptions()
{
    RegisterParameter("inactivity_timeout", InactivityTimeout)
        .Default(TDuration::Minutes(1));
    RegisterParameter("cpu_usage_threshold", CpuUsageThreshold)
        .Default(300);
    RegisterParameter("input_pipe_time_idle_fraction", InputPipeIdleTimeFraction)
        .Default(0.95);
    RegisterParameter("output_pipe_time_idle_fraction", OutputPipeIdleTimeFraction)
        .Default(0.95);
    RegisterParameter("update_period", UpdatePeriod)
        .Default(TDuration::Seconds(5));
    RegisterParameter("max_orchid_entry_count_per_type", MaxOrchidEntryCountPerType)
        .Default(100);
}

TDataBalancerOptions::TDataBalancerOptions()
{
    RegisterParameter("logging_min_consecutive_violation_count", LoggingMinConsecutiveViolationCount)
        .Default(1000);
    RegisterParameter("logging_period", LoggingPeriod)
        .Default(TDuration::Minutes(1));
    RegisterParameter("tolerance", Tolerance)
        .Default(2.0);
}

TOperationOptions::TOperationOptions()
{
    RegisterParameter("spec_template", SpecTemplate)
        .Default()
        .MergeBy(NYTree::EMergeStrategy::Combine);

    RegisterParameter("slice_data_weight_multiplier", SliceDataWeightMultiplier)
        .Alias("slice_data_size_multiplier")
        .Default(0.51)
        .GreaterThan(0.0);

    RegisterParameter("max_data_slices_per_job", MaxDataSlicesPerJob)
        // This is a reasonable default for jobs with user code.
        // Defaults for system jobs are in Initializer.
        .Default(1000)
        .GreaterThan(0);

    RegisterParameter("max_slice_data_weight", MaxSliceDataWeight)
        .Alias("max_slice_data_size")
        .Default(1_GB)
        .GreaterThan(0);

    RegisterParameter("min_slice_data_weight", MinSliceDataWeight)
        .Alias("min_slice_data_size")
        .Default(1_MB)
        .GreaterThan(0);

    RegisterParameter("max_input_table_count", MaxInputTableCount)
        .Default(3000)
        .GreaterThan(0);

    RegisterParameter("max_output_tables_times_jobs_count", MaxOutputTablesTimesJobsCount)
        .Default(20 * 100000)
        .GreaterThanOrEqual(100000);

    RegisterParameter("job_splitter", JobSplitter)
        .DefaultNew();

    RegisterParameter("max_build_retry_count", MaxBuildRetryCount)
        .Default(5)
        .GreaterThanOrEqual(0);

    RegisterParameter("data_weight_per_job_retry_factor", DataWeightPerJobRetryFactor)
        .Default(2.0)
        .GreaterThan(1.0);

    RegisterPostprocessor([&] () {
        if (MaxSliceDataWeight < MinSliceDataWeight) {
            THROW_ERROR_EXCEPTION("Minimum slice data weight must be less than or equal to maximum slice data size")
                    << TErrorAttribute("min_slice_data_weight", MinSliceDataWeight)
                    << TErrorAttribute("max_slice_data_weight", MaxSliceDataWeight);
        }
    });
}

TSimpleOperationOptions::TSimpleOperationOptions()
{
    RegisterParameter("max_job_count", MaxJobCount)
        .Default(100000);

    RegisterParameter("data_weight_per_job", DataWeightPerJob)
        .Alias("data_size_per_job")
        .Default(256_MB)
        .GreaterThan(0);
}

TMapOperationOptions::TMapOperationOptions()
{
    RegisterParameter("job_size_adjuster", JobSizeAdjuster)
        .DefaultNew();

    RegisterPreprocessor([&] () {
        DataWeightPerJob = 128_MB;
    });
}

TReduceOperationOptions::TReduceOperationOptions()
{
    RegisterPreprocessor([&] () {
        DataWeightPerJob = 128_MB;
    });
}

TSortOperationOptionsBase::TSortOperationOptionsBase()
{
    RegisterParameter("max_partition_job_count", MaxPartitionJobCount)
        .Default(100000)
        .GreaterThan(0);

    RegisterParameter("max_partition_count", MaxPartitionCount)
        .Default(10000)
        .GreaterThan(0);

    RegisterParameter("max_sample_size", MaxSampleSize)
        .Default(10_KB)
        .GreaterThanOrEqual(1_KB)
            // NB(psushin): removing this validator may lead to weird errors in sorting.
        .LessThanOrEqual(NTableClient::MaxSampleSize);

    RegisterParameter("compressed_block_size", CompressedBlockSize)
        .Default(1_MB)
        .GreaterThanOrEqual(1_KB);

    RegisterParameter("min_partition_weight", MinPartitionWeight)
        .Alias("min_partition_size")
        .Default(256_MB)
        .GreaterThanOrEqual(1);

    // Minimum is 1 for tests.
    RegisterParameter("min_uncompressed_block_size", MinUncompressedBlockSize)
        .Default(100_KB)
        .GreaterThanOrEqual(1);

    RegisterParameter("partition_job_size_adjuster", PartitionJobSizeAdjuster)
        .DefaultNew();

    RegisterParameter("data_balancer", DataBalancer)
        .DefaultNew();
}

TControllerAgentConfig::TControllerAgentConfig()
{
    SetUnrecognizedStrategy(NYTree::EUnrecognizedStrategy::KeepRecursive);

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
    RegisterParameter("desired_chunk_lists_per_release", DesiredChunkListsPerRelease)
        .Default(10 * 1000);

    RegisterParameter("enable_snapshot_building", EnableSnapshotBuilding)
        .Default(true);
    RegisterParameter("snapshot_period", SnapshotPeriod)
        .Default(TDuration::Seconds(300));
    RegisterParameter("snapshot_timeout", SnapshotTimeout)
        .Default(TDuration::Seconds(60));
    RegisterParameter("operation_controller_suspend_timeout", OperationControllerSuspendTimeout)
        .Default(TDuration::Seconds(5));
    RegisterParameter("parallel_snapshot_builder_count", ParallelSnapshotBuilderCount)
        .Default(4)
        .GreaterThan(0);
    RegisterParameter("snapshot_writer", SnapshotWriter)
        .DefaultNew();

    RegisterParameter("enable_snapshot_loading", EnableSnapshotLoading)
        .Default(false);
    RegisterParameter("snapshot_reader", SnapshotReader)
        .DefaultNew();

    RegisterParameter("transactions_refresh_period", TransactionsRefreshPeriod)
        .Default(TDuration::Seconds(3));
    RegisterParameter("operations_update_period", OperationsUpdatePeriod)
        .Default(TDuration::Seconds(3));
    RegisterParameter("chunk_unstage_period", ChunkUnstagePeriod)
        .Default(TDuration::MilliSeconds(100));

    RegisterParameter("enable_unrecognized_alert", EnableUnrecognizedAlert)
        .Default(true);

    RegisterParameter("max_children_per_attach_request", MaxChildrenPerAttachRequest)
        .Default(10000)
        .GreaterThan(0);

    RegisterParameter("chunk_location_throttler", ChunkLocationThrottler)
        .DefaultNew();

    RegisterParameter("event_log", EventLog)
        .DefaultNew();

    RegisterParameter("scheduler_handshake_rpc_timeout", SchedulerHandshakeRpcTimeout)
        .Default(TDuration::Seconds(10));
    RegisterParameter("scheduler_failure_backoff", SchedulerHandshakeFailureBackoff)
        .Default(TDuration::Seconds(1));

    RegisterParameter("scheduler_heartbeat_rpc_timeout", SchedulerHeartbeatRpcTimeout)
        .Default(TDuration::Seconds(10));
    RegisterParameter("scheduler_failure_backoff", SchedulerHeartbeatFailureBackoff)
        .Default(TDuration::MilliSeconds(100));
    RegisterParameter("scheduler_heartbeat_period", SchedulerHeartbeatPeriod)
        .Default(TDuration::MilliSeconds(100));

    RegisterParameter("config_update_period", ConfigUpdatePeriod)
        .Default(TDuration::Seconds(10));

    RegisterParameter("exec_nodes_update_period", ExecNodesUpdatePeriod)
        .Default(TDuration::Seconds(10));
    RegisterParameter("operations_push_period", OperationsPushPeriod)
        .Default(TDuration::Seconds(1));
    RegisterParameter("operation_alerts_push_period", OperationAlertsPushPeriod)
        .Default(TDuration::Seconds(3));
    RegisterParameter("suspicious_jobs_push_period", SuspiciousJobsPushPeriod)
        .Default(TDuration::Seconds(3));

    RegisterParameter("controller_thread_count", ControllerThreadCount)
        .Default(16)
        .GreaterThan(0);

    RegisterParameter("controller_static_orchid_update_period", ControllerStaticOrchidUpdatePeriod)
        .Default(TDuration::Seconds(10));

    RegisterParameter("max_concurrent_safe_core_dumps", MaxConcurrentSafeCoreDumps)
        .Default(1)
        .GreaterThanOrEqual(0);

    RegisterParameter("scheduling_tag_filter_expire_timeout", SchedulingTagFilterExpireTimeout)
        .Default(TDuration::Seconds(10));

    RegisterParameter("operation_time_limit", OperationTimeLimit)
        .Default();
    RegisterParameter("operation_time_limit_check_period", OperationTimeLimitCheckPeriod)
        .Default(TDuration::Seconds(1));

    RegisterParameter("resource_demand_sanity_check_period", ResourceDemandSanityCheckPeriod)
        .Default(TDuration::Seconds(15));

    RegisterParameter("operation_initialization_timeout", OperationInitializationTimeout)
        .Default(TDuration::Minutes(10));
    RegisterParameter("operation_transaction_timeout", OperationTransactionTimeout)
        .Default(TDuration::Minutes(300));
    RegisterParameter("operation_transaction_ping_period", OperationTransactionPingPeriod)
        .Default(TDuration::Seconds(30));

    RegisterParameter("operation_progress_log_backoff", OperationLogProgressBackoff)
        .Default(TDuration::Seconds(1));

    RegisterParameter("task_update_period", TaskUpdatePeriod)
        .Default(TDuration::Seconds(3));

    RegisterParameter("operation_controller_fail_timeout", OperationControllerFailTimeout)
        .Default(TDuration::Seconds(120));

    RegisterParameter("available_exec_nodes_check_period", AvailableExecNodesCheckPeriod)
        .Default(TDuration::Seconds(5));

    RegisterParameter("banned_exec_nodes_check_period", BannedExecNodesCheckPeriod)
        .Default(TDuration::Minutes(5));

    RegisterParameter("operation_progress_analysis_period", OperationProgressAnalysisPeriod)
        .Default(TDuration::Seconds(10));

    RegisterParameter("operation_build_progress_period", OperationBuildProgressPeriod)
        .Default(TDuration::Seconds(3));

    RegisterParameter("check_tentative_tree_eligibility_period", CheckTentativeTreeEligibilityPeriod)
        .Default(TDuration::Seconds(10));

    RegisterParameter("max_available_exec_node_resources_update_period", MaxAvailableExecNodeResourcesUpdatePeriod)
        .Default(TDuration::Seconds(10));

    RegisterParameter("max_job_nodes_per_operation", MaxJobNodesPerOperation)
        .Default(200)
        .GreaterThanOrEqual(0)
        .LessThanOrEqual(250);

    RegisterParameter("max_archived_job_spec_count_per_operation", MaxArchivedJobSpecCountPerOperation)
        .Default(500)
        .GreaterThanOrEqual(0)
        .LessThanOrEqual(5000);

    RegisterParameter("guaranteed_archived_job_spec_count_per_operation", GuaranteedArchivedJobSpecCountPerOperation)
        .Default(10)
        .GreaterThanOrEqual(0)
        .LessThanOrEqual(100);

    RegisterParameter("min_job_duration_to_archive_job_spec", MinJobDurationToArchiveJobSpec)
        .Default(TDuration::Minutes(30))
        .GreaterThanOrEqual(TDuration::Minutes(5));

    RegisterParameter("max_chunks_per_fetch", MaxChunksPerFetch)
        .Default(100000)
        .GreaterThan(0);

    RegisterParameter("max_user_file_count", MaxUserFileCount)
        .Default(1000)
        .GreaterThan(0);
    RegisterParameter("max_user_file_size", MaxUserFileSize)
        .Alias("max_file_size")
        .Default(10_GB);
    RegisterParameter("max_user_file_table_data_weight", MaxUserFileTableDataWeight)
        .Default(10_GB);
    RegisterParameter("max_user_file_chunk_count", MaxUserFileChunkCount)
        .Default(1000);

    RegisterParameter("max_input_table_count", MaxInputTableCount)
        .Default(1000)
        .GreaterThan(0);

    RegisterParameter("max_ranges_on_table", MaxRangesOnTable)
        .Default(1000)
        .GreaterThan(0);

    RegisterParameter("safe_online_node_count", SafeOnlineNodeCount)
        .GreaterThanOrEqual(0)
        .Default(1);

    RegisterParameter("safe_scheduler_online_time", SafeSchedulerOnlineTime)
        .Default(TDuration::Minutes(10));

    RegisterParameter("controller_exec_node_info_update_period", ControllerExecNodeInfoUpdatePeriod)
        .Default(TDuration::Seconds(30));

    RegisterParameter("max_chunks_per_locate_request", MaxChunksPerLocateRequest)
        .GreaterThan(0)
        .Default(10000);

    RegisterParameter("operation_options", OperationOptions)
        .Default()
        .MergeBy(NYTree::EMergeStrategy::Combine);

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
    RegisterParameter("vanilla_operation_options", VanillaOperationOptions)
        .DefaultNew();

    RegisterParameter("environment", Environment)
        .Default(THashMap<TString, TString>())
        .MergeBy(NYTree::EMergeStrategy::Combine);

    RegisterParameter("enable_controller_failure_spec_option", EnableControllerFailureSpecOption)
        .Default(false);

    RegisterParameter("enable_job_revival", EnableJobRevival)
        .Default(true);

    RegisterParameter("enable_locality", EnableLocality)
        .Default(true);

    RegisterParameter("fetcher", Fetcher)
        .DefaultNew();

    RegisterParameter("udf_registry_path", UdfRegistryPath)
        .Default();

    RegisterParameter("enable_tmpfs", EnableTmpfs)
        .Default(true);
    RegisterParameter("enable_map_job_size_adjustment", EnableMapJobSizeAdjustment)
        .Default(true);
    RegisterParameter("enable_job_splitting", EnableJobSplitting)
        .Default(true);

    RegisterParameter("heavy_job_spec_slice_count_threshold", HeavyJobSpecSliceCountThreshold)
        .Default(1000)
        .GreaterThan(0);

    //! By default we disable job size adjustment for partition maps,
    //! since it may lead to partition data skew between nodes.
    RegisterParameter("enable_partition_map_job_size_adjustment", EnablePartitionMapJobSizeAdjustment)
        .Default(false);

    RegisterParameter("user_job_memory_digest_precision", UserJobMemoryDigestPrecision)
        .Default(0.01)
        .GreaterThan(0);
    RegisterParameter("user_job_memory_reserve_quantile", UserJobMemoryReserveQuantile)
        .InRange(0.0, 1.0)
        .Default(0.95);
    RegisterParameter("job_proxy_memory_reserve_quantile", JobProxyMemoryReserveQuantile)
        .InRange(0.0, 1.0)
        .Default(0.95);
    RegisterParameter("resource_overdraft_factor", ResourceOverdraftFactor)
        .InRange(1.0, 10.0)
        .Default(1.1);

    RegisterParameter("iops_threshold", IopsThreshold)
        .Default();
    RegisterParameter("iops_throttler_limit", IopsThrottlerLimit)
        .Default();

    RegisterParameter("chunk_scraper", ChunkScraper)
        .DefaultNew();

    RegisterParameter("max_total_slice_count", MaxTotalSliceCount)
        .Default((i64) 10 * 1000 * 1000)
        .GreaterThan(0);

    RegisterParameter("operation_alerts", OperationAlerts)
        .DefaultNew();

    RegisterParameter("controller_row_buffer_chunk_size", ControllerRowBufferChunkSize)
        .Default(64_KB)
        .GreaterThan(0);

    RegisterParameter("testing_options", TestingOptions)
        .DefaultNew();

    RegisterParameter("suspicious_jobs", SuspiciousJobs)
        .DefaultNew();

    RegisterParameter("job_spec_codec", JobSpecCodec)
        .Default(NCompression::ECodec::Lz4);

    RegisterParameter("job_metrics_report_period", JobMetricsReportPeriod)
        .Default(TDuration::Seconds(15));

    RegisterParameter("system_layer_path", SystemLayerPath)
        .Default();

    RegisterParameter("default_layer_path", DefaultLayerPath)
        .Default();

    RegisterParameter("schedule_job_statistics_log_backoff", ScheduleJobStatisticsLogBackoff)
        .Default(TDuration::Seconds(1));

    RegisterParameter("job_spec_slice_throttler", JobSpecSliceThrottler)
        .Default(New<NConcurrency::TThroughputThrottlerConfig>(500000));

    RegisterParameter("static_orchid_cache_update_period", StaticOrchidCacheUpdatePeriod)
        .Default(TDuration::Seconds(1));

    RegisterParameter("cached_running_jobs_update_period", CachedRunningJobsUpdatePeriod)
        .Default();

    RegisterParameter("tagged_memory_statistics_update_period", TaggedMemoryStatisticsUpdatePeriod)
        .Default(TDuration::Seconds(5));

    RegisterParameter("alerts_update_period", AlertsUpdatePeriod)
        .Default(TDuration::Seconds(1));

    RegisterParameter("total_controller_memory_limit", TotalControllerMemoryLimit)
        .Default();

    RegisterParameter("schedule_job_controller_queue", ScheduleJobControllerQueue)
        .Default(EOperationControllerQueue::Default);

    RegisterParameter("build_job_spec_controller_queue", BuildJobSpecControllerQueue)
        .Default(EOperationControllerQueue::Default);

    RegisterParameter("job_events_controller_queue", JobEventsControllerQueue)
        .Default(EOperationControllerQueue::Default);

    RegisterParameter("schedule_job_wait_time_threshold", ScheduleJobWaitTimeThreshold)
        .Default(TDuration::Seconds(5));

    RegisterParameter("allow_users_group_read_intermediate_data", AllowUsersGroupReadIntermediateData)
        .Default(false);

    RegisterParameter("custom_job_metrics", CustomJobMetrics)
        .Default();

    RegisterPreprocessor([&] () {
        EventLog->MaxRowWeight = 128_MB;
        if (!EventLog->Path) {
            EventLog->Path = "//sys/scheduler/event_log";
        }

        ChunkLocationThrottler->Limit = 10000;

        // Value in options is an upper bound hint on uncompressed data size for merge jobs.
        OrderedMergeOperationOptions->DataWeightPerJob = 20_GB;
        OrderedMergeOperationOptions->MaxDataSlicesPerJob = 10000;

        SortedMergeOperationOptions->DataWeightPerJob = 20_GB;
        SortedMergeOperationOptions->MaxDataSlicesPerJob = 10000;

        UnorderedMergeOperationOptions->DataWeightPerJob = 20_GB;
        UnorderedMergeOperationOptions->MaxDataSlicesPerJob = 10000;
    });

    RegisterPostprocessor([&] {
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
        UpdateOptions(&VanillaOperationOptions, OperationOptions);

        for (const auto& customJobMetricDescription : CustomJobMetrics) {
            for (auto metricName : TEnumTraits<NScheduler::EJobMetricName>::GetDomainValues()) {
                if (FormatEnum(metricName) == customJobMetricDescription.ProfilingName) {
                    THROW_ERROR_EXCEPTION("Metric with profiling name $Qv is already presented",
                         customJobMetricDescription.ProfilingName);
                }
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

DEFINE_DYNAMIC_PHOENIX_TYPE(TEraseOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TMapOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TMapReduceOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TOrderedMergeOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TReduceOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TRemoteCopyOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TSimpleOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TSortedMergeOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TSortOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TSortOperationOptionsBase);
DEFINE_DYNAMIC_PHOENIX_TYPE(TUnorderedMergeOperationOptions);
DEFINE_DYNAMIC_PHOENIX_TYPE(TVanillaOperationOptions);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
