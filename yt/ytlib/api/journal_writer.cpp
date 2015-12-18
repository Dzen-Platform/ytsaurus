#include "journal_writer.h"
#include "private.h"
#include "config.h"

#include <yt/ytlib/chunk_client/chunk_list_ypath_proxy.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_owner_ypath_proxy.h>
#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/chunk_ypath_proxy.h>
#include <yt/ytlib/chunk_client/data_node_service_proxy.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/private.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>
#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/journal_client/journal_ypath_proxy.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/master_ypath_proxy.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/transaction_client/transaction_listener.h>
#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/config.h>

#include <yt/ytlib/api/transaction.h>

#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/nonblocking_queue.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/address.h>
#include <yt/core/misc/variant.h>

#include <yt/core/rpc/helpers.h>

#include <yt/core/ytree/attribute_helpers.h>

#include <deque>
#include <queue>

namespace NYT {
namespace NApi {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NRpc;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTransactionClient;
using namespace NJournalClient;
using namespace NNodeTrackerClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

class TJournalWriter
    : public IJournalWriter
{
public:
    TJournalWriter(
        IClientPtr client,
        const TYPath& path,
        const TJournalWriterOptions& options)
        : Impl_(New<TImpl>(client, path, options))
    { }

    ~TJournalWriter()
    {
        Impl_->Cancel();
    }

    virtual TFuture<void> Open() override
    {
        return Impl_->Open();
    }

    virtual TFuture<void> Write(const std::vector<TSharedRef>& rows) override
    {
        return Impl_->Write(rows);
    }

    virtual TFuture<void> Close() override
    {
        return Impl_->Close();
    }

private:
    // NB: PImpl is used to enable external lifetime control (see TJournalWriter::dtor and TImpl::Cancel).
    class TImpl
        : public TTransactionListener
    {
    public:
        TImpl(
            IClientPtr client,
            const TYPath& path,
            const TJournalWriterOptions& options)
            : Client_(client)
            , Path_(path)
            , Options_(options)
            , Config_(options.Config ? options.Config : New<TJournalWriterConfig>())
        {
            if (Options_.TransactionId) {
                Transaction_ = Client_->AttachTransaction(Options_.TransactionId);
            }

            Logger.AddTag("Path: %v, TransactionId: %v",
                Path_,
                Options_.TransactionId);

            // Spawn the actor.
            BIND(&TImpl::ActorMain, MakeStrong(this))
                .AsyncVia(Invoker_)
                .Run();

            if (Transaction_) {
                ListenTransaction(Transaction_);
            }
        }

        TFuture<void> Open()
        {
            return OpenedPromise_;
        }

        TFuture<void> Write(const std::vector<TSharedRef>& rows)
        {
            TGuard<TSpinLock> guard(CurrentBatchSpinLock_);

            if (!Error_.IsOK()) {
                return MakeFuture(Error_);
            }

            TFuture<void> result = VoidFuture;
            for (const auto& row : rows) {
                YCHECK(!row.Empty());
                auto batch = EnsureCurrentBatch();
                // NB: We can form a handful of batches but since flushes are monotonic,
                // the last one will do.
                result = AppendToBatch(batch, row);
                if (IsBatchFull(batch)) {
                    FlushCurrentBatch();
                }
            }

            return result;
        }

        TFuture<void> Close()
        {
            EnqueueCommand(TCloseCommand());
            return ClosedPromise_;
        }

        void Cancel()
        {
            EnqueueCommand(TCancelCommand());
        }

    private:
        const IClientPtr Client_;
        const TYPath Path_;
        const TJournalWriterOptions Options_;
        const TJournalWriterConfigPtr Config_;

        const IInvokerPtr Invoker_ = NChunkClient::TDispatcher::Get()->GetWriterInvoker();

        NLogging::TLogger Logger = ApiLogger;

        struct TBatch
            : public TIntrinsicRefCounted
        {
            i64 FirstRowIndex = -1;
            i64 DataSize = 0;
            std::vector<TSharedRef> Rows;
            TPromise<void> FlushedPromise = NewPromise<void>();
            int FlushedReplicas = 0;
        };

        typedef TIntrusivePtr<TBatch> TBatchPtr;

        TSpinLock CurrentBatchSpinLock_;
        TError Error_;
        TBatchPtr CurrentBatch_;
        TDelayedExecutorCookie CurrentBatchFlushCookie_;

        TPromise<void> OpenedPromise_ = NewPromise<void>();

        bool Closing_ = false;
        TPromise<void> ClosedPromise_ = NewPromise<void>();

        ITransactionPtr Transaction_;
        ITransactionPtr UploadTransaction_;
        
        int ReplicationFactor_ = -1;
        int ReadQuorum_ = -1;
        int WriteQuorum_ = -1;
        Stroka Account_;

        TObjectId ObjectId_;
        TChunkListId ChunkListId_;
        IChannelPtr UploadMasterChannel_;

        struct TNode
            : public TRefCounted
        {
            const TNodeDescriptor Descriptor;

            TDataNodeServiceProxy LightProxy;
            TDataNodeServiceProxy HeavyProxy;
            TPeriodicExecutorPtr PingExecutor;

            i64 FirstPendingBlockIndex = 0;
            i64 FirstPendingRowIndex = 0;

            std::queue<TBatchPtr> PendingBatches;
            std::vector<TBatchPtr> InFlightBatches;

            TNode(
                const TNodeDescriptor& descriptor,
                IChannelPtr lightChannel,
                IChannelPtr heavyChannel,
                TDuration rpcTimeout)
                : Descriptor(descriptor)
                , LightProxy(lightChannel)
                , HeavyProxy(heavyChannel)
            {
                LightProxy.SetDefaultTimeout(rpcTimeout);
                HeavyProxy.SetDefaultTimeout(rpcTimeout);
            }
        };

        typedef TIntrusivePtr<TNode> TNodePtr;
        typedef TWeakPtr<TNode> TNodeWeakPtr;

        TNodeDirectoryPtr NodeDirectory_ = New<TNodeDirectory>();

        struct TChunkSession
            : public TRefCounted
        {
            TChunkId ChunkId;
            std::vector<TNodePtr> Nodes;
            i64 RowCount = 0;
            i64 DataSize = 0;
            i64 FlushedRowCount = 0;
            i64 FlushedDataSize = 0;
        };

        typedef TIntrusivePtr<TChunkSession> TChunkSessionPtr;
        typedef TWeakPtr<TChunkSession> TChunkSessionWeakPtr;

        TChunkSessionPtr CurrentSession_;

        i64 CurrentRowIndex_ = 0;
        std::deque<TBatchPtr> PendingBatches_;

        typedef TBatchPtr TBatchCommand;

        struct TCloseCommand { };
        
        struct TCancelCommand { };

        struct TSwitchChunkCommand
        {
            TChunkSessionPtr Session;
        };

        typedef TVariant<
            TBatchCommand,
            TCloseCommand,
            TCancelCommand,
            TSwitchChunkCommand
        > TCommand;

        TNonblockingQueue<TCommand> CommandQueue_;

        yhash_map<Stroka, TInstant> BannedNodeToDeadline_;


        void EnqueueCommand(TCommand command)
        {
            CommandQueue_.Enqueue(std::move(command));
        }
        
        TCommand DequeueCommand()
        {
            return WaitFor(CommandQueue_.Dequeue())
                .ValueOrThrow();
        }


        void BanNode(const Stroka& address)
        {
            if (BannedNodeToDeadline_.find(address) == BannedNodeToDeadline_.end()) {
                BannedNodeToDeadline_.insert(std::make_pair(address, TInstant::Now() + Config_->NodeBanTimeout));
                LOG_INFO("Node banned (Address: %v)", address);
            }
        }

        std::vector<Stroka> GetBannedNodes()
        {
            std::vector<Stroka> result;
            auto now = TInstant::Now();
            auto it = BannedNodeToDeadline_.begin();
            while (it != BannedNodeToDeadline_.end()) {
                auto jt = it++;
                if (jt->second < now) {
                    LOG_INFO("Node unbanned (Address: %v)", jt->first);
                    BannedNodeToDeadline_.erase(jt);
                } else {
                    result.push_back(jt->first);
                }
            }
            return result;
        }


        void OpenJournal()
        {
            auto cellTag = InvalidCellTag;

            {
                LOG_INFO("Requesting basic journal attributes");

                auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
                TObjectServiceProxy proxy(channel);

                auto req = TJournalYPathProxy::GetBasicAttributes(Path_);
                req->set_permissions(static_cast<ui32>(EPermission::Write));
                SetTransactionId(req, Transaction_);

                auto rspOrError = WaitFor(proxy.Execute(req));
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    rspOrError,
                    "Error requesting basic attributes of journal %v",
                    Path_);

                const auto& rsp = rspOrError.Value();
                ObjectId_ = FromProto<TObjectId>(rsp->object_id());
                cellTag = rsp->cell_tag();

                LOG_INFO("Basic journal attributes received (ObjectId: %v, CellTag: %v)",
                    ObjectId_,
                    cellTag);
            }

            {
                auto type = TypeFromId(ObjectId_);
                if (type != EObjectType::Journal) {
                    THROW_ERROR_EXCEPTION("Invalid type of %v: expected %Qlv, actual %Qlv",
                        Path_,
                        EObjectType::Journal,
                        type);
                }
            }

            UploadMasterChannel_ = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag);
            auto objectIdPath = FromObjectId(ObjectId_);

            {
                LOG_INFO("Requesting extended journal attributes");

                auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
                TObjectServiceProxy proxy(channel);

                auto req = TCypressYPathProxy::Get(objectIdPath);
                SetTransactionId(req, UploadTransaction_);
                TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
                attributeFilter.Keys.push_back("type");
                attributeFilter.Keys.push_back("replication_factor");
                attributeFilter.Keys.push_back("read_quorum");
                attributeFilter.Keys.push_back("write_quorum");
                attributeFilter.Keys.push_back("account");
                ToProto(req->mutable_attribute_filter(), attributeFilter);

                auto rspOrError = WaitFor(proxy.Execute(req));
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    rspOrError,
                    "Error requesting extended attributes of journal %v",
                    Path_);

                auto rsp = rspOrError.Value();
                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();
                ReplicationFactor_ = attributes.Get<int>("replication_factor");
                ReadQuorum_ = attributes.Get<int>("read_quorum");
                WriteQuorum_ = attributes.Get<int>("write_quorum");
                Account_ = attributes.Get<Stroka>("account");

                LOG_INFO("Extended journal attributes received (ReplicationFactor: %v, WriteQuorum: %v, Account: %v)",
                    ReplicationFactor_,
                    WriteQuorum_,
                    Account_);
            }

            {
                LOG_INFO("Starting journal upload");

                auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
                TObjectServiceProxy proxy(channel);

                auto batchReq = proxy.ExecuteBatch();

                {
                    auto* prerequisitesExt = batchReq->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
                    for (const auto& id : Options_.PrerequisiteTransactionIds) {
                        auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
                        ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
                    }
                }

                {
                    auto req = TJournalYPathProxy::BeginUpload(objectIdPath);
                    req->set_update_mode(static_cast<int>(EUpdateMode::Append));
                    req->set_lock_mode(static_cast<int>(ELockMode::Exclusive));
                    req->set_upload_transaction_title(Format("Upload to %v", Path_));
                    req->set_upload_transaction_timeout(ToProto(Client_->GetConnection()->GetConfig()->TransactionManager->DefaultTransactionTimeout));
                    GenerateMutationId(req);
                    SetTransactionId(req, Transaction_);
                    batchReq->AddRequest(req, "begin_upload");
                }

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    GetCumulativeError(batchRspOrError),
                    "Error starting upload to journal %v",
                    Path_);
                const auto& batchRsp = batchRspOrError.Value();

                {
                    auto rsp = batchRsp->GetResponse<TJournalYPathProxy::TRspBeginUpload>("begin_upload").Value();
                    auto uploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());

                    TTransactionAttachOptions options;
                    options.PingAncestors = Options_.PingAncestors;
                    options.AutoAbort = true;

                    UploadTransaction_ = Client_->AttachTransaction(uploadTransactionId, options);
                    ListenTransaction(UploadTransaction_);

                    LOG_INFO("Journal upload started (UploadTransactionId: %v)",
                        uploadTransactionId);
                }
            }

            {
                LOG_INFO("Requesting journal upload parameters");

                auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower, cellTag);
                TObjectServiceProxy proxy(channel);

                auto req = TJournalYPathProxy::GetUploadParams(objectIdPath);
                SetTransactionId(req, UploadTransaction_);

                auto rspOrError = WaitFor(proxy.Execute(req));
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    rspOrError,
                    "Error requesting upload parameters for journal %v",
                    Path_);

                const auto& rsp = rspOrError.Value();
                ChunkListId_ = FromProto<TChunkListId>(rsp->chunk_list_id());

                LOG_INFO("Journal upload parameters received (ChunkListId: %v)",
                    ChunkListId_);
            }

            LOG_INFO("Journal opened");
            OpenedPromise_.Set(TError());
        }

        void CloseJournal()
        {
            LOG_INFO("Closing journal");

            auto objectIdPath = FromObjectId(ObjectId_);

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
            TObjectServiceProxy proxy(channel);

            auto batchReq = proxy.ExecuteBatch();

            {
                auto* prerequisitesExt = batchReq->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
                for (const auto& id : Options_.PrerequisiteTransactionIds) {
                    auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
                    ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
                }
            }

            {
                auto req = TJournalYPathProxy::EndUpload(objectIdPath);
                SetTransactionId(req, UploadTransaction_);
                GenerateMutationId(req);
                batchReq->AddRequest(req, "end_upload");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(
                GetCumulativeError(batchRspOrError),
                "Error finishing upload to journal %v",
                Path_);

            UploadTransaction_->Detach();


            LOG_INFO("Journal closed");
            ClosedPromise_.TrySet(TError());
        }

        bool TryOpenChunk()
        {
            CurrentSession_ = New<TChunkSession>();

            LOG_INFO("Creating chunk");

            {
                TObjectServiceProxy proxy(UploadMasterChannel_);

                auto req = TMasterYPathProxy::CreateObject();
                req->set_type(static_cast<int>(EObjectType::JournalChunk));
                req->set_account(Account_);
                ToProto(req->mutable_transaction_id(), UploadTransaction_->GetId());

                auto* reqExt = req->mutable_extensions()->MutableExtension(TChunkCreationExt::chunk_creation_ext);
                reqExt->set_replication_factor(ReplicationFactor_);
                reqExt->set_read_quorum(ReadQuorum_);
                reqExt->set_write_quorum(WriteQuorum_);
                reqExt->set_movable(true);
                reqExt->set_vital(true);
                reqExt->set_erasure_codec(static_cast<int>(NErasure::ECodec::None));

                auto rspOrError = WaitFor(proxy.Execute(req));
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error creating chunk");
                const auto& rsp = rspOrError.Value();

                CurrentSession_->ChunkId = FromProto<TChunkId>(rsp->object_id());
            }

            LOG_INFO("Chunk created (ChunkId: %v)",
                CurrentSession_->ChunkId);

            std::vector<TChunkReplica> replicas;
            std::vector<TNodeDescriptor> targets;
            {
                TChunkServiceProxy proxy(UploadMasterChannel_);

                auto req = proxy.AllocateWriteTargets();
                ToProto(req->mutable_chunk_id(), CurrentSession_->ChunkId);
                ToProto(req->mutable_forbidden_addresses(), GetBannedNodes());
                if (Config_->PreferLocalHost) {
                    req->set_preferred_host_name(TAddressResolver::Get()->GetLocalHostName());
                }
                req->set_desired_target_count(ReplicationFactor_);
                req->set_min_target_count(WriteQuorum_);

                auto rspOrError = WaitFor(req->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error allocating write targets");
                const auto& rsp = rspOrError.Value();

                NodeDirectory_->MergeFrom(rsp->node_directory());

                replicas = NYT::FromProto<TChunkReplica>(rsp->replicas());
                for (auto replica : replicas) {
                    const auto& descriptor = NodeDirectory_->GetDescriptor(replica);
                    targets.push_back(descriptor);
                }
            }

            LOG_INFO("Write targets allocated (Targets: [%v])",
                JoinToString(targets));

            const auto& networkName = Client_->GetConnection()->GetConfig()->NetworkName;
            for (const auto& target : targets) {
                auto address = target.GetAddressOrThrow(networkName);
                auto lightChannel = LightNodeChannelFactory->CreateChannel(address);
                auto heavyChannel = HeavyNodeChannelFactory->CreateChannel(address);
                auto node = New<TNode>(
                    target,
                    lightChannel,
                    heavyChannel,
                    Config_->NodeRpcTimeout);
                CurrentSession_->Nodes.push_back(node);
            }

            LOG_INFO("Starting chunk sessions");
            try {
                std::vector<TFuture<void>> asyncResults;
                for (auto node : CurrentSession_->Nodes) {
                    auto req = node->LightProxy.StartChunk();
                    ToProto(req->mutable_chunk_id(), CurrentSession_->ChunkId);
                    ToProto(req->mutable_workload_descriptor(), Config_->WorkloadDescriptor);
                    req->set_optimize_for_latency(true);
                    auto asyncRsp = req->Invoke().Apply(
                        BIND(&TImpl::OnChunkStarted, MakeStrong(this), node)
                            .AsyncVia(Invoker_));
                    asyncResults.push_back(asyncRsp);
                }
                auto result = WaitFor(Combine(asyncResults));
                THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error starting chunk sessions");
            } catch (const std::exception& ex) {
                LOG_WARNING(TError(ex));
                CurrentSession_.Reset();
                return false;
            }
            LOG_INFO("Chunk sessions started");

            for (auto node : CurrentSession_->Nodes) {
                node->PingExecutor = New<TPeriodicExecutor>(
                    Invoker_,
                    BIND(&TImpl::SendPing, MakeWeak(this), MakeWeak(CurrentSession_), MakeWeak(node)),
                    Config_->NodePingPeriod);
                node->PingExecutor->Start();
            }

            LOG_INFO("Attaching chunk");
            {
                TObjectServiceProxy proxy(UploadMasterChannel_);
                auto batchReq = proxy.ExecuteBatch();

                {
                    YCHECK(!replicas.empty());
                    auto req = TChunkYPathProxy::Confirm(FromObjectId(CurrentSession_->ChunkId));
                    req->mutable_chunk_info();
                    ToProto(req->mutable_replicas(), replicas);
                    auto* meta = req->mutable_chunk_meta();
                    meta->set_type(static_cast<int>(EChunkType::Journal));
                    meta->set_version(0);
                    TMiscExt miscExt;
                    SetProtoExtension(meta->mutable_extensions(), miscExt);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req, "confirm");
                }
                {
                    auto req = TChunkListYPathProxy::Attach(FromObjectId(ChunkListId_));
                    ToProto(req->add_children_ids(), CurrentSession_->ChunkId);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req, "attach");
                }

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error attaching chunk");
            }
            LOG_INFO("Chunk attached");

            for (auto batch : PendingBatches_) {
                EnqueueBatchToSession(batch);
            }

            return true;
        }

        void OpenChunk()
        {
            for (int attempt = 0; attempt < Config_->MaxChunkOpenAttempts; ++attempt) {
                if (TryOpenChunk())
                    return;
            }
            THROW_ERROR_EXCEPTION("All %v attempts to open a chunk were unsuccessful",
                Config_->MaxChunkOpenAttempts);
        }

        void WriteChunk()
        {
            while (true) {
                ValidateAborted();
                auto command = DequeueCommand();
                if (command.Is<TCloseCommand>()) {
                    HandleClose();
                    break;
                } else if (command.Is<TCancelCommand>()) {
                    throw TFiberCanceledException();
                } else if (auto* typedCommand = command.TryAs<TBatchCommand>()) {
                    HandleBatch(*typedCommand);
                    if (IsSessionOverfull()) {
                        SwitchChunk();
                        break;
                    }
                } else if (auto* typedCommand = command.TryAs<TSwitchChunkCommand>()) {
                    if (typedCommand->Session == CurrentSession_) {
                        SwitchChunk();
                        break;
                    }
                }
            }
        }

        void HandleClose()
        {
            LOG_INFO("Closing journal writer");
            Closing_ = true;
        }

        void HandleBatch(TBatchPtr batch)
        {
            i64 rowCount = batch->Rows.size();

            LOG_DEBUG("Batch ready (Rows: %v-%v)",
                CurrentRowIndex_,
                CurrentRowIndex_ + rowCount - 1);

            batch->FirstRowIndex = CurrentRowIndex_;
            CurrentRowIndex_ += rowCount;

            PendingBatches_.push_back(batch);

            EnqueueBatchToSession(batch);
        }

        bool IsSessionOverfull()
        {
            return
                CurrentSession_->RowCount > Config_->MaxChunkRowCount ||
                CurrentSession_->DataSize > Config_->MaxChunkDataSize;
        }

        void EnqueueBatchToSession(TBatchPtr batch)
        {
            // Reset flushed replica count: this batch might have already been 
            // flushed (partially) by the previous (failed session).
            if (batch->FlushedReplicas > 0) {
                LOG_DEBUG("Resetting flushed replica counter (Rows: %v-%v, FlushCounter: %v)",
                    batch->FirstRowIndex,
                    batch->FirstRowIndex + batch->Rows.size() - 1,
                    batch->FlushedReplicas);
                batch->FlushedReplicas = 0;
            }

            CurrentSession_->RowCount += batch->Rows.size();
            CurrentSession_->DataSize += batch->DataSize;

            for (auto node : CurrentSession_->Nodes) {
                node->PendingBatches.push(batch);
                MaybeFlushBlocks(node);
            }
        }

        void SwitchChunk()
        {
            LOG_INFO("Switching chunk");
        }

        void CloseChunk()
        {
            // Release the current session to prevent writing more rows
            // or detecting failed pings.
            auto session = CurrentSession_;
            CurrentSession_.Reset();

            LOG_INFO("Finishing chunk sessions");
            for (auto node : session->Nodes) {
                auto req = node->LightProxy.FinishChunk();
                ToProto(req->mutable_chunk_id(), session->ChunkId);
                req->Invoke().Subscribe(
                    BIND(&TImpl::OnChunkFinished, MakeStrong(this), node)
                        .Via(Invoker_));
                if (node->PingExecutor) {
                    node->PingExecutor->Stop();
                    node->PingExecutor.Reset();
                }
            }

            {
                LOG_INFO("Sealing chunk (ChunkId: %v, RowCount: %v)",
                    session->ChunkId,
                    session->FlushedRowCount);

                TObjectServiceProxy proxy(UploadMasterChannel_);

                auto req = TChunkYPathProxy::Seal(FromObjectId(session->ChunkId));
                auto* info = req->mutable_info();
                info->set_sealed(true);
                info->set_row_count(session->FlushedRowCount);
                info->set_uncompressed_data_size(session->FlushedDataSize);
                info->set_compressed_data_size(session->FlushedDataSize);

                auto rspOrError = WaitFor(proxy.Execute(req));
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error sealing chunk %v",
                    session->ChunkId);

                LOG_INFO("Chunk sealed");
            }
        }


        void ActorMain()
        {
            try {
                GuardedActorMain();
            } catch (const std::exception& ex) {
                try {
                    PumpFailed(ex);
                } catch (const std::exception& ex) {
                    LOG_ERROR(ex, "Error pumping journal writer command queue");
                }
            }
        }

        void GuardedActorMain()
        {
            OpenJournal();
            do {
                OpenChunk();
                WriteChunk();
                CloseChunk();
            } while (!Closing_ || !PendingBatches_.empty());
            CloseJournal();
        }

        void PumpFailed(const TError& error)
        {
            LOG_WARNING(error, "Journal writer failed");

            {
                TGuard<TSpinLock> guard(CurrentBatchSpinLock_);
                Error_ = error;
                if (CurrentBatch_) {
                    auto promise = CurrentBatch_->FlushedPromise;
                    CurrentBatch_.Reset();
                    guard.Release();
                    promise.Set(error);
                }
            }

            OpenedPromise_.TrySet(error);
            ClosedPromise_.TrySet(error);

            for (auto batch : PendingBatches_) {
                batch->FlushedPromise.Set(error);
            }
            PendingBatches_.clear();

            while (true) {
                auto command = DequeueCommand();
                if (auto* typedCommand = command.TryAs<TBatchCommand>()) {
                    (*typedCommand)->FlushedPromise.Set(error);
                } else if (command.Is<TCancelCommand>()) {
                    throw TFiberCanceledException();
                } else {
                    // Ignore.
                }
            }
        }


        static TFuture<void> AppendToBatch(const TBatchPtr& batch, const TSharedRef& row)
        {
            YASSERT(row);
            batch->Rows.push_back(row);
            batch->DataSize += row.Size();
            return batch->FlushedPromise;
        }

        bool IsBatchFull(const TBatchPtr& batch)
        {
            return
                batch->DataSize > Config_->MaxBatchDataSize ||
                batch->Rows.size() > Config_->MaxBatchRowCount;
        }


        TBatchPtr EnsureCurrentBatch()
        {
            VERIFY_SPINLOCK_AFFINITY(CurrentBatchSpinLock_);

            if (!CurrentBatch_) {
                CurrentBatch_ = New<TBatch>();
                CurrentBatchFlushCookie_ = TDelayedExecutor::Submit(
                    BIND(&TImpl::OnBatchTimeout, MakeWeak(this), CurrentBatch_)
                        .Via(Invoker_),
                    Config_->MaxBatchDelay);
            }

            return CurrentBatch_;
        }

        void OnBatchTimeout(TBatchPtr batch)
        {
            TGuard<TSpinLock> guard(CurrentBatchSpinLock_);
            if (CurrentBatch_ == batch) {
                FlushCurrentBatch();
            }
        }

        void FlushCurrentBatch()
        {
            VERIFY_SPINLOCK_AFFINITY(CurrentBatchSpinLock_);

            if (CurrentBatchFlushCookie_) {
                TDelayedExecutor::CancelAndClear(CurrentBatchFlushCookie_);
            }

            EnqueueCommand(TBatchCommand(CurrentBatch_));
            CurrentBatch_.Reset();
        }
  

        void SendPing(TChunkSessionWeakPtr session_, TNodeWeakPtr node_)
        {
            auto session = session_.Lock();
            if (!session)
                return;

            auto node = node_.Lock();
            if (!node)
                return;

            LOG_DEBUG("Sending ping (Address: %v, ChunkId: %v)",
                node->Descriptor.GetDefaultAddress(),
                session->ChunkId);

            auto req = node->LightProxy.PingSession();
            ToProto(req->mutable_chunk_id(), session->ChunkId);
            req->Invoke().Subscribe(
                BIND(&TImpl::OnPingSent, MakeWeak(this), session, node)
                    .Via(Invoker_));
        }

        void OnPingSent(TChunkSessionPtr session, TNodePtr node, const TDataNodeServiceProxy::TErrorOrRspPingSessionPtr& rspOrError)
        {
            if (session != CurrentSession_)
                return;

            if (!rspOrError.IsOK()) {
                OnReplicaFailed(rspOrError, node, session);
                return;
            }

            LOG_DEBUG("Ping succeeded (Address: %v, ChunkId: %v)",
                node->Descriptor.GetDefaultAddress(),
                session->ChunkId);
        }


        void OnChunkStarted(TNodePtr node, const TDataNodeServiceProxy::TErrorOrRspStartChunkPtr& rspOrError)
        {
            if (rspOrError.IsOK()) {
                LOG_DEBUG("Chunk session started (Address: %v)",
                    node->Descriptor.GetDefaultAddress());
            } else {
                BanNode(node->Descriptor.GetDefaultAddress());
                THROW_ERROR_EXCEPTION("Error starting session at %v",
                    node->Descriptor.GetDefaultAddress())
                    << rspOrError;
            }
        }

        void OnChunkFinished(TNodePtr node, const TDataNodeServiceProxy::TErrorOrRspFinishChunkPtr& rspOrError)
        {
            if (rspOrError.IsOK()) {
                LOG_DEBUG("Chunk session finished (Address: %v)",
                    node->Descriptor.GetDefaultAddress());
            } else {
                BanNode(node->Descriptor.GetDefaultAddress());
                LOG_WARNING(rspOrError, "Chunk session has failed to finish (Address: %v)",
                    node->Descriptor.GetDefaultAddress());
            }
        }


        void MaybeFlushBlocks(TNodePtr node)
        {
            if (!node->InFlightBatches.empty() || node->PendingBatches.empty())
                return;

            i64 flushRowCount = 0;
            i64 flushDataSize = 0;

            auto req = node->HeavyProxy.PutBlocks();
            ToProto(req->mutable_chunk_id(), CurrentSession_->ChunkId);
            req->set_first_block_index(node->FirstPendingBlockIndex);
            req->set_flush_blocks(true);

            YASSERT(node->InFlightBatches.empty());
            while (flushRowCount <= Config_->MaxFlushRowCount &&
                   flushDataSize <= Config_->MaxFlushDataSize &&
                   !node->PendingBatches.empty())
            {
                auto batch = node->PendingBatches.front();
                node->PendingBatches.pop();

                req->Attachments().insert(req->Attachments().end(), batch->Rows.begin(), batch->Rows.end());

                flushRowCount += batch->Rows.size();
                flushDataSize += batch->DataSize;

                node->InFlightBatches.push_back(batch);
            }

            LOG_DEBUG("Flushing journal replica (Address: %v, BlockIds: %v:%v-%v, Rows: %v-%v)",
                node->Descriptor.GetDefaultAddress(),
                CurrentSession_->ChunkId,
                node->FirstPendingBlockIndex,
                node->FirstPendingBlockIndex + flushRowCount - 1,
                node->FirstPendingRowIndex,
                node->FirstPendingRowIndex + flushRowCount - 1);

            req->Invoke().Subscribe(
                BIND(&TImpl::OnBlocksFlushed, MakeWeak(this), CurrentSession_, node, flushRowCount)
                    .Via(Invoker_));
        }

        void OnBlocksFlushed(
            TChunkSessionPtr session,
            TNodePtr node,
            i64 flushRowCount,
            const TDataNodeServiceProxy::TErrorOrRspPutBlocksPtr& rspOrError)
        {
            if (session != CurrentSession_)
                return;

            if (!rspOrError.IsOK()) {
                OnReplicaFailed(rspOrError, node, session);
                return;
            }

            LOG_DEBUG("Journal replica flushed (Address: %v, BlockIds: %v:%v-%v, Rows: %v-%v)",
                node->Descriptor.GetDefaultAddress(),
                session->ChunkId,
                node->FirstPendingBlockIndex,
                node->FirstPendingBlockIndex + flushRowCount - 1,
                node->FirstPendingRowIndex,
                node->FirstPendingRowIndex + flushRowCount - 1);

            for (const auto& batch : node->InFlightBatches) {
                ++batch->FlushedReplicas;
            }

            node->FirstPendingBlockIndex += flushRowCount;
            node->FirstPendingRowIndex += flushRowCount;
            node->InFlightBatches.clear();

            std::vector<TPromise<void>> fulfilledPromises;
            while (!PendingBatches_.empty()) {
                auto front = PendingBatches_.front();
                if (front->FlushedReplicas <  WriteQuorum_)
                    break;

                fulfilledPromises.push_back(front->FlushedPromise);
                session->FlushedRowCount += front->Rows.size();
                session->FlushedDataSize += front->DataSize;
                PendingBatches_.pop_front();

                LOG_DEBUG("Rows are flushed by quorum (Rows: %v-%v)",
                    front->FirstRowIndex,
                    front->FirstRowIndex + front->Rows.size() - 1);
            }

            MaybeFlushBlocks(node);

            for (auto& promise : fulfilledPromises) {
                promise.Set();
            }
        }


        void OnReplicaFailed(const TError& error, TNodePtr node, TChunkSessionPtr session)
        {
            const auto& address = node->Descriptor.GetDefaultAddress();
            LOG_WARNING(error, "Journal replica failed (Address: %v, ChunkId: %v)",
                address,
                session->ChunkId);

            BanNode(address);

            TSwitchChunkCommand command;
            command.Session = session;
            EnqueueCommand(command);
        }
    };


    const TIntrusivePtr<TImpl> Impl_;

};

IJournalWriterPtr CreateJournalWriter(
    IClientPtr client,
    const TYPath& path,
    const TJournalWriterOptions& options)
{
    return New<TJournalWriter>(client, path, options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
