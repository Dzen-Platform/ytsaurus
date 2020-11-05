#include "journal_writer.h"
#include "private.h"
#include "config.h"
#include "transaction.h"
#include "connection.h"

#include <yt/client/api/journal_writer.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_owner_ypath_proxy.h>
#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/data_node_service_proxy.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/session_id.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>
#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/journal_client/journal_ypath_proxy.h>
#include <yt/ytlib/journal_client/helpers.h>
#include <yt/ytlib/journal_client/proto/format.pb.h>

#include <yt/client/node_tracker_client/node_directory.h>
#include <yt/ytlib/node_tracker_client/channel.h>

#include <yt/client/object_client/helpers.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/transaction_client/transaction_listener.h>
#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/config.h>

#include <yt/client/api/transaction.h>

#include <yt/client/chunk_client/chunk_replica.h>

#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/nonblocking_queue.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/thread_affinity.h>
#include <yt/core/concurrency/action_queue.h>

#include <yt/core/rpc/helpers.h>
#include <yt/core/rpc/retrying_channel.h>
#include <yt/core/rpc/dispatcher.h>

#include <yt/core/ytree/helpers.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/library/erasure/codec.h>

#include <deque>
#include <queue>

namespace NYT::NApi::NNative {

using namespace NChunkClient::NProto;
using namespace NChunkClient;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NJournalClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient::NProto;
using namespace NObjectClient;
using namespace NProfiling;
using namespace NRpc;
using namespace NTransactionClient;
using namespace NTransactionClient;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;

using NYT::TRange;

// Suppress ambiguity with NProto::TSessionId.
using NChunkClient::TSessionId;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EJournalWriterChunkSessionState,
    (Allocating)
    (Allocated)
    (Current)
    (Discarded)
);

class TJournalWriter
    : public IJournalWriter
{
public:
    TJournalWriter(
        IClientPtr client,
        TYPath path,
        TJournalWriterOptions options)
        : Impl_(New<TImpl>(
            std::move(client),
            std::move(path),
            std::move(options)))
    { }

    ~TJournalWriter()
    {
        Impl_->Cancel();
    }

    virtual TFuture<void> Open() override
    {
        return Impl_->Open();
    }

    virtual TFuture<void> Write(TRange<TSharedRef> rows) override
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
            , Profiler(options.Profiler)
            , Logger(NLogging::TLogger(ApiLogger)
                .AddTag("Path: %v, TransactionId: %v",
                    Path_,
                    Options_.TransactionId))
        {
            if (Options_.TransactionId) {
                TTransactionAttachOptions attachOptions{
                    .Ping = true
                };
                Transaction_ = Client_->AttachTransaction(Options_.TransactionId, attachOptions);
            }

            for (auto transactionId : Options_.PrerequisiteTransactionIds) {
                TTransactionAttachOptions attachOptions{
                    .Ping = false
                };
                auto transaction = Client_->AttachTransaction(transactionId, attachOptions);
                StartProbeTransaction(transaction, Config_->PrerequisiteTransactionProbePeriod);
            }

            // Spawn the actor.
            BIND(&TImpl::ActorMain, MakeStrong(this))
                .AsyncVia(Invoker_)
                .Run();

            if (Transaction_) {
                StartListenTransaction(Transaction_);
            }
        }

        TFuture<void> Open()
        {
            return OpenedPromise_;
        }

        TFuture<void> Write(TRange<TSharedRef> rows)
        {
            TGuard<TAdaptiveLock> guard(CurrentBatchSpinLock_);

            if (!Error_.IsOK()) {
                return MakeFuture(Error_);
            }

            auto result = VoidFuture;
            for (const auto& row : rows) {
                YT_VERIFY(row);
                auto batch = EnsureCurrentBatch();
                // NB: We can form a handful of batches but since flushes are monotonic,
                // the last one will do.
                result = AppendToBatch(batch, row);
            }

            return result;
        }

        TFuture<void> Close()
        {
            if (Config_->DontClose) {
                return VoidFuture;
            }

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
        const TProfiler Profiler;
        const NLogging::TLogger Logger;

        const IInvokerPtr Invoker_ = CreateSerializedInvoker(NRpc::TDispatcher::Get()->GetHeavyInvoker());

        struct TBatch
            : public TRefCounted
        {
            i64 FirstRowIndex = -1;
            i64 RowCount = 0;
            i64 DataSize = 0;
            
            std::vector<TSharedRef> Rows;
            std::vector<std::vector<TSharedRef>> ErasureRows;
            
            const TPromise<void> FlushedPromise = NewPromise<void>();
            int FlushedReplicas = 0;
            
            TCpuInstant StartTime;
        };

        using TBatchPtr = TIntrusivePtr<TBatch>;

        TAdaptiveLock CurrentBatchSpinLock_;
        TError Error_;
        TBatchPtr CurrentBatch_;
        TDelayedExecutorCookie CurrentBatchFlushCookie_;

        TPromise<void> OpenedPromise_ = NewPromise<void>();

        bool Closing_ = false;
        TPromise<void> ClosedPromise_ = NewPromise<void>();

        NApi::ITransactionPtr Transaction_;
        NApi::ITransactionPtr UploadTransaction_;

        NErasure::ECodec ErasureCodec_ = NErasure::ECodec::None;
        int ReplicationFactor_ = -1;
        int ReadQuorum_ = -1;
        int WriteQuorum_ = -1;
        TString Account_;
        TString PrimaryMedium_;

        TObjectId ObjectId_;
        TCellTag NativeCellTag_ = InvalidCellTag;
        TCellTag ExternalCellTag_ = InvalidCellTag;

        TChunkListId ChunkListId_;
        IChannelPtr UploadMasterChannel_;

        struct TNode
            : public TRefCounted
        {
            const int Index;
            const TNodeDescriptor Descriptor;

            TDataNodeServiceProxy LightProxy;
            TDataNodeServiceProxy HeavyProxy;
            TPeriodicExecutorPtr PingExecutor;

            bool Started = false;

            i64 FirstPendingBlockIndex = 0;
            i64 FirstPendingRowIndex = -1;

            std::queue<TBatchPtr> PendingBatches;
            std::vector<TBatchPtr> InFlightBatches;

            TCpuDuration LagTime = 0;

            TNode(
                int index,
                TNodeDescriptor descriptor,
                IChannelPtr lightChannel,
                IChannelPtr heavyChannel,
                TDuration rpcTimeout,
                TTagIdList tagIds)
                : Index(index)
                , Descriptor(std::move(descriptor))
                , LightProxy(std::move(lightChannel))
                , HeavyProxy(std::move(heavyChannel))
            {
                LightProxy.SetDefaultTimeout(rpcTimeout);
                HeavyProxy.SetDefaultTimeout(rpcTimeout);
            }
        };

        using TNodePtr = TIntrusivePtr<TNode>;
        using TNodeWeakPtr = TWeakPtr<TNode>;

        const TNodeDirectoryPtr NodeDirectory_ = New<TNodeDirectory>();

        using EChunkSessionState = EJournalWriterChunkSessionState;

        struct TChunkSession
            : public TRefCounted
        {
            explicit TChunkSession(int index)
                : Index(index)
            { }
            
            const int Index;
            
            TSessionId Id;
            std::vector<TNodePtr> Nodes;
            
            i64 FlushedRowCount = 0;
            i64 FlushedDataSize = 0;
            
            EChunkSessionState State = EChunkSessionState::Allocating;
            bool SwitchScheduled = false;

            i64 FirstRowIndex = -1;
            
            TSharedRef HeaderRow;

            TShardedAggregateGauge MaxReplicaLag{"/max_replica_lag"};
            TShardedAggregateGauge WriteQuorumLag{"/write_quorum_lag"};
        };

        using TChunkSessionPtr = TIntrusivePtr<TChunkSession>;
        using TChunkSessionWeakPtr = TWeakPtr<TChunkSession>;

        TChunkSessionPtr CurrentChunkSession_;

        int NextChunkSessionIndex_ = 0;

        TPromise<TChunkSessionPtr> AllocatedChunkSessionPromise_;
        int AllocatedChunkSessionIndex_ = -1;

        i64 CurrentRowIndex_ = 0;
        std::deque<TBatchPtr> PendingBatches_;

        struct TBatchCommand
        {
            TBatchPtr Batch;
        };

        struct TCloseCommand
        { };

        struct TCancelCommand
        { };

        struct TSwitchChunkCommand
        {
            TChunkSessionPtr Session;
        };

        using TCommand = std::variant<
            TBatchCommand,
            TCloseCommand,
            TCancelCommand,
            TSwitchChunkCommand
        >;

        TNonblockingQueue<TCommand> CommandQueue_;

        THashMap<TString, TInstant> BannedNodeToDeadline_;


        void EnqueueCommand(TCommand command)
        {
            CommandQueue_.Enqueue(std::move(command));
        }

        TCommand DequeueCommand()
        {
            return WaitFor(CommandQueue_.Dequeue())
                .ValueOrThrow();
        }


        void BanNode(const TString& address)
        {
            if (BannedNodeToDeadline_.emplace(address, TInstant::Now() + Config_->NodeBanTimeout).second) {
                YT_LOG_DEBUG("Node banned (Address: %v)", address);
            }
        }

        std::vector<TString> GetBannedNodes()
        {
            std::vector<TString> result;
            auto now = TInstant::Now();
            auto it = BannedNodeToDeadline_.begin();
            while (it != BannedNodeToDeadline_.end()) {
                auto jt = it++;
                if (jt->second < now) {
                    YT_LOG_DEBUG("Node unbanned (Address: %v)", jt->first);
                    BannedNodeToDeadline_.erase(jt);
                } else {
                    result.push_back(jt->first);
                }
            }
            return result;
        }

        void OpenJournal()
        {
            TUserObject userObject(Path_);

            {
                TTimingGuard timingGuard(&Profiler, "/get_basic_attributes_time");

                GetUserObjectBasicAttributes(
                    Client_,
                    {&userObject},
                    Transaction_ ? Transaction_->GetId() : NullTransactionId,
                    Logger,
                    EPermission::Write);
            }

            ObjectId_ = userObject.ObjectId;
            NativeCellTag_ = CellTagFromId(ObjectId_);
            ExternalCellTag_ = userObject.ExternalCellTag;

            auto objectIdPath = FromObjectId(ObjectId_);

            if (userObject.Type != EObjectType::Journal) {
                THROW_ERROR_EXCEPTION("Invalid type of %v: expected %Qlv, actual %Qlv",
                    Path_,
                    EObjectType::Journal,
                    userObject.Type);
            }

            UploadMasterChannel_ = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, ExternalCellTag_);

            {
                TTimingGuard timingGuard(&Profiler, "/begin_upload_time");

                YT_LOG_DEBUG("Starting journal upload");

                auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, NativeCellTag_);
                TObjectServiceProxy proxy(channel);

                auto batchReq = proxy.ExecuteBatch();

                {
                    auto* prerequisitesExt = batchReq->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
                    for (auto id : Options_.PrerequisiteTransactionIds) {
                        auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
                        ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
                    }
                }

                {
                    auto req = TJournalYPathProxy::BeginUpload(objectIdPath);
                    req->set_update_mode(ToProto<int>(EUpdateMode::Append));
                    req->set_lock_mode(ToProto<int>(ELockMode::Exclusive));
                    req->set_upload_transaction_title(Format("Upload to %v", Path_));
                    req->set_upload_transaction_timeout(ToProto<i64>(Client_->GetNativeConnection()->GetConfig()->UploadTransactionTimeout));
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
                    StartListenTransaction(UploadTransaction_);

                    YT_LOG_DEBUG("Journal upload started (UploadTransactionId: %v)",
                        uploadTransactionId);
                }
            }

            {
                TTimingGuard timingGuard(&Profiler, "/get_extended_attributes_time");

                YT_LOG_DEBUG("Requesting extended journal attributes");

                auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Follower, NativeCellTag_);
                TObjectServiceProxy proxy(channel);

                auto req = TYPathProxy::Get(objectIdPath + "/@");
                AddCellTagToSyncWith(req, ObjectId_);
                SetTransactionId(req, UploadTransaction_);
                std::vector<TString> attributeKeys{
                    "type",
                    "erasure_codec",
                    "replication_factor",
                    "read_quorum",
                    "write_quorum",
                    "account",
                    "primary_medium"
                };
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);

                auto rspOrError = WaitFor(proxy.Execute(req));
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    rspOrError,
                    "Error requesting extended attributes of journal %v",
                    Path_);

                auto rsp = rspOrError.Value();
                auto attributes = ConvertToAttributes(TYsonString(rsp->value()));
                ErasureCodec_ = attributes->Get<NErasure::ECodec>("erasure_codec");
                ReplicationFactor_ = attributes->Get<int>("replication_factor");
                ReadQuorum_ = attributes->Get<int>("read_quorum");
                WriteQuorum_ = attributes->Get<int>("write_quorum");
                Account_ = attributes->Get<TString>("account");
                PrimaryMedium_ = attributes->Get<TString>("primary_medium");

                YT_LOG_DEBUG("Extended journal attributes received (ErasureCodec: %v, ReplicationFactor: %v, WriteQuorum: %v, "
                    "Account: %v, PrimaryMedium: %v)",
                    ErasureCodec_,
                    ReplicationFactor_,
                    WriteQuorum_,
                    Account_,
                    PrimaryMedium_);
            }

            {
                TTimingGuard timingGuard(&Profiler, "/get_upload_parameters_time");

                YT_LOG_DEBUG("Requesting journal upload parameters");

                auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Follower, ExternalCellTag_);
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
                CurrentRowIndex_ = rsp->row_count();

                YT_LOG_DEBUG("Journal upload parameters received (ChunkListId: %v, RowCount: %v)",
                    ChunkListId_,
                    CurrentRowIndex_);
            }

            YT_LOG_DEBUG("Journal opened");
            OpenedPromise_.Set(TError());
        }

        void CloseJournal()
        {
            YT_LOG_DEBUG("Closing journal");

            TTimingGuard timingGuard(&Profiler, "/end_upload_time");

            auto objectIdPath = FromObjectId(ObjectId_);

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, NativeCellTag_);
            TObjectServiceProxy proxy(channel);

            auto batchReq = proxy.ExecuteBatch();

            {
                auto* prerequisitesExt = batchReq->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
                for (auto id : Options_.PrerequisiteTransactionIds) {
                    auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
                    ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
                }
            }

            StopListenTransaction(UploadTransaction_);

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

            ClosedPromise_.TrySet(TError());

            YT_LOG_DEBUG("Journal closed");
        }

        TChunkSessionPtr TryOpenChunkSession(int sessionIndex)
        {
            auto session = New<TChunkSession>(sessionIndex);

            TTimingGuard timingGuard(&Profiler, "/open_session_time");
            TWallTimer timer;

            YT_LOG_DEBUG("Creating chunk");

            {
                TTimingGuard timingGuard(&Profiler, "/create_chunk_time");

                TChunkServiceProxy proxy(UploadMasterChannel_);

                auto batchReq = proxy.ExecuteBatch();
                GenerateMutationId(batchReq);
                batchReq->set_suppress_upstream_sync(true);
                if (Config_->PreallocateChunks) {
                    batchReq->RequireServerFeature(EMasterFeature::OverlayedJournals);
                }

                auto* req = batchReq->add_create_chunk_subrequests();
                req->set_type(ToProto<int>(ErasureCodec_ == NErasure::ECodec::None ? EObjectType::JournalChunk : EObjectType::ErasureJournalChunk));
                req->set_account(Account_);
                ToProto(req->mutable_transaction_id(), UploadTransaction_->GetId());
                req->set_replication_factor(ReplicationFactor_);
                req->set_medium_name(PrimaryMedium_);
                req->set_erasure_codec(ToProto<int>(ErasureCodec_));
                req->set_read_quorum(ReadQuorum_);
                req->set_write_quorum(WriteQuorum_);
                req->set_movable(true);
                req->set_vital(true);
                req->set_overlayed(Config_->PreallocateChunks);

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    GetCumulativeError(batchRspOrError),
                    "Error creating chunk");

                const auto& batchRsp = batchRspOrError.Value();
                const auto& rsp = batchRsp->create_chunk_subresponses(0);

                session->Id = FromProto<TSessionId>(rsp.session_id());
            }

            YT_LOG_DEBUG("Chunk created (SessionId: %v, ElapsedTime: %v)",
                session->Id,
                timer.GetElapsedTime());

            int replicaCount = ErasureCodec_ == NErasure::ECodec::None
                ? ReplicationFactor_
                : NErasure::GetCodec(ErasureCodec_)->GetTotalPartCount();

            TChunkReplicaWithMediumList replicas;
            try {
                TTimingGuard timingGuard(&Profiler, "/allocate_write_targets_time");
                replicas = AllocateWriteTargets(
                    Client_,
                    session->Id,
                    replicaCount,
                    replicaCount,
                    std::nullopt,
                    Config_->PreferLocalHost,
                    GetBannedNodes(),
                    NodeDirectory_,
                    Logger);
            } catch (const std::exception& ex) {
                YT_LOG_WARNING(TError(ex));
                return nullptr;
            }

            YT_VERIFY(replicas.size() == replicaCount);
            if (ErasureCodec_ != NErasure::ECodec::None) {
                for (int index = 0; index < replicaCount; ++index) {
                    replicas[index] = TChunkReplicaWithMedium(replicas[index].GetNodeId(), index, replicas[index].GetMediumIndex());
                }
            }

            for (int index = 0; index < replicas.size(); ++index) {
                auto replica = replicas[index];
                const auto& descriptor = NodeDirectory_->GetDescriptor(replica);
                auto lightChannel = Client_->GetChannelFactory()->CreateChannel(descriptor);
                auto heavyChannel = CreateRetryingChannel(
                    Config_->NodeChannel,
                    lightChannel,
                    BIND([] (const TError& error) {
                        return error.FindMatching(NChunkClient::EErrorCode::WriteThrottlingActive).operator bool();
                    }));
                auto node = New<TNode>(
                    index,
                    descriptor,
                    std::move(lightChannel),
                    std::move(heavyChannel),
                    Config_->NodeRpcTimeout,
                    TTagIdList{TProfileManager::Get()->RegisterTag("replica_address", descriptor.GetDefaultAddress())});
                session->Nodes.push_back(node);
            }

            YT_LOG_DEBUG("Starting chunk session at nodes (ElapsedTime: %v)",
                timer.GetElapsedTime());

            try {
                TTimingGuard timingGuard(&Profiler, "/start_node_session_time");

                std::vector<TFuture<void>> futures;
                for (const auto& node : session->Nodes) {
                    auto req = node->LightProxy.StartChunk();
                    ToProto(req->mutable_session_id(), GetSessionIdForNode(session, node));
                    ToProto(req->mutable_workload_descriptor(), Config_->WorkloadDescriptor);
                    req->set_enable_multiplexing(Options_.EnableMultiplexing);

                    futures.push_back(req->Invoke().Apply(
                        BIND(&TImpl::OnChunkStarted, MakeStrong(this), session, node)
                            .AsyncVia(Invoker_)));
                }

                auto result = WaitFor(AllSucceeded(
                    futures,
                    TFutureCombinerOptions{.CancelInputOnShortcut = false}));
                THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error starting chunk sessions");
            } catch (const std::exception& ex) {
                YT_LOG_WARNING(TError(ex));
                return nullptr;
            }

            YT_LOG_DEBUG("Chunk session started at nodes (ElapsedTime: %v)",
                timer.GetElapsedTime());

            for (const auto& node : session->Nodes) {
                node->PingExecutor = New<TPeriodicExecutor>(
                    Invoker_,
                    BIND(&TImpl::SendPing, MakeWeak(this), MakeWeak(session), MakeWeak(node)),
                    Config_->NodePingPeriod);
                node->PingExecutor->Start();
            }

            auto chunkId = session->Id.ChunkId;

            YT_LOG_DEBUG("Confirming chunk (ElapsedTime: %v)",
                timer.GetElapsedTime());

            {
                TTimingGuard timingGuard(&Profiler, "/confirm_chunk_time");

                TChunkServiceProxy proxy(UploadMasterChannel_);
                auto batchReq = proxy.ExecuteBatch();
                GenerateMutationId(batchReq);
                batchReq->set_suppress_upstream_sync(true);

                YT_VERIFY(!replicas.empty());
                auto* req = batchReq->add_confirm_chunk_subrequests();
                ToProto(req->mutable_chunk_id(), chunkId);
                req->mutable_chunk_info();
                ToProto(req->mutable_replicas(), replicas);
                auto* meta = req->mutable_chunk_meta();
                meta->set_type(ToProto<int>(EChunkType::Journal));
                meta->set_version(0);
                TMiscExt miscExt;
                SetProtoExtension(meta->mutable_extensions(), miscExt);

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    GetCumulativeError(batchRspOrError),
                    "Error confirming chunk %v",
                    chunkId);
            }
            YT_LOG_DEBUG("Chunk confirmed (ElapsedTime: %v)",
                timer.GetElapsedTime());

            YT_LOG_DEBUG("Attaching chunk (ElapsedTime: %v)",
                timer.GetElapsedTime());
            {
                TTimingGuard timingGuard(&Profiler, "/attach_chunk_time");

                TChunkServiceProxy proxy(UploadMasterChannel_);
                auto batchReq = proxy.ExecuteBatch();
                GenerateMutationId(batchReq);
                batchReq->set_suppress_upstream_sync(true);

                auto* req = batchReq->add_attach_chunk_trees_subrequests();
                ToProto(req->mutable_parent_id(), ChunkListId_);
                ToProto(req->add_child_ids(), chunkId);

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    GetCumulativeError(batchRspOrError),
                    "Error attaching chunk %v",
                    chunkId);
            }
            YT_LOG_DEBUG("Chunk attached (ElapsedTime: %v)",
                timer.GetElapsedTime());

            return session;
        }

        void ScheduleChunkSessionAllocation()
        {
            if (AllocatedChunkSessionPromise_) {
                return;
            }
            
            AllocatedChunkSessionIndex_ = NextChunkSessionIndex_++;
            AllocatedChunkSessionPromise_ = NewPromise<TChunkSessionPtr>();
            
            YT_LOG_DEBUG("Scheduling chunk session allocation (SessionIndex: %v)",
                AllocatedChunkSessionIndex_);

            ScheduleAllocateChunkSession(AllocatedChunkSessionPromise_, AllocatedChunkSessionIndex_);
        }
        
        void ScheduleAllocateChunkSession(TPromise<TChunkSessionPtr> promise, int sessionIndex)
        {
            BIND(&TImpl::TryOpenChunkSession, MakeStrong(this), sessionIndex)
                .AsyncVia(Invoker_)
                .Run()
                .Subscribe(
                    BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TChunkSessionPtr>& sessionOrError) {
                        if (!sessionOrError.IsOK()) {
                            YT_LOG_WARNING(sessionOrError, "Failed to allocate chunk session (SessionIndex: %v)",
                                sessionIndex);
                            promise.TrySet(sessionOrError);
                            return;
                        }

                        const auto& session = sessionOrError.Value();
                        if (!session) {
                            YT_LOG_DEBUG("Failed to allocate chunk session; backing off and retrying (SessionIndex: %v, BackoffTime: %v)",
                                sessionIndex,
                                Config_->OpenSessionBackoffTime);

                            TDelayedExecutor::Submit(
                                BIND(&TImpl::ScheduleAllocateChunkSession, MakeWeak(this), promise, sessionIndex)
                                    .Via(Invoker_),
                                Config_->OpenSessionBackoffTime);
                            return;
                        }

                        // NB: Avoid overwriting EChunkSessionState::Discarded state.
                        if (session->State == EChunkSessionState::Allocating) {
                            session->State = EChunkSessionState::Allocated;
                        }                         

                        YT_LOG_DEBUG("Chunk session allocated (SessionIndex: %v, SessionId: %v, SessionState: %v)",
                            sessionIndex,
                            session->Id,
                            session->State);
                        promise.TrySet(session);
                    }).Via(Invoker_));
        }

        TChunkSessionPtr OpenChunkSession()
        {
            while (true) {
                ScheduleChunkSessionAllocation();
                
                auto future = AllocatedChunkSessionPromise_.ToFuture();
                
                AllocatedChunkSessionIndex_ = -1;
                AllocatedChunkSessionPromise_.Reset();

                auto session = WaitFor(future)
                    .ValueOrThrow();

                if (Config_->PreallocateChunks) {
                    ScheduleChunkSessionAllocation();
                }

                if (session->State != EChunkSessionState::Allocated) {
                    YT_LOG_DEBUG("Dropping chunk session due to invalid state (SessionId: %v, SessionState: %v)",
                        session->Id,
                        session->State);
                    continue;
                }

                YT_VERIFY(session->State == EChunkSessionState::Allocated);
                session->State = EChunkSessionState::Current;

                return session;
            }
        }

        void OpenChunk()
        {
            CurrentChunkSession_ = OpenChunkSession();

            if (!PendingBatches_.empty()) {
                const auto& firstBatch = PendingBatches_.front();
                const auto& lastBatch = PendingBatches_.back();
                YT_LOG_DEBUG("Batches re-enqueued (Rows: %v-%v, Session: %v)",
                    firstBatch->FirstRowIndex,
                    lastBatch->FirstRowIndex + lastBatch->RowCount - 1,
                    CurrentChunkSession_->Id);

                for (const auto& batch : PendingBatches_) {
                    EnqueueBatchToCurrentChunkSession(batch);
                }
            }

            TDelayedExecutor::Submit(
                BIND(&TImpl::OnSessionTimeout, MakeWeak(this), MakeWeak(CurrentChunkSession_)),
                Config_->MaxChunkSessionDuration);
        }

        void OnSessionTimeout(const TWeakPtr<TChunkSession>& session_)
        {
            auto session = session_.Lock();
            if (!session) {
                return;
            }

            YT_LOG_DEBUG("Session timeout; requesting chunk switch");
            ScheduleChunkSessionSwitch(session);
        }

        void WriteChunk()
        {
            while (true) {
                ValidateAborted();
                auto command = DequeueCommand();
                auto mustBreak = false;
                Visit(command,
                    [&] (TCloseCommand) {
                        HandleClose();
                        mustBreak = true;
                    },
                    [&] (TCancelCommand) {
                        HandleCancel();
                    },
                    [&] (const TBatchCommand& typedCommand) {
                        const auto& batch = typedCommand.Batch;

                        YT_LOG_DEBUG("Batch enqueued (Rows: %v-%v)",
                            batch->FirstRowIndex,
                            batch->FirstRowIndex + batch->RowCount - 1);

                        HandleBatch(batch);
                    },
                    [&] (const TSwitchChunkCommand& typedCommand) {
                        if (typedCommand.Session != CurrentChunkSession_) {
                            return;
                        }
                        mustBreak = true;
                    });

                if (mustBreak) {
                    YT_LOG_DEBUG("Switching chunk");
                    break;
                }
            }
        }

        void HandleClose()
        {
            YT_LOG_DEBUG("Closing journal writer");
            Closing_ = true;
        }

        [[noreturn]] void HandleCancel()
        {
            if (AllocatedChunkSessionPromise_) {
                AllocatedChunkSessionPromise_.TrySet(TError(NYT::EErrorCode::Canceled, "Writer canceled"));
            }
            throw TFiberCanceledException();
        }

        void HandleBatch(const TBatchPtr& batch)
        {
            if (ErasureCodec_ != NErasure::ECodec::None) {
                batch->ErasureRows = EncodeErasureJournalRows(ErasureCodec_, batch->Rows);
                batch->Rows.clear();
            }
            PendingBatches_.push_back(batch);
            EnqueueBatchToCurrentChunkSession(batch);
        }

        void EnqueueBatchToCurrentChunkSession(const TBatchPtr& batch)
        {
            // Check flushed replica count: this batch might have already been
            // flushed (partially) by the previous (failed session).
            if (batch->FlushedReplicas > 0) {
                YT_LOG_DEBUG("Resetting flushed replica counter (Rows: %v-%v, FlushCounter: %v)",
                    batch->FirstRowIndex,
                    batch->FirstRowIndex + batch->RowCount - 1,
                    batch->FlushedReplicas);
                batch->FlushedReplicas = 0;
            }

            if (CurrentChunkSession_->FirstRowIndex < 0) {
                auto firstRowIndex = batch->FirstRowIndex;

                YT_LOG_DEBUG("Initializing first row index of chunk session (SessionId: %v, FirstRowIndex: %v)",
                    CurrentChunkSession_->Id,
                    firstRowIndex);

                CurrentChunkSession_->FirstRowIndex = firstRowIndex;
                for (const auto& node : CurrentChunkSession_->Nodes) {
                    node->FirstPendingRowIndex = firstRowIndex;
                }

                NJournalClient::NProto::TOverlayedJournalChunkHeader header;
                header.set_first_row_index(firstRowIndex);

                CurrentChunkSession_->HeaderRow = SerializeProtoToRef(header);
            }

            for (const auto& node : CurrentChunkSession_->Nodes) {
                node->PendingBatches.push(batch);
                MaybeFlushBlocks(CurrentChunkSession_, node);
            }
        }

        void CloseChunk()
        {
            // Release the current session to prevent writing more rows
            // or detecting failed pings.
            auto session = CurrentChunkSession_;
            CurrentChunkSession_.Reset();

            session->State = EChunkSessionState::Discarded;

            auto sessionId = session->Id;

            YT_LOG_DEBUG("Finishing chunk session");

            for (const auto& node : session->Nodes) {
                auto req = node->LightProxy.FinishChunk();
                ToProto(req->mutable_session_id(), GetSessionIdForNode(session, node));
                req->Invoke().Subscribe(
                    BIND(&TImpl::OnChunkFinished, MakeStrong(this), node)
                        .Via(Invoker_));
                if (node->PingExecutor) {
                    node->PingExecutor->Stop();
                    node->PingExecutor.Reset();
                }
            }

            if (!Config_->PreallocateChunks) {
                TTimingGuard timingGuard(&Profiler, "/seal_chunk_time");

                YT_LOG_DEBUG("Sealing chunk (SessionId: %v, RowCount: %v)",
                    sessionId,
                    session->FlushedRowCount);

                TChunkServiceProxy proxy(UploadMasterChannel_);

                auto batchReq = proxy.ExecuteBatch();
                GenerateMutationId(batchReq);
                batchReq->set_suppress_upstream_sync(true);

                auto* req = batchReq->add_seal_chunk_subrequests();
                ToProto(req->mutable_chunk_id(), sessionId.ChunkId);
                req->mutable_info()->set_row_count(session->FlushedRowCount);
                req->mutable_info()->set_uncompressed_data_size(session->FlushedDataSize);
                req->mutable_info()->set_compressed_data_size(session->FlushedDataSize);

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    GetCumulativeError(batchRspOrError),
                    "Error sealing chunk %v",
                    sessionId);

                YT_LOG_DEBUG("Chunk sealed (SessionId: %v)",
                    sessionId);
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
                    YT_LOG_ERROR(ex, "Error pumping journal writer command queue");
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
            YT_LOG_WARNING(error, "Journal writer failed");

            {
                TGuard<TAdaptiveLock> guard(CurrentBatchSpinLock_);
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

            for (const auto& batch : PendingBatches_) {
                batch->FlushedPromise.Set(error);
            }
            PendingBatches_.clear();

            while (true) {
                auto command = DequeueCommand();
                Visit(command,
                    [&] (const TBatchCommand& typedCommand) {
                        const auto& batch = typedCommand.Batch;
                        batch->FlushedPromise.Set(error);
                    },
                    [&] (TCancelCommand) {
                        throw TFiberCanceledException();
                    },
                    [&] (const auto&) {
                        // ignore
                    });
            }
        }


        TFuture<void> AppendToBatch(const TBatchPtr& batch, const TSharedRef& row)
        {
            YT_ASSERT(row);
            batch->Rows.push_back(row);
            batch->RowCount += 1;
            batch->DataSize += row.Size();
            ++CurrentRowIndex_;
            return batch->FlushedPromise;
        }

        TBatchPtr EnsureCurrentBatch()
        {
            VERIFY_SPINLOCK_AFFINITY(CurrentBatchSpinLock_);

            if (CurrentBatch_ &&
                (CurrentBatch_->RowCount >= Config_->MaxBatchRowCount ||
                 CurrentBatch_->DataSize >= Config_->MaxBatchDataSize))
            {
                FlushCurrentBatch();
            }

            if (!CurrentBatch_) {
                CurrentBatch_ = New<TBatch>();
                CurrentBatch_->StartTime = GetCpuInstant();
                CurrentBatch_->FirstRowIndex = CurrentRowIndex_;
                CurrentBatchFlushCookie_ = TDelayedExecutor::Submit(
                    BIND(&TImpl::OnBatchTimeout, MakeWeak(this), CurrentBatch_)
                        .Via(Invoker_),
                    Config_->MaxBatchDelay);
            }

            return CurrentBatch_;
        }

        void OnBatchTimeout(const TBatchPtr& batch)
        {
            TGuard<TAdaptiveLock> guard(CurrentBatchSpinLock_);
            if (CurrentBatch_ == batch) {
                FlushCurrentBatch();
            }
        }

        void FlushCurrentBatch()
        {
            VERIFY_SPINLOCK_AFFINITY(CurrentBatchSpinLock_);

            TDelayedExecutor::CancelAndClear(CurrentBatchFlushCookie_);

            YT_LOG_DEBUG("Flushing batch (Rows: %v-%v, DataSize: %v)",
                CurrentBatch_->FirstRowIndex,
                CurrentBatch_->FirstRowIndex + CurrentBatch_->RowCount - 1,
                CurrentBatch_->DataSize);

            EnqueueCommand(TBatchCommand{CurrentBatch_});
            CurrentBatch_.Reset();
        }


        void SendPing(
            const TChunkSessionWeakPtr& weakSession,
            const TNodeWeakPtr& weakNode)
        {
            auto session = weakSession.Lock();
            if (!session) {
                return;
            }

            auto node = weakNode.Lock();
            if (!node) {
                return;
            }

            if (!node->Started) {
                return;
            }

            YT_LOG_DEBUG("Sending ping (Address: %v, SessionId: %v)",
                node->Descriptor.GetDefaultAddress(),
                session->Id);

            auto req = node->LightProxy.PingSession();
            ToProto(req->mutable_session_id(), GetSessionIdForNode(session, node));
            req->Invoke().Subscribe(
                BIND(&TImpl::OnPingSent, MakeWeak(this), session, node)
                    .Via(Invoker_));
        }

        void OnPingSent(
            const TChunkSessionPtr& session,
            const TNodePtr& node,
            TDataNodeServiceProxy::TErrorOrRspPingSessionPtr rspOrError)
        {
            if (SimulateReplicaFailure(node, session)) {
                return;
            }
            if (rspOrError.IsOK()) {
                YT_LOG_DEBUG("Ping succeeded (Address: %v, SessionId: %v)",
                    node->Descriptor.GetDefaultAddress(),
                    session->Id);

                const auto& rsp = rspOrError.Value();
                if (rsp->close_demanded()) {
                    OnReplicaCloseDemanded(node, session);
                }
            } else {
                OnReplicaFailure(rspOrError, node, session);
            }
        }


        void OnChunkStarted(
            const TChunkSessionPtr& session,
            const TNodePtr& node,
            const TDataNodeServiceProxy::TErrorOrRspStartChunkPtr& rspOrError)
        {
            if (rspOrError.IsOK()) {
                YT_LOG_DEBUG("Chunk session started at node (Address: %v)",
                    node->Descriptor.GetDefaultAddress());
                node->Started = true;
                if (CurrentChunkSession_ == session) {
                    MaybeFlushBlocks(CurrentChunkSession_, node);
                }
            } else {
                YT_LOG_WARNING(rspOrError, "Session has failed to start at node; requesting chunk switch (SessionId: %v, Address: %v)",
                    session->Id,
                    node->Descriptor.GetDefaultAddress());
                ScheduleChunkSessionSwitch(session);
                BanNode(node->Descriptor.GetDefaultAddress());
                THROW_ERROR_EXCEPTION("Error starting session at %v",
                    node->Descriptor.GetDefaultAddress())
                    << rspOrError;
            }
        }

        void OnChunkFinished(
            const TNodePtr& node,
            const TDataNodeServiceProxy::TErrorOrRspFinishChunkPtr& rspOrError)
        {
            if (rspOrError.IsOK()) {
                YT_LOG_DEBUG("Chunk session finished at node (Address: %v)",
                    node->Descriptor.GetDefaultAddress());
            } else {
                BanNode(node->Descriptor.GetDefaultAddress());
                YT_LOG_WARNING(rspOrError, "Chunk session has failed to finish at node (Address: %v)",
                    node->Descriptor.GetDefaultAddress());
            }
        }


        void MaybeFlushBlocks(const TChunkSessionPtr& session, const TNodePtr& node)
        {
            if (!node->Started) {
                return;
            }

            if (session->SwitchScheduled) {
                return;
            }

            YT_VERIFY(node->FirstPendingRowIndex >= 0);

            if (!node->InFlightBatches.empty()) {
                auto lagTime = GetCpuInstant() - node->InFlightBatches.front()->StartTime;
                UpdateReplicaLag(session, node, lagTime);
                return;
            }

            if (node->PendingBatches.empty()) {
                UpdateReplicaLag(session, node, 0);
                return;
            }

            auto lagTime = GetCpuInstant() - node->PendingBatches.front()->StartTime;
            UpdateReplicaLag(session, node, lagTime);

            i64 flushRowCount = 0;
            i64 flushDataSize = 0;

            auto req = node->HeavyProxy.PutBlocks();
            req->SetMultiplexingBand(EMultiplexingBand::Heavy);
            ToProto(req->mutable_session_id(), GetSessionIdForNode(CurrentChunkSession_, node));
            req->set_flush_blocks(true);

            if (Config_->PreallocateChunks) {
                if (node->FirstPendingBlockIndex == 0 && Config_->PreallocateChunks) {
                    req->set_first_block_index(0);
                    req->Attachments().push_back(CurrentChunkSession_->HeaderRow);
                } else {
                    req->set_first_block_index(node->FirstPendingBlockIndex + 1);
                }
            } else {
                req->set_first_block_index(node->FirstPendingBlockIndex);
            }

            YT_VERIFY(node->InFlightBatches.empty());
            while (flushRowCount <= Config_->MaxFlushRowCount &&
                   flushDataSize <= Config_->MaxFlushDataSize &&
                   !node->PendingBatches.empty())
            {
                auto batch = node->PendingBatches.front();
                node->PendingBatches.pop();

                const auto& rows = ErasureCodec_ == NErasure::ECodec::None
                    ? batch->Rows
                    : batch->ErasureRows[node->Index];
                req->Attachments().insert(req->Attachments().end(), rows.begin(), rows.end());

                flushRowCount += batch->RowCount;
                flushDataSize += GetByteSize(rows);

                node->InFlightBatches.push_back(batch);
            }

            YT_LOG_DEBUG("Writing journal replica (Address: %v, BlockIds: %v:%v-%v, Rows: %v-%v, DataSize: %v, LagTime: %v)",
                node->Descriptor.GetDefaultAddress(),
                CurrentChunkSession_->Id,
                node->FirstPendingBlockIndex,
                node->FirstPendingBlockIndex + flushRowCount - 1,
                node->FirstPendingRowIndex,
                node->FirstPendingRowIndex + flushRowCount - 1,
                flushDataSize,
                CpuDurationToDuration(lagTime));

            req->Invoke().Subscribe(
                BIND(&TImpl::OnBlocksWritten, MakeWeak(this), CurrentChunkSession_, node, flushRowCount)
                    .Via(Invoker_));
        }

        void OnBlocksWritten(
            const TChunkSessionPtr& session,
            const TNodePtr& node,
            i64 flushRowCount,
            TDataNodeServiceProxy::TErrorOrRspPutBlocksPtr rspOrError)
        {
            if (session != CurrentChunkSession_) {
                return;
            }

            if (SimulateReplicaFailure(node, session)) {
                return;
            }

            if (!rspOrError.IsOK()) {
                OnReplicaFailure(rspOrError, node, session);
                return;
            }

            YT_LOG_DEBUG("Journal replica written (Address: %v, BlockIds: %v:%v-%v, Rows: %v-%v)",
                node->Descriptor.GetDefaultAddress(),
                session->Id,
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
                if (front->FlushedReplicas <  WriteQuorum_) {
                    break;
                }

                fulfilledPromises.push_back(front->FlushedPromise);
                session->FlushedRowCount += front->RowCount;
                session->FlushedDataSize += front->DataSize;
                PendingBatches_.pop_front();

                YT_LOG_DEBUG("Rows are written by quorum (Rows: %v-%v)",
                    front->FirstRowIndex,
                    front->FirstRowIndex + front->RowCount - 1);
            }

            for (const auto& promise : fulfilledPromises) {
                promise.Set();
            }

            if (!session->SwitchScheduled && session->FlushedRowCount >= Config_->MaxChunkRowCount) {
                YT_LOG_DEBUG("Chunk row count limit exceeded; requesting chunk switch (RowCount: %v, SessionId: %v)",
                    session->FlushedRowCount,
                    session->Id);
                ScheduleChunkSessionSwitch(session);
            }
            
            if (!session->SwitchScheduled && session->FlushedDataSize >= Config_->MaxChunkDataSize) {
                YT_LOG_DEBUG("Chunk data size limit exceeded; requesting chunk switch (DataSize: %v, SessionId: %v)",
                    session->FlushedDataSize,
                    session->Id);
                ScheduleChunkSessionSwitch(session);
            }

            MaybeFlushBlocks(CurrentChunkSession_, node);
        }

        bool SimulateReplicaFailure(
            const TNodePtr& node,
            const TChunkSessionPtr& session)
        {
            if (Config_->ReplicaFailureProbability == 0.0 ||
                RandomNumber<double>() >= Config_->ReplicaFailureProbability)
            {
                return false;
            }
            const auto& address = node->Descriptor.GetDefaultAddress();
            YT_LOG_WARNING("Simulated journal replica failure; requesting switch (Address: %v, SessionId: %v)",
                address,
                session->Id);
            ScheduleChunkSessionSwitch(session);
            return true;
        }

        void OnReplicaFailure(
            const TError& error,
            const TNodePtr& node,
            const TChunkSessionPtr& session)
        {
            const auto& address = node->Descriptor.GetDefaultAddress();
            YT_LOG_WARNING(error, "Journal replica failure; requesting switch (Address: %v, SessionId: %v)",
                address,
                session->Id);
            ScheduleChunkSessionSwitch(session);
            BanNode(address);
        }

        static bool IsChunkSessionAlive(const TChunkSessionPtr& session)
        {
            return
                session &&
                (session->State == EChunkSessionState::Allocated || session->State == EChunkSessionState::Current) &&
                !session->SwitchScheduled;
        }

        bool IsSafeToSwitchSessionOnDemand()
        {
            if (!Config_->PreallocateChunks) {
                return true;
            }

            if (!IsChunkSessionAlive(CurrentChunkSession_)) {
                return false;
            }

            if (!AllocatedChunkSessionPromise_) {
                return false;
            }

            if (!AllocatedChunkSessionPromise_.IsSet()) {
                return false;
            }

            const auto& preallocatedSessionOrError = AllocatedChunkSessionPromise_.Get();
            if (!preallocatedSessionOrError.IsOK()) {
                return false;
            }

            const auto& preallocatedSession = preallocatedSessionOrError.Value();
            if (!IsChunkSessionAlive(preallocatedSession)) {
                return false;
            }

            return true;
        }

        void OnReplicaCloseDemanded(
            const TNodePtr& node,
            const TChunkSessionPtr& session)
        {
            const auto& address = node->Descriptor.GetDefaultAddress();
            BanNode(address);
            if (IsSafeToSwitchSessionOnDemand()) {
                YT_LOG_DEBUG("Journal replica has demanded to close the session; requesting switch (Address: %v, SessionId: %v)",
                    address,
                    session->Id);
                ScheduleChunkSessionSwitch(session);
            } else {
                YT_LOG_DEBUG("Journal replica has demanded to close the session but switching is not safe at the moment; ignoring (Address: %v, SessionId: %v)",
                    address,
                    session->Id);
            }
        }

        
        void ScheduleChunkSessionSwitch(const TChunkSessionPtr& session)
        {
            if (session->SwitchScheduled) {
                YT_LOG_DEBUG("Chunk session is already switched (SessionId: %v)",
                    session->Id);
                return;
            }
            session->SwitchScheduled = true;

            YT_LOG_DEBUG("Scheduling chunk session switch (SessionId: %v, SessionState: %v)",
                session->Id,
                session->State);
            
            switch (session->State) {
                case EChunkSessionState::Current:
                    EnqueueCommand(TSwitchChunkCommand{session});
                    break;

                case EChunkSessionState::Allocating:
                case EChunkSessionState::Allocated:
                    session->State = EChunkSessionState::Discarded;
                    if (AllocatedChunkSessionIndex_ == session->Index) {
                        YT_LOG_DEBUG("Resetting chunk session promise");
                        AllocatedChunkSessionIndex_ = -1;
                        AllocatedChunkSessionPromise_.Reset();
                        if (Config_->PreallocateChunks) {
                            ScheduleChunkSessionAllocation();
                        }
                    }
                    break;

                case EChunkSessionState::Discarded:
                    break;

                default:
                    YT_ABORT();
            }
        }

        
        void UpdateReplicaLag(const TChunkSessionPtr& session, const TNodePtr& node, TCpuDuration lagTime)
        {
            node->LagTime = lagTime;

            std::vector<std::pair<NProfiling::TCpuDuration, int>> replicas;
            for (int index = 0; index < session->Nodes.size(); ++index) {
                replicas.emplace_back(session->Nodes[index]->LagTime, index);
            }

            std::sort(replicas.begin(), replicas.end());

            Profiler.Update(session->WriteQuorumLag, CpuDurationToValue(replicas[WriteQuorum_ - 1].first));
            Profiler.Update(session->MaxReplicaLag, CpuDurationToValue(replicas.back().first));

            YT_LOG_DEBUG("Journal replicas lag updated (Replicas: %v)",
                MakeFormattableView(replicas, [&] (auto* builder, const auto& replica) {
                    builder->AppendFormat("%v=>%v",
                        session->Nodes[replica.second]->Descriptor.GetDefaultAddress(),
                        CpuDurationToDuration(replica.first));
                }));
        }
        
        TSessionId GetSessionIdForNode(const TChunkSessionPtr& session, const TNodePtr& node)
        {
            auto chunkId = ErasureCodec_ == NErasure::ECodec::None
                ? session->Id.ChunkId
                : EncodeChunkId(TChunkIdWithIndex(session->Id.ChunkId, node->Index));
            return TSessionId(chunkId, session->Id.MediumIndex);
        }
    };


    const TIntrusivePtr<TImpl> Impl_;
};

IJournalWriterPtr CreateJournalWriter(
    IClientPtr client,
    TYPath path,
    TJournalWriterOptions options)
{
    return New<TJournalWriter>(
        std::move(client),
        std::move(path),
        std::move(options));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
