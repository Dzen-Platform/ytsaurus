#pragma once

#include "public.h"

#include <yt/yt/core/rpc/config.h>

#include <yt/yt/core/ytree/yson_struct.h>

namespace NYT::NQueueClient {

////////////////////////////////////////////////////////////////////////////////

class TQueueAgentStageChannelConfig
    : public NRpc::TBalancingChannelConfig
    , public NRpc::TRetryingChannelConfig
{
    REGISTER_YSON_STRUCT(TQueueAgentStageChannelConfig);

    static void Register(TRegistrar)
    { }
};

DEFINE_REFCOUNTED_TYPE(TQueueAgentStageChannelConfig)

////////////////////////////////////////////////////////////////////////////////

class TQueueAgentConnectionConfig
    : public NYTree::TYsonStruct
{
public:
    THashMap<TString, TQueueAgentStageChannelConfigPtr> Stages;

    REGISTER_YSON_STRUCT(TQueueAgentConnectionConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TQueueAgentConnectionConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueClient
