#pragma once

#include "public.h"

#include <yt/yt/core/ytree/yson_struct.h>

namespace NYT::NContainers {

////////////////////////////////////////////////////////////////////////////////

class TCGroupConfig
    : public virtual NYTree::TYsonStruct
{
public:
    std::vector<TString> SupportedCGroups;

    bool IsCGroupSupported(const TString& cgroupType) const;

    REGISTER_YSON_STRUCT(TCGroupConfig);

    static void Register(TRegistrar registrar);
};

////////////////////////////////////////////////////////////////////////////////

class TPortoExecutorConfig
    : public NYTree::TYsonStruct
{
public:
    TDuration RetriesTimeout;
    TDuration PollPeriod;
    TDuration ApiTimeout;
    TDuration ApiDiskTimeout;
    bool EnableNetworkIsolation;

    REGISTER_YSON_STRUCT(TPortoExecutorConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TPortoExecutorConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NContainers
