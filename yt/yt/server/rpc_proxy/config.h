#pragma once

#include "public.h"

#include <yt/yt/server/lib/rpc_proxy/config.h>

#include <yt/yt/server/lib/dynamic_config/config.h>

#include <yt/yt/server/lib/misc/config.h>

#include <yt/yt/ytlib/auth/config.h>

#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/ytlib/security_client/config.h>

#include <yt/yt/client/api/config.h>

#include <yt/yt/client/formats/public.h>

#include <yt/yt/core/misc/config.h>

#include <yt/yt/core/rpc/grpc/config.h>

#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/library/tracing/jaeger/sampler.h>

namespace NYT::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

class TDiscoveryServiceConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    bool Enable;
    TDuration LivenessUpdatePeriod;
    TDuration ProxyUpdatePeriod;
    TDuration AvailabilityPeriod;
    TDuration BackoffPeriod;

    TDiscoveryServiceConfig()
    {
        RegisterParameter("enable", Enable)
            .Default(true);
        RegisterParameter("liveness_update_period", LivenessUpdatePeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("proxy_update_period", ProxyUpdatePeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("availability_period", AvailabilityPeriod)
            .Default(TDuration::Seconds(15))
            .GreaterThan(LivenessUpdatePeriod);
        RegisterParameter("backoff_period", BackoffPeriod)
            .Default(TDuration::Seconds(60))
            .GreaterThan(AvailabilityPeriod);
    }
};

DEFINE_REFCOUNTED_TYPE(TDiscoveryServiceConfig)

////////////////////////////////////////////////////////////////////////////////

class TAccessCheckerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Whether access checker is enabled.
    bool Enabled;

    //! Access checker will check use permission for
    //! PathPrefix/ProxyRole path.
    TString PathPrefix;

    //! Parameters of the permission cache.
    NSecurityClient::TPermissionCacheConfigPtr Cache;

    TAccessCheckerConfig()
    {
        RegisterParameter("enabled", Enabled)
            .Default(false);

        RegisterParameter("path_prefix", PathPrefix)
            .Default("//sys/rpc_proxy_roles");

        RegisterParameter("cache", Cache)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TAccessCheckerConfig)

////////////////////////////////////////////////////////////////////////////////

class TAccessCheckerDynamicConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Whether access checker is enabled.
    std::optional<bool> Enabled;

    TAccessCheckerDynamicConfig()
    {
        RegisterParameter("enabled", Enabled)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TAccessCheckerDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

class TProxyConfig
    : public TDeprecatedServerConfig
    , public NAuth::TAuthenticationManagerConfig
{
public:
    //! Proxy-to-master connection.
    NApi::NNative::TConnectionConfigPtr ClusterConnection;
    TApiServiceConfigPtr ApiService;
    TDiscoveryServiceConfigPtr DiscoveryService;
    //! Known RPC proxy addresses.
    NNodeTrackerClient::TNetworkAddressList Addresses;
    int WorkerThreadPoolSize;

    TAccessCheckerConfigPtr AccessChecker;

    //! GRPC server configuration.
    NRpc::NGrpc::TServerConfigPtr GrpcServer;

    NYTree::IMapNodePtr CypressAnnotations;

    bool AbortOnUnrecognizedOptions;
    //! For testing purposes.
    bool RetryRequestQueueSizeLimitExceeded;

    NDynamicConfig::TDynamicConfigManagerConfigPtr DynamicConfigManager;

    // COMPAT(gritukan): Drop it after migration to tagged configs.
    TString DynamicConfigPath;
    bool UseTaggedDynamicConfig;

    TProxyConfig()
    {
        RegisterParameter("cluster_connection", ClusterConnection)
            .Default();

        RegisterParameter("grpc_server", GrpcServer)
            .Default();
        RegisterParameter("api_service", ApiService)
            .DefaultNew();
        RegisterParameter("discovery_service", DiscoveryService)
            .DefaultNew();
        RegisterParameter("addresses", Addresses)
            .Default();
        RegisterParameter("worker_thread_pool_size", WorkerThreadPoolSize)
            .GreaterThan(0)
            .Default(8);

        RegisterParameter("access_checker", AccessChecker)
            .DefaultNew();

        RegisterParameter("cypress_annotations", CypressAnnotations)
            .Default(NYTree::BuildYsonNodeFluently()
                .BeginMap()
                .EndMap()
            ->AsMap());

        RegisterParameter("abort_on_unrecognized_options", AbortOnUnrecognizedOptions)
            .Default(false);

        RegisterParameter("retry_request_queue_size_limit_exceeded", RetryRequestQueueSizeLimitExceeded)
            .Default(true);

        RegisterParameter("dynamic_config_manager", DynamicConfigManager)
            .DefaultNew();

        RegisterParameter("dynamic_config_path", DynamicConfigPath)
            .Default("//sys/rpc_proxies/@config");
        RegisterParameter("use_tagged_dynamic_config", UseTaggedDynamicConfig)
            .Default(false);

        RegisterPostprocessor([&] {
            if (GrpcServer && GrpcServer->Addresses.size() > 1) {
                THROW_ERROR_EXCEPTION("Multiple GRPC addresses are not supported");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TProxyConfig)

////////////////////////////////////////////////////////////////////////////////

class TProxyDynamicConfig
    : public TDeprecatedSingletonsDynamicConfig
{
public:
    TApiServiceDynamicConfigPtr Api;

    NTracing::TSamplerConfigPtr Tracing;
    THashMap<NFormats::EFormatType, TFormatConfigPtr> Formats;

    TAccessCheckerDynamicConfigPtr AccessChecker;

    TProxyDynamicConfig()
    {
        RegisterParameter("api", Api)
            .DefaultNew();

        RegisterParameter("tracing", Tracing)
            .DefaultNew();
        RegisterParameter("formats", Formats)
            .Default();

        RegisterParameter("access_checker", AccessChecker)
            .DefaultNew();

        // COMPAT(gritukan, levysotsky)
        RegisterPostprocessor([&] {
            if (Api->Formats.empty()) {
                Api->Formats = Formats;
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TProxyDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpcProxy
