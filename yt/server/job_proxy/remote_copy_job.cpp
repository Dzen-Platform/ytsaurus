#include "remote_copy_job.h"
#include "private.h"
#include "config.h"
#include "job_detail.h"

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/config.h>
#include <yt/ytlib/api/native_connection.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/chunk_writer.h>
#include <yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/ytlib/chunk_client/data_statistics.h>
#include <yt/ytlib/chunk_client/erasure_reader.h>
#include <yt/ytlib/chunk_client/erasure_writer.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/replication_reader.h>
#include <yt/ytlib/chunk_client/replication_writer.h>
#include <yt/ytlib/chunk_client/chunk_service_proxy.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/core/erasure/codec.h>

namespace NYT {
namespace NJobProxy {

using namespace NRpc;
using namespace NYTree;
using namespace NYson;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NScheduler::NProto;
using namespace NScheduler;
using namespace NJobTrackerClient::NProto;
using namespace NTableClient;
using namespace NApi;
using namespace NErasure;

using NJobTrackerClient::TStatistics;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = JobProxyLogger;
static const auto& Profiler = JobProxyProfiler;

////////////////////////////////////////////////////////////////////////////////

class TRemoteCopyJob
    : public TJob
{
public:
    explicit TRemoteCopyJob(IJobHostPtr host)
        : TJob(host)
        , JobSpec_(host->GetJobSpec())
        , SchedulerJobSpecExt_(JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext))
        , RemoteCopyJobSpecExt_(JobSpec_.GetExtension(TRemoteCopyJobSpecExt::remote_copy_job_spec_ext))
        , ReaderConfig_(Host_->GetConfig()->JobIO->TableReader)
        , WriterConfig_(Host_->GetConfig()->JobIO->TableWriter)
    {
        YCHECK(SchedulerJobSpecExt_.input_specs_size() == 1);
        YCHECK(SchedulerJobSpecExt_.output_specs_size() == 1);

        for (const auto& inputChunkSpec : SchedulerJobSpecExt_.input_specs(0).chunks()) {
            YCHECK(!inputChunkSpec.has_lower_limit());
            YCHECK(!inputChunkSpec.has_upper_limit());
        }
    }

    virtual void Initialize() override
    {
        WriterOptionsTemplate_ = ConvertTo<TTableWriterOptionsPtr>(
            TYsonString(SchedulerJobSpecExt_.output_specs(0).table_writer_options()));
        OutputChunkListId_ = FromProto<TChunkListId>(
            SchedulerJobSpecExt_.output_specs(0).chunk_list_id());

        const auto& remoteCopySpec = JobSpec_.GetExtension(TRemoteCopyJobSpecExt::remote_copy_job_spec_ext);
        auto remoteConnectionConfig = ConvertTo<TNativeConnectionConfigPtr>(TYsonString(remoteCopySpec.connection_config()));
        RemoteConnection_ = CreateNativeConnection(remoteConnectionConfig);

        RemoteClient_ = RemoteConnection_->CreateNativeClient(TClientOptions(NSecurityClient::JobUserName));
    }

    virtual TJobResult Run() override
    {
        PROFILE_TIMING ("/remote_copy_time") {
            for (const auto& inputChunkSpec : SchedulerJobSpecExt_.input_specs(0).chunks()) {
                CopyChunk(inputChunkSpec);
            }
        }

        TJobResult result;
        ToProto(result.mutable_error(), TError());
        return result;
    }

    virtual void Abort() override
    { }

    virtual double GetProgress() const override
    {
        // Caution: progress calculated approximately (assuming all chunks have equal size).
        double chunkProgress = TotalChunkSize_ ? static_cast<double>(CopiedChunkSize_) / *TotalChunkSize_ : 0.0;
        return (CopiedChunkCount_ + chunkProgress) / SchedulerJobSpecExt_.input_specs(0).chunks_size();
    }

    virtual std::vector<TChunkId> GetFailedChunkIds() const override
    {
        return FailedChunkId_
            ? std::vector<TChunkId>(1, *FailedChunkId_)
            : std::vector<TChunkId>();
    }

    virtual TStatistics GetStatistics() const override
    {
        TStatistics result;
        result.AddSample("/data/input", DataStatistics_);
        result.AddSample(
            "/data/output/" + NYPath::ToYPathLiteral(0),
            DataStatistics_);
        return result;
    }

private:
    const TJobSpec& JobSpec_;
    const TSchedulerJobSpecExt& SchedulerJobSpecExt_;
    const TRemoteCopyJobSpecExt& RemoteCopyJobSpecExt_;
    const TTableReaderConfigPtr ReaderConfig_;
    const TTableWriterConfigPtr WriterConfig_;

    TTableWriterOptionsPtr WriterOptionsTemplate_;

    TChunkListId OutputChunkListId_;

    INativeConnectionPtr RemoteConnection_;
    INativeClientPtr RemoteClient_;

    int CopiedChunkCount_ = 0;
    i64 CopiedChunkSize_ = 0;
    TNullable<i64> TotalChunkSize_;

    TDataStatistics DataStatistics_;

    TNullable<TChunkId> FailedChunkId_;


    void CopyChunk(const TChunkSpec& inputChunkSpec)
    {
        CopiedChunkSize_ = 0;

        auto writerOptions = CloneYsonSerializable(WriterOptionsTemplate_);
        auto inputChunkId = NYT::FromProto<TChunkId>(inputChunkSpec.chunk_id());

        LOG_INFO("Copying input chunk (ChunkId: %v)",
            inputChunkId);

        auto erasureCodecId = NErasure::ECodec(inputChunkSpec.erasure_codec());
        writerOptions->ErasureCodec = erasureCodecId;

        auto inputReplicas = NYT::FromProto<TChunkReplicaList>(inputChunkSpec.replicas());
        auto transactionId = FromProto<TTransactionId>(SchedulerJobSpecExt_.output_transaction_id());
        LOG_INFO("Creating output chunk");

        // Create output chunk.
        TChunkId outputChunkId;
        try {
            outputChunkId = CreateChunk(
                Host_->GetClient(),
                CellTagFromId(OutputChunkListId_),
                writerOptions,
                transactionId,
                OutputChunkListId_,
                Logger);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION(
                NChunkClient::EErrorCode::ChunkCreationFailed,
                "Error creating chunk")
                << ex;
        }

        LOG_INFO("Output chunk created (ChunkId: %v)",
            outputChunkId);

        // Copy chunk.
        LOG_INFO("Copying chunk data");

        TChunkInfo chunkInfo;
        TChunkMeta chunkMeta;
        TChunkReplicaList writtenReplicas;

        if (erasureCodecId != NErasure::ECodec::None) {
            auto erasureCodec = NErasure::GetCodec(erasureCodecId);
            auto readers = CreateErasureAllPartsReaders(
                ReaderConfig_,
                New<TRemoteReaderOptions>(),
                RemoteClient_,
                Host_->GetInputNodeDirectory(),
                inputChunkId,
                inputReplicas,
                erasureCodec,
                Host_->GetBlockCache());

            chunkMeta = GetChunkMeta(readers.front());

            auto writers = CreateErasurePartWriters(
                WriterConfig_,
                New<TRemoteWriterOptions>(),
                outputChunkId,
                erasureCodec,
                New<TNodeDirectory>(),
                Host_->GetClient());

            YCHECK(readers.size() == writers.size());

            auto erasurePlacementExt = GetProtoExtension<TErasurePlacementExt>(chunkMeta.extensions());

            // Compute an upper bound for total size.
            TotalChunkSize_ =
                GetProtoExtension<TMiscExt>(chunkMeta.extensions()).compressed_data_size() +
                erasurePlacementExt.parity_block_count() * erasurePlacementExt.parity_block_size() * erasurePlacementExt.parity_part_count();

            i64 diskSpace = 0;
            for (int i = 0; i < static_cast<int>(readers.size()); ++i) {
                int blockCount = (i < erasureCodec->GetDataPartCount())
                    ? erasurePlacementExt.part_infos(i).block_sizes_size()
                    : erasurePlacementExt.parity_block_count();

                // ToDo(psushin): copy chunk parts is parallel.
                DoCopy(readers[i], writers[i], blockCount, chunkMeta);
                diskSpace += writers[i]->GetChunkInfo().disk_space();

                auto replicas = writers[i]->GetWrittenChunkReplicas();
                YCHECK(replicas.size() == 1);
                auto replica = TChunkReplica(replicas.front().GetNodeId(), i);

                writtenReplicas.push_back(replica);
            }
            chunkInfo.set_disk_space(diskSpace);
        } else {
            auto reader = CreateReplicationReader(
                ReaderConfig_,
                New<TRemoteReaderOptions>(),
                RemoteClient_,
                Host_->GetInputNodeDirectory(),
                Host_->LocalDescriptor(),
                inputChunkId,
                TChunkReplicaList(),
                Host_->GetBlockCache());

            chunkMeta = GetChunkMeta(reader);

            auto writer = CreateReplicationWriter(
                WriterConfig_,
                New<TRemoteWriterOptions>(),
                outputChunkId,
                TChunkReplicaList(),
                New<TNodeDirectory>(),
                Host_->GetClient());

            auto blocksExt = GetProtoExtension<TBlocksExt>(chunkMeta.extensions());
            int blockCount = static_cast<int>(blocksExt.blocks_size());

            TotalChunkSize_ = GetProtoExtension<TMiscExt>(chunkMeta.extensions()).compressed_data_size();

            DoCopy(reader, writer, blockCount, chunkMeta);

            chunkInfo = writer->GetChunkInfo();
            writtenReplicas = writer->GetWrittenChunkReplicas();
        }

        // Prepare data statistics.
        auto miscExt = GetProtoExtension<TMiscExt>(chunkMeta.extensions());

        TDataStatistics chunkStatistics;
        chunkStatistics.set_compressed_data_size(miscExt.compressed_data_size());
        chunkStatistics.set_uncompressed_data_size(miscExt.uncompressed_data_size());
        chunkStatistics.set_row_count(miscExt.row_count());
        chunkStatistics.set_chunk_count(1);
        DataStatistics_ += chunkStatistics;

        // Confirm chunk.
        LOG_INFO("Confirming output chunk");
        YCHECK(!writtenReplicas.empty());
        {
            static const yhash_set<int> masterMetaTags{
                TProtoExtensionTag<TMiscExt>::Value,
                TProtoExtensionTag<NTableClient::NProto::TBoundaryKeysExt>::Value
            };

            auto masterChunkMeta = chunkMeta;
            FilterProtoExtensions(
                masterChunkMeta.mutable_extensions(),
                chunkMeta.extensions(),
                masterMetaTags);

            auto outputCellTag = CellTagFromId(OutputChunkListId_);
            auto outputMasterChannel = Host_->GetClient()->GetMasterChannelOrThrow(EMasterChannelKind::Leader, outputCellTag);
            TChunkServiceProxy proxy(outputMasterChannel);

            auto batchReq = proxy.ExecuteBatch();
            GenerateMutationId(batchReq);

            auto* req = batchReq->add_confirm_chunk_subrequests();
            ToProto(req->mutable_chunk_id(), outputChunkId);
            *req->mutable_chunk_info() = chunkInfo;
            *req->mutable_chunk_meta() = masterChunkMeta;
            NYT::ToProto(req->mutable_replicas(), writtenReplicas);

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(
                GetCumulativeError(batchRspOrError),
                NChunkClient::EErrorCode::MasterCommunicationFailed,
                "Failed to confirm chunk %v",
                outputChunkId);
        }
    }

    void DoCopy(
        IChunkReaderPtr reader,
        IChunkWriterPtr writer,
        int blockCount,
        const TChunkMeta& meta)
    {
        auto error = WaitFor(writer->Open());
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Error opening writer");

        for (int i = 0; i < blockCount; ++i) {
            auto asyncResult = reader->ReadBlocks(ReaderConfig_->WorkloadDescriptor, i, 1);
            auto result = WaitFor(asyncResult);
            if (!result.IsOK()) {
                FailedChunkId_ = reader->GetChunkId();
                THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error reading block");
            }

            auto block = result.Value().front();
            CopiedChunkSize_ += block.Size();

            if (!writer->WriteBlock(block)) {
                auto result = WaitFor(writer->GetReadyEvent());
                THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error writing block");
            }
        }

        {
            auto result = WaitFor(writer->Close(meta));
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error closing chunk");
        }
    }

    // Request input chunk meta. Input and output chunk metas are the same.
    TChunkMeta GetChunkMeta(IChunkReaderPtr reader)
    {
        auto asyncResult = reader->GetMeta(ReaderConfig_->WorkloadDescriptor);
        auto result = WaitFor(asyncResult);
        if (!result.IsOK()) {
            FailedChunkId_ = reader->GetChunkId();
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Failed to get chunk meta");
        }
        return result.Value();
    }
};

IJobPtr CreateRemoteCopyJob(IJobHostPtr host)
{
    return New<TRemoteCopyJob>(host);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
