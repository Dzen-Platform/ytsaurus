#include "config.h"

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NCellMaster {

using namespace NObjectClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TCellMasterConfig::TCellMasterConfig()
{
    RegisterParameter("networks", Networks)
        .Default(NNodeTrackerClient::DefaultNetworkPreferences);
    RegisterParameter("primary_master", PrimaryMaster)
        .Default();
    RegisterParameter("secondary_masters", SecondaryMasters)
        .Default();
    RegisterParameter("election_manager", ElectionManager)
        .DefaultNew();
    RegisterParameter("changelogs", Changelogs);
    RegisterParameter("snapshots", Snapshots);
    RegisterParameter("hydra_manager", HydraManager)
        .DefaultNew();
    RegisterParameter("cell_directory", CellDirectory)
        .DefaultNew();
    RegisterParameter("cell_directory_synchronizer", CellDirectorySynchronizer)
        .DefaultNew();
    RegisterParameter("hive_manager", HiveManager)
        .DefaultNew();
    RegisterParameter("node_tracker", NodeTracker)
        .DefaultNew();
    RegisterParameter("chunk_manager", ChunkManager)
        .DefaultNew();
    RegisterParameter("object_service", ObjectService)
        .DefaultNew();
    RegisterParameter("cypress_manager", CypressManager)
        .DefaultNew();
    RegisterParameter("replicated_table_tracker", ReplicatedTableTracker)
        .DefaultNew();
    RegisterParameter("enable_timestamp_manager", EnableTimestampManager)
        .Default(true);
    RegisterParameter("timestamp_manager", TimestampManager)
        .DefaultNew();
    RegisterParameter("timestamp_provider", TimestampProvider);
    RegisterParameter("discovery_server", DiscoveryServer)
        .Default();
    RegisterParameter("transaction_supervisor", TransactionSupervisor)
        .DefaultNew();
    RegisterParameter("multicell_manager", MulticellManager)
        .DefaultNew();
    RegisterParameter("world_initializer", WorldInitializer)
        .DefaultNew();
    RegisterParameter("security_manager", SecurityManager)
        .DefaultNew();
    RegisterParameter("enable_provision_lock", EnableProvisionLock)
        .Default(true);
    RegisterParameter("bus_client", BusClient)
        .DefaultNew();
    RegisterParameter("cypress_annotations", CypressAnnotations)
        .Default(BuildYsonNodeFluently()
            .BeginMap()
            .EndMap()
        ->AsMap());
    RegisterParameter("abort_on_unrecognized_options", AbortOnUnrecognizedOptions)
        .Default(false);
    RegisterParameter("enable_networking", EnableNetworking)
        .Default(true);
    RegisterParameter("cluster_connection", ClusterConnection)
        .Optional();

    RegisterPostprocessor([&] {
        if (SecondaryMasters.size() > MaxSecondaryMasterCells) {
            THROW_ERROR_EXCEPTION("Too many secondary master cells");
        }

        auto cellId = PrimaryMaster->CellId;
        auto primaryCellTag = CellTagFromId(PrimaryMaster->CellId);
        THashSet<TCellTag> cellTags = {primaryCellTag};
        for (const auto& cellConfig : SecondaryMasters) {
            if (ReplaceCellTagInId(cellConfig->CellId, primaryCellTag) != cellId) {
                THROW_ERROR_EXCEPTION("Invalid cell id %v specified for secondary master in server configuration",
                    cellConfig->CellId);
            }
            auto cellTag = CellTagFromId(cellConfig->CellId);
            if (!cellTags.insert(cellTag).second) {
                THROW_ERROR_EXCEPTION("Duplicate cell tag %v in server configuration",
                    cellTag);
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

TDynamicClusterConfig::TDynamicClusterConfig()
{
    RegisterParameter("enable_safe_mode", EnableSafeMode)
        .Default(false);
    RegisterParameter("enable_descending_sort_order", EnableDescendingSortOrder)
        .Default(false);
    RegisterParameter("enable_descending_sort_order_dynamic", EnableDescendingSortOrderDynamic)
        .Default(false);
    RegisterParameter("chunk_manager", ChunkManager)
        .DefaultNew();
    RegisterParameter("cell_manager", CellManager)
        .DefaultNew();
    RegisterParameter("tablet_manager", TabletManager)
        .DefaultNew();
    RegisterParameter("chaos_manager", ChaosManager)
        .DefaultNew();
    RegisterParameter("node_tracker", NodeTracker)
        .DefaultNew();
    RegisterParameter("object_manager", ObjectManager)
        .DefaultNew();
    RegisterParameter("security_manager", SecurityManager)
        .DefaultNew();
    RegisterParameter("cypress_manager", CypressManager)
        .DefaultNew();
    RegisterParameter("multicell_manager", MulticellManager)
        .DefaultNew();
    RegisterParameter("transaction_manager", TransactionManager)
        .DefaultNew();
    RegisterParameter("scheduler_pool_manager", SchedulerPoolManager)
        .DefaultNew();
    RegisterParameter("cell_master", CellMaster)
        .DefaultNew();
    RegisterParameter("object_service", ObjectService)
        .DefaultNew();
    RegisterParameter("chunk_service", ChunkService)
        .DefaultNew();

    RegisterPostprocessor([&] {
        if (EnableDescendingSortOrderDynamic && !EnableDescendingSortOrder) {
            THROW_ERROR_EXCEPTION(
                "Setting enable_descending_sort_order_dynamic requires "
                "enable_descending_sort_order to be set");
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
