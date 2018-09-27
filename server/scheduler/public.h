#pragma once

#include <yp/server/misc/public.h>

#include <yp/server/master/public.h>

#include <yp/server/objects/public.h>

#include <array>

namespace NYP {
namespace NServer {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TCluster)
class TObject;
class TNode;
class TTopologyZone;
class TPod;
class TPodSet;
class TNodeSegment;
class TInternetAddress;
class TAccount;

template <class T>
class TLabelFilterCache;

class TScheduleQueue;
class TAllocationPlan;

struct TAllocationStatistics;

DECLARE_REFCOUNTED_CLASS(TResourceManager)
DECLARE_REFCOUNTED_CLASS(TScheduler)

DECLARE_REFCOUNTED_CLASS(TSchedulerConfig)

constexpr size_t MaxResourceDimensions = 2;
using TResourceCapacities = std::array<ui64, MaxResourceDimensions>;

using NObjects::TObjectId;
using NObjects::EResourceKind;

extern const TString TopologyLabel;

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NServer
} // namespace NYP
