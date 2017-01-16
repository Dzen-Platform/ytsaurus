#pragma once

#include <yt/ytlib/chunk_client/config.h>

#include <yt/ytlib/driver/config.h>

#include <yt/core/misc/address.h>

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/logging/config.h>

#include <yt/core/tracing/config.h>

namespace NYT {
namespace NNodeJS {

////////////////////////////////////////////////////////////////////////////////

class THttpProxyConfig
    : public NYTree::TYsonSerializable
{
public:
    NChunkClient::TDispatcherConfigPtr ChunkClientDispatcher;
    NDriver::TDriverConfigPtr Driver;
    TAddressResolverConfigPtr AddressResolver;
    NLogging::TLogConfigPtr Logging;
    NTracing::TTraceManagerConfigPtr Tracing;

    THttpProxyConfig()
    {
        RegisterParameter("logging", Logging)
            .DefaultNew();
        RegisterParameter("tracing", Tracing)
            .DefaultNew();
        RegisterParameter("chunk_client_dispatcher", ChunkClientDispatcher)
            .DefaultNew();
        RegisterParameter("driver", Driver)
            .DefaultNew();
        RegisterParameter("address_resolver", AddressResolver)
            .DefaultNew();
    }
};

typedef TIntrusivePtr<THttpProxyConfig> THttpProxyConfigPtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeJS
} // namespace NYT
