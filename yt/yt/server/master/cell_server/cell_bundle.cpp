#include "config.h"
#include "private.h"
#include "cell_bundle.h"
#include "cell_base.h"
#include "yt/server/master/tablet_server/private.h"

#include <yt/server/master/tablet_server/tablet_action.h>
#include <yt/server/master/tablet_server/tablet_cell.h>
#include <yt/server/master/tablet_server/tablet_cell_bundle.h>
#include <yt/server/master/tablet_server/config.h>

#include <yt/ytlib/tablet_client/config.h>

#include <yt/core/profiling/profile_manager.h>

namespace NYT::NCellServer {

using namespace NCellMaster;
using namespace NObjectServer;
using namespace NTabletClient;
using namespace NTabletServer;
using namespace NChunkClient;
using namespace NYson;
using namespace NYTree;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

TCellBundle::TCellBundle(TCellBundleId id)
    : TNonversionedObjectBase(id)
    , Acd_(this)
    , Options_(New<TTabletCellOptions>())
    , CellBalancerConfig_(New<TCellBalancerConfig>())
    , Health_(ETabletCellHealth::Failed)
    , DynamicOptions_(New<TDynamicTabletCellOptions>())
{ }

TString TCellBundle::GetLowercaseObjectName() const
{
    return Format("cell bundle %Qv", Name_);
}

TString TCellBundle::GetCapitalizedObjectName() const
{
    return Format("Cell bundle %Qv", Name_);
}

void TCellBundle::Save(TSaveContext& context) const
{
    TNonversionedObjectBase::Save(context);

    using NYT::Save;
    Save(context, Name_);
    Save(context, Acd_);
    Save(context, *Options_);
    Save(context, *DynamicOptions_);
    Save(context, DynamicConfigVersion_);
    Save(context, NodeTagFilter_);
    Save(context, *CellBalancerConfig_);
    Save(context, Health_);
}

void TCellBundle::Load(TLoadContext& context)
{
    TNonversionedObjectBase::Load(context);

    using NYT::Load;
    Load(context, Name_);
    Load(context, Acd_);
    Load(context, *Options_);
    Load(context, *DynamicOptions_);
    Load(context, DynamicConfigVersion_);
    Load(context, NodeTagFilter_);
    Load(context, *CellBalancerConfig_);
    Load(context, Health_);

    InitializeProfilingCounters();
}

void TCellBundle::SetName(TString name)
{
    Name_ = name;
    InitializeProfilingCounters();
}

TString TCellBundle::GetName() const
{
    return Name_;
}

TDynamicTabletCellOptionsPtr TCellBundle::GetDynamicOptions() const
{
    return DynamicOptions_;
}

void TCellBundle::SetDynamicOptions(TDynamicTabletCellOptionsPtr dynamicOptions)
{
    DynamicOptions_ = std::move(dynamicOptions);
    ++DynamicConfigVersion_;
}

void TCellBundle::InitializeProfilingCounters()
{
    auto profiler = TabletServerProfiler
        .WithTag("tablet_cell_bundle", Name_);

    ProfilingCounters_.Profiler = profiler;
    ProfilingCounters_.TabletCellCount = profiler.WithSparse().Gauge("/tablet_cell_count");
    ProfilingCounters_.ReplicaSwitch = profiler.Counter("/switch_tablet_replica_mode_count");
    ProfilingCounters_.InMemoryMoves = profiler.Counter("/in_memory_moves_count");
    ProfilingCounters_.ExtMemoryMoves = profiler.Counter("/ext_memory_moves_count");
    ProfilingCounters_.TabletMerges = profiler.Counter("/tablet_merges_count");
    ProfilingCounters_.TabletCellMoves = profiler.Counter("/tablet_cell_moves");

    ProfilingCounters_.PeerAssignment = profiler.Counter("/peer_assignment");
}

TCounter& TCellBundleProfilingCounters::GetLeaderReassignment(const TString& reason)
{
    auto it = LeaderReassignment.find(reason);
    if (it == LeaderReassignment.end()) {
        it = LeaderReassignment.emplace(
            reason,
            Profiler.WithTag("reason", reason).Counter("/leader_reassignment")).first;
    }
    return it->second;
}

TCounter& TCellBundleProfilingCounters::GetPeerRevocation(const TString& reason)
{
    auto it = PeerRevocation.find(reason);
    if (it == PeerRevocation.end()) {
        it = PeerRevocation.emplace(
            reason,
            Profiler.WithTag("reason", reason).Counter("/peer_revocation")).first;
    }
    return it->second;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellServer
