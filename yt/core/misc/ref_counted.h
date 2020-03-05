#pragma once

#include "intrusive_ptr.h"
#include "port.h"

#include <library/ytalloc/api/ytalloc.h>

#include <util/generic/noncopyable.h>

#include <atomic>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TSourceLocation;

class TRefCountedBase;

class TRefCountedImpl;

//! Default base class for all ref-counted types.
/*!
 *  Supports weak pointers.
 *
 *  Instances are created with a single memory allocation.
 */
using TRefCounted = TRefCountedImpl;

// COMPAT(lukyan): Both versions are lightweight and support weak pointers.
using TIntrinsicRefCounted = TRefCountedImpl;

using TRefCountedTypeCookie = int;
const int NullRefCountedTypeCookie = -1;

using TRefCountedTypeKey = const void*;

////////////////////////////////////////////////////////////////////////////////

// Used to avoid including heavy ref_counted_tracker.h
class TRefCountedTrackerFacade
{
public:
    static TRefCountedTypeCookie GetCookie(
        TRefCountedTypeKey typeKey,
        size_t instanceSize,
        const NYT::TSourceLocation& location);

    static void AllocateInstance(TRefCountedTypeCookie cookie);
    static void FreeInstance(TRefCountedTypeCookie cookie);

    static void AllocateTagInstance(TRefCountedTypeCookie cookie);
    static void FreeTagInstance(TRefCountedTypeCookie cookie);

    static void AllocateSpace(TRefCountedTypeCookie cookie, size_t size);
    static void FreeSpace(TRefCountedTypeCookie cookie, size_t size);

    // Typically invoked from GDB console.
    // Dumps the ref-counted statistics sorted by "bytes alive".
    static void Dump();
};

////////////////////////////////////////////////////////////////////////////////

//! A technical base class for TRefCountedImpl and promise states.
class TRefCountedBase
{
public:
    TRefCountedBase() = default;
    virtual ~TRefCountedBase() noexcept = default;

    void operator delete(void* ptr) noexcept;

    virtual const void* GetDerived() const = 0;

private:
    TRefCountedBase(const TRefCountedBase&) = delete;
    TRefCountedBase(TRefCountedBase&&) = delete;

    TRefCountedBase& operator=(const TRefCountedBase&) = delete;
    TRefCountedBase& operator=(TRefCountedBase&&) = delete;

};

////////////////////////////////////////////////////////////////////////////////

//! Base class for all reference-counted objects.
class TRefCountedImpl
    : public TRefCountedBase
{
public:
    TRefCountedImpl() = default;
    ~TRefCountedImpl() noexcept = default;

    //! Increments the strong reference counter.
    void Ref() const noexcept;

    //! Decrements the strong reference counter.
    void Unref() const;

    //! Increments the strong reference counter if it is not null.
    bool TryRef() const noexcept;

    //! Increments the weak reference counter.
    void WeakRef() const noexcept;

    //! Decrements the weak reference counter.
    void WeakUnref() const;

    //! Returns current number of strong references to the object.
    /*!
     * Note that you should never ever use this method in production code.
     * This method is mainly for debugging purposes.
     */
    int GetRefCount() const noexcept;

    //! Returns current number of weak references to the object.
    int GetWeakRefCount() const noexcept;

    //! Tries to obtain an intrusive pointer for an object that may had
    //! already lost all of its references and, thus, is about to be deleted.
    /*!
     * You may call this method at any time provided that you have a valid
     * raw pointer to an object. The call either returns an intrusive pointer
     * for the object (thus ensuring that the object won't be destroyed until
     * you're holding this pointer) or NULL indicating that the last reference
     * had already been lost and the object is on its way to heavens.
     * All these steps happen atomically.
     *
     * Under all circumstances it is caller's responsibility the make sure that
     * the object is not destroyed during the call to #DangerousGetPtr.
     * Typically this is achieved by keeping a (lock-protected) collection of
     * raw pointers, taking a lock in object's destructor, and unregistering
     * its raw pointer from the collection there.
     */
    template <class T>
    static TIntrusivePtr<T> DangerousGetPtr(T* object);

private:
    //! Number of strong references.
    mutable std::atomic<int> StrongCount_ = {1};

    //! Number of weak references plus one if there is at least one strong reference.
    mutable std::atomic<int> WeakCount_ = {1};

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define REF_COUNTED_INL_H_
#include "ref_counted-inl.h"
#undef REF_COUNTED_INL_H_
