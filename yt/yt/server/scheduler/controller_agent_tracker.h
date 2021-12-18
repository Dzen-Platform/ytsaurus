#pragma once

#include "public.h"

#include <yt/yt/core/ytree/public.h>

#include <yt/yt/core/rpc/service_detail.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

/*!
 *  Thread affinity: Control thread (unless noted otherwise)
 */
class TControllerAgentTracker
    : public TRefCounted
{
public:
    TControllerAgentTracker(
        TSchedulerConfigPtr config,
        TBootstrap* bootstrap);
    ~TControllerAgentTracker();

    void Initialize();

    std::vector<TControllerAgentPtr> GetAgents() const;

    IOperationControllerPtr CreateController(const TOperationPtr& operation);

    TControllerAgentPtr PickAgentForOperation(const TOperationPtr& operation);
    void AssignOperationToAgent(
        const TOperationPtr& operation,
        const TControllerAgentPtr& agent);

    void UnregisterOperationFromAgent(const TOperationPtr& operation);

    void UpdateConfig(TSchedulerConfigPtr config);

    /*!
     *  Thread affinity: any
     */
    void HandleAgentFailure(const TControllerAgentPtr& agent, const TError& error);

    using TCtxAgentHandshake = NRpc::TTypedServiceContext<
        NScheduler::NProto::TReqHandshake,
        NScheduler::NProto::TRspHandshake>;
    using TCtxAgentHandshakePtr = TIntrusivePtr<TCtxAgentHandshake>;
    void ProcessAgentHandshake(const TCtxAgentHandshakePtr& context);

    using TCtxAgentHeartbeat = NRpc::TTypedServiceContext<
        NScheduler::NProto::TReqHeartbeat,
        NScheduler::NProto::TRspHeartbeat>;
    using TCtxAgentHeartbeatPtr = TIntrusivePtr<TCtxAgentHeartbeat>;
    void ProcessAgentHeartbeat(const TCtxAgentHeartbeatPtr& context);

    using TCtxAgentScheduleJobHeartbeat = NRpc::TTypedServiceContext<
        NScheduler::NProto::TReqScheduleJobHeartbeat,
        NScheduler::NProto::TRspScheduleJobHeartbeat>;
    using TCtxAgentScheduleJobHeartbeatPtr = TIntrusivePtr<TCtxAgentScheduleJobHeartbeat>;
    void ProcessAgentScheduleJobHeartbeat(const TCtxAgentScheduleJobHeartbeatPtr& context);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TControllerAgentTracker)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
