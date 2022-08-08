#include "config.h"

#include <yt/yt/client/api/client.h>

namespace NYT::NObjectClient {

////////////////////////////////////////////////////////////////////////////////

void TObjectAttributeCacheConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("read_from", &TThis::ReadFrom)
        .Default(NApi::EMasterChannelKind::Follower);
    registrar.Parameter("master_cache_expire_after_successful_update_time", &TThis::MasterCacheExpireAfterSuccessfulUpdateTime)
        .Default(TDuration::Seconds(15));
    registrar.Parameter("master_cache_expire_after_failed_update_time", &TThis::MasterCacheExpireAfterFailedUpdateTime)
        .Default(TDuration::Seconds(15));
    registrar.Parameter("master_cache_cache_sticky_group_size", &TThis::MasterCacheStickyGroupSize)
        .Default();
}

NApi::TMasterReadOptions TObjectAttributeCacheConfig::GetMasterReadOptions()
{
    return NApi::TMasterReadOptions{
        .ReadFrom = ReadFrom,
        .ExpireAfterSuccessfulUpdateTime = MasterCacheExpireAfterSuccessfulUpdateTime,
        .ExpireAfterFailedUpdateTime = MasterCacheExpireAfterFailedUpdateTime,
        .CacheStickyGroupSize = MasterCacheStickyGroupSize
    };
}

////////////////////////////////////////////////////////////////////////////////

void TObjectServiceCacheConfig::Register(TRegistrar registrar)
{
    registrar.Preprocessor([] (TThis* config) {
        config->Capacity = 1_GB;
    });

    registrar.Parameter("top_entry_byte_rate_threshold", &TThis::TopEntryByteRateThreshold)
        .Default(10_KB);
}

////////////////////////////////////////////////////////////////////////////////

void TObjectServiceCacheDynamicConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("top_entry_byte_rate_threshold", &TThis::TopEntryByteRateThreshold)
        .Optional();
}

////////////////////////////////////////////////////////////////////////////////

void TCachingObjectServiceConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cache_ttl_ratio", &TThis::CacheTtlRatio)
        .InRange(0, 1)
        .Default(0.5);
    registrar.Parameter("entry_byte_rate_limit", &TThis::EntryByteRateLimit)
        .GreaterThan(0)
        .Default(10_MB);
}

////////////////////////////////////////////////////////////////////////////////

void TCachingObjectServiceDynamicConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cache_ttl_ratio", &TThis::CacheTtlRatio)
        .InRange(0, 1)
        .Optional();
    registrar.Parameter("entry_byte_rate_limit", &TThis::EntryByteRateLimit)
        .GreaterThan(0)
        .Optional();
}

////////////////////////////////////////////////////////////////////////////////

void TReqExecuteBatchWithRetriesConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("base_backoff", &TThis::StartBackoff)
        .Default(TDuration::Seconds(1));
    registrar.Parameter("max_backoff", &TThis::MaxBackoff)
        .Default(TDuration::Seconds(20));
    registrar.Parameter("backoff_multiplier", &TThis::BackoffMultiplier)
        .GreaterThanOrEqual(1)
        .Default(2);
    registrar.Parameter("retry_count", &TThis::RetryCount)
        .GreaterThanOrEqual(0)
        .Default(5);
}

////////////////////////////////////////////////////////////////////////////////

void TAbcConfig::Register(TRegistrar registrar) {
    registrar.Parameter("id", &TThis::Id)
        .GreaterThan(0);
    registrar.Parameter("name", &TThis::Name)
        .Default()
        .NonEmpty();
    registrar.Parameter("slug", &TThis::Slug)
        .NonEmpty();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectClient
