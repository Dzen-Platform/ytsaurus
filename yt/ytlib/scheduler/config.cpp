#include "config.h"

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

TJobIOConfig::TJobIOConfig()
{
    RegisterParameter("table_reader", TableReader)
        .DefaultNew();
    RegisterParameter("table_writer", TableWriter)
        .DefaultNew();

    RegisterParameter("control_attributes", ControlAttributes)
        .DefaultNew();

    RegisterParameter("error_file_writer", ErrorFileWriter)
        .DefaultNew();

    RegisterParameter("buffer_row_count", BufferRowCount)
        .Default((i64) 10000)
        .GreaterThan(0);

    RegisterParameter("pipe_io_pool_size", PipeIOPoolSize)
        .Default(1)
        .GreaterThan(0);

    RegisterInitializer([&] () {
        ErrorFileWriter->UploadReplicationFactor = 1;
    });
}

TTestingOperationOptions::TTestingOperationOptions()
{
    RegisterParameter("scheduling_delay", SchedulingDelay)
        .Default(TDuration::Seconds(0));
    RegisterParameter("scheduling_delay_type", SchedulingDelayType)
        .Default(ESchedulingDelayType::Sync);
}

TSupportsSchedulingTagsConfig::TSupportsSchedulingTagsConfig()
{
    RegisterParameter("scheduling_tag", SchedulingTag)
        .Default();

    RegisterParameter("scheduling_tag_filter", SchedulingTagFilter)
        .Default();
}

void TSupportsSchedulingTagsConfig::OnLoaded()
{
    if (SchedulingTag) {
        if (!SchedulingTagFilter.Clauses().empty()) {
            THROW_ERROR_EXCEPTION("Options \"scheduling_tag\" and \"scheduling_tag_filter\" "
                "cannot be specified simultanously")
                << TErrorAttribute("scheduling_tag", *SchedulingTag)
                << TErrorAttribute("scheduling_tag_filter", SchedulingTagFilter);
        }
        TConjunctiveClause clause;
        clause.Include() = std::vector<Stroka>({*SchedulingTag});
        SchedulingTagFilter.Clauses().push_back(clause);
        SchedulingTag = Null;
    }
    if (SchedulingTagFilter.Clauses().size() > MaxSchedulingTagRuleCount) {
        THROW_ERROR_EXCEPTION("Specifying more than %v scheduling tag filters is not allowed",
            MaxSchedulingTagRuleCount);
    }
}

TOperationSpecBase::TOperationSpecBase()
{
    RegisterParameter("intermediate_data_account", IntermediateDataAccount)
        .Default("intermediate");
    RegisterParameter("intermediate_compression_codec", IntermediateCompressionCodec)
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("intermediate_data_replication_factor", IntermediateDataReplicationFactor)
        .Default(1);
    RegisterParameter("intermediate_data_medium", IntermediateDataMediumName)
        .Default(NChunkClient::DefaultStoreMediumName);
    RegisterParameter("intermediate_data_acl", IntermediateDataAcl)
        .Default(NYTree::BuildYsonNodeFluently()
            .BeginList()
                .Item().BeginMap()
                    .Item("action").Value("allow")
                    .Item("subjects").BeginList()
                        .Item().Value("everyone")
                    .EndList()
                    .Item("permissions").BeginList()
                        .Item().Value("read")
                    .EndList()
                .EndMap()
            .EndList()->AsList());

    RegisterParameter("job_node_account", JobNodeAccount)
        .Default(NSecurityClient::TmpAccountName);

    RegisterParameter("unavailable_chunk_strategy", UnavailableChunkStrategy)
        .Default(EUnavailableChunkAction::Wait);
    RegisterParameter("unavailable_chunk_tactics", UnavailableChunkTactics)
        .Default(EUnavailableChunkAction::Wait);

    RegisterParameter("max_data_size_per_job", MaxDataSizePerJob)
        .Default((i64) 200 * 1024 * 1024 * 1024)
        .GreaterThan(0);

    RegisterParameter("max_failed_job_count", MaxFailedJobCount)
        .Default(100)
        .GreaterThanOrEqual(0)
        .LessThanOrEqual(10000);
    RegisterParameter("max_stderr_count", MaxStderrCount)
        .Default(100)
        .GreaterThanOrEqual(0)
        .LessThanOrEqual(100);

    RegisterParameter("job_proxy_memory_overcommit_limit", JobProxyMemoryOvercommitLimit)
        .Default()
        .GreaterThanOrEqual(0);

    RegisterParameter("job_proxy_ref_counted_tracker_log_period", JobProxyRefCountedTrackerLogPeriod)
        .Default(TDuration::Seconds(5));

    RegisterParameter("enable_sort_verification", EnableSortVerification)
        .Default(true);

    RegisterParameter("title", Title)
        .Default();

    RegisterParameter("check_multichunk_files", CheckMultichunkFiles)
        .Default(true);

    RegisterParameter("time_limit", TimeLimit)
        .Default();

    RegisterParameter("testing", TestingOperationOptions)
        .Default();

    RegisterParameter("owners", Owners)
        .Default();

    RegisterParameter("secure_vault", SecureVault)
        .Default();

    RegisterParameter("fail_controller", FailController)
        .Default(false);

    RegisterParameter("available_nodes_missing_timeout", AvailableNodesMissingTimeout)
        .Default(TDuration::Hours(1));

    RegisterValidator([&] () {
        if (UnavailableChunkStrategy == EUnavailableChunkAction::Wait &&
            UnavailableChunkTactics == EUnavailableChunkAction::Skip)
        {
            THROW_ERROR_EXCEPTION("Your tactics conflicts with your strategy, Luke!");
        }
    });

    RegisterValidator([&] () {
        if (SecureVault) {
            for (const auto& name : SecureVault->GetKeys()) {
                ValidateEnvironmentVariableName(name);
            }
        }
    });

    // XXX(ignat): it seems that GetOptions is not used for this config.
    // Should we delete this line?
    SetKeepOptions(true);
}

TUserJobSpec::TUserJobSpec()
{
    RegisterParameter("command", Command)
        .NonEmpty();
    RegisterParameter("file_paths", FilePaths)
        .Default();
    RegisterParameter("format", Format)
        .Default();
    RegisterParameter("input_format", InputFormat)
        .Default();
    RegisterParameter("output_format", OutputFormat)
        .Default();
    RegisterParameter("enable_input_table_index", EnableInputTableIndex)
        .Default();
    RegisterParameter("environment", Environment)
        .Default();
    RegisterParameter("cpu_limit", CpuLimit)
        .Default(1)
        .GreaterThanOrEqual(0);
    RegisterParameter("job_time_limit", JobTimeLimit)
        .Default()
        .GreaterThanOrEqual(TDuration::Seconds(1));
    RegisterParameter("memory_limit", MemoryLimit)
        .Default((i64) 512 * 1024 * 1024)
        .GreaterThan(0)
        .LessThanOrEqual((i64)1024 * 1024 * 1024 * 1024);
    RegisterParameter("memory_reserve_factor", MemoryReserveFactor)
        .Default(0.5)
        .GreaterThan(0.)
        .LessThanOrEqual(1.);
    RegisterParameter("include_memory_mapped_files", IncludeMemoryMappedFiles)
        .Default(true);
    RegisterParameter("use_yamr_descriptors", UseYamrDescriptors)
        .Default(false);
    RegisterParameter("check_input_fully_consumed", CheckInputFullyConsumed)
        .Default(false);
    RegisterParameter("max_stderr_size", MaxStderrSize)
        .Default((i64)5 * 1024 * 1024) // 5MB
        .GreaterThan(0)
        .LessThanOrEqual((i64)1024 * 1024 * 1024);
    RegisterParameter("custom_statistics_count_limit", CustomStatisticsCountLimit)
        .Default(128)
        .GreaterThan(0)
        .LessThanOrEqual(1024);
    RegisterParameter("tmpfs_size", TmpfsSize)
        .Default(Null)
        .GreaterThan(0);
    RegisterParameter("tmpfs_path", TmpfsPath)
        .Default(Null);
    RegisterParameter("copy_files", CopyFiles)
        .Default(false);

    RegisterValidator([&] () {
        if (TmpfsSize && *TmpfsSize > MemoryLimit) {
            THROW_ERROR_EXCEPTION("Size of tmpfs must be less than or equal to memory limit")
                << TErrorAttribute("tmpfs_size", *TmpfsSize)
                << TErrorAttribute("memory_limit", MemoryLimit);
        }
        // Memory reserve should greater than or equal to tmpfs_size (see YT-5518 for more details).
        if (TmpfsPath) {
            i64 tmpfsSize = TmpfsSize ? *TmpfsSize : MemoryLimit;
            MemoryReserveFactor = std::min(1.0, std::max(MemoryReserveFactor, double(tmpfsSize) / MemoryLimit));
        }
    });

    RegisterValidator([&] () {
        for (const auto& pair : Environment) {
            ValidateEnvironmentVariableName(pair.first);
        }
    });
}

void TUserJobSpec::InitEnableInputTableIndex(int inputTableCount, TJobIOConfigPtr jobIOConfig)
{
    if (!EnableInputTableIndex) {
        EnableInputTableIndex = (inputTableCount != 1);
    }

    jobIOConfig->ControlAttributes->EnableTableIndex = *EnableInputTableIndex;
}

TInputlyQueryableSpec::TInputlyQueryableSpec()
{
    RegisterParameter("input_query", InputQuery)
        .Default();
    RegisterParameter("input_schema", InputSchema)
        .Default();

    RegisterValidator([&] () {
        if (InputSchema && !InputQuery) {
            THROW_ERROR_EXCEPTION("Found \"input_schema\" without \"input_query\" in operation spec");
        }
    });
}

TOperationWithUserJobSpec::TOperationWithUserJobSpec()
{
    RegisterParameter("stderr_table_path", StderrTablePath)
        .Default();
    RegisterParameter("stderr_table_writer_config", StderrTableWriterConfig)
        .DefaultNew();

    RegisterParameter("core_table_path", CoreTablePath)
        .Default();
    RegisterParameter("core_table_writer_config", CoreTableWriterConfig)
        .DefaultNew();

    RegisterParameter("enable_job_splitting", EnableJobSplitting)
        .Default(true);
}

void TOperationWithUserJobSpec::OnLoaded()
{
    if (StderrTablePath) {
        *StderrTablePath = StderrTablePath->Normalize();
    }

    if (CoreTablePath) {
        *CoreTablePath = CoreTablePath->Normalize();
    }
}

TOperationWithLegacyControllerSpec::TOperationWithLegacyControllerSpec()
{
    RegisterParameter("use_legacy_controller", UseLegacyController)
        .Default(true);
}

TSimpleOperationSpecBase::TSimpleOperationSpecBase()
{
    RegisterParameter("data_size_per_job", DataSizePerJob)
        .Default()
        .GreaterThan(0);
    RegisterParameter("job_count", JobCount)
        .Default()
        .GreaterThan(0);
    RegisterParameter("max_job_count", MaxJobCount)
        .Default()
        .GreaterThan(0);
    RegisterParameter("locality_timeout", LocalityTimeout)
        .Default(TDuration::Seconds(5));
    RegisterParameter("job_io", JobIO)
        .DefaultNew();

    RegisterParameter("job_proxy_memory_digest", JobProxyMemoryDigest)
        .Default(New<TLogDigestConfig>(0.5, 2.0, 1.0));
}

TUnorderedOperationSpecBase::TUnorderedOperationSpecBase()
{
    RegisterParameter("input_table_paths", InputTablePaths)
        .NonEmpty();

    RegisterInitializer([&] () {
        JobIO->TableReader->MaxBufferSize = (i64) 256 * 1024 * 1024;
    });
}

void TUnorderedOperationSpecBase::OnLoaded()
{
    TSimpleOperationSpecBase::OnLoaded();

    InputTablePaths = NYT::NYPath::Normalize(InputTablePaths);
}

TMapOperationSpec::TMapOperationSpec()
{
    RegisterParameter("mapper", Mapper)
        .DefaultNew();
    RegisterParameter("output_table_paths", OutputTablePaths)
        .NonEmpty();
    RegisterParameter("ordered", Ordered)
        .Default(false);
}

void TMapOperationSpec::OnLoaded()
{
    TUnorderedOperationSpecBase::OnLoaded();

    OutputTablePaths = NYT::NYPath::Normalize(OutputTablePaths);

    Mapper->InitEnableInputTableIndex(InputTablePaths.size(), JobIO);
}

TUnorderedMergeOperationSpec::TUnorderedMergeOperationSpec()
{
    RegisterParameter("output_table_path", OutputTablePath);
    RegisterParameter("combine_chunks", CombineChunks)
        .Default(false);
    RegisterParameter("force_transform", ForceTransform)
        .Default(false);
    RegisterParameter("schema_inference_mode", SchemaInferenceMode)
        .Default(ESchemaInferenceMode::Auto);
}

void TUnorderedMergeOperationSpec::OnLoaded()
{
    TUnorderedOperationSpecBase::OnLoaded();

    OutputTablePath = OutputTablePath.Normalize();
}

TMergeOperationSpec::TMergeOperationSpec()
{
    RegisterParameter("input_table_paths", InputTablePaths)
        .NonEmpty();
    RegisterParameter("output_table_path", OutputTablePath);
    RegisterParameter("mode", Mode)
        .Default(EMergeMode::Unordered);
    RegisterParameter("combine_chunks", CombineChunks)
        .Default(false);
    RegisterParameter("force_transform", ForceTransform)
        .Default(false);
    RegisterParameter("merge_by", MergeBy)
        .Default();
    RegisterParameter("schema_inference_mode", SchemaInferenceMode)
        .Default(ESchemaInferenceMode::Auto);
}

void TMergeOperationSpec::OnLoaded()
{
    TSimpleOperationSpecBase::OnLoaded();

    InputTablePaths = NYT::NYPath::Normalize(InputTablePaths);
    OutputTablePath = OutputTablePath.Normalize();
}

TEraseOperationSpec::TEraseOperationSpec()
{
    RegisterParameter("table_path", TablePath);
    RegisterParameter("combine_chunks", CombineChunks)
        .Default(false);
    RegisterParameter("schema_inference_mode", SchemaInferenceMode)
        .Default(ESchemaInferenceMode::Auto);
}

void TEraseOperationSpec::OnLoaded()
{
    TSimpleOperationSpecBase::OnLoaded();

    TablePath = TablePath.Normalize();
}

TReduceOperationSpecBase::TReduceOperationSpecBase()
{
    RegisterParameter("reducer", Reducer)
        .DefaultNew();
    RegisterParameter("input_table_paths", InputTablePaths)
        .NonEmpty();
    RegisterParameter("output_table_paths", OutputTablePaths)
        .NonEmpty();

    RegisterValidator([&] () {
        if (!JoinBy.empty()) {
            NTableClient::ValidateKeyColumns(JoinBy);
        }
    });
}

void TReduceOperationSpecBase::OnLoaded()
{
    TSimpleOperationSpecBase::OnLoaded();

    InputTablePaths = NYT::NYPath::Normalize(InputTablePaths);
    OutputTablePaths = NYT::NYPath::Normalize(OutputTablePaths);

    Reducer->InitEnableInputTableIndex(InputTablePaths.size(), JobIO);
}

TReduceOperationSpec::TReduceOperationSpec()
{
    RegisterParameter("join_by", JoinBy)
        .Default();
    RegisterParameter("reduce_by", ReduceBy)
        .NonEmpty();
    RegisterParameter("sort_by", SortBy)
        .Default();
    RegisterParameter("pivot_keys", PivotKeys)
        .Default();

    RegisterValidator([&] () {
        if (!ReduceBy.empty()) {
            NTableClient::ValidateKeyColumns(ReduceBy);
        }

        if (!SortBy.empty()) {
            NTableClient::ValidateKeyColumns(SortBy);
        }
    });
}

TJoinReduceOperationSpec::TJoinReduceOperationSpec()
{
    RegisterParameter("join_by", JoinBy)
        .NonEmpty();
}

void TJoinReduceOperationSpec::OnLoaded()
{
    TReduceOperationSpecBase::OnLoaded();
    bool hasPrimary = false;
    for (const auto& path: InputTablePaths) {
        hasPrimary |= path.GetPrimary();
    }
    if (hasPrimary) {
        for (auto& path: InputTablePaths) {
            path.Attributes().Set("foreign", !path.GetPrimary());
            path.Attributes().Remove("primary");
        }
    }
}

TSortOperationSpecBase::TSortOperationSpecBase()
{
    RegisterParameter("input_table_paths", InputTablePaths)
        .NonEmpty();
    RegisterParameter("partition_count", PartitionCount)
        .Default()
        .GreaterThan(0);
    RegisterParameter("partition_data_size", PartitionDataSize)
        .Default()
        .GreaterThan(0);
    RegisterParameter("data_size_per_sort_job", DataSizePerShuffleJob)
        .Default((i64)2 * 1024 * 1024 * 1024)
        .GreaterThan(0);
    RegisterParameter("shuffle_start_threshold", ShuffleStartThreshold)
        .Default(0.75)
        .InRange(0.0, 1.0);
    RegisterParameter("merge_start_threshold", MergeStartThreshold)
        .Default(0.9)
        .InRange(0.0, 1.0);
    RegisterParameter("sort_locality_timeout", SortLocalityTimeout)
        .Default(TDuration::Minutes(1));
    RegisterParameter("sort_assignment_timeout", SortAssignmentTimeout)
        .Default(TDuration::Seconds(5));
    RegisterParameter("shuffle_network_limit", ShuffleNetworkLimit)
        .Default(0);
    RegisterParameter("sort_by", SortBy)
        .NonEmpty();
    RegisterParameter("enable_partitioned_data_balancing", EnablePartitionedDataBalancing)
        .Default(true);
    RegisterParameter("partitioned_data_balancing_tolerance", PartitionedDataBalancingTolerance)
        .Default(3.0);

    RegisterParameter("sort_job_proxy_memory_digest", SortJobProxyMemoryDigest)
        .Default(New<TLogDigestConfig>(0.5, 1.0, 1.0));
    RegisterParameter("partition_job_proxy_memory_digest", PartitionJobProxyMemoryDigest)
        .Default(New<TLogDigestConfig>(0.5, 2.0, 1.0));

    RegisterValidator([&] () {
        NTableClient::ValidateKeyColumns(SortBy);
    });


}

void TSortOperationSpecBase::OnLoaded()
{
    TOperationSpecBase::OnLoaded();

    InputTablePaths = NYT::NYPath::Normalize(InputTablePaths);
}

TSortOperationSpec::TSortOperationSpec()
{
    RegisterParameter("output_table_path", OutputTablePath);
    RegisterParameter("samples_per_partition", SamplesPerPartition)
        .Default(1000)
        .GreaterThan(1);
    RegisterParameter("partition_job_io", PartitionJobIO)
        .DefaultNew();
    RegisterParameter("sort_job_io", SortJobIO)
        .DefaultNew();
    RegisterParameter("merge_job_io", MergeJobIO)
        .DefaultNew();

    // Provide custom names for shared settings.
    RegisterParameter("partition_job_count", PartitionJobCount)
        .Default()
        .GreaterThan(0);
    RegisterParameter("data_size_per_partition_job", DataSizePerPartitionJob)
        .Default()
        .GreaterThan(0);
    RegisterParameter("simple_sort_locality_timeout", SimpleSortLocalityTimeout)
        .Default(TDuration::Seconds(5));
    RegisterParameter("simple_merge_locality_timeout", SimpleMergeLocalityTimeout)
        .Default(TDuration::Seconds(5));
    RegisterParameter("partition_locality_timeout", PartitionLocalityTimeout)
        .Default(TDuration::Seconds(5));
    RegisterParameter("merge_locality_timeout", MergeLocalityTimeout)
        .Default(TDuration::Minutes(1));

    RegisterParameter("merge_job_proxy_memory_digest", MergeJobProxyMemoryDigest)
        .Default(New<TLogDigestConfig>(0.5, 2.0, 1.0));
    RegisterParameter("schema_inference_mode", SchemaInferenceMode)
        .Default(ESchemaInferenceMode::Auto);

    RegisterParameter("data_size_per_sorted_merge_job", DataSizePerSortedJob)
        .Default(Null);

    RegisterInitializer([&] () {
        PartitionJobIO->TableReader->MaxBufferSize = (i64) 1024 * 1024 * 1024;
        PartitionJobIO->TableWriter->MaxBufferSize = (i64) 2 * 1024 * 1024 * 1024; // 2 GB

        SortJobIO->TableReader->MaxBufferSize = (i64) 1024 * 1024 * 1024;
        SortJobIO->TableReader->RetryCount = 3;
        MergeJobIO->TableReader->RetryCount = 3;

        MapSelectivityFactor = 1.0;
    });
}

void TSortOperationSpec::OnLoaded()
{
    TSortOperationSpecBase::OnLoaded();

    OutputTablePath = OutputTablePath.Normalize();
}

TMapReduceOperationSpec::TMapReduceOperationSpec()
{
    RegisterParameter("output_table_paths", OutputTablePaths)
        .NonEmpty();
    RegisterParameter("reduce_by", ReduceBy)
        .Default();
    // Mapper can be absent -- leave it Null by default.
    RegisterParameter("mapper", Mapper)
        .Default();
    // ReduceCombiner can be absent -- leave it Null by default.
    RegisterParameter("reduce_combiner", ReduceCombiner)
        .Default();
    RegisterParameter("reducer", Reducer)
        .DefaultNew();
    RegisterParameter("map_job_io", PartitionJobIO)
        .DefaultNew();
    RegisterParameter("sort_job_io", SortJobIO)
        .DefaultNew();
    RegisterParameter("reduce_job_io", MergeJobIO)
        .DefaultNew();

    // Provide custom names for shared settings.
    RegisterParameter("map_job_count", PartitionJobCount)
        .Default()
        .GreaterThan(0);
    RegisterParameter("data_size_per_map_job", DataSizePerPartitionJob)
        .Default()
        .GreaterThan(0);
    RegisterParameter("map_locality_timeout", PartitionLocalityTimeout)
        .Default(TDuration::Seconds(5));
    RegisterParameter("reduce_locality_timeout", MergeLocalityTimeout)
        .Default(TDuration::Minutes(1));
    RegisterParameter("map_selectivity_factor", MapSelectivityFactor)
        .Default(1.0)
        .GreaterThan(0);

    RegisterParameter("sorted_reduce_job_proxy_memory_digest", SortedReduceJobProxyMemoryDigest)
        .Default(New<TLogDigestConfig>(0.5, 2.0, 1.0));
    RegisterParameter("partition_reduce_job_proxy_memory_digest", PartitionReduceJobProxyMemoryDigest)
        .Default(New<TLogDigestConfig>(0.5, 1.0, 1.0));
    RegisterParameter("reduce_combiner_job_proxy_memory_digest", ReduceCombinerJobProxyMemoryDigest)
        .Default(New<TLogDigestConfig>(0.5, 1.0, 1.0));

    RegisterParameter("data_size_per_reduce_job", DataSizePerSortedJob)
        .Default(Null);

    RegisterParameter("force_reduce_combiners", ForceReduceCombiners)
        .Default(false);

    // The following settings are inherited from base but make no sense for map-reduce:
    //   SimpleSortLocalityTimeout
    //   SimpleMergeLocalityTimeout
    //   MapSelectivityFactor

    RegisterInitializer([&] () {
        PartitionJobIO->TableReader->MaxBufferSize = (i64) 256 * 1024 * 1024;
        PartitionJobIO->TableWriter->MaxBufferSize = (i64) 2 * 1024 * 1024 * 1024; // 2 GBs

        SortJobIO->TableReader->MaxBufferSize = (i64) 1024 * 1024 * 1024;

        SortJobIO->TableReader->RetryCount = 3;
        MergeJobIO->TableReader->RetryCount = 3;
    });

    RegisterValidator([&] () {
        auto throwError = [] (NTableClient::EControlAttribute attribute, const Stroka& jobType) {
            THROW_ERROR_EXCEPTION(
                "%Qlv contol attribute is not supported by %v jobs in map-reduce operation",
                attribute,
                jobType);
        };
        auto validateControlAttributes = [&] (const NFormats::TControlAttributesConfigPtr& attributes, const Stroka& jobType) {
            if (attributes->EnableTableIndex) {
                throwError(NTableClient::EControlAttribute::TableIndex, jobType);
            }
            if (attributes->EnableRowIndex) {
                throwError(NTableClient::EControlAttribute::RowIndex, jobType);
            }
            if (attributes->EnableRangeIndex) {
                throwError(NTableClient::EControlAttribute::RangeIndex, jobType);
            }
        };
        if (ForceReduceCombiners && !ReduceCombiner) {
            THROW_ERROR_EXCEPTION("Found \"force_reduce_combiners\" without \"reduce_combiner\" in operation spec");
        }
        validateControlAttributes(MergeJobIO->ControlAttributes, "reduce");
        validateControlAttributes(SortJobIO->ControlAttributes, "reduce_combiner");

        if (!ReduceBy.empty()) {
            NTableClient::ValidateKeyColumns(ReduceBy);
        }
    });
}

void TMapReduceOperationSpec::OnLoaded()
{
    TSortOperationSpecBase::OnLoaded();

    if (ReduceBy.empty()) {
        ReduceBy = SortBy;
    }

    OutputTablePaths = NYT::NYPath::Normalize(OutputTablePaths);

    if (Mapper) {
        Mapper->InitEnableInputTableIndex(InputTablePaths.size(), PartitionJobIO);
    }
    Reducer->InitEnableInputTableIndex(1, MergeJobIO);
}

TRemoteCopyOperationSpec::TRemoteCopyOperationSpec()
{
    RegisterParameter("cluster_name", ClusterName)
        .Default();
    RegisterParameter("input_table_paths", InputTablePaths)
        .NonEmpty();
    RegisterParameter("output_table_path", OutputTablePath);
    RegisterParameter("network_name", NetworkName)
        .Default();
    RegisterParameter("cluster_connection", ClusterConnection)
        .Default();
    RegisterParameter("max_chunk_count_per_job", MaxChunkCountPerJob)
        .Default(100);
    RegisterParameter("copy_attributes", CopyAttributes)
        .Default(false);
    RegisterParameter("attribute_keys", AttributeKeys)
        .Default();
    RegisterParameter("schema_inference_mode", SchemaInferenceMode)
        .Default(ESchemaInferenceMode::Auto);
}

void TRemoteCopyOperationSpec::OnLoaded()
{
    TOperationSpecBase::OnLoaded();

    InputTablePaths = NYPath::Normalize(InputTablePaths);
    OutputTablePath = OutputTablePath.Normalize();

    if (!ClusterName && !ClusterConnection) {
        THROW_ERROR_EXCEPTION("Neither cluster name nor cluster connection specified.");
    }
}

