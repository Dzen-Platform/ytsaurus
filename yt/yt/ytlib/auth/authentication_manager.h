#pragma once

#include "public.h"

#include <yt/yt/client/api/public.h>

#include <yt/yt/core/rpc/public.h>

#include <yt/yt/core/actions/public.h>

#include <yt/yt/core/concurrency/public.h>

#include <yt/yt/core/profiling/profiler.h>

namespace NYT::NAuth {

////////////////////////////////////////////////////////////////////////////////

class TAuthenticationManager
    : public TRefCounted
{
public:
    TAuthenticationManager(
        TAuthenticationManagerConfigPtr config,
        NConcurrency::IPollerPtr poller,
        NApi::IClientPtr client,
        NProfiling::TRegistry profiler = AuthProfiler);

    ~TAuthenticationManager();

    const NRpc::IAuthenticatorPtr& GetRpcAuthenticator() const;
    const NAuth::ITokenAuthenticatorPtr& GetTokenAuthenticator() const;
    const NAuth::ICookieAuthenticatorPtr& GetCookieAuthenticator() const;
    const NAuth::ITicketAuthenticatorPtr& GetTicketAuthenticator() const;
    const NAuth::ITvmServicePtr& GetTvmService() const;

private:
    class TImpl;
    const std::unique_ptr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TAuthenticationManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NAuth
