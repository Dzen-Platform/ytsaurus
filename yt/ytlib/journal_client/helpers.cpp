#include "stdafx.h"
#include "helpers.h"
#include "private.h"

#include <core/misc/string.h>

#include <core/concurrency/parallel_awaiter.h>

#include <core/logging/log.h>

#include <ytlib/chunk_client/private.h>
#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/data_node_service_proxy.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <atomic>

namespace NYT {
namespace NJournalClient {

using namespace NConcurrency;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

class TAbortSessionsQuorumSession
    : public TRefCounted
{
public:
    TAbortSessionsQuorumSession(
        const TChunkId& chunkId,
        const std::vector<TNodeDescriptor>& replicas,
        TDuration timeout,
        int quorum)
        : ChunkId_(chunkId)
        , Replicas_(replicas)
        , Timeout_(timeout)
        , Quorum_(quorum)
        , Logger(JournalClientLogger)
    { }

    TFuture<void> Run()
    {
        BIND(&TAbortSessionsQuorumSession::DoRun, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
            .Run();
        return Promise_;
    }

private:
    const TChunkId ChunkId_;
    const std::vector<TNodeDescriptor> Replicas_;
    const TDuration Timeout_;
    const int Quorum_;

    int SuccessCounter_ = 0;
    int ResponseCounter_ = 0;

    std::vector<TError> InnerErrors_;

    TPromise<void> Promise_ = NewPromise<void>();

    NLogging::TLogger Logger;


    void DoRun()
    {
        LOG_INFO("Aborting journal chunk session quroum (ChunkId: %v, Addresses: [%v])",
            ChunkId_,
            JoinToString(Replicas_));

        if (Replicas_.size() < Quorum_) {
            auto error = TError("Unable to abort sessions quorum for journal chunk %v: too few replicas known, %v given, %v needed",
                ChunkId_,
                Replicas_.size(),
                Quorum_);
            Promise_.Set(error);
            return;
        }

        for (const auto& descriptor : Replicas_) {
            auto channel = LightNodeChannelFactory->CreateChannel(descriptor.GetInterconnectAddress());
            TDataNodeServiceProxy proxy(channel);
            proxy.SetDefaultTimeout(Timeout_);
            auto req = proxy.FinishChunk();
            ToProto(req->mutable_chunk_id(), ChunkId_);
            req->Invoke().Subscribe(BIND(&TAbortSessionsQuorumSession::OnResponse, MakeStrong(this), descriptor)
                .Via(GetCurrentInvoker()));
        }
    }

    void OnResponse(
        const TNodeDescriptor& descriptor,
        const TDataNodeServiceProxy::TErrorOrRspFinishChunkPtr& rspOrError)
    {
        ++ResponseCounter_;
        // NB: Missing session is also OK.
        if (rspOrError.IsOK() || rspOrError.GetCode() == NChunkClient::EErrorCode::NoSuchSession) {
            ++SuccessCounter_;
            LOG_INFO("Journal chunk session aborted successfully (ChunkId: %v, Address: %v)",
                ChunkId_,
                descriptor.GetDefaultAddress());

        } else {
            InnerErrors_.push_back(rspOrError);
            LOG_WARNING(rspOrError, "Failed to abort journal chunk session (ChunkId: %v, Address: %v)",
                ChunkId_,
                descriptor.GetDefaultAddress());
        }

        if (SuccessCounter_ == Quorum_) {
            LOG_INFO("Journal chunk session quroum aborted successfully (ChunkId: %v)",
                ChunkId_);
            Promise_.TrySet();
        }
        
        if (ResponseCounter_ == Replicas_.size()) {
            auto combinedError = TError("Unable to abort sessions quorum for journal chunk %v",
                ChunkId_)
                << InnerErrors_;
            Promise_.TrySet(combinedError);
        }
    }

};

TFuture<void> AbortSessionsQuorum(
    const TChunkId& chunkId,
    const std::vector<TNodeDescriptor>& replicas,
    TDuration timeout,
    int quorum)
{
    return New<TAbortSessionsQuorumSession>(chunkId, replicas, timeout, quorum)
        ->Run();
}

////////////////////////////////////////////////////////////////////////////////

class TComputeQuorumRowCountSession
    : public TRefCounted
{
public:
    TComputeQuorumRowCountSession(
        const TChunkId& chunkId,
        const std::vector<TNodeDescriptor>& replicas,
        TDuration timeout,
        int quorum)
        : ChunkId_(chunkId)
        , Replicas_(replicas)
        , Timeout_(timeout)
        , Quorum_(quorum)
        , Logger(JournalClientLogger)
    { }

    TFuture<TMiscExt> Run()
    {
        BIND(&TComputeQuorumRowCountSession::DoRun, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
            .Run();
        return Promise_;
    }

private:
    const TChunkId ChunkId_;
    const std::vector<TNodeDescriptor> Replicas_;
    const TDuration Timeout_;
    const int Quorum_;

    std::vector<TMiscExt> Infos_;
    std::vector<TError> InnerErrors_;

    TPromise<TMiscExt> Promise_ = NewPromise<TMiscExt>();

    NLogging::TLogger Logger;


    void DoRun()
    {
        if (Replicas_.size() < Quorum_) {
            auto error = TError("Unable to compute quorum info for journal chunk %v: too few replicas known, %v given, %v needed",
                ChunkId_,
                Replicas_.size(),
                Quorum_);
            Promise_.Set(error);
            return;
        }

        LOG_INFO("Computing quorum info for journal chunk (ChunkId: %v, Addresses: [%v])",
            ChunkId_,
            JoinToString(Replicas_));

        auto awaiter = New<TParallelAwaiter>(GetCurrentInvoker());
        for (const auto& descriptor : Replicas_) {
            auto channel = LightNodeChannelFactory->CreateChannel(descriptor.GetInterconnectAddress());
            TDataNodeServiceProxy proxy(channel);
            proxy.SetDefaultTimeout(Timeout_);
            auto req = proxy.GetChunkMeta();
            ToProto(req->mutable_chunk_id(), ChunkId_);
            req->add_extension_tags(TProtoExtensionTag<TMiscExt>::Value);
            awaiter->Await(
                req->Invoke(),
                BIND(&TComputeQuorumRowCountSession::OnResponse, MakeStrong(this), descriptor));
        }

        awaiter->Complete(
            BIND(&TComputeQuorumRowCountSession::OnComplete, MakeStrong(this)));
    }

    void OnResponse(
        const TNodeDescriptor& descriptor,
        const TDataNodeServiceProxy::TErrorOrRspGetChunkMetaPtr& rspOrError)
    {
        if (rspOrError.IsOK()) {
            const auto& rsp = rspOrError.Value();
            auto miscExt = GetProtoExtension<TMiscExt>(rsp->chunk_meta().extensions());
            Infos_.push_back(miscExt);

            LOG_INFO("Received info for journal chunk (ChunkId: %v, Address: %v, RowCount: %v, UncompressedDataSize: %v, CompressedDataSize: %v)",
                ChunkId_,
                descriptor.GetDefaultAddress(),
                miscExt.row_count(),
                miscExt.uncompressed_data_size(),
                miscExt.compressed_data_size());
        } else {
            InnerErrors_.push_back(rspOrError);

            LOG_WARNING(rspOrError, "Failed to get journal info (ChunkId: %v, Address: %v)",
                ChunkId_,
                descriptor.GetDefaultAddress());
        }
    }

    void OnComplete()
    {
        if (Infos_.size() < Quorum_) {
            auto error = TError("Unable to compute quorum info for journal chunk %v: too few replicas alive, %v found, %v needed",
                ChunkId_,
                Infos_.size(),
                Quorum_)
                << InnerErrors_;
            Promise_.Set(error);
            return;
        }

        std::sort(
            Infos_.begin(),
            Infos_.end(),
            [] (const TMiscExt& lhs, const TMiscExt& rhs) {
                return lhs.row_count() < rhs.row_count();
            });

        const auto& quorumInfo = Infos_[Quorum_ - 1];

        LOG_INFO("Quorum info for journal chunk computed successfully (ChunkId: %v, RowCount: %v, UncompressedDataSize: %v, CompressedDataSize: %v)",
            ChunkId_,
            quorumInfo.row_count(),
            quorumInfo.uncompressed_data_size(),
            quorumInfo.compressed_data_size());

        Promise_.Set(quorumInfo);
    }

};

TFuture<TMiscExt> ComputeQuorumInfo(
    const TChunkId& chunkId,
    const std::vector<TNodeDescriptor>& replicas,
    TDuration timeout,
    int quorum)
{
    return New<TComputeQuorumRowCountSession>(chunkId, replicas, timeout, quorum)
        ->Run();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJournalClient
} // namespace NYT

