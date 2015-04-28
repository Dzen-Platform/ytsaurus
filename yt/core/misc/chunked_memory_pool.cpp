#include "stdafx.h"
#include "chunked_memory_pool.h"
#include "serialize.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

const i64 TChunkedMemoryPool::DefaultChunkSize = 4096;
const double TChunkedMemoryPool::DefaultMaxSmallBlockSizeRatio = 0.25;

////////////////////////////////////////////////////////////////////////////////

TChunkedMemoryPool::TChunkedMemoryPool(
    i64 chunkSize,
    double maxSmallBlockSizeRatio,
    TRefCountedTypeCookie tagCookie)
    : ChunkSize_(chunkSize)
    , MaxSmallBlockSize_(static_cast<i64>(ChunkSize_ * maxSmallBlockSizeRatio))
    , TagCookie_(tagCookie)
{
    SetupFreeZone();
}

char* TChunkedMemoryPool::AllocateUnalignedSlow(i64 size)
{
    auto* large = AllocateSlowCore(size);
    if (large) {
        return large;
    }
    return AllocateUnaligned(size);
}

char* TChunkedMemoryPool::AllocateAlignedSlow(i64 size, int align)
{
    // NB: Do not rely on any particular alignment of chunks.
    auto* large = AllocateSlowCore(size + align);
    if (large) {
        return AlignUp(large, size);
    }
    return AllocateAligned(size, align);
}

char* TChunkedMemoryPool::AllocateSlowCore(i64 size)
{
    if (size > MaxSmallBlockSize_) {
        auto block = TSharedMutableRef::Allocate(size, false, TagCookie_);
        LargeBlocks_.push_back(block);
        Capacity_ += size;
        return block.Begin();
    }

    if (CurrentChunkIndex_ + 1 >= Chunks_.size()) {
        auto chunk = TSharedMutableRef::Allocate(ChunkSize_, false, TagCookie_);
        Chunks_.push_back(chunk);
        Capacity_ += ChunkSize_;
        CurrentChunkIndex_ = static_cast<int>(Chunks_.size()) - 1;
    } else {
        ++CurrentChunkIndex_;
    }

    SetupFreeZone();

    return nullptr;
}

void TChunkedMemoryPool::Clear()
{
    CurrentChunkIndex_ = 0;
    Size_ = 0;
    SetupFreeZone();

    for (const auto& block : LargeBlocks_) {
        Capacity_ -= block.Size();
    }
    LargeBlocks_.clear();
}

i64 TChunkedMemoryPool::GetSize() const
{
    return Size_;
}

i64 TChunkedMemoryPool::GetCapacity() const
{
    return Capacity_;
}

void TChunkedMemoryPool::SetupFreeZone()
{
    if (CurrentChunkIndex_ >= Chunks_.size()) {
        FreeZoneBegin_ = nullptr;
        FreeZoneEnd_ = nullptr;
    } else {
        auto& chunk = Chunks_[CurrentChunkIndex_];
        FreeZoneBegin_ = chunk.Begin();
        FreeZoneEnd_ = chunk.End();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
