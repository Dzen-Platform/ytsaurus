#pragma once

#include <yt/yt/server/lib/hydra/public.h>

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/core/misc/small_vector.h>

#include <bitset>

namespace NYT::NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TReqRemoveNode;

using TReqRegisterNode = NNodeTrackerClient::NProto::TReqRegisterNode;
using TReqIncrementalHeartbeat = NNodeTrackerClient::NProto::TReqIncrementalHeartbeat;
using TReqFullHeartbeat = NNodeTrackerClient::NProto::TReqFullHeartbeat;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

using NNodeTrackerClient::TNodeId;
using NNodeTrackerClient::ENodeState;
using NNodeTrackerClient::InvalidNodeId;
using NNodeTrackerClient::TRackId;
using NNodeTrackerClient::TDataCenterId;
using NNodeTrackerClient::TAddressMap;
using NNodeTrackerClient::TNodeAddressMap;
using NNodeTrackerClient::TNodeDescriptor;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TNodeTracker)
DECLARE_REFCOUNTED_CLASS(TNodeDiscoveryManager)

DECLARE_REFCOUNTED_CLASS(TNodeGroupConfig)
DECLARE_REFCOUNTED_CLASS(TNodeTrackerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicNodeTrackerConfig)
DECLARE_REFCOUNTED_CLASS(TNodeDiscoveryManagerConfig)

DECLARE_REFCOUNTED_STRUCT(IExecNodeTracker)

DECLARE_ENTITY_TYPE(TNode, NObjectClient::TObjectId, ::THash<NObjectClient::TObjectId>)
DECLARE_ENTITY_TYPE(TRack, TRackId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TDataCenter, TDataCenterId, NObjectClient::TDirectObjectIdHash)

using TNodeList = SmallVector<TNode*, NChunkClient::TypicalReplicaCount>;

class TNodeDirectoryBuilder;

constexpr int MaxRackCount = 255;
// NB: +1 is because of null rack.
constexpr int RackIndexBound = MaxRackCount + 1;
constexpr int NullRackIndex = 0;
using TRackSet = std::bitset<RackIndexBound>;

constexpr int MaxDataCenterCount = 16;
constexpr int NullDataCenterIndex = 0;
// NB: +1 is because of null dataCenter.
using TDataCenterSet = std::bitset<MaxDataCenterCount + 1>;

constexpr int TypicalInterDCEdgeCount = 9; // (2 DCs + null DC)^2
static_assert(
    TypicalInterDCEdgeCount <= NNodeTrackerServer::MaxDataCenterCount * NNodeTrackerServer::MaxDataCenterCount,
    "TypicalInterDCEdgeCount is too large.");

////////////////////////////////////////////////////////////////////////////////

// COMPAT(savrus) Keep in sync with ENodeFlavor until 21.1 prevails.
DEFINE_ENUM(ENodeHeartbeatType,
    ((Cluster)      (0))
    ((Data)         (1))
    ((Exec)         (2))
    ((Tablet)       (3))
    ((Cellar)       (4))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNodeTrackerServer
