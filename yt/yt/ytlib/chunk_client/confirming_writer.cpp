#include "confirming_writer.h"

#include "block.h"
#include "chunk_meta_extensions.h"
#include "chunk_service_proxy.h"
#include "config.h"
#include "deferred_chunk_meta.h"
#include "dispatcher.h"
#include "erasure_part_writer.h"
#include "erasure_writer.h"
#include "helpers.h"
#include "private.h"
#include "replication_writer.h"
#include "session_id.h"

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/yt/core/concurrency/scheduler.h>

#include <yt/yt/library/erasure/impl/codec.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/misc/finally.h>

#include <yt/yt/core/ytree/yson_serializable.h>

namespace NYT::NChunkClient {

using namespace NApi;
using namespace NRpc;
using namespace NObjectClient;
using namespace NErasure;
using namespace NConcurrency;
using namespace NYTree;
using namespace NTableClient;
using namespace NNodeTrackerClient;

using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

class TConfirmingWriter
    : public IChunkWriter
{
public:
    TConfirmingWriter(
        TMultiChunkWriterConfigPtr config,
        TMultiChunkWriterOptionsPtr options,
        TCellTag cellTag,
        TTransactionId transactionId,
        TChunkListId parentChunkListId,
        TNodeDirectoryPtr nodeDirectory,
        NNative::IClientPtr client,
        IBlockCachePtr blockCache,
        IThroughputThrottlerPtr throttler,
        TTrafficMeterPtr trafficMeter,
        TSessionId sessionId)
        : Config_(CloneYsonSerializable(config))
        , Options_(options)
        , CellTag_(cellTag)
        , TransactionId_(transactionId)
        , ParentChunkListId_(parentChunkListId)
        , NodeDirectory_(nodeDirectory)
        , Client_(client)
        , BlockCache_(blockCache)
        , Throttler_(throttler)
        , TrafficMeter_(trafficMeter)
        , SessionId_(sessionId)
        , Logger(ChunkClientLogger.WithTag("TransactionId: %v", TransactionId_))
    {
        Config_->UploadReplicationFactor = std::min(
            Config_->UploadReplicationFactor,
            Options_->ReplicationFactor);
        Config_->MinUploadReplicationFactor = std::min(
            Config_->MinUploadReplicationFactor,
            Options_->ReplicationFactor);
    }


    virtual TFuture<void> Open() override
    {
        YT_VERIFY(!Initialized_);
        YT_VERIFY(!OpenFuture_);

        OpenFuture_ = BIND(&TConfirmingWriter::OpenSession, MakeWeak(this))
            .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
            .Run();
        return OpenFuture_;
    }

    virtual bool WriteBlock(const TBlock& block) override
    {
        return WriteBlocks({block});
    }

    virtual bool WriteBlocks(const std::vector<TBlock>& blocks) override
    {
        YT_VERIFY(Initialized_);
        YT_VERIFY(OpenFuture_.IsSet());

        if (!OpenFuture_.Get().IsOK()) {
            return false;
        } else {
            return UnderlyingWriter_->WriteBlocks(blocks);
        }
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        YT_VERIFY(Initialized_);
        YT_VERIFY(OpenFuture_.IsSet());
        if (!OpenFuture_.Get().IsOK()) {
            return OpenFuture_;
        } else {
            return UnderlyingWriter_->GetReadyEvent();
        }
    }

    virtual TFuture<void> Close(const TDeferredChunkMetaPtr& chunkMeta) override
    {
        YT_VERIFY(Initialized_);
        YT_VERIFY(OpenFuture_.IsSet());

        ChunkMeta_ = chunkMeta;

        return BIND(
            &TConfirmingWriter::DoClose,
            MakeWeak(this))
            .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

    virtual const NProto::TChunkInfo& GetChunkInfo() const override
    {
        YT_VERIFY(Closed_);
        return UnderlyingWriter_->GetChunkInfo();
    }

    virtual const NProto::TDataStatistics& GetDataStatistics() const override
    {
        YT_VERIFY(Closed_);
        return DataStatistics_;
    }

    virtual TChunkReplicaWithMediumList GetWrittenChunkReplicas() const override
    {
        YT_VERIFY(UnderlyingWriter_);
        return UnderlyingWriter_->GetWrittenChunkReplicas();
    }

    virtual TChunkId GetChunkId() const override
    {
        return SessionId_.ChunkId;
    }

    virtual NErasure::ECodec GetErasureCodecId() const override
    {
        return Options_->ErasureCodec;
    }

    virtual bool IsCloseDemanded() const override
    {
        if (UnderlyingWriter_) {
            return UnderlyingWriter_->IsCloseDemanded();
        } else {
            return false;
        }
    }

private:
    const TMultiChunkWriterConfigPtr Config_;
    const TMultiChunkWriterOptionsPtr Options_;
    const TCellTag CellTag_;
    const TTransactionId TransactionId_;
    const TChunkListId ParentChunkListId_;
    const TNodeDirectoryPtr NodeDirectory_;
    const NNative::IClientPtr Client_;
    const IBlockCachePtr BlockCache_;
    const IThroughputThrottlerPtr Throttler_;
    const TTrafficMeterPtr TrafficMeter_;

    IChunkWriterPtr UnderlyingWriter_;

    std::atomic<bool> Initialized_ = {false};
    std::atomic<bool> Closed_ = {false};
    TSessionId SessionId_;
    TFuture<void> OpenFuture_;

    TDeferredChunkMetaPtr ChunkMeta_;
    NProto::TDataStatistics DataStatistics_;

    NLogging::TLogger Logger;

    void OpenSession()
    {
        auto finally = Finally([&] () {
            Initialized_ = true;
        });

        if (SessionId_.ChunkId) {
            YT_LOG_DEBUG("Writing existing chunk (ChunkId: %v)", SessionId_.ChunkId);
        } else {
            SessionId_ = NChunkClient::CreateChunk(
                Client_,
                CellTag_,
                Options_,
                TransactionId_,
                ParentChunkListId_,
                Logger);
            YT_LOG_DEBUG("Chunk created");
        }

        Logger.AddTag("ChunkId: %v", SessionId_);

        UnderlyingWriter_ = CreateUnderlyingWriter();
        WaitFor(UnderlyingWriter_->Open())
            .ThrowOnError();

        YT_LOG_DEBUG("Chunk writer opened");
    }

    IChunkWriterPtr CreateUnderlyingWriter() const
    {
        if (Options_->ErasureCodec == ECodec::None) {
            return CreateReplicationWriter(
                Config_,
                Options_,
                SessionId_,
                TChunkReplicaWithMediumList(),
                NodeDirectory_,
                Client_,
                BlockCache_,
                TrafficMeter_,
                Throttler_);
        }

        auto* erasureCodec = GetCodec(Options_->ErasureCodec);
        // NB(psushin): we don't ask master for new erasure replicas,
        // because we cannot guarantee proper replica placement.
        auto options = CloneYsonSerializable(Options_);
        options->AllowAllocatingNewTargetNodes = Config_->EnableErasureTargetNodeReallocation;
        auto config = CloneYsonSerializable(Config_);
        // Block reordering is done in erasure writer.
        config->EnableBlockReordering = false;
        auto writers = CreateAllErasurePartWriters(
            config,
            options,
            SessionId_,
            erasureCodec,
            NodeDirectory_,
            Client_,
            TrafficMeter_,
            Throttler_,
            BlockCache_);
        return CreateErasureWriter(
            Config_,
            SessionId_,
            Options_->ErasureCodec,
            erasureCodec,
            writers,
            Config_->WorkloadDescriptor);
    }

    void DoClose()
    {
        auto error = WaitFor(UnderlyingWriter_->Close(ChunkMeta_));

        THROW_ERROR_EXCEPTION_IF_FAILED(
            error,
            "Failed to close chunk %v",
            SessionId_.ChunkId);

        YT_LOG_DEBUG("Chunk closed");

        auto replicas = UnderlyingWriter_->GetWrittenChunkReplicas();
        YT_VERIFY(!replicas.empty());

        static const THashSet<int> masterMetaTags{
            TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value,
            TProtoExtensionTag<NTableClient::NProto::TBoundaryKeysExt>::Value,
            TProtoExtensionTag<NTableClient::NProto::THeavyColumnStatisticsExt>::Value
        };

        // Underlying writer should have called ChunkMeta_->Finalize().
        YT_VERIFY(ChunkMeta_->IsFinalized());

        NChunkClient::NProto::TChunkMeta masterChunkMeta(*ChunkMeta_);
        FilterProtoExtensions(
            masterChunkMeta.mutable_extensions(),
            ChunkMeta_->extensions(),
            masterMetaTags);

        // Sanity check.
        YT_VERIFY(FindProtoExtension<NChunkClient::NProto::TMiscExt>(masterChunkMeta.extensions()));

        auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, CellTag_);
        TChunkServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();
        GenerateMutationId(batchReq);
        batchReq->set_suppress_upstream_sync(true);

        auto* req = batchReq->add_confirm_chunk_subrequests();
        ToProto(req->mutable_chunk_id(), SessionId_.ChunkId);
        *req->mutable_chunk_info() = UnderlyingWriter_->GetChunkInfo();
        req->mutable_chunk_meta()->Swap(&masterChunkMeta);
        req->set_request_statistics(true);
        ToProto(req->mutable_replicas(), replicas);

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(
            GetCumulativeError(batchRspOrError),
            EErrorCode::MasterCommunicationFailed,
            "Failed to confirm chunk %v",
            SessionId_.ChunkId);

        const auto& batchRsp = batchRspOrError.Value();
        const auto& rsp = batchRsp->confirm_chunk_subresponses(0);
        DataStatistics_ = rsp.statistics();

        Closed_ = true;

        YT_LOG_DEBUG("Chunk confirmed");
    }
};

////////////////////////////////////////////////////////////////////////////////

IChunkWriterPtr CreateConfirmingWriter(
    TMultiChunkWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    TCellTag cellTag,
    TTransactionId transactionId,
    TChunkListId parentChunkListId,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    NNative::IClientPtr client,
    IBlockCachePtr blockCache,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr throttler,
    TSessionId sessionId)
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
        throttler,
        trafficMeter,
        sessionId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
