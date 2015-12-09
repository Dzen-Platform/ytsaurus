#pragma once

#include "public.h"

#include <yt/server/chunk_server/config.h>

#include <yt/server/cypress_server/config.h>

#include <yt/server/hive/config.h>

#include <yt/server/hydra/config.h>

#include <yt/server/misc/config.h>

#include <yt/server/node_tracker_server/config.h>

#include <yt/server/object_server/config.h>

#include <yt/server/security_server/config.h>

#include <yt/server/tablet_server/config.h>

#include <yt/server/transaction_server/config.h>

#include <yt/ytlib/election/config.h>

#include <yt/ytlib/hive/config.h>

#include <yt/ytlib/transaction_client/config.h>

#include <yt/core/rpc/config.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

class TMasterCellConfig
    : public NElection::TCellConfig
{
public:
    NObjectClient::TCellTag CellTag = 0;

    TMasterCellConfig()
    {
        RegisterParameter("cell_tag", CellTag);
    }
};

DEFINE_REFCOUNTED_TYPE(TMasterCellConfig)

class TMasterHydraManagerConfig
    : public NHydra::TDistributedHydraManagerConfig
{
public:
    int MaxSnapshotsToKeep;

    NRpc::TResponseKeeperConfigPtr ResponseKeeper;

    TMasterHydraManagerConfig()
    {
        RegisterParameter("max_snapshots_to_keep", MaxSnapshotsToKeep)
            .GreaterThanOrEqual(0)
            .Default(3);

        RegisterParameter("response_keeper", ResponseKeeper)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TMasterHydraManagerConfig)

class TCellMasterConfig
    : public TServerConfig
{
public:
    TMasterCellConfigPtr Master;

    NHydra::TFileChangelogStoreConfigPtr Changelogs;
    NHydra::TLocalSnapshotStoreConfigPtr Snapshots;
    TMasterHydraManagerConfigPtr HydraManager;

    NHive::TCellDirectoryConfigPtr CellDirectory;
    NHive::THiveManagerConfigPtr HiveManager;

    NNodeTrackerServer::TNodeTrackerConfigPtr NodeTracker;

    NTransactionServer::TTransactionManagerConfigPtr TransactionManager;

    NChunkServer::TChunkManagerConfigPtr ChunkManager;

    NObjectServer::TObjectManagerConfigPtr ObjectManager;

    NCypressServer::TCypressManagerConfigPtr CypressManager;

    NSecurityServer::TSecurityManagerConfigPtr SecurityManager;

    NTabletServer::TTabletManagerConfigPtr TabletManager;

    NTransactionServer::TTimestampManagerConfigPtr TimestampManager;

    NTransactionClient::TRemoteTimestampProviderConfigPtr TimestampProvider;

    NHive::TTransactionSupervisorConfigPtr TransactionSupervisor;

    //! RPC interface port number.
    int RpcPort;

    //! HTTP monitoring interface port number.
    int MonitoringPort;

    //! If |true| then |//sys/@provision_lock| is set during cluster initialization.
    bool EnableProvisionLock;

    TCellMasterConfig()
    {
        RegisterParameter("master", Master);
        RegisterParameter("changelogs", Changelogs);
        RegisterParameter("snapshots", Snapshots);
        RegisterParameter("hydra_manager", HydraManager)
            .DefaultNew();
        RegisterParameter("cell_directory", CellDirectory)
            .DefaultNew();
        RegisterParameter("hive_manager", HiveManager)
            .DefaultNew();
        RegisterParameter("node_tracker", NodeTracker)
            .DefaultNew();
        RegisterParameter("transaction_manager", TransactionManager)
            .DefaultNew();
        RegisterParameter("chunk_manager", ChunkManager)
            .DefaultNew();
        RegisterParameter("object_manager", ObjectManager)
            .DefaultNew();
        RegisterParameter("cypress_manager", CypressManager)
            .DefaultNew();
        RegisterParameter("security_manager", SecurityManager)
            .DefaultNew();
        RegisterParameter("tablet_manager", TabletManager)
            .DefaultNew();
        RegisterParameter("timestamp_manager", TimestampManager)
            .DefaultNew();
        RegisterParameter("timestamp_provider", TimestampProvider);
        RegisterParameter("transaction_supervisor", TransactionSupervisor)
            .DefaultNew();
        RegisterParameter("rpc_port", RpcPort)
            .Default(9000);
        RegisterParameter("monitoring_port", MonitoringPort)
            .Default(10000);
        RegisterParameter("enable_provision_lock", EnableProvisionLock)
            .Default(true);
    }
};

DEFINE_REFCOUNTED_TYPE(TCellMasterConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
