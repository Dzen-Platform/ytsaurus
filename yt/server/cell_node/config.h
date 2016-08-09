#pragma once

#include "public.h"

#include <yt/server/exec_agent/config.h>

#include <yt/server/misc/config.h>

#include <yt/server/object_server/config.h>

#include <yt/server/query_agent/config.h>

#include <yt/server/tablet_node/config.h>

#include <yt/server/hive/config.h>

#include <yt/ytlib/api/config.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/concurrency/config.h>

namespace NYT {
namespace NCellNode {

////////////////////////////////////////////////////////////////////////////////

class TResourceLimitsConfig
    : public NYTree::TYsonSerializable
{
public:
    i64 Memory;

    TResourceLimitsConfig()
    {
        // Very low default, override for production use.
        RegisterParameter("memory", Memory)
            .GreaterThanOrEqual(0)
            .Default((i64) 5 * 1024 * 1024 * 1024);
    }
};

DEFINE_REFCOUNTED_TYPE(TResourceLimitsConfig)

////////////////////////////////////////////////////////////////////////////////

class TBatchingChunkServiceConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration MaxBatchDelay;
    int MaxBatchCost;
    NConcurrency::TThroughputThrottlerConfigPtr CostThrottler;

    TBatchingChunkServiceConfig()
    {
        RegisterParameter("max_batch_delay", MaxBatchDelay)
            .Default(TDuration::Zero());
        RegisterParameter("max_batch_cost", MaxBatchCost)
            .Default(1000);
        RegisterParameter("cost_throttler", CostThrottler)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TBatchingChunkServiceConfig)

////////////////////////////////////////////////////////////////////////////////

class TCellNodeConfig
    : public TServerConfig
{
public:
    //! Interval between Orchid cache rebuilds.
    TDuration OrchidCacheUpdatePeriod;

    //! Node-to-master connection.
    NApi::TConnectionConfigPtr ClusterConnection;

    //! Cell directory synchronization.
    NHive::TCellDirectorySynchronizerConfigPtr CellDirectorySynchronizer;

    //! Data node configuration part.
    NDataNode::TDataNodeConfigPtr DataNode;

    //! Exec node configuration part.
    NExecAgent::TExecAgentConfigPtr ExecAgent;

    //! Tablet node configuration part.
    NTabletNode::TTabletNodeConfigPtr TabletNode;

    //! Query node configuration part.
    NQueryAgent::TQueryAgentConfigPtr QueryAgent;

    //! Metadata cache service configuration.
    NObjectServer::TMasterCacheServiceConfigPtr MasterCacheService;

    //! Chunk Service batcher and redirector.
    TBatchingChunkServiceConfigPtr BatchingChunkService;

    //! Known node addresses.
    NNodeTrackerClient::TAddressList Addresses;

    //! A set of tags to be assigned to this node.
    /*!
     * These tags are merged with others (e.g. provided by user and provided by master) to form
     * the full set of tags.
     */
    std::vector<Stroka> Tags;

    //! Limits for the node process and all jobs controlled by it.
    TResourceLimitsConfigPtr ResourceLimits;

    TCellNodeConfig()
    {
        RegisterParameter("orchid_cache_update_period", OrchidCacheUpdatePeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("cluster_connection", ClusterConnection);
        RegisterParameter("cell_directory_synchronizer", CellDirectorySynchronizer)
            .DefaultNew();
        RegisterParameter("data_node", DataNode)
            .DefaultNew();
        RegisterParameter("exec_agent", ExecAgent)
            .DefaultNew();
        RegisterParameter("tablet_node", TabletNode)
            .DefaultNew();
        RegisterParameter("query_agent", QueryAgent)
            .DefaultNew();
        RegisterParameter("master_cache_service", MasterCacheService)
            .DefaultNew();
        RegisterParameter("batching_chunk_service", BatchingChunkService)
            .DefaultNew();
        RegisterParameter("addresses", Addresses)
            .Default();
        RegisterParameter("tags", Tags)
            .Default();
        RegisterParameter("resource_limits", ResourceLimits)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TCellNodeConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellNode
} // namespace NYT
