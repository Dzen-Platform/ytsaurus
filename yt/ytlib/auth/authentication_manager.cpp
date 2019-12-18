#include "authentication_manager.h"
#include "caching_tvm_service.h"
#include "cookie_authenticator.h"
#include "default_blackbox_service.h"
#include "default_tvm_service.h"
#include "ticket_authenticator.h"
#include "token_authenticator.h"

#include <yt/ytlib/auth/config.h>

#include <yt/core/rpc/authenticator.h>

namespace NYT::NAuth {

using namespace NApi;
using namespace NRpc;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TAuthenticationManager::TImpl
{
public:
    TImpl(
        TAuthenticationManagerConfigPtr config,
        IPollerPtr poller,
        IClientPtr client,
        NProfiling::TProfiler profiler)
    {
        std::vector<NRpc::IAuthenticatorPtr> rpcAuthenticators;
        std::vector<NAuth::ITokenAuthenticatorPtr> tokenAuthenticators;

        if (config->TvmService && poller) {
            TvmService_ = CreateCachingTvmService(
                CreateDefaultTvmService(
                    config->TvmService,
                    poller,
                    profiler.AppendPath("/tvm/remote")),
                config->TvmService,
                profiler.AppendPath("/tvm/cache"));
        }

        IBlackboxServicePtr blackboxService;
        if (config->BlackboxService && poller) {
            blackboxService = CreateDefaultBlackboxService(
                config->BlackboxService,
                TvmService_,
                poller,
                profiler.AppendPath("/blackbox"));
        }

        if (config->BlackboxTokenAuthenticator && blackboxService) {
            tokenAuthenticators.push_back(
                CreateCachingTokenAuthenticator(
                    config->BlackboxTokenAuthenticator,
                    CreateBlackboxTokenAuthenticator(
                        config->BlackboxTokenAuthenticator,
                        blackboxService,
                        profiler.AppendPath("/blackbox_token_authenticator/remote")),
                    profiler.AppendPath("/blackbox_token_authenticator/cache")));
        }

        if (config->CypressTokenAuthenticator && client) {
            tokenAuthenticators.push_back(
                CreateCachingTokenAuthenticator(
                    config->CypressTokenAuthenticator,
                    CreateCypressTokenAuthenticator(
                        config->CypressTokenAuthenticator,
                        client),
                    profiler.AppendPath("/cypress_token_authenticator/cache")));
        }

        if (config->BlackboxCookieAuthenticator && blackboxService) {
            CookieAuthenticator_ = CreateCachingCookieAuthenticator(
                config->BlackboxCookieAuthenticator,
                CreateBlackboxCookieAuthenticator(
                    config->BlackboxCookieAuthenticator,
                    blackboxService),
                profiler.AppendPath("/blackbox_cookie_authenticator/cache"));
            rpcAuthenticators.push_back(
                CreateCookieAuthenticatorWrapper(CookieAuthenticator_));
        }

        if (blackboxService && config->BlackboxTicketAuthenticator) {
            TicketAuthenticator_ = CreateBlackboxTicketAuthenticator(
                config->BlackboxTicketAuthenticator,
                blackboxService);
            rpcAuthenticators.push_back(
                CreateTicketAuthenticatorWrapper(TicketAuthenticator_));
        }

        if (!tokenAuthenticators.empty()) {
            rpcAuthenticators.push_back(CreateTokenAuthenticatorWrapper(
                CreateCompositeTokenAuthenticator(tokenAuthenticators)));
        }

        if (!config->RequireAuthentication) {
            tokenAuthenticators.push_back(CreateNoopTokenAuthenticator());
        }
        TokenAuthenticator_ = CreateCompositeTokenAuthenticator(tokenAuthenticators);

        if (!config->RequireAuthentication) {
            rpcAuthenticators.push_back(NRpc::CreateNoopAuthenticator());
        }
        RpcAuthenticator_ = CreateCompositeAuthenticator(std::move(rpcAuthenticators));
    }

    const NRpc::IAuthenticatorPtr& GetRpcAuthenticator() const
    {
        return RpcAuthenticator_;
    }

    const ITokenAuthenticatorPtr& GetTokenAuthenticator() const
    {
        return TokenAuthenticator_;
    }

    const ICookieAuthenticatorPtr& GetCookieAuthenticator() const
    {
        return CookieAuthenticator_;
    }

    const ITicketAuthenticatorPtr& GetTicketAuthenticator() const
    {
        return TicketAuthenticator_;
    }

    const ITvmServicePtr& GetTvmService() const
    {
        return TvmService_;
    }

private:
    ITvmServicePtr TvmService_;
    NRpc::IAuthenticatorPtr RpcAuthenticator_;
    ITokenAuthenticatorPtr TokenAuthenticator_;
    ICookieAuthenticatorPtr CookieAuthenticator_;
    ITicketAuthenticatorPtr TicketAuthenticator_;
};

////////////////////////////////////////////////////////////////////////////////

TAuthenticationManager::TAuthenticationManager(
    TAuthenticationManagerConfigPtr config,
    IPollerPtr poller,
    IClientPtr client,
    NProfiling::TProfiler profiler)
    : Impl_(std::make_unique<TImpl>(
        std::move(config),
        std::move(poller),
        std::move(client),
        std::move(profiler)))
{ }

const NRpc::IAuthenticatorPtr& TAuthenticationManager::GetRpcAuthenticator() const
{
    return Impl_->GetRpcAuthenticator();
}

const ITokenAuthenticatorPtr& TAuthenticationManager::GetTokenAuthenticator() const
{
    return Impl_->GetTokenAuthenticator();
}

const ICookieAuthenticatorPtr& TAuthenticationManager::GetCookieAuthenticator() const
{
    return Impl_->GetCookieAuthenticator();
}

const ITicketAuthenticatorPtr& TAuthenticationManager::GetTicketAuthenticator() const
{
    return Impl_->GetTicketAuthenticator();
}

const ITvmServicePtr& TAuthenticationManager::GetTvmService() const
{
    return Impl_->GetTvmService();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NAuth
