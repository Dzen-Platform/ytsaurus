#pragma once

#include "ref_counted.h"
#include "source_location.h"

#include <util/system/defaults.h>

#include <typeinfo>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

/*!
 * \defgroup yt_new New<T> safe smart pointer constructors
 * \ingroup yt_new
 *
 * This is collection of safe smart pointer constructors.
 *
 * \{
 *
 * \page yt_new_rationale Rationale
 * New<T> function family was designed to prevent the following problem.
 * Consider the following piece of code.
 *
 * \code
 *     class TFoo
 *         : public virtual TRefCounted
 *     {
 *     public:
 *         TFoo();
 *     };
 *
 *     typedef TIntrusivePtr<TFoo> TFooPtr;
 *
 *     void RegisterObject(TFooPtr foo)
 *     {
 *         ...
 *     }
 *
 *     TFoo::TFoo()
 *     {
 *         // ... do something before
 *         RegisterObject(this);
 *         // ... do something after
 *     }
 * \endcode
 *
 * What will happen on <tt>new TFoo()</tt> construction? After memory allocation
 * the reference counter for newly created instance would be initialized to
     zero.
 * Afterwards, the control goes to TFoo constructor. To invoke
 * <tt>RegisterObject</tt> a new temporary smart pointer to the current instance
 * have to be created effectively incrementing the reference counter (now one).
 * After <tt>RegisterObject</tt> returns the control to the constructor
 * the temporary pointer is destroyed effectively decrementing the reference
 * counter to zero hence triggering object destruction during its initialization.
 *
 * To avoid this undefined behavior <tt>New<T></tt> was introduced.
 * <tt>New<T></tt> holds a fake
 * reference to the object during its construction effectively preventing
 * premature destruction.
 *
 * \note An initialization like <tt>TIntrusivePtr&lt;T&gt; p = new T()</tt>
 * would result in a dangling reference due to internals of #New<T> and
 * #TRefCountedBase.
 */

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TRefCountedTypeTag { };

template <class T>
TRefCountedTypeKey GetRefCountedTypeKey();

TRefCountedTypeCookie GetRefCountedTypeCookie(
    TRefCountedTypeKey typeKey,
    const TSourceLocation& location);

template <class T>
TRefCountedTypeCookie GetRefCountedTypeCookie();

template <class T, class TTag, int Counter>
TRefCountedTypeCookie GetRefCountedTypeCookieWithLocation(
    const TSourceLocation& location);

template <class T>
size_t SpaceUsed(const T* instance);

////////////////////////////////////////////////////////////////////////////////

//! Allocates a new instance of |T|.
template <class T, class... As>
TIntrusivePtr<T> New(As&&... args);

//! Allocates a new instance of |T|.
//! The allocation is additionally marked with #location.
template <class T, class TTag, int Counter, class... As>
inline TIntrusivePtr<T> NewWithLocation(
    const TSourceLocation& location,
    As&&... args);

//! Enables calling #New and co for types with private ctors.
#define DECLARE_NEW_FRIEND() \
    template <class T, class... As> \
    friend ::NYT::TIntrusivePtr<T> NYT::NDetail::NewImpl( \
        ::NYT::TRefCountedTypeCookie cookie, \
        size_t extraSpaceSize, \
        As&& ... args) noexcept

////////////////////////////////////////////////////////////////////////////////

//! Allocates an instance of |T| with additional storage of #extraSpaceSize bytes.
template <class T, class... As>
TIntrusivePtr<T> NewWithExtraSpace(
    size_t extraSpaceSize,
    As&&... args);

//! CRTP mixin enabling access to instance's extra space.
template <class T>
class TWithExtraSpace
{
protected:
    const void* GetExtraSpacePtr() const;
    void* GetExtraSpacePtr();

};

/*! \} */

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define NEW_INL_H_
#include "new-inl.h"
#undef NEW_INL_H_
