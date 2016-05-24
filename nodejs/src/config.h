#pragma once

#include <yt/ytlib/chunk_client/config.h>

#include <yt/ytlib/driver/config.h>

#include <yt/core/misc/address.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NNodeJS {

////////////////////////////////////////////////////////////////////////////////

class THttpProxyConfig
    : public NYTree::TYsonSerializable
{
public:
    NYTree::INodePtr Logging;
    NYTree::INodePtr Tracing;
    NChunkClient::TDispatcherConfigPtr ChunkClientDispatcher;
    NDriver::TDriverConfigPtr Driver;
    TAddressResolverConfigPtr AddressResolver;

    THttpProxyConfig()
    {
        RegisterParameter("logging", Logging);
        RegisterParameter("tracing", Tracing);
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
