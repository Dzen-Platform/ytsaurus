#pragma once

#include <yt/core/misc/hash.h>
#include <yt/core/misc/ref_counted.h>

#include <util/generic/stroka.h>

namespace NYT {
namespace NBlackbox {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TDefaultBlackboxServiceConfig)
DECLARE_REFCOUNTED_CLASS(TTokenAuthenticatorConfig)
DECLARE_REFCOUNTED_CLASS(TCachingTokenAuthenticatorConfig)
DECLARE_REFCOUNTED_CLASS(TCookieAuthenticatorConfig)

DECLARE_REFCOUNTED_STRUCT(IBlackboxService)

DECLARE_REFCOUNTED_STRUCT(ICookieAuthenticator)
DECLARE_REFCOUNTED_STRUCT(ITokenAuthenticator)

struct TTokenCredentials
{
    Stroka Token;
    Stroka UserIp;
};

struct TAuthenticationResult
{
    Stroka Login;
    Stroka Realm;
};

inline bool operator==(
    const TTokenCredentials& lhs,
    const TTokenCredentials& rhs)
{
    return std::tie(lhs.Token, lhs.UserIp) == std::tie(rhs.Token, rhs.UserIp);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NBlackbox
} // namespace NYT

template <>
struct hash<NYT::NBlackbox::TTokenCredentials>
{
    inline size_t operator()(const NYT::NBlackbox::TTokenCredentials& credentials) const
    {
        return HashCombineImpl(
            THash<Stroka>()(credentials.Token),
            THash<Stroka>()(credentials.UserIp));
    }
};

