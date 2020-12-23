#pragma once

#include "public.h"

#include <yt/core/compression/public.h>

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/concurrency/config.h>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

// Common options shared between all services in one server.
class TServiceCommonConfig
    : public NYTree::TYsonSerializable
{
public:
    bool EnablePerUserProfiling;
    bool ForceTracing;

    TServiceCommonConfig()
    {
        RegisterParameter("enable_per_user_profiling", EnablePerUserProfiling)
            .Default(false);
        RegisterParameter("force_tracing", ForceTracing)
            .Default(false);
    }
};

DEFINE_REFCOUNTED_TYPE(TServiceCommonConfig)

////////////////////////////////////////////////////////////////////////////////

class TServerConfig
    : public TServiceCommonConfig
{
public:
    THashMap<TString, NYTree::INodePtr> Services;

    TServerConfig()
    {
        RegisterParameter("services", Services)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TServerConfig)

////////////////////////////////////////////////////////////////////////////////

class TServiceConfig
    : public NYTree::TYsonSerializable
{
public:
    std::optional<bool> EnablePerUserProfiling;
    std::optional<bool> ForceTracing;

    THashMap<TString, TMethodConfigPtr> Methods;

    static const int DefaultAuthenticationQueueSizeLimit;
    int AuthenticationQueueSizeLimit;

    static const TDuration DefaultPendingPayloadsTimeout;
    TDuration PendingPayloadsTimeout;

    TServiceConfig()
    {
        RegisterParameter("enable_per_user_profiling", EnablePerUserProfiling)
            .Optional();
        RegisterParameter("force_tracing", ForceTracing)
            .Optional();
        RegisterParameter("methods", Methods)
            .Optional();
        RegisterParameter("authentication_queue_size_limit", AuthenticationQueueSizeLimit)
            .Alias("max_authentication_queue_size")
            .Default(DefaultAuthenticationQueueSizeLimit);
        RegisterParameter("pending_payloads_timeout", PendingPayloadsTimeout)
            .Default(DefaultPendingPayloadsTimeout);
    }
};

DEFINE_REFCOUNTED_TYPE(TServiceConfig)

////////////////////////////////////////////////////////////////////////////////

class TMethodConfig
    : public NYTree::TYsonSerializable
{
public:
    static const bool DefaultHeavy;
    bool Heavy;

    static const int DefaultQueueSizeLimit;
    int QueueSizeLimit;

    static const int DefaultConcurrencyLimit;
    int ConcurrencyLimit;

    static const NLogging::ELogLevel DefaultLogLevel;
    NLogging::ELogLevel LogLevel;

    static const TDuration DefaultLoggingSuppressionTimeout;
    TDuration LoggingSuppressionTimeout;

    NConcurrency::TThroughputThrottlerConfigPtr RequestBytesThrottler;

    static const NConcurrency::TThroughputThrottlerConfigPtr DefaultLoggingSuppressionFailedRequestThrottler;
    NConcurrency::TThroughputThrottlerConfigPtr LoggingSuppressionFailedRequestThrottler;

    std::optional<bool> ForceTracing;

    TMethodConfig()
    {
        RegisterParameter("heavy", Heavy)
            .Default(DefaultHeavy);
        RegisterParameter("queue_size_limit", QueueSizeLimit)
            .Alias("max_queue_size")
            .Default(DefaultQueueSizeLimit);
        RegisterParameter("concurrency_limit", ConcurrencyLimit)
            .Alias("max_concurrency")
            .Default(DefaultConcurrencyLimit);
        RegisterParameter("log_level", LogLevel)
            .Default(DefaultLogLevel);
        RegisterParameter("request_bytes_throttler", RequestBytesThrottler)
            .Default();
        RegisterParameter("logging_suppression_timeout", LoggingSuppressionTimeout)
            .Default(DefaultLoggingSuppressionTimeout);
        RegisterParameter("logging_suppression_failed_request_throttler", LoggingSuppressionFailedRequestThrottler)
            .Default(DefaultLoggingSuppressionFailedRequestThrottler);
        RegisterParameter("force_tracing", ForceTracing)
            .Optional();
    }
};

DEFINE_REFCOUNTED_TYPE(TMethodConfig)

////////////////////////////////////////////////////////////////////////////////

class TRetryingChannelConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Time to wait between consequent attempts.
    TDuration RetryBackoffTime;

    //! Maximum number of retry attempts to make.
    int RetryAttempts;

    //! Maximum time to spend while retrying.
    //! If null then no limit is enforced.
    std::optional<TDuration> RetryTimeout;

    TRetryingChannelConfig()
    {
        RegisterParameter("retry_backoff_time", RetryBackoffTime)
            .Default(TDuration::Seconds(3));
        RegisterParameter("retry_attempts", RetryAttempts)
            .GreaterThanOrEqual(1)
            .Default(10);
        RegisterParameter("retry_timeout", RetryTimeout)
            .GreaterThanOrEqual(TDuration::Zero())
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TRetryingChannelConfig)

////////////////////////////////////////////////////////////////////////////////

class TBalancingChannelConfigBase
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Timeout for |Discover| requests.
    TDuration DiscoverTimeout;

    //! Timeout for acknowledgement of all RPC requests going through the channel.
    TDuration AcknowledgementTimeout;

    //! Interval between automatic rediscovery of active peers.
    /*!
     *  Discovery is started automatically if no active peers are known.
     *  In some cases, however, this is not enough.
     *  E.g. a follower may become active and thus eligible for load balancing.
     *  This setting controls the period of time after which the channel
     *  starts rediscovering peers even if an active one is known.
     */
    TDuration RediscoverPeriod;

    //! A random duration from 0 to #RediscoverSplay is added to #RediscoverPeriod on each
    //! rediscovery attempt.
    TDuration RediscoverSplay;

    //! Time between consequent attempts to reconnect to a peer, which
    //! returns a hard failure (i.e. non-OK response) to |Discover| request.
    TDuration HardBackoffTime;

    //! Time between consequent attempts to reconnect to a peer, which
    //! returns a soft failure (i.e. "down" response) to |Discover| request.
    TDuration SoftBackoffTime;

    TBalancingChannelConfigBase()
    {
        RegisterParameter("discover_timeout", DiscoverTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("acknowledgement_timeout", AcknowledgementTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("rediscover_period", RediscoverPeriod)
            .Default(TDuration::Seconds(60));
        RegisterParameter("rediscover_splay", RediscoverSplay)
            .Default(TDuration::Seconds(15));
        RegisterParameter("hard_backoff_time", HardBackoffTime)
            .Default(TDuration::Seconds(60));
        RegisterParameter("soft_backoff_time", SoftBackoffTime)
            .Default(TDuration::Seconds(15));
    }
};

////////////////////////////////////////////////////////////////////////////////

class TDynamicChannelPoolConfig
    : public TBalancingChannelConfigBase
{
public:
    //! Maximum number of peers to query in parallel when locating alive ones.
    int MaxConcurrentDiscoverRequests;

    //! For sticky mode: number of consistent hash tokens to assign to each peer.
    int HashesPerPeer;

    //! In case too many peers are known, the pool will only maintain this many peers.
    int MaxPeerCount;

    //! When more than #MaxPeerCount peers are known an attempt to add more is
    //! typically ignored. To avoid stucking with the same peer set forever, one
    //! random peer could be evicted after #RandomPeerEvictionPeriod.
    TDuration RandomPeerEvictionPeriod;

    TDynamicChannelPoolConfig()
    {
        RegisterParameter("max_concurrent_discover_requests", MaxConcurrentDiscoverRequests)
            .GreaterThan(0)
            .Default(10);
        RegisterParameter("hashes_per_peer", HashesPerPeer)
            .GreaterThan(0)
            .Default(10);
        RegisterParameter("max_peer_count", MaxPeerCount)
            .GreaterThan(1)
            .Default(100);
        RegisterParameter("random_peer_eviction_period", RandomPeerEvictionPeriod)
            .Default(TDuration::Minutes(1));
    }
};

DEFINE_REFCOUNTED_TYPE(TDynamicChannelPoolConfig)

////////////////////////////////////////////////////////////////////////////////

class TBalancingChannelConfig
    : public TDynamicChannelPoolConfig
{
public:
    //! List of seed addresses.
    std::vector<TString> Addresses;

    TBalancingChannelConfig()
    {
        RegisterParameter("addresses", Addresses);
    }
};

DEFINE_REFCOUNTED_TYPE(TBalancingChannelConfig)

////////////////////////////////////////////////////////////////////////////////

class TThrottlingChannelConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Maximum allowed number of requests per second.
    int RateLimit;

    TThrottlingChannelConfig()
    {
        RegisterParameter("rate_limit", RateLimit)
            .GreaterThan(0)
            .Default(10);
    }
};

DEFINE_REFCOUNTED_TYPE(TThrottlingChannelConfig)

////////////////////////////////////////////////////////////////////////////////

class TResponseKeeperConfig
    : public NYTree::TYsonSerializable
{
public:
    //! For how long responses are kept in memory.
    TDuration ExpirationTime;

    //! If |true| then initial warmup is enabled. In particular, #WarmupTime and #ExpirationTime are
    //! checked against each other. If |false| then initial warmup is disabled and #WarmupTime is ignored.
    bool EnableWarmup;

    //! For how long the keeper remains passive after start and merely collects all responses.
    TDuration WarmupTime;

    TResponseKeeperConfig()
    {
        RegisterParameter("expiration_time", ExpirationTime)
            .Default(TDuration::Minutes(5));
        RegisterParameter("enable_warmup", EnableWarmup)
            .Default(true);
        RegisterParameter("warmup_time", WarmupTime)
            .Default(TDuration::Minutes(6));
        RegisterPostprocessor([&] () {
            if (EnableWarmup && WarmupTime < ExpirationTime) {
                THROW_ERROR_EXCEPTION("\"warmup_time\" cannot be less than \"expiration_time\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TResponseKeeperConfig)

////////////////////////////////////////////////////////////////////////////////

class TMultiplexingBandConfig
    : public NYTree::TYsonSerializable
{
public:
    int TosLevel;
    THashMap<TString, int> NetworkToTosLevel;

    TMultiplexingBandConfig()
    {
        RegisterParameter("tos_level", TosLevel)
            .Default(NYT::NBus::DefaultTosLevel);

        RegisterParameter("network_to_tos_level", NetworkToTosLevel)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TMultiplexingBandConfig)

////////////////////////////////////////////////////////////////////////////////

class TDispatcherConfig
    : public NYTree::TYsonSerializable
{
public:
    static constexpr int DefaultHeavyPoolSize = 16;
    static constexpr int DefaultCompressionPoolSize = 8;
    int HeavyPoolSize;
    int CompressionPoolSize;

    TEnumIndexedVector<EMultiplexingBand, TMultiplexingBandConfigPtr> MultiplexingBands;

    TDispatcherConfig();
    TDispatcherConfigPtr ApplyDynamic(const TDispatcherDynamicConfigPtr& dynamicConfig) const;
};

DEFINE_REFCOUNTED_TYPE(TDispatcherConfig)

////////////////////////////////////////////////////////////////////////////////

class TDispatcherDynamicConfig
    : public NYTree::TYsonSerializable
{
public:
    std::optional<int> HeavyPoolSize;
    std::optional<int> CompressionPoolSize;

    std::optional<TEnumIndexedVector<EMultiplexingBand, TMultiplexingBandConfigPtr>> MultiplexingBands;

    TDispatcherDynamicConfig();
};

DEFINE_REFCOUNTED_TYPE(TDispatcherDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
