#pragma once

#include "public.h"

#include <yt/yt/server/lib/misc/config.h>

#include <yt/yt/client/file_client/config.h>

#include <yt/yt/ytlib/hydra/config.h>

#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/library/tracing/jaeger/tracer.h>

#include <yt/yt/core/bus/tcp/config.h>

#include <yt/yt/core/net/address.h>

#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/node.h>
#include <yt/yt/core/ytree/yson_struct.h>

namespace NYT::NJobProxy {

////////////////////////////////////////////////////////////////////////////////

class TJobThrottlerConfig
    : public NYTree::TYsonStruct
{
public:
    TDuration MinBackoffTime;
    TDuration MaxBackoffTime;
    double BackoffMultiplier;

    TDuration RpcTimeout;

    REGISTER_YSON_STRUCT(TJobThrottlerConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TJobThrottlerConfig)

////////////////////////////////////////////////////////////////////////////////

class TCoreWatcherConfig
    : public NYTree::TYsonStruct
{
public:
    //! Cores lookup period.
    TDuration Period;

    //! Input/output operations timeout.
    TDuration IOTimeout;

    //! Finalization timeout.
    TDuration FinalizationTimeout;

    //! Cumulative timeout for cores processing.
    TDuration CoresProcessingTimeout;

    REGISTER_YSON_STRUCT(TCoreWatcherConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TCoreWatcherConfig)

////////////////////////////////////////////////////////////////////////////////

class TUserJobNetworkAddress
    : public NYTree::TYsonStruct
{
public:
    NNet::TIP6Address Address;

    TString Name;

    REGISTER_YSON_STRUCT(TUserJobNetworkAddress);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TUserJobNetworkAddress)

////////////////////////////////////////////////////////////////////////////////

class TTmpfsManagerConfig
    : public NYTree::TYsonStruct
{
public:
    std::vector<TString> TmpfsPaths;

    REGISTER_YSON_STRUCT(TTmpfsManagerConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TTmpfsManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TMemoryTrackerConfig
    : public NYTree::TYsonStruct
{
public:
    bool IncludeMemoryMappedFiles;

    bool UseSMapsMemoryTracker;

    TDuration MemoryStatisticsCachePeriod;

    REGISTER_YSON_STRUCT(TMemoryTrackerConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TMemoryTrackerConfig)

////////////////////////////////////////////////////////////////////////////////

class TBindConfig
    : public NYTree::TYsonStruct
{
public:
    TString ExternalPath;
    TString InternalPath;
    bool ReadOnly;

    REGISTER_YSON_STRUCT(TBindConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TBindConfig)

////////////////////////////////////////////////////////////////////////////////


class TJobProxyConfig
    : public TServerConfig
{
public:
    // Job-specific parameters.
    int SlotIndex = -1;

    TTmpfsManagerConfigPtr TmpfsManager;

    TMemoryTrackerConfigPtr MemoryTracker;

    std::vector<TBindConfigPtr> Binds;

    std::vector<TString> GpuDevices;

    //! Path for container root.
    std::optional<TString> RootPath;
    bool MakeRootFSWritable;

    //! Path to write stderr (for testing purposes).
    std::optional<TString> StderrPath;

    // Job-independent parameters.
    NApi::NNative::TConnectionConfigPtr ClusterConnection;

    NBus::TTcpBusClientConfigPtr SupervisorConnection;
    TDuration SupervisorRpcTimeout;

    TDuration HeartbeatPeriod;
    TDuration InputPipeBlinkerPeriod;

    NYTree::INodePtr JobEnvironment;

    //! Addresses derived from node local descriptor to leverage locality.
    NNodeTrackerClient::TAddressMap Addresses;
    TString LocalHostName;
    std::optional<TString> Rack;
    std::optional<TString> DataCenter;

    i64 AheadMemoryReserve;

    bool AlwaysAbortOnMemoryReserveOverdraft;

    bool TestRootFS;

    TJobThrottlerConfigPtr JobThrottler;

    //! Hostname to set in container.
    std::optional<TString> HostName;

    bool EnableNat64;

    //! Network addresses to bind into container.
    std::vector<TUserJobNetworkAddressPtr> NetworkAddresses;

    bool AbortOnUnrecognizedOptions;

    bool AbortOnUncaughtException;

    TCoreWatcherConfigPtr CoreWatcher;

    bool TestPollJobShell;

    //! If set, user job will not receive uid.
    //! For testing purposes only.
    bool DoNotSetUserId;

    //! This option can disable memory limit check for user jobs.
    //! Used in arcadia tests, since it's almost impossible to set
    //! proper memory limits for asan builds.
    bool CheckUserJobMemoryLimit;

    //! Compat option for urgent disable of job shell audit.
    bool EnableJobShellSeccopm;

    //! Enabled using porto kill for signalling instead of manual discovery of process pid.
    bool UsePortoKillForSignalling;

    bool ForceIdleCpuPolicy;

    bool UploadDebugArtifactChunks;

    REGISTER_YSON_STRUCT(TJobProxyConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TJobProxyConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobProxyDynamicConfig
    : public NYTree::TYsonStruct
{
public:
    NTracing::TJaegerTracerDynamicConfigPtr Jaeger;

    bool EnableJobShellSeccopm;

    bool UsePortoKillForSignalling;

    bool ForceIdleCpuPolicy;

    bool UploadDebugArtifactChunks;

    bool AbortOnUncaughtException;

    NYTree::INodePtr JobEnvironment;

    REGISTER_YSON_STRUCT(TJobProxyDynamicConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TJobProxyDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
