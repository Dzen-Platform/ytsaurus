#include "stdafx.h"
#include "node_tracker_service.h"
#include "node.h"
#include "node_tracker.h"
#include "private.h"
#include "config.h"

#include <ytlib/node_tracker_client/node_tracker_service_proxy.h>

#include <server/hydra/rpc_helpers.h>

#include <server/object_server/object_manager.h>

#include <server/chunk_server/chunk_manager.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/hydra_facade.h>
#include <server/cell_master/master_hydra_service.h>
#include <server/cell_master/world_initializer.h>

namespace NYT {
namespace NNodeTrackerServer {

using namespace NHydra;
using namespace NCellMaster;
using namespace NNodeTrackerClient;
using namespace NChunkServer;

using NNodeTrackerClient::NProto::TChunkAddInfo;
using NNodeTrackerClient::NProto::TChunkRemoveInfo;

////////////////////////////////////////////////////////////////////////////////

class TNodeTrackerService
    : public NCellMaster::TMasterHydraServiceBase
{
public:
    explicit TNodeTrackerService(
        TNodeTrackerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap)
        : TMasterHydraServiceBase(
            bootstrap,
            TNodeTrackerServiceProxy::GetServiceName(),
            NodeTrackerServerLogger,
            TNodeTrackerServiceProxy::GetProtocolVersion())
        , Config_(config)
        {
            RegisterMethod(RPC_SERVICE_METHOD_DESC(RegisterNode));
            RegisterMethod(RPC_SERVICE_METHOD_DESC(FullHeartbeat)
                .SetRequestHeavy(true)
                .SetInvoker(bootstrap->GetHydraFacade()->GetGuardedAutomatonInvoker(EAutomatonThreadQueue::Heartbeat)));
            RegisterMethod(RPC_SERVICE_METHOD_DESC(IncrementalHeartbeat)
                .SetRequestHeavy(true));
        }

private:
    const TNodeTrackerConfigPtr Config_;


    DECLARE_RPC_SERVICE_METHOD(NNodeTrackerClient::NProto, RegisterNode)
    {
        UNUSED(response);

        ValidateActiveLeader();

        auto worldInitializer = Bootstrap_->GetWorldInitializer();
        if (worldInitializer->CheckProvisionLock()) {
            THROW_ERROR_EXCEPTION(
                "Provision lock is found, which indicates a fresh instance of masters being run. "
                "If this is not intended then please check snapshot/changelog directories location. "
                "Ignoring this warning and removing the lock may cause UNRECOVERABLE DATA LOSS! "
                "If you are sure and wish to continue then run 'yt remove //sys/@provision_lock'");
        }

        auto addresses = FromProto<TAddressMap>(request->addresses());
        const auto& address = GetDefaultAddress(addresses);
        const auto& statistics = request->statistics();

        context->SetRequestInfo("Address: %v, %v",
            address,
            statistics);

        auto nodeTracker = Bootstrap_->GetNodeTracker();
        auto config = nodeTracker->FindNodeConfigByAddress(address);
        if (config && config->Banned) {
            THROW_ERROR_EXCEPTION("Node %v is banned", address);
        }

        if (!nodeTracker->TryAcquireNodeRegistrationSemaphore()) {
            context->Reply(TError(
                NRpc::EErrorCode::Unavailable,
                "Node registration throttling is active"));
            return;
        }

        nodeTracker
            ->CreateRegisterNodeMutation(*request)
            ->Commit()
            .Subscribe(CreateRpcResponseHandler(context));
    }

    DECLARE_RPC_SERVICE_METHOD(NNodeTrackerClient::NProto, FullHeartbeat)
    {
        ValidateActiveLeader();

        auto nodeId = request->node_id();
        const auto& statistics = request->statistics();

        auto nodeTracker = Bootstrap_->GetNodeTracker();
        auto* node = nodeTracker->GetNodeOrThrow(nodeId);

        context->SetRequestInfo("NodeId: %v, Address: %v, %v",
            nodeId,
            node->GetDefaultAddress(),
            statistics);

        if (node->GetState() != ENodeState::Registered) {
            context->Reply(TError(
                NNodeTrackerClient::EErrorCode::InvalidState,
                "Cannot process a full heartbeat in %Qlv state",
                node->GetState()));
            return;
        }

        nodeTracker
            ->CreateFullHeartbeatMutation(context)
            ->Commit()
            .Subscribe(CreateRpcResponseHandler(context));
    }

    DECLARE_RPC_SERVICE_METHOD(NNodeTrackerClient::NProto, IncrementalHeartbeat)
    {
        ValidateActiveLeader();

        auto nodeId = request->node_id();
        const auto& statistics = request->statistics();

        auto nodeTracker = Bootstrap_->GetNodeTracker();
        auto* node = nodeTracker->GetNodeOrThrow(nodeId);

        context->SetRequestInfo("NodeId: %v, Address: %v, %v",
            nodeId,
            node->GetDefaultAddress(),
            statistics);

        if (node->GetState() != ENodeState::Online) {
            context->Reply(TError(
                NNodeTrackerClient::EErrorCode::InvalidState,
                "Cannot process an incremental heartbeat in %v state",
                node->GetState()));
            return;
        }

        nodeTracker
            ->CreateIncrementalHeartbeatMutation(context)
            ->Commit()
            .Subscribe(CreateRpcResponseHandler(context));
    }

};

NRpc::IServicePtr CreateNodeTrackerService(
    TNodeTrackerConfigPtr config,
    NCellMaster::TBootstrap* bootstrap)
{
    return New<TNodeTrackerService>(config, bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