TResourceLimitsConfig::TResourceLimitsConfig()
{
    RegisterParameter("user_slots", UserSlots)
        .Default()
        .GreaterThanOrEqual(0);
    RegisterParameter("cpu", Cpu)
        .Default()
        .GreaterThanOrEqual(0);
    RegisterParameter("network", Network)
        .Default()
        .GreaterThanOrEqual(0);
    RegisterParameter("memory", Memory)
        .Default()
        .GreaterThanOrEqual(0);
}

TSchedulableConfig::TSchedulableConfig()
{
    RegisterParameter("weight", Weight)
        .Default(1.0)
        .InRange(MinSchedulableWeight, MaxSchedulableWeight);

    RegisterParameter("max_share_ratio", MaxShareRatio)
        .Default(1.0)
        .InRange(0.0, 1.0);
    RegisterParameter("resource_limits", ResourceLimits)
        .DefaultNew();

    RegisterParameter("min_share_ratio", MinShareRatio)
        .Default(0.0)
        .InRange(0.0, 1.0);
    RegisterParameter("min_share_resources", MinShareResources)
        .DefaultNew();

    RegisterParameter("min_share_preemption_timeout", MinSharePreemptionTimeout)
        .Default();
    RegisterParameter("fair_share_preemption_timeout", FairSharePreemptionTimeout)
        .Default();
    RegisterParameter("fair_share_starvation_tolerance", FairShareStarvationTolerance)
        .InRange(0.0, 1.0)
        .Default();

    RegisterParameter("min_share_preemption_timeout_limit", MinSharePreemptionTimeoutLimit)
        .Default();
    RegisterParameter("fair_share_preemption_timeout_limit", FairSharePreemptionTimeoutLimit)
        .Default();
    RegisterParameter("fair_share_starvation_tolerance_limit", FairShareStarvationToleranceLimit)
        .InRange(0.0, 1.0)
        .Default();

    RegisterParameter("allow_aggressive_starvation_preemption", AllowAggressiveStarvationPreemption)
        .Default();
}

TPoolConfig::TPoolConfig()
{
    RegisterParameter("mode", Mode)
        .Default(ESchedulingMode::FairShare);

    RegisterParameter("max_running_operation_count", MaxRunningOperationCount)
        .Alias("max_running_operations")
        .Default();

    RegisterParameter("max_operation_count", MaxOperationCount)
        .Alias("max_operations")
        .Default();

    RegisterParameter("fifo_sort_parameters", FifoSortParameters)
        .Default({EFifoSortParameter::Weight, EFifoSortParameter::StartTime})
        .NonEmpty();

    RegisterParameter("enable_aggressive_starvation", EnableAggressiveStarvation)
        .Alias("aggressive_starvation_enabled")
        .Default(false);

    RegisterParameter("forbid_immediate_operations", ForbidImmediateOperations)
        .Default(false);
}

void TPoolConfig::Validate()
{
    if (MaxOperationCount && MaxRunningOperationCount && *MaxOperationCount < *MaxRunningOperationCount) {
        THROW_ERROR_EXCEPTION("%Qv must be greater that or equal to %Qv, but %v < %v",
            "max_operation_count",
            "max_runnning_operation_count",
            *MaxOperationCount,
            *MaxRunningOperationCount);
    }
}

TStrategyOperationSpec::TStrategyOperationSpec()
{
    RegisterParameter("pool", Pool)
        .Default()
        .NonEmpty();
}

TOperationRuntimeParams::TOperationRuntimeParams()
{
    RegisterParameter("weight", Weight)
        .Default(1.0)
        .InRange(MinSchedulableWeight, MaxSchedulableWeight);
}

TSchedulerConnectionConfig::TSchedulerConnectionConfig()
{
    RegisterParameter("rpc_timeout", RpcTimeout)
        .Default(TDuration::Seconds(60));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
