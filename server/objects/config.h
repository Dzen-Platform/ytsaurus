#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/ypath/public.h>

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

class TObjectManagerConfig
    : public NYT::NYTree::TYsonSerializable
{
public:
    TDuration RemovedObjectsSweepPeriod;
    TDuration RemovedObjectsGraceTimeout;

    TObjectManagerConfig()
    {
        RegisterParameter("removed_objects_sweep_period", RemovedObjectsSweepPeriod)
            .Default(TDuration::Minutes(10));
        RegisterParameter("removed_objects_grace_timeout", RemovedObjectsGraceTimeout)
            .Default(TDuration::Hours(24));
    }
};

DEFINE_REFCOUNTED_TYPE(TObjectManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TTransactionManagerConfig
    : public NYT::NYTree::TYsonSerializable
{
public:
    i64 InputRowLimit;
    i64 OutputRowLimit;
    int MaxKeysPerLookupRequest;

    TTransactionManagerConfig()
    {
        RegisterParameter("input_row_limit", InputRowLimit)
            .Default(10000000);
        RegisterParameter("output_row_limit", OutputRowLimit)
            .Default(10000000);
        RegisterParameter("max_keys_per_lookup_request", MaxKeysPerLookupRequest)
            .GreaterThan(0)
            .Default(100);
    }
};

DEFINE_REFCOUNTED_TYPE(TTransactionManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects
