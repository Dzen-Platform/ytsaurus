#include "cookie_authenticator.h"
#include "blackbox_service.h"
#include "config.h"
#include "helpers.h"
#include "private.h"
#include "auth_cache.h"

#include <yt/yt/core/crypto/crypto.h>

#include <yt/yt/core/rpc/authenticator.h>

#include <util/string/split.h>

namespace NYT::NAuth {

using namespace NYTree;
using namespace NYPath;
using namespace NCrypto;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = AuthLogger;

////////////////////////////////////////////////////////////////////////////////

TString SignCsrfToken(const TString& userId, const TString& key, TInstant now)
{
    auto msg = userId + ":" + ToString(now.TimeT());
    return CreateSha256Hmac(key, msg) + ":" + ToString(now.TimeT());
}

TError CheckCsrfToken(
    const TString& csrfToken,
    const TString& userId,
    const TString& key,
    TInstant expirationTime)
{
    std::vector<TString> parts;
    StringSplitter(csrfToken).Split(':').AddTo(&parts);
    if (parts.size() != 2) {
        return TError("Malformed CSRF token");
    }

    auto signTime = TInstant::Seconds(FromString<time_t>(parts[1]));
    if (signTime < expirationTime) {
        return TError(NRpc::EErrorCode::InvalidCsrfToken, "CSRF token expired")
            << TErrorAttribute("sign_time", signTime);
    }

    auto msg = userId + ":" + ToString(signTime.TimeT());
    auto expectedToken = CreateSha256Hmac(key, msg);
    if (!ConstantTimeCompare(expectedToken, parts[0])) {
        return TError(NRpc::EErrorCode::InvalidCsrfToken, "Invalid CSFR token signature")
            << TErrorAttribute("provided_signature", parts[0])
            << TErrorAttribute("user_fingerprint", msg);
    }

    return {};
}

////////////////////////////////////////////////////////////////////////////////

// TODO(sandello): Indicate to end-used that cookie must be resigned.
class TBlackboxCookieAuthenticator
    : public ICookieAuthenticator
{
public:
    TBlackboxCookieAuthenticator(
        TBlackboxCookieAuthenticatorConfigPtr config,
        IBlackboxServicePtr blackboxService)
        : Config_(std::move(config))
        , BlackboxService_(std::move(blackboxService))
    { }

    virtual TFuture<TAuthenticationResult> Authenticate(
        const TCookieCredentials& credentials) override
    {
        auto sessionIdMD5 = TMD5Hasher().Append(credentials.SessionId).GetHexDigestUpper();
        auto sslSessionIdMD5 = TMD5Hasher().Append(credentials.SslSessionId.value_or("")).GetHexDigestUpper();
        auto userIP = FormatUserIP(credentials.UserIP);

        YT_LOG_DEBUG(
            "Authenticating user via session cookie (SessionIdMD5: %v, SslSessionIdMD5: %v, UserIP: %v)",
            sessionIdMD5,
            sslSessionIdMD5,
            userIP);

        THashMap<TString, TString> params{
            {"sessionid", credentials.SessionId},
            {"host", Config_->Domain},
            {"userip", userIP},
            {"get_user_ticket", "yes"}
        };

        if (credentials.SslSessionId) {
            params["sslsessionid"] = *credentials.SslSessionId;
        }

        return BlackboxService_->Call("sessionid", params)
            .Apply(BIND(
                &TBlackboxCookieAuthenticator::OnCallResult,
                MakeStrong(this),
                std::move(sessionIdMD5),
                std::move(sslSessionIdMD5)));
    }

private:
    const TBlackboxCookieAuthenticatorConfigPtr Config_;
    const IBlackboxServicePtr BlackboxService_;

private:
    TFuture<TAuthenticationResult> OnCallResult(
        const TString& sessionIdMD5,
        const TString& sslSessionIdMD5,
        const INodePtr& data)
    {
        auto result = OnCallResultImpl(data);
        if (!result.IsOK()) {
            YT_LOG_DEBUG(result, "Authentication failed (SessionIdMD5: %v, SslSessionIdMD5: %v)", sessionIdMD5, sslSessionIdMD5);
            result.Attributes().Set("sessionid_md5", sessionIdMD5);
            result.Attributes().Set("sslsessionid_md5", sslSessionIdMD5);
        } else {
            YT_LOG_DEBUG(
                "Authentication successful (SessionIdMD5: %v, SslSessionIdMD5: %v, Login: %v, Realm: %v)",
                sessionIdMD5,
                sslSessionIdMD5,
                result.Value().Login,
                result.Value().Realm);
        }
        return MakeFuture(result);
    }

    TErrorOr<TAuthenticationResult> OnCallResultImpl(const INodePtr& data)
    {
        auto statusId = GetByYPath<i64>(data, "/status/id");
        if (!statusId.IsOK()) {
            return TError("Blackbox returned invalid response");
        }

        if (statusId.Value() != EBlackboxStatus::Valid && statusId.Value() != EBlackboxStatus::NeedReset) {
            auto error = GetByYPath<TString>(data, "/error");
            auto reason = error.IsOK() ? error.Value() : "unknown";
            return TError(NRpc::EErrorCode::InvalidCredentials, "Blackbox rejected session cookie")
                << TErrorAttribute("reason", reason);
        }

        auto login = BlackboxService_->GetLogin(data);

        // Sanity checks.
        if (!login.IsOK()) {
            return TError("Blackbox returned invalid response")
                << login;
        }

        TAuthenticationResult result;
        result.Login = login.Value();
        result.Realm = "blackbox:cookie";
        auto userTicket = GetByYPath<TString>(data, "/user_ticket");
        if (userTicket.IsOK()) {
            result.UserTicket = userTicket.Value();
        }
        return result;
    }
};

ICookieAuthenticatorPtr CreateBlackboxCookieAuthenticator(
    TBlackboxCookieAuthenticatorConfigPtr config,
    IBlackboxServicePtr blackboxService)
{
    return New<TBlackboxCookieAuthenticator>(std::move(config), std::move(blackboxService));
}

////////////////////////////////////////////////////////////////////////////////

struct TCookieAuthenticatorCacheKey
{
    TCookieCredentials Credentials;

    operator size_t() const
    {
        size_t result = 0;
        HashCombine(result, Credentials.SessionId);
        HashCombine(result, Credentials.SslSessionId);
        return result;
    }

    bool operator == (const TCookieAuthenticatorCacheKey& other) const
    {
        return
            Credentials.SessionId == other.Credentials.SessionId &&
            Credentials.SslSessionId == other.Credentials.SslSessionId;
    }
};

class TCachingCookieAuthenticator
    : public ICookieAuthenticator
    , private TAuthCache<TCookieAuthenticatorCacheKey, TAuthenticationResult, NNet::TNetworkAddress>
{
public:
    TCachingCookieAuthenticator(
        TCachingCookieAuthenticatorConfigPtr config,
        ICookieAuthenticatorPtr underlying,
        NProfiling::TProfiler profiler)
        : TAuthCache(config->Cache, std::move(profiler))
        , UnderlyingAuthenticator_(std::move(underlying))
    { }

    virtual TFuture<TAuthenticationResult> Authenticate(const TCookieCredentials& credentials) override
    {
        return Get(TCookieAuthenticatorCacheKey{credentials}, credentials.UserIP);
    }

private:
    const ICookieAuthenticatorPtr UnderlyingAuthenticator_;

    virtual TFuture<TAuthenticationResult> DoGet(
        const TCookieAuthenticatorCacheKey& key,
        const NNet::TNetworkAddress& userIP) noexcept override
    {
        auto credentials = key.Credentials;
        credentials.UserIP = userIP;
        return UnderlyingAuthenticator_->Authenticate(credentials);
    }
};

ICookieAuthenticatorPtr CreateCachingCookieAuthenticator(
    TCachingCookieAuthenticatorConfigPtr config,
    ICookieAuthenticatorPtr authenticator,
    NProfiling::TProfiler profiler)
{
    return New<TCachingCookieAuthenticator>(
        std::move(config),
        std::move(authenticator),
        std::move(profiler));
}

////////////////////////////////////////////////////////////////////////////////

class TCookieAuthenticatorWrapper
    : public NRpc::IAuthenticator
{
public:
    explicit TCookieAuthenticatorWrapper(ICookieAuthenticatorPtr underlying)
        : Underlying_(std::move(underlying))
    { }

    virtual TFuture<NRpc::TAuthenticationResult> Authenticate(
        const NRpc::TAuthenticationContext& context) override
    {
        if (!context.Header->HasExtension(NRpc::NProto::TCredentialsExt::credentials_ext)) {
            return std::nullopt;
        }

        const auto& ext = context.Header->GetExtension(NRpc::NProto::TCredentialsExt::credentials_ext);
        if (!ext.has_session_id() && !ext.has_ssl_session_id()) {
            return std::nullopt;
        }

        if (!context.UserIP.IsIP4() && !context.UserIP.IsIP6()) {
            return std::nullopt;
        }

        TCookieCredentials credentials;
        credentials.SessionId = ext.session_id();
        credentials.SslSessionId = ext.ssl_session_id();
        credentials.UserIP = context.UserIP;
        return Underlying_->Authenticate(credentials).Apply(
            BIND([=] (const TAuthenticationResult& authResult) {
                NRpc::TAuthenticationResult rpcResult;
                rpcResult.User = authResult.Login;
                rpcResult.Realm = authResult.Realm;
                rpcResult.UserTicket = authResult.UserTicket;
                return rpcResult;
            }));
    }
private:
    const ICookieAuthenticatorPtr Underlying_;
};

NRpc::IAuthenticatorPtr CreateCookieAuthenticatorWrapper(ICookieAuthenticatorPtr underlying)
{
    return New<TCookieAuthenticatorWrapper>(std::move(underlying));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NAuth
