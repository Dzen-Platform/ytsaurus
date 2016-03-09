#include "erasure_reader.h"
#include "block_cache.h"
#include "chunk_meta_extensions.h"
#include "chunk_reader.h"
#include "chunk_replica.h"
#include "chunk_writer.h"
#include "config.h"
#include "dispatcher.h"
#include "replication_reader.h"

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/config.h>
#include <yt/ytlib/api/connection.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/erasure/codec.h>
#include <yt/core/erasure/helpers.h>

#include <numeric>

namespace NYT {
namespace NChunkClient {

using namespace NApi;
using namespace NErasure;
using namespace NConcurrency;
using namespace NChunkClient::NProto;

using NYT::FromProto;

///////////////////////////////////////////////////////////////////////////////

namespace {

TFuture<TChunkMeta> GetPlacementMeta(
    const IChunkReaderPtr& reader,
    const TWorkloadDescriptor& workloadDescriptor)
{
    return reader->GetMeta(
        workloadDescriptor,
        Null,
        std::vector<int>{
            TProtoExtensionTag<TErasurePlacementExt>::Value
        });
}

} // namespace

///////////////////////////////////////////////////////////////////////////////
// Non-repairing reader

class TNonReparingReaderSession
    : public TRefCounted
{
public:
    TNonReparingReaderSession(
        const std::vector<IChunkReaderPtr>& readers,
        const std::vector<TPartInfo>& partInfos,
        const std::vector<int>& blockIndexes,
        const TWorkloadDescriptor& workloadDescriptor)
        : Readers_(readers)
        , PartInfos_(partInfos)
        , BlockIndexes_(blockIndexes)
        , WorkloadDescriptor_(workloadDescriptor)
    { }


    TFuture<std::vector<TSharedRef>> Run()
    {
        // For each reader we find blocks to read and their initial indices.
        std::vector<
            std::pair<
                std::vector<int>, // indices of blocks in the part
                TPartIndexList    // indices of blocks in the requested blockIndexes
            > > blockLocations(Readers_.size());

        // Fill BlockLocations_ using information about blocks in parts
        int initialPosition = 0;
        for (int blockIndex : BlockIndexes_) {
            YCHECK(blockIndex >= 0);

            // Searching for the part of a given block.
            auto it = std::upper_bound(PartInfos_.begin(), PartInfos_.end(), blockIndex, TPartComparer());
            YCHECK(it != PartInfos_.begin());
            do {
                --it;
            } while (it != PartInfos_.begin() && (it->first_block_index() > blockIndex || it->block_sizes().size() == 0));

            YCHECK(it != PartInfos_.end());
            int readerIndex = it - PartInfos_.begin();

            YCHECK(blockIndex >= it->first_block_index());
            int blockInPartIndex = blockIndex - it->first_block_index();

            YCHECK(blockInPartIndex < it->block_sizes().size());
            blockLocations[readerIndex].first.push_back(blockInPartIndex);
            blockLocations[readerIndex].second.push_back(initialPosition++);
        }

        std::vector<TFuture<std::vector<TSharedRef>>> readBlocksFutures;
        for (int readerIndex = 0; readerIndex < Readers_.size(); ++readerIndex) {
            auto reader = Readers_[readerIndex];
            readBlocksFutures.push_back(reader->ReadBlocks(WorkloadDescriptor_, blockLocations[readerIndex].first));
        }

        return Combine(readBlocksFutures).Apply(
            BIND([=, this_ = MakeStrong(this)] (std::vector<std::vector<TSharedRef>> readBlocks) {
                std::vector<TSharedRef> resultBlocks(BlockIndexes_.size());
                for (int readerIndex = 0; readerIndex < readBlocks.size(); ++readerIndex) {
                    for (int blockIndex = 0; blockIndex < readBlocks[readerIndex].size(); ++blockIndex) {
                        resultBlocks[blockLocations[readerIndex].second[blockIndex]] = readBlocks[readerIndex][blockIndex];
                    }
                }
                return resultBlocks;
            }));
    }

private:
    struct TPartComparer
    {
        bool operator()(int position, const TPartInfo& info) const
        {
            return position < info.first_block_index();
        }
    };

    const std::vector<IChunkReaderPtr> Readers_;
    const std::vector<TPartInfo> PartInfos_;
    const std::vector<int> BlockIndexes_;
    const TWorkloadDescriptor WorkloadDescriptor_;
};

///////////////////////////////////////////////////////////////////////////////

class TNonRepairingReader
    : public IChunkReader
{
public:
    explicit TNonRepairingReader(const std::vector<IChunkReaderPtr>& readers)
        : Readers_(readers)
    {
        YCHECK(!Readers_.empty());
    }

    virtual TFuture<std::vector<TSharedRef>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        const std::vector<int>& blockIndexes) override
    {
        return PreparePartInfos(workloadDescriptor).Apply(
            BIND([=, this_ = MakeStrong(this)] () {
                auto session = New<TNonReparingReaderSession>(
                    Readers_,
                    PartInfos_,
                    blockIndexes,
                    workloadDescriptor);
                return session->Run();
            }).AsyncVia(TDispatcher::Get()->GetReaderInvoker()));
    }

    virtual TFuture<std::vector<TSharedRef>> ReadBlocks(
        const TWorkloadDescriptor& workloadDescriptor,
        int firstBlockIndex,
        int blockCount) override
    {
        // TODO(babenko): implement when first needed
        YUNIMPLEMENTED();
    }

    virtual TFuture<TChunkMeta> GetMeta(
        const TWorkloadDescriptor& workloadDescriptor,
        const TNullable<int>& partitionTag,
        const TNullable<std::vector<int>>& extensionTags) override
    {
        // TODO(ignat): check that no storage-layer extensions are being requested
        YCHECK(!partitionTag);
        return Readers_.front()->GetMeta(workloadDescriptor, partitionTag, extensionTags);
    }

    virtual TChunkId GetChunkId() const override
    {
        return Readers_.front()->GetChunkId();
    }

private:
    const std::vector<IChunkReaderPtr> Readers_;

    std::vector<TPartInfo> PartInfos_;


    TFuture<void> PreparePartInfos(const TWorkloadDescriptor& workloadDescriptor)
    {
        if (!PartInfos_.empty()) {
            return MakePromise(TError());
        }

        return GetPlacementMeta(this, workloadDescriptor).Apply(
            BIND(&TNonRepairingReader::OnGotPlacementMeta, MakeStrong(this))
                .AsyncVia(TDispatcher::Get()->GetReaderInvoker()));
    }

