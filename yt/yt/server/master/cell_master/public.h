#pragma once

#include <yt/yt/core/misc/enum.h>
#include <yt/yt/core/misc/public.h>

#include <yt/yt/client/cell_master_client/public.h>

namespace NYT::NCellMaster {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TCellStatistics;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TMasterHydraManagerConfig)
DECLARE_REFCOUNTED_CLASS(TMasterConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TDiscoveryServersConfig)
DECLARE_REFCOUNTED_CLASS(TMulticellManagerConfig)
DECLARE_REFCOUNTED_CLASS(TWorldInitializerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicMulticellManagerConfig)
DECLARE_REFCOUNTED_CLASS(TCellMasterConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicCellMasterConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicClusterConfig)
DECLARE_REFCOUNTED_CLASS(TSerializationDumperConfig)

DECLARE_REFCOUNTED_CLASS(TMasterAutomaton)
DECLARE_REFCOUNTED_CLASS(TMasterAutomatonPart)
DECLARE_REFCOUNTED_CLASS(THydraFacade)
DECLARE_REFCOUNTED_CLASS(TWorldInitializer)
DECLARE_REFCOUNTED_CLASS(TIdentityManager)
DECLARE_REFCOUNTED_CLASS(TMulticellManager)
DECLARE_REFCOUNTED_CLASS(TConfigManager)
DECLARE_REFCOUNTED_CLASS(TEpochHistoryManager)
DECLARE_REFCOUNTED_CLASS(TMultiPhaseCellSyncSession);
DECLARE_REFCOUNTED_CLASS(TMultiPhaseCellSyncSession);
DECLARE_REFCOUNTED_CLASS(TDiskSpaceProfiler);

DECLARE_REFCOUNTED_STRUCT(IAlertManager)

class TBootstrap;

enum class EMasterReign;
class TLoadContext;
class TSaveContext;
using TPersistenceContext = TCustomPersistenceContext<TSaveContext, TLoadContext, EMasterReign>;

DEFINE_ENUM(EAutomatonThreadQueue,
    (Default)
    (TimestampManager)
    (ConfigManager)
    (ChunkManager)
    (CypressManager)
    (CypressService)
    (PortalManager)
    (HiveManager)
    (JournalManager)
    (NodeTracker)
    (ObjectManager)
    (SecurityManager)
    (TransactionManager)
    (MulticellManager)
    (TabletManager)
    (CypressNodeExpirationTracker)
    (ChunkExpirationTracker)
    (TabletBalancer)
    (TabletTracker)
    (TabletCellJanitor)
    (TabletGossip)
    (TabletDecommissioner)
    (SecurityGossip)
    (Periodic)
    (Mutation)
    (ChunkMaintenance)
    (ChunkLocator)
    (ChunkFetchingTraverser)
    (ChunkStatisticsTraverser)
    (ChunkRequisitionUpdateTraverser)
    (ChunkReplicaAllocator)
    (ChunkService)
    (CypressTraverser)
    (NodeTrackerService)
    (NodeTrackerGossip)
    (ObjectService)
    (TransactionSupervisor)
    (GarbageCollector)
    (JobTrackerService)
    (ReplicatedTableTracker)
    (MulticellGossip)
    (ClusterDirectorySynchronizer)
    (CellDirectorySynchronizer)
    (ResponseKeeper)
    (TamedCellManager)
    (SchedulerPoolManager)
    (RecursiveResourceUsageCache)
    (ExecNodeTracker)
    (ExecNodeTrackerService)
    (CellarNodeTracker)
    (CellarNodeTrackerService)
    (TabletNodeTracker)
    (TabletNodeTrackerService)
    (DataNodeTracker)
    (DataNodeTrackerService)
    (TableManager)
);

DEFINE_ENUM(EAutomatonThreadBucket,
    (Gossips)
);

using NCellMasterClient::EMasterCellRole;
using NCellMasterClient::EMasterCellRoles;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
