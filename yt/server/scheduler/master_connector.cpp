#include "master_connector.h"
#include "private.h"
#include "helpers.h"
#include "scheduler.h"
#include "scheduler_strategy.h"
#include "serialize.h"
#include "snapshot_builder.h"
#include "snapshot_downloader.h"

#include <yt/server/cell_scheduler/bootstrap.h>
#include <yt/server/cell_scheduler/config.h>

#include <yt/ytlib/chunk_client/chunk_list_ypath_proxy.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>
#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/file_client/file_ypath_proxy.h>

#include <yt/ytlib/hive/cluster_directory.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/master_ypath_proxy.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_ypath_proxy.h>

#include <yt/ytlib/api/transaction.h>

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/rpc/serialized_channel.h>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NObjectServer;
using namespace NChunkClient;
using namespace NFileClient;
using namespace NTransactionClient;
using namespace NRpc;
using namespace NApi;
using namespace NSecurityClient;
using namespace NTransactionClient::NProto;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;

////////////////////////////////////////////////////////////////////

class TMasterConnector::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TSchedulerConfigPtr config,
        NCellScheduler::TBootstrap* bootstrap)
        : Config(config)
        , Bootstrap(bootstrap)
        , Proxy(Bootstrap->GetMasterClient()->GetMasterChannelOrThrow(EMasterChannelKind::Leader))
        , ClusterDirectory(Bootstrap->GetClusterDirectory())
    { }

    void Start()
    {
        Bootstrap->GetControlInvoker()->Invoke(BIND(
            &TImpl::StartConnecting,
            MakeStrong(this)));
    }

    bool IsConnected() const
    {
        return Connected;
    }

    IInvokerPtr GetCancelableControlInvoker() const
    {
        return CancelableControlInvoker;
    }

    TFuture<void> CreateOperationNode(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        auto operationId = operation->GetId();
        LOG_INFO("Creating operation node (OperationId: %v)",
            operationId);

        auto strategy = Bootstrap->GetScheduler()->GetStrategy();

        auto path = GetOperationPath(operationId);
        auto batchReq = StartBatchRequest();
        {
            auto req = TYPathProxy::Set(path);
            req->set_value(BuildYsonStringFluently()
                .BeginAttributes()
                    .Do(BIND(&ISchedulerStrategy::BuildOperationAttributes, strategy, operationId))
                    .Do(BIND(&BuildInitializingOperationAttributes, operation))
                    .Item("brief_spec").BeginMap()
                        .Do(BIND(&IOperationController::BuildBriefSpec, operation->GetController()))
                        .Do(BIND(&ISchedulerStrategy::BuildBriefSpec, strategy, operationId))
                    .EndMap()
                    .Item("progress").BeginMap().EndMap()
                    .Item("brief_progress").BeginMap().EndMap()
                    .Item("opaque").Value("true")
                .EndAttributes()
                .BeginMap()
                    .Item("jobs").BeginAttributes()
                        .Item("opaque").Value("true")
                    .EndAttributes()
                    .BeginMap().EndMap()
                .EndMap()
                .Data());
            GenerateMutationId(req);
            batchReq->AddRequest(req);
        }

        {
            auto acl = NYTree::BuildYsonNodeFluently()
                .BeginList()
                    .Item().BeginMap()
                        .Item("action").Value(ESecurityAction::Allow)
                        .Item("subjects").BeginList()
                            .Item().Value(operation->GetAuthenticatedUser())
                        .EndList()
                        .Item("permissions").BeginList()
                            .Item().Value(EPermission::Write)
                        .EndList()
                    .EndMap()
                .EndList();


            auto req = TYPathProxy::Set(path + "/@acl");
            req->set_value(ConvertToYsonString(acl).Data());

            batchReq->AddRequest(req);
        }

        return batchReq->Invoke().Apply(
            BIND(&TImpl::OnOperationNodeCreated, MakeStrong(this), operation)
                .AsyncVia(Bootstrap->GetControlInvoker()));
    }

    TFuture<void> ResetRevivingOperationNode(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);
        YCHECK(operation->GetState() == EOperationState::Reviving);

        auto operationId = operation->GetId();
        LOG_INFO("Resetting reviving operation node (OperationId: %v)",
            operationId);

        auto* list = GetUpdateList(operationId);
        auto batchReq = StartBatchRequest(list);

        auto attributes = ConvertToAttributes(BuildYsonStringFluently()
            .BeginMap()
                .Do(BIND(&BuildRunningOperationAttributes, operation))
                .Item("progress").BeginMap().EndMap()
                .Item("brief_progress").BeginMap().EndMap()
            .EndMap());

        for (const auto& key : attributes->List()) {
            auto req = TYPathProxy::Set(GetOperationPath(operationId) + "/@" + ToYPathLiteral(key));
            req->set_value(attributes->GetYson(key).Data());
            GenerateMutationId(req);
            batchReq->AddRequest(req);
        }

        return batchReq->Invoke().Apply(
            BIND(
                &TImpl::OnRevivingOperationNodeReset,
                MakeStrong(this),
                operation)
            .AsyncVia(GetCancelableControlInvoker()));
    }

    TFuture<void> FlushOperationNode(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        auto id = operation->GetId();
        LOG_INFO("Flushing operation node (OperationId: %v)",
            id);

        auto* list = FindUpdateList(id);
        if (!list) {
            LOG_INFO("Operation node is not registered, omitting flush (OperationId: %v)",
                id);
            return VoidFuture;
        }

        return UpdateOperationNode(list).Apply(
            BIND(&TImpl::OnOperationNodeFlushed, MakeStrong(this), operation)
                .Via(CancelableControlInvoker));
    }


    void CreateJobNode(
        TJobPtr job,
        const TChunkId& stderrChunkId,
        const TChunkId& failContextChunkId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        LOG_DEBUG("Creating job node (OperationId: %v, JobId: %v, StdErrChunkId: %v, FailContextChunkId: %v)",
            job->GetOperationId(),
            job->GetId(),
            stderrChunkId,
            failContextChunkId);

        auto* list = GetUpdateList(job->GetOperationId());
        TJobRequest request;
        request.Job = job;
        request.StderrChunkId = stderrChunkId;
        request.FailContextChunkId = failContextChunkId;
        list->JobRequests.push_back(request);
    }

    TFuture<void> AttachToLivePreview(
        TOperationPtr operation,
        const TChunkListId& chunkListId,
        const std::vector<TChunkTreeId>& childrenIds)
    {
        return BIND(&TImpl::DoAttachToLivePreview, MakeStrong(this))
            .AsyncVia(CancelableControlInvoker)
            .Run(operation, chunkListId, childrenIds);
    }

    void AddGlobalWatcherRequester(TWatcherRequester requester)
    {
        GlobalWatcherRequesters.push_back(requester);
    }

    void AddGlobalWatcherHandler(TWatcherHandler handler)
    {
        GlobalWatcherHandlers.push_back(handler);
    }

    void AddOperationWatcherRequester(TOperationPtr operation, TWatcherRequester requester)
    {
        auto* list = GetOrCreateWatcherList(operation);
        list->WatcherRequesters.push_back(requester);
    }

    void AddOperationWatcherHandler(TOperationPtr operation, TWatcherHandler handler)
    {
        auto* list = GetOrCreateWatcherList(operation);
        list->WatcherHandlers.push_back(handler);
    }

    void AttachJobContext(
        const TYPath& path,
        const TChunkId& chunkId,
        TJobPtr job)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(chunkId);

        auto client = Bootstrap->GetMasterClient();

        try {
            TJobFile file{
                job->GetId(),
                path,
                chunkId,
                "input_context"
            };
            SaveJobFiles(job->GetOperation(), { file });
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error saving input context for job %v into %v",
                job->GetId(),
                path)
                << ex;
        }
    }


    DEFINE_SIGNAL(void(const TMasterHandshakeResult& result), MasterConnected);
    DEFINE_SIGNAL(void(), MasterDisconnected);

    DEFINE_SIGNAL(void(TOperationPtr operation), UserTransactionAborted);
    DEFINE_SIGNAL(void(TOperationPtr operation), SchedulerTransactionAborted);

