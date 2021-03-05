#pragma once

#include "public.h"

#include <yt/yt/ytlib/node_tracker_client/proto/node_tracker_service.pb.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/core/actions/signal.h>

namespace NYT::NClusterNode {

////////////////////////////////////////////////////////////////////////////////

//! Mediates connection between a cluster node and its master.
/*!
*  \note
*  Thread affinity: Control
*/
struct IMasterConnector
    : public TRefCounted
{
    //! Raised with each heartbeat.
    //! Subscribers may provide additional dynamic alerts to be reported to master.
    DECLARE_INTERFACE_SIGNAL(void(std::vector<TError>* alerts), PopulateAlerts);

    //! Raised when node successfully connects and registers at the primary master.
    DECLARE_INTERFACE_SIGNAL(void(NNodeTrackerClient::TNodeId nodeId), MasterConnected);

    //! Raised when node disconnects from masters.
    DECLARE_INTERFACE_SIGNAL(void(), MasterDisconnected);

    //! Initializes master connector.
    virtual void Initialize() = 0;

    //! Starts communication with master.
    virtual void Start() = 0;

    //! Returns cluster node master heartbeat request. This function is used only for compatibility
    //! with legacy master connector and will be removed after switching to new heartbeats.
    virtual NNodeTrackerClient::NProto::TReqHeartbeat GetHeartbeatRequest() = 0;

    //! Processes cluster node master heartbeat response. This function is used only for compatibility
    //! with legacy master connector and will be removed after switching to new heartbeats.
    virtual void OnHeartbeatResponse(const NNodeTrackerClient::NProto::TRspHeartbeat& response) = 0;

    //! Adds a given message to the list of alerts sent to master with each heartbeat.
    /*!
    *  \note
    *  Thread affinity: any
    */
    virtual void RegisterStaticAlert(const TError& alert) = 0;

    //! Returns a dynamically updated node descriptor.
    /*!
    *  \note
    *  Thread affinity: any
    */
    virtual NNodeTrackerClient::TNodeDescriptor GetLocalDescriptor() const = 0;

    //! Returns invoker that becomes cancelled on master unregistration.
    virtual const IInvokerPtr& GetMasterConnectionInvoker() const = 0;

    // Called by master connectors after fatal error during communication with master.
    // It resets master connector state and re-registers node at master.
    virtual void ResetAndRegisterAtMaster(bool firstTime = false) = 0;

    //! Returns the channel used for communication with a leader of a given cell.
    /*!
     *  This channel is neither authenticated nor retrying.
     */
    virtual NRpc::IChannelPtr GetMasterChannel(NObjectClient::TCellTag cellTag) = 0;

    //! Returns |True| iff node is currently connected to master.
    /*!
    *  \note
    *  Thread affinity: any
    */
    virtual bool IsConnected() const = 0;

    //! Returns the node id assigned by master or |InvalidNodeId| if the node
    //! is not registered.
    /*!
    *  \note
    *  Thread affinity: any
    */
    virtual NNodeTrackerClient::TNodeId GetNodeId() const = 0;

    //! Returns |True| if new heartbeats are used for communication with master.
    /*!
    *  \note
    *  Thread affinity: any
    */
    virtual bool UseNewHeartbeats() const = 0;

    //! Returns list of all master cell tags (including the primary).
    /*!
    *  \note
    *  Thread affinity: any
    */
    virtual const NObjectClient::TCellTagList& GetMasterCellTags() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IMasterConnector)

////////////////////////////////////////////////////////////////////////////////

IMasterConnectorPtr CreateMasterConnector(
    TBootstrap* bootstrap,
    const NNodeTrackerClient::TAddressMap& rpcAddresses,
    const NNodeTrackerClient::TAddressMap& skynetHttpAddresses,
    const NNodeTrackerClient::TAddressMap& monitoringHttpAddresses,
    const std::vector<TString>& nodeTags);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClusterNode
