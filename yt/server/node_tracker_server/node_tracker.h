#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/hydra/entity_map.h>

#include <yt/ytlib/hive/cell_directory.h>

#include <yt/ytlib/hydra/public.h>

#include <yt/ytlib/node_tracker_client/node_statistics.h>

#include <yt/core/actions/signal.h>

#include <yt/core/rpc/service_detail.h>

namespace NYT {
namespace NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

class TNodeTracker
    : public TRefCounted
{
public:
    TNodeTracker(
        TNodeTrackerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);
    ~TNodeTracker();

    void Initialize();

    using TCtxRegisterNode = NRpc::TTypedServiceContext<
        NNodeTrackerClient::NProto::TReqRegisterNode,
        NNodeTrackerClient::NProto::TRspRegisterNode>;
    using TCtxRegisterNodePtr = TIntrusivePtr<TCtxRegisterNode>;
    void ProcessRegisterNode(TCtxRegisterNodePtr context);

    typedef NRpc::TTypedServiceContext<
        NNodeTrackerClient::NProto::TReqFullHeartbeat,
        NNodeTrackerClient::NProto::TRspFullHeartbeat> TCtxFullHeartbeat;
    using TCtxFullHeartbeatPtr = TIntrusivePtr<TCtxFullHeartbeat>;
    void ProcessFullHeartbeat(TCtxFullHeartbeatPtr context);

    using TCtxIncrementalHeartbeat = NRpc::TTypedServiceContext<
        NNodeTrackerClient::NProto::TReqIncrementalHeartbeat,
        NNodeTrackerClient::NProto::TRspIncrementalHeartbeat>;
    using TCtxIncrementalHeartbeatPtr = TIntrusivePtr<TCtxIncrementalHeartbeat>;
    void ProcessIncrementalHeartbeat(TCtxIncrementalHeartbeatPtr context);


    DECLARE_ENTITY_MAP_ACCESSORS(Node, TNode);
    DECLARE_ENTITY_MAP_ACCESSORS(Rack, TRack);


    //! Fired when a node gets registered.
    DECLARE_SIGNAL(void(TNode* node), NodeRegistered);
    
    //! Fired when a node gets unregistered.
    DECLARE_SIGNAL(void(TNode* node), NodeUnregistered);

    //! Fired when a node gets disposed (after being unregistered).
    DECLARE_SIGNAL(void(TNode* node), NodeDisposed);

    //! Fired when node "banned" flag changes.
    DECLARE_SIGNAL(void(TNode* node), NodeBanChanged);

    //! Fired when node "decommissioned" flag changes.
    DECLARE_SIGNAL(void(TNode* node), NodeDecommissionChanged);

    //! Fired when node rack changes.
    DECLARE_SIGNAL(void(TNode* node), NodeRackChanged);

    //! Fired when a full heartbeat is received from a node.
    DECLARE_SIGNAL(void(
        TNode* node,
        NNodeTrackerClient::NProto::TReqFullHeartbeat* request),
        FullHeartbeat);

    //! Fired when an incremental heartbeat is received from a node.
    DECLARE_SIGNAL(void(
        TNode* node,
        NNodeTrackerClient::NProto::TReqIncrementalHeartbeat* request,
        NNodeTrackerClient::NProto::TRspIncrementalHeartbeat* response),
        IncrementalHeartbeat);


    //! Returns a node with a given id (|nullptr| if none).
    TNode* FindNode(TNodeId id);

    //! Returns a node with a given id (fails if none).
    TNode* GetNode(TNodeId id);

    //! Returns a node with a given id (throws if none).
    TNode* GetNodeOrThrow(TNodeId id);

    //! Returns a node registered at the given address (|nullptr| if none).
    TNode* FindNodeByAddress(const Stroka& address);

    //! Returns a node registered at the given address (fails if none).
    TNode* GetNodeByAddress(const Stroka& address);

    //! Returns a node registered at the given address (throws if none).
    TNode* GetNodeByAddressOrThrow(const Stroka& address);

    //! Returns an arbitrary node registered at the host (|nullptr| if none).
    TNode* FindNodeByHostName(const Stroka& hostName);

    //! Returns the list of all nodes belonging to a given rack.
    /*!
     *  #rack can be |nullptr|.
     */
    std::vector<TNode*> GetRackNodes(const TRack* rack);


    //! Sets the "banned" flag and notifies the subscribers.
    void SetNodeBanned(TNode* node, bool value);

    //! Sets the "decommissioned" flag and notifies the subscribers.
    void SetNodeDecommissioned(TNode* node, bool value);

    //! Sets the rack and notifies the subscribers.
    void SetNodeRack(TNode* node, TRack* rack);

    //! Sets the user tags for the node.
    void SetNodeUserTags(TNode* node, const std::vector<Stroka>& tags);


    //! Creates a new rack with a given name. Throws on name conflict.
    TRack* CreateRack(const Stroka& name);

    //! Destroys an existing rack.
    void DestroyRack(TRack* rack);

    //! Renames an existing racks. Throws on name conflict.
    void RenameRack(TRack* rack, const Stroka& newName);

    //! Returns a rack with a given name (|nullptr| if none).
    TRack* FindRackByName(const Stroka& name);

    //! Returns a rack with a given name (throws if none).
    TRack* GetRackByNameOrThrow(const Stroka& name);


    //! Returns the total cluster statistics, aggregated over all nodes.
    NNodeTrackerClient::TTotalNodeStatistics GetTotalNodeStatistics();

    //! Returns the number of nodes with ENodeState::Online aggregated state.
    int GetOnlineNodeCount();

private:
    class TImpl;
    class TClusterNodeTypeHandler;
    class TRackTypeHandler;

    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TNodeTracker)

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
