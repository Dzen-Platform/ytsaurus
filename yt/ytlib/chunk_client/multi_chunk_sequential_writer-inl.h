#ifndef MULTI_CHUNK_SEQUENTIAL_WRITER_INL_H_
#error "Direct inclusion of this file is not allowed, include multi_chunk_sequential_writer.h"
#endif
#undef MULTI_CHUNK_SEQUENTIAL_WRITER_INL_H_

#include "chunk_writer.h"
#include "chunk_list_ypath_proxy.h"
#include "chunk_ypath_proxy.h"
#include "dispatcher.h"
#include "chunk_ypath_proxy.h"
#include "helpers.h"
#include "erasure_writer.h"
#include "private.h"
#include "replication_writer.h"

#include <core/erasure/codec.h>

#include <core/misc/string.h>
#include <core/misc/address.h>
#include <core/misc/protobuf_helpers.h>

#include <core/ytree/yson_serializable.h>

#include <core/rpc/helpers.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/object_client/helpers.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

template <class TProvider>
TOldMultiChunkSequentialWriter<TProvider>::TOldMultiChunkSequentialWriter(
    TMultiChunkWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    TProviderPtr provider,
    NRpc::IChannelPtr masterChannel,
    const NTransactionClient::TTransactionId& transactionId,
    const TChunkListId& parentChunkListId)
    : Options(options)
    , MasterChannel(masterChannel)
    , TransactionId(transactionId)
    , ParentChunkListId(parentChunkListId)
    , NodeDirectory(New<NNodeTrackerClient::TNodeDirectory>())
    , Provider(provider)
    , Progress(0)
    , CompleteChunkSize(0)
    , CloseChunksAwaiter(New<NConcurrency::TParallelAwaiter>(TDispatcher::Get()->GetWriterInvoker()))
    , Logger(ChunkClientLogger)
{
    YCHECK(config);
    YCHECK(masterChannel);

    // Patch UploadReplicationFactor with respect to options.
    Config = NYTree::CloneYsonSerializable(config);
    Config->UploadReplicationFactor = std::min(Options->ReplicationFactor, Config->UploadReplicationFactor);

    Logger.AddTag("TransactionId: %v", TransactionId);
}

template <class TProvider>
TFuture<void> TOldMultiChunkSequentialWriter<TProvider>::Open()
{
    YCHECK(!State.HasRunningOperation());

    CreateNextSession();

    State.StartOperation();
    NextSession.ToFuture().Subscribe(BIND(
        &TOldMultiChunkSequentialWriter::InitCurrentSession,
        MakeWeak(this)));

    return State.GetOperationError();
}

template <class TProvider>
auto TOldMultiChunkSequentialWriter<TProvider>::GetCurrentWriter() -> TFacade*
{
    if (!CurrentSession.ChunkWriter) {
        return nullptr;
    }

    if (CurrentSession.ChunkWriter->GetMetaSize() > Config->MaxMetaSize) {
        LOG_DEBUG("Switching to next chunk: meta is too large (ChunkMetaSize: %v)",
            CurrentSession.ChunkWriter->GetMetaSize());

        SwitchSession();
    } else if (CurrentSession.ChunkWriter->GetDataSize() > Config->DesiredChunkSize) {
        i64 currentDataSize = CompleteChunkSize + CurrentSession.ChunkWriter->GetDataSize();
        i64 expectedInputSize = static_cast<i64>(currentDataSize * std::max(0.0, 1.0 - Progress));

        if (expectedInputSize > Config->DesiredChunkSize ||
            CurrentSession.ChunkWriter->GetDataSize() > 2 * Config->DesiredChunkSize)
        {
            LOG_DEBUG("Switching to next chunk: data is too large (CurrentSessionSize: %v, ExpectedInputSize: %v, DesiredChunkSize: %v)",
                CurrentSession.ChunkWriter->GetDataSize(),
                expectedInputSize,
                Config->DesiredChunkSize);

            SwitchSession();
        }
    }

    // If we switched session we should check that new session is ready.
    return CurrentSession.ChunkWriter
        ? CurrentSession.ChunkWriter->GetFacade()
        : nullptr;
}

