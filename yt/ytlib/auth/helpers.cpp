#include "helpers.h"

#include <yt/core/crypto/crypto.h>

#include <library/cpp/string_utils/quote/quote.h>
#include <library/cpp/string_utils/url/url.h>

namespace NYT::NAuth {

////////////////////////////////////////////////////////////////////////////////

TString GetCryptoHash(TStringBuf secret)
{
    return NCrypto::TSha1Hasher()
        .Append(secret)
        .GetHexDigestLower();
}

TString FormatUserIP(const NNet::TNetworkAddress& address)
{
    if (!address.IsIP()) {
        // Sometimes userIP is missing (e.g. user is connecting
        // from job using unix socket), but it is required by
        // blackbox. Put placeholder in place of a real IP.
        static const TString LocalUserIP = "127.0.0.1";
        return LocalUserIP;
    }
    return ToString(
        address,
        NNet::TNetworkAddressFormatOptions{
            .IncludePort = false,
            .IncludeTcpProtocol = false
        });
}

////////////////////////////////////////////////////////////////////////////////

static const THashSet<TString> PrivateUrlParams{
    "userip",
    "oauth_token",
    "sessionid",
    "sslsessionid"
};

void TSafeUrlBuilder::AppendString(TStringBuf str)
{
    RealUrl_.AppendString(str);
    SafeUrl_.AppendString(str);
}

void TSafeUrlBuilder::AppendChar(char ch)
{
    RealUrl_.AppendChar(ch);
    SafeUrl_.AppendChar(ch);
}

void TSafeUrlBuilder::AppendParam(TStringBuf key, TStringBuf value)
{
    auto size = key.length() + 4 + CgiEscapeBufLen(value.length());

    char* realBegin = RealUrl_.Preallocate(size);
    char* realIt = realBegin;
    memcpy(realIt, key.data(), key.length());
    realIt += key.length();
    *realIt = '=';
    realIt += 1;
    auto realEnd = CGIEscape(realIt, value.data(), value.length());
    RealUrl_.Advance(realEnd - realBegin);

    char* safeBegin = SafeUrl_.Preallocate(size);
    char* safeEnd = safeBegin;
    if (PrivateUrlParams.contains(key)) {
        memcpy(safeEnd, realBegin, realIt - realBegin);
        safeEnd += realIt - realBegin;
        memcpy(safeEnd, "***", 3);
        safeEnd += 3;
    } else {
        memcpy(safeEnd, realBegin, realEnd - realBegin);
        safeEnd += realEnd - realBegin;
    }
    SafeUrl_.Advance(safeEnd - safeBegin);
}

TString TSafeUrlBuilder::FlushRealUrl()
{
    return RealUrl_.Flush();
}

TString TSafeUrlBuilder::FlushSafeUrl()
{
    return SafeUrl_.Flush();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NAuth

