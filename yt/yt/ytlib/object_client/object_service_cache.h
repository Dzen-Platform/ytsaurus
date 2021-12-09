#pragma once

#include "private.h"

#include <yt/yt/core/misc/async_slru_cache.h>
#include <yt/yt/core/misc/historic_usage_aggregator.h>

#include <yt/yt/core/concurrency/spinlock.h>

#include <yt/yt/client/hydra/public.h>

#include <util/generic/ymath.h>

namespace NYT::NObjectClient {

////////////////////////////////////////////////////////////////////////////////

struct TObjectServiceCacheKey
{
    TCellTag CellTag;
    TString User;
    NYPath::TYPath Path;
    TString Service;
    TString Method;
    TSharedRef RequestBody;
    size_t RequestBodyHash;

    TObjectServiceCacheKey(
        TCellTag CellTag,
        TString user,
        NYPath::TYPath path,
        TString service,
        TString method,
        TSharedRef requestBody);

    operator size_t() const;
    bool operator == (const TObjectServiceCacheKey& other) const;

    i64 ComputeExtraSpace() const;
};

void FormatValue(TStringBuilderBase* builder, const TObjectServiceCacheKey& key, TStringBuf /*format*/);

////////////////////////////////////////////////////////////////////////////////

class TObjectServiceCacheEntry
    : public TAsyncCacheValueBase<TObjectServiceCacheKey, TObjectServiceCacheEntry>
{
public:
    TObjectServiceCacheEntry(
        const TObjectServiceCacheKey& key,
        bool success,
        NHydra::TRevision revision,
        TInstant timestamp,
        TSharedRefArray responseMessage,
        double byteRate,
        TInstant lastUpdateTime);

    DEFINE_BYVAL_RO_PROPERTY(bool, Success);
    DEFINE_BYVAL_RO_PROPERTY(TSharedRefArray, ResponseMessage);
    DEFINE_BYVAL_RO_PROPERTY(i64, TotalSpace);
    DEFINE_BYVAL_RO_PROPERTY(TInstant, Timestamp);
    DEFINE_BYVAL_RO_PROPERTY(NHydra::TRevision, Revision);

    void IncrementRate();

    double GetByteRate() const;
    TInstant GetLastUpdateTime() const;

private:
    std::atomic<double> ByteRate_ = 0;
    std::atomic<TInstant> LastUpdateTime_ = TInstant::Zero();
    YT_DECLARE_SPINLOCK(NThreading::TSpinLock, Lock_);

    i64 ComputeExtraSpace() const;
};

DEFINE_REFCOUNTED_TYPE(TObjectServiceCacheEntry)

////////////////////////////////////////////////////////////////////////////////

struct TCacheProfilingCounters
    : public TRefCounted
{
    explicit TCacheProfilingCounters(const NProfiling::TProfiler& profiler);

    NProfiling::TCounter HitRequestCount;
    NProfiling::TCounter HitResponseBytes;
    NProfiling::TCounter MissRequestCount;
};

DEFINE_REFCOUNTED_TYPE(TCacheProfilingCounters)

////////////////////////////////////////////////////////////////////////////////

class TObjectServiceCache
    : public TMemoryTrackingAsyncSlruCacheBase<TObjectServiceCacheKey, TObjectServiceCacheEntry>
{
public:
    TObjectServiceCache(
        TObjectServiceCacheConfigPtr config,
        IMemoryUsageTrackerPtr memoryTracker,
        const NLogging::TLogger& logger,
        const NProfiling::TProfiler& profiler);

    using TCookie = TAsyncSlruCacheBase<TObjectServiceCacheKey, TObjectServiceCacheEntry>::TInsertCookie;
    TCookie BeginLookup(
        NRpc::TRequestId requestId,
        const TObjectServiceCacheKey& key,
        TDuration expireAfterSuccessfulUpdateTime,
        TDuration expireAfterFailedUpdateTime,
        NHydra::TRevision refreshRevision);

    void EndLookup(
        NRpc::TRequestId requestId,
        TCookie cookie,
        const TSharedRefArray& responseMessage,
        NHydra::TRevision revision,
        bool success);

    NYTree::IYPathServicePtr GetOrchidService();

    void Configure(const TObjectServiceCacheDynamicConfigPtr& config);

private:
    const TObjectServiceCacheConfigPtr Config_;
    const NLogging::TLogger Logger;
    const NProfiling::TProfiler Profiler_;

    std::atomic<double> TopEntryByteRateThreshold_;

    YT_DECLARE_SPINLOCK(NThreading::TReaderWriterSpinLock, Lock_);

    using TProfilingCountersKey = std::tuple<TString, TString>;
    THashMap<TProfilingCountersKey, TCacheProfilingCountersPtr> KeyToCounters_;

    YT_DECLARE_SPINLOCK(NThreading::TReaderWriterSpinLock, ExpiredEntriesLock_);
    THashMap<TObjectServiceCacheKey, TObjectServiceCacheEntryPtr> ExpiredEntries_;

    YT_DECLARE_SPINLOCK(NThreading::TReaderWriterSpinLock, TopEntriesLock_);
    THashMap<TObjectServiceCacheKey, TObjectServiceCacheEntryPtr> TopEntries_;

    TCacheProfilingCountersPtr GetProfilingCounters(const TString& user, const TString& method);

    bool IsResurrectionSupported() const override;

    void OnAdded(const TObjectServiceCacheEntryPtr& entry) override;
    void OnRemoved(const TObjectServiceCacheEntryPtr& entry) override;
    i64 GetWeight(const TObjectServiceCacheEntryPtr& entry) const override;

    static bool IsExpired(
        const TObjectServiceCacheEntryPtr& entry,
        TDuration expireAfterSuccessfulUpdateTime,
        TDuration expireAfterFailedUpdateTime);

    void TouchEntry(const TObjectServiceCacheEntryPtr& entry);
    void DoBuildOrchid(NYson::IYsonConsumer* consumer);
};

DEFINE_REFCOUNTED_TYPE(TObjectServiceCache)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectClient
