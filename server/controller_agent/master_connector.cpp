#include "master_connector.h"
#include "operation_controller.h"
#include "snapshot_downloader.h"
#include "snapshot_builder.h"
#include "serialize.h"

#include <yt/server/scheduler/config.h>

#include <yt/server/cell_scheduler/bootstrap.h>
#include <yt/server/cell_scheduler/config.h>

#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/hive/cluster_directory.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/table_client/table_ypath_proxy.h>

#include <yt/ytlib/file_client/file_ypath_proxy.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/ytlib/scheduler/update_executor.h>

#include <yt/core/concurrency/periodic_executor.h>

namespace NYT {
namespace NControllerAgent {

using namespace NApi;
using namespace NRpc;
using namespace NYTree;
using namespace NYson;
using namespace NConcurrency;
using namespace NHiveClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NTableClient;
using namespace NFileClient;
using namespace NSecurityClient;
using namespace NTransactionClient;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////

static const auto& Logger = MasterConnectorLogger;

////////////////////////////////////////////////////////////////////

class TMasterConnector::TImpl
    : public TRefCounted
{
public:
    TImpl(
        IInvokerPtr invoker,
        TSchedulerConfigPtr config,
        NCellScheduler::TBootstrap* bootstrap)
        : Invoker_(invoker)
        , Config_(config)
        , Bootstrap_(bootstrap)
        , OperationNodesUpdateExecutor_(
            BIND(&TImpl::UpdateOperationNode, MakeStrong(this)),
            BIND(&TImpl::IsOperationInFinishedState, MakeStrong(this)),
            Logger)
        , TransactionRefreshExecutor_(New<TPeriodicExecutor>(
            Invoker_,
            BIND(&TImpl::RefreshTransactions, MakeStrong(this)),
            Config_->TransactionsRefreshPeriod,
            EPeriodicExecutorMode::Automatic))
        , SnapshotExecutor_(New<TPeriodicExecutor>(
            Invoker_,
            BIND(&TImpl::BuildSnapshot, MakeStrong(this)),
            Config_->SnapshotPeriod,
            EPeriodicExecutorMode::Automatic))
    {
        OperationNodesUpdateExecutor_.StartPeriodicUpdates(
            Invoker_,
            Config_->OperationsUpdatePeriod);
        TransactionRefreshExecutor_->Start();
        SnapshotExecutor_->Start();
    }

    const IInvokerPtr& GetInvoker() const
    {
        return Invoker_;
    }

    void RegisterOperation(
        const TOperationId& operationId,
        const IOperationControllerPtr& controller)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YCHECK(Controllers_.emplace(operationId, controller).second);
        OperationNodesUpdateExecutor_.AddUpdate(operationId, TOperationNodeUpdate(operationId));
    }

    void UnregisterOperation(const TOperationId& operationId)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        // NB: from OperationNodesUpdateExecutor_ operation will be removed by periodic update executor.
        // NB: Method can be called more than one time.
        {
            TGuard<TSpinLock> guard(ControllersLock_);
            Controllers_.erase(operationId);
        }
    }

    void CreateJobNode(TCreateJobNodeRequest createJobNodeRequest)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        LOG_DEBUG("Creating job node (OperationId: %v, JobId: %v, StderrChunkId: %v, FailContextChunkId: %v)",
            createJobNodeRequest.OperationId,
            createJobNodeRequest.JobId,
            createJobNodeRequest.StderrChunkId,
            createJobNodeRequest.FailContextChunkId);

        Invoker_->Invoke(BIND([this, this_ = MakeStrong(this), request = std::move(createJobNodeRequest)] {
            auto* updateParameters = OperationNodesUpdateExecutor_.GetUpdate(request.OperationId);
            updateParameters->JobRequests.emplace_back(std::move(request));
        }));
    }

    TFuture<void> FlushOperationNode(const TOperationId& operationId)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        LOG_INFO("Invoked flushing controller attributes of operation (OperationId: %v)",
            operationId);

        return BIND([=] {
                WaitFor(OperationNodesUpdateExecutor_.ExecuteUpdate(operationId))
                    .ThrowOnError();
            })
            .AsyncVia(Invoker_)
            .Run();
    }

    TFuture<void> AttachToLivePreview(
        const TOperationId& operationId,
        const TTransactionId& transactionId,
        const TNodeId& tableId,
        const std::vector<TChunkTreeId>& childIds)
    {
        return BIND(&TImpl::DoAttachToLivePreview, MakeStrong(this))
            .AsyncVia(Invoker_)
            .Run(operationId, transactionId, tableId, childIds);
    }

    TFuture<TOperationSnapshot> DownloadSnapshot(const TOperationId& operationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!Config_->EnableSnapshotLoading) {
            return MakeFuture<TOperationSnapshot>(TError("Snapshot loading is disabled in configuration"));
        }

        return BIND(&TImpl::DoDownloadSnapshot, MakeStrong(this), operationId)
            .AsyncVia(Invoker_)
            .Run();
    }

    TFuture<void> RemoveSnapshot(const TOperationId& operationId)
    {
        return BIND(&TImpl::DoRemoveSnapshot, MakeStrong(this), operationId)
            .AsyncVia(Invoker_)
            .Run();
    }

    void AttachJobContext(
        const TYPath& path,
        const TChunkId& chunkId,
        const TOperationId& operationId,
        const TJobId& jobId)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        YCHECK(chunkId);

        try {
            TJobFile file{
                jobId,
                path,
                chunkId,
                "input_context"
            };
            SaveJobFiles(operationId, { file });
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error saving input context for job %v into %v", jobId, path)
                << ex;
        }
    }

    void UpdateConfig(const TSchedulerConfigPtr& config)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        Config_ = config;
    }

private:
    IInvokerPtr Invoker_;
    TSchedulerConfigPtr Config_;
    NCellScheduler::TBootstrap* const Bootstrap_;

    TSpinLock ControllersLock_;
    yhash<TOperationId, IOperationControllerPtr> Controllers_;

    struct TLivePreviewRequest
    {
        TChunkListId TableId;
        TChunkTreeId ChildId;
    };

    struct TJobFile
    {
        TJobId JobId;
        TYPath Path;
        TChunkId ChunkId;
        Stroka DescriptionType;
    };

    struct TOperationNodeUpdate
    {
        explicit TOperationNodeUpdate(const TOperationId& operationId)
            : OperationId(operationId)
        { }

        TOperationId OperationId;
        TTransactionId TransactionId;
        std::vector<TCreateJobNodeRequest> JobRequests;
        std::vector<TLivePreviewRequest> LivePreviewRequests;
    };

    TUpdateExecutor<TOperationId, TOperationNodeUpdate> OperationNodesUpdateExecutor_;

    TPeriodicExecutorPtr TransactionRefreshExecutor_;
    TPeriodicExecutorPtr SnapshotExecutor_;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

    TObjectServiceProxy::TReqExecuteBatchPtr StartObjectBatchRequest(
        EMasterChannelKind channelKind = EMasterChannelKind::Leader,
        TCellTag cellTag = PrimaryMasterCellTag)
    {
        TObjectServiceProxy proxy(Bootstrap_
            ->GetMasterClient()
            ->GetMasterChannelOrThrow(channelKind, cellTag));
        auto batchReq = proxy.ExecuteBatch();
        return batchReq;
    }

    TChunkServiceProxy::TReqExecuteBatchPtr StartChunkBatchRequest(TCellTag cellTag = PrimaryMasterCellTag)
    {
        TChunkServiceProxy proxy(Bootstrap_
            ->GetMasterClient()
            ->GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag));
        return proxy.ExecuteBatch();
    }

    INativeConnectionPtr FindConnection(TCellTag cellTag)
    {
        auto localConnection = Bootstrap_->GetMasterClient()->GetNativeConnection();
        return cellTag == localConnection->GetCellTag()
            ? localConnection
            : Bootstrap_->GetClusterDirectory()->FindConnection(cellTag);
    }

    void RefreshTransactions()
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        // Collect all transactions that are used by currently running operations.
        yhash_set<TTransactionId> watchSet;

        {
            TGuard<TSpinLock> guard(ControllersLock_);
            for (auto pair : Controllers_) {
                const auto& controller = pair.second;
                for (const auto& transaction : controller->GetTransactions()) {
                    watchSet.insert(transaction->GetId());
                }
            }
        }

        yhash<TCellTag, TObjectServiceProxy::TReqExecuteBatchPtr> batchReqs;

        for (const auto& id : watchSet) {
            auto cellTag = CellTagFromId(id);
            if (batchReqs.find(cellTag) == batchReqs.end()) {
                auto connection = FindConnection(cellTag);
                if (!connection) {
                    continue;
                }
                auto channel = connection->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
                auto authenticatedChannel = CreateAuthenticatedChannel(channel, SchedulerUserName);
                TObjectServiceProxy proxy(authenticatedChannel);
                batchReqs[cellTag] = proxy.ExecuteBatch();
            }

            auto checkReq = TObjectYPathProxy::GetBasicAttributes(FromObjectId(id));
            batchReqs[cellTag]->AddRequest(checkReq, "check_tx_" + ToString(id));
        }

        LOG_INFO("Refreshing transactions");

        yhash<TCellTag, NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr> batchRsps;

        for (const auto& pair : batchReqs) {
            auto cellTag = pair.first;
            const auto& batchReq = pair.second;
            auto batchRspOrError = WaitFor(batchReq->Invoke());
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

        // Check every operation's transactions and raise appropriate notifications.
        {
            TGuard<TSpinLock> guard(ControllersLock_);
            for (auto pair : Controllers_) {
                const auto& controller = pair.second;
                for (const auto& transaction : controller->GetTransactions()) {
                    if (deadTransactionIds.find(transaction->GetId()) != deadTransactionIds.end()) {
                        controller->OnTransactionAborted(transaction->GetId());
                        break;
                    }
                }
            }
        }
    }

    void DoUpdateOperationNode(
        const TOperationId& operationId,
        const TTransactionId& transactionId,
        const std::vector<TCreateJobNodeRequest> jobRequests,
        const std::vector<TLivePreviewRequest> livePreviewRequests)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        try {
            CreateJobNodes(operationId, jobRequests);
        } catch (const std::exception& ex) {
            auto error = TError("Error creating job nodes for operation %v",
                operationId)
                << ex;
            if (error.FindMatching(NSecurityClient::EErrorCode::AccountLimitExceeded)) {
                LOG_DEBUG(error);
                return;
            } else {
                THROW_ERROR error;
            }
        }

        try {
            std::vector<TJobFile> files;
            for (const auto& request : jobRequests) {
                if (request.StderrChunkId) {
                    files.push_back({
                        request.JobId,
                        GetStderrPath(operationId, request.JobId),
                        request.StderrChunkId,
                        "stderr"
                    });
                }
                if (request.FailContextChunkId) {
                    files.push_back({
                        request.JobId,
                        GetFailContextPath(operationId, request.JobId),
                        request.FailContextChunkId,
                        "fail_context"
                    });
                }
            }
            SaveJobFiles(operationId, files);
        } catch (const std::exception& ex) {
            // NB: Don' treat this as a critical error.
            // Some of these chunks could go missing for a number of reasons.
            LOG_WARNING(ex, "Error saving job files (OperationId: %v)",
                operationId);
        }

        try {
            AttachLivePreviewChunks(operationId, transactionId, livePreviewRequests);
        } catch (const std::exception& ex) {
            // NB: Don' treat this as a critical error.
            // Some of these chunks could go missing for a number of reasons.
            LOG_WARNING(ex, "Error attaching live preview chunks (OperationId: %v)",
                operationId);
        }

        try {
            UpdateOperationNodeAttributes(operationId);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error updating operation node %v",
                operationId)
                << ex;
        }
    }

    TCallback<TFuture<void>()> UpdateOperationNode(const TOperationId& operationId, TOperationNodeUpdate* update)
    {
        return BIND(&TImpl::DoUpdateOperationNode,
            MakeStrong(this),
            operationId,
            update->TransactionId,
            Passed(std::move(update->JobRequests)),
            Passed(std::move(update->LivePreviewRequests)))
            .AsyncVia(Invoker_);
    }

    void UpdateOperationNodeAttributes(const TOperationId& operationId)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        auto batchReq = StartObjectBatchRequest();
        auto operationPath = GetOperationPath(operationId);
        auto controller = GetOperationController(operationId);
        if (!controller || !controller->HasProgress()) {
            return;
        }

        GenerateMutationId(batchReq);

        // Set progress.
        {
            auto progress = controller->GetProgress();
            YCHECK(progress);

            auto req = TYPathProxy::Set(operationPath + "/@progress");
            req->set_value(progress.GetData());
            batchReq->AddRequest(req, "update_op_node");
        }
        // Set brief progress.
        {
            auto progress = controller->GetBriefProgress();
            YCHECK(progress);

            auto req = TYPathProxy::Set(operationPath + "/@brief_progress");
            req->set_value(progress.GetData());
            batchReq->AddRequest(req, "update_op_node");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
    }

    void CreateJobNodes(
        const TOperationId& operationId,
        const std::vector<TCreateJobNodeRequest>& jobRequests)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        auto batchReq = StartObjectBatchRequest();

        for (const auto& request : jobRequests) {
            const auto& jobId = request.JobId;
            auto jobPath = GetJobPath(operationId, jobId);

            auto attributes = ConvertToAttributes(request.Attributes);
            auto req = TCypressYPathProxy::Create(jobPath);
            req->set_type(static_cast<int>(EObjectType::MapNode));
            ToProto(req->mutable_node_attributes(), *attributes);
            batchReq->AddRequest(req, "create");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        auto error = GetCumulativeError(batchRspOrError);
        if (!error.IsOK()) {
            if (error.FindMatching(NSecurityClient::EErrorCode::AccountLimitExceeded)) {
                LOG_ERROR(error, "Account limit exceeded while creating job nodes");
            } else {
                THROW_ERROR_EXCEPTION("Failed to create job nodes") << error;
            }
        }

        LOG_INFO("Created %v job nodes (OperationId: %v)", jobRequests.size(), operationId);
    }

    void AttachLivePreviewChunks(
        const TOperationId& operationId,
        const TTransactionId& transactionId,
        const std::vector<TLivePreviewRequest>& livePreviewRequests)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        struct TTableInfo
        {
            TNodeId TableId;
            TCellTag CellTag;
            std::vector<TChunkId> ChildIds;
            TTransactionId UploadTransactionId;
            TChunkListId UploadChunkListId;
            NChunkClient::NProto::TDataStatistics Statistics;
        };

        yhash<TNodeId, TTableInfo> tableIdToInfo;
        for (const auto& request : livePreviewRequests) {
            auto& tableInfo = tableIdToInfo[request.TableId];
            tableInfo.TableId = request.TableId;
            tableInfo.ChildIds.push_back(request.ChildId);

            LOG_DEBUG("Appending live preview chunk trees (OperationId: %v, TableId: %v, ChildCount: %v)",
                operationId,
                tableInfo.TableId,
                tableInfo.ChildIds.size());
        }

        if (tableIdToInfo.empty()) {
            return;
        }

        // BeginUpload
        {
            auto batchReq = StartObjectBatchRequest();

            for (const auto& pair : tableIdToInfo) {
                const auto& tableId = pair.first;
                {
                    auto req = TTableYPathProxy::BeginUpload(FromObjectId(tableId));
                    req->set_update_mode(static_cast<int>(EUpdateMode::Append));
                    req->set_lock_mode(static_cast<int>(ELockMode::Shared));
                    req->set_upload_transaction_title(Format("Attaching live preview chunks of operation %v",
                        operationId));
                    SetTransactionId(req, transactionId);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req, "begin_upload");
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            auto rsps = batchRsp->GetResponses<TChunkOwnerYPathProxy::TRspBeginUpload>("begin_upload");
            int rspIndex = 0;
            for (auto& pair : tableIdToInfo) {
                auto& tableInfo = pair.second;
                const auto& rsp = rsps[rspIndex++].Value();
                tableInfo.CellTag = rsp->cell_tag();
                tableInfo.UploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());
            }
        }

        yhash<TCellTag, std::vector<TTableInfo*>> cellTagToInfos;
        for (auto& pair : tableIdToInfo) {
            auto& tableInfo  = pair.second;
            cellTagToInfos[tableInfo.CellTag].push_back(&tableInfo);
        }

        // GetUploadParams
        for (auto& pair : cellTagToInfos) {
            auto cellTag = pair.first;
            auto& tableInfos = pair.second;

            auto batchReq = StartObjectBatchRequest(EMasterChannelKind::Follower, cellTag);
            for (const auto* tableInfo : tableInfos) {
                auto req = TTableYPathProxy::GetUploadParams(FromObjectId(tableInfo->TableId));
                SetTransactionId(req, tableInfo->UploadTransactionId);
                batchReq->AddRequest(req, "get_upload_params");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            auto rsps = batchRsp->GetResponses<TTableYPathProxy::TRspGetUploadParams>("get_upload_params");
            int rspIndex = 0;
            for (auto* tableInfo : tableInfos) {
                const auto& rsp = rsps[rspIndex++].Value();
                tableInfo->UploadChunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());
            }
        }

        // Attach
        for (auto& pair : cellTagToInfos) {
            auto cellTag = pair.first;
            auto& tableInfos = pair.second;

            auto batchReq = StartChunkBatchRequest(cellTag);
            GenerateMutationId(batchReq);
            batchReq->set_suppress_upstream_sync(true);

            std::vector<int> tableIndexToRspIndex;
            for (const auto* tableInfo : tableInfos) {
                size_t beginIndex = 0;
                const auto& childIds = tableInfo->ChildIds;
                while (beginIndex < childIds.size()) {
                    auto lastIndex = std::min(beginIndex + Config_->MaxChildrenPerAttachRequest, childIds.size());
                    bool isFinal = (lastIndex == childIds.size());
                    if (isFinal) {
                        tableIndexToRspIndex.push_back(batchReq->attach_chunk_trees_subrequests_size());
                    }
                    auto* req = batchReq->add_attach_chunk_trees_subrequests();
                    ToProto(req->mutable_parent_id(), tableInfo->UploadChunkListId);
                    for (auto index = beginIndex; index < lastIndex; ++index) {
                        ToProto(req->add_child_ids(), childIds[index]);
                    }
                    req->set_request_statistics(isFinal);
                    beginIndex = lastIndex;
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            const auto& rsps = batchRsp->attach_chunk_trees_subresponses();
            for (int tableIndex = 0; tableIndex < tableInfos.size(); ++tableIndex) {
                auto* tableInfo = tableInfos[tableIndex];
                const auto& rsp = rsps.Get(tableIndexToRspIndex[tableIndex]);
                tableInfo->Statistics = rsp.statistics();
            }
        }

        // EndUpload
        {
            auto batchReq = StartObjectBatchRequest();

            for (const auto& pair : tableIdToInfo) {
                const auto& tableId = pair.first;
                const auto& tableInfo = pair.second;
                {
                    auto req = TTableYPathProxy::EndUpload(FromObjectId(tableId));
                    *req->mutable_statistics() = tableInfo.Statistics;
                    SetTransactionId(req, tableInfo.UploadTransactionId);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req, "end_upload");
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
        }
    }

    void DoAttachToLivePreview(
        const TOperationId& operationId,
        const TTransactionId& transactionId,
        const TNodeId& tableId,
        const std::vector<TChunkTreeId>& childIds)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        auto* list = OperationNodesUpdateExecutor_.FindUpdate(operationId);
        if (!list) {
            LOG_DEBUG("Operation node is not registered, omitting live preview attach (OperationId: %v)",
                operationId);
            return;
        }

        if (!list->TransactionId) {
            list->TransactionId = transactionId;
        } else {
            // NB: Controller must attach all live preview chunks under the same transaction.
            YCHECK(list->TransactionId == transactionId);
        }

        LOG_TRACE("Attaching live preview chunk trees (OperationId: %v, TableId: %v, ChildCount: %v)",
            operationId,
            tableId,
            childIds.size());

        for (const auto& childId : childIds) {
            list->LivePreviewRequests.push_back(TLivePreviewRequest{tableId, childId});
        }
    }

    TOperationSnapshot DoDownloadSnapshot(const TOperationId& operationId)
    {
        auto snapshotPath = NScheduler::GetSnapshotPath(operationId);

        auto batchReq = StartObjectBatchRequest();
        auto req = TYPathProxy::Get(snapshotPath + "/@version");
        batchReq->AddRequest(req, "get_version");

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        const auto& batchRsp = batchRspOrError.ValueOrThrow();

        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_version");
        // Check for missing snapshots.
        if (rspOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
            THROW_ERROR_EXCEPTION("Snapshot does not exist");
        }
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting snapshot version");

        const auto& rsp = rspOrError.Value();
        int version = ConvertTo<int>(TYsonString(rsp->value()));

        LOG_INFO("Snapshot found (OperationId: %v, Version: %v)",
            operationId,
            version);

        if (!ValidateSnapshotVersion(version)) {
            THROW_ERROR_EXCEPTION("Snapshot version validation failed");
        }

        TOperationSnapshot snapshot;
        snapshot.Version = version;
        try {
            auto downloader = New<TSnapshotDownloader>(
                Config_,
                Bootstrap_,
                operationId);
            snapshot.Data = downloader->Run();
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error downloading snapshot") << ex;
        }
        return snapshot;
    }

    void DoRemoveSnapshot(const TOperationId& operationId)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        auto batchReq = StartObjectBatchRequest();
        auto req = TYPathProxy::Remove(NScheduler::GetSnapshotPath(operationId));
        req->set_force(true);
        batchReq->AddRequest(req, "remove_snapshot");

        auto batchRspOrError = WaitFor(batchReq->Invoke());

        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
    }

    void SaveJobFiles(const TOperationId& operationId, const std::vector<TJobFile>& files)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        auto client = Bootstrap_->GetMasterClient();
        auto connection = client->GetNativeConnection();

        ITransactionPtr transaction;
        {
            NApi::TTransactionStartOptions options;
            auto attributes = CreateEphemeralAttributes();
            attributes->Set("title", Format("Saving job files of operation %v", operationId));
            options.Attributes = std::move(attributes);

            transaction = WaitFor(client->StartTransaction(ETransactionType::Master, options))
                .ValueOrThrow();
        }

        const auto& transactionId = transaction->GetId();

        yhash<TCellTag, std::vector<TJobFile>> cellTagToFiles;
        for (const auto& file : files) {
            cellTagToFiles[CellTagFromId(file.ChunkId)].push_back(file);
        }

        for (const auto& pair : cellTagToFiles) {
            auto cellTag = pair.first;
            const auto& perCellFiles = pair.second;

            struct TJobFileInfo
            {
                TTransactionId UploadTransactionId;
                TNodeId NodeId;
                TChunkListId ChunkListId;
                NChunkClient::NProto::TDataStatistics Statistics;
            };

            std::vector<TJobFileInfo> infos;

            {
                auto batchReq = StartObjectBatchRequest();

                for (const auto& file : perCellFiles) {
                    {
                        auto req = TCypressYPathProxy::Create(file.Path);
                        req->set_recursive(true);
                        req->set_type(static_cast<int>(EObjectType::File));

                        auto attributes = CreateEphemeralAttributes();
                        if (cellTag == connection->GetPrimaryMasterCellTag()) {
                            attributes->Set("external", false);
                        } else {
                            attributes->Set("external_cell_tag", cellTag);
                        }
                        attributes->Set("vital", false);
                        attributes->Set("replication_factor", 1);
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
                        req->set_upload_transaction_title(Format("Saving files of job %v of operation %v",
                            file.JobId,
                            operationId));
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
                for (int index = 0; index < perCellFiles.size(); ++index) {
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
                auto batchReq = StartObjectBatchRequest(EMasterChannelKind::Follower, cellTag);

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
                auto batchReq = StartChunkBatchRequest(cellTag);
                GenerateMutationId(batchReq);
                batchReq->set_suppress_upstream_sync(true);

                for (int index = 0; index < perCellFiles.size(); ++index) {
                    const auto& file = perCellFiles[index];
                    const auto& info = infos[index];
                    auto* req = batchReq->add_attach_chunk_trees_subrequests();
                    ToProto(req->mutable_parent_id(), info.ChunkListId);
                    ToProto(req->add_child_ids(), file.ChunkId);
                    req->set_request_statistics(true);
                }

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
                const auto& batchRsp = batchRspOrError.Value();

                for (int index = 0; index < perCellFiles.size(); ++index) {
                    auto& info = infos[index];
                    const auto& rsp = batchRsp->attach_chunk_trees_subresponses(index);
                    info.Statistics = rsp.statistics();
                }
            }

            {
                auto batchReq = StartObjectBatchRequest();

                for (int index = 0; index < perCellFiles.size(); ++index) {
                    const auto& info = infos[index];
                    auto req = TFileYPathProxy::EndUpload(FromObjectId(info.NodeId));
                    *req->mutable_statistics() = info.Statistics;
                    SetTransactionId(req, info.UploadTransactionId);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req);
                }

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            }
        }

        WaitFor(transaction->Commit())
            .ThrowOnError();
    }

    void BuildSnapshot()
    {
        if (!Config_->EnableSnapshotBuilding)
            return;

        auto builder = New<TSnapshotBuilder>(
            Config_,
            Bootstrap_->GetScheduler(),
            Bootstrap_->GetMasterClient());

        // NB: Result is logged in the builder.
        auto error = WaitFor(builder->Run());
        if (error.IsOK()) {
            LOG_INFO("Snapshot builder finished");
        } else {
            LOG_ERROR(error, "Error building snapshots");
        }
    }

    IOperationControllerPtr GetOperationController(const TOperationId& operationId) const
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        TGuard<TSpinLock> guard(ControllersLock_);

        auto it = Controllers_.find(operationId);
        if (it == Controllers_.end()) {
            return nullptr;
        } else {
            return it->second;
        }
    }

    bool IsOperationInFinishedState(const TOperationNodeUpdate* update) const
    {
        return !GetOperationController(update->OperationId);
    }
};

////////////////////////////////////////////////////////////////////

TMasterConnector::TMasterConnector(
    IInvokerPtr invoker,
    TSchedulerConfigPtr config,
    NCellScheduler::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(invoker, config, bootstrap))
{ }

const IInvokerPtr& TMasterConnector::GetInvoker() const
{
    return Impl_->GetInvoker();
}

void TMasterConnector::RegisterOperation(
    const TOperationId& operationId,
    const IOperationControllerPtr& controller)
{
    Impl_->RegisterOperation(operationId, controller);
}

void TMasterConnector::UnregisterOperation(const TOperationId& operationId)
{
    Impl_->UnregisterOperation(operationId);
}

void TMasterConnector::CreateJobNode(TCreateJobNodeRequest createJobNodeRequest)
{
    return Impl_->CreateJobNode(std::move(createJobNodeRequest));
}

TFuture<void> TMasterConnector::FlushOperationNode(const TOperationId& operationId)
{
    return Impl_->FlushOperationNode(operationId);
}

TFuture<void> TMasterConnector::AttachToLivePreview(
    const TOperationId& operationId,
    const TTransactionId& transactionId,
    const TNodeId& tableId,
    const std::vector<TChunkTreeId>& childIds)
{
    return Impl_->AttachToLivePreview(operationId, transactionId, tableId, childIds);
}

TFuture<TOperationSnapshot> TMasterConnector::DownloadSnapshot(const TOperationId& operationId)
{
    return Impl_->DownloadSnapshot(operationId);
}

TFuture<void> TMasterConnector::RemoveSnapshot(const TOperationId& operationId)
{
    return Impl_->RemoveSnapshot(operationId);
}

void TMasterConnector::AttachJobContext(
    const TYPath& path,
    const TChunkId& chunkId,
    const TOperationId& operationId,
    const TJobId& jobId)
{
    return Impl_->AttachJobContext(path, chunkId, operationId, jobId);
}

void TMasterConnector::UpdateConfig(const TSchedulerConfigPtr& config)
{
    Impl_->UpdateConfig(config);
}

////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT


