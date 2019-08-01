#pragma once

#include <yt/core/misc/enum.h>
#include <yt/core/misc/public.h>

namespace NYT::NCellMaster {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TCellStatistics;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TMasterHydraManagerConfig)
DECLARE_REFCOUNTED_CLASS(TMasterConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TMulticellManagerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicMulticellManagerConfig)
DECLARE_REFCOUNTED_CLASS(TCellMasterConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicClusterConfig)

DECLARE_REFCOUNTED_CLASS(TMasterAutomaton)
DECLARE_REFCOUNTED_CLASS(TMasterAutomatonPart)
DECLARE_REFCOUNTED_CLASS(THydraFacade)
DECLARE_REFCOUNTED_CLASS(TWorldInitializer)
DECLARE_REFCOUNTED_CLASS(TIdentityManager)
DECLARE_REFCOUNTED_CLASS(TMulticellManager)
DECLARE_REFCOUNTED_CLASS(TConfigManager)
DECLARE_REFCOUNTED_CLASS(TAnnotationSetter)

class TBootstrap;

enum class EMasterSnapshotVersion;
class TLoadContext;
class TSaveContext;
using TPersistenceContext = TCustomPersistenceContext<TSaveContext, TLoadContext, EMasterSnapshotVersion>;

DEFINE_ENUM(EAutomatonThreadQueue,
    (Default)
    (TimestampManager)
    (ConfigManager)
    (ChunkManager)
    (CypressManager)
    (PortalManager)
    (HiveManager)
    (JournalManager)
    (NodeTracker)
    (ObjectManager)
    (SecurityManager)
    (TransactionManager)
    (MulticellManager)
    (TabletManager)
    (NodeExpirationTracker)
    (ChunkExpirationTracker)
    (TabletBalancer)
    (TabletTracker)
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
    (ObjectService)
    (TransactionSupervisor)
    (GarbageCollector)
    (JobTrackerService)
    (ReplicatedTableTracker)
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
