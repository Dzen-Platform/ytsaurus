#pragma once

#include <yt/core/misc/intrusive_ptr.h>

#include <yt/client/api/rpc_proxy/public.h>

namespace NYT::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap;

DECLARE_REFCOUNTED_CLASS(TRpcProxyConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TSecurityManagerConfig)
DECLARE_REFCOUNTED_CLASS(TApiServiceConfig)
DECLARE_REFCOUNTED_CLASS(TDiscoveryServiceConfig)

DECLARE_REFCOUNTED_STRUCT(IProxyCoordinator)
DECLARE_REFCOUNTED_CLASS(TSecurityManger)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpcProxy
