#pragma once

#include "public.h"

#include <yt/yt/server/lib/exec_node/config.h>

#include <yt/yt/server/lib/misc/config.h>

#include <yt/yt/client/file_client/config.h>

#include <yt/yt/ytlib/hydra/config.h>

#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/library/tracing/jaeger/tracer.h>

#include <yt/yt/core/bus/tcp/config.h>

#include <yt/yt/core/net/address.h>

#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/node.h>
#include <yt/yt/core/ytree/yson_serializable.h>

namespace NYT::NJobProxy {

////////////////////////////////////////////////////////////////////////////////

class TJobThrottlerConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration MinBackoffTime;
    TDuration MaxBackoffTime;
    double BackoffMultiplier;

    TDuration RpcTimeout;

    TJobThrottlerConfig()
    {
        RegisterParameter("min_backoff_time", MinBackoffTime)
            .Default(TDuration::MilliSeconds(100));

        RegisterParameter("max_backoff_time", MaxBackoffTime)
            .Default(TDuration::Seconds(60));

        RegisterParameter("backoff_multiplier", BackoffMultiplier)
            .Default(1.5);

        RegisterParameter("rpc_timeout", RpcTimeout)
            .Default(TDuration::Seconds(60));
    }
};

DEFINE_REFCOUNTED_TYPE(TJobThrottlerConfig)

////////////////////////////////////////////////////////////////////////////////

class TCoreWatcherConfig
    : public NYTree::TYsonSerializable
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

    TCoreWatcherConfig()
    {
        RegisterParameter("period", Period)
            .Default(TDuration::Seconds(5))
            .GreaterThan(TDuration::Zero());
        RegisterParameter("io_timeout", IOTimeout)
            .Default(TDuration::Seconds(60))
            .GreaterThan(TDuration::Zero());
        RegisterParameter("finalization_timeout", FinalizationTimeout)
            .Default(TDuration::Seconds(60))
            .GreaterThan(TDuration::Zero());
        RegisterParameter("cores_processing_timeout", CoresProcessingTimeout)
            .Default(TDuration::Minutes(15))
            .GreaterThan(TDuration::Zero());
    }
};

DEFINE_REFCOUNTED_TYPE(TCoreWatcherConfig)

////////////////////////////////////////////////////////////////////////////////

class TUserJobNetworkAddress
    : public NYTree::TYsonSerializable
{
public:
    NNet::TIP6Address Address;

    TString Name;

    TUserJobNetworkAddress()
    {
        RegisterParameter("address", Address)
            .Default();

        RegisterParameter("name", Name)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TUserJobNetworkAddress)

////////////////////////////////////////////////////////////////////////////////

class TTmpfsManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    std::vector<TString> TmpfsPaths;

    TTmpfsManagerConfig()
    {
        RegisterParameter("tmpfs_paths", TmpfsPaths)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TTmpfsManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TMemoryTrackerConfig
    : public NYTree::TYsonSerializable
{
public:
    bool IncludeMemoryMappedFiles;

    bool UseSMapsMemoryTracker;

    TDuration MemoryStatisticsCachePeriod;

    TMemoryTrackerConfig()
    {
        RegisterParameter("include_memory_mapped_files", IncludeMemoryMappedFiles)
            .Default(true);

        RegisterParameter("use_smaps_memory_tracker", UseSMapsMemoryTracker)
            .Default(false);

        RegisterParameter("memory_statisitcs_cache_period", MemoryStatisticsCachePeriod)
            .Default(TDuration::Zero());
    }
};

DEFINE_REFCOUNTED_TYPE(TMemoryTrackerConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobProxyConfig
    : public TDeprecatedServerConfig
{
public:
    // Job-specific parameters.
    int SlotIndex = -1;

    TTmpfsManagerConfigPtr TmpfsManager;

    TMemoryTrackerConfigPtr MemoryTracker;

    std::vector<NExecNode::TBindConfigPtr> Binds;

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

    bool TestRootFS;

    TJobThrottlerConfigPtr JobThrottler;

    //! Hostname to set in container.
    std::optional<TString> HostName;

    bool EnableNat64;

    //! Network addresses to bind into container.
    std::vector<TUserJobNetworkAddressPtr> NetworkAddresses;

    bool AbortOnUnrecognizedOptions;

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

    TJobProxyConfig()
    {
        RegisterParameter("slot_index", SlotIndex);

        RegisterParameter("tmpfs_manager", TmpfsManager)
            .DefaultNew();

        RegisterParameter("memory_tracker", MemoryTracker)
            .DefaultNew();

        RegisterParameter("root_path", RootPath)
            .Default();

        RegisterParameter("stderr_path", StderrPath)
            .Default();

        RegisterParameter("make_rootfs_writable", MakeRootFSWritable)
            .Default(false);

        RegisterParameter("binds", Binds)
            .Default();

        RegisterParameter("gpu_devices", GpuDevices)
            .Default();

        RegisterParameter("cluster_connection", ClusterConnection);

        RegisterParameter("supervisor_connection", SupervisorConnection);

        RegisterParameter("supervisor_rpc_timeout", SupervisorRpcTimeout)
            .Default(TDuration::Seconds(30));

        RegisterParameter("heartbeat_period", HeartbeatPeriod)
            .Default(TDuration::Seconds(5));

        RegisterParameter("input_pipe_blinker_period", InputPipeBlinkerPeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("job_environment", JobEnvironment);

        RegisterParameter("addresses", Addresses)
            .Default();

        RegisterParameter("local_host_name", LocalHostName)
            .Default();

        RegisterParameter("rack", Rack)
            .Default();

        RegisterParameter("data_center", DataCenter)
            .Default();

        RegisterParameter("ahead_memory_reserve", AheadMemoryReserve)
            .Default(100_MB);

        RegisterParameter("test_root_fs", TestRootFS)
            .Default(false);

        RegisterParameter("job_throttler", JobThrottler)
            .Default(nullptr);

        RegisterParameter("host_name", HostName)
            .Default();

        RegisterParameter("enable_nat64", EnableNat64)
            .Default(false);

        RegisterParameter("network_addresses", NetworkAddresses)
            .Default();

        RegisterParameter("abort_on_unrecognized_options", AbortOnUnrecognizedOptions)
            .Default(false);

        RegisterParameter("core_watcher", CoreWatcher)
            .DefaultNew();

        RegisterParameter("test_poll_job_shell", TestPollJobShell)
            .Default(false);

        RegisterParameter("do_not_set_user_id", DoNotSetUserId)
            .Default(false);

        RegisterParameter("check_user_job_memory_limit", CheckUserJobMemoryLimit)
            .Default(true);

        RegisterParameter("enable_job_shell_seccomp", EnableJobShellSeccopm)
            .Default(true);

        RegisterParameter("use_porto_kill_for_signalling", UsePortoKillForSignalling)
            .Default(false);

        RegisterParameter("force_idle_cpu_policy", ForceIdleCpuPolicy)
            .Default(false);

        RegisterParameter("upload_debug_artifact_chunks", UploadDebugArtifactChunks)
            .Default(true);

        RegisterPreprocessor([&] {
            SolomonExporter->EnableSelfProfiling = false;
            SolomonExporter->WindowSize = 1;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TJobProxyConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobTestingOptions
    : public NYTree::TYsonSerializable
{
public:
    std::optional<TDuration> DelayAfterNodeDirectoryPrepared;
    bool FailBeforeJobStart;
    bool ThrowInShallowMerge;

    TJobTestingOptions()
    {
        RegisterParameter("delay_after_node_directory_prepared", DelayAfterNodeDirectoryPrepared)
            .Default();
        RegisterParameter("fail_before_job_start", FailBeforeJobStart)
            .Default(false);
        RegisterParameter("throw_in_shallow_merge", ThrowInShallowMerge)
            .Default(false);
    }
};

DEFINE_REFCOUNTED_TYPE(TJobTestingOptions)

////////////////////////////////////////////////////////////////////////////////

class TJobProxyDynamicConfig
    : public NYTree::TYsonSerializable
{
public:
    NTracing::TJaegerTracerDynamicConfigPtr Jaeger;

    bool EnableJobShellSeccopm;

    bool UsePortoKillForSignalling;

    bool ForceIdleCpuPolicy;

    bool UploadDebugArtifactChunks;

    TJobProxyDynamicConfig()
    {
        RegisterParameter("jaeger", Jaeger)
            .DefaultNew();

        RegisterParameter("enable_job_shell_seccomp", EnableJobShellSeccopm)
            .Default(true);

        RegisterParameter("use_porto_kill_for_signalling", UsePortoKillForSignalling)
            .Default(false);

        RegisterParameter("force_idle_cpu_policy", ForceIdleCpuPolicy)
            .Default(false);

        RegisterParameter("upload_debug_artifact_chunks", UploadDebugArtifactChunks)
            .Default(true);
    }
};

DEFINE_REFCOUNTED_TYPE(TJobProxyDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
