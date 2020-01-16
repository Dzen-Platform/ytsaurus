#pragma once

#include "public.h"
#include "callback.h"
#include "invoker.h"

#include <yt/core/misc/error.h>
#include <yt/core/misc/optional.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

/*
 *  Futures and Promises come in pairs and provide means for one party
 *  to wait for the result of the computation performed by the other party.
 *
 *  TPromise<T> encapsulates the value-returning mechanism while
 *  TFuture<T> enables the clients to wait for this value.
 *  The value type is always TErrorOr<T> (which reduces to just TError for |T = void|).
 *
 *  TPromise<T> is implicitly convertible to TFuture<T> while the reverse conversion
 *  is not allowed. This prevents a "malicious" client from setting the value
 *  by itself.
 *
 *  TPromise<T> and TFuture<T> are lightweight refcounted handles pointing to the internal
 *  shared state. TFuture<T> acts as a weak reference while TPromise<T> acts as
 *  a strong reference. When no outstanding strong references (i.e. promises) to
 *  the shared state remain, the state automatically becomes failed
 *  with NYT::EErrorCode::Canceled error code.
 *
 *  Futures and Promises are thread-safe.
 */

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

template <class T>
class TPromiseState;
template <class T>
void Ref(TPromiseState<T>* state);
template <class T>
void Unref(TPromiseState<T>* state);

class TFutureStateBase;
void Ref(TFutureStateBase* state);
void Unref(TFutureStateBase* state);

template <class T>
class TFutureState;
template <class T>
void Ref(TFutureState<T>* state);
template <class T>
void Unref(TFutureState<T>* state);

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

//! Creates an empty (unset) promise.
template <class T>
TPromise<T> NewPromise();

//! Constructs a pre-set promise.
template <class T>
TPromise<T> MakePromise(TErrorOr<T> value);
template <class T>
TPromise<T> MakePromise(T value);

//! Constructs a successful pre-set future.
template <class T>
TFuture<T> MakeFuture(TErrorOr<T> value);
template <class T>
TFuture<T> MakeFuture(T value);

//! Constructs a well-known pre-set future like #VoidFuture.
//! For such futures ref-counting is essentially disabled.
template <class T>
TFuture<T> MakeWellKnownFuture(TErrorOr<T> value);

////////////////////////////////////////////////////////////////////////////////
// Comparison and swap.

bool operator==(const TAwaitable& lhs, const TAwaitable& rhs);
bool operator!=(const TAwaitable& lhs, const TAwaitable& rhs);
void swap(TAwaitable& lhs, TAwaitable& rhs);

template <class T>
bool operator==(const TFuture<T>& lhs, const TFuture<T>& rhs);
template <class T>
bool operator!=(const TFuture<T>& lhs, const TFuture<T>& rhs);
template <class T>
void swap(TFuture<T>& lhs, TFuture<T>& rhs);

template <class T>
bool operator==(const TPromise<T>& lhs, const TPromise<T>& rhs);
template <class T>
bool operator!=(const TPromise<T>& lhs, const TPromise<T>& rhs);
template <class T>
void swap(TPromise<T>& lhs, TPromise<T>& rhs);

////////////////////////////////////////////////////////////////////////////////
// A bunch of widely-used preset futures.

//! A pre-set successful |void| future.
extern const TFuture<void> VoidFuture;

//! A pre-set successful |bool| future with |true| value.
extern const TFuture<bool> TrueFuture;

//! A pre-set successful |bool| future with |false| value.
extern const TFuture<bool> FalseFuture;

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TFutureBase;

template <class T>
class TPromiseBase;

////////////////////////////////////////////////////////////////////////////////

//! A distilled version of TFuture able of notifying the subscribers of completion
//! but not providing any means to extract the computation result.
class TAwaitable
{
public:
    //! Creates a null awaitable.
    TAwaitable() = default;

    //! Checks if the awaitable is null.
    explicit operator bool() const;

    //! Drops underlying associated state resetting the awaitable to null.
    void Reset();

    //! Attaches a handler invoked when the awaitable is set.
    void Subscribe(TClosure handler) const;

    //! Notifies the producer that the promised value is no longer needed.
    //! Returns |true| if succeeded, |false| is the promise was already set or canceled.
    bool Cancel(const TError& error) const;

private:
    explicit TAwaitable(TIntrusivePtr<NYT::NDetail::TFutureStateBase> impl);

    TIntrusivePtr<NYT::NDetail::TFutureStateBase> Impl_;

    friend bool operator==(const TAwaitable& lhs, const TAwaitable& rhs);
    friend bool operator!=(const TAwaitable& lhs, const TAwaitable& rhs);
    friend void swap(TAwaitable& lhs, TAwaitable& rhs);
    template <class U>
    friend struct ::THash;
    template <class U>
    friend class TFutureBase;
};

////////////////////////////////////////////////////////////////////////////////

//! A base class for both TFuture<T> and its specialization TFuture<void>.
/*!
 *  The resulting value can be accessed by either subscribing (#Subscribe)
 *  for it or retrieving it explicitly (#Get, #TryGet). Also it is possible
 *  to move the value out of the future state (#SubscribeUnique, #GetUnique, #TryGetUnique).
 *  In the latter case, however, at most one extraction is possible;
 *  further attempts to access the value will result in UB.
 *  In particular, at most one call to #SubscribeUnique, #GetUnique, and #TryGetUnique (expect
 *  for calls returning null) must happen to any future state (possibly shared by multiple
 *  TFuture instances).
 */
template <class T>
class TFutureBase
{
public:
    using TValueType = T;

    //! Creates a null future.
    TFutureBase() = default;

    //! Checks if the future is null.
    explicit operator bool() const;

    //! Drops underlying associated state resetting the future to null.
    void Reset();

    //! Checks if the value is set.
    bool IsSet() const;

    //! Gets the value.
    /*!
     *  This call will block until the value is set.
     */
    const TErrorOr<T>& Get() const;

    //! Extracts the value by moving it out of the future state.
    /*!
     *  This call will block until the value is set.
     */
    TErrorOr<T> GetUnique() const;

    //! Waits for setting the value.
    /*!
     *  This call will block until either the value is set or timeout expired.
     */
    bool TimedWait(TDuration timeout) const;

    //! Gets the value; returns null if the value is not set yet.
    /*!
     *  This call does not block.
     */
    std::optional<TErrorOr<T>> TryGet() const;

    //! Extracts the value by moving it out of the future state; returns null if the value is not set yet.
    /*!
     *  This call does not block.
     */
    std::optional<TErrorOr<T>> TryGetUnique() const;

    //! Attaches a result handler.
    /*!
     *  \param handler A callback to call when the value gets set
     *  (passing the value as a parameter).
     *
     *  \note
     *  If the value is set before the call to #Subscribe, then
     *  #callback gets called synchronously.
     *
     *  \note
     *  If the callback throws an exception, the program terminates with
     *  a call to std::terminate. This is because the subscribers are notified synchronously
     *  and thus we have to ensure that the promise state remains valid by correctly
     *  finishing the Set call.
     */
    void Subscribe(TCallback<void(const TErrorOr<T>&)> handler) const;

    //! Similar to #Subscribe but enables moving the value to the handler.
    void SubscribeUnique(TCallback<void(TErrorOr<T>&&)> handler) const;

    //! Notifies the producer that the promised value is no longer needed.
    //! Returns |true| if succeeded, |false| is the promise was already set or canceled.
    bool Cancel(const TError& error) const;

    //! Returns a wrapper that suppresses cancellation attempts.
    TFuture<T> ToUncancelable() const;

    //! Returns a wrapper that handles cancellation requests by immediately becoming set
    //! with NYT::EErrorCode::Canceled code.
    TFuture<T> ToImmediatelyCancelable() const;

    //! Returns a future that is either set to an actual value (if the original one is set in timely manner)
    //! or to |EErrorCode::Timeout| (in case of timeout).
    TFuture<T> WithTimeout(TDuration timeout) const;
    TFuture<T> WithTimeout(std::optional<TDuration> timeout) const;

    //! Chains the asynchronous computation with another synchronous function.
    template <class R>
    TFuture<R> Apply(TCallback<R(const TErrorOr<T>&)> callback) const;

    //! Chains the asynchronous computation with another synchronous function.
    template <class R>
    TFuture<R> Apply(TCallback<TErrorOr<R>(const TErrorOr<T>&)> callback) const;

    //! Chains the asynchronous computation with another asynchronous function.
    template <class R>
    TFuture<R> Apply(TCallback<TFuture<R>(const TErrorOr<T>&)> callback) const;

    //! Chains the asynchronous computation with another asynchronous function.
    template <class R>
    TFuture<R> Apply(TCallback<TErrorOr<TFuture<R>>(const TErrorOr<T>&)> callback) const;

    //! Converts (successful) result to |U|; propagates errors as is.
    template <class U>
    TFuture<U> As() const;

    //! Converts to TAwaitable interface.
    TAwaitable AsAwaitable() const;

protected:
    explicit TFutureBase(TIntrusivePtr<NYT::NDetail::TFutureState<T>> impl);

    TIntrusivePtr<NYT::NDetail::TFutureState<T>> Impl_;

    template <class U>
    friend bool operator==(const TFuture<U>& lhs, const TFuture<U>& rhs);
    template <class U>
    friend bool operator!=(const TFuture<U>& lhs, const TFuture<U>& rhs);
    template <class U>
    friend void swap(TFuture<U>& lhs, TFuture<U>& rhs);
    template <class U>
    friend struct ::THash;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TFuture
    : public TFutureBase<T>
{
public:
    TFuture() = default;
    TFuture(std::nullopt_t);

    template <class R>
    TFuture<R> Apply(TCallback<R(const T&)> callback) const;
    template <class R>
    TFuture<R> Apply(TCallback<R(T)> callback) const;
    template <class R>
    TFuture<R> Apply(TCallback<TFuture<R>(const T&)> callback) const;
    template <class R>
    TFuture<R> Apply(TCallback<TFuture<R>(T)> callback) const;
    using TFutureBase<T>::Apply;

private:
    explicit TFuture(TIntrusivePtr<NYT::NDetail::TFutureState<T>> impl);

    template <class U>
    friend TFuture<U> MakeFuture(TErrorOr<U> value);
    template <class U>
    friend TFuture<U> MakeWellKnownFuture(TErrorOr<U> value);
    template <class U>
    friend TFuture<U> MakeFuture(U value);
    template <class U>
    // XXX(babenko): 'NYT::' is a workaround; cf. https://gcc.gnu.org/bugzilla/show_bug.cgi?id=52625
    friend class NYT::TFutureBase;
    template <class U>
    friend class TPromiseBase;
};

////////////////////////////////////////////////////////////////////////////////

template <>
class TFuture<void>
    : public TFutureBase<void>
{
public:
    TFuture() = default;
    TFuture(std::nullopt_t);

    template <class R>
    TFuture<R> Apply(TCallback<R()> callback) const;
    template <class R>
    TFuture<R> Apply(TCallback<TFuture<R>()> callback) const;
    using TFutureBase<void>::Apply;

private:
    explicit TFuture(const TIntrusivePtr<NYT::NDetail::TFutureState<void>> impl);

    template <class U>
    friend TFuture<U> MakeFuture(TErrorOr<U> value);
    template <class U>
    friend TFuture<U> MakeWellKnownFuture(TErrorOr<U> value);
    template <class U>
    // XXX(babenko): 'NYT::' is a workaround; cf. https://gcc.gnu.org/bugzilla/show_bug.cgi?id=52625
    friend class NYT::TFutureBase;
    template <class U>
    friend class TPromiseBase;
};

////////////////////////////////////////////////////////////////////////////////

//! A base class for both TPromise<T> and its specialization TPromise<void>.
template <class T>
class TPromiseBase
{
public:
    using TValueType = T;

    //! Creates a null promise.
    TPromiseBase() = default;

    //! Checks if the promise is null.
    explicit operator bool() const;

    //! Drops underlying associated state resetting the promise to null.
    void Reset();

    //! Checks if the value is set.
    bool IsSet() const;

    //! Sets the value.
    /*!
     *  Calling this method also invokes all the subscribers.
     */
    void Set(const TErrorOr<T>& value) const;
    void Set(TErrorOr<T>&& value) const;

    //! Sets the value when #another future is set.
    template <class U>
    void SetFrom(const TFuture<U>& another) const;

    //! Atomically invokes |Set|, if not already set or canceled.
    //! Returns |true| if succeeded, |false| is the promise was already set or canceled.
    bool TrySet(const TErrorOr<T>& value) const;
    bool TrySet(TErrorOr<T>&& value) const;

    //! Similar to #SetFrom but calls #TrySet instead of #Set.
    template <class U>
    void TrySetFrom(TFuture<U> another) const;

    //! Gets the value.
    /*!
     *  This call will block until the value is set.
     */
    const TErrorOr<T>& Get() const;

    //! Gets the value if set.
    /*!
     *  This call does not block.
     */
    std::optional<TErrorOr<T>> TryGet() const;

    //! Checks if the promise is canceled.
    bool IsCanceled() const;

    //! Attaches a cancellation handler.
    /*!
     *  \param handler A callback to call when TFuture<T>::Cancel is triggered
     *  by the client.
     *
     *  \note
     *  If the value is set before the call to #handlered, then
     *  #handler is discarded.
     */
    void OnCanceled(TCallback<void (const TError&)> handler) const;

    //! Converts promise into future.
    operator TFuture<T>() const;
    TFuture<T> ToFuture() const;

protected:
    explicit TPromiseBase(TIntrusivePtr<NYT::NDetail::TPromiseState<T>> impl);

    TIntrusivePtr<NYT::NDetail::TPromiseState<T>> Impl_;

    template <class U>
    friend bool operator==(const TPromise<U>& lhs, const TPromise<U>& rhs);
    template <class U>
    friend bool operator!=(const TPromise<U>& lhs, const TPromise<U>& rhs);
    template <class U>
    friend void swap(TPromise<U>& lhs, TPromise<U>& rhs);
    template <class U>
    friend struct ::hash;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TPromise
    : public TPromiseBase<T>
{
public:
    TPromise() = default;
    TPromise(std::nullopt_t);

    void Set(const T& value) const;
    void Set(T&& value) const;
    void Set(const TError& error) const;
    void Set(TError&& error) const;
    using TPromiseBase<T>::Set;

    bool TrySet(const T& value) const;
    bool TrySet(T&& value) const;
    bool TrySet(const TError& error) const;
    bool TrySet(TError&& error) const;
    using TPromiseBase<T>::TrySet;

private:
    explicit TPromise(TIntrusivePtr<NYT::NDetail::TPromiseState<T>> impl);

    template <class U>
    friend TPromise<U> NewPromise();
    template <class U>
    friend TPromise<U> MakePromise(TErrorOr<U> value);
    template <class U>
    friend TPromise<U> MakePromise(U value);
};

////////////////////////////////////////////////////////////////////////////////

template <>
class TPromise<void>
    : public TPromiseBase<void>
{
public:
    TPromise() = default;
    TPromise(std::nullopt_t);

    void Set() const;
    using TPromiseBase<void>::Set;

    bool TrySet() const;
    using TPromiseBase<void>::TrySet;

private:
    explicit TPromise(TIntrusivePtr<NYT::NDetail::TPromiseState<void>> state);

    template <class U>
    friend TPromise<U> NewPromise();
    template <class U>
    friend TPromise<U> MakePromise(TErrorOr<U> value);
};

////////////////////////////////////////////////////////////////////////////////

//! Provides a noncopyable but movable wrapper around TFuture<T> whose destructor
//! cancels the underlying future.
/*!
 *  TFutureHolder wraps a (typically resource-consuming) computation and cancels it on scope exit
 *  thus preventing leaking this computation.
 */
template <class T>
class TFutureHolder
{
public:
    //! Constructs an empty holder.
    TFutureHolder() = default;

