#include "config.h"

#include "object_service_cache.h"
#include "object_service_proxy.h"

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/async_cache.h>
#include <yt/core/misc/string.h>
#include <yt/core/misc/checksum.h>

#include <yt/core/rpc/helpers.h>
#include <yt/core/rpc/throttling_channel.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/ytree/proto/ypath.pb.h>

namespace NYT::NObjectClient {

using namespace NConcurrency;
using namespace NRpc;
using namespace NRpc::NProto;
using namespace NYPath;
using namespace NYTree;
using namespace NYTree::NProto;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TObjectServiceCacheKey::TObjectServiceCacheKey(
    TCellTag cellTag,
    TString user,
    TYPath path,
    TString service,
    TString method,
    TSharedRef requestBody)
    : CellTag(std::move(cellTag))
    , User(std::move(user))
    , Path(std::move(path))
    , Service(std::move(service))
    , Method(std::move(method))
    , RequestBody(std::move(requestBody))
    , RequestBodyHash(GetChecksum(RequestBody))
{ }

TObjectServiceCacheKey::operator size_t() const
{
    size_t result = 0;
    HashCombine(result, CellTag);
    HashCombine(result, User);
    HashCombine(result, Path);
    HashCombine(result, Service);
    HashCombine(result, Method);
    HashCombine(result, RequestBodyHash);
    return result;
}

bool TObjectServiceCacheKey::operator == (const TObjectServiceCacheKey& other) const
{
    return
        CellTag == other.CellTag &&
        User == other.User &&
        Path == other.Path &&
        Service == other.Service &&
        Method == other.Method &&
        RequestBodyHash == other.RequestBodyHash &&
        TRef::AreBitwiseEqual(RequestBody, other.RequestBody);
}

void FormatValue(TStringBuilderBase* builder, const TObjectServiceCacheKey& key, TStringBuf /*format*/)
{
    builder->AppendFormat("{%v %v %v.%v %v %x}",
        key.CellTag,
        key.User,
        key.Service,
        key.Method,
        key.Path,
        key.RequestBodyHash);
}

////////////////////////////////////////////////////////////////////////////////

TObjectServiceCacheEntry::TObjectServiceCacheEntry(
    const TObjectServiceCacheKey& key,
    bool success,
    NHydra::TRevision revision,
    TInstant timestamp,
    TSharedRefArray responseMessage,
    double byteRate,
    TInstant lastUpdateTime)
    : TAsyncCacheValueBase(key)
    , Success_(success)
    , ResponseMessage_(std::move(responseMessage))
    , TotalSpace_(GetByteSize(ResponseMessage_))
    , Timestamp_(timestamp)
    , Revision_(revision)
    , ByteRate_(byteRate)
    , LastUpdateTime_(lastUpdateTime)
{ }

void TObjectServiceCacheEntry::IncrementRate()
{
    TGuard guard(Lock_);

    auto now = TInstant::Now();
    if (LastUpdateTime_.load() == TInstant::Zero()) {
        ByteRate_ = TotalSpace_;
    } else {
        auto sinceLast = now - LastUpdateTime_;
        auto w = Exp2(-2. * sinceLast.SecondsFloat());
        ByteRate_ = w * ByteRate_ + TotalSpace_;
    }
    LastUpdateTime_ = now;
}

double TObjectServiceCacheEntry::GetByteRate() const
{
    return ByteRate_;
}

TInstant TObjectServiceCacheEntry::GetLastUpdateTime() const
{
    return LastUpdateTime_;
}

////////////////////////////////////////////////////////////////////////////////

TCacheProfilingCounters::TCacheProfilingCounters(const NProfiling::TTagIdList& tagIds)
    : HitRequestCount("/hit_request_count", tagIds)
    , HitResponseBytes("/hit_response_bytes", tagIds)
    , MissRequestCount("/miss_request_count", tagIds)
{ }

////////////////////////////////////////////////////////////////////////////////

TObjectServiceCache::TObjectServiceCache(
    const TObjectServiceCacheConfigPtr& config,
    const NLogging::TLogger& logger,
    const NProfiling::TProfiler& profiler)
    : TAsyncSlruCacheBase(config)
    , Logger(logger)
    , Profiler_(profiler)
    , TopEntryByteRateThreshold_(config->TopEntryByteRateThreshold)
{ }

TObjectServiceCache::TCookie TObjectServiceCache::BeginLookup(
    TRequestId requestId,
    const TObjectServiceCacheKey& key,
    TDuration successExpirationTime,
    TDuration failureExpirationTime,
    NHydra::TRevision refreshRevision)
{
    auto entry = Find(key);
    auto tryRemove = [&] () {
        {
            TWriterGuard guard(ExpiredEntriesLock_);
            ExpiredEntries_.emplace(key, entry);
        }

        TryRemove(entry);
    };

    bool cacheHit = false;
    if (entry) {
        if (refreshRevision && entry->GetRevision() != NHydra::NullRevision && entry->GetRevision() <= refreshRevision) {
            YT_LOG_DEBUG("Cache entry refresh requested (RequestId: %v, Key: %v, Revision: %llx, Success: %v)",
                requestId,
                key,
                entry->GetRevision(),
                entry->GetSuccess());

            tryRemove();
        } else if (IsExpired(entry, successExpirationTime, failureExpirationTime)) {
            YT_LOG_DEBUG("Cache entry expired (RequestId: %v, Key: %v, Revision: %llx, Success: %v)",
                requestId,
                key,
                entry->GetRevision(),
                entry->GetSuccess());

            tryRemove();
        } else {
            cacheHit = true;
            YT_LOG_DEBUG("Cache hit (RequestId: %v, Key: %v, Revision: %llx, Success: %v)",
                requestId,
                key,
                entry->GetRevision(),
                entry->GetSuccess());
        }

        TouchEntry(entry);
    } else {
        TReaderGuard guard(ExpiredEntriesLock_);

        if (auto it = ExpiredEntries_.find(key); it != ExpiredEntries_.end()) {
            TouchEntry(it->second);
        }
    }

    auto counters = GetProfilingCounters(key.User, key.Method);
    if (cacheHit) {
        Profiler_.Increment(counters->HitRequestCount);
        Profiler_.Increment(counters->HitResponseBytes, entry->GetTotalSpace());
    } else {
        Profiler_.Increment(counters->MissRequestCount);
    }

    return BeginInsert(key);
}

void TObjectServiceCache::EndLookup(
    NRpc::TRequestId requestId,
    TCookie cookie,
    const TSharedRefArray& responseMessage,
    NHydra::TRevision revision,
    bool success)
{
    const auto& key = cookie.GetKey();

    YT_LOG_DEBUG("Cache population request succeeded (RequestId: %v, Key: %v, Revision: %llx, Success: %v)",
        requestId,
        key,
        revision,
        success);

    auto rate = 0.0;
    auto lastUpdateTime = TInstant::Now();
    {
        TWriterGuard guard(ExpiredEntriesLock_);

        if (auto it = ExpiredEntries_.find(key); it != ExpiredEntries_.end()) {
            const auto& expiredEntry = it->second;
            rate = expiredEntry->GetByteRate();
            lastUpdateTime = expiredEntry->GetLastUpdateTime();
            ExpiredEntries_.erase(it);
        }
    }

    auto entry = New<TObjectServiceCacheEntry>(
        key,
        success,
        revision,
        TInstant::Now(),
        responseMessage,
        rate,
        lastUpdateTime);
    TouchEntry(entry);

    cookie.EndInsert(entry);
}

IYPathServicePtr TObjectServiceCache::GetOrchidService()
{
    auto producer = BIND(&TObjectServiceCache::DoBuildOrchid, MakeStrong(this));
    return IYPathService::FromProducer(producer);
}

TCacheProfilingCountersPtr TObjectServiceCache::GetProfilingCounters(const TString& user, const TString& method)
{
    auto key = std::make_tuple(user, method);

    {
        NConcurrency::TReaderGuard guard(Lock_);
        if (auto it = KeyToCounters_.find(key)) {
            return it->second;
        }
    }

    NProfiling::TTagIdList tagIds{
        NProfiling::TProfileManager::Get()->RegisterTag("user", user),
        NProfiling::TProfileManager::Get()->RegisterTag("method", method)
    };
    auto counters = New<TCacheProfilingCounters>(tagIds);

    {
        NConcurrency::TWriterGuard guard(Lock_);
        auto [it, inserted] = KeyToCounters_.emplace(key, std::move(counters));
        return it->second;
    }
}

bool TObjectServiceCache::IsResurrectionSupported() const
{
    return false;
}

void TObjectServiceCache::OnAdded(const TObjectServiceCacheEntryPtr& entry)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TAsyncSlruCacheBase::OnAdded(entry);

    const auto& key = entry->GetKey();
    YT_LOG_DEBUG("Cache entry added (Key: %v, Revision: %llx, Success: %v, TotalSpace: %v)",
        key,
        entry->GetRevision(),
        entry->GetSuccess(),
        entry->GetTotalSpace());
}

void TObjectServiceCache::OnRemoved(const TObjectServiceCacheEntryPtr& entry)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TAsyncSlruCacheBase::OnRemoved(entry);

    const auto& key = entry->GetKey();
    YT_LOG_DEBUG("Cache entry removed (Key: %v, Revision: %llx, Success: %v, TotalSpace: %v)",
        key,
        entry->GetRevision(),
        entry->GetSuccess(),
        entry->GetTotalSpace());

    TReaderGuard guard(ExpiredEntriesLock_);

    if (!ExpiredEntries_.contains(key)) {
        TWriterGuard guard(TopEntriesLock_);
        if (TopEntries_.erase(key) > 0) {
            YT_LOG_DEBUG("Removed entry from top (Key: %v)", key);
        }
    }
}

i64 TObjectServiceCache::GetWeight(const TObjectServiceCacheEntryPtr& entry) const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return entry->GetTotalSpace();
}

bool TObjectServiceCache::IsExpired(
    const TObjectServiceCacheEntryPtr& entry,
    TDuration successExpirationTime,
    TDuration failureExpirationTime)
{
    return
        TInstant::Now() > entry->GetTimestamp() +
        (entry->GetSuccess() ? successExpirationTime : failureExpirationTime);
}

void TObjectServiceCache::TouchEntry(const TObjectServiceCacheEntryPtr& entry)
{
    VERIFY_THREAD_AFFINITY_ANY();

    const auto& key = entry->GetKey();

    auto previous = entry->GetByteRate();
    entry->IncrementRate();
    auto current = entry->GetByteRate();

    if (previous < TopEntryByteRateThreshold_ && current >= TopEntryByteRateThreshold_) {
        TWriterGuard guard(TopEntriesLock_);

        if (entry->GetByteRate() >= TopEntryByteRateThreshold_) {
            if (TopEntries_.emplace(key, entry).second) {
                YT_LOG_DEBUG("Added entry to top (Key: %v, ByteRate: %v -> %v)",
                    key,
                    previous,
                    current);
            }
        }
    }

    if (previous >= TopEntryByteRateThreshold_ && current < TopEntryByteRateThreshold_) {
        TWriterGuard guard(TopEntriesLock_);

        if (entry->GetByteRate() < TopEntryByteRateThreshold_) {
            if (TopEntries_.erase(key) > 0) {
                YT_LOG_DEBUG("Removed entry from top (Key: %v, ByteRate: %v -> %v)",
                    key,
                    previous,
                    current);
            }
        }
    }
}

void TObjectServiceCache::DoBuildOrchid(IYsonConsumer* consumer)
{
    std::vector<std::pair<TObjectServiceCacheKey, TObjectServiceCacheEntryPtr>> top;
    {
        TReaderGuard guard(TopEntriesLock_);
        top = {TopEntries_.begin(), TopEntries_.end()};
    }

    std::sort(top.begin(), top.end(), [] (const auto& rhs, const auto& lhs) {
        return rhs.second->GetByteRate() > lhs.second->GetByteRate();
    });

    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("top_requests")
                .DoListFor(top, [&] (auto fluent, const auto& item) {
                    const auto& [key, entry] = item;
                    fluent
                        .Item().BeginMap()
                            .Item("cell_tag").Value(key.CellTag)
                            .Item("user").Value(key.User)
                            .Item("service").Value(key.Service)
                            .Item("method").Value(key.Method)
                            .Item("path").Value(key.Path)
                            .Item("request_body_hash").Value(key.RequestBodyHash)
                            .Item("byte_rate").Value(entry->GetByteRate())
                        .EndMap();
                })
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectClient
