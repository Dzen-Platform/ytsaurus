#pragma once
#ifndef EXPIRING_CACHE_INL_H_
#error "Direct inclusion of this file is not allowed, include expiring_cache.h"
#endif

#include "config.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue>
bool TExpiringCache<TKey, TValue>::TEntry::Expired(const TInstant& now) const
{
    return now > AccessDeadline || now > UpdateDeadline;
}

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
            if (!entry->Expired(now)) {
                entry->AccessDeadline = now + Config_->ExpireAfterAccessTime;
                return entry->Promise;
            }
        }
    }

    // Slow path.
    {
        NConcurrency::TWriterGuard guard(SpinLock_);
        auto it = Map_.find(key);

        if (it != Map_.end()) {
            auto& entry = it->second;
            if (entry->Promise.IsSet() && entry->Expired(now)) {
                NConcurrency::TDelayedExecutor::CancelAndClear(entry->ProbationCookie);
                Map_.erase(it);
            } else {
                entry->AccessDeadline = now + Config_->ExpireAfterAccessTime;
                return entry->Promise;
            }
        }

        auto entry = New<TEntry>();
        entry->UpdateDeadline = TInstant::Max();
        entry->AccessDeadline = now + Config_->ExpireAfterAccessTime;
        auto promise = entry->Promise = NewPromise<TValue>();
        // NB: we don't want to hold a strong reference to entry after releasing the guard, so we make a weak reference here.
        auto weakEntry = MakeWeak(entry);
        YCHECK(Map_.insert(std::make_pair(key, std::move(entry))).second);
        guard.Release();
        InvokeGet(weakEntry, key);
        return promise;
    }
}

template <class TKey, class TValue>
TFuture<typename TExpiringCache<TKey, TValue>::TCombinedValue> TExpiringCache<TKey, TValue>::Get(const std::vector<TKey>& keys)
{
    auto now = TInstant::Now();

    std::vector<TFuture<TValue>> results(keys.size());
    std::vector<size_t> fetchIndexes;

    // Fast path.
    {
        NConcurrency::TReaderGuard guard(SpinLock_);

        for (size_t index = 0; index < keys.size(); ++index) {
            auto it = Map_.find(keys[index]);
            if (it != Map_.end()) {
                const auto& entry = it->second;
                if (!entry->Expired(now)) {
                    results[index] = entry->Promise;
                    entry->AccessDeadline = now + Config_->ExpireAfterAccessTime;
                    continue;
                }
            }
            fetchIndexes.push_back(index);
        }
    }

    // Slow path.
    if (!fetchIndexes.empty()) {
        std::vector<size_t> invokeIndexes;
        std::vector<TWeakPtr<TEntry>> invokeEntries;

        NConcurrency::TWriterGuard guard(SpinLock_);
        for (auto index : fetchIndexes) {
            const auto& key = keys[index];

            auto it = Map_.find(keys[index]);
            if (it != Map_.end()) {
                auto& entry = it->second;
                if (entry->Promise.IsSet() && entry->Expired(now)) {
                    NConcurrency::TDelayedExecutor::CancelAndClear(entry->ProbationCookie);
                    Map_.erase(it);
                } else {
                    results[index] = entry->Promise;
                    entry->AccessDeadline = now + Config_->ExpireAfterAccessTime;
                    continue;
                }
            }

            auto entry = New<TEntry>();
            entry->UpdateDeadline = TInstant::Max();
            entry->AccessDeadline = now + Config_->ExpireAfterAccessTime;
            entry->Promise = NewPromise<TValue>();

            invokeIndexes.push_back(index);
            invokeEntries.push_back(entry);
            results[index] = entry->Promise;

            YCHECK(Map_.insert(std::make_pair(key, std::move(entry))).second);
        }

        std::vector<TKey> invokeKeys;
        for (auto index : invokeIndexes) {
            invokeKeys.push_back(keys[index]);
        }

        guard.Release();
        InvokeGetMany(invokeEntries, invokeKeys);
    }

    return Combine(results);
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
void TExpiringCache<TKey, TValue>::SetResult(const TWeakPtr<TEntry>& weakEntry, const TKey& key, const TErrorOr<TValue>& valueOrError)
{
    auto entry = weakEntry.Lock();
    if (!entry) {
        return;
    }

    auto it = Map_.find(key);
    Y_ASSERT(it != Map_.end() && it->second == entry);

    auto expirationTime = valueOrError.IsOK() ? Config_->ExpireAfterSuccessfulUpdateTime : Config_->ExpireAfterFailedUpdateTime;
    entry->UpdateDeadline = TInstant::Now() + expirationTime;
    if (entry->Promise.IsSet()) {
        entry->Promise = MakePromise(valueOrError);
    } else {
        entry->Promise.Set(valueOrError);
    }
    YCHECK(entry->Promise.IsSet());

    if (TInstant::Now() > entry->AccessDeadline) {
        Map_.erase(key);
        return;
    }

    if (TInstant::Now() > entry->AccessDeadline) {
        Map_.erase(key);
        return;
    }

    if (valueOrError.IsOK()) {
        NTracing::TNullTraceContextGuard guard;
        entry->ProbationCookie = NConcurrency::TDelayedExecutor::Submit(
            BIND(&TExpiringCache::InvokeGet, MakeWeak(this), MakeWeak(entry), key),
            Config_->RefreshTime);
    }
}

template <class TKey, class TValue>
void TExpiringCache<TKey, TValue>::InvokeGet(const TWeakPtr<TEntry>& weakEntry, const TKey& key)
{
    DoGet(key)
    .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TValue>& valueOrError) {
        NConcurrency::TWriterGuard guard(SpinLock_);

        SetResult(weakEntry, key, valueOrError);
    }));
}

template <class TKey, class TValue>
void TExpiringCache<TKey, TValue>::InvokeGetMany(const std::vector<TWeakPtr<TEntry>>& entries, const std::vector<TKey>& keys)
{
    DoGetMany(keys)
    .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<std::vector<TValue>>& valueOrError) {
        NConcurrency::TWriterGuard guard(SpinLock_);

        if (valueOrError.IsOK()) {
            for (size_t index = 0; index < keys.size(); ++index) {
                SetResult(entries[index], keys[index], valueOrError.Value()[index]);
            }
        } else {
            for (size_t index = 0; index < keys.size(); ++index) {
                SetResult(entries[index], keys[index], TError(valueOrError));
            }
        }
    }));
}

template <class TKey, class TValue>
TFuture<typename TExpiringCache<TKey, TValue>::TCombinedValue> TExpiringCache<TKey, TValue>::DoGetMany(const std::vector<TKey>& keys)
{
    std::vector<TFuture<TValue>> results;

    for (const auto& key : keys) {
        results.push_back(DoGet(key));
    }

    return Combine(results);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
