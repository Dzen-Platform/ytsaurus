#pragma once

#include "public.h"

#include <yt/yt/core/ytree/yson_serializable.h>

namespace NYT::NTimestampServer {

////////////////////////////////////////////////////////////////////////////////

class TTimestampManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration CalibrationPeriod;
    TDuration TimestampPreallocationInterval;
    TDuration TimestampReserveInterval;
    int MaxTimestampsPerRequest;
    TDuration RequestBackoffTime;
    bool EmbedCellTag;

    TTimestampManagerConfig()
    {
        RegisterParameter("calibration_period", CalibrationPeriod)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("timestamp_preallocation_interval", TimestampPreallocationInterval)
            .Alias("commit_advance")
            .Default(TDuration::Seconds(5));
        RegisterParameter("timestamp_reserve_interval", TimestampReserveInterval)
            .Default(TDuration::Seconds(1));
        RegisterParameter("max_timestamps_per_request", MaxTimestampsPerRequest)
            .GreaterThan(0)
            .Default(1000000);
        RegisterParameter("request_backoff_time", RequestBackoffTime)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("embed_cell_tag", EmbedCellTag)
            .Default(false);
    }
};

DEFINE_REFCOUNTED_TYPE(TTimestampManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTimestampServer
