#pragma once

#include "public.h"

#include <yt/yt/server/lib/job_proxy/config.h>

#include <yt/yt/server/lib/misc/config.h>

#include <yt/yt/ytlib/scheduler/helpers.h>

#include <yt/yt/core/ytree/yson_serializable.h>

#include <yt/yt/core/concurrency/config.h>

namespace NYT::NJobAgent {

////////////////////////////////////////////////////////////////////////////////

class TResourceLimitsConfig
    : public NYTree::TYsonStruct
{
public:
    int UserSlots;
    double Cpu;
    int Gpu;
    int Network;
    i64 UserMemory;
    i64 SystemMemory;
    int ReplicationSlots;
    i64 ReplicationDataSize;
    i64 MergeDataSize;
    int RemovalSlots;
    int RepairSlots;
    i64 RepairDataSize;
    int SealSlots;
    int MergeSlots;
    int AutotomySlots;
    int ReincarnationSlots;

    REGISTER_YSON_STRUCT(TResourceLimitsConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TResourceLimitsConfig)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EGpuInfoSourceType,
    (NvGpuManager)
    (NvidiaSmi)
);

class TGpuInfoSourceConfig
    : public NYTree::TYsonStruct
{
public:
    EGpuInfoSourceType Type;
    TString NvGpuManagerServiceAddress;
    TString NvGpuManagerServiceName;

    REGISTER_YSON_STRUCT(TGpuInfoSourceConfig);
    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TGpuInfoSourceConfig)

class TGpuManagerConfig
    : public NYTree::TYsonStruct
{
public:
    bool Enable;

    TDuration HealthCheckTimeout;
    TDuration HealthCheckPeriod;

    TDuration HealthCheckFailureBackoff;

    std::optional<TShellCommandConfigPtr> JobSetupCommand;

    std::optional<NYPath::TYPath> DriverLayerDirectoryPath;
    std::optional<TString> DriverVersion;
    TDuration DriverLayerFetchPeriod;
    TDuration DriverLayerFetchPeriodSplay;

    THashMap<TString, TString> CudaToolkitMinDriverVersion;

    TGpuInfoSourceConfigPtr GpuInfoSource;

    //! This is a special testing option.
    //! Instead of normal gpu discovery, it forces the node to believe the number of GPUs passed in the config.
    bool TestResource;
    //! These options enable testing gpu layers and setup commands.
    bool TestLayers;
    bool TestSetupCommands;
    bool TestExtraGpuCheckCommandFailure;

    int TestGpuCount;

    REGISTER_YSON_STRUCT(TGpuManagerConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TGpuManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TGpuManagerDynamicConfig
    : public NYTree::TYsonStruct
{
public:
    std::optional<TDuration> HealthCheckTimeout;
    std::optional<TDuration> HealthCheckPeriod;
    std::optional<TDuration> HealthCheckFailureBackoff;

    std::optional<TDuration> DriverLayerFetchPeriod;

    std::optional<THashMap<TString, TString>> CudaToolkitMinDriverVersion;

    TGpuInfoSourceConfigPtr GpuInfoSource;

    REGISTER_YSON_STRUCT(TGpuManagerDynamicConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TGpuManagerDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

class TShellCommandConfig
    : public NYTree::TYsonStruct
{
public:
    TString Path;
    std::vector<TString> Args;

    REGISTER_YSON_STRUCT(TShellCommandConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TShellCommandConfig)

////////////////////////////////////////////////////////////////////////////////

class TMappedMemoryControllerConfig
    : public NYTree::TYsonStruct
{
public:
    TDuration CheckPeriod;
    i64 ReservedMemory;

    REGISTER_YSON_STRUCT(TMappedMemoryControllerConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TMappedMemoryControllerConfig)

////////////////////////////////////////////////////////////////////////////////

class TMemoryPressureDetectorConfig
    : public NYTree::TYsonStruct
{
public:
    bool Enabled;

    TDuration CheckPeriod;

    // Free memory watermark multiplier will be increased upon reaching this threshold.
    int MajorPageFaultCountThreshold;

    // The value by which free memory watermark multiplier is increased.
    double MemoryWatermarkMultiplierIncreaseStep;

    // Max value of free memory watermark multiplier.
    double MaxMemoryWatermarkMultiplier;

    REGISTER_YSON_STRUCT(TMemoryPressureDetectorConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TMemoryPressureDetectorConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobControllerDynamicConfig
    : public NYTree::TYsonStruct
{
public:
    std::optional<TDuration> GetJobSpecsTimeout;
    std::optional<TDuration> TotalConfirmationPeriod;

    std::optional<TDuration> CpuOverdraftTimeout;
    std::optional<double> CpuToVCpuFactor;
    bool EnableCpuToVCpuFactor;
    std::optional<THashMap<TString, double>> CpuModelToCpuToVCpuFactor;
    std::optional<TDuration> MemoryOverdraftTimeout;

    std::optional<TDuration> ProfilingPeriod;

    std::optional<TDuration> ResourceAdjustmentPeriod;

    std::optional<TDuration> RecentlyRemovedJobsCleanPeriod;
    std::optional<TDuration> RecentlyRemovedJobsStoreTimeout;

    std::optional<TDuration> JobProxyBuildInfoUpdatePeriod;

    std::optional<bool> DisableJobProxyProfiling;

    TGpuManagerDynamicConfigPtr GpuManager;

    NJobProxy::TJobProxyDynamicConfigPtr JobProxy;

    TMemoryPressureDetectorConfigPtr MemoryPressureDetector;

    TDuration OperationInfosRequestPeriod;

    REGISTER_YSON_STRUCT(TJobControllerDynamicConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TJobControllerDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobControllerConfig
    : public NYTree::TYsonStruct
{
public:
    TResourceLimitsConfigPtr ResourceLimits;
    TDuration WaitingJobsTimeout;
    TDuration GetJobSpecsTimeout;
    TDuration TotalConfirmationPeriod;

    TDuration CpuOverdraftTimeout;
    TDuration MemoryOverdraftTimeout;

    TDuration ProfilingPeriod;

    TDuration ResourceAdjustmentPeriod;

    TDuration RecentlyRemovedJobsCleanPeriod;
    TDuration RecentlyRemovedJobsStoreTimeout;

    i64 FreeMemoryWatermark;

    double CpuPerTabletSlot;

    std::optional<double> CpuToVCpuFactor;
    std::optional<TString> CpuModel;

    //! Port set has higher priority than StartPort ans PortCount if it is specified.
    int StartPort;
    int PortCount;
    std::optional<THashSet<int>> PortSet;

    TGpuManagerConfigPtr GpuManager;

    TMappedMemoryControllerConfigPtr MappedMemoryController;

    std::optional<TShellCommandConfigPtr> JobSetupCommand;
    TString SetupCommandUser;

    TDuration JobProxyBuildInfoUpdatePeriod;

    bool DisableJobProxyProfiling;

    REGISTER_YSON_STRUCT(TJobControllerConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TJobControllerConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobReporterDynamicConfig
    : public NYTree::TYsonStruct
{
public:
    std::optional<bool> EnableJobReporter;
    std::optional<bool> EnableJobSpecReporter;
    std::optional<bool> EnableJobStderrReporter;
    std::optional<bool> EnableJobProfileReporter;
    std::optional<bool> EnableJobFailContextReporter;

    REGISTER_YSON_STRUCT(TJobReporterDynamicConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TJobReporterDynamicConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobReporterConfig
    : public TArchiveReporterConfig
{
public:
    TArchiveHandlerConfigPtr JobHandler;
    TArchiveHandlerConfigPtr OperationIdHandler;
    TArchiveHandlerConfigPtr JobSpecHandler;
    TArchiveHandlerConfigPtr JobStderrHandler;
    TArchiveHandlerConfigPtr JobFailContextHandler;
    TArchiveHandlerConfigPtr JobProfileHandler;

    TString User;
    bool ReportStatisticsLz4;

    // COMPAT(dakovalkov): Delete these when all job reporter configs are in new format.
    std::optional<int> MaxInProgressJobDataSize;
    std::optional<int> MaxInProgressOperationIdDataSize;
    std::optional<int> MaxInProgressJobSpecDataSize;
    std::optional<int> MaxInProgressJobStderrDataSize;
    std::optional<int> MaxInProgressJobFailContextDataSize;

    // Enables job reporter to send job events/statistics etc.
    bool EnableJobReporter;

    // Enables job reporter to send job specs.
    bool EnableJobSpecReporter;

    // Enables job reporter to send job stderrs.
    bool EnableJobStderrReporter;

    // Enables job reporter to send job profiles.
    bool EnableJobProfileReporter;

    // Enables job reporter to send job fail contexts.
    bool EnableJobFailContextReporter;

    REGISTER_YSON_STRUCT(TJobReporterConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TJobReporterConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent
