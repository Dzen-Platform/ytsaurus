#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/concurrency/config.h>

namespace NYT {
namespace NJobAgent {

////////////////////////////////////////////////////////////////////////////////

class TResourceLimitsConfig
    : public NYTree::TYsonSerializable
{
public:
    int UserSlots;
    int Cpu;
    int Network;
    i64 Memory;
    int ReplicationSlots;
    i64 ReplicationDataSize;
    int RemovalSlots;
    int RepairSlots;
    i64 RepairDataSize;
    int SealSlots;

    TResourceLimitsConfig()
    {
        // These are some very low default limits.
        // Override for production use.
        RegisterParameter("user_slots", UserSlots)
            .GreaterThanOrEqual(0)
            .Default(1);
        RegisterParameter("cpu", Cpu)
            .GreaterThanOrEqual(0)
            .Default(1);
        RegisterParameter("network", Network)
            .GreaterThanOrEqual(0)
            .Default(100);
        RegisterParameter("memory", Memory)
            .GreaterThanOrEqual(0)
            .Default(std::numeric_limits<i64>::max());
        RegisterParameter("replication_slots", ReplicationSlots)
            .GreaterThanOrEqual(0)
            .Default(16);
        RegisterParameter("replication_data_size", ReplicationDataSize)
            .Default((i64) 10 * 1024 * 1024 * 1024)
            .GreaterThanOrEqual(0);
        RegisterParameter("removal_slots", RemovalSlots)
            .GreaterThanOrEqual(0)
            .Default(16);
        RegisterParameter("repair_slots", RepairSlots)
            .GreaterThanOrEqual(0)
            .Default(4);
        RegisterParameter("repair_data_size", RepairDataSize)
            .Default((i64) 4 * 1024 * 1024 * 1024)
            .GreaterThanOrEqual(0);
        RegisterParameter("seal_slots", SealSlots)
            .GreaterThanOrEqual(0)
            .Default(16);
    }
};

DEFINE_REFCOUNTED_TYPE(TResourceLimitsConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobControllerConfig
    : public NYTree::TYsonSerializable
{
public:
    TResourceLimitsConfigPtr ResourceLimits;
    NConcurrency::TThroughputThrottlerConfigPtr StatisticsThrottler;
    TDuration WaitingJobsTimeout;

    TJobControllerConfig()
    {
        RegisterParameter("resource_limits", ResourceLimits)
            .DefaultNew();
        RegisterParameter("statistics_throttler", StatisticsThrottler)
            .DefaultNew();
        RegisterParameter("waiting_jobs_timeout", WaitingJobsTimeout)
            .Default(TDuration::Seconds(15));

        RegisterInitializer([&] () {
            // 100 kB/sec * 1000 [nodes] = 100 MB/sec that corresponds to
            // approximate incoming bandwidth of 1Gbit/sec of the scheduler.
            StatisticsThrottler->Limit = 100 * 1024; 
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TJobControllerConfig)

////////////////////////////////////////////////////////////////////////////////

class TStatisticsReporterConfig
    : public NYTree::TYsonSerializable
{
public:
    bool Enabled;
    TDuration ReportingPeriod;
    TDuration MinRepeatDelay;
    TDuration MaxRepeatDelay;
    int MaxItemsInProgressNormalPriority;
    int MaxItemsInProgressLowPriority;
    int MaxItemsInBatch;
    NYPath::TYPath TableName;

    TStatisticsReporterConfig()
    {
        RegisterParameter("enabled", Enabled)
            .Default(false);
        RegisterParameter("reporting_period", ReportingPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("min_repeat_delay", MinRepeatDelay)
            .Default(TDuration::Seconds(5));
        RegisterParameter("max_repeat_delay", MaxRepeatDelay)
            .Default(TDuration::Minutes(5));
        RegisterParameter("max_items_in_progress_normal_priority", MaxItemsInProgressNormalPriority)
            .Default(200000);
        RegisterParameter("max_items_in_progress_low_priority", MaxItemsInProgressLowPriority)
            .Default(50000);
        RegisterParameter("max_items_in_batch", MaxItemsInBatch)
            .Default(20000);
        RegisterParameter("table_name", TableName)
            .Default("//sys/operations_archive/jobs");
    }
};

DEFINE_REFCOUNTED_TYPE(TStatisticsReporterConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobAgent
} // namespace NYT