template <class TProvider>
TFuture<void> TOldMultiChunkSequentialWriter<TProvider>::GetReadyEvent()
{
    if (CurrentSession.ChunkWriter) {
        return CurrentSession.ChunkWriter->GetReadyEvent();
    } else {
        return State.GetOperationError();
    }
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::CreateNextSession()
{
    YCHECK(!NextSession);

    NextSession = NewPromise<TSession>();

    auto chunkType = (Options->ErasureCodec == NErasure::ECodec::None)
        ? NObjectClient::EObjectType::Chunk
        : NObjectClient::EObjectType::ErasureChunk;

    CreateChunk(MasterChannel, Config, Options, chunkType, TransactionId)
        .Subscribe(BIND(
            &TOldMultiChunkSequentialWriter<TProvider>::OnChunkCreated,
            MakeWeak(this))
        .Via(TDispatcher::Get()->GetWriterInvoker()));
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::OnChunkCreated(
    const NObjectClient::TMasterYPathProxy::TErrorOrRspCreateObjectsPtr& rspOrError)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YCHECK(NextSession);

    if (!State.IsActive()) {
        return;
    }

    try {
        THROW_ERROR_EXCEPTION_IF_FAILED(
            rspOrError,
            EErrorCode::MasterCommunicationFailed, 
            "Error creating chunk");
        const auto& rsp = rspOrError.Value();

        TSession session;
        session.ChunkId = NYT::FromProto<TChunkId>(rsp->object_ids(0));
        LOG_DEBUG("Chunk created (ChunkId: %v)", session.ChunkId);

        auto erasureCodecId = Options->ErasureCodec;
        if (erasureCodecId == NErasure::ECodec::None) {
            session.AsyncWriter = CreateReplicationWriter(
                Config,
                session.ChunkId,
                TChunkReplicaList(),
                NodeDirectory,
                MasterChannel);
        } else {
            auto* erasureCodec = NErasure::GetCodec(erasureCodecId);

            auto writers = CreateErasurePartWriters(
                Config,
                session.ChunkId,
                erasureCodec,
                NodeDirectory,
                MasterChannel,
                EWriteSessionType::User);

            session.AsyncWriter = CreateErasureWriter(
                Config,
                erasureCodec,
                writers);
        }

        auto error = NConcurrency::WaitFor(session.AsyncWriter->Open());
        THROW_ERROR_EXCEPTION_IF_FAILED(error)

        NextSession.Set(session);
    } catch (const std::exception& ex) {
        auto error = TError("Failed to start next session") << ex;
        LOG_WARNING(error);
        State.Fail(error);
    }
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::SetProgress(double progress)
{
    Progress = progress;
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::InitCurrentSession(const TErrorOr<TSession>& nextSessionOrError)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!nextSessionOrError.IsOK()) {
        State.Fail(nextSessionOrError);
        return;
    }

    auto nextSession = nextSessionOrError.Value();
    nextSession.ChunkWriter = Provider->CreateChunkWriter(nextSession.AsyncWriter);
    CurrentSession = nextSession;

    NextSession.Reset();
    CreateNextSession();

    State.FinishOperation();
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::SwitchSession()
{
    State.StartOperation();
    YCHECK(NextSession);

    auto this_ = MakeStrong(this);
    auto startNextSession = [this, this_] (const TError& error) {
        if (!error.IsOK())
            return;

        NextSession.ToFuture().Subscribe(BIND(
            &TOldMultiChunkSequentialWriter::InitCurrentSession,
            MakeWeak(this)));
    };

    auto result = FinishCurrentSession();
    if (Config->SyncChunkSwitch) {
        // Wait and block writing, until previous chunks has been completely closed.
        // This prevents double memory accounting in scheduler memory usage estimates.
        result.Subscribe(BIND(startNextSession));
    } else {
        // Start writing next chunk asap.
        startNextSession(TError());
    }

}

template <class TProvider>
TFuture<void> TOldMultiChunkSequentialWriter<TProvider>::FinishCurrentSession()
{
    if (CurrentSession.IsNull()) {
        return MakePromise(TError());
    }

    auto finishResult = NewPromise<void>();
    if (CurrentSession.ChunkWriter->GetDataSize() > 0) {
        LOG_DEBUG("Finishing chunk (ChunkId: %v)",
            CurrentSession.ChunkId);

        Provider->OnChunkFinished();

        NChunkClient::NProto::TChunkSpec chunkSpec;
        ToProto(chunkSpec.mutable_chunk_id(), CurrentSession.ChunkId);

        int chunkIndex = -1;
        {
            TGuard<TSpinLock> guard(WrittenChunksGuard);
            chunkIndex = WrittenChunks.size();
            WrittenChunks.push_back(chunkSpec);
        }

        CloseChunksAwaiter->Await(finishResult.ToFuture(), BIND(
            &TOldMultiChunkSequentialWriter::OnChunkFinished,
            MakeWeak(this),
            CurrentSession.ChunkId));

        CurrentSession.ChunkWriter->Close().Subscribe(BIND(
            &TOldMultiChunkSequentialWriter::OnChunkClosed,
            MakeWeak(this),
            chunkIndex,
            CurrentSession,
            finishResult));

    } else {
        LOG_DEBUG("Canceling empty chunk (ChunkId: %v)",
            CurrentSession.ChunkId);
        finishResult.Set(TError());
    }

    CurrentSession.Reset();
    return finishResult;
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::OnChunkClosed(
    int chunkIndex,
    TSession currentSession,
    TPromise<void> finishResult,
    const TError& error)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!error.IsOK()) {
        auto wrappedError = TError("Error closing chunk") << error;
        finishResult.Set(wrappedError);
        return;
    }

    auto asyncWriter = currentSession.AsyncWriter;
    auto chunkWriter = currentSession.ChunkWriter;

    CompleteChunkSize += chunkWriter->GetDataSize();

    Provider->OnChunkClosed(chunkWriter);

    LOG_DEBUG("Chunk closed (ChunkId: %v)",
        currentSession.ChunkId);

    auto replicas = asyncWriter->GetWrittenChunkReplicas();

    NObjectClient::TObjectServiceProxy objectProxy(MasterChannel);
    auto batchReq = objectProxy.ExecuteBatch();
    {
        auto req = TChunkYPathProxy::Confirm(
            NObjectClient::FromObjectId(currentSession.ChunkId));
        NRpc::GenerateMutationId(req);
        *req->mutable_chunk_info() = asyncWriter->GetChunkInfo();
        NYT::ToProto(req->mutable_replicas(), replicas);
        *req->mutable_chunk_meta() = chunkWriter->GetMasterMeta();

        batchReq->AddRequest(req);
    }
    {
        // Initialize the entry earlier prepared in FinishCurrentSession.
        TGuard<TSpinLock> guard(WrittenChunksGuard);
        auto& chunkSpec = WrittenChunks[chunkIndex];
        NYT::ToProto(chunkSpec.mutable_chunk_id(), currentSession.ChunkId);
        NYT::ToProto(chunkSpec.mutable_replicas(), replicas);
        *chunkSpec.mutable_chunk_meta() = chunkWriter->GetSchedulerMeta();
    }

    batchReq->Invoke().Subscribe(BIND(
        &TOldMultiChunkSequentialWriter::OnChunkConfirmed,
        MakeWeak(this),
        currentSession.ChunkId,
        finishResult));
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::OnChunkConfirmed(
    TChunkId chunkId,
    TPromise<void> finishResult,
    const NObjectClient::TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto error = NObjectClient::GetCumulativeError(batchRspOrError);
    if (!error.IsOK()) {
        auto wrappedError = TError(
            EErrorCode::MasterCommunicationFailed,
            "Error confirming chunk %v",
            chunkId) << error;

        finishResult.Set(wrappedError);
        return;
    }

    LOG_DEBUG("Chunk confirmed (ChunkId: %v)",
        chunkId);

    finishResult.Set(TError());
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::OnChunkFinished(
    TChunkId chunkId,
    const TError& error)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!error.IsOK()) {
        State.Fail(error);
        return;
    }

    LOG_DEBUG("Chunk successfully closed and registered (ChunkId: %v)",
        chunkId);
}

template <class TProvider>
TFuture<void> TOldMultiChunkSequentialWriter<TProvider>::Close()
{
    if (State.IsActive()) {
        State.StartOperation();
        FinishCurrentSession();

        CloseChunksAwaiter->Complete(BIND(
            &TOldMultiChunkSequentialWriter::AttachChunks,
            MakeWeak(this)));
    }

    return State.GetOperationError();
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::AttachChunks()
{
    if (!State.IsActive()) {
        return;
    }

    if (ParentChunkListId == NullChunkListId) {
        LOG_DEBUG("Chunk sequence writer closed, no chunks attached");

        State.Close();
        State.FinishOperation();
        return;
    }

    NObjectClient::TObjectServiceProxy objectProxy(MasterChannel);
    auto batchReq = objectProxy.ExecuteBatch();

    for (const auto& chunkSpec : WrittenChunks) {
        auto req = TChunkListYPathProxy::Attach(
            NObjectClient::FromObjectId(ParentChunkListId));
        *req->add_children_ids() = chunkSpec.chunk_id();
        NRpc::GenerateMutationId(req);
        batchReq->AddRequest(req);
    }

    batchReq->Invoke().Subscribe(BIND(
        &TOldMultiChunkSequentialWriter::OnClose,
        MakeWeak(this)));
}

template <class TProvider>
void TOldMultiChunkSequentialWriter<TProvider>::OnClose(
    const NObjectClient::TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
{
    if (!State.IsActive()) {
        return;
    }

    auto error = NObjectClient::GetCumulativeError(batchRspOrError);
    if (!error.IsOK()) {
        auto wrappedError = TError(
            EErrorCode::MasterCommunicationFailed,
            "Error attaching chunks to chunk list %v",
            ParentChunkListId)
            << error;
        State.Fail(wrappedError);
        return;
    }

    LOG_DEBUG("Chunk sequence writer closed");

    State.Close();
    State.FinishOperation();
}

template <class TProvider>
const std::vector<NChunkClient::NProto::TChunkSpec>& TOldMultiChunkSequentialWriter<TProvider>::GetWrittenChunks() const
{
    return WrittenChunks;
}

template <class TProvider>
NNodeTrackerClient::TNodeDirectoryPtr TOldMultiChunkSequentialWriter<TProvider>::GetNodeDirectory() const
{
    return NodeDirectory;
}

template <class TProvider>
auto TOldMultiChunkSequentialWriter<TProvider>::GetProvider() -> TProviderPtr
{
    return Provider;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
