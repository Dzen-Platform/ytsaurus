#include "erasure_writer.h"

#include "block_reorderer.h"
#include "chunk_meta_extensions.h"
#include "chunk_writer.h"
#include "config.h"
#include "deferred_chunk_meta.h"
#include "dispatcher.h"
#include "replication_writer.h"
#include "helpers.h"
#include "erasure_helpers.h"
#include "block.h"
#include "private.h"
#include "yt/yt/core/misc/public.h"

#include <yt/yt/client/api/client.h>

#include <yt/yt/ytlib/chunk_client/proto/chunk_info.pb.h>
#include <yt/yt/ytlib/chunk_client/chunk_service_proxy.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/library/erasure/impl/codec.h>

#include <yt/yt/core/misc/numeric_helpers.h>

#include <yt/yt/core/rpc/dispatcher.h>

namespace NYT::NChunkClient {

using namespace NErasure;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NErasureHelpers;

////////////////////////////////////////////////////////////////////////////////

namespace {

////////////////////////////////////////////////////////////////////////////////
// Helpers

// Split blocks into continuous groups of approximately equal sizes.
std::vector<std::vector<TBlock>> SplitBlocks(
    const std::vector<TBlock>& blocks,
    int groupCount)
{
    i64 totalSize = 0;
    for (const auto& block : blocks) {
        totalSize += block.Size();
    }

    std::vector<std::vector<TBlock>> groups(1);
    i64 currentSize = 0;
    for (const auto& block : blocks) {
        groups.back().push_back(block);
        currentSize += block.Size();
        // Current group is fulfilled if currentSize / currentGroupCount >= totalSize / groupCount
        while (currentSize * groupCount >= totalSize * groups.size() &&
               groups.size() < groupCount)
        {
            groups.push_back(std::vector<TBlock>());
        }
    }

    YT_VERIFY(groups.size() == groupCount);

    return groups;
}

std::vector<i64> BlocksToSizes(const std::vector<TBlock>& blocks)
{
    std::vector<i64> sizes;
    for (const auto& block : blocks) {
        sizes.push_back(block.Size());
    }
    return sizes;
}

class TInMemoryBlocksReader
    : public IBlocksReader
{
public:
    TInMemoryBlocksReader(const std::vector<TBlock>& blocks)
        : Blocks_(blocks)
    { }

    virtual TFuture<std::vector<TBlock>> ReadBlocks(const std::vector<int>& blockIndexes) override
    {
        std::vector<TBlock> blocks;
        for (int index = 0; index < blockIndexes.size(); ++index) {
            int blockIndex = blockIndexes[index];
            YT_VERIFY(0 <= blockIndex && blockIndex < Blocks_.size());
            blocks.push_back(Blocks_[blockIndex]);
        }
        return MakeFuture(blocks);
    }

private:
    const std::vector<TBlock>& Blocks_;
};

DEFINE_REFCOUNTED_TYPE(TInMemoryBlocksReader)

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TErasureWriter
    : public IChunkWriter
{
public:
    TErasureWriter(
        TErasureWriterConfigPtr config,
        TSessionId sessionId,
        ECodec codecId,
        ICodec* codec,
        const std::vector<IChunkWriterPtr>& writers,
        const TWorkloadDescriptor& workloadDescriptor)
        : Config_(config)
        , SessionId_(sessionId)
        , CodecId_(codecId)
        , Codec_(codec)
        , WorkloadDescriptor_(workloadDescriptor)
        , Writers_(writers)
        , BlockReorderer_(config)
    {
        YT_VERIFY(writers.size() == codec->GetTotalPartCount());
        VERIFY_INVOKER_THREAD_AFFINITY(TDispatcher::Get()->GetWriterInvoker(), WriterThread);

        ChunkInfo_.set_disk_space(0);
    }

    virtual TFuture<void> Open() override
    {
        return BIND(&TErasureWriter::DoOpen, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

    virtual bool WriteBlock(const TBlock& block) override
    {
        Blocks_.push_back(block);
        return true;
    }

    virtual bool WriteBlocks(const std::vector<TBlock>& blocks) override
    {
        for (const auto& block : blocks) {
            WriteBlock(block);
        }
        return true;
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return MakeFuture(TError());
    }

    virtual const NProto::TChunkInfo& GetChunkInfo() const override
    {
        return ChunkInfo_;
    }

    virtual const NProto::TDataStatistics& GetDataStatistics() const override
    {
        YT_ABORT();
    }

    virtual ECodec GetErasureCodecId() const override
    {
        return CodecId_;
    }

    virtual TChunkReplicaWithMediumList GetWrittenChunkReplicas() const override
    {
        TChunkReplicaWithMediumList result;
        for (int i = 0; i < Writers_.size(); ++i) {
            auto replicas = Writers_[i]->GetWrittenChunkReplicas();
            YT_VERIFY(replicas.size() == 1);
            auto replica = TChunkReplicaWithMedium(
                replicas.front().GetNodeId(),
                i,
                replicas.front().GetMediumIndex());

            result.push_back(replica);
        }
        return result;
    }

    virtual bool IsCloseDemanded() const override
    {
        bool isCloseDemanded = false;
        for (const auto& writer : Writers_) {
            isCloseDemanded |= writer->IsCloseDemanded();
        }
        return isCloseDemanded;
    }

    virtual TFuture<void> Close(const TDeferredChunkMetaPtr& chunkMeta) override;

    virtual TChunkId GetChunkId() const override
    {
        return SessionId_.ChunkId;
    }

private:
    const TErasureWriterConfigPtr Config_;
    const TSessionId SessionId_;
    const ECodec CodecId_;
    ICodec* const Codec_;
    const TWorkloadDescriptor WorkloadDescriptor_;

    bool IsOpen_ = false;

    std::vector<IChunkWriterPtr> Writers_;
    std::vector<TBlock> Blocks_;

    // Information about blocks, necessary to write blocks
    // and encode parity parts
    std::vector<std::vector<TBlock>> Groups_;
    TParityPartSplitInfo ParityPartSplitInfo_;

    std::vector<TChecksum> BlockChecksums_;

    // Chunk meta with information about block placement
    TDeferredChunkMetaPtr ChunkMeta_ = New<TDeferredChunkMeta>();
    NProto::TErasurePlacementExt PlacementExt_;
    NProto::TChunkInfo ChunkInfo_;

    TBlockReorderer BlockReorderer_;

    DECLARE_THREAD_AFFINITY_SLOT(WriterThread);

    void PrepareBlocks();

    void PrepareChunkMeta(const TDeferredChunkMetaPtr& chunkMeta);

    void DoOpen();

    TFuture<void> WriteDataBlocks();

    void WriteDataPart(int partIndex, IChunkWriterPtr writer, const std::vector<TBlock>& blocks);

    TFuture<void> EncodeAndWriteParityBlocks();

    void OnWritten();
};

////////////////////////////////////////////////////////////////////////////////

void TErasureWriter::PrepareBlocks()
{
    if (Config_->ErasureStoreOriginalBlockChecksums) {
        for (auto& block : Blocks_) {
            block.Checksum = block.GetOrComputeChecksum();
            BlockChecksums_.push_back(block.Checksum);
        }
    }

    BlockReorderer_.ReorderBlocks(Blocks_);

    Groups_ = SplitBlocks(Blocks_, Codec_->GetDataPartCount());

    i64 partSize = 0;
    for (const auto& group : Groups_) {
        i64 size = 0;
        for (const auto& block : group) {
            size += block.Size();
        }
        partSize = std::max(partSize, size);
    }
    partSize = RoundUp(partSize, static_cast<i64>(Codec_->GetWordSize()));

    ParityPartSplitInfo_ = TParityPartSplitInfo::Build(Config_->ErasureWindowSize, partSize);
}

void TErasureWriter::PrepareChunkMeta(const TDeferredChunkMetaPtr& chunkMeta)
{
    int start = 0;
    for (const auto& group : Groups_) {
        auto* info = PlacementExt_.add_part_infos();
        // NB: these block indexes are calculated after the reordering,
        // so we will set them here and not in the deferred callback.
        info->set_first_block_index(start);
        for (const auto& block : group) {
            info->add_block_sizes(block.Size());
        }
        start += group.size();
    }
    PlacementExt_.set_parity_part_count(Codec_->GetParityPartCount());
    PlacementExt_.set_parity_block_count(ParityPartSplitInfo_.BlockCount);
    PlacementExt_.set_parity_block_size(Config_->ErasureWindowSize);
    PlacementExt_.set_parity_last_block_size(ParityPartSplitInfo_.LastBlockSize);
    PlacementExt_.mutable_part_checksums()->Resize(Codec_->GetTotalPartCount(), NullChecksum);

    if (Config_->ErasureStoreOriginalBlockChecksums) {
        NYT::ToProto(PlacementExt_.mutable_block_checksums(), BlockChecksums_);
    }

    ChunkMeta_ = chunkMeta;
    ChunkMeta_->BlockIndexMapping() = BlockReorderer_.BlockIndexMapping();
    ChunkMeta_->Finalize();
}

void TErasureWriter::DoOpen()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    std::vector<TFuture<void>> asyncResults;
    for (auto writer : Writers_) {
        asyncResults.push_back(writer->Open());
    }
    WaitFor(AllSucceeded(asyncResults))
        .ThrowOnError();

    IsOpen_ = true;
}

TFuture<void> TErasureWriter::WriteDataBlocks()
{
    VERIFY_THREAD_AFFINITY(WriterThread);
    YT_VERIFY(Groups_.size() <= Writers_.size());

    std::vector<TFuture<void>> asyncResults;
    for (int index = 0; index < Groups_.size(); ++index) {
        asyncResults.push_back(
            BIND(
                &TErasureWriter::WriteDataPart,
                MakeStrong(this),
                index,
                Writers_[index],
                Groups_[index])
            .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
            .Run());
    }
    return AllSucceeded(asyncResults);
}

void TErasureWriter::WriteDataPart(int partIndex, IChunkWriterPtr writer, const std::vector<TBlock>& blocks)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    std::vector<TChecksum> blockChecksums;
    for (const auto& block : blocks) {
        TBlock blockWithChecksum{block};
        blockWithChecksum.Checksum = block.GetOrComputeChecksum();
        blockChecksums.push_back(blockWithChecksum.Checksum);

        if (!writer->WriteBlock(blockWithChecksum)) {
            WaitFor(writer->GetReadyEvent())
                .ThrowOnError();
        }
    }

    auto checksum = CombineChecksums(blockChecksums);
    YT_VERIFY(checksum != NullChecksum || blockChecksums.empty() ||
        std::all_of(blockChecksums.begin(), blockChecksums.end(), [] (TChecksum value) {
            return value == NullChecksum;
        }));

    PlacementExt_.mutable_part_checksums()->Set(partIndex, checksum);
}

TFuture<void> TErasureWriter::EncodeAndWriteParityBlocks()
{
    VERIFY_INVOKER_AFFINITY(NRpc::TDispatcher::Get()->GetCompressionPoolInvoker());

    TPartIndexList parityIndices;
    for (int index = Codec_->GetDataPartCount(); index < Codec_->GetTotalPartCount(); ++index) {
        parityIndices.push_back(index);
    }

    std::vector<IPartBlockProducerPtr> blockProducers;
    for (const auto& group : Groups_) {
        auto blocksReader = New<TInMemoryBlocksReader>(group);
        blockProducers.push_back(New<TPartReader>(blocksReader, BlocksToSizes(group)));
    }

    std::vector<TPartWriterPtr> writerConsumers;
    std::vector<IPartBlockConsumerPtr> blockConsumers;
    for (auto index : parityIndices) {
        writerConsumers.push_back(New<TPartWriter>(
            Writers_[index],
            ParityPartSplitInfo_.GetSizes(),
            /* computeChecksums */ true));
        blockConsumers.push_back(writerConsumers.back());
    }

    std::vector<TPartRange> ranges(1, TPartRange{0, ParityPartSplitInfo_.GetPartSize()});
    auto encoder = New<TPartEncoder>(
        Codec_,
        parityIndices,
        ParityPartSplitInfo_,
        ranges,
        blockProducers,
        blockConsumers);
    encoder->Run();

    std::vector<std::pair<int, TChecksum>> partChecksums;
    for (int index = 0; index < parityIndices.size(); ++index) {
        partChecksums.push_back(std::make_pair(parityIndices[index], writerConsumers[index]->GetPartChecksum()));
    }

    return BIND([=, this_ = MakeStrong(this)] () {
        // Access to PlacementExt_ must be from WriterInvoker only.
        for (auto [index, checksum] : partChecksums) {
            PlacementExt_.mutable_part_checksums()->Set(index, checksum);
        }
    })
    .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
    .Run();
}

TFuture<void> TErasureWriter::Close(const TDeferredChunkMetaPtr& chunkMeta)
{
    YT_VERIFY(IsOpen_);

    PrepareBlocks();
    PrepareChunkMeta(chunkMeta);

    auto compressionInvoker = CreateFixedPriorityInvoker(
        NRpc::TDispatcher::Get()->GetPrioritizedCompressionPoolInvoker(),
        WorkloadDescriptor_.GetPriority());

    std::vector<TFuture<void>> asyncResults {
        BIND(&TErasureWriter::WriteDataBlocks, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
            .Run(),
        BIND(&TErasureWriter::EncodeAndWriteParityBlocks, MakeStrong(this))
            .AsyncVia(compressionInvoker)
            .Run()
    };

    return AllSucceeded(asyncResults).Apply(
        BIND(&TErasureWriter::OnWritten, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetWriterInvoker()));
}


void TErasureWriter::OnWritten()
{
    std::vector<TFuture<void>> asyncResults;

    SetProtoExtension(ChunkMeta_->mutable_extensions(), PlacementExt_);

    for (const auto& writer : Writers_) {
        asyncResults.push_back(writer->Close(ChunkMeta_));
    }

    WaitFor(AllSucceeded(asyncResults))
        .ThrowOnError();

    i64 diskSpace = 0;
    for (const auto& writer : Writers_) {
        diskSpace += writer->GetChunkInfo().disk_space();
    }
    ChunkInfo_.set_disk_space(diskSpace);

    Groups_.clear();
    Blocks_.clear();
}

////////////////////////////////////////////////////////////////////////////////

IChunkWriterPtr CreateErasureWriter(
    TErasureWriterConfigPtr config,
    TSessionId sessionId,
    ECodec codecId,
    ICodec* codec,
    const std::vector<IChunkWriterPtr>& writers,
    const TWorkloadDescriptor& workloadDescriptor)
{
    return New<TErasureWriter>(
        config,
        sessionId,
        codecId,
        codec,
        writers,
        workloadDescriptor);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
