#ifndef EXPIRING_CACHE_INL_H_
#error "Direct inclusion of this file is not allowed, include expiring_cache.h"
#endif

#include "config.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue>
TExpiringCache<TKey, TValue>::TExpiringCache(TExpiringCacheConfigPtr config)
    : Config_(std::move(config))
{ }

template <class TKey, class TValue>
TFuture<TValue> TExpiringCache<TKey, TValue>::Get(const TKey& key)
{
    auto now = TInstant::Now();

    // Fast path.
    {
        NConcurrency::TReaderGuard guard(SpinLock_);
        auto it = Map_.find(key);
        if (it != Map_.end()) {
            const auto& entry = it->second;
            if (now < entry.Deadline) {
                return entry.Promise;
            }
        }
    }

    // Slow path.
    {
        NConcurrency::TWriterGuard guard(SpinLock_);
        auto it = Map_.find(key);
        if (it == Map_.end()) {
            TEntry entry;
            entry.Deadline = TInstant::Max();
            auto promise = entry.Promise = NewPromise<TValue>();
            YCHECK(Map_.insert(std::make_pair(key, entry)).second);
            guard.Release();
            InvokeGet(key);
            return promise;
        }

        auto& entry = it->second;
        const auto& promise = entry.Promise;
        if (!promise.IsSet()) {
            return promise;
        }

        if (now > entry.Deadline) {
            // Evict and retry.
            NConcurrency::TDelayedExecutor::CancelAndClear(entry.ProbationCookie);
            entry.ProbationFuture.Cancel();
            entry.ProbationFuture.Reset();
            Map_.erase(it);
            guard.Release();
            return Get(key);
        }

        return promise;
    }
}

template <class TKey, class TValue>
bool TExpiringCache<TKey, TValue>::TryRemove(const TKey& key)
{
    NConcurrency::TWriterGuard guard(SpinLock_);
    return Map_.erase(key) != 0;
}

template <class TKey, class TValue>
void TExpiringCache<TKey, TValue>::Clear()
{
    NConcurrency::TWriterGuard guard(SpinLock_);
    Map_.clear();
}

template <class TKey, class TValue>
void TExpiringCache<TKey, TValue>::InvokeGet(const TKey& key)
{
    NConcurrency::TWriterGuard guard(SpinLock_);

    auto it = Map_.find(key);
    if (it == Map_.end()) {
        return;
    }

    auto future = it->second.ProbationFuture = DoGet(key);

    guard.Release();

    future.Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TValue>& valueOrError) {
        NConcurrency::TWriterGuard guard(SpinLock_);
        auto it = Map_.find(key);
        if (it == Map_.end())
            return;

        auto& entry = it->second;

        auto expirationTime = valueOrError.IsOK() ? Config_->SuccessExpirationTime : Config_->FailureExpirationTime;
        entry.Deadline = TInstant::Now() + expirationTime;
        if (entry.Promise.IsSet()) {
            entry.Promise = MakePromise(valueOrError);
        } else {
            entry.Promise.Set(valueOrError);
        }

        if (valueOrError.IsOK()) {
            NTracing::TNullTraceContextGuard guard;
            entry.ProbationCookie = NConcurrency::TDelayedExecutor::Submit(
                BIND(&TExpiringCache::InvokeGet, MakeWeak(this), key),
                Config_->SuccessProbationTime);
        }
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
