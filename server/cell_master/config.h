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

#include <yt/server/journal_server/config.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/election/config.h>

#include <yt/ytlib/hive/config.h>

#include <yt/ytlib/transaction_client/config.h>

#include <yt/core/rpc/config.h>

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
            .Default(TDuration::Seconds(1));
    }
};

DEFINE_REFCOUNTED_TYPE(TMulticellManagerConfig)

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

    NHive::TCellDirectoryConfigPtr CellDirectory;
    NHive::TCellDirectorySynchronizerConfigPtr CellDirectorySynchronizer;
    NHive::THiveManagerConfigPtr HiveManager;

    NNodeTrackerServer::TNodeTrackerConfigPtr NodeTracker;

    NTransactionServer::TTransactionManagerConfigPtr TransactionManager;

    NChunkServer::TChunkManagerConfigPtr ChunkManager;

    NJournalServer::TJournalManagerConfigPtr JournalManager;

    NObjectServer::TObjectManagerConfigPtr ObjectManager;

    NObjectServer::TObjectServiceConfigPtr ObjectService;

    NCypressServer::TCypressManagerConfigPtr CypressManager;

    NSecurityServer::TSecurityManagerConfigPtr SecurityManager;

    NTabletServer::TTabletManagerConfigPtr TabletManager;

    NTransactionServer::TTimestampManagerConfigPtr TimestampManager;

    NTransactionClient::TRemoteTimestampProviderConfigPtr TimestampProvider;

    NHive::TTransactionSupervisorConfigPtr TransactionSupervisor;

    TMulticellManagerConfigPtr MulticellManager;

    //! If |true| then |//sys/@provision_lock| is set during cluster initialization.
    bool EnableProvisionLock;

    TCellMasterConfig();
};

DEFINE_REFCOUNTED_TYPE(TCellMasterConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
