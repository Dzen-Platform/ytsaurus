#include "block_fetcher.h"

#include "private.h"
#include "block_cache.h"
#include "config.h"
#include "dispatcher.h"
#include "chunk_reader_memory_manager.h"
#include "chunk_reader_statistics.h"

#include <yt/yt/ytlib/memory_trackers/block_tracker.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/core/compression/codec.h>

#include <yt/yt/core/concurrency/action_queue.h>

#include <yt/yt/core/rpc/dispatcher.h>

#include <yt/yt/core/profiling/timing.h>

namespace NYT::NChunkClient {

using namespace NConcurrency;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

TBlockFetcher::TBlockFetcher(
    TBlockFetcherConfigPtr config,
    std::vector<TBlockInfo> blockInfos,
    TChunkReaderMemoryManagerPtr memoryManager,
    std::vector<IChunkReaderPtr> chunkReaders,
    IBlockCachePtr blockCache,
    NCompression::ECodec codecId,
    double compressionRatio,
    const TClientChunkReadOptions& chunkReadOptions)
    : Config_(std::move(config))
    , BlockInfos_(std::move(blockInfos))
    , ChunkReaders_(std::move(chunkReaders))
    , BlockCache_(std::move(blockCache))
    , CompressionInvoker_(
        codecId == NCompression::ECodec::None
        ? nullptr
        : GetCompressionInvoker(chunkReadOptions.WorkloadDescriptor))
    , ReaderInvoker_(CreateSerializedInvoker(TDispatcher::Get()->GetReaderInvoker()))
    , CompressionRatio_(compressionRatio)
    , MemoryManager_(std::move(memoryManager))
    , Codec_(NCompression::GetCodec(codecId))
    , ChunkReadOptions_(chunkReadOptions)
    , Logger(ChunkClientLogger)
{
    YT_VERIFY(!ChunkReaders_.empty());
    YT_VERIFY(BlockCache_);
    YT_VERIFY(!BlockInfos_.empty());

    if (ChunkReadOptions_.ReadSessionId) {
        Logger.AddTag("ReadSessionId: %v", ChunkReadOptions_.ReadSessionId);
    }

    auto getBlockDescriptor = [&] (const TBlockInfo& blockInfo) {
        return TBlockDescriptor{
            .ReaderIndex = blockInfo.ReaderIndex,
            .BlockIndex = blockInfo.BlockIndex
        };
    };

    std::sort(
        BlockInfos_.begin(),
        BlockInfos_.end(),
        [&] (const TBlockInfo& lhs, const TBlockInfo& rhs) {
            if (lhs.Priority != rhs.Priority) {
                return lhs.Priority < rhs.Priority;
            } else {
                return
                    std::make_pair(lhs.ReaderIndex, lhs.BlockIndex) <
                    std::make_pair(rhs.ReaderIndex, rhs.BlockIndex);
            }
        });

    int windowSize = 1;
    i64 totalRemainingSize = 0;
    for (int index = 0; index + 1 < std::ssize(BlockInfos_); ++index) {
        if (getBlockDescriptor(BlockInfos_[index]) != getBlockDescriptor(BlockInfos_[index + 1])) {
            ++windowSize;
        }
        totalRemainingSize += BlockInfos_[index].UncompressedDataSize;
    }
    totalRemainingSize += BlockInfos_.back().UncompressedDataSize;

    Window_ = std::make_unique<TWindowSlot[]>(windowSize);
    TotalRemainingFetches_ = BlockInfos_.size();
    TotalRemainingSize_ = totalRemainingSize;

    // We consider contiguous segments consisting of the same block and store them
    // in the BlockIndexToWindowIndex hashmap.
    // [leftIndex, rightIndex) is a half-interval containing all blocks
    // equal to BlockInfos[leftIndex].
    // We also explicitly unique the elements of BlockInfos_.
    std::vector<TBlockDescriptor> blockDescriptors;
    blockDescriptors.reserve(windowSize);
    i64 totalBlockUncompressedSize = 0;
    int windowIndex = 0;
    for (int leftIndex = 0, rightIndex = 0; leftIndex != std::ssize(BlockInfos_); leftIndex = rightIndex) {
        auto& currentBlock = BlockInfos_[leftIndex];
        while (
            rightIndex != std::ssize(BlockInfos_) &&
            getBlockDescriptor(BlockInfos_[rightIndex]) == getBlockDescriptor(currentBlock))
        {
            ++rightIndex;
        }

        auto currentBlockDescriptor = std::make_pair(currentBlock.ReaderIndex, currentBlock.BlockIndex);
        if (BlockDescriptorToWindowIndex_.contains(currentBlockDescriptor)) {
            auto windowIndex = GetOrCrash(BlockDescriptorToWindowIndex_, currentBlockDescriptor);
            Window_[windowIndex].RemainingFetches += rightIndex - leftIndex;
        } else {
            BlockDescriptorToWindowIndex_[currentBlockDescriptor] = windowIndex;
            Window_[windowIndex].RemainingFetches = rightIndex - leftIndex;
            blockDescriptors.push_back(getBlockDescriptor(currentBlock));
            totalBlockUncompressedSize += currentBlock.UncompressedDataSize;
            if (windowIndex != leftIndex) {
                BlockInfos_[windowIndex] = std::move(currentBlock);
            }
            ++windowIndex;
        }
    }
    YT_VERIFY(windowIndex == windowSize);

    // Now Window_ and BlockInfos_ correspond to each other.
    BlockInfos_.resize(windowSize);

    MemoryManager_->SetTotalSize(totalBlockUncompressedSize + Config_->WindowSize);
    MemoryManager_->SetPrefetchMemorySize(std::min(Config_->WindowSize, totalRemainingSize));

    std::vector<TChunkId> chunkIds;
    chunkIds.reserve(ChunkReaders_.size());
    for (const auto& chunkReader : ChunkReaders_) {
        chunkIds.push_back(chunkReader->GetChunkId());
    }

    TStringBuilder builder;
    bool first = true;
    for (const auto& blockDescriptor : blockDescriptors) {
        builder.AppendString(first ? "[" : ", ");
        first = false;

        auto chunkId = ChunkReaders_[blockDescriptor.ReaderIndex]->GetChunkId();
        builder.AppendFormat("%v:%v", chunkId, blockDescriptor.BlockIndex);
    }
    builder.AppendChar(']');

    YT_LOG_DEBUG("Creating block fetcher (Blocks: %v)", builder.Flush());

    YT_VERIFY(totalRemainingSize > 0);

    FetchNextGroupMemoryFuture_ =
        MemoryManager_->AsyncAcquire(
            std::min(totalRemainingSize, Config_->GroupSize));
    FetchNextGroupMemoryFuture_.Subscribe(BIND(
        &TBlockFetcher::FetchNextGroup,
            MakeWeak(this))
        .Via(ReaderInvoker_));
}

TBlockFetcher::~TBlockFetcher()
{
    MemoryManager_->Finalize();
}

bool TBlockFetcher::HasMoreBlocks() const
{
    return TotalRemainingFetches_ > 0;
}

i64 TBlockFetcher::GetBlockSize(int readerIndex, int blockIndex) const
{
    auto blockDescriptor = std::make_pair(readerIndex, blockIndex);
    int windowIndex = GetOrCrash(BlockDescriptorToWindowIndex_, blockDescriptor);
    return BlockInfos_[windowIndex].UncompressedDataSize;
}

i64 TBlockFetcher::GetBlockSize(int blockIndex) const
{
    YT_VERIFY(std::ssize(ChunkReaders_) == 1);
    return GetBlockSize(/*readerIndex*/ 0, blockIndex);
}

TFuture<TBlock> TBlockFetcher::FetchBlock(int readerIndex, int blockIndex)
{
    YT_VERIFY(HasMoreBlocks());

    auto blockDescriptor = std::make_pair(readerIndex, blockIndex);
    int windowIndex = GetOrCrash(BlockDescriptorToWindowIndex_, blockDescriptor);
    auto& windowSlot = Window_[windowIndex];

    auto blockPromise = GetBlockPromise(windowSlot);

    YT_VERIFY(windowSlot.RemainingFetches > 0);
    if (!windowSlot.FetchStarted.test_and_set()) {
        auto chunkId = ChunkReaders_[readerIndex]->GetChunkId();

        YT_LOG_DEBUG("Fetching block out of turn "
            "(ChunkId: %v, BlockIndex: %v, WindowIndex: %v)",
            chunkId,
            blockIndex,
            windowIndex);

        windowSlot.MemoryUsageGuard = MemoryManager_->Acquire(
            BlockInfos_[windowIndex].UncompressedDataSize);

        TBlockId blockId(chunkId, blockIndex);

        auto cachedBlock = Config_->UseUncompressedBlockCache
            ? BlockCache_->FindBlock(blockId, EBlockType::UncompressedData).Block
            : TBlock();
        if (cachedBlock) {
            ChunkReadOptions_.ChunkReaderStatistics->DataBytesReadFromCache.fetch_add(
                cachedBlock.Size(),
                std::memory_order_relaxed);

            cachedBlock = AttachCategory(
                std::move(cachedBlock),
                ChunkReadOptions_.BlockTracker,
                ChunkReadOptions_.MemoryCategory);

            TRef ref = cachedBlock.Data;
            windowSlot.MemoryUsageGuard->CaptureBlock(std::move(cachedBlock.Data));

            cachedBlock = TBlock(TSharedRef(ref, std::move(windowSlot.MemoryUsageGuard)));

            blockPromise.Set(std::move(cachedBlock));
            TotalRemainingSize_ -= BlockInfos_[windowIndex].UncompressedDataSize;
        } else {
            TBlockDescriptor blockDescriptor{
                .ReaderIndex = readerIndex,
                .BlockIndex = blockIndex
            };
            ReaderInvoker_->Invoke(BIND(
                &TBlockFetcher::RequestBlocks,
                MakeWeak(this),
                std::vector{windowIndex},
                std::vector{blockDescriptor},
                BlockInfos_[windowIndex].UncompressedDataSize));
        }
    }

    auto blockFuture = blockPromise.ToFuture();
    if (--windowSlot.RemainingFetches == 0 && blockFuture.IsSet()) {
        ReaderInvoker_->Invoke(
            BIND(&TBlockFetcher::ReleaseBlocks,
                MakeWeak(this),
                std::vector{windowIndex}));
    }

    --TotalRemainingFetches_;

    return blockFuture;
}

TFuture<TBlock> TBlockFetcher::FetchBlock(int blockIndex)
{
    YT_VERIFY(std::ssize(ChunkReaders_) == 1);
    return FetchBlock(/*readerIndex*/ 0, blockIndex);
}

void TBlockFetcher::DecompressBlocks(
    std::vector<int> windowIndexes,
    std::vector<TBlock> compressedBlocks)
{
    YT_VERIFY(windowIndexes.size() == compressedBlocks.size());

    std::vector<int> windowIndexesToRelease;
    for (int i = 0; i < std::ssize(compressedBlocks); ++i) {
        auto& compressedBlock = compressedBlocks[i];
        auto compressedBlockSize = compressedBlock.Size();
        int windowIndex = windowIndexes[i];
        const auto& blockInfo = BlockInfos_[windowIndex];
        auto readerIndex = blockInfo.ReaderIndex;
        auto blockIndex = blockInfo.BlockIndex;

        auto chunkId = ChunkReaders_[readerIndex]->GetChunkId();
        TBlockId blockId(chunkId, blockIndex);

        TSharedRef uncompressedBlock;
        if (Codec_->GetId() == NCompression::ECodec::None) {
            uncompressedBlock = std::move(compressedBlock.Data);
        } else {
            YT_LOG_DEBUG("Started decompressing block "
                "(ChunkId: %v, BlockIndex: %v, WindowIndex: %v, Codec: %v)",
                chunkId,
                blockIndex,
                windowIndex,
                Codec_->GetId());

            {
                TWallTimer timer;
                uncompressedBlock = Codec_->Decompress(compressedBlock.Data);
                DecompressionTime_ += timer.GetElapsedValue();
                YT_VERIFY(std::ssize(uncompressedBlock) == blockInfo.UncompressedDataSize);
            }

            YT_LOG_DEBUG("Finished decompressing block "
                "(ChunkId: %v, BlockIndex: %v, WindowIndex: %v, CompressedSize: %v, UncompressedSize: %v, Codec: %v)",
                chunkId,
                blockIndex,
                windowIndex,
                compressedBlock.Size(),
                uncompressedBlock.Size(),
                Codec_->GetId());
        }

        if (Config_->UseUncompressedBlockCache) {
            BlockCache_->PutBlock(
                blockId,
                EBlockType::UncompressedData,
                TBlock(uncompressedBlock));
        }

        UncompressedDataSize_ += uncompressedBlock.Size();
        CompressedDataSize_ += compressedBlockSize;

        auto& windowSlot = Window_[windowIndex];

        uncompressedBlock = AttachCategory(
            std::move(uncompressedBlock),
            ChunkReadOptions_.BlockTracker,
            ChunkReadOptions_.MemoryCategory);

        TRef ref = uncompressedBlock;
        windowSlot.MemoryUsageGuard->CaptureBlock(std::move(uncompressedBlock));

        uncompressedBlock = TSharedRef(
            ref,
            std::move(windowSlot.MemoryUsageGuard));

        GetBlockPromise(windowSlot).Set(TBlock(std::move(uncompressedBlock)));
        if (windowSlot.RemainingFetches == 0) {
            windowIndexesToRelease.push_back(windowIndex);
        }
    }

    if (!windowIndexesToRelease.empty()) {
        ReaderInvoker_->Invoke(
            BIND(&TBlockFetcher::ReleaseBlocks,
                MakeWeak(this),
                std::move(windowIndexesToRelease)));
    }
}

void TBlockFetcher::FetchNextGroup(const TErrorOr<TMemoryUsageGuardPtr>& memoryUsageGuardOrError)
{
    if (!memoryUsageGuardOrError.IsOK()) {
        YT_LOG_INFO(memoryUsageGuardOrError,
            "Failed to acquire memory in chunk reader memory manager");
        return;
    }

    const auto& memoryUsageGuard = memoryUsageGuardOrError.Value();
    auto* underlyingGuard = memoryUsageGuard->GetGuard();

    std::vector<int> windowIndexes;
    std::vector<TBlockDescriptor> blockDescriptors;
    i64 uncompressedSize = 0;
    i64 availableSlots = underlyingGuard->GetSlots();
    while (FirstUnfetchedWindowIndex_ < std::ssize(BlockInfos_)) {
        const auto& blockInfo = BlockInfos_[FirstUnfetchedWindowIndex_];
        auto readerIndex = blockInfo.ReaderIndex;
        auto blockIndex = blockInfo.BlockIndex;
        auto chunkId = ChunkReaders_[readerIndex]->GetChunkId();

        if (windowIndexes.empty() || uncompressedSize + blockInfo.UncompressedDataSize <= availableSlots) {
            if (Window_[FirstUnfetchedWindowIndex_].FetchStarted.test_and_set()) {
                // This block has been already requested out of order.
                YT_LOG_DEBUG("Skipping out of turn block (ChunkId: %v, BlockIndex: %v, WindowIndex: %v)",
                    chunkId,
                    blockIndex,
                    FirstUnfetchedWindowIndex_);
                ++FirstUnfetchedWindowIndex_;
                continue;
            }

            auto transferred = underlyingGuard->TransferSlots(
                std::min(
                    static_cast<i64>(blockInfo.UncompressedDataSize),
                    underlyingGuard->GetSlots()));
            Window_[FirstUnfetchedWindowIndex_].MemoryUsageGuard = New<TMemoryUsageGuard>(
                std::move(transferred),
                memoryUsageGuard->GetMemoryManager());

            TBlockId blockId(chunkId, blockIndex);
            auto cachedBlock = Config_->UseUncompressedBlockCache
                ? BlockCache_->FindBlock(blockId, EBlockType::UncompressedData).Block
                : TBlock();
            if (cachedBlock) {
                ChunkReadOptions_.ChunkReaderStatistics->DataBytesReadFromCache.fetch_add(
                    cachedBlock.Size(),
                    std::memory_order_relaxed);

                auto& windowSlot = Window_[FirstUnfetchedWindowIndex_];

                cachedBlock = AttachCategory(
                    std::move(cachedBlock),
                    ChunkReadOptions_.BlockTracker,
                    ChunkReadOptions_.MemoryCategory);

                TRef ref = cachedBlock.Data;
                windowSlot.MemoryUsageGuard->CaptureBlock(std::move(cachedBlock.Data));
                cachedBlock = TBlock(TSharedRef(
                    ref,
                    std::move(windowSlot.MemoryUsageGuard)));

                GetBlockPromise(windowSlot).Set(std::move(cachedBlock));

                TotalRemainingSize_ -= blockInfo.UncompressedDataSize;
            } else {
                uncompressedSize += blockInfo.UncompressedDataSize;
                windowIndexes.push_back(FirstUnfetchedWindowIndex_);
                blockDescriptors.push_back(TBlockDescriptor{
                    .ReaderIndex = readerIndex,
                    .BlockIndex = blockIndex
                });
            }
        } else {
            break;
        }

        ++FirstUnfetchedWindowIndex_;
    }

    if (windowIndexes.empty()) {
        FetchingCompleted_ = true;
        MemoryManager_->Finalize();
        return;
    }

    if (TotalRemainingSize_ > 0) {
        auto nextGroupSize = std::min<i64>(TotalRemainingSize_, Config_->GroupSize);
        MemoryManager_->SetPrefetchMemorySize(nextGroupSize);
        FetchNextGroupMemoryFuture_ = MemoryManager_->AsyncAcquire(nextGroupSize);
        FetchNextGroupMemoryFuture_.Subscribe(BIND(
            &TBlockFetcher::FetchNextGroup,
                MakeWeak(this))
            .Via(ReaderInvoker_));
    }

    RequestBlocks(
        std::move(windowIndexes),
        std::move(blockDescriptors),
        uncompressedSize);
}

void TBlockFetcher::MarkFailedBlocks(const std::vector<int>& windowIndexes, const TError& error)
{
    for (auto index : windowIndexes) {
        GetBlockPromise(Window_[index]).Set(error);
    }
}

void TBlockFetcher::ReleaseBlocks(const std::vector<int>& windowIndexes)
{
    YT_LOG_DEBUG("Releasing blocks (WindowIndexes: %v)",
        MakeShrunkFormattableView(windowIndexes, TDefaultFormatter(), 3));

    for (auto index : windowIndexes) {
        ResetBlockPromise(Window_[index]);
    }
}

TPromise<TBlock> TBlockFetcher::GetBlockPromise(TWindowSlot& windowSlot)
{
    auto guard = Guard(windowSlot.BlockPromiseLock);
    if (!windowSlot.BlockPromise) {
        windowSlot.BlockPromise = NewPromise<TBlock>();
    }
    return windowSlot.BlockPromise;
}

void TBlockFetcher::ResetBlockPromise(TWindowSlot& windowSlot)
{
    TPromise<TBlock> promise;
    {
        auto guard = Guard(windowSlot.BlockPromiseLock);
        promise = std::move(windowSlot.BlockPromise);
    }
}

void TBlockFetcher::RequestBlocks(
    std::vector<int> windowIndexes,
    std::vector<TBlockDescriptor> blockDescriptors,
    i64 uncompressedSize)
{
    YT_VERIFY(windowIndexes.size() == blockDescriptors.size());

    TotalRemainingSize_ -= uncompressedSize;

    THashMap<int, std::vector<int>> readerIndexToBlockIndices;
    THashMap<int, std::vector<int>> readerIndexToWindowIndices;
    for (int index = 0; index < std::ssize(blockDescriptors); ++index) {
        const auto& blockDescriptor = blockDescriptors[index];
        auto readerIndex = blockDescriptor.ReaderIndex;
        readerIndexToBlockIndices[readerIndex].push_back(blockDescriptor.BlockIndex);
        readerIndexToWindowIndices[readerIndex].push_back(windowIndexes[index]);
    }

    for (auto& [readerIndex, blockIndices] : readerIndexToBlockIndices) {
        const auto& chunkReader = ChunkReaders_[readerIndex];

        YT_LOG_DEBUG("Requesting block group (ChunkId: %v, Blocks: %v, UncompressedSize: %v)",
            chunkReader->GetChunkId(),
            MakeShrunkFormattableView(blockIndices, TDefaultFormatter(), 3),
            uncompressedSize);

        auto future = chunkReader->ReadBlocks(
            ChunkReadOptions_,
            blockIndices,
            static_cast<i64>(uncompressedSize * CompressionRatio_));

        // NB: Handling |OnGotBlocks| in an arbitrary thread seems OK.
        future.SubscribeUnique(
            BIND(
                &TBlockFetcher::OnGotBlocks,
                MakeWeak(this),
                readerIndex,
                Passed(std::move(readerIndexToWindowIndices[readerIndex])),
                Passed(std::move(blockIndices))));
    }
}

void TBlockFetcher::OnGotBlocks(
    int readerIndex,
    std::vector<int> windowIndexes,
    std::vector<int> blockIndexes,
    TErrorOr<std::vector<TBlock>>&& blocksOrError)
{
    if (!blocksOrError.IsOK()) {
        MarkFailedBlocks(windowIndexes, blocksOrError);
        return;
    }

    auto blocks = std::move(blocksOrError.Value());

    for (auto& block: blocks) {
        block = AttachCategory(
            std::move(block),
            ChunkReadOptions_.BlockTracker,
            ChunkReadOptions_.MemoryCategory);
    }

    auto chunkId = ChunkReaders_[readerIndex]->GetChunkId();
    YT_LOG_DEBUG("Got block group (ChunkId: %v, Blocks: %v)",
        chunkId,
        MakeShrunkFormattableView(blockIndexes, TDefaultFormatter(), 3));

    if (Codec_->GetId() == NCompression::ECodec::None) {
        DecompressBlocks(
            std::move(windowIndexes),
            std::move(blocks));
    } else {
        CompressionInvoker_->Invoke(BIND(
            &TBlockFetcher::DecompressBlocks,
            MakeWeak(this),
            Passed(std::move(windowIndexes)),
            Passed(std::move(blocks))));
    }
}

bool TBlockFetcher::IsFetchingCompleted() const
{
    return FetchingCompleted_;
}

i64 TBlockFetcher::GetUncompressedDataSize() const
{
    return UncompressedDataSize_;
}

i64 TBlockFetcher::GetCompressedDataSize() const
{
    return CompressedDataSize_;
}

TCodecDuration TBlockFetcher::GetDecompressionTime() const
{
    return TCodecDuration{
        Codec_->GetId(),
        NProfiling::ValueToDuration(DecompressionTime_)
    };
}

////////////////////////////////////////////////////////////////////////////////

TSequentialBlockFetcher::TSequentialBlockFetcher(
    TBlockFetcherConfigPtr config,
    std::vector<TBlockInfo> blockInfos,
    TChunkReaderMemoryManagerPtr memoryManager,
    std::vector<IChunkReaderPtr> chunkReaders,
    IBlockCachePtr blockCache,
    NCompression::ECodec codecId,
    double compressionRatio,
    const TClientChunkReadOptions& chunkReadOptions)
    : TBlockFetcher(
        std::move(config),
        blockInfos,
        std::move(memoryManager),
        std::move(chunkReaders),
        std::move(blockCache),
        codecId,
        compressionRatio,
        chunkReadOptions)
    , OriginalOrderBlockInfos_(std::move(blockInfos))
{ }

TFuture<TBlock> TSequentialBlockFetcher::FetchNextBlock()
{
    YT_VERIFY(CurrentIndex_ < std::ssize(OriginalOrderBlockInfos_));
    const auto& blockInfo = OriginalOrderBlockInfos_[CurrentIndex_++];
    return FetchBlock(blockInfo.ReaderIndex, blockInfo.BlockIndex);
}

i64 TSequentialBlockFetcher::GetNextBlockSize() const
{
    YT_VERIFY(CurrentIndex_ < std::ssize(OriginalOrderBlockInfos_));
    return OriginalOrderBlockInfos_[CurrentIndex_].UncompressedDataSize;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
