#include "blob.h"
#include "ref.h"

#include <library/cpp/ytalloc/api/ytalloc.h>

#include <util/system/align.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

const size_t InitialBlobCapacity = 16;
const double BlobCapacityMultiplier = 1.5;

TBlob::TBlob(
    TRefCountedTypeCookie tagCookie,
    size_t size,
    bool initiailizeStorage,
    size_t alignment)
    : Alignment_(alignment)
{
    SetTagCookie(tagCookie);
    if (size == 0) {
        Reset();
    } else {
        Allocate(std::max(size, InitialBlobCapacity));
        Size_ = size;
        if (initiailizeStorage) {
            memset(Begin_, 0, Size_);
        }
    }
}

TBlob::TBlob(
    TRefCountedTypeCookie tagCookie,
    const void* data,
    size_t size,
    size_t alignment)
    : Alignment_(alignment)
{
    YT_VERIFY(Alignment_ > 0);
    SetTagCookie(tagCookie);
    Reset();
    Append(data, size);
}

TBlob::TBlob(const TBlob& other)
    : Alignment_(other.Alignment_)
{
    SetTagCookie(other);
    if (other.Size_ == 0) {
        Reset();
    } else {
        Allocate(std::max(InitialBlobCapacity, other.Size_));
        memcpy(Begin_, other.Begin_, other.Size_);
        Size_ = other.Size_;
    }
}

TBlob::TBlob(TBlob&& other) noexcept
    : Buffer_(other.Buffer_)
    , Begin_(other.Begin_)
    , Size_(other.Size_)
    , Capacity_(other.Capacity_)
    , Alignment_(other.Alignment_)
{
    SetTagCookie(other);
    other.Reset();
}

TBlob::~TBlob()
{
    Free();
}

void TBlob::Reserve(size_t newCapacity)
{
    if (newCapacity > Capacity_) {
        Reallocate(newCapacity);
    }
}

void TBlob::Resize(size_t newSize, bool initializeStorage /*= true*/)
{
    if (newSize > Size_) {
        if (newSize > Capacity_) {
            size_t newCapacity;
            if (Capacity_ == 0) {
                newCapacity = std::max(InitialBlobCapacity, newSize);
            } else {
                newCapacity = std::max(static_cast<size_t>(Capacity_ * BlobCapacityMultiplier), newSize);
            }
            Reallocate(newCapacity);
        }
        if (initializeStorage) {
            memset(Begin_ + Size_, 0, newSize - Size_);
        }
    }
    Size_ = newSize;
}

TBlob& TBlob::operator = (const TBlob& rhs)
{
    if (this != &rhs) {
        this->~TBlob();
        new(this) TBlob(rhs);
    }
    return *this;
}

TBlob& TBlob::operator = (TBlob&& rhs) noexcept
{
    if (this != &rhs) {
        this->~TBlob();
        new(this) TBlob(std::move(rhs));
    }
    return *this;
}

void TBlob::Append(const void* data, size_t size)
{
    if (Size_ + size > Capacity_) {
        Resize(Size_ + size, false);
        memcpy(Begin_ + Size_ - size, data, size);
    } else {
        memcpy(Begin_ + Size_, data, size);
        Size_ += size;
    }
}

void TBlob::Append(TRef ref)
{
    Append(ref.Begin(), ref.Size());
}

void TBlob::Append(char ch)
{
    if (Size_ + 1 > Capacity_) {
        Resize(Size_ + 1, false);
        Begin_[Size_ - 1] = ch;
    } else {
        Begin_[Size_++] = ch;
    }
}

void TBlob::Reset()
{
    Buffer_ = Begin_ = nullptr;
    Size_ = Capacity_ = 0;
}

void TBlob::Allocate(size_t newCapacity)
{
    YT_VERIFY(!Buffer_);
    Buffer_ = static_cast<char*>(NYTAlloc::Allocate(newCapacity + Alignment_ - 1));
    Begin_ = AlignUp(Buffer_, Alignment_);
    Capacity_ = newCapacity;
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
    TRefCountedTrackerFacade::AllocateTagInstance(TagCookie_);
    TRefCountedTrackerFacade::AllocateSpace(TagCookie_, newCapacity);
#endif
}

void TBlob::Reallocate(size_t newCapacity)
{
    if (!Buffer_) {
        Allocate(newCapacity);
        return;
    }
    char* newBuffer = static_cast<char*>(NYTAlloc::Allocate(newCapacity + Alignment_ - 1));
    char* newBegin = AlignUp(newBuffer, Alignment_);
    memcpy(newBegin, Begin_, Size_);
    NYTAlloc::FreeNonNull(Buffer_);
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
    TRefCountedTrackerFacade::AllocateSpace(TagCookie_, newCapacity);
    TRefCountedTrackerFacade::FreeSpace(TagCookie_, Capacity_);
#endif
    Buffer_ = newBuffer;
    Begin_ = newBegin;
    Capacity_ = newCapacity;
}

void TBlob::Free()
{
    if (!Buffer_) {
        return;
    }
    NYTAlloc::FreeNonNull(Buffer_);
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
    TRefCountedTrackerFacade::FreeTagInstance(TagCookie_);
    TRefCountedTrackerFacade::FreeSpace(TagCookie_, Capacity_);
#endif
    Reset();
}

void TBlob::SetTagCookie(TRefCountedTypeCookie tagCookie)
{
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
    TagCookie_ = tagCookie;
#endif
}

void TBlob::SetTagCookie(const TBlob& other)
{
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
    TagCookie_ = other.TagCookie_;
#endif
}

void swap(TBlob& left, TBlob& right)
{
    if (&left != &right) {
        std::swap(left.Buffer_, right.Buffer_);
        std::swap(left.Begin_, right.Begin_);
        std::swap(left.Size_, right.Size_);
        std::swap(left.Capacity_, right.Capacity_);
        std::swap(left.Alignment_, right.Alignment_);
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
        std::swap(left.TagCookie_, right.TagCookie_);
#endif
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
