#pragma once

#include <yt/core/misc/guid.h>

#include <yt/core/bus/public.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TReqDiscover;
class TRspDiscover;
class TRequestHeader;
class TResponseHeader;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

class TClientRequest;

template <class TRequestMessage, class TResponse>
class TTypedClientRequest;

class TClientResponse;

template <class TResponseMessage>
class TTypedClientResponse;

template <class TRequestMessage, class TResponseMessage>
class TTypedServiceContext;

struct TServiceId;

struct TAuthenticationResult;

DECLARE_REFCOUNTED_STRUCT(IClientRequest)
DECLARE_REFCOUNTED_STRUCT(IClientRequestControl)
DECLARE_REFCOUNTED_STRUCT(IClientResponseHandler)
DECLARE_REFCOUNTED_STRUCT(IServer)
DECLARE_REFCOUNTED_STRUCT(IService)
DECLARE_REFCOUNTED_STRUCT(IServiceWithReflection)
DECLARE_REFCOUNTED_STRUCT(IServiceContext)
DECLARE_REFCOUNTED_STRUCT(IChannel)
DECLARE_REFCOUNTED_STRUCT(IChannelFactory)
DECLARE_REFCOUNTED_STRUCT(IRoamingChannelProvider)
DECLARE_REFCOUNTED_STRUCT(IAuthenticator)

DECLARE_REFCOUNTED_CLASS(TClientContext)
DECLARE_REFCOUNTED_CLASS(TServiceBase)
DECLARE_REFCOUNTED_CLASS(TChannelWrapper)
DECLARE_REFCOUNTED_CLASS(TStaticChannelFactory)
DECLARE_REFCOUNTED_CLASS(TClientRequestControlThunk)

DECLARE_REFCOUNTED_CLASS(TResponseKeeper)

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TServerConfig)
DECLARE_REFCOUNTED_CLASS(TServiceConfig)
DECLARE_REFCOUNTED_CLASS(TMethodConfig)
DECLARE_REFCOUNTED_CLASS(TRetryingChannelConfig)
DECLARE_REFCOUNTED_CLASS(TBalancingChannelConfig)
DECLARE_REFCOUNTED_CLASS(TThrottlingChannelConfig)
DECLARE_REFCOUNTED_CLASS(TResponseKeeperConfig)
DECLARE_REFCOUNTED_CLASS(TMultiplexingBandConfig)
DECLARE_REFCOUNTED_CLASS(TDispatcherConfig)

////////////////////////////////////////////////////////////////////////////////

typedef TGuid TRequestId;
extern const TRequestId NullRequestId;

typedef TGuid TRealmId;
extern const TRealmId NullRealmId;

typedef TGuid TMutationId;
extern const TMutationId NullMutationId;

extern const TString RootUserName;

DEFINE_ENUM(EMultiplexingBand,
    ((Default)               (0))
    ((Control)               (1))
    ((Heavy)                 (2))
);

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EErrorCode,
    ((TransportError)               (static_cast<int>(NBus::EErrorCode::TransportError)))
    ((ProtocolError)                (101))
    ((NoSuchService)                (102))
    ((NoSuchMethod)                 (103))
    ((Unavailable)                  (105))
    ((PoisonPill)                   (106))
    ((RequestQueueSizeLimitExceeded)(108))
    ((AuthenticationError)          (109))
    ((InvalidCsrfToken)             (110))
);

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMessageFormat,
    ((Protobuf)    (0))
    ((Json)        (1))
    ((Yson)        (2))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