    void OnGotPlacementMeta(const TChunkMeta& meta)
    {
        auto extension = GetProtoExtension<TErasurePlacementExt>(meta.extensions());
        PartInfos_ = FromProto<std::vector<TPartInfo>>(extension.part_infos());

        // Check that part infos are correct.
        YCHECK(PartInfos_.front().first_block_index() == 0);
        for (int i = 0; i + 1 < PartInfos_.size(); ++i) {
            YCHECK(PartInfos_[i].first_block_index() + PartInfos_[i].block_sizes().size() == PartInfos_[i + 1].first_block_index());
        }
    }

};

IChunkReaderPtr CreateNonRepairingErasureReader(
    const std::vector<IChunkReaderPtr>& dataBlockReaders)
{
    return New<TNonRepairingReader>(dataBlockReaders);
}

///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Repairing readers

//! Asynchronously reads data by window of size windowSize.
//! It is guaranteed that each original block will be read only once.
class TWindowReader
    : public TRefCounted
{
public:
    TWindowReader(
        IChunkReaderPtr reader,
        const std::vector<i64>& blockSizes,
        const TWorkloadDescriptor& workloadDescriptor)
        : Reader_(reader)
        , BlockSizes_(blockSizes)
        , BlockCount_(blockSizes.size())
        , WorkloadDescriptor_(workloadDescriptor)
    { }

    TFuture<TSharedRef> Read(i64 windowSize)
    {
        YCHECK(WindowSize_ == -1);

        WindowSize_ = windowSize;
        auto promise = NewPromise<TSharedRef>();

        Continue(promise);

        return promise;
    }

private:
    const IChunkReaderPtr Reader_;
    const std::vector<i64> BlockSizes_;
    const int BlockCount_;
    const TWorkloadDescriptor WorkloadDescriptor_;

    //! Window size requested by the currently served #Read.
    i64 WindowSize_ = -1;

    //! Blocks already fetched via the underlying reader.
    std::deque<TSharedRef> Blocks_;

    // Current number of read blocks.
    int BlockIndex_ = 0;

    //! Total blocks data size.
    i64 BlocksDataSize_ = 0;

    //! Total size of data returned from |Read|
    i64 BuildDataSize_ = 0;

    //! Offset of used data in the first block.
    i64 FirstBlockOffset_ = 0;


    void Continue(TPromise<TSharedRef> promise)
    {
        if (BlockIndex_ >= BlockCount_ ||  BlocksDataSize_ >= BuildDataSize_ + WindowSize_) {
            Complete(promise, BuildWindow(WindowSize_));
            return;
        }

        std::vector<int> blockIndexes;

        int requestBlockCount = 0;
        i64 requestedSize = 0;
        while (BlockIndex_ + requestBlockCount < BlockCount_ &&  BlocksDataSize_ + requestedSize < BuildDataSize_ + WindowSize_) {
            requestedSize += BlockSizes_[BlockIndex_ + requestBlockCount];
            blockIndexes.push_back(BlockIndex_ + requestBlockCount);
            requestBlockCount++;
        }

        Reader_->ReadBlocks(WorkloadDescriptor_, blockIndexes).Subscribe(
            BIND(&TWindowReader::OnBlockRead, MakeStrong(this), promise)
                .Via(TDispatcher::Get()->GetReaderInvoker()));
    }

    void Complete(TPromise<TSharedRef> promise, const TErrorOr<TSharedRef>& blockOrError)
    {
        WindowSize_ = -1;
        promise.Set(blockOrError);
    }

    void OnBlockRead(TPromise<TSharedRef> promise, const TErrorOr<std::vector<TSharedRef>>& blocksOrError)
    {
        if (!blocksOrError.IsOK()) {
            Complete(promise, TError(blocksOrError));
            return;
        }

        const auto& blocks = blocksOrError.Value();
        for (const auto& block : blocks) {
            BlockIndex_ += 1;
            Blocks_.push_back(block);
            BlocksDataSize_ += block.Size();
        }

        Continue(promise);
    }

    TSharedRef BuildWindow(i64 windowSize)
    {
        // Allocate the resulting window filling it with zeros (used as padding).
        struct TRepairWindowTag { };
        auto result = TSharedMutableRef::Allocate<TRepairWindowTag>(windowSize);

        i64 resultPosition = 0;
        while (!Blocks_.empty()) {
            auto block = Blocks_.front();

            // Begin and end inside of current block

            i64 beginIndex = FirstBlockOffset_;
            i64 endIndex = std::min(beginIndex + windowSize - resultPosition, (i64)block.Size());
            i64 size = endIndex - beginIndex;

            std::copy(block.Begin() + beginIndex, block.Begin() + endIndex, result.Begin() + resultPosition);
            resultPosition += size;

            FirstBlockOffset_ += size;
            if (endIndex == block.Size()) {
                Blocks_.pop_front();
                FirstBlockOffset_ = 0;
            } else {
                break;
            }
        }
        BuildDataSize_ += windowSize;

        return result;
    }

};

typedef TIntrusivePtr<TWindowReader> TWindowReaderPtr;

///////////////////////////////////////////////////////////////////////////////

//! Does the job opposite to that of TWindowReader.
//! Consumes windows and returns blocks of the current part that
//! can be reconstructed.
class TRepairPartReader
{
public:
    explicit TRepairPartReader(const std::vector<i64>& blockSizes)
        : BlockIndex_(0)
        , BlockSizes_(blockSizes)
    {
        if (!BlockSizes_.empty()) {
            PrepareNextBlock();
        }
    }

