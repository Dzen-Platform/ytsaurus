#ifndef NEW_INL_H_
#error "Direct inclusion of this file is not allowed, include new.h"
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

namespace {

//! A per-translation unit tag type.
struct TCurrentTranslationUnitTag
{ };

} // namespace

template <class T>
TRefCountedTypeKey GetRefCountedTypeKey()
{
    return &typeid(T);
}

TRefCountedTypeCookie GetRefCountedTypeCookie(
    TRefCountedTypeKey typeKey,
    const TSourceLocation& location);

template <class T>
Y_FORCE_INLINE TRefCountedTypeCookie GetRefCountedTypeCookie()
{
    static auto cookie = NullRefCountedTypeCookie;
    if (Y_UNLIKELY(cookie == NullRefCountedTypeCookie)) {
        cookie = GetRefCountedTypeCookie(
            GetRefCountedTypeKey<T>(),
            NYT::TSourceLocation());
    }
    return cookie;
}

template <class T, class TTag, int Counter>
Y_FORCE_INLINE TRefCountedTypeCookie GetRefCountedTypeCookieWithLocation(const TSourceLocation& location)
{
    static auto cookie = NullRefCountedTypeCookie;
    if (Y_UNLIKELY(cookie == NullRefCountedTypeCookie)) {
        cookie = GetRefCountedTypeCookie(
            GetRefCountedTypeKey<T>(),
            location);
    }
    return cookie;
}

template <class T>
Y_FORCE_INLINE size_t SpaceUsed(const T* /*instance*/)
{
    return sizeof(T);
}

namespace NDetail {

Y_FORCE_INLINE void InitializeNewInstance(
    void* /*instance*/,
    void* /*ptr*/)
{ }

Y_FORCE_INLINE void InitializeNewInstance(
    TRefCounted* instance,
    void* ptr)
{
    instance->GetRefCounter()->SetPtr(ptr);
}

template <class T, class... As>
Y_FORCE_INLINE TIntrusivePtr<T> NewImpl(
    TRefCountedTypeCookie cookie,
    size_t extraSpaceSize,
    As&& ... args) noexcept
{
    auto totalSize = sizeof(T) + extraSpaceSize;
    auto* ptr = ::malloc(totalSize);
    auto* instance = static_cast<T*>(ptr);

    new (instance) T(std::forward<As>(args)...);

    InitializeNewInstance(instance, ptr);
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
    InitializeRefCountedTracking(instance, cookie, SpaceUsed(instance));
#endif

    return TIntrusivePtr<T>(instance, false);
}

} // namespace NDetail

template <class T, class... As>
Y_FORCE_INLINE TIntrusivePtr<T> New(As&&... args)
{
    return NewWithExtraSpace<T>(0, std::forward<As>(args)...);
}

template <class T, class... As>
TIntrusivePtr<T> NewWithExtraSpace(
    size_t extraSpaceSize,
    As&&... args)
{
    return NDetail::NewImpl<T>(
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
        GetRefCountedTypeCookie<T>(),
#else
        NullRefCountedTypeCookie, // unused
#endif
        extraSpaceSize,
        std::forward<As>(args)...);
}

template <class T, class TTag, int Counter, class... As>
Y_FORCE_INLINE TIntrusivePtr<T> NewWithLocation(
    const TSourceLocation& location,
    As&&... args)
{
    return NDetail::NewImpl<T>(
#ifdef YT_ENABLE_REF_COUNTED_TRACKING
        GetRefCountedTypeCookieWithLocation<T, TTag, Counter>(location),
#else
        NullRefCountedTypeCookie, // unused
#endif
        0,
        std::forward<As>(args)...);
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
const void* TWithExtraSpace<T>::GetExtraSpacePtr() const
{
    return static_cast<const T*>(this) + 1;
}

template <class T>
void* TWithExtraSpace<T>::GetExtraSpacePtr()
{
    return static_cast<T*>(this) + 1;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
