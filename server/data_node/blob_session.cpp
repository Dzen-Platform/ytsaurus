#include "blob_session.h"
#include "private.h"
#include "blob_chunk.h"
#include "chunk_block_manager.h"
#include "chunk_store.h"
#include "config.h"
#include "location.h"
#include "session_manager.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/ytlib/chunk_client/chunk_meta.pb.h>
#include <yt/ytlib/chunk_client/file_writer.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/fs.h>

#include <yt/core/profiling/scoped_timer.h>

namespace NYT {
namespace NDataNode {

using namespace NRpc;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NCellNode;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static NProfiling::TSimpleCounter DiskBlobWriteByteCounter("/disk_blob_write_bytes");

////////////////////////////////////////////////////////////////////////////////

TBlobSession::TBlobSession(
    TDataNodeConfigPtr config,
    TBootstrap* bootstrap,
    const TChunkId& chunkId,
    const TSessionOptions& options,
    TStoreLocationPtr location,
    TLease lease)
    : TSessionBase(
        config,
        bootstrap,
        chunkId,
        options,
        location,
        lease)
{ }

TFuture<void> TBlobSession::DoStart()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    WriteInvoker_->Invoke(BIND(&TBlobSession::DoOpenWriter, MakeStrong(this)));

    // No need to wait for the writer to get opened.
    return VoidFuture;
}

TFuture<IChunkPtr> TBlobSession::DoFinish(
    const TChunkMeta* chunkMeta,
    const TNullable<int>& blockCount)
{
    YCHECK(chunkMeta != nullptr);
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (!blockCount) {
        THROW_ERROR_EXCEPTION("Attempt to finish a blob session %v without specifying block count",
            ChunkId_);
    }

    if (*blockCount != BlockCount_) {
        THROW_ERROR_EXCEPTION("Block count mismatch in blob session %v: expected %v, got %v",
            ChunkId_,
            BlockCount_,
            *blockCount);
    }

    for (int blockIndex = WindowStartBlockIndex_; blockIndex < Window_.size(); ++blockIndex) {
        const auto& slot = GetSlot(blockIndex);
        if (slot.State != ESlotState::Empty) {
            THROW_ERROR_EXCEPTION(
                NChunkClient::EErrorCode::WindowError,
                "Attempt to finish a session with an unflushed block %v:%v",
                ChunkId_,
                blockIndex);
        }
    }

    return CloseWriter(*chunkMeta).Apply(
        BIND(&TBlobSession::OnWriterClosed, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetControlInvoker()));
}

TChunkInfo TBlobSession::GetChunkInfo() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Writer_->GetChunkInfo();
}

TFuture<void> TBlobSession::DoPutBlocks(
    int startBlockIndex,
    const std::vector<TSharedRef>& blocks,
    bool enableCaching)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (blocks.empty()) {
        return VoidFuture;
    }

    auto chunkBlockManager = Bootstrap_->GetChunkBlockManager();

    int blockIndex = startBlockIndex;
    i64 requestSize = 0;
    std::vector<int> receivedBlockIndexes;
    for (const auto& block : blocks) {
        TBlockId blockId(ChunkId_, blockIndex);
        ValidateBlockIsInWindow(blockIndex);

        if (!Location_->HasEnoughSpace(block.Size())) {
            return MakeFuture(TError(
                NChunkClient::EErrorCode::NoLocationAvailable,
                "No enough space left on location"));
        }

        auto* tracker = Bootstrap_->GetMemoryUsageTracker();
        auto guardOrError = TNodeMemoryTrackerGuard::TryAcquire(
            tracker,
            EMemoryCategory::BlobSession,
            block.Size());
        if (!guardOrError.IsOK()) {
            return MakeFuture(TError(guardOrError));
        }

        auto& slot = GetSlot(blockIndex);
        if (slot.State != ESlotState::Empty) {
            if (TRef::AreBitwiseEqual(slot.Block, block)) {
                LOG_WARNING("Skipped duplicate block (Block: %v)", blockIndex);
                continue;
            }

            return MakeFuture(TError(
                NChunkClient::EErrorCode::BlockContentMismatch,
                "Block %v:%v with a different content already received",
                ChunkId_,
                blockIndex)
                << TErrorAttribute("window_start", WindowStartBlockIndex_));
        }

        ++BlockCount_;

        slot.State = ESlotState::Received;
        slot.Block = block;
        slot.MemoryTrackerGuard = std::move(guardOrError.Value());

        if (enableCaching) {
            chunkBlockManager->PutCachedBlock(blockId, block, Null);
        }

        Location_->UpdateUsedSpace(block.Size());
        Size_ += block.Size();
        requestSize += block.Size();
        receivedBlockIndexes.push_back(blockIndex);
        ++blockIndex;
    }

    LOG_DEBUG_UNLESS(
        receivedBlockIndexes.empty(),
        "Blocks received (Blocks: [%v])",
        JoinToString(receivedBlockIndexes));

    auto sessionManager = Bootstrap_->GetSessionManager();
    while (WindowIndex_ < Window_.size()) {
        auto& slot = GetSlot(WindowIndex_);
        YCHECK(slot.State == ESlotState::Received || slot.State == ESlotState::Empty);
        if (slot.State == ESlotState::Empty)
            break;

        slot.PendingIOGuard = Location_->IncreasePendingIOSize(
            EIODirection::Write,
            Options_.WorkloadDescriptor,
            slot.Block.Size());

        BIND(
            &TBlobSession::DoWriteBlock,
            MakeStrong(this),
            slot.Block,
            WindowIndex_)
        .AsyncVia(WriteInvoker_)
        .Run()
        .Subscribe(
            BIND(&TBlobSession::OnBlockWritten, MakeStrong(this), WindowIndex_)
                .Via(Bootstrap_->GetControlInvoker()));

        ++WindowIndex_;
    }

    auto throttler = Bootstrap_->GetInThrottler(Options_.WorkloadDescriptor);
    return throttler->Throttle(requestSize);
}

TFuture<void> TBlobSession::DoSendBlocks(
    int firstBlockIndex,
    int blockCount,
    const TNodeDescriptor& targetDescriptor)
{
    TDataNodeServiceProxy proxy(ChannelFactory->CreateChannel(targetDescriptor.GetInterconnectAddress()));
    proxy.SetDefaultTimeout(Config_->NodeRpcTimeout);

    auto req = proxy.PutBlocks();
    ToProto(req->mutable_chunk_id(), ChunkId_);
    req->set_first_block_index(firstBlockIndex);

    i64 requestSize = 0;
    for (int blockIndex = firstBlockIndex; blockIndex < firstBlockIndex + blockCount; ++blockIndex) {
        auto block = GetBlock(blockIndex);
        req->Attachments().push_back(block);
        requestSize += block.Size();
    }

    auto throttler = Bootstrap_->GetOutThrottler(Options_.WorkloadDescriptor);
    return throttler->Throttle(requestSize).Apply(BIND([=] () {
        return req->Invoke().As<void>();
    }));
}

void TBlobSession::DoWriteBlock(const TSharedRef& block, int blockIndex)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(Error_);

    LOG_DEBUG("Started writing block %v (BlockSize: %v)",
        blockIndex,
        block.Size());

    NProfiling::TScopedTimer timer;
    try {
        if (!Writer_->WriteBlock(block)) {
            auto result = Writer_->GetReadyEvent().Get();
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
            YUNREACHABLE();
        }
    } catch (const std::exception& ex) {
        TBlockId blockId(ChunkId_, blockIndex);
        SetFailed(TError(
            NChunkClient::EErrorCode::IOError,
            "Error writing chunk block %v",
            blockId)
            << ex);
    }

    auto writeTime = timer.GetElapsed();

    LOG_DEBUG("Finished writing block %v", blockIndex);

    auto& locationProfiler = Location_->GetProfiler();
    locationProfiler.Enqueue("/blob_block_write_size", block.Size());
    locationProfiler.Enqueue("/blob_block_write_time", writeTime.MicroSeconds());
    locationProfiler.Enqueue("/blob_block_write_throughput", block.Size() * 1000000 / (1 + writeTime.MicroSeconds()));

    DataNodeProfiler.Increment(DiskBlobWriteByteCounter, block.Size());

    THROW_ERROR_EXCEPTION_IF_FAILED(Error_);
}

void TBlobSession::OnBlockWritten(int blockIndex, const TError& error)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto& slot = GetSlot(blockIndex);
    slot.PendingIOGuard.Release();
    if (error.IsOK()) {
        YCHECK(slot.State == ESlotState::Received);
        slot.State = ESlotState::Written;
        slot.WrittenPromise.Set(TError());
    }
}

TFuture<void> TBlobSession::DoFlushBlocks(int blockIndex)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    // TODO(psushin): verify monotonicity of blockIndex
    ValidateBlockIsInWindow(blockIndex);

    const auto& slot = GetSlot(blockIndex);
    if (slot.State == ESlotState::Empty) {
        THROW_ERROR_EXCEPTION(
            NChunkClient::EErrorCode::WindowError,
            "Attempt to flush an unreceived block %v:%v",
            ChunkId_,
            blockIndex);
    }

    // WrittenPromise is set in the control thread, hence no need for AsyncVia.
    return slot.WrittenPromise.ToFuture().Apply(
        BIND(&TBlobSession::OnBlockFlushed, MakeStrong(this), blockIndex));
}

void TBlobSession::OnBlockFlushed(int blockIndex, const TError& error)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    ReleaseBlocks(blockIndex);

    THROW_ERROR_EXCEPTION_IF_FAILED(error);
}

void TBlobSession::DoCancel()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    AbortWriter()
        .Apply(BIND(&TBlobSession::OnWriterAborted, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetControlInvoker()));
}

void TBlobSession::DoOpenWriter()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    LOG_TRACE("Started opening blob chunk writer");

    PROFILE_TIMING ("/blob_chunk_open_time") {
        try {
            auto fileName = Location_->GetChunkPath(ChunkId_);
            Writer_ = New<TFileWriter>(ChunkId_, fileName, Options_.SyncOnClose);
            // File writer opens synchronously.
            Writer_->Open()
                .Get()
                .ThrowOnError();
        }
        catch (const std::exception& ex) {
            SetFailed(TError(
                NChunkClient::EErrorCode::IOError,
                "Error creating chunk %v",
                ChunkId_)
                << ex);
            return;
        }
    }

    LOG_TRACE("Finished opening blob chunk writer");
}

TFuture<void> TBlobSession::AbortWriter()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    return BIND(&TBlobSession::DoAbortWriter, MakeStrong(this))
        .AsyncVia(WriteInvoker_)
        .Run();
}

void TBlobSession::DoAbortWriter()
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(Error_);

    LOG_DEBUG("Started aborting chunk writer");

    PROFILE_TIMING ("/blob_chunk_abort_time") {
        try {
            Writer_->Abort();
        } catch (const std::exception& ex) {
            SetFailed(TError(
                NChunkClient::EErrorCode::IOError,
                "Error aborting chunk %v",
                ChunkId_)
                << ex);
        }
        Writer_.Reset();
    }

    LOG_DEBUG("Finished aborting chunk writer");

    THROW_ERROR_EXCEPTION_IF_FAILED(Error_);
}

void TBlobSession::OnWriterAborted(const TError& error)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO(error, "Session canceled");

    ReleaseSpace();
    Finished_.Fire(error);

    THROW_ERROR_EXCEPTION_IF_FAILED(error);
}

TFuture<void> TBlobSession::CloseWriter(const TChunkMeta& chunkMeta)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    return BIND(&TBlobSession::DoCloseWriter,MakeStrong(this), chunkMeta)
        .AsyncVia(WriteInvoker_)
        .Run();
}

