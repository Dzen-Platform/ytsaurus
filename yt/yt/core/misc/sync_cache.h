#pragma once

#include "public.h"

#include <yt/core/concurrency/rw_spinlock.h>

#include <yt/core/profiling/profiler.h>

#include <atomic>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash>
class TSyncSlruCacheBase;

template <class TKey, class TValue, class THash = ::hash<TKey>>
class TSyncCacheValueBase
    : public virtual TRefCounted
{
public:
    const TKey& GetKey() const;

protected:
    explicit TSyncCacheValueBase(const TKey& key);

private:
    const TKey Key_;

};

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash = THash<TKey>>
class TSyncSlruCacheBase
    : public virtual TRefCounted
{
public:
    typedef TIntrusivePtr<TValue> TValuePtr;

    int GetSize() const;
    std::vector<TValuePtr> GetAll();

    TValuePtr Find(const TKey& key);

    bool TryInsert(const TValuePtr& value, TValuePtr* existingValue = nullptr);
    bool TryRemove(const TKey& key);
    bool TryRemove(const TValuePtr& value);
    void Clear();

protected:
    TSlruCacheConfigPtr Config_;

    explicit TSyncSlruCacheBase(
        TSlruCacheConfigPtr config,
        const NProfiling::TProfiler& profiler = NProfiling::TProfiler());

    virtual i64 GetWeight(const TValuePtr& value) const;
    virtual void OnAdded(const TValuePtr& value);
    virtual void OnRemoved(const TValuePtr& value);

    void OnProfiling();

private:
    struct TItem
        : public TIntrusiveListItem<TItem>
    {
        explicit TItem(TValuePtr value);

        TValuePtr Value;
        bool Younger;
    };

    struct TShard
    {
        NConcurrency::TReaderWriterSpinLock SpinLock;

        TIntrusiveListWithAutoDelete<TItem, TDelete> YoungerLruList;
        TIntrusiveListWithAutoDelete<TItem, TDelete> OlderLruList;

        size_t YoungerWeightCounter = 0;
        size_t OlderWeightCounter = 0;

        THashMap<TKey, TItem*, THash> ItemMap;

        std::vector<TItem*> TouchBuffer;
        std::atomic<int> TouchBufferPosition = {0};
    };

    std::unique_ptr<TShard[]> Shards_;

    std::atomic<int> Size_ = {0};

    NProfiling::TProfiler Profiler;
    NProfiling::TMonotonicCounter HitWeightCounter_;
    NProfiling::TMonotonicCounter MissedWeightCounter_;
    NProfiling::TMonotonicCounter DroppedWeightCounter_;
    NProfiling::TSimpleGauge YoungerWeightCounter_;
    NProfiling::TSimpleGauge OlderWeightCounter_;

    TShard* GetShardByKey(const TKey& key) const;

    bool Touch(TShard* shard, TItem* item);
    void DrainTouchBuffer(TShard* shard);

    void Trim(TShard* shard, NConcurrency::TWriterGuard& guard);

    void PushToYounger(TShard* shard, TItem* item);
    void MoveToYounger(TShard* shard, TItem* item);
    void MoveToOlder(TShard* shard, TItem* item);
    void Pop(TShard* shard, TItem* item);

};

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash = THash<TKey>>
class TSimpleLruCache
{
public:
    explicit TSimpleLruCache(size_t maxWeight);

    size_t GetSize() const;

    const TValue& Get(const TKey& key);
    TValue* Find(const TKey& key);
    TValue* Insert(const TKey& key, TValue value, size_t weight = 1);

    void Clear();

private:
    struct TItem
    {
        TItem(TValue value, size_t weight)
            : Value(std::move(value))
            , Weight(weight)
        { }

        TValue Value;
        size_t Weight;
        typename std::list<typename THashMap<TKey, TItem, THash>::iterator>::iterator LruListIterator;
    };

    size_t MaxWeight_ = 0;
    size_t CurrentWeight_ = 0;

    using TItemMap = THashMap<TKey, TItem, THash>;
    TItemMap ItemMap_;
    mutable std::list<typename TItemMap::iterator> LruList_;

    void Pop();
    void UpdateLruList(typename TItemMap::iterator);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define SYNC_CACHE_INL_H_
#include "sync_cache-inl.h"
#undef SYNC_CACHE_INL_H_
