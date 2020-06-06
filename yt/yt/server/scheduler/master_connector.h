#pragma once

#include "public.h"

#include <yt/server/lib/controller_agent/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/client/api/public.h>

#include <yt/core/actions/signal.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

//! Information retrieved during scheduler-master handshake.
struct TMasterHandshakeResult
{
    std::vector<TOperationPtr> Operations;
};

using TWatcherRequester = TCallback<void(NObjectClient::TObjectServiceProxy::TReqExecuteBatchPtr)>;
using TWatcherHandler = TCallback<void(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr)>;

DEFINE_ENUM(EMasterConnectorState,
    (Disconnected)
    (Connecting)
    (Connected)
);

DEFINE_ENUM(EWatcherType,
    (NodeAttributes)
);

//! Mediates communication between scheduler and master.
/*!
 *  \note Thread affinity: control unless noted otherwise
 */
class TMasterConnector
{
public:
    TMasterConnector(
        TSchedulerConfigPtr config,
        TBootstrap* bootstrap);
    ~TMasterConnector();

    /*!
     *  \note Thread affinity: any
     */
    void Start();

    /*!
     *  \note Thread affinity: any
     */
    EMasterConnectorState GetState() const;

    /*!
     *  \note Thread affinity: any
     */
    TInstant GetConnectionTime() const;

    const NApi::ITransactionPtr& GetLockTransaction() const;

    const IInvokerPtr& GetCancelableControlInvoker(EControlQueue queue) const;

    void Disconnect(const TError& error);

    void RegisterOperation(const TOperationPtr& operation);
    void UnregisterOperation(const TOperationPtr& operation);

    TFuture<void> CreateOperationNode(const TOperationPtr& operation);
    TFuture<void> UpdateInitializedOperationNode(const TOperationPtr& operation);
    TFuture<void> FlushOperationNode(const TOperationPtr& operation);
    TFuture<void> FetchOperationRevivalDescriptors(const std::vector<TOperationPtr>& operations);
    TFuture<NYson::TYsonString> GetOperationNodeProgressAttributes(const TOperationPtr& operation);

    void StoreStrategyStateAsync(TPersistentStrategyStatePtr strategyState);

    void AttachJobContext(
        const NYPath::TYPath& path,
        NChunkClient::TChunkId chunkId,
        TOperationId operationId,
        TJobId jobId,
        const TString& usrt);

    void SetSchedulerAlert(ESchedulerAlertType alertType, const TError& alert);

    void AddCommonWatcher(
        TWatcherRequester requester,
        TWatcherHandler handler,
        std::optional<ESchedulerAlertType> alertType = std::nullopt);

    void SetCustomWatcher(
        EWatcherType type,
        TWatcherRequester requester,
        TWatcherHandler handler,
        TDuration period,
        std::optional<ESchedulerAlertType> alertType = std::nullopt);

    void UpdateConfig(const TSchedulerConfigPtr& config);

    //! Raised when connection prcoess starts.
    //! Subscribers may throw and yield.
    DECLARE_SIGNAL(void(), MasterConnecting);

    //! Raised during connection process.
    //! Handshake result contains operations created from Cypress data; all of these have valid revival descriptors.
    //! Subscribers may throw and yield.
    DECLARE_SIGNAL(void(const TMasterHandshakeResult& result), MasterHandshake);

    //! Raised when connection is complete.
    //! Subscribers may throw but cannot yield.
    DECLARE_SIGNAL(void(), MasterConnected);

    //! Raised when disconnect happens.
    //! Subscribers cannot neither throw nor yield
    DECLARE_SIGNAL(void(), MasterDisconnected);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