    //! Constructs an empty holder.
    TFutureHolder(std::nullopt_t);

    //! Wraps #future into a holder.
    TFutureHolder(TFuture<T> future);

    //! Cancels the underlying future (if any).
    ~TFutureHolder();

    TFutureHolder(const TFutureHolder<T>& other) = delete;
    TFutureHolder(TFutureHolder<T>&& other) = default;

    TFutureHolder& operator = (const TFutureHolder<T>& other) = delete;
    TFutureHolder& operator = (TFutureHolder<T>&& other) = default;

    //! Returns |true| if the holder has an underlying future.
    explicit operator bool() const;

    //! Returns the underlying future.
    const TFuture<T>& Get() const;

    //! Returns the underlying future.
    TFuture<T>& Get();

    //! Returns the underlying future.
    const TFuture<T>& operator*() const; // noexcept

    //! Returns the underlying future.
    TFuture<T>& operator*(); // noexcept

    //! Returns the underlying future.
    const TFuture<T>* operator->() const; // noexcept

    //! Returns the underlying future.
    TFuture<T>* operator->(); // noexcept

private:
    TFuture<T> Future_;
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TFutureCombineTraits
{
    using TCombinedVector = std::vector<T>;
    template <class K>
    using TCombinedHashMap = THashMap<K, T>;
};

template <>
struct TFutureCombineTraits<void>
{
    using TCombinedVector = void;

    template <class K>
    using TCombinedHashMap = void;
};

//! Combines a number of same-typed asynchronous computations into a single one.
/*!
 *  If |T| is |void|, then the asynchronous return type is |void|, otherwise
 *  it is |std::vector<T>| / |THashMap<K, T>|.
 *  The order of results always coincides with that of #futures (for vector variant of Combine).
 *
 *  If any of #futures fails, the others are canceled and the error is propagated immediately.
 */
template <class T>
TFuture<typename TFutureCombineTraits<T>::TCombinedVector> Combine(
    std::vector<TFuture<T>> futures);
template <class K, class T>
TFuture<typename TFutureCombineTraits<T>::template TCombinedHashMap<K>> Combine(
    const THashMap<K, TFuture<T>>& futures);

//! Same as #Combine but only wait for #quorum successful results.
/*!
 *  A single local failure, however, still propagates into a global failure.
 *  In contrast to #Combine, for non-void results their relative order is not guaranteed.
 */
template <class T>
TFuture<typename TFutureCombineTraits<T>::TCombinedVector> CombineQuorum(
    std::vector<TFuture<T>> futures,
    int quorum);

//! A variant of |Combine| that accepts future holders instead of futures.
template <class T>
TFuture<typename TFutureCombineTraits<T>::TCombinedVector> Combine(
    std::vector<TFutureHolder<T>> holders);

//! Similar to #Combine but waits for the results in all components, i.e.
//! errors occurring in components will not cause early termination.
template <class T>
TFuture<std::vector<TErrorOr<T>>> CombineAll(
    std::vector<TFuture<T>> futures);

//! Executes given #callbacks, allowing up to #concurrencyLimit simultaneous invocations.
template <class T>
TFuture<std::vector<TErrorOr<T>>> RunWithBoundedConcurrency(
    std::vector<TCallback<TFuture<T>()>> callbacks,
    int concurrencyLimit);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define FUTURE_INL_H_
#include "future-inl.h"
#undef FUTURE_INL_H_
