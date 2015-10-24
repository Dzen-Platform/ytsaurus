#pragma once

#include "public.h"

#include <core/rpc/config.h>

#include <ytlib/election/config.h>

#include <ytlib/hive/config.h>

#include <ytlib/transaction_client/config.h>

#include <server/hydra/config.h>

#include <server/hive/config.h>

#include <server/node_tracker_server/config.h>

#include <server/transaction_server/config.h>

#include <server/chunk_server/config.h>

#include <server/journal_server/config.h>

#include <server/cypress_server/config.h>

#include <server/object_server/config.h>

#include <server/security_server/config.h>

#include <server/tablet_server/config.h>

#include <server/misc/config.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

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

////////////////////////////////////////////////////////////////////////////////

class TMulticellManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Timeout for requests issued between masters. This applies to
    //! follower-to-leader forwarding and cross-cell interactions.
    TDuration MasterRpcTimeout;

    TDuration CellStatisticsGossipPeriod;

    TMulticellManagerConfig()
    {
        RegisterParameter("master_rpc_timeout", MasterRpcTimeout)
            .Default(TDuration::Seconds(30));

        RegisterParameter("cell_statistics_gossip_period", CellStatisticsGossipPeriod)
            .Default(TDuration::Seconds(10));
    }
};

DEFINE_REFCOUNTED_TYPE(TMulticellManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TCellMasterConfig
    : public TServerConfig
{
public:
    NElection::TCellConfigPtr PrimaryMaster;
    std::vector<NElection::TCellConfigPtr> SecondaryMasters;

    NHydra::TFileChangelogStoreConfigPtr Changelogs;
    NHydra::TLocalSnapshotStoreConfigPtr Snapshots;
    TMasterHydraManagerConfigPtr HydraManager;

    NHive::TCellDirectoryConfigPtr CellDirectory;
    NHive::TCellDirectorySynchronizerConfigPtr CellDirectorySynchronizer;
    NHive::THiveManagerConfigPtr HiveManager;

    NNodeTrackerServer::TNodeTrackerConfigPtr NodeTracker;

    NTransactionServer::TTransactionManagerConfigPtr TransactionManager;

    NChunkServer::TChunkManagerConfigPtr ChunkManager;

    NJournalServer::TJournalManagerConfigPtr JournalManager;

    NObjectServer::TObjectManagerConfigPtr ObjectManager;

    NCypressServer::TCypressManagerConfigPtr CypressManager;

    NSecurityServer::TSecurityManagerConfigPtr SecurityManager;

    NTabletServer::TTabletManagerConfigPtr TabletManager;

    NTransactionServer::TTimestampManagerConfigPtr TimestampManager;

    NTransactionClient::TRemoteTimestampProviderConfigPtr TimestampProvider;

    NHive::TTransactionSupervisorConfigPtr TransactionSupervisor;

    TMulticellManagerConfigPtr MulticellManager;

    //! RPC interface port number.
    int RpcPort;

    //! HTTP monitoring interface port number.
    int MonitoringPort;

    //! If |true| then |//sys/@provision_lock| is set during cluster initialization.
    bool EnableProvisionLock;

    TCellMasterConfig();
};

DEFINE_REFCOUNTED_TYPE(TCellMasterConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
