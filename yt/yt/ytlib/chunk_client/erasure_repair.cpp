#include "erasure_repair.h"

#include "chunk_meta_extensions.h"
#include "chunk_reader.h"
#include "chunk_writer.h"
#include "config.h"
#include "deferred_chunk_meta.h"
#include "dispatcher.h"
#include "erasure_helpers.h"
#include "private.h"
#include "chunk_reader_options.h"
#include "chunk_reader_statistics.h"
#include "yt/yt/core/misc/error.h"

#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/action_queue.h>

#include <yt/yt/core/misc/checksum.h>

#include <yt/yt/library/erasure/impl/codec.h>

#include <numeric>

namespace NYT::NChunkClient {

using namespace NErasure;
using namespace NConcurrency;
using namespace NChunkClient::NProto;
using namespace NErasureHelpers;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

//! Caching chunk reader that assumes monotonic requests for block indexes with possible overlaps.
//! Also supports functionality to save blocks with given indexes.
class TSequentialCachingBlocksReader
    : public IBlocksReader
{
public:
    TSequentialCachingBlocksReader(
        IChunkReaderPtr reader,
        const TClientChunkReadOptions& options,
        const std::vector<int>& blocksToSave = {})
        : UnderlyingReader_(reader)
        , ChunkReadOptions_(options)
        , BlocksToSave_(blocksToSave)
        , SavedBlocks_(blocksToSave.size())
    {
        for (size_t index = 0; index < blocksToSave.size(); ++index) {
            BlockIndexToBlocksToSaveIndex_[blocksToSave[index]] = index;
        }
    }

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
            ++index;
        }

        YT_VERIFY(index == CachedBlocks_.size());

        if (index < blockIndexes.size()) {
            auto blockIndexesToRequest = std::vector<int>(blockIndexes.begin() + index, blockIndexes.end());
            auto blocksFuture = UnderlyingReader_->ReadBlocks(ChunkReadOptions_, blockIndexesToRequest);
            return blocksFuture.Apply(BIND([=, this_ = MakeStrong(this)] (const std::vector<TBlock>& blocks) mutable {
                for (int index = 0; index < blockIndexesToRequest.size(); ++index) {
                    auto blockIndex = blockIndexesToRequest[index];
                    auto block = blocks[index];
                    auto it = BlockIndexToBlocksToSaveIndex_.find(blockIndex);
                    if (it != BlockIndexToBlocksToSaveIndex_.end()) {
                        SavedBlocks_[it->second] = block;
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
        THashMap<int, int> blockIndexToSavedBlocksIndex;
        int counter = 0;
        for (int index = 0; index < BlocksToSave_.size(); ++index) {
            if (!SavedBlocks_[index]) {
                indexesToRead.push_back(BlocksToSave_[index]);
                blockIndexToSavedBlocksIndex[counter++] = index;
            }
        }
        auto blocksFuture = UnderlyingReader_->ReadBlocks(ChunkReadOptions_, indexesToRead);
        return blocksFuture.Apply(BIND([=, this_ = MakeStrong(this)] (const std::vector<TBlock>& blocks) mutable {
            for (int index = 0; index < blocks.size(); ++index) {
                int savedBlocksIndex = GetOrCrash(blockIndexToSavedBlocksIndex, index);
                SavedBlocks_[savedBlocksIndex] = blocks[index];
            }
        }));
    }

    std::vector<TBlock> GetSavedBlocks() const
    {
        std::vector<TBlock> result;
        for (const auto& blockOrNull : SavedBlocks_) {
            YT_VERIFY(blockOrNull);
            result.push_back(*blockOrNull);
        }
        return result;
    }

private:
    const IChunkReaderPtr UnderlyingReader_;
    const TClientChunkReadOptions ChunkReadOptions_;
    const std::vector<int> BlocksToSave_;
    THashMap<int, int> BlockIndexToBlocksToSaveIndex_;

    std::vector<std::optional<TBlock>> SavedBlocks_;
    std::deque<std::pair<int, TBlock>> CachedBlocks_;
};

DECLARE_REFCOUNTED_TYPE(TSequentialCachingBlocksReader)
DEFINE_REFCOUNTED_TYPE(TSequentialCachingBlocksReader)

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
        const TClientChunkReadOptions& options)
        : Codec_(codec)
        , Readers_(readers)
        , Writers_(writers)
        , ErasedIndices_(erasedIndices)
        , ChunkReadOptions_(options)
    {
        YT_VERIFY(erasedIndices.size() == writers.size());
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
    const TClientChunkReadOptions ChunkReadOptions_;

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
            WaitFor(AllSucceeded(asyncResults))
                .ThrowOnError();
        }

        // Get placement extension.
        auto placementExt = WaitFor(GetPlacementMeta(
            Readers_.front(),
            ChunkReadOptions_))
            .ValueOrThrow();
        ProcessPlacementExt(placementExt);

        // Prepare erasure part readers.
        std::vector<IPartBlockProducerPtr> blockProducers;
        for (int index = 0; index < Readers_.size(); ++index) {
            auto monotonicReader = New<TSequentialCachingBlocksReader>(
                Readers_[index],
                ChunkReadOptions_);
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
        auto meta = WaitFor(reader->GetMeta(ChunkReadOptions_))
            .ValueOrThrow();
        auto deferredMeta = New<TDeferredChunkMeta>();
        deferredMeta->CopyFrom(*meta);
        deferredMeta->Finalize();

        // Validate repaired parts checksums.
        if (placementExt.part_checksums_size() != 0) {
            YT_VERIFY(placementExt.part_checksums_size() == Codec_->GetTotalPartCount());

            for (int index = 0; index < Writers_.size(); ++index) {
                TChecksum repairedPartChecksum = writerConsumers[index]->GetPartChecksum();
                TChecksum expectedPartChecksum = placementExt.part_checksums(ErasedIndices_[index]);

                YT_VERIFY(expectedPartChecksum == NullChecksum || repairedPartChecksum == expectedPartChecksum);
            }
        }

        // Close all writers.
        {
            std::vector<TFuture<void>> asyncResults;
            for (auto writer : Writers_) {
                asyncResults.push_back(writer->Close(deferredMeta));
            }
            WaitFor(AllSucceeded(asyncResults))
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
        YT_VERIFY(repairIndices);
        YT_VERIFY(repairIndices->size() == Readers_.size());

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
    const TClientChunkReadOptions& options)
{
    auto session = New<TRepairAllPartsSession>(
        codec,
        erasedIndices,
        readers,
        writers,
        options);
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
        if (LastRange_ && *LastRange_ == range) {
            return VoidFuture;
        }

        YT_VERIFY(!LastRange_ || LastRange_->End <= range.Begin);
        LastRange_ = range;

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

        return VoidFuture;
    }

    std::vector<TBlock> GetSavedBlocks()
    {
        YT_VERIFY(TotalBytes_ == SavedBytes_);
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

    std::optional<TPartRange> LastRange_;
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

class TRepairingErasureReaderSession
    : public TRefCounted
{
public:
    TRepairingErasureReaderSession(
        TChunkId chunkId,
        ICodec* codec,
        const TPartIndexList& erasedIndices,
        const std::vector<IChunkReaderAllowingRepairPtr>& readers,
        const TErasurePlacementExt& placementExt,
        const std::vector<int>& blockIndexes,
        const TClientChunkReadOptions& options,
        const IInvokerPtr& readerInvoker,
        const TLogger& logger)
        : ChunkId_(chunkId)
        , Codec_(codec)
        , ErasedIndices_(erasedIndices)
        , Readers_(readers)
        , PlacementExt_(placementExt)
        , BlockIndexes_(blockIndexes)
        , ChunkReadOptions_(options)
        , Logger(logger)
        , ParityPartSplitInfo_(GetParityPartSplitInfo(PlacementExt_))
        , DataBlocksPlacementInParts_(BuildDataBlocksPlacementInParts(BlockIndexes_, PlacementExt_))
        , ReaderInvoker_(readerInvoker)
    {
        auto repairIndices = *Codec_->GetRepairIndices(ErasedIndices_);
        YT_VERIFY(std::is_sorted(ErasedIndices_.begin(), ErasedIndices_.end()));
        YT_VERIFY(std::is_sorted(repairIndices.begin(), repairIndices.end()));

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
                auto partReader = New<TSequentialCachingBlocksReader>(
                    Readers_[readerIndex++],
                    ChunkReadOptions_,
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
                RepairPartReaders_.push_back(New<TSequentialCachingBlocksReader>(
                    Readers_[readerIndex++],
                    ChunkReadOptions_));
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
        return BIND(&TRepairingErasureReaderSession::RepairBlocks, MakeStrong(this))
            .AsyncVia(ReaderInvoker_)
            .Run()
            .Apply(BIND(&TRepairingErasureReaderSession::ReadRemainingBlocks, MakeStrong(this)))
            .Apply(BIND(&TRepairingErasureReaderSession::BuildResult, MakeStrong(this)));
    }

private:
    const TChunkId ChunkId_;
    const ICodec* const Codec_;
    const TPartIndexList ErasedIndices_;
    const std::vector<IChunkReaderAllowingRepairPtr> Readers_;
    const TErasurePlacementExt PlacementExt_;
    const std::vector<int> BlockIndexes_;
    const TClientChunkReadOptions ChunkReadOptions_;
    const TLogger Logger;

    TParityPartSplitInfo ParityPartSplitInfo_;
    TDataBlocksPlacementInParts DataBlocksPlacementInParts_;
    std::vector<std::vector<i64>> ErasedPartBlockSizes_;
    std::vector<std::vector<i64>> RepairPartBlockSizes_;

    std::vector<TSequentialCachingBlocksReaderPtr> AllPartReaders_;
    std::vector<TSequentialCachingBlocksReaderPtr> RepairPartReaders_;
    std::vector<TPartBlockSaverPtr> PartBlockSavers_;

    std::vector<IPartBlockProducerPtr> BlockProducers_;
    std::vector<IPartBlockConsumerPtr> BlockConsumers_;

    std::vector<TPartRange> RepairRanges_;

    IInvokerPtr ReaderInvoker_;

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
        WaitFor(AllSucceeded(asyncResults))
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
            bool isRepairedPart = std::binary_search(ErasedIndices_.begin(), ErasedIndices_.end(), partIndex);
            if (isRepairedPart) {
                blocks = PartBlockSavers_[partBlockSaverIndex++]->GetSavedBlocks();
            } else {
                blocks = AllPartReaders_[partReaderIndex++]->GetSavedBlocks();
            }

            for (int index = 0; index < blocksPlacementInPart.IndexesInRequest.size(); ++index) {
                int indexInRequest = blocksPlacementInPart.IndexesInRequest[index];

                if (isRepairedPart && PlacementExt_.block_checksums_size() != 0) {
                    int blockIndex = BlockIndexes_[indexInRequest];
                    YT_VERIFY(blockIndex < PlacementExt_.block_checksums_size());

                    TChecksum actualChecksum = GetChecksum(blocks[index].Data);
                    TChecksum expectedChecksum = PlacementExt_.block_checksums(blockIndex);

                    if (actualChecksum != expectedChecksum) {
                        auto error = TError("Invalid block checksum in repaired part")
                            << TErrorAttribute("chunk_id", ChunkId_)
                            << TErrorAttribute("block_index", blockIndex)
                            << TErrorAttribute("expected_checksum", expectedChecksum)
                            << TErrorAttribute("actual_checksum", actualChecksum)
                            << TErrorAttribute("recalculated_checksum", GetChecksum(blocks[index].Data));

                        YT_LOG_ALERT(error);
                        THROW_ERROR error;
                    }
                }
                result[indexInRequest] = blocks[index];
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
        TChunkId chunkId,
        ICodec* codec,
        const TPartIndexList& erasedIndices,
        const std::vector<IChunkReaderAllowingRepairPtr>& readers,
        const TLogger& logger)
        : TErasureChunkReaderBase(chunkId, codec, readers)
        , ErasedIndices_(erasedIndices)
        , ReaderInvoker_(CreateSerializedInvoker(TDispatcher::Get()->GetReaderInvoker()))
        , Logger(logger)
    { }

    virtual TFuture<std::vector<TBlock>> ReadBlocks(
        const TClientChunkReadOptions& options,
        const std::vector<int>& blockIndexes,
        std::optional<i64> /* estimatedSize */) override
    {
        // NB(psushin): do not use estimated size for throttling here, repair requires much more traffic than estimated.
        // When reading erasure chunks we fallback to post-throttling.
        return PreparePlacementMeta(options).Apply(
            BIND([=, this_ = MakeStrong(this)] {
                auto session = New<TRepairingErasureReaderSession>(
                    GetChunkId(),
                    Codec_,
                    ErasedIndices_,
                    Readers_,
                    PlacementExt_,
                    blockIndexes,
                    options,
                    ReaderInvoker_,
                    Logger);
                return session->Run();
            }).AsyncVia(ReaderInvoker_));
    }

    virtual TFuture<std::vector<TBlock>> ReadBlocks(
        const TClientChunkReadOptions& options,
        int firstBlockIndex,
        int blockCount,
        std::optional<i64> /* estimatedSize */) override
    {
        // Implement when first needed.
        YT_UNIMPLEMENTED();
    }

    virtual TInstant GetLastFailureTime() const override
    {
        auto lastFailureTime = TInstant();
        for (const auto& reader : Readers_) {
            lastFailureTime = std::max(lastFailureTime, reader->GetLastFailureTime());
        }
        return lastFailureTime;
    }

private:
    const TPartIndexList ErasedIndices_;
    IInvokerPtr ReaderInvoker_;
    const TLogger Logger;
};

IChunkReaderPtr CreateRepairingErasureReader(
    TChunkId chunkId,
    ICodec* codec,
    const TPartIndexList& erasedIndices,
    const std::vector<IChunkReaderAllowingRepairPtr>& readers,
    const NLogging::TLogger& logger)
{
    return New<TRepairReader>(
        chunkId,
        codec,
        erasedIndices,
        readers,
        logger);
}

////////////////////////////////////////////////////////////////////////////////

TFuture<void> RepairErasedParts(
    NErasure::ICodec* codec,
    const NErasure::TPartIndexList& erasedIndices,
    const std::vector<IChunkReaderAllowingRepairPtr>& readers,
    const std::vector<IChunkWriterPtr>& writers,
    const TClientChunkReadOptions& options)
{
    std::vector<IChunkReaderPtr> simpleReaders(readers.begin(), readers.end());
    return RepairErasedParts(codec, erasedIndices, simpleReaders, writers, options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient

