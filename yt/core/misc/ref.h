#pragma once

#include "blob.h"
#include "common.h"
#include "new.h"
#include "range.h"
#include "shared_range.h"

#include <type_traits>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! A non-owning reference to a range of memory.
class TRef
    : public TRange<char>
{
public:
    //! Creates a null TRef.
    TRef() = default;

    //! Creates a TRef for a given block of memory.
    TRef(const void* data, size_t size);

    //! Creates a TRef for a given range of memory.
    TRef(const void* begin, const void* end);


    //! Creates a non-owning TRef for a given blob.
    static TRef FromBlob(const TBlob& blob);

    //! Creates a non-owning TRef for a given string.
    static TRef FromString(const TString& str);

    //! Creates a non-owning TRef for a given pod structure.
    template <class T>
    static TRef FromPod(const T& data);

    //! Creates a TRef for a part of existing range.
    TRef Slice(size_t startOffset, size_t endOffset) const;

    //! Compares the content for bitwise equality.
    static bool AreBitwiseEqual(TRef lhs, TRef rhs);
};

extern const TRef EmptyRef;

////////////////////////////////////////////////////////////////////////////////

//! A non-owning reference to a mutable range of memory.
//! Use with caution :)
class TMutableRef
    : public TMutableRange<char>
{
public:
    //! Creates a null TMutableRef.
    TMutableRef() = default;

    //! Creates a TMutableRef for a given block of memory.
    TMutableRef(void* data, size_t size);

    //! Creates a TMutableRef for a given range of memory.
    TMutableRef(void* begin, void* end);

    //! Converts a TMutableRef to TRef.
    operator TRef() const;


    //! Creates a non-owning TMutableRef for a given blob.
    static TMutableRef FromBlob(TBlob& blob);

    //! Creates a non-owning TMutableRef for a given pod structure.
    template <class T>
    static TMutableRef FromPod(T& data);

    //! Creates a non-owning TMutableRef for a given string.
    //! Ensures that the string is not shared.
    static TMutableRef FromString(TString& str);

    //! Creates a TMutableRef for a part of existing range.
    TMutableRef Slice(size_t startOffset, size_t endOffset) const;
};

////////////////////////////////////////////////////////////////////////////////

//! Default tag type for memory blocks allocated via TSharedRef.
/*!
 *  Each newly allocated TSharedRef blob is associated with a tag type
 *  that appears in ref-counted statistics.
 */
struct TDefaultSharedBlobTag { };

//! A reference to a range of memory with shared ownership.
class TSharedRef
    : public TSharedRange<char>
{
public:
    //! Creates a null TSharedRef.
    TSharedRef() = default;

    //! Creates a TSharedRef with a given holder.
    TSharedRef(TRef ref, THolderPtr holder);

    //! Creates a TSharedRef from a pointer and length.
    TSharedRef(const void* data, size_t length, THolderPtr holder);

    //! Creates a TSharedRange from a range.
    TSharedRef(const void* begin, const void* end, THolderPtr holder);

    //! Converts a TSharedRef to TRef.
    operator TRef() const;


    //! Creates a TSharedRef from a string.
    //! Since strings are ref-counted, no data is copied.
    //! The memory is marked with a given tag.
    template <class TTag>
    static TSharedRef FromString(TString str);

    //! Creates a TSharedRef from a string.
    //! Since strings are ref-counted, no data is copied.
    //! The memory is marked with TDefaultSharedBlobTag.
    static TSharedRef FromString(TString str);

    //! Creates a TSharedRef reference from a string.
    //! Since strings are ref-counted, no data is copied.
    //! The memory is marked with a given tag.
    static TSharedRef FromString(TString str, TRefCountedTypeCookie tagCookie);

    //! Creates a TSharedRef for a given blob taking ownership of its content.
    static TSharedRef FromBlob(TBlob&& blob);

    //! Creates a copy of a given TRef.
    //! The memory is marked with a given tag.
    static TSharedRef MakeCopy(TRef ref, TRefCountedTypeCookie tagCookie);

    //! Creates a copy of a given TRef.
    //! The memory is marked with a given tag.
    template <class TTag>
    static TSharedRef MakeCopy(TRef ref);

    //! Creates a TSharedRef for a part of existing range.
    TSharedRef Slice(size_t startOffset, size_t endOffset) const;

    //! Creates a TSharedRef for a part of existing range.
    TSharedRef Slice(const void* begin, const void* end) const;

    //! Creates a vector of slices with specified size.
    std::vector<TSharedRef> Split(size_t partSize) const;

private:
    class TBlobHolder
        : public TIntrinsicRefCounted
    {
    public:
        explicit TBlobHolder(TBlob&& blob);

    private:
        const TBlob Blob_;
    };

    class TStringHolder
        : public TIntrinsicRefCounted
    {
    public:
        TStringHolder(TString&& string, TRefCountedTypeCookie cookie);
        ~TStringHolder();

    private:
        const TString String_;
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
        const TRefCountedTypeCookie Cookie_;
#endif
    };
};

extern const TSharedRef EmptySharedRef;

////////////////////////////////////////////////////////////////////////////////

//! A reference to a mutable range of memory with shared ownership.
//! Use with caution :)
class TSharedMutableRef
    : public TSharedMutableRange<char>
{
public:
    //! Creates a null TSharedMutableRef.
    TSharedMutableRef() = default;

    //! Creates a TSharedMutableRef with a given holder.
    TSharedMutableRef(const TMutableRef& ref, THolderPtr holder);

    //! Creates a TSharedMutableRef from a pointer and length.
    TSharedMutableRef(void* data, size_t length, THolderPtr holder);

    //! Creates a TSharedMutableRange from a range.
    TSharedMutableRef(void* begin, void* end, THolderPtr holder);

    //! Converts a TSharedMutableRef to TMutableRef.
    operator TMutableRef() const;

    //! Converts a TSharedMutableRef to TSharedRef.
    operator TSharedRef() const;

    //! Converts a TSharedMutableRef to TRef.
    operator TRef() const;


    //! Allocates a new shared block of memory.
    //! The memory is marked with a given tag.
    template <class TTag>
    static TSharedMutableRef Allocate(size_t size, bool initializeStorage = true);

    //! Allocates a new shared block of memory.
    //! The memory is marked with TDefaultSharedBlobTag.
    static TSharedMutableRef Allocate(size_t size, bool initializeStorage = true);

    //! Allocates a new shared block of memory.
    //! The memory is marked with a given tag.
    static TSharedMutableRef Allocate(size_t size, bool initializeStorage, TRefCountedTypeCookie tagCookie);

    //! Creates a TSharedMutableRef for the whole blob taking ownership of its content.
    static TSharedMutableRef FromBlob(TBlob&& blob);

    //! Creates a copy of a given TRef.
    //! The memory is marked with a given tag.
    static TSharedMutableRef MakeCopy(TRef ref, TRefCountedTypeCookie tagCookie);

    //! Creates a copy of a given TRef.
    //! The memory is marked with a given tag.
    template <class TTag>
    static TSharedMutableRef MakeCopy(TRef ref);

    //! Creates a reference for a part of existing range.
    TSharedMutableRef Slice(size_t startOffset, size_t endOffset) const;

    //! Creates a reference for a part of existing range.
    TSharedMutableRef Slice(void* begin, void* end) const;

private:
    class TBlobHolder
        : public TIntrinsicRefCounted
    {
    public:
        explicit TBlobHolder(TBlob&& blob);

    private:
        const TBlob Blob_;
    };

    class TAllocationHolder
        : public TIntrinsicRefCounted
        , public TWithExtraSpace<TAllocationHolder>
    {
    public:
        TAllocationHolder(size_t size, bool initializeStorage, TRefCountedTypeCookie cookie);
        ~TAllocationHolder();

        TMutableRef GetRef();

    private:
        const size_t Size_;
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
        const TRefCountedTypeCookie Cookie_;
#endif
    };
};

////////////////////////////////////////////////////////////////////////////////

//! A smart-pointer to a ref-counted immutable sequence of TSharedRef-s.
class TSharedRefArray
{
public:
    TSharedRefArray() = default;
    TSharedRefArray(const TSharedRefArray& other);
    TSharedRefArray(TSharedRefArray&& other) noexcept;

    explicit TSharedRefArray(const TSharedRef& part);
    explicit TSharedRefArray(TSharedRef&& part);

    struct TCopyParts
    { };
    struct TMoveParts
    { };

    template <class TParts>
    TSharedRefArray(const TParts& parts, TCopyParts);
    template <class TParts>
    TSharedRefArray(TParts&& parts, TMoveParts);

    TSharedRefArray& operator = (const TSharedRefArray& other);
    TSharedRefArray& operator = (TSharedRefArray&& other);

    explicit operator bool() const;

    void Reset();

    size_t Size() const;
    i64 ByteSize() const;
    bool Empty() const;
    const TSharedRef& operator [] (size_t index) const;

    const TSharedRef* Begin() const;
    const TSharedRef* End() const;

    std::vector<TSharedRef> ToVector() const;

    TSharedRef Pack() const;
    static TSharedRefArray Unpack(const TSharedRef& packedRef);

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl_;

    explicit TSharedRefArray(TIntrusivePtr<TImpl> impl);

    template <class... As>
    static TIntrusivePtr<TImpl> NewImpl(size_t size, As... args);
};

// STL interop.
const TSharedRef* begin(const TSharedRefArray& array);
const TSharedRef* end(const TSharedRefArray& array);

////////////////////////////////////////////////////////////////////////////////

TString ToString(TRef ref);
TString ToString(const TMutableRef& ref);
TString ToString(const TSharedRef& ref);
TString ToString(const TSharedMutableRef& ref);

size_t GetPageSize();
size_t RoundUpToPage(size_t bytes);

size_t GetByteSize(TRef ref);
size_t GetByteSize(const TSharedRefArray& array);
template <class T>
size_t GetByteSize(const std::vector<T>& parts);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define REF_INL_H_
#include "ref-inl.h"
#undef REF_INL_H_

