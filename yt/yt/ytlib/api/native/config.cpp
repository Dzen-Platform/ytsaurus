#include "config.h"

#include <yt/yt/ytlib/hive/config.h>

#include <yt/yt/ytlib/node_tracker_client/config.h>

#include <yt/yt/ytlib/scheduler/config.h>

#include <yt/yt/ytlib/table_client/config.h>

#include <yt/yt/ytlib/tablet_client/config.h>

#include <yt/yt/ytlib/queue_client/config.h>

#include <yt/yt/ytlib/transaction_client/config.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/transaction_client/config.h>

namespace NYT::NApi::NNative {

using namespace NObjectClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

void TMasterConnectionConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("rpc_timeout", &TThis::RpcTimeout)
        .Default(TDuration::Seconds(30));

    registrar.Parameter("enable_master_cache_discovery", &TThis::EnableMasterCacheDiscovery)
        .Default(true);
    registrar.Parameter("master_cache_discovery_period", &TThis::MasterCacheDiscoveryPeriod)
        .Default(TDuration::Minutes(1));
    registrar.Parameter("master_cache_discovery_period_splay", &TThis::MasterCacheDiscoveryPeriodSplay)
        .Default(TDuration::Seconds(10));

    registrar.Preprocessor([] (TThis* config) {
        config->RetryAttempts = 100;
        config->RetryTimeout = TDuration::Minutes(3);
    });

    registrar.Postprocessor([] (TThis* config) {
        if (config->EnableMasterCacheDiscovery && config->Endpoints) {
            THROW_ERROR_EXCEPTION("Cannot specify \"endpoints\" when master cache discovery is enabled");
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TClockServersConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("rpc_timeout", &TThis::RpcTimeout)
        .Default(TDuration::Seconds(30));
}

////////////////////////////////////////////////////////////////////////////////

TConnectionConfig::TConnectionConfig()
{
    RegisterParameter("networks", Networks)
        .Default();
    RegisterParameter("timestamp_provider", TimestampProvider)
        .Default();
    RegisterParameter("cell_directory", CellDirectory)
        .DefaultNew();
    RegisterParameter("cell_directory_synchronizer", CellDirectorySynchronizer)
        .DefaultNew();
    RegisterParameter("clock_servers", ClockServers)
        .Default();
    RegisterParameter("master_cell_directory_synchronizer", MasterCellDirectorySynchronizer)
        .DefaultNew();
    RegisterParameter("scheduler", Scheduler)
        .DefaultNew();
    RegisterParameter("queue_agent", QueueAgent)
        .DefaultNew();
    RegisterParameter("transaction_manager", TransactionManager)
        .DefaultNew();
    RegisterParameter("block_cache", BlockCache)
        .DefaultNew();
    RegisterParameter("chunk_meta_cache", ChunkMetaCache)
        .DefaultNew();
    RegisterParameter("chunk_replica_cache", ChunkReplicaCache)
        .DefaultNew();
    RegisterParameter("cluster_directory_synchronizer", ClusterDirectorySynchronizer)
        .DefaultNew();
    RegisterParameter("medium_directory_synchronizer", MediumDirectorySynchronizer)
        .DefaultNew();
    RegisterParameter("node_directory_synchronizer", NodeDirectorySynchronizer)
        .DefaultNew();
    RegisterParameter("chunk_slice_fetcher", ChunkSliceFetcher)
        .DefaultNew();

    RegisterParameter("query_evaluator", QueryEvaluator)
        .DefaultNew();
    RegisterParameter("default_select_rows_timeout", DefaultSelectRowsTimeout)
        // COMPAT(babenko)
        .Alias("query_timeout")
        .Default(TDuration::Seconds(60));
    RegisterParameter("select_rows_response_codec", SelectRowsResponseCodec)
        // COMPAT(babenko)
        .Alias("query_response_codec")
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("default_input_row_limit", DefaultInputRowLimit)
        .GreaterThan(0)
        .Default(1000000);
    RegisterParameter("default_output_row_limit", DefaultOutputRowLimit)
        .GreaterThan(0)
        .Default(1000000);

    RegisterParameter("column_evaluator_cache", ColumnEvaluatorCache)
        .DefaultNew();

    RegisterParameter("write_rows_timeout", WriteRowsTimeout)
        // COMPAT(babenko)
        .Alias("write_timeout")
        .Default(TDuration::Seconds(60));
    RegisterParameter("write_rows_request_codec", WriteRowsRequestCodec)
        // COMPAT(babenko)
        .Alias("write_request_codec")
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("max_rows_per_write_request", MaxRowsPerWriteRequest)
        .GreaterThan(0)
        .Default(1000);
    RegisterParameter("max_data_weight_per_write_request", MaxDataWeightPerWriteRequest)
        .GreaterThan(0)
        .Default(64_MB);
    RegisterParameter("max_rows_per_transaction", MaxRowsPerTransaction)
        .GreaterThan(0)
        .Default(100000);

    RegisterParameter("default_lookup_rows_timeout", DefaultLookupRowsTimeout)
        // COMPAT(babenko)
        .Alias("lookup_timeout")
        .Default(TDuration::Seconds(60));
    RegisterParameter("lookup_rows_request_codec", LookupRowsRequestCodec)
        .Alias("lookup_request_codec")
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("lookup_rows_response_codec", LookupRowsResponseCodec)
        .Alias("lookup_response_codec")
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("max_rows_per_lookup_request", MaxRowsPerLookupRequest)
        .Alias("max_rows_per_read_request")
        .GreaterThan(0)
        .Default(1000);

    RegisterParameter("udf_registry_path", UdfRegistryPath)
        .Default("//tmp/udfs");
    RegisterParameter("function_registry_cache", FunctionRegistryCache)
        .DefaultNew();
    RegisterParameter("function_impl_cache", FunctionImplCache)
        .DefaultNew();

    RegisterParameter("thread_pool_size", ThreadPoolSize)
        .Default(4);

    RegisterParameter("bus_client", BusClient)
        .DefaultNew();
    RegisterParameter("idle_channel_ttl", IdleChannelTtl)
        .Default(TDuration::Minutes(5));

    RegisterParameter("default_get_in_sync_replicas_timeout", DefaultGetInSyncReplicasTimeout)
        .Default(TDuration::Seconds(15));
    RegisterParameter("default_get_tablet_infos_timeout", DefaultGetTabletInfosTimeout)
        .Default(TDuration::Seconds(15));
    RegisterParameter("default_trim_table_timeout", DefaultTrimTableTimeout)
        .Default(TDuration::Seconds(15));
    RegisterParameter("default_get_operation_retry_interval", DefaultGetOperationRetryInterval)
        .Default(TDuration::Seconds(3));
    RegisterParameter("default_get_operation_timeout", DefaultGetOperationTimeout)
        .Default(TDuration::Minutes(5));
    RegisterParameter("default_list_jobs_timeout", DefaultListJobsTimeout)
        .Default(TDuration::Seconds(60));
    RegisterParameter("default_get_job_timeout", DefaultGetJobTimeout)
        .Default(TDuration::Seconds(60));
    RegisterParameter("default_list_operations_timeout", DefaultListOperationsTimeout)
        .Default(TDuration::Seconds(60));

    RegisterParameter("cypress_write_yson_nesting_level_limit", CypressWriteYsonNestingLevelLimit)
        .Default(NYson::OriginalNestingLevelLimit)
        .LessThanOrEqual(NYson::NewNestingLevelLimit);

    RegisterParameter("job_prober_rpc_timeout", JobProberRpcTimeout)
        .Default(TDuration::Seconds(45));

    RegisterParameter("default_cache_sticky_group_size", DefaultCacheStickyGroupSize)
        .Alias("cache_sticky_group_size_override")
        .Default(1);
    RegisterParameter("enable_dynamic_cache_sticky_group_size", EnableDynamicCacheStickyGroupSize)
        .Default(false);

    RegisterParameter("max_request_window_size", MaxRequestWindowSize)
        .GreaterThan(0)
        .Default(65536);

    RegisterParameter("upload_transaction_timeout", UploadTransactionTimeout)
        .Default(TDuration::Seconds(15));
    RegisterParameter("hive_sync_rpc_timeout", HiveSyncRpcTimeout)
        .Default(TDuration::Seconds(30));

    RegisterParameter("connection_name", ConnectionName)
        .Alias("name")
        .Default("default");

    RegisterParameter("permission_cache", PermissionCache)
        .DefaultNew();

    RegisterParameter("job_shell_descriptor_cache", JobShellDescriptorCache)
        .Alias("job_node_descriptor_cache")
        .DefaultNew();

    RegisterParameter("max_chunks_per_fetch", MaxChunksPerFetch)
        .Default(100'000)
        .GreaterThan(0);

    RegisterParameter("max_chunks_per_locate_request", MaxChunksPerLocateRequest)
        .Default(10'000)
        .GreaterThan(0);

    RegisterParameter("nested_input_transaction_timeout", NestedInputTransactionTimeout)
        .Default(TDuration::Minutes(10));
    RegisterParameter("nested_input_transaction_ping_period", NestedInputTransactionPingPeriod)
        .Default(TDuration::Minutes(1));

    RegisterParameter("cluster_liveness_check_timeout", ClusterLivenessCheckTimeout)
        .Default(TDuration::Seconds(15));

    RegisterParameter("chunk_fetch_retries", ChunkFetchRetries)
        .DefaultNew();

    RegisterParameter("enable_networking", EnableNetworking)
        .Default(true);

    RegisterParameter("sync_replica_cache", SyncReplicaCache)
        .DefaultNew();

    RegisterParameter("chaos_cell_channel", ChaosCellChannel)
        .DefaultNew();

    RegisterParameter("hydra_admin_channel", HydraAdminChannel)
        .DefaultNew();

    RegisterPreprocessor([&] {
        FunctionImplCache->Capacity = 100;

        JobShellDescriptorCache->ExpireAfterAccessTime = TDuration::Minutes(5);
        JobShellDescriptorCache->ExpireAfterSuccessfulUpdateTime = TDuration::Minutes(5);
        JobShellDescriptorCache->RefreshTime = TDuration::Minutes(1);

        SyncReplicaCache->ExpireAfterSuccessfulUpdateTime = TDuration::Minutes(5);
        SyncReplicaCache->RefreshTime = TDuration::Seconds(5);
    });
}

////////////////////////////////////////////////////////////////////////////////

TConnectionDynamicConfig::TConnectionDynamicConfig()
{
    RegisterParameter("sync_replica_cache", SyncReplicaCache)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

TRemoteTimestampProviderConfigPtr CreateRemoteTimestampProviderConfig(TMasterConnectionConfigPtr config)
{
    auto timestampProviderConfig = New<TRemoteTimestampProviderConfig>();

    // Use masters for timestamp generation.
    timestampProviderConfig->Addresses = config->Addresses;
    timestampProviderConfig->RpcTimeout = config->RpcTimeout;

    // TRetryingChannelConfig
    timestampProviderConfig->RetryBackoffTime = config->RetryBackoffTime;
    timestampProviderConfig->RetryAttempts = config->RetryAttempts;
    timestampProviderConfig->RetryTimeout = config->RetryTimeout;

    return timestampProviderConfig;
}

/////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative

