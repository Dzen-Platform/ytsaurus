#pragma once

#include <yp/server/misc/public.h>

#include <yp/server/master/public.h>

#include <yp/server/objects/public.h>

#include <array>

namespace NYP::NServer::NScheduler {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TCluster)
class TObject;
class TNode;
class TTopologyZone;
class TPod;
class TPodSet;
class TNodeSegment;
class TAccount;
class TInternetAddress;
class TNetworkModule;
class TResource;

template <class T>
class TLabelFilterCache;

class TScheduleQueue;
class TAllocationPlan;

struct TAllocationStatistics;

DECLARE_REFCOUNTED_STRUCT(IGlobalResourceAllocator)

DECLARE_REFCOUNTED_CLASS(TResourceManager)
DECLARE_REFCOUNTED_CLASS(TScheduler)

DECLARE_REFCOUNTED_CLASS(TEveryNodeSelectionStrategyConfig)
DECLARE_REFCOUNTED_CLASS(TPodNodeScoreConfig)
DECLARE_REFCOUNTED_CLASS(TGlobalResourceAllocatorConfig)
DECLARE_REFCOUNTED_CLASS(TSchedulerConfig)

constexpr size_t MaxResourceDimensions = 3;
using TResourceCapacities = std::array<ui64, MaxResourceDimensions>;

using NObjects::TObjectId;
using NObjects::EResourceKind;

extern const TString TopologyLabel;

DECLARE_REFCOUNTED_STRUCT(IPodNodeScore)

DEFINE_ENUM(EPodNodeScoreType,
    (NodeRandomHash)
    (FreeCpuMemoryShareVariance)
    (FreeCpuMemoryShareSquaredMinDelta)
);

using TPodNodeScoreValue = double;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NScheduler