private:
    const TSchedulerConfigPtr Config;
    NCellScheduler::TBootstrap* const Bootstrap;

    TObjectServiceProxy Proxy;
    NHive::TClusterDirectoryPtr ClusterDirectory;

    TCancelableContextPtr CancelableContext;
    IInvokerPtr CancelableControlInvoker;

    bool Connected = false;

    ITransactionPtr LockTransaction;

    TPeriodicExecutorPtr TransactionRefreshExecutor;
    TPeriodicExecutorPtr OperationNodesUpdateExecutor;
    TPeriodicExecutorPtr WatchersExecutor;
    TPeriodicExecutorPtr SnapshotExecutor;
    TPeriodicExecutorPtr ClusterDirectoryUpdateExecutor;

    std::vector<TWatcherRequester> GlobalWatcherRequesters;
    std::vector<TWatcherHandler>   GlobalWatcherHandlers;

    struct TJobRequest
    {
        TJobPtr Job;
        TChunkId StderrChunkId;
        TChunkId FailContextChunkId;
    };

    struct TLivePreviewRequest
    {
        TChunkListId ChunkListId;
        TChunkTreeId ChildId;
    };

    struct TUpdateList
    {
        TUpdateList(IChannelPtr masterChannel, TOperationPtr operation)
            : Operation(operation)
            , Proxy(CreateSerializedChannel(masterChannel))
        { }

        TOperationPtr Operation;
        std::vector<TJobRequest> JobRequests;
        std::vector<TLivePreviewRequest> LivePreviewRequests;
        TObjectServiceProxy Proxy;
        TFuture<void> LastUpdateFuture = VoidFuture;
    };

    yhash_map<TOperationId, TUpdateList> UpdateLists;

    struct TWatcherList
    {
        explicit TWatcherList(TOperationPtr operation)
            : Operation(operation)
        { }

        TOperationPtr Operation;
        std::vector<TWatcherRequester> WatcherRequesters;
        std::vector<TWatcherHandler>   WatcherHandlers;
    };

    yhash_map<TOperationId, TWatcherList> WatcherLists;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);


    void StartConnecting()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Connecting to master");

        auto pipeline = New<TRegistrationPipeline>(this);
        BIND(&TRegistrationPipeline::Run, pipeline)
            .AsyncVia(Bootstrap->GetControlInvoker())
            .Run()
            .Subscribe(BIND(&TImpl::OnConnected, MakeStrong(this))
                .Via(Bootstrap->GetControlInvoker()));
    }

    void OnConnected(const TErrorOr<TMasterHandshakeResult>& resultOrError)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!resultOrError.IsOK()) {
            LOG_ERROR(resultOrError, "Error connecting to master");
            TDelayedExecutor::Submit(
                BIND(&TImpl::StartConnecting, MakeStrong(this))
                    .Via(Bootstrap->GetControlInvoker()),
                Config->ConnectRetryBackoffTime);
            return;
        }

        LOG_INFO("Master connected");

        YCHECK(!Connected);
        Connected = true;

        CancelableContext = New<TCancelableContext>();
        CancelableControlInvoker = CancelableContext->CreateInvoker(Bootstrap->GetControlInvoker());

        const auto& result = resultOrError.Value();
        for (auto operation : result.Operations) {
            CreateUpdateList(operation);
        }
        for (auto handler : GlobalWatcherHandlers) {
            handler.Run(result.WatcherResponses);
        }

        LockTransaction->SubscribeAborted(
            BIND(&TImpl::OnLockTransactionAborted, MakeWeak(this))
                .Via(CancelableControlInvoker));

        StartPeriodicActivities();

        MasterConnected_.Fire(result);
    }

    void OnLockTransactionAborted()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_WARNING("Lock transaction aborted");

        Disconnect();
    }



    class TRegistrationPipeline
        : public TRefCounted
    {
    public:
        explicit TRegistrationPipeline(TIntrusivePtr<TImpl> owner)
            : Owner(owner)
        {
            auto localHostName = TAddressResolver::Get()->GetLocalHostName();
            int port = Owner->Bootstrap->GetConfig()->RpcPort;
            ServiceAddress = BuildServiceAddress(localHostName, port);
        }

        TMasterHandshakeResult Run()
        {
            RegisterInstance();
            StartLockTransaction();
            TakeLock();
            AssumeControl();
            UpdateClusterDirectory();
            ListOperations();
            RequestOperationAttributes();
            CheckOperationTransactions();
            DownloadSnapshots();
            AbortTransactions();
            RemoveSnapshots();
            InvokeWatchers();
            return Result;
        }

    private:
        TIntrusivePtr<TImpl> Owner;
        Stroka ServiceAddress;
        std::vector<TOperationId> OperationIds;
        std::vector<TOperationId> AbortingOperationIds;
        TMasterHandshakeResult Result;

        // - Register scheduler instance.
        void RegisterInstance()
        {
            auto batchReq = Owner->StartBatchRequest(false);
            auto path = "//sys/scheduler/instances/" + ToYPathLiteral(ServiceAddress);
            {
                auto req = TCypressYPathProxy::Create(path);
                req->set_ignore_existing(true);
                req->set_type(static_cast<int>(EObjectType::MapNode));
                GenerateMutationId(req);
                batchReq->AddRequest(req);
            }
            {
                auto req = TCypressYPathProxy::Create(path + "/orchid");
                req->set_ignore_existing(true);
                req->set_type(static_cast<int>(EObjectType::Orchid));
                auto attributes = CreateEphemeralAttributes();
                attributes->Set("remote_address", ServiceAddress);
                ToProto(req->mutable_node_attributes(), *attributes);
                GenerateMutationId(req);
                batchReq->AddRequest(req);
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
        }

        // - Start lock transaction.
        void StartLockTransaction()
        {
            TTransactionStartOptions options;
            options.AutoAbort = true;
            options.Timeout = Owner->Config->LockTransactionTimeout;
            auto attributes = CreateEphemeralAttributes();
            attributes->Set("title", Format("Scheduler lock at %v", ServiceAddress));
            options.Attributes = std::move(attributes);

            auto client = Owner->Bootstrap->GetMasterClient();
            auto transactionOrError = WaitFor(Owner->Bootstrap->GetMasterClient()->StartTransaction(
                ETransactionType::Master,
                options));
            THROW_ERROR_EXCEPTION_IF_FAILED(transactionOrError, "Error starting lock transaction");

            Owner->LockTransaction = transactionOrError.Value();

            LOG_INFO("Lock transaction is %v", Owner->LockTransaction->GetId());
        }

        // - Take lock.
        void TakeLock()
        {
            auto result = WaitFor(Owner->LockTransaction->LockNode("//sys/scheduler/lock", ELockMode::Exclusive));
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error taking scheduler lock");
        }

        // - Publish scheduler address.
        // - Update orchid address.
        void AssumeControl()
        {
            auto batchReq = Owner->StartBatchRequest();
            auto schedulerAddress = Owner->Bootstrap->GetLocalAddress();
            {
                auto req = TYPathProxy::Set("//sys/scheduler/@address");
                req->set_value(ConvertToYsonString(schedulerAddress).Data());
                GenerateMutationId(req);
                batchReq->AddRequest(req, "set_scheduler_address");
            }
            {
                auto req = TYPathProxy::Set("//sys/scheduler/orchid/@remote_address");
                req->set_value(ConvertToYsonString(schedulerAddress).Data());
                GenerateMutationId(req);
                batchReq->AddRequest(req, "set_orchid_address");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
        }

        void UpdateClusterDirectory()
        {
            Owner->Bootstrap->GetClusterDirectory()->UpdateSelf();
            Owner->UpdateClusterDirectory();
        }

        // - Request operations and their states.
        void ListOperations()
        {
            auto batchReq = Owner->StartBatchRequest();
            {
                auto req = TYPathProxy::List("//sys/operations");
                std::vector<Stroka> attributeKeys{"state"};
                ToProto(req->mutable_attributes(), attributeKeys);
                batchReq->AddRequest(req, "list_operations");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            {
                auto rsp = batchRsp->GetResponse<TYPathProxy::TRspList>("list_operations").Value();
                auto operationsListNode = ConvertToNode(TYsonString(rsp->value()));
                auto operationsList = operationsListNode->AsList();
                LOG_INFO("Operations list received, %v operations total",
                    operationsList->GetChildCount());
                OperationIds.clear();
                for (auto operationNode : operationsList->GetChildren()) {
                    auto id = TOperationId::FromString(operationNode->GetValue<Stroka>());
                    auto state = operationNode->Attributes().Get<EOperationState>("state");
                    if (IsOperationInProgress(state) || state == EOperationState::Aborting) {
                        OperationIds.push_back(id);
                    }
                }
            }
        }

        // - Request attributes for unfinished operations.
        // - Recreate operation instance from fetched data.
        void RequestOperationAttributes()
        {
            auto batchReq = Owner->StartBatchRequest();
            {
                LOG_INFO("Fetching attributes for %v unfinished operations",
                    OperationIds.size());
                for (const auto& operationId : OperationIds) {
                    auto req = TYPathProxy::Get(GetOperationPath(operationId) + "/@");
                    // Keep in sync with CreateOperationFromAttributes.
                    std::vector<Stroka> attributeKeys{
                        "operation_type",
                        "mutation_id",
                        "user_transaction_id",
                        "sync_scheduler_transaction_id",
                        "async_scheduler_transaction_id",
                        "input_transaction_id",
                        "output_transaction_id",
                        "spec",
                        "authenticated_user",
                        "start_time",
                        "state",
                        "suspended"};
                    ToProto(req->mutable_attributes(), attributeKeys);
                    batchReq->AddRequest(req, "get_op_attr");
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            {
                auto rsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_op_attr");
                YCHECK(rsps.size() == OperationIds.size());

                for (int index = 0; index < static_cast<int>(rsps.size()); ++index) {
                    const auto& operationId = OperationIds[index];
                    auto rsp = rsps[index].Value();
                    auto attributesNode = ConvertToAttributes(TYsonString(rsp->value()));
                    auto operation = Owner->CreateOperationFromAttributes(operationId, *attributesNode);

                    Result.Operations.push_back(operation);
                    if (operation->GetState() == EOperationState::Aborting) {
                        Result.AbortingOperations.push_back(operation);
                    } else {
                        Result.RevivingOperations.push_back(operation);
                    }
                }
            }
        }

        // - Try to ping the previous incarnations of scheduler transactions.
        void CheckOperationTransactions()
        {
            std::vector<TFuture<void>> asyncResults;
            for (auto operation : Result.RevivingOperations) {
                operation->SetState(EOperationState::Reviving);

                auto checkTransaction = [&] (TOperationPtr operation, ITransactionPtr transaction) {
                    if (!transaction)
                        return;

                    asyncResults.push_back(transaction->Ping().Apply(
                        BIND([=] (const TError& error) {
                            if (!error.IsOK() && !operation->GetCleanStart()) {
                                operation->SetCleanStart(true);
                                LOG_INFO("Error renewing operation transaction, will use clean start (OperationId: %v, TransactionId: %v)",
                                    operation->GetId(),
                                    transaction->GetId());
                            }
                        })));
                };

                // NB: Async transaction is not checked.
                checkTransaction(operation, operation->GetUserTransaction());
                checkTransaction(operation, operation->GetSyncSchedulerTransaction());
                checkTransaction(operation, operation->GetInputTransaction());
                checkTransaction(operation, operation->GetOutputTransaction());
            }

            WaitFor(Combine(asyncResults))
                .ThrowOnError();
        }

        // - Check snapshots for existence and validate versions.
        void DownloadSnapshots()
        {
            for (auto operation : Result.RevivingOperations) {
                if (!operation->GetCleanStart()) {
                    if (!DownloadSnapshot(operation)) {
                        operation->SetCleanStart(true);
                    }
                }
            }
        }

        bool DownloadSnapshot(TOperationPtr operation)
        {
            const auto& operationId = operation->GetId();
            auto snapshotPath = GetSnapshotPath(operationId);

            auto batchReq = Owner->StartBatchRequest();
            auto req = TYPathProxy::Get(snapshotPath + "/@version");
            batchReq->AddRequest(req, "get_version");

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError);
            const auto& batchRsp = batchRspOrError.Value();

            auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_version");
            // Check for missing snapshots.
            if (rspOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
                LOG_INFO("Snapshot does not exist, will use clean start (OperationId: %v)",
                    operationId);
                return false;
            }
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting snapshot version");

            const auto& rsp = rspOrError.Value();
            int version = ConvertTo<int>(TYsonString(rsp->value()));

            LOG_INFO("Snapshot found (OperationId: %v, Version: %v)",
                operationId,
                version);

            if (!ValidateSnapshotVersion(version)) {
                LOG_INFO("Snapshot version validation failed, will use clean start (OperationId: %v)",
                    operationId);
                return false;
            }

            if (!Owner->Config->EnableSnapshotLoading) {
                LOG_INFO("Snapshot loading is disabled in configuration (OperationId: %v)",
                    operationId);
                return false;
            }

            try {
                auto downloader = New<TSnapshotDownloader>(
                    Owner->Config,
                    Owner->Bootstrap,
                    operation);
                downloader->Run();
            } catch (const std::exception& ex) {
                LOG_ERROR(ex, "Error downloading snapshot (OperationId: %v)",
                    operationId);
                return false;
            }

            // Everything seems OK.
            LOG_INFO("Operation state will be recovered from snapshot (OperationId: %v)",
                operationId);
            return true;
        }

        // - Abort orphaned transactions.
        void AbortTransactions()
        {
            std::vector<TFuture<void>> asyncResults;
            for (auto operation : Result.Operations) {
                auto scheduleAbort = [&] (ITransactionPtr transaction) {
                    if (!transaction)
                        return;
                    asyncResults.push_back(transaction->Abort());
                };

                // NB: Async transaction is always aborted.
                {
                    scheduleAbort(operation->GetAsyncSchedulerTransaction());
                    operation->SetAsyncSchedulerTransaction(nullptr);
                }

                if (operation->GetCleanStart()) {
                    LOG_INFO("Aborting operation transactions (OperationId: %v)",
                        operation->GetId());

                    operation->SetHasActiveTransactions(false);

                    // NB: Don't touch user transaction.
                    scheduleAbort(operation->GetSyncSchedulerTransaction());
                    operation->SetSyncSchedulerTransaction(nullptr);

                    scheduleAbort(operation->GetInputTransaction());
                    operation->SetInputTransaction(nullptr);

                    scheduleAbort(operation->GetOutputTransaction());
                    operation->SetOutputTransaction(nullptr);
                } else {
                    LOG_INFO("Reusing operation transactions (OperationId: %v)",
                        operation->GetId());
                }
            }

            WaitFor(Combine(asyncResults))
                .ThrowOnError();
        }

        // - Remove unneeded snapshots.
        void RemoveSnapshots()
        {
            auto batchReq = Owner->StartBatchRequest();

            for (auto operation : Result.Operations) {
                if (operation->GetCleanStart()) {
                    auto req = TYPathProxy::Remove(GetSnapshotPath(operation->GetId()));
                    req->set_force(true);
                    batchReq->AddRequest(req, "remove_snapshot");
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError);

            {
                const auto& batchRsp = batchRspOrError.Value();
                auto rspsOrError = batchRsp->GetResponses<TYPathProxy::TRspRemove>("remove_snapshot");
                for (auto rspOrError : rspsOrError) {
                    THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error removing snapshot");
                }
            }
        }

        // - Send watcher requests.
        void InvokeWatchers()
        {
            auto batchReq = Owner->StartBatchRequest();
            for (auto requester : Owner->GlobalWatcherRequesters) {
                requester.Run(batchReq);
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError);
            Result.WatcherResponses = batchRspOrError.Value();
        }

    };


    TObjectServiceProxy::TReqExecuteBatchPtr StartBatchRequest(bool requireTransaction = true)
    {
        return DoStartBatchRequest(&Proxy, requireTransaction);
    }

    TObjectServiceProxy::TReqExecuteBatchPtr StartBatchRequest(TUpdateList* list, bool requireTransaction = true)
    {
        return DoStartBatchRequest(&list->Proxy, requireTransaction);
    }

    TObjectServiceProxy::TReqExecuteBatchPtr DoStartBatchRequest(TObjectServiceProxy* proxy, bool requireTransaction = true)
    {
        auto batchReq = proxy->ExecuteBatch();
        if (requireTransaction) {
            YCHECK(LockTransaction);
            auto* prerequisitesExt = batchReq->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
            auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
            ToProto(prerequisiteTransaction->mutable_transaction_id(), LockTransaction->GetId());
        }
        return batchReq;
    }


    void Disconnect()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!Connected)
            return;

        LOG_WARNING("Master disconnected");

        Connected = false;

        LockTransaction.Reset();

        ClearUpdateLists();
        ClearWatcherLists();

        StopPeriodicActivities();

        CancelableContext->Cancel();

        MasterDisconnected_.Fire();

        StartConnecting();
    }


    TOperationPtr CreateOperationFromAttributes(const TOperationId& operationId, const IAttributeDictionary& attributes)
    {
        auto getTransaction = [&] (const TTransactionId& id, bool ping) -> ITransactionPtr {
            if (!id) {
                return nullptr;
            }
            auto clusterDirectory = Bootstrap->GetClusterDirectory();
            auto connection = clusterDirectory->GetConnection(CellTagFromId(id));
            auto client = connection->CreateClient(TClientOptions(SchedulerUserName));
            TTransactionAttachOptions options;
            options.Ping = ping;
            options.PingAncestors = false;
            return client->AttachTransaction(id, options);
        };

        auto userTransaction = getTransaction(
            attributes.Get<TTransactionId>("user_transaction_id"),
            false);

        auto syncTransaction = getTransaction(
            attributes.Get<TTransactionId>("sync_scheduler_transaction_id"),
            true);

        auto asyncTransaction = getTransaction(
            attributes.Get<TTransactionId>("async_scheduler_transaction_id"),
            true);

        auto inputTransaction = getTransaction(
            attributes.Get<TTransactionId>("input_transaction_id"),
            true);

        auto outputTransaction = getTransaction(
            attributes.Get<TTransactionId>("output_transaction_id"),
            true);

        auto operation = New<TOperation>(
            operationId,
            attributes.Get<EOperationType>("operation_type"),
            attributes.Get<TMutationId>("mutation_id"),
            userTransaction,
            attributes.Get<INodePtr>("spec")->AsMap(),
            attributes.Get<Stroka>("authenticated_user"),
            attributes.Get<TInstant>("start_time"),
            attributes.Get<EOperationState>("state"),
            attributes.Get<bool>("suspended"));

        operation->SetSyncSchedulerTransaction(syncTransaction);
        operation->SetAsyncSchedulerTransaction(asyncTransaction);
        operation->SetInputTransaction(inputTransaction);
        operation->SetOutputTransaction(outputTransaction);
        operation->SetHasActiveTransactions(true);

        return operation;
    }


    void StartPeriodicActivities()
    {
        TransactionRefreshExecutor = New<TPeriodicExecutor>(
            CancelableControlInvoker,
            BIND(&TImpl::RefreshTransactions, MakeWeak(this)),
            Config->TransactionsRefreshPeriod,
            EPeriodicExecutorMode::Automatic);
        TransactionRefreshExecutor->Start();

        OperationNodesUpdateExecutor = New<TPeriodicExecutor>(
            CancelableControlInvoker,
            BIND(&TImpl::UpdateOperationNodes, MakeWeak(this)),
            Config->OperationsUpdatePeriod,
            EPeriodicExecutorMode::Automatic);
        OperationNodesUpdateExecutor->Start();

        WatchersExecutor = New<TPeriodicExecutor>(
            CancelableControlInvoker,
            BIND(&TImpl::UpdateWatchers, MakeWeak(this)),
            Config->WatchersUpdatePeriod,
            EPeriodicExecutorMode::Automatic);
        WatchersExecutor->Start();

        ClusterDirectoryUpdateExecutor = New<TPeriodicExecutor>(
            CancelableControlInvoker,
            BIND(&TImpl::UpdateClusterDirectory, MakeWeak(this)),
            Config->ClusterDirectoryUpdatePeriod,
            EPeriodicExecutorMode::Automatic);
        ClusterDirectoryUpdateExecutor->Start();

        SnapshotExecutor = New<TPeriodicExecutor>(
            CancelableControlInvoker,
            BIND(&TImpl::BuildSnapshot, MakeWeak(this)),
            Config->SnapshotPeriod,
            EPeriodicExecutorMode::Automatic);
        SnapshotExecutor->Start();
    }

    void StopPeriodicActivities()
    {
        if (TransactionRefreshExecutor) {
            TransactionRefreshExecutor->Stop();
            TransactionRefreshExecutor.Reset();
        }

        if (OperationNodesUpdateExecutor) {
            OperationNodesUpdateExecutor->Stop();
            OperationNodesUpdateExecutor.Reset();
        }

        if (WatchersExecutor) {
            WatchersExecutor->Stop();
            WatchersExecutor.Reset();
        }

        if (ClusterDirectoryUpdateExecutor) {
            ClusterDirectoryUpdateExecutor->Stop();
            ClusterDirectoryUpdateExecutor.Reset();
        }

        if (SnapshotExecutor) {
            SnapshotExecutor->Stop();
            SnapshotExecutor.Reset();
        }
    }


    void RefreshTransactions()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        // Collect all transactions that are used by currently running operations.
        yhash_set<TTransactionId> watchSet;
        auto watchTransaction = [&] (ITransactionPtr transaction) {
            if (transaction) {
                watchSet.insert(transaction->GetId());
            }
        };

        auto operations = Bootstrap->GetScheduler()->GetOperations();
        for (auto operation : operations) {
            if (!operation->GetHasActiveTransactions()) {
                continue;
            }

            watchTransaction(operation->GetUserTransaction());
            watchTransaction(operation->GetSyncSchedulerTransaction());
            watchTransaction(operation->GetAsyncSchedulerTransaction());
            watchTransaction(operation->GetInputTransaction());
            watchTransaction(operation->GetOutputTransaction());
        }

        yhash_map<TCellTag, TObjectServiceProxy::TReqExecuteBatchPtr> batchReqs;

        for (const auto& id : watchSet) {
            auto cellTag = CellTagFromId(id);
            if (batchReqs.find(cellTag) == batchReqs.end()) {
                auto connection = ClusterDirectory->GetConnection(cellTag);
                auto channel = connection->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
                TObjectServiceProxy proxy(channel);
                batchReqs[cellTag] = proxy.ExecuteBatch();
            }

            auto checkReq = TObjectYPathProxy::GetBasicAttributes(FromObjectId(id));
            batchReqs[cellTag]->AddRequest(checkReq, "check_tx_" + ToString(id));
        }

        LOG_INFO("Refreshing transactions");

        yhash_map<TCellTag, NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr> batchRsps;

        for (const auto& pair : batchReqs) {
            auto cellTag = pair.first;
            const auto& batchReq = pair.second;
            auto batchRspOrError = WaitFor(batchReq->Invoke(), CancelableControlInvoker);
            if (batchRspOrError.IsOK()) {
                batchRsps[cellTag] = batchRspOrError.Value();
            } else {
                LOG_ERROR(batchRspOrError, "Error refreshing transactions (CellTag: %v)",
                    cellTag);
            }
        }

        yhash_set<TTransactionId> deadTransactionIds;

        for (const auto& id : watchSet) {
            auto cellTag = CellTagFromId(id);
            auto it = batchRsps.find(cellTag);
            if (it != batchRsps.end()) {
                const auto& batchRsp = it->second;
                auto rspOrError = batchRsp->GetResponse("check_tx_" + ToString(id));
                if (!rspOrError.IsOK()) {
                    deadTransactionIds.insert(id);
                }
            }
        }

        LOG_INFO("Transactions refreshed");

        auto isTransactionAlive = [&] (TOperationPtr operation, ITransactionPtr transaction) -> bool {
            if (!transaction) {
                return true;
            }

            if (deadTransactionIds.find(transaction->GetId()) == deadTransactionIds.end()) {
                return true;
            }

            return false;
        };

        auto isUserTransactionAlive = [&] (TOperationPtr operation, ITransactionPtr transaction) -> bool {
            if (isTransactionAlive(operation, transaction)) {
                return true;
            }

            LOG_INFO("Expired user transaction found (OperationId: %v, TransactionId: %v)",
                operation->GetId(),
                transaction->GetId());
            return false;
        };

        auto isSchedulerTransactionAlive = [&] (TOperationPtr operation, ITransactionPtr transaction) -> bool {
            if (isTransactionAlive(operation, transaction)) {
                return true;
            }

            LOG_INFO("Expired scheduler transaction found (OperationId: %v, TransactionId: %v)",
                operation->GetId(),
                transaction->GetId());
            return false;
        };

        // Check every operation's transactions and raise appropriate notifications.
        for (auto operation : operations) {
            if (!operation->GetHasActiveTransactions()) {
                continue;
            }

            if (!isUserTransactionAlive(operation, operation->GetUserTransaction())) {
                UserTransactionAborted_.Fire(operation);
            }

            if (!isSchedulerTransactionAlive(operation, operation->GetSyncSchedulerTransaction()) ||
                !isSchedulerTransactionAlive(operation, operation->GetAsyncSchedulerTransaction()) ||
                !isSchedulerTransactionAlive(operation, operation->GetInputTransaction()) ||
                !isSchedulerTransactionAlive(operation, operation->GetOutputTransaction()))
            {
                SchedulerTransactionAborted_.Fire(operation);
            }
        }
    }

    TUpdateList* CreateUpdateList(TOperationPtr operation)
    {
        LOG_DEBUG("Operation update list registered (OperationId: %v)",
            operation->GetId());
        auto channel = Bootstrap->GetMasterClient()->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TUpdateList list(channel, operation);
        auto pair = UpdateLists.insert(std::make_pair(operation->GetId(), list));
        YCHECK(pair.second);
        return &pair.first->second;
    }

    TUpdateList* FindUpdateList(const TOperationId& operationId)
    {
        auto it = UpdateLists.find(operationId);
        return it == UpdateLists.end() ? nullptr : &it->second;
    }

    TUpdateList* GetUpdateList(const TOperationId& operationId)
    {
        auto* result = FindUpdateList(operationId);
        YCHECK(result);
        return result;
    }

    void RemoveUpdateList(TOperationPtr operation)
    {
        LOG_DEBUG("Operation update list unregistered (OperationId: %v)",
            operation->GetId());
        YCHECK(UpdateLists.erase(operation->GetId()));
    }

    void ClearUpdateLists()
    {
        UpdateLists.clear();
    }


    TWatcherList* GetOrCreateWatcherList(TOperationPtr operation)
    {
        auto it = WatcherLists.find(operation->GetId());
        if (it == WatcherLists.end()) {
            it = WatcherLists.insert(std::make_pair(
                operation->GetId(),
                TWatcherList(operation))).first;
        }
        return &it->second;
    }

    TWatcherList* FindWatcherList(TOperationPtr operation)
    {
        auto it = WatcherLists.find(operation->GetId());
        return it == WatcherLists.end() ? nullptr : &it->second;
    }

    void ClearWatcherLists()
    {
        WatcherLists.clear();
    }

    void UpdateOperationNodes()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        LOG_INFO("Updating nodes for %v operations",
            UpdateLists.size());

        // Issue updates for active operations.
        std::vector<TOperationPtr> finishedOperations;
        std::vector<TFuture<void>> asyncResults;
        for (auto& pair : UpdateLists) {
            auto& list = pair.second;
            auto operation = list.Operation;
            if (operation->IsFinishedState()) {
                finishedOperations.push_back(operation);
            } else {
                LOG_DEBUG("Updating operation node (OperationId: %v)",
                    operation->GetId());

                asyncResults.push_back(UpdateOperationNode(&list).Apply(
                    BIND(&TImpl::OnOperationNodeUpdated, MakeStrong(this), operation)
                        .AsyncVia(CancelableControlInvoker)));
            }
        }

        // Cleanup finished operations.
        for (auto operation : finishedOperations) {
            RemoveUpdateList(operation);
        }

        auto result = WaitFor(Combine(asyncResults));
        if (!result.IsOK()) {
            LOG_ERROR(result, "Error updating operation nodes");
            Disconnect();
            return;
        }

        LOG_INFO("Operation nodes updated");
    }

    void OnOperationNodeUpdated(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        LOG_DEBUG("Operation node updated (OperationId: %v)",
            operation->GetId());
    }



    //void AddUpdateLivePreview(
    //    TObjectServiceProxy::TReqExecuteBatchPtr batchReq,
    //    std::vector<TLivePreviewRequest> livePreviewRequests)
    //{
    //    // Sort by chunk list.
    //    std::sort(
    //        livePreviewRequests.begin(),
    //        livePreviewRequests.end(),
    //        [] (const TLivePreviewRequest& lhs, const TLivePreviewRequest& rhs) {
    //            return lhs.ChunkListId < rhs.ChunkListId;
    //        });
    //
    //    // Group by chunk list.
    //    int rangeBegin = 0;
    //    while (rangeBegin < static_cast<int>(livePreviewRequests.size())) {
    //        int rangeEnd = rangeBegin; // non-inclusive
    //        while (rangeEnd < static_cast<int>(livePreviewRequests.size()) &&
    //               livePreviewRequests[rangeBegin].ChunkListId == livePreviewRequests[rangeEnd].ChunkListId)
    //        {
    //            ++rangeEnd;
    //        }
    //
    //        auto req = TChunkListYPathProxy::Attach(FromObjectId(livePreviewRequests[rangeBegin].ChunkListId));
    //        GenerateMutationId(req);
    //        for (int index = rangeBegin; index < rangeEnd; ++index) {
    //            ToProto(req->add_children_ids(), livePreviewRequests[index].ChildId);
    //        }
    //        batchReq->AddRequest(req, "update_live_preview");
    //
    //        rangeBegin = rangeEnd;
    //    }
    //}

    void UpdateOperationNodeAttributes(TOperationPtr operation)
    {
        auto batchReq = StartBatchRequest();
        auto state = operation->GetState();
        auto operationPath = GetOperationPath(operation->GetId());
        auto controller = operation->GetController();

        GenerateMutationId(batchReq);

        // Set suspended flag.
        {
            auto req = TYPathProxy::Set(operationPath + "/@suspended");
            req->set_value(ConvertToYsonString(operation->GetSuspended()).Data());
            batchReq->AddRequest(req, "update_op_node");
        }

        if ((state == EOperationState::Running || IsOperationFinished(state)) && controller) {
            // Set progress.
            {
                auto req = TYPathProxy::Set(operationPath + "/@progress");
                req->set_value(BuildYsonStringFluently()
                    .BeginMap()
                        .Do(BIND([=] (IYsonConsumer* consumer) {
                            WaitFor(
                                BIND(&IOperationController::BuildProgress, controller)
                                    .AsyncVia(controller->GetInvoker())
                                    .Run(consumer));
                        }))
                    .EndMap().Data());
                batchReq->AddRequest(req, "update_op_node");

            }
            // Set brief progress.
            {
                auto req = TYPathProxy::Set(operationPath + "/@brief_progress");
                req->set_value(BuildYsonStringFluently()
                    .BeginMap()
                        .Do(BIND([=] (IYsonConsumer* consumer) {
                            WaitFor(
                                BIND(&IOperationController::BuildBriefProgress, controller)
                                    .AsyncVia(controller->GetInvoker())
                                    .Run(consumer));
                        }))
                    .EndMap().Data());
                batchReq->AddRequest(req, "update_op_node");
            }
        }

        // Set result.
        if (operation->IsFinishedState() && controller) {
            auto req = TYPathProxy::Set(operationPath + "/@result");
            req->set_value(ConvertToYsonString(BIND(
                &IOperationController::BuildResult,
                controller)).Data());
            batchReq->AddRequest(req, "update_op_node");
        }

        // Set end time, if given.
        if (operation->GetFinishTime()) {
            auto req = TYPathProxy::Set(operationPath + "/@finish_time");
            req->set_value(ConvertToYsonString(operation->GetFinishTime().Get()).Data());
            batchReq->AddRequest(req, "update_op_node");
        }

        // Set state.
        {
            auto req = TYPathProxy::Set(operationPath + "/@state");
            req->set_value(ConvertToYsonString(operation->GetState()).Data());
            batchReq->AddRequest(req, "update_op_node");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
    }

    struct TJobFile
    {
        TJobId JobId;
        TYPath Path;
        TChunkId ChunkId;
        Stroka DescriptionType;
    };

    void SaveJobFiles(TOperationPtr operation, const std::vector<TJobFile>& files)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto client = Bootstrap->GetMasterClient();

        ITransactionPtr transaction;
        {
            NApi::TTransactionStartOptions options;
            options.PrerequisiteTransactionIds = {LockTransaction->GetId()};
            auto attributes = CreateEphemeralAttributes();
            attributes->Set("title", Format("Saving job files for operation %v", operation->GetId()));
            options.Attributes = std::move(attributes);

            transaction = WaitFor(client->StartTransaction(ETransactionType::Master, options))
                .ValueOrThrow();
        }

        const auto& transactionId = transaction->GetId();

        struct TJobFileInfo
        {
            TTransactionId UploadTransactionId;
            TNodeId NodeId;
            TChunkListId ChunkListId;
        };

        std::vector<TJobFileInfo> infos;

        {
            auto batchReq = StartBatchRequest();

            for (const auto& file : files) {
                {
                    auto req = TCypressYPathProxy::Create(file.Path);
                    req->set_recursive(true);
                    req->set_type(static_cast<int>(EObjectType::File));

                    auto attributes = CreateEphemeralAttributes();
                    attributes->Set("external", false);
                    attributes->Set("vital", false);
                    attributes->Set("replication_factor", 1);
                    attributes->Set("account", TmpAccountName);
                    attributes->Set(
                        "description", BuildYsonStringFluently()
                        .BeginMap()
                            .Item("type").Value(file.DescriptionType)
                            .Item("job_id").Value(file.JobId)
                        .EndMap());
                    ToProto(req->mutable_node_attributes(), *attributes);

                    SetTransactionId(req, transactionId);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req, "create");
                }
                {
                    auto req = TFileYPathProxy::BeginUpload(file.Path);
                    req->set_update_mode(static_cast<int>(EUpdateMode::Overwrite));
                    req->set_lock_mode(static_cast<int>(ELockMode::Exclusive));
                    GenerateMutationId(req);
                    SetTransactionId(req, transactionId);
                    batchReq->AddRequest(req, "begin_upload");
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            auto createRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspCreate>("create");
            auto beginUploadRsps = batchRsp->GetResponses<TFileYPathProxy::TRspBeginUpload>("begin_upload");
            for (int index = 0; index < files.size(); ++index) {
                infos.push_back(TJobFileInfo());
                auto& info = infos.back();

                {
                    const auto& rsp = createRsps[index].Value();
                    info.NodeId = FromProto<TNodeId>(rsp->node_id());
                }
                {
                    const auto& rsp = beginUploadRsps[index].Value();
                    info.UploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());
                }
            }
        }

        {
            auto batchReq = StartBatchRequest();

            for (const auto& info : infos) {
                auto req = TFileYPathProxy::GetUploadParams(FromObjectId(info.NodeId));
                SetTransactionId(req, info.UploadTransactionId);
                batchReq->AddRequest(req, "get_upload_params");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            auto getUploadParamsRsps = batchRsp->GetResponses<TFileYPathProxy::TRspGetUploadParams>("get_upload_params");
            for (int index = 0; index < getUploadParamsRsps.size(); ++index) {
                const auto& rsp = getUploadParamsRsps[index].Value();
                auto& info = infos[index];
                info.ChunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());
            }
        }

        {
            auto batchReq = StartBatchRequest();

            for (int index = 0; index < files.size(); ++index) {
                const auto& file = files[index];
                const auto& info = infos[index];

                {
                    auto req = TChunkListYPathProxy::Attach(FromObjectId(info.ChunkListId));
                    ToProto(req->add_children_ids(), file.ChunkId);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req, "attach");
                }
                {
                    auto req = TFileYPathProxy::EndUpload(FromObjectId(info.NodeId));
                    req->set_derive_statistics(true);
                    SetTransactionId(req, info.UploadTransactionId);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req, "end_upload");
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
        }

        WaitFor(transaction->Commit())
            .ThrowOnError();
    }

    void CreateJobNodes(
        TOperationPtr operation,
        std::vector<TJobRequest>& jobRequests)
    {
        auto batchReq = StartBatchRequest();

        for (const auto& request : jobRequests) {
            auto job = request.Job;
            auto jobPath = GetJobPath(operation->GetId(), job->GetId());
            auto req = TYPathProxy::Set(jobPath);
            req->set_value(
                BuildYsonStringFluently()
                    .BeginAttributes()
                        .Do(BIND(&BuildJobAttributes, job))
                    .EndAttributes()
                    .BeginMap()
                    .EndMap()
                    .Data());
            batchReq->AddRequest(req, "create");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
    }

    void DoUpdateOperationNode(
        TOperationPtr operation,
        std::vector<TJobRequest> jobRequests,
        std::vector<TLivePreviewRequest> livePreviewRequests)
    {
        try {
            CreateJobNodes(operation, jobRequests);

            std::vector<TJobFile> files;
            for (const auto& request : jobRequests) {
                if (request.StderrChunkId) {
                    files.push_back({
                        request.Job->GetId(),
                        GetStderrPath(operation->GetId(), request.Job->GetId()),
                        request.StderrChunkId,
                        "stderr"});
                }
                if (request.FailContextChunkId) {
                    files.push_back({
                        request.Job->GetId(),
                        GetFailContextPath(operation->GetId(), request.Job->GetId()),
                        request.FailContextChunkId,
                        "fail_context"});
                }
            }
            SaveJobFiles(operation, files);
            // NB: Update operation attributes after updating all job nodes.
            // Tests assume, that all job files are present, when operation 
            // is in one of the terminal states.
            UpdateOperationNodeAttributes(operation);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error updating operation node %v",
                operation->GetId())
                << ex;
        }
    }

    TFuture<void> UpdateOperationNode(TUpdateList* list)
    {
        auto operation = list->Operation;

        auto lastUpdateFuture = list->LastUpdateFuture.Apply(
            BIND(
                &TImpl::DoUpdateOperationNode,
                MakeStrong(this),
                operation,
                Passed(std::move(list->JobRequests)),
                Passed(std::move(list->LivePreviewRequests)))
            .AsyncVia(CancelableControlInvoker));

        list->LastUpdateFuture = lastUpdateFuture;
        return lastUpdateFuture;
    }

    void OnOperationNodeCreated(
        TOperationPtr operation,
        const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto operationId = operation->GetId();
        auto error = GetCumulativeError(batchRspOrError);
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Error creating operation node %v",
            operationId);

        CreateUpdateList(operation);

        LOG_INFO("Operation node created (OperationId: %v)",
            operationId);
    }

    void OnRevivingOperationNodeReset(
        TOperationPtr operation,
        const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        auto operationId = operation->GetId();

        auto error = GetCumulativeError(batchRspOrError);
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Error resetting reviving operation node %v",
            operationId);

        LOG_INFO("Reviving operation node reset (OperationId: %v)",
            operationId);
    }

    void OnOperationNodeFlushed(
        TOperationPtr operation,
        const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        auto operationId = operation->GetId();

        if (!error.IsOK()) {
            LOG_ERROR(error);
            Disconnect();
            return;
        }

        LOG_INFO("Operation node flushed (OperationId: %v)",
            operationId);
    }


    void UpdateWatchers()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        LOG_INFO("Updating watchers");

        // Global watchers.
        {
            auto batchReq = StartBatchRequest();
            for (auto requester : GlobalWatcherRequesters) {
                requester.Run(batchReq);
            }
            batchReq->Invoke().Subscribe(
                BIND(&TImpl::OnGlobalWatchersUpdated, MakeStrong(this))
                    .Via(CancelableControlInvoker));
        }

        // Purge obsolete watchers.
        {
            auto it = WatcherLists.begin();
            while (it != WatcherLists.end()) {
                auto jt = it++;
                const auto& list = jt->second;
                if (list.Operation->IsFinishedState()) {
                    WatcherLists.erase(jt);
                }
            }
        }

        // Per-operation watchers.
        for (const auto& pair : WatcherLists) {
            const auto& list = pair.second;
            auto operation = list.Operation;
            if (operation->GetState() != EOperationState::Running)
                continue;

            auto batchReq = StartBatchRequest();
            for (auto requester : list.WatcherRequesters) {
                requester.Run(batchReq);
            }
            batchReq->Invoke().Subscribe(
                BIND(&TImpl::OnOperationWatchersUpdated, MakeStrong(this), operation)
                    .Via(CancelableControlInvoker));
        }
    }

    void OnGlobalWatchersUpdated(const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        if (!batchRspOrError.IsOK()) {
            LOG_ERROR(batchRspOrError, "Error updating global watchers");
            return;
        }

        const auto& batchRsp = batchRspOrError.Value();
        for (auto handler : GlobalWatcherHandlers) {
            handler.Run(batchRsp);
        }

        LOG_INFO("Global watchers updated");
    }

    void OnOperationWatchersUpdated(TOperationPtr operation, const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        if (!batchRspOrError.IsOK()) {
            LOG_ERROR(batchRspOrError, "Error updating operation watchers (OperationId: %v)",
                operation->GetId());
            return;
        }

        if (operation->GetState() != EOperationState::Running)
            return;

        auto* list = FindWatcherList(operation);
        if (!list)
            return;

        const auto& batchRsp = batchRspOrError.Value();
        for (auto handler : list->WatcherHandlers) {
            handler.Run(batchRsp);
        }

        LOG_INFO("Operation watchers updated (OperationId: %v)",
            operation->GetId());
    }


    void BuildSnapshot()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!Config->EnableSnapshotBuilding)
            return;

        auto builder = New<TSnapshotBuilder>(
            Config,
            Bootstrap->GetScheduler(),
            Bootstrap->GetMasterClient());

        // NB: Result is logged in the builder.
        auto error = WaitFor(builder->Run());
        if (!error.IsOK()) {
            LOG_ERROR(error, "Error building snapshots");
        }
    }


    void UpdateClusterDirectory()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto asyncRspOrError = Proxy.Execute(TYPathProxy::Get("//sys/clusters"));
        auto rspOrError = WaitFor(asyncRspOrError);

        if (!rspOrError.IsOK()) {
            LOG_WARNING(rspOrError, "Error requesting cluster directory");
            return;
        }

        try {
            const auto& rsp = rspOrError.Value();
            auto clustersNode = ConvertToNode(TYsonString(rsp->value()))->AsMap();

            for (auto name : ClusterDirectory->GetClusterNames()) {
                if (!clustersNode->FindChild(name)) {
                    ClusterDirectory->RemoveCluster(name);
                }
            }

            for (const auto& pair : clustersNode->GetChildren()) {
                const auto& clusterName = pair.first;
                auto config = ConvertTo<NApi::TConnectionConfigPtr>(pair.second);
                ClusterDirectory->UpdateCluster(clusterName, config);
            }

            LOG_DEBUG("Cluster directory updated successfully");
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error updating cluster directory");
        }
    }

    void DoAttachToLivePreview(
        TOperationPtr operation,
        const TChunkListId& chunkListId,
        const std::vector<TChunkTreeId>& childrenIds)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected);

        LOG_DEBUG("Attaching live preview chunk trees (OperationId: %v, ChunkListId: %v, ChildrenCount: %v)",
                  operation->GetId(),
                  chunkListId,
                  childrenIds.size());

        auto* list = GetUpdateList(operation->GetId());
        for (const auto& childId : childrenIds) {
            TLivePreviewRequest request;
            request.ChunkListId = chunkListId;
            request.ChildId = childId;
            list->LivePreviewRequests.push_back(request);
        }
    }
};

////////////////////////////////////////////////////////////////////

TMasterConnector::TMasterConnector(
    TSchedulerConfigPtr config,
    NCellScheduler::TBootstrap* bootstrap)
    : Impl(New<TImpl>(config, bootstrap))
{ }

TMasterConnector::~TMasterConnector()
{ }

void TMasterConnector::Start()
{
    Impl->Start();
}

bool TMasterConnector::IsConnected() const
{
    return Impl->IsConnected();
}

IInvokerPtr TMasterConnector::GetCancelableControlInvoker() const
{
    return Impl->GetCancelableControlInvoker();
}

TFuture<void> TMasterConnector::CreateOperationNode(TOperationPtr operation)
{
    return Impl->CreateOperationNode(operation);
}

TFuture<void> TMasterConnector::ResetRevivingOperationNode(TOperationPtr operation)
{
    return Impl->ResetRevivingOperationNode(operation);
}

TFuture<void> TMasterConnector::FlushOperationNode(TOperationPtr operation)
{
    return Impl->FlushOperationNode(operation);
}

void TMasterConnector::CreateJobNode(
    TJobPtr job,
    const TChunkId& stderrChunkId,
    const TChunkId& failContextChunkId)
{
    return Impl->CreateJobNode(job, stderrChunkId, failContextChunkId);
}

void TMasterConnector::AttachJobContext(
    const TYPath& path,
    const TChunkId& chunkId,
    TJobPtr job)
{
    return Impl->AttachJobContext(path, chunkId, job);
}

TFuture<void> TMasterConnector::AttachToLivePreview(
    TOperationPtr operation,
    const TChunkListId& chunkListId,
    const std::vector<TChunkTreeId>& childrenIds)
{
    return Impl->AttachToLivePreview(operation, chunkListId, childrenIds);
}

void TMasterConnector::AddGlobalWatcherRequester(TWatcherRequester requester)
{
    Impl->AddGlobalWatcherRequester(requester);
}

void TMasterConnector::AddGlobalWatcherHandler(TWatcherHandler handler)
{
    Impl->AddGlobalWatcherHandler(handler);
}

void TMasterConnector::AddOperationWatcherRequester(TOperationPtr operation, TWatcherRequester requester)
{
    Impl->AddOperationWatcherRequester(operation, requester);
}

void TMasterConnector::AddOperationWatcherHandler(TOperationPtr operation, TWatcherHandler handler)
{
    Impl->AddOperationWatcherHandler(operation, handler);
}

DELEGATE_SIGNAL(TMasterConnector, void(const TMasterHandshakeResult& result), MasterConnected, *Impl);
DELEGATE_SIGNAL(TMasterConnector, void(), MasterDisconnected, *Impl);
DELEGATE_SIGNAL(TMasterConnector, void(TOperationPtr operation), UserTransactionAborted, *Impl)
DELEGATE_SIGNAL(TMasterConnector, void(TOperationPtr operation), SchedulerTransactionAborted, *Impl)

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

