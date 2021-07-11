#include "slab_allocator.h"

#include <yt/yt/core/misc/atomic_ptr.h>

#include <yt/yt/library/profiling/sensor.h>

#include <library/cpp/ytalloc/api/ytalloc.h>

namespace NYT {

/////////////////////////////////////////////////////////////////////////////

static constexpr size_t SegmentSize = 64_KB;
static_assert(SegmentSize >= NYTAlloc::LargeAllocationSizeThreshold, "Segment size violation");

constexpr size_t AcquireMemoryGranularity = 500_KB;
static_assert(AcquireMemoryGranularity % 2 == 0, "Must be divisible by 2");

struct TArenaCounters
{
    TArenaCounters() = default;

    explicit TArenaCounters(const NProfiling::TProfiler& profiler)
        : AllocatedItems(profiler.Counter("/lookup/allocated_items"))
        , FreedItems(profiler.Counter("/lookup/freed_items"))
        , AliveItems(profiler.Gauge("/lookup/alive_items"))
        , AliveSegments(profiler.Gauge("/lookup/alive_segments"))
    { }

    NProfiling::TCounter AllocatedItems;
    NProfiling::TCounter FreedItems;
    NProfiling::TGauge AliveItems;
    NProfiling::TGauge AliveSegments;
};

class TSmallArena final
    : public TRefTracked<TSmallArena>
    , public TArenaCounters
{
public:
    static constexpr bool EnableHazard = true;

    struct TFreeListItem
        : public TFreeListItemBase<TFreeListItem>
    { };

    using TSimpleFreeList = TFreeList<TFreeListItem>;

    TSmallArena(
        size_t rank,
        size_t segmentSize,
        IMemoryUsageTrackerPtr memoryTracker,
        const NProfiling::TProfiler& profiler)
        : TArenaCounters(profiler.WithTag("rank", ToString(rank)))
        , ObjectSize_(NYTAlloc::SmallRankToSize[rank])
        , ObjectCount_(segmentSize / ObjectSize_)
        , MemoryTracker_(std::move(memoryTracker))
    {
        YT_VERIFY(ObjectCount_ > 0);
    }

    void* Allocate()
    {
        auto* obj = FreeList_.Extract();
        if (Y_LIKELY(obj)) {
            AllocatedItems.Increment();
            // Fast path.
            return obj;
        }

        return AllocateSlow();
    }

    void Free(void* obj)
    {
        FreedItems.Increment();
        FreeList_.Put(static_cast<TFreeListItem*>(obj));
        Unref(this);
    }

    ~TSmallArena()
    {
        static const auto& Logger = LockFreePtrLogger;

        FreeList_.ExtractAll();

        size_t segmentCount = 0;
        auto* segment = Segments_.ExtractAll();
        while (segment) {
            auto* next = segment->Next.load(std::memory_order_acquire);
            NYTAlloc::Free(segment);
            segment = next;
            ++segmentCount;
        }

        YT_VERIFY(static_cast<ssize_t>(segmentCount) == SegmentCount_.load());

        size_t totalSize = segmentCount * (sizeof(TFreeListItem) + ObjectSize_ * ObjectCount_);

        YT_LOG_TRACE("Destroying arena (ObjectSize: %v, TotalSize: %v)",
            ObjectSize_,
            totalSize);

        if (MemoryTracker_) {
            MemoryTracker_->Release(totalSize);
        }

    #ifdef YT_ENABLE_REF_COUNTED_TRACKING
        TRefCountedTrackerFacade::FreeSpace(GetRefCountedTypeCookie<TSmallArena>(), totalSize);
    #endif
    }

    bool IsReallocationNeeded() const
    {
        auto refCount = GetRefCounter(this)->GetRefCount();
        auto segmentCount = SegmentCount_.load();
        return segmentCount > 0 && refCount * 2 < static_cast<ssize_t>(segmentCount * ObjectCount_);
    }

    IMemoryUsageTrackerPtr GetMemoryTracker() const
    {
        return MemoryTracker_;
    }

private:
    const size_t ObjectSize_;
    const size_t ObjectCount_;

    TSimpleFreeList FreeList_;
    TSimpleFreeList Segments_;
    std::atomic<int> SegmentCount_ = 0;
    const IMemoryUsageTrackerPtr MemoryTracker_;

    std::pair<TFreeListItem*, TFreeListItem*> BuildFreeList(char* ptr)
    {
        auto head = reinterpret_cast<TFreeListItem*>(ptr);

        // Build chain of chunks.
        auto objectCount = ObjectCount_;
        auto objectSize = ObjectSize_;

        YT_VERIFY(objectCount > 0);
        YT_VERIFY(objectSize > 0);
        auto lastPtr = ptr + objectSize * (objectCount - 1);

        while (objectCount-- > 1) {
            auto* current = reinterpret_cast<TFreeListItem*>(ptr);
            ptr += objectSize;

            current->Next.store(reinterpret_cast<TFreeListItem*>(ptr), std::memory_order_release);
        }

        YT_VERIFY(ptr == lastPtr);

        auto* current = reinterpret_cast<TFreeListItem*>(ptr);
        current->Next.store(nullptr, std::memory_order_release);

        return {head, current};
    }

    void* AllocateSlow()
    {
        // For large chunks it is better to allocate SegmentSize + sizeof(TFreeListItem) space
        // than allocate SegmentSize and use ObjectCount_ - 1.
        auto totalSize = sizeof(TFreeListItem) + ObjectSize_ * ObjectCount_;

        if (MemoryTracker_ && !MemoryTracker_->TryAcquire(totalSize).IsOK()) {
            return nullptr;
        }

        auto segmentCount = SegmentCount_.load();
        auto refCount = GetRefCounter(this)->GetRefCount();
        static const auto& Logger = LockFreePtrLogger;

        YT_LOG_TRACE("Allocating segment (ObjectSize: %v, RefCount: %v, SegmentCount: %v, TotalObjectCapacity: %v, TotalSize: %v)",
            ObjectSize_,
            refCount,
            segmentCount,
            segmentCount * ObjectCount_,
            segmentCount * totalSize);

#ifdef YT_ENABLE_REF_COUNTED_TRACKING
        TRefCountedTrackerFacade::AllocateSpace(GetRefCountedTypeCookie<TSmallArena>(), totalSize);
#endif

        auto* ptr = NYTAlloc::Allocate(totalSize);

        // Save segments in list to free them in destructor.
        Segments_.Put(static_cast<TFreeListItem*>(ptr));

        ++SegmentCount_;

        AllocatedItems.Increment();
        AliveSegments.Update(SegmentCount_.load());

        auto [head, tail] = BuildFreeList(static_cast<char*>(ptr) + sizeof(TFreeListItem));

        // Extract one element.
        auto* next = head->Next.load();
        FreeList_.Put(next, tail);
        return head;
    }
};

DEFINE_REFCOUNTED_TYPE(TSmallArena)

/////////////////////////////////////////////////////////////////////////////

class TLargeArena
{
public:
    explicit TLargeArena(IMemoryUsageTrackerPtr memoryTracker)
        : MemoryTracker_(std::move(memoryTracker))
    { }

    void* Allocate(size_t size)
    {
        if (!TryAcquireMemory(size)) {
            return nullptr;
        }
        ++RefCount_;
        auto ptr = NYTAlloc::Allocate(size);
        auto allocatedSize = NYTAlloc::GetAllocationSize(ptr);
        YT_VERIFY(allocatedSize == size);
        return ptr;
    }

    void Free(void* ptr)
    {
        auto allocatedSize = NYTAlloc::GetAllocationSize(ptr);
        ReleaseMemory(allocatedSize);
        NYTAlloc::Free(ptr);
        Unref();
    }

    size_t Unref()
    {
        auto count = --RefCount_;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    bool TryAcquireMemory(size_t size)
    {
        if (!MemoryTracker_) {
            return true;
        }

        auto overheadMemory = OverheadMemory_.load();
        do {
            if (overheadMemory < size) {
                auto targetAcquire = std::max(AcquireMemoryGranularity, size);
                auto result = MemoryTracker_->TryAcquire(targetAcquire);
                if (result.IsOK()) {
                    OverheadMemory_.fetch_add(targetAcquire - size);
                    return true;
                } else {
                    return false;
                }
            }
        } while (!OverheadMemory_.compare_exchange_weak(overheadMemory, overheadMemory - size));

        return true;
    }

    void ReleaseMemory(size_t size)
    {
        if (!MemoryTracker_) {
            return;
        }

        auto overheadMemory = OverheadMemory_.load();

        while (overheadMemory + size > AcquireMemoryGranularity) {
            if (OverheadMemory_.compare_exchange_weak(overheadMemory, AcquireMemoryGranularity / 2)) {
                MemoryTracker_->Release(overheadMemory + size - AcquireMemoryGranularity / 2);
                return;
            }
        }

        OverheadMemory_.fetch_add(size);
    }

private:
    const IMemoryUsageTrackerPtr MemoryTracker_;
    // One ref from allocator plus refs from allocated objects.
    std::atomic<size_t> RefCount_ = 1;
    std::atomic<size_t> OverheadMemory_ = 0;
};

/////////////////////////////////////////////////////////////////////////////

TSlabAllocator::TSlabAllocator(
    const NProfiling::TProfiler& profiler,
    IMemoryUsageTrackerPtr memoryTracker)
    : Profiler_(profiler)
{
    for (size_t rank = 1; rank < NYTAlloc::SmallRankCount; ++rank) {
        // There is no std::make_unique overload with custom deleter.
        SmallArenas_[rank].Exchange(New<TSmallArena>(rank, SegmentSize, memoryTracker, Profiler_));
    }

    LargeArena_.reset(new TLargeArena(memoryTracker));
}

namespace {

TLargeArena* TryGetLargeArenaFromTag(uintptr_t tag)
{
    return tag & 1ULL ? reinterpret_cast<TLargeArena*>(tag & ~1ULL) : nullptr;
}

TSmallArena* GetSmallArenaFromTag(uintptr_t tag)
{
    return reinterpret_cast<TSmallArena*>(tag);
}

uintptr_t MakeTagFromArena(TLargeArena* arena)
{
    auto result = reinterpret_cast<uintptr_t>(arena);
    YT_ASSERT((result & 1ULL) == 0);
    return result | 1ULL;
}

uintptr_t MakeTagFromArena(TSmallArena* segment)
{
    auto result = reinterpret_cast<uintptr_t>(segment);
    YT_ASSERT((result & 1ULL) == 0);
    return result & ~1ULL;
}

const uintptr_t* GetHeaderFromPtr(const void* ptr)
{
    return static_cast<const uintptr_t*>(ptr) - 1;
}

uintptr_t* GetHeaderFromPtr(void* ptr)
{
    return static_cast<uintptr_t*>(ptr) - 1;
}

} // namespace

void TSlabAllocator::TLargeArenaDeleter::operator() (TLargeArena* arena)
{
    arena->Unref();
}

void* TSlabAllocator::Allocate(size_t size)
{
    size += sizeof(uintptr_t);

    uintptr_t tag = 0;
    void* ptr = nullptr;
    if (size < NYTAlloc::LargeAllocationSizeThreshold) {
        auto rank = NYTAlloc::SizeToSmallRank(size);

        auto arena = SmallArenas_[rank].Acquire();
        YT_VERIFY(arena);
        ptr = arena->Allocate();
        if (ptr) {
            auto* arenaPtr = arena.Release();
            arenaPtr->AliveItems.Update(GetRefCounter(arenaPtr)->GetRefCount());

            tag = MakeTagFromArena(arenaPtr);
        }
    } else {
        ptr = LargeArena_->Allocate(size);
        tag = MakeTagFromArena(LargeArena_.get());
    }

    if (!ptr) {
        return nullptr;
    }

    // Mutes TSAN data race with write Next in TFreeList::Push.
    auto* header = static_cast<std::atomic<uintptr_t>*>(ptr);
    header->store(tag, std::memory_order_release);

    return header + 1;
}

void TSlabAllocator::ReallocateArenasIfNeeded()
{
    for (size_t rank = 2; rank < NYTAlloc::SmallRankCount; ++rank) {
        // Rank is not used.
        if (rank == 3) {
            continue;
        }

        auto arena = SmallArenas_[rank].Acquire();
        if (arena->IsReallocationNeeded()) {
            SmallArenas_[rank].SwapIfCompare(
                arena,
                New<TSmallArena>(rank, SegmentSize, arena->GetMemoryTracker(), Profiler_));
        }
    }
}

void TSlabAllocator::Free(void* ptr)
{
    YT_ASSERT(ptr);
    auto* header = GetHeaderFromPtr(ptr);
    auto tag = *header;

    if (auto* largeArena = TryGetLargeArenaFromTag(tag)) {
        largeArena->Free(header);
    } else {
        auto* arenaPtr = GetSmallArenaFromTag(tag);
        arenaPtr->Free(header);
        arenaPtr->AliveItems.Update(GetRefCounter(arenaPtr)->GetRefCount());
    }
}

bool IsReallocationNeeded(const void* ptr)
{
    auto tag = *GetHeaderFromPtr(ptr);
    return !TryGetLargeArenaFromTag(tag) && GetSmallArenaFromTag(tag)->IsReallocationNeeded();
}

/////////////////////////////////////////////////////////////////////////////

} // namespace NYT