void TBlobSession::DoCloseWriter(const TChunkMeta& chunkMeta)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(Error_);

    LOG_DEBUG("Started closing chunk writer (ChunkSize: %v)",
        Writer_->GetDataSize());

    PROFILE_TIMING ("/blob_chunk_close_time") {
        try {
            auto result = Writer_->Close(chunkMeta).Get();
            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        } catch (const std::exception& ex) {
            SetFailed(TError(
                NChunkClient::EErrorCode::IOError,
                "Error closing chunk %v",
                ChunkId_)
                << ex);
        }
    }

    LOG_DEBUG("Finished closing chunk writer");

    THROW_ERROR_EXCEPTION_IF_FAILED(Error_);
}

IChunkPtr TBlobSession::OnWriterClosed(const TError& error)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    ReleaseSpace();

    if (!error.IsOK()) {
        LOG_WARNING(error, "Session has failed to finish");
        Finished_.Fire(error);
        THROW_ERROR(error);
    }

    TChunkDescriptor descriptor;
    descriptor.Id = ChunkId_;
    descriptor.DiskSpace = Writer_->GetChunkInfo().disk_space();
    auto chunk = New<TStoredBlobChunk>(
        Bootstrap_,
        Location_,
        descriptor,
        &Writer_->GetChunkMeta());

    auto chunkStore = Bootstrap_->GetChunkStore();
    chunkStore->RegisterNewChunk(chunk);

    Finished_.Fire(TError());

    return chunk;
}

void TBlobSession::ReleaseBlocks(int flushedBlockIndex)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YCHECK(WindowStartBlockIndex_ <= flushedBlockIndex);

    while (WindowStartBlockIndex_ <= flushedBlockIndex) {
        auto& slot = GetSlot(WindowStartBlockIndex_);
        YCHECK(slot.State == ESlotState::Written);
        slot.Block = TSharedRef();
        slot.MemoryTrackerGuard.Release();
        slot.PendingIOGuard.Release();
        slot.WrittenPromise.Reset();
        ++WindowStartBlockIndex_;
    }

    LOG_DEBUG("Released blocks (WindowStart: %v)",
        WindowStartBlockIndex_);
}

bool TBlobSession::IsInWindow(int blockIndex)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    return blockIndex >= WindowStartBlockIndex_;
}

void TBlobSession::ValidateBlockIsInWindow(int blockIndex)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (!IsInWindow(blockIndex)) {
        THROW_ERROR_EXCEPTION(
            NChunkClient::EErrorCode::WindowError,
            "Block %v:%v is out of the window",
            ChunkId_,
            blockIndex);
    }
}

TBlobSession::TSlot& TBlobSession::GetSlot(int blockIndex)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    YCHECK(IsInWindow(blockIndex));

    while (Window_.size() <= blockIndex) {
        // NB: do not use resize here!
        // Newly added slots must get a fresh copy of WrittenPromise promise.
        // Using resize would cause all of these slots to share a single promise.
        Window_.emplace_back();
    }

    return Window_[blockIndex];
}

TSharedRef TBlobSession::GetBlock(int blockIndex)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    ValidateBlockIsInWindow(blockIndex);

    Ping();

    const auto& slot = GetSlot(blockIndex);
    if (slot.State == ESlotState::Empty) {
        THROW_ERROR_EXCEPTION(
            NChunkClient::EErrorCode::WindowError,
            "Trying to retrieve a block %v:%v that is not received yet",
            ChunkId_,
            blockIndex);
    }

    LOG_DEBUG("Block retrieved (Block: %v)", blockIndex);

    return slot.Block;
}

void TBlobSession::MarkAllSlotsWritten(const TError& error)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    for (auto& slot : Window_) {
        if (slot.State == ESlotState::Received) {
            slot.State = ESlotState::Written;
            slot.WrittenPromise.Set(error);
        }
    }
}

void TBlobSession::ReleaseSpace()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    Location_->UpdateUsedSpace(-Size_);
}

void TBlobSession::SetFailed(const TError& error)
{
    VERIFY_THREAD_AFFINITY(WriterThread);

    if (!Error_.IsOK())
        return;

    Error_ = TError("Session failed") << error;

    Bootstrap_->GetControlInvoker()->Invoke(
        BIND(&TBlobSession::MarkAllSlotsWritten, MakeStrong(this), error));

    Location_->Disable(Error_);
    YUNREACHABLE(); // Disable() exits the process.
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