    std::vector<TSharedRef> Add(const TSharedRef& window)
    {
        std::vector<TSharedRef> result;

        i64 offset = 0;
        while (offset < window.Size() && BlockIndex_ < BlockSizes_.size()) {
            i64 size = std::min(window.Size() - offset, CurrentBlock_.Size() - CompletedOffset_);
            std::copy(
                window.Begin() + offset,
                window.Begin() + offset + size,
                CurrentBlock_.Begin() + CompletedOffset_);

            offset += size;
            CompletedOffset_ += size;
            if (CompletedOffset_ == CurrentBlock_.Size()) {
                result.push_back(CurrentBlock_);
                BlockIndex_ += 1;
                if (BlockIndex_ < BlockSizes_.size()) {
                    PrepareNextBlock();
                }
            }
        }

        return result;
    }

private:
    void PrepareNextBlock()
    {
        CompletedOffset_ = 0;

        struct TRepairBlockTag { };
        CurrentBlock_ = TSharedMutableRef::Allocate<TRepairBlockTag>(BlockSizes_[BlockIndex_]);
    }

    int BlockIndex_;
    std::vector<i64> BlockSizes_;

    TSharedMutableRef CurrentBlock_;
    i64 CompletedOffset_;

};

///////////////////////////////////////////////////////////////////////////////

// This reader asynchronously repairs blocks of given parts.
// It is designed to minimize memory consumption.
//
// We store repaired blocks queue. When RepairNextBlock() is called,
// we first check the queue, if it isn't empty then we extract the block. Otherwise
// we read window from each part, repair windows of erased parts and add it
// to blocks and add it to RepairPartReaders. All blocks that can be
// reconstructed we add to queue.
class TRepairReader
    : public TRefCounted
{
public:
    struct TBlock
    {
        TBlock()
            : Index(-1)
        { }

        TBlock(TSharedRef data, int index)
            : Data(data)
            , Index(index)
        { }

        TSharedRef Data;
        int Index;
    };

    TRepairReader(
        NErasure::ICodec* codec,
        const std::vector<IChunkReaderPtr>& readers,
        const TPartIndexList& erasedIndices,
        const TPartIndexList& repairIndices,
        const TWorkloadDescriptor& workloadDescriptor)
        : Codec_(codec)
        , Readers_(readers)
        , ErasedIndices_(erasedIndices)
        , RepairIndices_(repairIndices)
        , WorkloadDescriptor_(workloadDescriptor)
    {
        YCHECK(Codec_->GetRepairIndices(ErasedIndices_));
        YCHECK(Codec_->GetRepairIndices(ErasedIndices_)->size() == Readers_.size());
    }

    TFuture<void> Prepare();

    bool HasNextBlock() const
    {
        YCHECK(Prepared_);
        return RepairedBlockCount_ < ErasedBlockCount_;
    }

    TFuture<TBlock> RepairNextBlock();

    i64 GetErasedDataSize() const;

private:
    NErasure::ICodec* const Codec_;
    const std::vector<IChunkReaderPtr> Readers_;
    const TPartIndexList ErasedIndices_;
    const TPartIndexList RepairIndices_;
    const TWorkloadDescriptor WorkloadDescriptor_;

    std::vector<TWindowReaderPtr> WindowReaders_;
    std::vector<TRepairPartReader> RepairBlockReaders_;

    std::deque<TBlock> RepairedBlocksQueue_;

    bool Prepared_ = false;

    int WindowIndex_ = 0;
    int WindowCount_ = -1;
    i64 WindowSize_ = -1;
    i64 LastWindowSize_ = -1;

    i64 ErasedDataSize_ = 0;

    int ErasedBlockCount_ = 0;
    int RepairedBlockCount_ = 0;

    TFuture<void> RepairBlockIfNeeded();
    TBlock OnBlockRepaired();
    TFuture<void> OnBlocksCollected(const std::vector<TSharedRef>& blocks);
    TFuture<void> Repair(const std::vector<TSharedRef>& aliveWindows);
    void OnGotMeta(const TChunkMeta& meta);

};

typedef TIntrusivePtr<TRepairReader> TRepairReaderPtr;

///////////////////////////////////////////////////////////////////////////////

TFuture<TRepairReader::TBlock> TRepairReader::RepairNextBlock()
{
    YCHECK(Prepared_);
    YCHECK(HasNextBlock());

    return RepairBlockIfNeeded().Apply(BIND(&TRepairReader::OnBlockRepaired, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetReaderInvoker()));
}

TRepairReader::TBlock TRepairReader::OnBlockRepaired()
{
    YCHECK(!RepairedBlocksQueue_.empty());
    auto block = RepairedBlocksQueue_.front();
    RepairedBlocksQueue_.pop_front();
    RepairedBlockCount_ += 1;
    return block;
}

TFuture<void> TRepairReader::Repair(const std::vector<TSharedRef>& aliveWindows)
{
    auto repairedWindows = Codec_->Decode(aliveWindows, ErasedIndices_);
    YCHECK(repairedWindows.size() == ErasedIndices_.size());
    for (int i = 0; i < repairedWindows.size(); ++i) {
        auto repairedWindow = repairedWindows[i];
        for (const auto& block : RepairBlockReaders_[i].Add(repairedWindow)) {
            RepairedBlocksQueue_.push_back(TBlock(block, ErasedIndices_[i]));
        }
    }

    if (RepairedBlocksQueue_.empty()) {
        return RepairBlockIfNeeded();
    } else {
        return VoidFuture;
    }
}

TFuture<void> TRepairReader::OnBlocksCollected(const std::vector<TSharedRef>& blocks)
{
    return BIND(&TRepairReader::Repair, MakeStrong(this), blocks)
        .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
        .Run();
}

TFuture<void> TRepairReader::RepairBlockIfNeeded()
{
    YCHECK(HasNextBlock());

    if (!RepairedBlocksQueue_.empty()) {
        return VoidFuture;
    }

    WindowIndex_ += 1;
    i64 windowSize = (WindowIndex_ == WindowCount_) ? LastWindowSize_ : WindowSize_;

    std::vector<TFuture<TSharedRef>> asyncBlocks;
    for (auto windowReader : WindowReaders_) {
        asyncBlocks.push_back(windowReader->Read(windowSize));
    }

    return Combine(asyncBlocks).Apply(
        BIND(&TRepairReader::OnBlocksCollected, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetReaderInvoker()));
}

void TRepairReader::OnGotMeta(const TChunkMeta& meta)
{
    auto placementExt = GetProtoExtension<TErasurePlacementExt>(meta.extensions());

    WindowCount_ = placementExt.parity_block_count();
    WindowSize_ = placementExt.parity_block_size();
    LastWindowSize_ = placementExt.parity_last_block_size();

    auto recoveryIndices = Codec_->GetRepairIndices(ErasedIndices_);
    YCHECK(recoveryIndices);
    YCHECK(recoveryIndices->size() == Readers_.size());

    for (int i = 0; i < Readers_.size(); ++i) {
        int recoveryIndex = (*recoveryIndices)[i];

        std::vector<i64> blockSizes;
        if (recoveryIndex < Codec_->GetDataPartCount()) {
            const auto& blockSizesProto = placementExt.part_infos().Get(recoveryIndex).block_sizes();
            blockSizes = std::vector<i64>(blockSizesProto.begin(), blockSizesProto.end());
        } else {
            blockSizes = std::vector<i64>(placementExt.parity_block_count(), placementExt.parity_block_size());
            blockSizes.back() = placementExt.parity_last_block_size();
        }

        WindowReaders_.push_back(New<TWindowReader>(
            Readers_[i],
            blockSizes,
            WorkloadDescriptor_));
    }

    for (int erasedIndex : ErasedIndices_) {
        std::vector<i64> blockSizes;
        if (erasedIndex < Codec_->GetDataPartCount()) {
            blockSizes = std::vector<i64>(
                placementExt.part_infos().Get(erasedIndex).block_sizes().begin(),
                placementExt.part_infos().Get(erasedIndex).block_sizes().end());
        } else {
            blockSizes = std::vector<i64>(
                placementExt.parity_block_count(),
                placementExt.parity_block_size());
            blockSizes.back() = placementExt.parity_last_block_size();
        }
        ErasedBlockCount_ += blockSizes.size();
        ErasedDataSize_ += std::accumulate(blockSizes.begin(), blockSizes.end(), 0LL);
        RepairBlockReaders_.push_back(TRepairPartReader(blockSizes));
    }

    Prepared_ = true;
}

TFuture<void> TRepairReader::Prepare()
{
    YCHECK(!Prepared_);
    YCHECK(!Readers_.empty());

    auto reader = Readers_.front();
    return GetPlacementMeta(reader, WorkloadDescriptor_).Apply(
        BIND(&TRepairReader::OnGotMeta, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetReaderInvoker()));
}

i64 TRepairReader::GetErasedDataSize() const
{
    YCHECK(Prepared_);
    return ErasedDataSize_;
}

///////////////////////////////////////////////////////////////////////////////

class TRepairAllPartsSession
    : public TRefCounted
{
public:
    TRepairAllPartsSession(
        NErasure::ICodec* codec,
        const TPartIndexList& erasedIndices,
        const std::vector<IChunkReaderPtr>& readers,
        const std::vector<IChunkWriterPtr>& writers,
        const TWorkloadDescriptor& workloadDescriptor,
        TRepairProgressHandler onProgress)
        : Reader_(New<TRepairReader>(
            codec,
            readers,
            erasedIndices,
            erasedIndices,
            workloadDescriptor))
        , Readers_(readers)
        , Writers_(writers)
        , WorkloadDescriptor_(workloadDescriptor)
        , OnProgress_(std::move(onProgress))
    {
        YCHECK(erasedIndices.size() == writers.size());

        for (int i = 0; i < erasedIndices.size(); ++i) {
            IndexToWriter_[erasedIndices[i]] = writers[i];
        }
    }

    TFuture<void> Run()
    {
        // Check if any blocks are missing at all.
        if (IndexToWriter_.empty()) {
            YCHECK(Readers_.empty());
            YCHECK(Writers_.empty());
            return VoidFuture;
        }

        return BIND(&TRepairAllPartsSession::DoRun, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
            .Run();
    }

private:
    void DoRun()
    {
        // Prepare reader.
        WaitFor(Reader_->Prepare())
            .ThrowOnError();

        // Open writers.
        {
            std::vector<TFuture<void>> asyncResults;
            for (auto writer : Writers_) {
                asyncResults.push_back(writer->Open());
            }
            WaitFor(Combine(asyncResults))
                .ThrowOnError();
        }

        // Repair all blocks with the help of TRepairReader and push them to the
        // corresponding writers.
        while (Reader_->HasNextBlock()) {
            auto block = WaitFor(Reader_->RepairNextBlock())
                .ValueOrThrow();

            RepairedDataSize_ += block.Data.Size();

            if (OnProgress_) {
                double progress = static_cast<double>(RepairedDataSize_) / Reader_->GetErasedDataSize();
                OnProgress_.Run(progress);
            }

            auto writer = GetWriterForIndex(block.Index);
            if (!writer->WriteBlock(block.Data)) {
                WaitFor(writer->GetReadyEvent())
                    .ThrowOnError();
            }
        }

        // Fetch chunk meta.
        auto reader = Readers_.front(); // an arbitrary one will do
        auto meta = WaitFor(reader->GetMeta(WorkloadDescriptor_))
            .ValueOrThrow();

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

    IChunkWriterPtr GetWriterForIndex(int index)
    {
        auto it = IndexToWriter_.find(index);
        YCHECK(it != IndexToWriter_.end());
        return it->second;
    }


    const TRepairReaderPtr Reader_;
    const std::vector<IChunkReaderPtr> Readers_;
    const std::vector<IChunkWriterPtr> Writers_;
    const TWorkloadDescriptor WorkloadDescriptor_;
    const TRepairProgressHandler OnProgress_;

    yhash_map<int, IChunkWriterPtr> IndexToWriter_;

    i64 RepairedDataSize_ = 0;

};

TFuture<void> RepairErasedParts(
    NErasure::ICodec* codec,
    const TPartIndexList& erasedIndices,
    const std::vector<IChunkReaderPtr>& readers,
    const std::vector<IChunkWriterPtr>& writers,
    const TWorkloadDescriptor& workloadDescriptor,
    TRepairProgressHandler onProgress)
{
    auto session = New<TRepairAllPartsSession>(
        codec,
        erasedIndices,
        readers,
        writers,
        workloadDescriptor,
        onProgress);
    return session->Run();
}

///////////////////////////////////////////////////////////////////////////////

namespace {

std::vector<IChunkReaderPtr> CreateErasurePartsReaders(
    TReplicationReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    NApi::IClientPtr client,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const TChunkId& chunkId,
    const TChunkReplicaList& replicas,
    const NErasure::ICodec* codec,
    int partCount,
    IBlockCachePtr blockCache,
    IThroughputThrottlerPtr throttler)
{
    YCHECK(IsErasureChunkId(chunkId));

    auto sortedReplicas = replicas;
    std::sort(
        sortedReplicas.begin(),
        sortedReplicas.end(),
        [] (TChunkReplica lhs, TChunkReplica rhs) {
            return lhs.GetIndex() < rhs.GetIndex();
        });

    std::vector<IChunkReaderPtr> readers;
    readers.reserve(partCount);

    {
        auto it = sortedReplicas.begin();
        while (it != sortedReplicas.end() && it->GetIndex() < partCount) {
            auto jt = it;
            while (jt != sortedReplicas.end() && it->GetIndex() == jt->GetIndex()) {
                ++jt;
            }

            TChunkReplicaList partReplicas(it, jt);
            auto partId = ErasurePartIdFromChunkId(chunkId, it->GetIndex());
            auto reader = CreateReplicationReader(
                config,
                options,
                client,
                nodeDirectory,
                Null,
                partId,
                partReplicas,
                blockCache,
                throttler);
            readers.push_back(reader);

            it = jt;
        }
    }
    YCHECK(readers.size() == partCount);

    return readers;
}

} // namespace

std::vector<IChunkReaderPtr> CreateErasureDataPartsReaders(
    TReplicationReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    NApi::IClientPtr client,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const TChunkId& chunkId,
    const TChunkReplicaList& seedReplicas,
    const NErasure::ICodec* codec,
    const Stroka& networkName,
    IBlockCachePtr blockCache,
    NConcurrency::IThroughputThrottlerPtr throttler)
{
    return CreateErasurePartsReaders(
        config,
        options,
        client,
        nodeDirectory,
        chunkId,
        seedReplicas,
        codec,
        codec->GetDataPartCount(),
        blockCache,
        throttler);
}

std::vector<IChunkReaderPtr> CreateErasureAllPartsReaders(
    TReplicationReaderConfigPtr config,
    TRemoteReaderOptionsPtr options,
    NApi::IClientPtr client,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const TChunkId& chunkId,
    const TChunkReplicaList& seedReplicas,
    const NErasure::ICodec* codec,
    IBlockCachePtr blockCache,
    NConcurrency::IThroughputThrottlerPtr throttler)
{
    return CreateErasurePartsReaders(
        config,
        options,
        client,
        nodeDirectory,
        chunkId,
        seedReplicas,
        codec,
        codec->GetTotalPartCount(),
        blockCache,
        throttler);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

