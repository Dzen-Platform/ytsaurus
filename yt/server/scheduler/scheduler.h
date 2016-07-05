#pragma once

#include "public.h"

#include <yt/server/cell_scheduler/public.h>

#include <yt/ytlib/hydra/public.h>

#include <yt/ytlib/job_tracker_client/job_tracker_service.pb.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/rpc/service_detail.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

class TScheduler
    : public TRefCounted
{
public:
    TScheduler(
        TSchedulerConfigPtr config,
        NCellScheduler::TBootstrap* bootstrap);

    ~TScheduler();

    void Initialize();

    ISchedulerStrategy* GetStrategy();

    NYTree::IYPathServicePtr GetOrchidService();

    std::vector<TOperationPtr> GetOperations();

    IInvokerPtr GetSnapshotIOInvoker();

    bool IsConnected();
    void ValidateConnected();

    TOperationPtr FindOperation(const TOperationId& id);
    TOperationPtr GetOperationOrThrow(const TOperationId& id);

    TFuture<TOperationPtr> StartOperation(
        EOperationType type,
        const NTransactionClient::TTransactionId& transactionId,
        const NRpc::TMutationId& mutationId,
        NYTree::IMapNodePtr spec,
        const Stroka& user);

    TFuture<void> AbortOperation(TOperationPtr operation, const TError& error, const Stroka& user);
    TFuture<void> SuspendOperation(TOperationPtr operation, const Stroka& user);
    TFuture<void> ResumeOperation(TOperationPtr operation, const Stroka& user);
    TFuture<void> CompleteOperation(
        TOperationPtr operation,
        const TError& error,
        const Stroka& user);

    TFuture<NYson::TYsonString> Strace(const TJobId& jobId, const Stroka& user);
    TFuture<void> DumpInputContext(const TJobId& jobId, const NYPath::TYPath& path, const Stroka& user);
    TFuture<void> SignalJob(const TJobId& jobId, const Stroka& signalName, const Stroka& user);
    TFuture<void> AbandonJob(const TJobId& jobId, const Stroka& user);
    TFuture<NYson::TYsonString> PollJobShell(const TJobId& jobId, const NYson::TYsonString& parameters, const Stroka& user);
    TFuture<void> AbortJob(const TJobId& jobId, const Stroka& user);

    using TCtxHeartbeat = NRpc::TTypedServiceContext<
        NJobTrackerClient::NProto::TReqHeartbeat,
        NJobTrackerClient::NProto::TRspHeartbeat>;
    using TCtxHeartbeatPtr = TIntrusivePtr<TCtxHeartbeat>;
    void ProcessHeartbeat(TCtxHeartbeatPtr context);

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl_;

    class TSchedulingContext;

};

DEFINE_REFCOUNTED_TYPE(TScheduler)

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

