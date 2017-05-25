#include "encoding_writer.h"
#include "private.h"
#include "block_cache.h"
#include "chunk_writer.h"
#include "config.h"
#include "dispatcher.h"

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/core/compression/codec.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/misc/finally.h>
#include <yt/core/misc/serialize.h>
#include <yt/core/misc/checksum.h>

namespace NYT {
namespace NChunkClient {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

TEncodingWriter::TEncodingWriter(
    TEncodingWriterConfigPtr config,
    TEncodingWriterOptionsPtr options,
    IChunkWriterPtr chunkWriter,
    IBlockCachePtr blockCache,
    const NLogging::TLogger& logger)
    : Config_(config)
    , Options_(options)
    , ChunkWriter_(chunkWriter)
    , BlockCache_(blockCache)
    , Logger(logger)
    , CompressionRatio_(Config_->DefaultCompressionRatio)
    , CompressionInvoker_(CreateSerializedInvoker(CreateFixedPriorityInvoker(
        TDispatcher::Get()->GetCompressionPoolInvoker(),
        Config_->WorkloadDescriptor.GetPriority())))
    , Semaphore_(New<TAsyncSemaphore>(Config_->EncodeWindowSize))
    , Codec_(NCompression::GetCodec(options->CompressionCodec))
    , WritePendingBlockCallback_(BIND(
        &TEncodingWriter::WritePendingBlock,
        MakeWeak(this)))
{ }

void TEncodingWriter::WriteBlock(TSharedRef block)
{
    EnsureOpen();

    UncompressedSize_ += block.Size();
    Semaphore_->Acquire(block.Size());
    BIND(
        &TEncodingWriter::DoCompressBlock,
        MakeWeak(this),
        std::move(block))
    .Via(CompressionInvoker_)
    .Run();
}

void TEncodingWriter::WriteBlock(std::vector<TSharedRef> vectorizedBlock)
{
    EnsureOpen();

    for (const auto& part : vectorizedBlock) {
        Semaphore_->Acquire(part.Size());
        UncompressedSize_ += part.Size();
    }
    BIND(
        &TEncodingWriter::DoCompressVector,
        MakeWeak(this),
        std::move(vectorizedBlock))
    .Via(CompressionInvoker_)
    .Run();
}

void TEncodingWriter::EnsureOpen()
{
    if (!OpenFuture_) {
        OpenFuture_ = ChunkWriter_->Open();
        OpenFuture_.Subscribe(BIND([this, this_ = MakeStrong(this)] (const TError& error) {
            if (!error.IsOK()) {
                CompletionError_.TrySet(error);
            } else {
                LOG_DEBUG("Underlying session for encoding writer opened (ChunkId: %v)",
                    ChunkWriter_->GetChunkId());
                PendingBlocks_.Dequeue().Subscribe(
                    WritePendingBlockCallback_.Via(CompressionInvoker_));
            }
        }));
    }
}

void TEncodingWriter::CacheUncompressedBlock(const TSharedRef& block, int blockIndex)
{
    // We cannot cache blocks before chunk writer is open, since we do not know the #ChunkId.
    auto blockId = TBlockId(ChunkWriter_->GetChunkId(), blockIndex);
    BlockCache_->Put(blockId, EBlockType::UncompressedData, TBlock(block), Null);
}

// Serialized compression invoker affinity (don't use thread affinity because of thread pool).
void TEncodingWriter::DoCompressBlock(const TSharedRef& uncompressedBlock)
{
    LOG_DEBUG("Compressing block (Block: %v)", AddedBlockIndex_);

    TBlock compressedBlock;
    compressedBlock.Data = Codec_->Compress(uncompressedBlock);

    CompressedSize_ += compressedBlock.Size();

    if (Config_->ComputeChecksum) {
        compressedBlock.Checksum = GetChecksum(compressedBlock.Data);
    }

    if (Config_->VerifyCompression) {
        VerifyBlock(uncompressedBlock, compressedBlock.Data);
    }

    if (Any(BlockCache_->GetSupportedBlockTypes() & EBlockType::UncompressedData)) {
        OpenFuture_.Apply(BIND(
            &TEncodingWriter::CacheUncompressedBlock,
            MakeWeak(this),
            uncompressedBlock,
            AddedBlockIndex_));
    }

    int sizeToRelease = -static_cast<i64>(compressedBlock.Size()) + uncompressedBlock.Size();
    ProcessCompressedBlock(compressedBlock, sizeToRelease);
}

// Serialized compression invoker affinity (don't use thread affinity because of thread pool).
void TEncodingWriter::DoCompressVector(const std::vector<TSharedRef>& uncompressedVectorizedBlock)
{
    LOG_DEBUG("Compressing block (Block: %v)", AddedBlockIndex_);

    TBlock compressedBlock;
    compressedBlock.Data = Codec_->Compress(uncompressedVectorizedBlock);

    CompressedSize_ += compressedBlock.Size();

    if (Config_->ComputeChecksum) {
        compressedBlock.Checksum = GetChecksum(compressedBlock.Data);
    }

    if (Config_->VerifyCompression) {
        VerifyVector(uncompressedVectorizedBlock, compressedBlock.Data);
    }

    if (Any(BlockCache_->GetSupportedBlockTypes() & EBlockType::UncompressedData)) {
        struct TMergedTag { };
        // Handle none codec separately to avoid merging block parts twice.
        auto uncompressedBlock = Options_->CompressionCodec == NCompression::ECodec::None
            ? compressedBlock.Data
            : MergeRefsToRef<TMergedTag>(uncompressedVectorizedBlock);
        OpenFuture_.Apply(BIND(
            &TEncodingWriter::CacheUncompressedBlock,
            MakeWeak(this),
            uncompressedBlock,
            AddedBlockIndex_));
    }

    i64 sizeToRelease = -static_cast<i64>(compressedBlock.Size()) + GetByteSize(uncompressedVectorizedBlock);
    ProcessCompressedBlock(compressedBlock, sizeToRelease);
}

void TEncodingWriter::VerifyVector(
    const std::vector<TSharedRef>& uncompressedVectorizedBlock,
    const TSharedRef& compressedBlock)
{
    auto decompressedBlock = Codec_->Decompress(compressedBlock);

    LOG_FATAL_IF(
        decompressedBlock.Size() != GetByteSize(uncompressedVectorizedBlock),
        "Compression verification failed");

    const char* current = decompressedBlock.Begin();
    for (const auto& block : uncompressedVectorizedBlock) {
        LOG_FATAL_IF(
            !TRef::AreBitwiseEqual(TRef(current, block.Size()), block),
            "Compression verification failed");
        current += block.Size();
    }
}

void TEncodingWriter::VerifyBlock(
    const TSharedRef& uncompressedBlock,
    const TSharedRef& compressedBlock)
{
    auto decompressedBlock = Codec_->Decompress(compressedBlock);
    LOG_FATAL_IF(
        !TRef::AreBitwiseEqual(decompressedBlock, uncompressedBlock),
        "Compression verification failed");
}

// Serialized compression invoker affinity (don't use thread affinity because of thread pool).
void TEncodingWriter::ProcessCompressedBlock(const TBlock& block, i64 sizeToRelease)
{
    CompressionRatio_ = double(CompressedSize_) / UncompressedSize_;

    if (sizeToRelease > 0) {
        Semaphore_->Release(sizeToRelease);
    } else {
        Semaphore_->Acquire(-sizeToRelease);
    }

    PendingBlocks_.Enqueue(block);
    LOG_DEBUG("Pending block added (Block: %v)", AddedBlockIndex_);

    ++AddedBlockIndex_;
}

// Serialized compression invoker affinity (don't use thread affinity because of thread pool).
void TEncodingWriter::WritePendingBlock(const TErrorOr<TBlock>& blockOrError)
{
    if (!blockOrError.IsOK()) {
        // Sentinel element.
        CompletionError_.Set(TError());
        return;
    }

    LOG_DEBUG("Writing pending block (Block: %v)", WrittenBlockIndex_);

    auto& block = blockOrError.Value();
    auto isReady = ChunkWriter_->WriteBlock(block);
    ++WrittenBlockIndex_;

    auto finally = Finally([&] (){
        Semaphore_->Release(block.Size());
    });

    if (!isReady) {
        auto error = WaitFor(ChunkWriter_->GetReadyEvent());
        if (!error.IsOK()) {
            CompletionError_.Set(error);
            return;
        }
    }

    PendingBlocks_.Dequeue().Subscribe(
        WritePendingBlockCallback_.Via(CompressionInvoker_));
}

bool TEncodingWriter::IsReady() const
{
    return Semaphore_->IsReady() && !CompletionError_.IsSet();
}

TFuture<void> TEncodingWriter::GetReadyEvent()
{
    auto promise = NewPromise<void>();
    promise.TrySetFrom(CompletionError_.ToFuture());
    promise.TrySetFrom(Semaphore_->GetReadyEvent());

    return promise.ToFuture();
}

TFuture<void> TEncodingWriter::Flush()
{
    // This must be the last enqueued element.
    BIND([this, this_ = MakeStrong(this)] () {
        PendingBlocks_.Enqueue(TError("Sentinel value"));
    })
    .Via(CompressionInvoker_)
    .Run();
    return CompletionError_.ToFuture();
}

i64 TEncodingWriter::GetUncompressedSize() const
{
    return UncompressedSize_;
}

i64 TEncodingWriter::GetCompressedSize() const
{
    // NB: #CompressedSize_ may have not been updated yet (updated in compression invoker).
    return static_cast<i64>(GetUncompressedSize() * GetCompressionRatio());
}

double TEncodingWriter::GetCompressionRatio() const
{
    return CompressionRatio_.load();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
