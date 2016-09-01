#include "config.h"

#include <yt/ytlib/hive/config.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/ytlib/scheduler/config.h>

#include <yt/ytlib/table_client/config.h>

#include <yt/ytlib/tablet_client/config.h>

#include <yt/ytlib/transaction_client/config.h>

#include <yt/ytlib/object_client/helpers.h>

namespace NYT {
namespace NApi {

using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

TMasterConnectionConfig::TMasterConnectionConfig()
{
    RegisterParameter("rpc_timeout", RpcTimeout)
        .Default(TDuration::Seconds(15));
}

////////////////////////////////////////////////////////////////////////////////

TNativeConnectionConfig::TNativeConnectionConfig()
{
    RegisterParameter("networks", Networks)
        .Default(NNodeTrackerClient::DefaultNetworkPreferences);
    RegisterParameter("primary_master", PrimaryMaster);
    RegisterParameter("secondary_masters", SecondaryMasters)
        .Default();
    RegisterParameter("master_cache", MasterCache)
        .Default();
    RegisterParameter("enable_read_from_followers", EnableReadFromFollowers)
        .Default(true);
    RegisterParameter("timestamp_provider", TimestampProvider)
        .Default();
    RegisterParameter("cell_directory", CellDirectory)
        .DefaultNew();
    RegisterParameter("scheduler", Scheduler)
        .DefaultNew();
    RegisterParameter("transaction_manager", TransactionManager)
        .DefaultNew();
    RegisterParameter("block_cache", BlockCache)
        .DefaultNew();
    RegisterParameter("table_mount_cache", TableMountCache)
        .DefaultNew();

    RegisterParameter("query_evaluator", QueryEvaluator)
        .DefaultNew();
    RegisterParameter("query_timeout", QueryTimeout)
        .Default(TDuration::Seconds(60));
    RegisterParameter("query_response_codec", QueryResponseCodec)
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("default_input_row_limit", DefaultInputRowLimit)
        .GreaterThan(0)
        .Default(1000000);
    RegisterParameter("default_output_row_limit", DefaultOutputRowLimit)
        .GreaterThan(0)
        .Default(1000000);

    RegisterParameter("column_evaluator_cache", ColumnEvaluatorCache)
        .DefaultNew();

    RegisterParameter("write_timeout", WriteTimeout)
        .Default(TDuration::Seconds(60));
    RegisterParameter("write_request_codec", WriteRequestCodec)
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("max_rows_per_write_request", MaxRowsPerWriteRequest)
        .GreaterThan(0)
        .Default(1000);
    RegisterParameter("max_rows_per_transaction", MaxRowsPerTransaction)
        .GreaterThan(0)
        .Default(100000);

    RegisterParameter("lookup_timeout", LookupTimeout)
        .Default(TDuration::Seconds(60));
    RegisterParameter("lookup_request_codec", LookupRequestCodec)
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("lookup_response_codec", LookupResponseCodec)
        .Default(NCompression::ECodec::Lz4);
    RegisterParameter("max_rows_per_read_request", MaxRowsPerReadRequest)
        .GreaterThan(0)
        .Default(1000);

    RegisterParameter("enable_udf", EnableUdf)
        .Default(false);
    RegisterParameter("udf_registry_path", UdfRegistryPath)
        .Default("//tmp/udfs");
    RegisterParameter("function_registry_cache", FunctionRegistryCache)
        .DefaultNew();
    RegisterParameter("function_impl_cache", FunctionImplCache)
        .DefaultNew();

    RegisterParameter("table_mount_info_update_retry_count", TableMountInfoUpdateRetryCount)
        .GreaterThanOrEqual(0)
        .Default(5);
    RegisterParameter("table_mount_info_update_retry_time", TableMountInfoUpdateRetryPeriod)
        .GreaterThan(TDuration::MicroSeconds(0))
        .Default(TDuration::Seconds(1));

    RegisterParameter("light_pool_size", LightPoolSize)
        .Describe("Number of threads handling light requests")
        .Default(1);
    RegisterParameter("heavy_pool_size", HeavyPoolSize)
        .Describe("Number of threads handling heavy requests")
        .Default(4);

    RegisterParameter("max_concurrent_requests", MaxConcurrentRequests)
        .Describe("Maximum concurrent requests in client")
        .GreaterThan(0)
        .Default(1000);

    RegisterInitializer([&] () {
        FunctionImplCache->Capacity = 100;
    });

    RegisterValidator([&] () {
        const auto& cellId = PrimaryMaster->CellId;
        auto primaryCellTag = CellTagFromId(PrimaryMaster->CellId);
        yhash_set<TCellTag> cellTags = {primaryCellTag};
        for (const auto& cellConfig : SecondaryMasters) {
            if (ReplaceCellTagInId(cellConfig->CellId, primaryCellTag) != cellId) {
                THROW_ERROR_EXCEPTION("Invalid cell id %v specified for secondary master in connection configuration",
                    cellConfig->CellId);
            }
            auto cellTag = CellTagFromId(cellConfig->CellId);
            if (!cellTags.insert(cellTag).second) {
                THROW_ERROR_EXCEPTION("Duplicate cell tag %v in connection configuration",
                    cellTag);
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

