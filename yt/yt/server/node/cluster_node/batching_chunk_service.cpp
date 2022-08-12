#include "batching_chunk_service.h"
#include "config.h"
#include "private.h"

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/ytlib/node_tracker_client/node_directory_builder.h>

#include <yt/yt/ytlib/chunk_client/chunk_service_proxy.h>

#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/ytlib/hydra/peer_channel.h>

#include <yt/yt/ytlib/security_client/public.h>

#include <yt/yt/core/rpc/service_detail.h>
#include <yt/yt/core/rpc/helpers.h>
#include <yt/yt/core/rpc/dispatcher.h>
#include <yt/yt/core/rpc/retrying_channel.h>

#include <yt/yt/core/concurrency/thread_affinity.h>
#include <yt/yt/core/concurrency/delayed_executor.h>
#include <yt/yt/core/concurrency/throughput_throttler.h>

namespace NYT::NClusterNode {

using namespace NRpc;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NElection;
using namespace NApi;
using namespace NApi::NNative;
using namespace NHydra;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

class TBatchingChunkService
    : public NRpc::TServiceBase
{
public:
    TBatchingChunkService(
        TCellId cellId,
        TBatchingChunkServiceConfigPtr serviceConfig,
        TMasterConnectionConfigPtr connectionConfig,
        IChannelFactoryPtr channelFactory)
        : TServiceBase(
            NRpc::TDispatcher::Get()->GetHeavyInvoker(),
            TChunkServiceProxy::GetDescriptor(),
            ClusterNodeLogger.WithTag("CellTag: %v", CellTagFromId(cellId)),
            cellId)
        , ServiceConfig_(std::move(serviceConfig))
        , ConnectionConfig_(std::move(connectionConfig))
        , LeaderChannel_(CreateMasterChannel(channelFactory, ConnectionConfig_, EPeerKind::Leader))
        , FollowerChannel_(CreateMasterChannel(channelFactory, ConnectionConfig_, EPeerKind::Follower))
        , CostThrottler_(CreateReconfigurableThroughputThrottler(ServiceConfig_->CostThrottler))
        , LocateChunksBatcher_(New<TLocateChunksBatcher>(this))
        , LocateDynamicStoresBatcher_(New<TLocateDynamicStoresBatcher>(this))
        , AllocateWriteTargetsBatcher_(New<TAllocateWriteTargetsBatcher>(this))
        , ExecuteBatchBatcher_(New<TExecuteBatchBatcher>(this))
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(LocateChunks));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(LocateDynamicStores));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AllocateWriteTargets));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ExecuteBatch));
    }

private:
    const TBatchingChunkServiceConfigPtr ServiceConfig_;
    const TMasterConnectionConfigPtr ConnectionConfig_;

    const IChannelPtr LeaderChannel_;
    const IChannelPtr FollowerChannel_;

    const IThroughputThrottlerPtr CostThrottler_;


    static IChannelPtr CreateMasterChannel(
        IChannelFactoryPtr channelFactory,
        TMasterConnectionConfigPtr config,
        EPeerKind peerKind)
    {
        return CreateRetryingChannel(
            config,
            CreatePeerChannel(config, channelFactory, peerKind));
    }

    template <class TRequestMessage, class TResponseMessage, class TState>
    class TBatcherBase
        : public TRefCounted
    {
    public:
        using TResponse = TTypedClientResponse<TResponseMessage>;
        using TResponsePtr = TIntrusivePtr<TResponse>;
        using TRequest = TTypedClientRequest<TRequestMessage, TResponse>;
        using TRequestPtr = TIntrusivePtr<TRequest>;
        using TContext = TTypedServiceContext<TRequestMessage, TResponseMessage>;
        using TContextPtr = TIntrusivePtr<TContext>;

        explicit TBatcherBase(TBatchingChunkService* owner)
            : Owner_(owner)
            , Logger(owner->Logger)
            , LeaderProxy_(owner->LeaderChannel_)
            , FollowerProxy_(owner->FollowerChannel_)
        { }

        void HandleRequest(const TContextPtr& context)
        {
            auto owner = Owner_.Lock();
            if (!owner) {
                return;
            }

            context->SetRequestInfo();

            if (context->IsRetry()) {
                THROW_ERROR_EXCEPTION("Retries are not supported by batcher");
            }

            auto guard = Guard(SpinLock_);

            const auto& user = context->RequestHeader().user();
            auto userIt = UserToBatch_.find(user);
            TBatchPtr currentBatch;

            if (userIt == UserToBatch_.end()) {
                currentBatch = New<TBatch>();

                currentBatch->BatchRequest = CreateBatchRequest();
                auto& request = currentBatch->BatchRequest;
                GenerateMutationId(request);
                request->SetUser(user);
                request->SetTimeout(owner->ConnectionConfig_->RpcTimeout);
                EmplaceOrCrash(UserToBatch_, user, currentBatch);

                TDelayedExecutor::Submit(
                    BIND(&TBatcherBase::OnTimeout, MakeStrong(this), user, currentBatch),
                    owner->ServiceConfig_->MaxBatchDelay);
            } else {
                currentBatch = userIt->second;
            }

            const auto& batchRequest = currentBatch->BatchRequest;

            auto& state = currentBatch->ContextsWithStates.emplace_back(context, TState()).second;
            BatchRequest(&context->Request(), batchRequest.Get(), &state);
            UpdateLogicalRequestWeight(batchRequest, state);

            YT_LOG_DEBUG("Chunk Service request batched (User: %v, RequestId: %v -> %v)",
                user,
                context->GetRequestId(),
                batchRequest->GetRequestId());

            if (GetCost(batchRequest) >= owner->ServiceConfig_->MaxBatchCost) {
                DoFlush(user);
            }
        }

    protected:
        const TWeakPtr<TBatchingChunkService> Owner_;
        const NLogging::TLogger Logger;

        struct TBatch
            : public TRefCounted
        {
            TRequestPtr BatchRequest;
            std::vector<std::pair<TContextPtr, TState>> ContextsWithStates;
        };

        using TBatchPtr = TIntrusivePtr<TBatch>;

        TChunkServiceProxy LeaderProxy_;
        TChunkServiceProxy FollowerProxy_;

        YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, SpinLock_);

        THashMap<TString, TBatchPtr> UserToBatch_;

        virtual TRequestPtr CreateBatchRequest() = 0;
        virtual void BatchRequest(
            const TRequestMessage* request,
            TRequestMessage* batchRequest,
            TState* state) = 0;
        virtual void UnbatchResponse(
            TResponseMessage* response,
            const TResponseMessage* batchResponse,
            const TState& state) = 0;
        virtual int GetCost(const TRequestPtr& request) const = 0;

        virtual void UpdateLogicalRequestWeight(const TRequestPtr& /*request*/, const TState& /*lastState*/) const
        { }


        template <class T>
        static void BatchSubrequests(
            const ::google::protobuf::RepeatedPtrField<T>& src,
            ::google::protobuf::RepeatedPtrField<T>* dst,
            std::vector<int>* indexes)
        {
            for (const auto& subrequest : src) {
                int index = dst->size();
                indexes->push_back(index);
                *dst->Add() = subrequest;
            }
        }

        template <class T>
        static void UnbatchSubresponses(
            const ::google::protobuf::RepeatedPtrField<T>& src,
            ::google::protobuf::RepeatedPtrField<T>* dst,
            const std::vector<int>& indexes)
        {
            for (int index : indexes) {
                *dst->Add() = src.Get(index);
            }
        }

        static void AddReplicaList(TNodeDirectoryBuilder& builder, const NYT::NChunkClient::NProto::TRspLocateChunks_TSubresponse& subresponse)
        {
            builder.Add(FromProto<TChunkReplicaList>(subresponse.replicas()));
        }

        static void AddReplicaList(TNodeDirectoryBuilder& builder, const NYT::NChunkClient::NProto::TRspLocateDynamicStores_TSubresponse& subresponse)
        {
            if (subresponse.has_chunk_spec()) {
                builder.Add(FromProto<TChunkReplicaList>(subresponse.chunk_spec().replicas()));
            }
        }

        static void AddReplicaList(TNodeDirectoryBuilder& builder, const NYT::NChunkClient::NProto::TRspAllocateWriteTargets_TSubresponse& subresponse)
        {
            builder.Add(FromProto<TChunkReplicaList>(subresponse.replicas()));
        }

        template <class TBatchResponse, class TResponse>
        static void BuildResponseNodeDirectory(const TBatchResponse* batchResponse, TResponse* response)
        {
            auto nodeDirectory = New<TNodeDirectory>();
            nodeDirectory->MergeFrom(batchResponse->node_directory());
            TNodeDirectoryBuilder builder(nodeDirectory, response->mutable_node_directory());
            for (const auto& subresponse : response->subresponses()) {
                AddReplicaList(builder, subresponse);
            }
        }

    private:
        void OnTimeout(const TString& user, const TBatchPtr& batch)
        {
            auto guard = Guard(SpinLock_);
            auto userIt = UserToBatch_.find(user);
            if (userIt != UserToBatch_.end() && userIt->second == batch) {
                DoFlush(user);
            }
        }

        void DoFlush(const TString& user)
        {
            VERIFY_SPINLOCK_AFFINITY(SpinLock_);

            auto owner = Owner_.Lock();
            if (!owner) {
                return;
            }

            auto userIt = UserToBatch_.find(user);
            YT_VERIFY(userIt != UserToBatch_.end());

            auto batch = std::move(userIt->second);
            UserToBatch_.erase(userIt);

            auto cost = GetCost(batch->BatchRequest);
            owner->CostThrottler_->Throttle(cost)
                .Subscribe(BIND(&TBatcherBase::DoSendBatch, MakeStrong(this), batch)
                    .Via(owner->GetDefaultInvoker()));
        }

        void DoSendBatch(const TBatchPtr& batch, const TError& /*error*/)
        {
            auto owner = Owner_.Lock();
            if (!owner) {
                return;
            }

            YT_LOG_DEBUG("Chunk Service batch request sent (RequestId: %v)",
                batch->BatchRequest->GetRequestId());

            batch->BatchRequest->Invoke().Subscribe(
                BIND(&TBatcherBase::OnBatchResponse, MakeStrong(this), batch)
                    .Via(owner->GetDefaultInvoker()));
        }

        void OnBatchResponse(const TBatchPtr& batch, const TErrorOr<TResponsePtr>& responseOrError)
        {
            if (responseOrError.IsOK()) {
                YT_LOG_DEBUG("Chunk Service batch request succeeded (RequestId: %v)",
                    batch->BatchRequest->GetRequestId());
            } else {
                YT_LOG_DEBUG(responseOrError, "Chunk Service batch request failed (RequestId: %v)",
                    batch->BatchRequest->GetRequestId());
            }

            for (const auto& [context, state] : batch->ContextsWithStates) {
                if (responseOrError.IsOK()) {
                    UnbatchResponse(&context->Response(), responseOrError.Value().Get(), state);
                    context->Reply();
                } else {
                    context->Reply(responseOrError);
                }
            }
        }
    };


    struct TLocateChunksState
    {
        std::vector<int> Indexes;
    };

    class TLocateChunksBatcher
        : public TBatcherBase<
            NChunkClient::NProto::TReqLocateChunks,
            NChunkClient::NProto::TRspLocateChunks,
            TLocateChunksState>
    {
    public:
        explicit TLocateChunksBatcher(TBatchingChunkService* owner)
            : TBatcherBase(owner)
        { }

    protected:
        TChunkServiceProxy::TReqLocateChunksPtr CreateBatchRequest() override
        {
            auto req = FollowerProxy_.LocateChunks();
            req->SetRequestHeavy(true);
            req->SetResponseHeavy(true);
            return req;
        }

        void BatchRequest(
            const NChunkClient::NProto::TReqLocateChunks* request,
            NChunkClient::NProto::TReqLocateChunks* batchRequest,
            TLocateChunksState* state) override
        {
            BatchSubrequests(request->subrequests(), batchRequest->mutable_subrequests(), &state->Indexes);
        }

        void UnbatchResponse(
            NChunkClient::NProto::TRspLocateChunks* response,
            const NChunkClient::NProto::TRspLocateChunks* batchResponse,
            const TLocateChunksState& state) override
        {
            UnbatchSubresponses(batchResponse->subresponses(), response->mutable_subresponses(), state.Indexes);
            BuildResponseNodeDirectory(batchResponse, response);
            response->set_revision(batchResponse->revision());
        }

        int GetCost(const TRequestPtr& request) const override
        {
            return request->subrequests_size();
        }
    };

    const TIntrusivePtr<TLocateChunksBatcher> LocateChunksBatcher_;

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, LocateChunks)
    {
        LocateChunksBatcher_->HandleRequest(context);
    }


    struct TLocateDynamicStoresState
    {
        std::vector<int> Indexes;
    };

    class TLocateDynamicStoresBatcher
        : public TBatcherBase<
            NChunkClient::NProto::TReqLocateDynamicStores,
            NChunkClient::NProto::TRspLocateDynamicStores,
            TLocateDynamicStoresState>
    {
    public:
        explicit TLocateDynamicStoresBatcher(TBatchingChunkService* owner)
            : TBatcherBase(owner)
        { }

    protected:
        TChunkServiceProxy::TReqLocateDynamicStoresPtr CreateBatchRequest() override
        {
            auto req = FollowerProxy_.LocateDynamicStores();
            req->SetRequestHeavy(true);
            req->SetResponseHeavy(true);
            return req;
        }

        void BatchRequest(
            const NChunkClient::NProto::TReqLocateDynamicStores* request,
            NChunkClient::NProto::TReqLocateDynamicStores* batchRequest,
            TLocateDynamicStoresState* state) override
        {
            BatchSubrequests(request->subrequests(), batchRequest->mutable_subrequests(), &state->Indexes);
            if (request->fetch_all_meta_extensions() || batchRequest->fetch_all_meta_extensions()) {
                batchRequest->set_fetch_all_meta_extensions(true);
                batchRequest->clear_extension_tags();
            } else {
                for (auto extension : request->extension_tags()) {
                    const auto& batched = batchRequest->extension_tags();
                    if (std::find(batched.begin(), batched.end(), extension) == batched.end()) {
                        batchRequest->add_extension_tags(extension);
                    }
                }
            }
        }

        void UnbatchResponse(
            NChunkClient::NProto::TRspLocateDynamicStores* response,
            const NChunkClient::NProto::TRspLocateDynamicStores* batchResponse,
            const TLocateDynamicStoresState& state) override
        {
            UnbatchSubresponses(batchResponse->subresponses(), response->mutable_subresponses(), state.Indexes);
            BuildResponseNodeDirectory(batchResponse, response);
        }

        int GetCost(const TRequestPtr& request) const override
        {
            return request->subrequests().size();
        }
    };

    const TIntrusivePtr<TLocateDynamicStoresBatcher> LocateDynamicStoresBatcher_;

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, LocateDynamicStores)
    {
        LocateDynamicStoresBatcher_->HandleRequest(context);
    }


    struct TAllocateWriteTargetsState
    {
        std::vector<int> Indexes;
    };

    class TAllocateWriteTargetsBatcher
        : public TBatcherBase<
            NChunkClient::NProto::TReqAllocateWriteTargets,
            NChunkClient::NProto::TRspAllocateWriteTargets,
            TAllocateWriteTargetsState>
    {
    public:
        explicit TAllocateWriteTargetsBatcher(TBatchingChunkService* owner)
            : TBatcherBase(owner)
        { }

    protected:
        TChunkServiceProxy::TReqAllocateWriteTargetsPtr CreateBatchRequest() override
        {
            return LeaderProxy_.AllocateWriteTargets();
        }

        void BatchRequest(
            const NChunkClient::NProto::TReqAllocateWriteTargets* request,
            NChunkClient::NProto::TReqAllocateWriteTargets* batchRequest,
            TAllocateWriteTargetsState* state) override
        {
            BatchSubrequests(request->subrequests(), batchRequest->mutable_subrequests(), &state->Indexes);
        }

        void UnbatchResponse(
            NChunkClient::NProto::TRspAllocateWriteTargets* response,
            const NChunkClient::NProto::TRspAllocateWriteTargets* batchResponse,
            const TAllocateWriteTargetsState& state) override
        {
            UnbatchSubresponses(batchResponse->subresponses(), response->mutable_subresponses(), state.Indexes);
            BuildResponseNodeDirectory(batchResponse, response);
        }

        int GetCost(const TRequestPtr& request) const override
        {
            return request->subrequests_size();
        }
    };

    const TIntrusivePtr<TAllocateWriteTargetsBatcher> AllocateWriteTargetsBatcher_;

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, AllocateWriteTargets)
    {
        AllocateWriteTargetsBatcher_->HandleRequest(context);
    }



    struct TExecuteBatchState
    {
        std::vector<int> CreateIndexes;
        std::vector<int> ConfirmIndexes;
        std::vector<int> SealIndexes;
        std::vector<int> AttachIndexes;
    };

    class TExecuteBatchBatcher
        : public TBatcherBase<
            NChunkClient::NProto::TReqExecuteBatch,
            NChunkClient::NProto::TRspExecuteBatch,
            TExecuteBatchState>
    {
    public:
        explicit TExecuteBatchBatcher(TBatchingChunkService* owner)
            : TBatcherBase(owner)
        { }

    protected:
        TChunkServiceProxy::TReqExecuteBatchPtr CreateBatchRequest() override
        {
            return LeaderProxy_.ExecuteBatch();
        }

        void BatchRequest(
            const NChunkClient::NProto::TReqExecuteBatch* request,
            NChunkClient::NProto::TReqExecuteBatch* batchRequest,
            TExecuteBatchState* state) override
        {
            BatchSubrequests(request->create_chunk_subrequests(), batchRequest->mutable_create_chunk_subrequests(), &state->CreateIndexes);
            BatchSubrequests(request->confirm_chunk_subrequests(), batchRequest->mutable_confirm_chunk_subrequests(), &state->ConfirmIndexes);
            BatchSubrequests(request->seal_chunk_subrequests(), batchRequest->mutable_seal_chunk_subrequests(), &state->SealIndexes);
            BatchSubrequests(request->attach_chunk_trees_subrequests(), batchRequest->mutable_attach_chunk_trees_subrequests(), &state->AttachIndexes);
        }

        void UnbatchResponse(
            NChunkClient::NProto::TRspExecuteBatch* response,
            const NChunkClient::NProto::TRspExecuteBatch* batchResponse,
            const TExecuteBatchState& state) override
        {
            UnbatchSubresponses(batchResponse->create_chunk_subresponses(), response->mutable_create_chunk_subresponses(), state.CreateIndexes);
            UnbatchSubresponses(batchResponse->confirm_chunk_subresponses(), response->mutable_confirm_chunk_subresponses(), state.ConfirmIndexes);
            UnbatchSubresponses(batchResponse->seal_chunk_subresponses(), response->mutable_seal_chunk_subresponses(), state.SealIndexes);
            UnbatchSubresponses(batchResponse->attach_chunk_trees_subresponses(), response->mutable_attach_chunk_trees_subresponses(), state.AttachIndexes);
        }

        int GetCost(const TRequestPtr& request) const override
        {
            return
                request->create_chunk_subrequests_size() +
                request->confirm_chunk_subrequests_size() +
                request->seal_chunk_subrequests_size() +
                request->attach_chunk_trees_subrequests_size();
        }

        void UpdateLogicalRequestWeight(const TRequestPtr& request, const TExecuteBatchState& lastState) const override
        {
            auto& header = request->Header();
            header.set_logical_request_weight(
                header.logical_request_weight() + std::ssize(lastState.CreateIndexes));
        }
    };

    const TIntrusivePtr<TExecuteBatchBatcher> ExecuteBatchBatcher_;

    DECLARE_RPC_SERVICE_METHOD(NChunkClient::NProto, ExecuteBatch)
    {
        YT_VERIFY(request->create_chunk_lists_subrequests_size() == 0);
        YT_VERIFY(request->unstage_chunk_tree_subrequests_size() == 0);
        ExecuteBatchBatcher_->HandleRequest(context);
    }
};

IServicePtr CreateBatchingChunkService(
    TCellId cellId,
    TBatchingChunkServiceConfigPtr serviceConfig,
    TMasterConnectionConfigPtr connectionConfig,
    IChannelFactoryPtr channelFactory)
{
    return New<TBatchingChunkService>(
        cellId,
        serviceConfig,
        connectionConfig,
        channelFactory);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClusterNode
