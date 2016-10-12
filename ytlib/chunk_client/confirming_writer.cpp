#include "confirming_writer.h"
#include "private.h"
#include "chunk_meta_extensions.h"
#include "chunk_replica.h"
#include "config.h"
#include "dispatcher.h"
#include "erasure_writer.h"
#include "helpers.h"
#include "replication_writer.h"
#include "chunk_service_proxy.h"
#include "helpers.h"

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/connection.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/finally.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NChunkClient {

using namespace NApi;
using namespace NChunkClient::NProto;
using namespace NRpc;
using namespace NObjectClient;
using namespace NErasure;
using namespace NConcurrency;
using namespace NYTree;
using namespace NTableClient;
using namespace NNodeTrackerClient;

using NYT::ToProto;

///////////////////////////////////////////////////////////////////////////////

class TConfirmingWriter
    : public IChunkWriter
{
public:
    TConfirmingWriter(
        TMultiChunkWriterConfigPtr config,
        TMultiChunkWriterOptionsPtr options,
        TCellTag cellTag,
        const TTransactionId& transactionId,
        const TChunkListId& parentChunkListId,
        TNodeDirectoryPtr nodeDirectory,
        IClientPtr client,
        IBlockCachePtr blockCache,
        IThroughputThrottlerPtr throttler);

    virtual TFuture<void> Open() override;
    virtual bool WriteBlock(const TSharedRef& block) override;
    virtual bool WriteBlocks(const std::vector<TSharedRef>& blocks) override;

    virtual TFuture<void> GetReadyEvent() override;

    virtual TFuture<void> Close(const TChunkMeta& chunkMeta) override;

    virtual const TChunkInfo& GetChunkInfo() const override;
    virtual const TDataStatistics& GetDataStatistics() const override;
    
    virtual TChunkReplicaList GetWrittenChunkReplicas() const override;

    virtual TChunkId GetChunkId() const override;

    virtual NErasure::ECodec GetErasureCodecId() const override;

private:
    const TMultiChunkWriterConfigPtr Config_;
    const TMultiChunkWriterOptionsPtr Options_;
    const TCellTag CellTag_;
    const TTransactionId TransactionId_;
    const TChunkListId ParentChunkListId_;
    const TNodeDirectoryPtr NodeDirectory_;
    const IClientPtr Client_;
    const IBlockCachePtr BlockCache_;
    const IThroughputThrottlerPtr Throttler_;

    IChunkWriterPtr UnderlyingWriter_;

    std::atomic<bool> Initialized_ = { false };
    std::atomic<bool> Closed_ = { false };
    TChunkId ChunkId_ = NullChunkId;
    TFuture<void> OpenFuture_;

    TChunkMeta ChunkMeta_;
    TDataStatistics DataStatistics_;

    NLogging::TLogger Logger;

    void OpenSession();
    TChunkId CreateChunk() const;
    IChunkWriterPtr CreateUnderlyingWriter() const;
    void DoClose();
};

///////////////////////////////////////////////////////////////////////////////

TConfirmingWriter::TConfirmingWriter(
    TMultiChunkWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    TCellTag cellTag,
    const TTransactionId& transactionId,
    const TChunkListId& parentChunkListId,
    TNodeDirectoryPtr nodeDirectory,
    IClientPtr client,
    IBlockCachePtr blockCache,
    IThroughputThrottlerPtr throttler)
    : Config_(config)
    , Options_(options)
    , CellTag_(cellTag)
    , TransactionId_(transactionId)
    , ParentChunkListId_(parentChunkListId)
    , NodeDirectory_(nodeDirectory)
    , Client_(client)
    , BlockCache_(blockCache)
    , Throttler_(throttler)
    , Logger(ChunkClientLogger)
{
    Logger.AddTag("TransactionId: %v", TransactionId_);
}

TFuture<void> TConfirmingWriter::Open()
{
    YCHECK(!Initialized_);
    YCHECK(!OpenFuture_);

    OpenFuture_ = BIND(&TConfirmingWriter::OpenSession, MakeWeak(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();
    return OpenFuture_;
}

bool TConfirmingWriter::WriteBlock(const TSharedRef& block)
{
    return WriteBlocks(std::vector<TSharedRef>(1, block));
}

bool TConfirmingWriter::WriteBlocks(const std::vector<TSharedRef>& blocks)
{
    YCHECK(Initialized_);
    YCHECK(OpenFuture_.IsSet());

    if (!OpenFuture_.Get().IsOK()) {
        return false;
    } else {
        return UnderlyingWriter_->WriteBlocks(blocks);
    }
}

TFuture<void> TConfirmingWriter::GetReadyEvent()
{
    YCHECK(Initialized_);
    YCHECK(OpenFuture_.IsSet());
    if (!OpenFuture_.Get().IsOK()) {
        return OpenFuture_;
    } else {
        return UnderlyingWriter_->GetReadyEvent();
    }
}

TFuture<void> TConfirmingWriter::Close(const TChunkMeta& chunkMeta)
{
    YCHECK(Initialized_);
    YCHECK(OpenFuture_.IsSet());

    ChunkMeta_ = chunkMeta;

    return BIND(
        &TConfirmingWriter::DoClose,
        MakeWeak(this))
    .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
    .Run();
}

const TChunkInfo& TConfirmingWriter::GetChunkInfo() const
{
    YCHECK(Closed_);
    return UnderlyingWriter_->GetChunkInfo();
}

const TDataStatistics& TConfirmingWriter::GetDataStatistics() const
{
    YCHECK(Closed_);
    return DataStatistics_;
}

TChunkReplicaList TConfirmingWriter::GetWrittenChunkReplicas() const
{
    YCHECK(UnderlyingWriter_);
    return UnderlyingWriter_->GetWrittenChunkReplicas();
}

TChunkId TConfirmingWriter::GetChunkId() const
{
    return ChunkId_;
}

NErasure::ECodec TConfirmingWriter::GetErasureCodecId() const
{
    return Options_->ErasureCodec;
}

void TConfirmingWriter::OpenSession()
{
    auto finally = Finally([&] () {
        Initialized_ = true;
    });

    ChunkId_ = CreateChunk();

    Logger.AddTag("ChunkId: %v", ChunkId_);
    LOG_DEBUG("Chunk created");

    UnderlyingWriter_ = CreateUnderlyingWriter();
    WaitFor(UnderlyingWriter_->Open())
        .ThrowOnError();

    LOG_DEBUG("Chunk writer opened");
}

TChunkId TConfirmingWriter::CreateChunk() const
{
    try {
        return NChunkClient::CreateChunk(
            Client_,
            CellTag_,
            Options_,
            TransactionId_,
            ParentChunkListId_,
            Logger);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION(
            EErrorCode::MasterCommunicationFailed,
            "Error creating chunk")
            << ex;
    }
}

IChunkWriterPtr TConfirmingWriter::CreateUnderlyingWriter() const
{
    if (Options_->ErasureCodec == ECodec::None) {
        return CreateReplicationWriter(
            Config_,
            Options_,
            ChunkId_,
            TChunkReplicaList(),
            NodeDirectory_,
            Client_,
            BlockCache_,
            Throttler_);
    }

    auto* erasureCodec = GetCodec(Options_->ErasureCodec);
    // NB(psushin): we don't ask master for new erasure replicas,
    // because we cannot guarantee proper replica placement.
    auto options = CloneYsonSerializable(Options_);
    options->AllowAllocatingNewTargetNodes = false;
    auto writers = CreateErasurePartWriters(
        Config_,
        options,
        ChunkId_,
        erasureCodec,
        NodeDirectory_,
        Client_,
        Throttler_,
        BlockCache_);
    return CreateErasureWriter(
        Config_,
        ChunkId_,
        Options_->ErasureCodec,
        erasureCodec,
        writers);
}

void TConfirmingWriter::DoClose()
{
    auto error = WaitFor(UnderlyingWriter_->Close(ChunkMeta_));

    THROW_ERROR_EXCEPTION_IF_FAILED(
        error,
        "Failed to close chunk %v",
        ChunkId_);

    LOG_DEBUG("Chunk closed");

    auto replicas = UnderlyingWriter_->GetWrittenChunkReplicas();
    YCHECK(!replicas.empty());

    static const yhash_set<int> masterMetaTags{
        TProtoExtensionTag<TMiscExt>::Value,
        TProtoExtensionTag<NTableClient::NProto::TBoundaryKeysExt>::Value,
        TProtoExtensionTag<NTableClient::NProto::TOldBoundaryKeysExt>::Value
    };

    auto masterChunkMeta = ChunkMeta_;
    FilterProtoExtensions(
        masterChunkMeta.mutable_extensions(),
        ChunkMeta_.extensions(),
        masterMetaTags);

    // Sanity check.
    YCHECK(FindProtoExtension<TMiscExt>(masterChunkMeta.extensions()));

    auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, CellTag_);
    TChunkServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();
    GenerateMutationId(batchReq);
    batchReq->set_suppress_upstream_sync(true);

    auto* req = batchReq->add_confirm_chunk_subrequests();
    ToProto(req->mutable_chunk_id(), ChunkId_);
    *req->mutable_chunk_info() = UnderlyingWriter_->GetChunkInfo();
    *req->mutable_chunk_meta() = masterChunkMeta;
    req->set_request_statistics(true);
    ToProto(req->mutable_replicas(), replicas);

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        GetCumulativeError(batchRspOrError),
        EErrorCode::MasterCommunicationFailed,
        "Failed to confirm chunk %v",
        ChunkId_);

    const auto& batchRsp = batchRspOrError.Value();
    const auto& rsp = batchRsp->confirm_chunk_subresponses(0);
    DataStatistics_ = rsp.statistics();

    Closed_ = true;

    LOG_DEBUG("Chunk confirmed");
}

///////////////////////////////////////////////////////////////////////////////

IChunkWriterPtr CreateConfirmingWriter(
    TMultiChunkWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    TCellTag cellTag,
    const TTransactionId& transactionId,
    const TChunkListId& parentChunkListId,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    IClientPtr client,
    IBlockCachePtr blockCache,
    IThroughputThrottlerPtr throttler)
{
    return New<TConfirmingWriter>(
        config,
        options,
        cellTag,
        transactionId,
        parentChunkListId,
        nodeDirectory,
        client,
        blockCache,
        throttler);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
