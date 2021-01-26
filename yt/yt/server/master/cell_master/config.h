#pragma once

#include "public.h"

#include <yt/server/master/chunk_server/config.h>

#include <yt/server/master/cypress_server/config.h>

#include <yt/server/lib/hive/config.h>

#include <yt/server/lib/hydra/config.h>

#include <yt/server/lib/misc/config.h>

#include <yt/server/master/node_tracker_server/config.h>

#include <yt/server/master/object_server/config.h>

#include <yt/server/master/security_server/config.h>

#include <yt/server/master/tablet_server/config.h>

#include <yt/server/master/transaction_server/config.h>

#include <yt/server/master/journal_server/config.h>

#include <yt/server/lib/timestamp_server/config.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/ytlib/election/config.h>

#include <yt/ytlib/hive/config.h>

#include <yt/ytlib/transaction_client/config.h>

#include <yt/core/bus/tcp/config.h>

#include <yt/core/rpc/config.h>

namespace NYT::NCellMaster {

////////////////////////////////////////////////////////////////////////////////

class TMasterHydraManagerConfig
    : public NHydra::TDistributedHydraManagerConfig
    , public NHydra::TLocalHydraJanitorConfig
{
public:
    NRpc::TResponseKeeperConfigPtr ResponseKeeper;

    TMasterHydraManagerConfig()
    {
        RegisterParameter("response_keeper", ResponseKeeper)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TMasterHydraManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TMasterConnectionConfig
    : public NRpc::TRetryingChannelConfig
{
public:
    //! Timeout for RPC requests to masters.
    TDuration RpcTimeout;

    TMasterConnectionConfig()
    {
        RegisterParameter("rpc_timeout", RpcTimeout)
            .Default(TDuration::Seconds(30));

        RegisterPreprocessor([&] {
            RetryAttempts = 100;
            RetryTimeout = TDuration::Minutes(3);
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TMasterConnectionConfig)

////////////////////////////////////////////////////////////////////////////////

class TMulticellManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Applies to follower-to-leader forwarding and cross-cell interactions.
    TMasterConnectionConfigPtr MasterConnection;

    //! Maximum time to wait before syncing with upstream cells.
    TDuration UpstreamSyncDelay;

    TMulticellManagerConfig()
    {
        RegisterParameter("master_connection", MasterConnection)
            .DefaultNew();
        RegisterParameter("upstream_sync_delay", UpstreamSyncDelay)
            .Default(TDuration::MilliSeconds(10));
    }
};

DEFINE_REFCOUNTED_TYPE(TMulticellManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TWorldInitializerConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration InitRetryPeriod;

    TDuration InitTransactionTimeout;

    TDuration UpdatePeriod;

    TWorldInitializerConfig()
    {
        RegisterParameter("init_retry_period", InitRetryPeriod)
            .Default(TDuration::Seconds(3));
        RegisterParameter("init_transaction_timeout", InitTransactionTimeout)
            .Default(TDuration::Seconds(60));
        RegisterParameter("update_period", UpdatePeriod)
            .Default(TDuration::Minutes(5));
    }
};

DEFINE_REFCOUNTED_TYPE(TWorldInitializerConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicMulticellManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration CellStatisticsGossipPeriod;
    THashMap<NObjectServer::TCellTag, EMasterCellRoles> CellRoles;
    THashMap<NObjectServer::TCellTag, TString> CellNames;

    TDynamicMulticellManagerConfig()
    {
        RegisterParameter("cell_statistics_gossip_period", CellStatisticsGossipPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("cell_roles", CellRoles)
            .Default();
        RegisterParameter("cell_names", CellNames)
            .Default();

        RegisterPostprocessor([&] () {
            for (const auto& [cellTag, cellRoles] : CellRoles) {
                if (None(cellRoles)) {
                    THROW_ERROR_EXCEPTION("Cell %v has no roles",
                        cellTag);
                }
            }

            THashMap<TString, NObjectServer::TCellTag> nameToCellTag;
            for (const auto& [cellTag, cellName] : CellNames) {
                NObjectClient::TCellTag cellTagCellName;
                if (TryFromString(cellName, cellTagCellName)) {
                    THROW_ERROR_EXCEPTION("Invalid cell name %Qv",
                        cellName);
                }
                auto [it, inserted] = nameToCellTag.emplace(cellName, cellTag);
                if (!inserted) {
                    THROW_ERROR_EXCEPTION("Duplicate cell name %Qv for cell tags %v and %v",
                        cellName,
                        cellTag,
                        it->second);
                }
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicMulticellManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TCellMasterConfig
    : public TServerConfig
{
public:
    NNodeTrackerClient::TNetworkPreferenceList Networks;

    NElection::TCellConfigPtr PrimaryMaster;
    std::vector<NElection::TCellConfigPtr> SecondaryMasters;

    NElection::TDistributedElectionManagerConfigPtr ElectionManager;

    NHydra::TFileChangelogStoreConfigPtr Changelogs;
    NHydra::TLocalSnapshotStoreConfigPtr Snapshots;
    TMasterHydraManagerConfigPtr HydraManager;

    NHiveClient::TCellDirectoryConfigPtr CellDirectory;
    NHiveServer::TCellDirectorySynchronizerConfigPtr CellDirectorySynchronizer;
    NHiveServer::THiveManagerConfigPtr HiveManager;

    NNodeTrackerServer::TNodeTrackerConfigPtr NodeTracker;

    NChunkServer::TChunkManagerConfigPtr ChunkManager;

    NObjectServer::TObjectServiceConfigPtr ObjectService;

    NTabletServer::TTabletManagerConfigPtr TabletManager;

    NCypressServer::TCypressManagerConfigPtr CypressManager;

    NTabletServer::TReplicatedTableTrackerConfigPtr ReplicatedTableTracker;

    bool EnableTimestampManager;
    NTimestampServer::TTimestampManagerConfigPtr TimestampManager;

    NTransactionClient::TRemoteTimestampProviderConfigPtr TimestampProvider;

    NHiveServer::TTransactionSupervisorConfigPtr TransactionSupervisor;

    TMulticellManagerConfigPtr MulticellManager;

    TWorldInitializerConfigPtr WorldInitializer;

    NSecurityServer::TSecurityManagerConfigPtr SecurityManager;

    //! If |true| then |//sys/@provision_lock| is set during cluster initialization.
    bool EnableProvisionLock;

    NBus::TTcpBusConfigPtr BusClient;

    NYTree::IMapNodePtr CypressAnnotations;

    bool AbortOnUnrecognizedOptions;

    bool EnableNetworking;

    TCellMasterConfig();
};

DEFINE_REFCOUNTED_TYPE(TCellMasterConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicCellMasterConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration MutationTimeCommitPeriod;

    TDynamicCellMasterConfig()
    {
        RegisterParameter("mutation_time_commit_period", MutationTimeCommitPeriod)
            .Default(TDuration::Minutes(10));
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicCellMasterConfig)

////////////////////////////////////////////////////////////////////////////////

class TDynamicClusterConfig
    : public NYTree::TYsonSerializable
{
public:
    bool EnableSafeMode;
    bool EnableDescendingSortOrder;
    bool EnableDescendingSortOrderDynamic;
    NChunkServer::TDynamicChunkManagerConfigPtr ChunkManager;
    NTabletServer::TDynamicTabletManagerConfigPtr TabletManager;
    NNodeTrackerServer::TDynamicNodeTrackerConfigPtr NodeTracker;
    NObjectServer::TDynamicObjectManagerConfigPtr ObjectManager;
    NSecurityServer::TDynamicSecurityManagerConfigPtr SecurityManager;
    NCypressServer::TDynamicCypressManagerConfigPtr CypressManager;
    TDynamicMulticellManagerConfigPtr MulticellManager;
    NTransactionServer::TDynamicTransactionManagerConfigPtr TransactionManager;
    TDynamicCellMasterConfigPtr CellMaster;
    NObjectServer::TDynamicObjectServiceConfigPtr ObjectService;
    NChunkServer::TDynamicChunkServiceConfigPtr ChunkService;

    TDynamicClusterConfig();
};

DEFINE_REFCOUNTED_TYPE(TDynamicClusterConfig)

////////////////////////////////////////////////////////////////////////////////

class TSerializationDumperConfig
    : public NYTree::TYsonSerializable
{
public:
    i64 LowerLimit;
    i64 UpperLimit;

    TSerializationDumperConfig()
    {
        RegisterParameter("lower_limit", LowerLimit)
            .GreaterThanOrEqual(0)
            .Default(0);
        RegisterParameter("upper_limit", UpperLimit)
            .GreaterThanOrEqual(0)
            .Default(std::numeric_limits<i64>::max());

        RegisterPostprocessor([&] () {
            if (LowerLimit >= UpperLimit) {
                THROW_ERROR_EXCEPTION("'UpperLimit' must be greater than 'LowerLimit'")
                    << TErrorAttribute("actual_lower_limit", LowerLimit)
                    << TErrorAttribute("actual_upper_limit", UpperLimit);
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TSerializationDumperConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
