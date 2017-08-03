#include "erasure_repair.h"
#include "chunk_meta_extensions.h"
#include "chunk_reader.h"
#include "chunk_writer.h"
#include "config.h"
#include "dispatcher.h"
#include "erasure_helpers.h"
#include "private.h"

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/erasure/codec.h>

#include <numeric>

namespace NYT {
namespace NChunkClient {

using namespace NErasure;
using namespace NConcurrency;
using namespace NChunkClient::NProto;
using namespace NErasureHelpers;

////////////////////////////////////////////////////////////////////////////////

//! TODO: think about other name.
//! Caching chunk reader that assumes monotonic requests for block indexes with possible overlaps.
//! Also supports functionality to save blocks with given indexes.
class TMonotonicBlocksReader
    : public IBlocksReader
{
public:
    TMonotonicBlocksReader(IChunkReaderPtr reader, TWorkloadDescriptor workloadDescriptor, const std::vector<int>& blocksToSave = {})
        : UnderlyingReader_(reader)
        , WorkloadDescriptor_(workloadDescriptor)
        , BlocksToSave_(blocksToSave)
        , SavedBlocks_(blocksToSave.size())
    { }

    virtual TFuture<std::vector<TBlock>> ReadBlocks(const std::vector<int>& blockIndexes) override
    {
        if (blockIndexes.empty()) {
            return MakeFuture(std::vector<TBlock>());
        }

        while (!CachedBlocks_.empty() && CachedBlocks_.front().first < blockIndexes.front()) {
            CachedBlocks_.pop_front();
        }

        std::vector<TBlock> resultBlocks;

        int index = 0;
        while (index < blockIndexes.size() && index < CachedBlocks_.size()) {
            resultBlocks.push_back(CachedBlocks_[index].second);
        }

        YCHECK(index == CachedBlocks_.size());

        if (index < blockIndexes.size()) {
            auto blockIndexesToRequest = std::vector<int>(blockIndexes.begin() + index, blockIndexes.end());
            auto blocksFuture = UnderlyingReader_->ReadBlocks(WorkloadDescriptor_, blockIndexesToRequest);
            return blocksFuture.Apply(BIND([=, this_ = MakeStrong(this)] (const std::vector<TBlock>& blocks) mutable {
                for (int index = 0; index < blockIndexesToRequest.size(); ++index) {
                    auto blockIndex = blockIndexesToRequest[index];
                    auto block = blocks[index];
                    auto it = std::lower_bound(BlocksToSave_.begin(), BlocksToSave_.end(), blockIndex);
                    if (it != BlocksToSave_.end() && *it == blockIndex) {
                        SavedBlocks_[it - BlocksToSave_.begin()] = block;
                    }
                    CachedBlocks_.push_back(std::make_pair(blockIndex, block));
                }
                resultBlocks.insert(resultBlocks.end(), blocks.begin(), blocks.end());
                return resultBlocks;
            }));
        } else {
            return MakeFuture(resultBlocks);
        }
    }

    TFuture<void> ReadMissingBlocksToSave()
    {
        std::vector<int> indexesToRead;
        yhash<int, int> blockIndexToSavedBlocksIndex;
        int counter = 0;
        for (int index = 0; index < BlocksToSave_.size(); ++index) {
            if (!SavedBlocks_[index]) {
                indexesToRead.push_back(BlocksToSave_[index]);
                blockIndexToSavedBlocksIndex[counter++] = index;
            }
        }
        auto blocksFuture = UnderlyingReader_->ReadBlocks(WorkloadDescriptor_, indexesToRead);
        return blocksFuture.Apply(BIND([=, this_ = MakeStrong(this)] (const std::vector<TBlock>& blocks) mutable {
            for (int index = 0; index < blocks.size(); ++index) {
                auto it = blockIndexToSavedBlocksIndex.find(index);
                YCHECK(it != blockIndexToSavedBlocksIndex.end());
                SavedBlocks_[it->second] = blocks[index];
            }
        }));
    }

    std::vector<TBlock> GetSavedBlocks() const
    {
        std::vector<TBlock> result;
        for (const auto& blockOrNull : SavedBlocks_) {
            YCHECK(blockOrNull);
            result.push_back(*blockOrNull);
        }
        return result;
    }

private:
    const IChunkReaderPtr UnderlyingReader_;
    const TWorkloadDescriptor WorkloadDescriptor_;
    const std::vector<int> BlocksToSave_;

    std::vector<TNullable<TBlock>> SavedBlocks_;
    std::deque<std::pair<int, TBlock>> CachedBlocks_;
};

DECLARE_REFCOUNTED_TYPE(TMonotonicBlocksReader)
DEFINE_REFCOUNTED_TYPE(TMonotonicBlocksReader)

////////////////////////////////////////////////////////////////////////////////

class TRepairAllPartsSession
    : public TRefCounted
{
public:
    TRepairAllPartsSession(
        ICodec* codec,
        const TPartIndexList& erasedIndices,
        const std::vector<IChunkReaderPtr>& readers,
        const std::vector<IChunkWriterPtr>& writers,
        const TWorkloadDescriptor& workloadDescriptor)
        : Codec_(codec)
        , Readers_(readers)
        , Writers_(writers)
        , ErasedIndices_(erasedIndices)
        , WorkloadDescriptor_(workloadDescriptor)
    {
        YCHECK(erasedIndices.size() == writers.size());
    }

    TFuture<void> Run()
    {
        if (Readers_.empty()) {
            return VoidFuture;
        }

        return BIND(&TRepairAllPartsSession::DoRun, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
            .Run();
    }

private:
    const ICodec* const Codec_;
    const std::vector<IChunkReaderPtr> Readers_;
    const std::vector<IChunkWriterPtr> Writers_;
    const TPartIndexList ErasedIndices_;
    const TWorkloadDescriptor WorkloadDescriptor_;

    TParityPartSplitInfo ParityPartSplitInfo_;

    std::vector<std::vector<i64>> ErasedPartBlockSizes_;
    std::vector<std::vector<i64>> RepairPartBlockSizes_;

    i64 ErasedDataSize_ = 0;
    int ErasedBlockCount_ = 0;

    void DoRun()
    {
        // Open writers.
        {
            std::vector<TFuture<void>> asyncResults;
            for (auto writer : Writers_) {
                asyncResults.push_back(writer->Open());
            }
            WaitFor(Combine(asyncResults))
                .ThrowOnError();
        }

        // Get placement extension.
        auto placementExt = WaitFor(GetPlacementMeta(Readers_.front(), WorkloadDescriptor_))
            .ValueOrThrow();
        ProcessPlacementExt(placementExt);

        // Prepare erasure part readers.
        std::vector<IPartBlockProducerPtr> blockProducers;
        for (int index = 0; index < Readers_.size(); ++index) {
            auto monotonicReader = New<TMonotonicBlocksReader>(Readers_[index], WorkloadDescriptor_);
            blockProducers.push_back(New<TPartReader>(
                monotonicReader,
                RepairPartBlockSizes_[index]));
        }

        // Prepare erasure part writers.
        std::vector<TPartWriterPtr> writerConsumers;
        std::vector<IPartBlockConsumerPtr> blockConsumers;
        for (int index = 0; index < Writers_.size(); ++index) {
            writerConsumers.push_back(New<TPartWriter>(
                Writers_[index],
                ErasedPartBlockSizes_[index],
                /* computeChecksums */ true));
            blockConsumers.push_back(writerConsumers.back());
        }

        // Run encoder.
        std::vector<TPartRange> ranges(1, TPartRange{0, ParityPartSplitInfo_.GetPartSize()});
        auto encoder = New<TPartEncoder>(
            Codec_,
            ErasedIndices_,
            ParityPartSplitInfo_,
            ranges,
            blockProducers,
            blockConsumers);
        encoder->Run();

        // Fetch chunk meta.
        auto reader = Readers_.front(); // an arbitrary one will do
        auto meta = WaitFor(reader->GetMeta(WorkloadDescriptor_))
            .ValueOrThrow();

        // Validate repaired parts checksums
        if (placementExt.part_checksums_size() != 0) {
            YCHECK(placementExt.part_checksums_size() == Codec_->GetTotalPartCount());

            for (int index = 0; index < Writers_.size(); ++index) {
                TChecksum repairedPartChecksum = writerConsumers[index]->GetPartChecksum();
                TChecksum expectedPartChecksum = placementExt.part_checksums(ErasedIndices_[index]);

                YCHECK(expectedPartChecksum == NullChecksum || repairedPartChecksum == expectedPartChecksum);
            }
        }

        // Close all writers.
        {
            std::vector<TFuture<void>> asyncResults;
            for (auto writer : Writers_) {
                asyncResults.push_back(writer->Close(meta));
            }
            WaitFor(Combine(asyncResults))
                .ThrowOnError();
        }
    }

    void ProcessPlacementExt(const TErasurePlacementExt& placementExt)
    {
        ParityPartSplitInfo_ = TParityPartSplitInfo(
            placementExt.parity_block_count(),
            placementExt.parity_block_size(),
            placementExt.parity_last_block_size());

        auto repairIndices = Codec_->GetRepairIndices(ErasedIndices_);
        YCHECK(repairIndices);
        YCHECK(repairIndices->size() == Readers_.size());

        for (int i = 0; i < Readers_.size(); ++i) {
            int repairIndex = (*repairIndices)[i];
            RepairPartBlockSizes_.push_back(GetBlockSizes(repairIndex, placementExt));
        }

        for (int erasedIndex : ErasedIndices_) {
            auto blockSizes = GetBlockSizes(erasedIndex, placementExt);
            ErasedPartBlockSizes_.push_back(blockSizes);
            ErasedBlockCount_ += blockSizes.size();
            ErasedDataSize_ += std::accumulate(blockSizes.begin(), blockSizes.end(), 0LL);
        }
    }

    std::vector<i64> GetBlockSizes(int partIndex, const TErasurePlacementExt& placementExt)
    {
        if (partIndex < Codec_->GetDataPartCount()) {
            const auto& blockSizesProto = placementExt.part_infos().Get(partIndex).block_sizes();
            return std::vector<i64>(blockSizesProto.begin(), blockSizesProto.end());
        } else {
            return ParityPartSplitInfo_.GetSizes();
        }
    }
};

TFuture<void> RepairErasedParts(
    ICodec* codec,
    const TPartIndexList& erasedIndices,
    const std::vector<IChunkReaderPtr>& readers,
    const std::vector<IChunkWriterPtr>& writers,
    const TWorkloadDescriptor& workloadDescriptor)
{
    auto session = New<TRepairAllPartsSession>(
        codec,
        erasedIndices,
        readers,
        writers,
        workloadDescriptor);
    return session->Run();
}

////////////////////////////////////////////////////////////////////////////////

class TPartBlockSaver
    : public IPartBlockConsumer
{
public:
    TPartBlockSaver(const std::vector<TPartRange>& ranges)
        : Ranges_(ranges)
        , Blocks_(ranges.size())
    {
        for (int index = 0; index < Ranges_.size(); ++index) {
            auto size = Ranges_[index].Size();
            Blocks_[index] = TSharedMutableRef::Allocate(size);
            TotalBytes_ += size;
        }
    }

    virtual TFuture<void> Consume(const TPartRange& range, const TSharedRef& block) override
    {
        YCHECK(range.Begin >= Cursor_);
        Cursor_ = range.End;

        for (int index = 0; index < Ranges_.size(); ++index) {
            auto blockRange = Ranges_[index];
            auto intersection = Intersection(blockRange, range);
            if (!intersection) {
                continue;
            }
            memcpy(
                Blocks_[index].Begin() + (intersection.Begin - blockRange.Begin),
                block.Begin() + (intersection.Begin - range.Begin),
                intersection.Size());
            SavedBytes_ += intersection.Size();
        }

        return MakeFuture(TError());
    }

    std::vector<TBlock> GetSavedBlocks()
    {
        YCHECK(TotalBytes_ == SavedBytes_);
        std::vector<TBlock> result;
        for (const auto& block : Blocks_) {
            result.emplace_back(TSharedRef(block));
        }
        return result;
    }

private:
    const std::vector<TPartRange> Ranges_;

    std::vector<TSharedMutableRef> Blocks_;
    i64 TotalBytes_ = 0;
    i64 SavedBytes_ = 0;
    i64 Cursor_ = 0;
};

class TEmptyPartBlockConsumer
    : public IPartBlockConsumer
{
public:
    virtual TFuture<void> Consume(const TPartRange& range, const TSharedRef& block) override
    {
        return MakeFuture(TError());
    }
};

DECLARE_REFCOUNTED_TYPE(TPartBlockSaver)
DEFINE_REFCOUNTED_TYPE(TPartBlockSaver)

class TRepairingReaderSession
    : public TRefCounted
{
public:
    TRepairingReaderSession(
        ICodec* codec,
        const TPartIndexList& erasedIndices,
        const std::vector<IChunkReaderPtr>& readers,
        const TErasurePlacementExt& placementExt,
        const std::vector<int>& blockIndexes,
        const TWorkloadDescriptor& workloadDescriptor)
        : Codec_(codec)
        , ErasedIndices_(erasedIndices)
        , Readers_(readers)
        , PlacementExt_(placementExt)
        , BlockIndexes_(blockIndexes)
        , WorkloadDescriptor_(workloadDescriptor)
        , ParityPartSplitInfo_(GetParityPartSplitInfo(PlacementExt_))
        , DataBlocksPlacementInParts_(BuildDataBlocksPlacementInParts(BlockIndexes_, PlacementExt_))
    {
        auto repairIndices = *Codec_->GetRepairIndices(ErasedIndices_);
        YCHECK(std::is_sorted(ErasedIndices_.begin(), ErasedIndices_.end()));
        YCHECK(std::is_sorted(repairIndices.begin(), repairIndices.end()));

        for (int partIndex : repairIndices) {
            RepairPartBlockSizes_.push_back(GetBlockSizes(partIndex, PlacementExt_));
        }
        for (int erasedIndex : ErasedIndices_) {
            ErasedPartBlockSizes_.push_back(GetBlockSizes(erasedIndex, PlacementExt_));
        }

        auto dataPartCount = Codec_->GetDataPartCount();

        std::vector<TPartRange> repairRanges;

        // Index in Readers_ array, we consider part in ascending order and support index of current reader.
        int readerIndex = 0;

        // Prepare data part readers and block savers.
        for (int partIndex = 0; partIndex < dataPartCount; ++partIndex) {
            auto blocksPlacementInPart = DataBlocksPlacementInParts_[partIndex];
            if (std::binary_search(ErasedIndices_.begin(), ErasedIndices_.end(), partIndex)) {
                PartBlockSavers_.push_back(New<TPartBlockSaver>(blocksPlacementInPart.Ranges));
                repairRanges.insert(
                    repairRanges.end(),
                    blocksPlacementInPart.Ranges.begin(),
                    blocksPlacementInPart.Ranges.end());
            } else {
                auto partReader = New<TMonotonicBlocksReader>(
                    Readers_[readerIndex++],
                    WorkloadDescriptor_,
                    blocksPlacementInPart.IndexesInPart);
                AllPartReaders_.push_back(partReader);
                if (std::binary_search(repairIndices.begin(), repairIndices.end(), partIndex)) {
                    RepairPartReaders_.push_back(partReader);
                }
            }
        }

        // Finish building repair part readers.
        for (auto partIndex : repairIndices) {
            if (partIndex >= dataPartCount) {
                RepairPartReaders_.push_back(New<TMonotonicBlocksReader>(
                    Readers_[readerIndex++],
                    WorkloadDescriptor_));
            }
        }

        // Build part block producers.
        for (int index = 0; index < repairIndices.size(); ++index) {
            BlockProducers_.push_back(New<TPartReader>(
                RepairPartReaders_[index],
                RepairPartBlockSizes_[index]));
        }

        // Build part block consumers.
        BlockConsumers_.insert(BlockConsumers_.end(), PartBlockSavers_.begin(), PartBlockSavers_.end());
        for (auto partIndex : ErasedIndices_) {
            if (partIndex >= dataPartCount) {
                BlockConsumers_.push_back(New<TEmptyPartBlockConsumer>());
            }
        }

        // Simplify repair ranges.
        RepairRanges_ = Union(repairRanges);
    }

    TFuture<std::vector<TBlock>> Run()
    {
        return BIND(&TRepairingReaderSession::RepairBlocks, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
            .Run()
            .Apply(BIND(&TRepairingReaderSession::ReadRemainingBlocks, MakeStrong(this)))
            .Apply(BIND(&TRepairingReaderSession::BuildResult, MakeStrong(this)));
    }

private:
    const ICodec* const Codec_;
    const TPartIndexList ErasedIndices_;
    const std::vector<IChunkReaderPtr> Readers_;
    const TErasurePlacementExt PlacementExt_;
    const std::vector<int> BlockIndexes_;
    const TWorkloadDescriptor WorkloadDescriptor_;

    TParityPartSplitInfo ParityPartSplitInfo_;
    TDataBlocksPlacementInParts DataBlocksPlacementInParts_;
    std::vector<std::vector<i64>> ErasedPartBlockSizes_;
    std::vector<std::vector<i64>> RepairPartBlockSizes_;

    std::vector<TMonotonicBlocksReaderPtr> AllPartReaders_;
    std::vector<TMonotonicBlocksReaderPtr> RepairPartReaders_;
    std::vector<TPartBlockSaverPtr> PartBlockSavers_;

    std::vector<IPartBlockProducerPtr> BlockProducers_;
    std::vector<IPartBlockConsumerPtr> BlockConsumers_;

    std::vector<TPartRange> RepairRanges_;

    void RepairBlocks()
    {
        auto encoder = New<TPartEncoder>(
            Codec_,
            ErasedIndices_,
            ParityPartSplitInfo_,
            RepairRanges_,
            BlockProducers_,
            BlockConsumers_);
        encoder->Run();
    }

    void ReadRemainingBlocks()
    {
        std::vector<TFuture<void>> asyncResults;
        for (auto reader : AllPartReaders_) {
            asyncResults.push_back(reader->ReadMissingBlocksToSave());
        }
        WaitFor(Combine(asyncResults))
            .ThrowOnError();
    }

    std::vector<TBlock> BuildResult()
    {
        std::vector<TBlock> result(BlockIndexes_.size());
        int partBlockSaverIndex = 0;
        int partReaderIndex = 0;
        for (int partIndex = 0; partIndex < Codec_->GetDataPartCount(); ++partIndex) {
            auto blocksPlacementInPart = DataBlocksPlacementInParts_[partIndex];

            std::vector<TBlock> blocks;
            if (std::binary_search(ErasedIndices_.begin(), ErasedIndices_.end(), partIndex)) {
                blocks = PartBlockSavers_[partBlockSaverIndex++]->GetSavedBlocks();
            } else {
                blocks = AllPartReaders_[partReaderIndex++]->GetSavedBlocks();
            }

            for (int index = 0; index < blocksPlacementInPart.IndexesInRequest.size(); ++index) {
                result[blocksPlacementInPart.IndexesInRequest[index]] = blocks[index];
            }
        }
        return result;
    }

};

class TRepairReader
    : public TErasureChunkReaderBase
{
public:
    TRepairReader(
        ICodec* codec,
        const TPartIndexList& erasedIndices,
        const std::vector<IChunkReaderPtr>& readers)
        : TErasureChunkReaderBase(codec, readers)
        , ErasedIndices_(erasedIndices)
    { }

    virtual TFuture<std::vector<TBlock>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        const std::vector<int>& blockIndexes) override
    {
        return PreparePlacementMeta(workloadDescriptor).Apply(
            BIND([=, this_ = MakeStrong(this)] () {
                auto session = New<TRepairingReaderSession>(
                    Codec_,
                    ErasedIndices_,
                    Readers_,
                    PlacementExt_,
                    blockIndexes,
                    workloadDescriptor);
                return session->Run();
            }).AsyncVia(TDispatcher::Get()->GetReaderInvoker()));
    }

    virtual TFuture<std::vector<TBlock>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        int firstBlockIndex,
        int blockCount) override
    {
        // Implement when first needed.
        Y_UNIMPLEMENTED();
    }

private:
    const TPartIndexList ErasedIndices_;
};

IChunkReaderPtr CreateRepairingErasureReader(
    ICodec* codec,
    const TPartIndexList& erasedIndices,
    const std::vector<IChunkReaderPtr>& readers)
{
    return New<TRepairReader>(
        codec,
        erasedIndices,
        readers);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

