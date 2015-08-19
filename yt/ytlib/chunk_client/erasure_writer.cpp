#include "public.h"
#include "config.h"
#include "dispatcher.h"
#include "chunk_writer.h"
#include "chunk_replica.h"
#include "chunk_meta_extensions.h"
#include "replication_writer.h"

#include <ytlib/api/client.h>

#include <ytlib/chunk_client/chunk_service_proxy.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <core/concurrency/scheduler.h>
#include <core/concurrency/parallel_awaiter.h>

#include <core/erasure/codec.h>

#include <core/misc/address.h>

#include <core/ytree/yson_serializable.h>

namespace NYT {
namespace NChunkClient {

using namespace NConcurrency;
using namespace NNodeTrackerClient;

///////////////////////////////////////////////////////////////////////////////

namespace {

///////////////////////////////////////////////////////////////////////////////
// Helpers

// Split blocks into continuous groups of approximately equal sizes.
std::vector<std::vector<TSharedRef>> SplitBlocks(
    const std::vector<TSharedRef>& blocks,
    int groupCount)
{
    i64 totalSize = 0;
    for (const auto& block : blocks) {
        totalSize += block.Size();
    }

    std::vector<std::vector<TSharedRef>> groups(1);
    i64 currentSize = 0;
    for (const auto& block : blocks) {
        groups.back().push_back(block);
        currentSize += block.Size();
        // Current group is fulfilled if currentSize / currentGroupCount >= totalSize / groupCount
        while (currentSize * groupCount >= totalSize * groups.size() &&
               groups.size() < groupCount)
        {
            groups.push_back(std::vector<TSharedRef>());
        }
    }

    YCHECK(groups.size() == groupCount);

    return groups;
}

i64 RoundUp(i64 num, i64 mod)
{
    if (num % mod == 0) {
        return num;
    }
    return num + mod - (num % mod);
}

class TSlicer
{
public:
    explicit TSlicer(const std::vector<TSharedRef>& blocks)
        : Blocks_(blocks)
    { }

    TSharedRef GetSlice(i64 start, i64 end) const
    {
        YCHECK(start >= 0);
        YCHECK(start <= end);

        TSharedMutableRef result;

        i64 pos = 0;
        i64 resultSize = end - start;

        // We use lazy initialization.
        auto initialize = [&] () {
            if (!result) {
                struct TErasureWriterSliceTag { };
                result = TSharedMutableRef::Allocate<TErasureWriterSliceTag>(resultSize);
            }
        };

        i64 currentStart = 0;

        for (const auto& block : Blocks_) {
            i64 innerStart = std::max(static_cast<i64>(0), start - currentStart);
            i64 innerEnd = std::min(static_cast<i64>(block.Size()), end - currentStart);

            if (innerStart < innerEnd) {
                if (resultSize == innerEnd - innerStart) {
                    return block.Slice(innerStart, innerEnd);
                }

                initialize();
                std::copy(block.Begin() + innerStart, block.Begin() + innerEnd, result.Begin() + pos);

                pos += (innerEnd - innerStart);
            }
            currentStart += block.Size();

            if (pos == resultSize || currentStart >= end) {
                break;
            }
        }

        initialize();
        return result;
    }

private:

    // Mutable since we want to return subref of blocks.
    mutable std::vector<TSharedRef> Blocks_;
};

} // namespace

///////////////////////////////////////////////////////////////////////////////

class TErasureWriter
    : public IChunkWriter
{
public:
    TErasureWriter(
        TErasureWriterConfigPtr config,
        const TChunkId& chunkId,
        NErasure::ICodec* codec,
        const std::vector<IChunkWriterPtr>& writers)
        : Config_(config)
        , ChunkId_(chunkId)
        , Codec_(codec)
        , Writers_(writers)
    {
        YCHECK(writers.size() == codec->GetTotalPartCount());
        VERIFY_INVOKER_THREAD_AFFINITY(TDispatcher::Get()->GetWriterInvoker(), WriterThread);

        ChunkInfo_.set_disk_space(0);
    }

    virtual TFuture<void> Open() override
    {
        return BIND(&TErasureWriter::DoOpen, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

    virtual bool WriteBlock(const TSharedRef& block) override
    {
        Blocks_.push_back(block);
        return true;
    }

    virtual bool WriteBlocks(const std::vector<TSharedRef>& blocks) override
    {
        bool result = true;
        for (const auto& block : blocks) {
            result = WriteBlock(block);
        }
        return result;
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        auto error = TPromise<void>();
        error.Set(TError());
        return error;
    }

    virtual const NProto::TChunkInfo& GetChunkInfo() const override
    {
        return ChunkInfo_;
    }

    virtual TChunkReplicaList GetWrittenChunkReplicas() const override
    {
        TChunkReplicaList result;
        for (int i = 0; i < Writers_.size(); ++i) {
            auto replicas = Writers_[i]->GetWrittenChunkReplicas();
            YCHECK(replicas.size() == 1);
            auto replica = TChunkReplica(replicas.front().GetNodeId(), i);

            result.push_back(replica);
        }
        return result;
    }

    virtual TFuture<void> Close(const NProto::TChunkMeta& chunkMeta) override;

    virtual TChunkId GetChunkId() const override
    {
        return ChunkId_;
    }

private:
    void PrepareBlocks();

    void PrepareChunkMeta(const NProto::TChunkMeta& chunkMeta);

    void DoOpen();

    TFuture<void> WriteDataBlocks();

    void EncodeAndWriteParityBlocks();

    void WriteDataPart(IChunkWriterPtr writer, const std::vector<TSharedRef>& blocks);

    TFuture<void> WriteParityBlocks(const std::vector<TSharedRef>& blocks);

    TFuture<void> CloseParityWriters();

    void OnClosed();


    const TErasureWriterConfigPtr Config_;
    const TChunkId ChunkId_;
    NErasure::ICodec* const Codec_;

    bool IsOpen_ = false;

    std::vector<IChunkWriterPtr> Writers_;
    std::vector<TSharedRef> Blocks_;

    // Information about blocks, necessary to write blocks
    // and encode parity parts
    std::vector<std::vector<TSharedRef>> Groups_;
    std::vector<TSlicer> Slicers_;
    i64 ParityDataSize_;
    int WindowCount_;

    // Chunk meta with information about block placement
    NProto::TChunkMeta ChunkMeta_;
    NProto::TChunkInfo ChunkInfo_;

    DECLARE_THREAD_AFFINITY_SLOT(WriterThread);

};

///////////////////////////////////////////////////////////////////////////////

void TErasureWriter::PrepareBlocks()
{
    Groups_ = SplitBlocks(Blocks_, Codec_->GetDataPartCount());

    YCHECK(Slicers_.empty());

    // Calculate size of parity blocks and form slicers
    ParityDataSize_ = 0;
    for (const auto& group : Groups_) {
        i64 size = 0;
        i64 maxBlockSize = 0;
        for (const auto& block : group) {
            size += block.Size();
            maxBlockSize = std::max(maxBlockSize, (i64)block.Size());
        }
        ParityDataSize_ = std::max(ParityDataSize_, size);

        Slicers_.push_back(TSlicer(group));
    }

    // Calculate number of windows
    ParityDataSize_ = RoundUp(ParityDataSize_, Codec_->GetWordSize());

    WindowCount_ = ParityDataSize_ / Config_->ErasureWindowSize;
    if (ParityDataSize_ % Config_->ErasureWindowSize != 0) {
        WindowCount_ += 1;
    }
}

void TErasureWriter::PrepareChunkMeta(const NProto::TChunkMeta& chunkMeta)
{
    int start = 0;
    NProto::TErasurePlacementExt placementExt;
    for (const auto& group : Groups_) {
        auto* info = placementExt.add_part_infos();
        info->set_first_block_index(start);
        for (const auto& block : group) {
            info->add_block_sizes(block.Size());
        }
        start += group.size();
    }
    placementExt.set_parity_part_count(Codec_->GetParityPartCount());
    placementExt.set_parity_block_count(WindowCount_);
    placementExt.set_parity_block_size(Config_->ErasureWindowSize);
    placementExt.set_parity_last_block_size(ParityDataSize_ - (Config_->ErasureWindowSize * (WindowCount_ - 1)));

    ChunkMeta_ = chunkMeta;
    SetProtoExtension(ChunkMeta_.mutable_extensions(), placementExt);
}

void TErasureWriter::DoOpen()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    std::vector<TFuture<void>> asyncResults;
    for (auto writer : Writers_) {
        asyncResults.push_back(writer->Open());
    }
    WaitFor(Combine(asyncResults))
        .ThrowOnError();

    IsOpen_ = true;
}

TFuture<void> TErasureWriter::WriteDataBlocks()
{
    VERIFY_THREAD_AFFINITY(WriterThread);
    YCHECK(Groups_.size() <= Writers_.size());

    std::vector<TFuture<void>> asyncResults;
    for (int index = 0; index < Groups_.size(); ++index) {
        asyncResults.push_back(
            BIND(
                &TErasureWriter::WriteDataPart,
                MakeStrong(this),
                Writers_[index],
                Groups_[index])
            .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
            .Run());
    }
    return Combine(asyncResults);
}

void TErasureWriter::WriteDataPart(IChunkWriterPtr writer, const std::vector<TSharedRef>& blocks)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    for (const auto& block : blocks) {
        if (!writer->WriteBlock(block)) {
            WaitFor(writer->GetReadyEvent())
                .ThrowOnError();
        }
    }

    WaitFor(writer->Close(ChunkMeta_))
        .ThrowOnError();
}

void TErasureWriter::EncodeAndWriteParityBlocks()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    for (i64 begin = 0; begin < ParityDataSize_; begin += Config_->ErasureWindowSize) {
        i64 end = std::min(begin + Config_->ErasureWindowSize, ParityDataSize_);
        auto asyncParityBlocks =
            BIND([=, this_ = MakeStrong(this)] () {
                // Generate bytes from [begin, end) for parity blocks.
                std::vector<TSharedRef> slices;
                for (const auto& slicer : Slicers_) {
                    slices.push_back(slicer.GetSlice(begin, end));
                }
                return Codec_->Encode(slices);
            })
            .AsyncVia(TDispatcher::Get()->GetErasurePoolInvoker())
            .Run();
        auto parityBlocksOrError = WaitFor(asyncParityBlocks);
        const auto& parityBlocks = parityBlocksOrError.ValueOrThrow();
        WaitFor(WriteParityBlocks(parityBlocks))
            .ThrowOnError();
    }

    WaitFor(CloseParityWriters())
        .ThrowOnError();
}

TFuture<void> TErasureWriter::WriteParityBlocks(const std::vector<TSharedRef>& blocks)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    // Write blocks of current window in parallel manner.
    std::vector<TFuture<void>> asyncResults;
    for (int index = 0; index < Codec_->GetParityPartCount(); ++index) {
        const auto& writer = Writers_[Codec_->GetDataPartCount() + index];
        writer->WriteBlock(blocks[index]);
        asyncResults.push_back(writer->GetReadyEvent());
    }
    return Combine(asyncResults);
}

TFuture<void> TErasureWriter::CloseParityWriters()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    std::vector<TFuture<void>> asyncResults;
    for (int index = 0; index < Codec_->GetParityPartCount(); ++index) {
        const auto& writer = Writers_[Codec_->GetDataPartCount() + index];
        asyncResults.push_back(writer->Close(ChunkMeta_));
    }
    return Combine(asyncResults);
}

TFuture<void> TErasureWriter::Close(const NProto::TChunkMeta& chunkMeta)
{
    YCHECK(IsOpen_);

    PrepareBlocks();
    PrepareChunkMeta(chunkMeta);

    auto invoker = TDispatcher::Get()->GetWriterInvoker();

    std::vector<TFuture<void>> asyncResults {
        BIND(&TErasureWriter::WriteDataBlocks, MakeStrong(this))
            .AsyncVia(invoker)
            .Run(),
        BIND(&TErasureWriter::EncodeAndWriteParityBlocks, MakeStrong(this))
            .AsyncVia(invoker)
            .Run()
    };

    return Combine(asyncResults).Apply(
        BIND(&TErasureWriter::OnClosed, MakeStrong(this)));
}


void TErasureWriter::OnClosed()
{
    i64 diskSpace = 0;
    for (auto writer : Writers_) {
        diskSpace += writer->GetChunkInfo().disk_space();
    }
    ChunkInfo_.set_disk_space(diskSpace);

    Slicers_.clear();
    Groups_.clear();
    Blocks_.clear();
}

///////////////////////////////////////////////////////////////////////////////

IChunkWriterPtr CreateErasureWriter(
    TErasureWriterConfigPtr config,
    const TChunkId& chunkId,
    NErasure::ICodec* codec,
    const std::vector<IChunkWriterPtr>& writers)
{
    return New<TErasureWriter>(
        config,
        chunkId,
        codec,
        writers);
}

///////////////////////////////////////////////////////////////////////////////

std::vector<IChunkWriterPtr> CreateErasurePartWriters(
    TReplicationWriterConfigPtr config,
    TRemoteWriterOptionsPtr options,
    const TChunkId& chunkId,
    NErasure::ICodec* codec,
    TNodeDirectoryPtr nodeDirectory,
    NApi::IClientPtr client,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
{
    // Patch writer configs to ignore upload replication factor for erasure chunk parts.
    auto partConfig = NYTree::CloneYsonSerializable(config);
    partConfig->UploadReplicationFactor = 1;

    TChunkServiceProxy proxy(client->GetMasterChannel(NApi::EMasterChannelKind::LeaderOrFollower));

    auto req = proxy.AllocateWriteTargets();
    req->set_desired_target_count(codec->GetTotalPartCount());
    req->set_min_target_count(codec->GetTotalPartCount());
    if (partConfig->PreferLocalHost) {
        req->set_preferred_host_name(TAddressResolver::Get()->GetLocalHostName());
    }
    ToProto(req->mutable_chunk_id(), chunkId);

    auto rspOrError = WaitFor(req->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        rspOrError,
        EErrorCode::MasterCommunicationFailed, 
        "Failed to allocate write targets for chunk %v", 
        chunkId);
    auto rsp = rspOrError.Value();

    nodeDirectory->MergeFrom(rsp->node_directory());
    auto replicas = NYT::FromProto<TChunkReplica, TChunkReplicaList>(rsp->replicas());

    YCHECK(replicas.size() == codec->GetTotalPartCount());

    std::vector<IChunkWriterPtr> writers;
    for (int index = 0; index < codec->GetTotalPartCount(); ++index) {
        auto partId = ErasurePartIdFromChunkId(chunkId, index);
        writers.push_back(CreateReplicationWriter(
            partConfig,
            options,
            partId,
            TChunkReplicaList(1, replicas[index]),
            nodeDirectory,
            client,
            blockCache,
            throttler));
    }

    return writers;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
