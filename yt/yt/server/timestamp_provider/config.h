#pragma once

#include <yt/yt/server/lib/misc/config.h>

#include <yt/yt/core/bus/tcp/config.h>

namespace NYT::NTimestampProvider {

////////////////////////////////////////////////////////////////////////////////

class TTimestampProviderConfig
    : public TServerConfig
{
public:
    bool AbortOnUnrecognizedOptions;

    NBus::TTcpBusConfigPtr BusClient;

    NTransactionClient::TRemoteTimestampProviderConfigPtr TimestampProvider;

    REGISTER_YSON_STRUCT(TTimestampProviderConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TTimestampProviderConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTimestampProvider
