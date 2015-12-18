#pragma once

#include <yt/core/misc/enum.h>
#include <yt/core/misc/public.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TCellStatistics;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TMasterHydraManagerConfig)
DECLARE_REFCOUNTED_CLASS(TMulticellManagerConfig)
DECLARE_REFCOUNTED_CLASS(TCellMasterConfig)

DECLARE_REFCOUNTED_CLASS(TMasterAutomaton)
DECLARE_REFCOUNTED_CLASS(TMasterAutomatonPart)
DECLARE_REFCOUNTED_CLASS(THydraFacade)
DECLARE_REFCOUNTED_CLASS(TWorldInitializer)
DECLARE_REFCOUNTED_CLASS(TIdentityManager)
DECLARE_REFCOUNTED_CLASS(TMulticellManager)

class TBootstrap;

class TLoadContext;
class TSaveContext;
using TPersistenceContext = TCustomPersistenceContext<TSaveContext, TLoadContext>;

DEFINE_ENUM(EAutomatonThreadQueue,
    (Default)
    (Mutation)
    (ChunkMaintenance)
    (ChunkLocator)
    (ChunkTraverser)
    (FullHeartbeat)
    (IncrementalHeartbeat)
    (RpcService)
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
