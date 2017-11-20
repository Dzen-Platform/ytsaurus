#pragma once

#include "public.h"

#include <yt/core/concurrency/public.h>
#include <yt/core/concurrency/async_stream.h>

#include <yt/core/actions/callback.h>

#include <yt/core/misc/enum.h>
#include <yt/core/misc/ref.h>
#include <yt/core/misc/property.h>
#include <yt/core/misc/nullable.h>

#include <util/stream/zerocopy.h>

namespace NYT {
namespace NHttp {

////////////////////////////////////////////////////////////////////////////////

//! Copy-pasted from http_parser.h in order to avoid exposing it in our headers.
//! Please keep in sync, LOL.

#define YT_HTTP_STATUS_MAP(XX)                                              \
  XX(100, Continue,                        Continue)                        \
  XX(101, SwitchingProtocols,              Switching Protocols)             \
  XX(102, Processing,                      Processing)                      \
  XX(200, Ok,                              OK)                              \
  XX(201, Created,                         Created)                         \
  XX(202, Accepted,                        Accepted)                        \
  XX(203, NonAuthoritativeInformation,     Non-Authoritative Information)   \
  XX(204, NoContent,                       No Content)                      \
  XX(205, ResetContent,                    Reset Content)                   \
  XX(206, PartialContent,                  Partial Content)                 \
  XX(207, MultiStatus,                     Multi-Status)                    \
  XX(208, AlreadyReported,                 Already Reported)                \
  XX(226, ImUsed,                          IM Used)                         \
  XX(300, MultipleChoices,                 Multiple Choices)                \
  XX(301, MovedPermanently,                Moved Permanently)               \
  XX(302, Found,                           Found)                           \
  XX(303, SeeOther,                        See Other)                       \
  XX(304, NotModified,                     Not Modified)                    \
  XX(305, UseProxy,                        Use Proxy)                       \
  XX(307, TemporaryRedirect,               Temporary Redirect)              \
  XX(308, PermanentRedirect,               Permanent Redirect)              \
  XX(400, BadRequest,                      Bad Request)                     \
  XX(401, Unauthorized,                    Unauthorized)                    \
  XX(402, PaymentRequired,                 Payment Required)                \
  XX(403, Forbidden,                       Forbidden)                       \
  XX(404, NotFound,                        Not Found)                       \
  XX(405, MethodNotAllowed,                Method Not Allowed)              \
  XX(406, NotAcceptable,                   Not Acceptable)                  \
  XX(407, ProxyAuthenticationRequired,     Proxy Authentication Required)   \
  XX(408, RequestTimeout,                  Request Timeout)                 \
  XX(409, Conflict,                        Conflict)                        \
  XX(410, Gone,                            Gone)                            \
  XX(411, LengthRequired,                  Length Required)                 \
  XX(412, PreconditionFailed,              Precondition Failed)             \
  XX(413, PayloadTooLarge,                 Payload Too Large)               \
  XX(414, UriTooLong,                      URI Too Long)                    \
  XX(415, UnsupportedMediaType,            Unsupported Media Type)          \
  XX(416, RangeNotSatisfiable,             Range Not Satisfiable)           \
  XX(417, ExpectationFailed,               Expectation Failed)              \
  XX(421, MisdirectedRequest,              Misdirected Request)             \
  XX(422, UnprocessableEntity,             Unprocessable Entity)            \
  XX(423, Locked,                          Locked)                          \
  XX(424, FailedDependency,                Failed Dependency)               \
  XX(426, UpgradeRequired,                 Upgrade Required)                \
  XX(428, PreconditionRequired,            Precondition Required)           \
  XX(429, TooManyRequests,                 Too Many Requests)               \
  XX(431, RequestHeaderFieldsTooLarge,     Request Header Fields Too Large) \
  XX(451, UnavailableForLegalReasons,      Unavailable For Legal Reasons)   \
  XX(500, InternalServerError,             Internal Server Error)           \
  XX(501, NotImplemented,                  Not Implemented)                 \
  XX(502, BadGateway,                      Bad Gateway)                     \
  XX(503, ServiceUnavailable,              Service Unavailable)             \
  XX(504, GatewayTimeout,                  Gateway Timeout)                 \
  XX(505, HttpVersionNotSupported,         HTTP Version Not Supported)      \
  XX(506, VariantAlsoNegotiates,           Variant Also Negotiates)         \
  XX(507, InsufficientStorage,             Insufficient Storage)            \
  XX(508, LoopDetected,                    Loop Detected)                   \
  XX(510, NotExtended,                     Not Extended)                    \
  XX(511, NetworkAuthenticationRequired,   Network Authentication Required) \

/* Request Methods */
#define YT_HTTP_METHOD_MAP(XX)      \
  XX(0,  Delete,      DELETE)       \
  XX(1,  Get,         GET)          \
  XX(2,  Head,        HEAD)         \
  XX(3,  Post,        POST)         \
  XX(4,  Put,         PUT)          \
  /* pathological */                \
  XX(5,  Connect,     CONNECT)      \
  XX(6,  Options,     OPTIONS)      \
  XX(7,  Trace,       TRACE)        \
  /* WebDAV */                      \
  XX(8,  Copy,        COPY)         \
  XX(9,  Lock,        LOCK)         \
  XX(10, Mkcol,       MKCOL)        \
  XX(11, Move,        MOVE)         \
  XX(12, Propfind,    PROPFIND)     \
  XX(13, Proppatch,   PROPPATCH)    \
  XX(14, Search,      SEARCH)       \
  XX(15, Unlock,      UNLOCK)       \
  XX(16, Bind,        BIND)         \
  XX(17, Rebind,      REBIND)       \
  XX(18, Unbind,      UNBIND)       \
  XX(19, Acl,         ACL)          \
  /* subversion */                  \
  XX(20, Report,      REPORT)       \
  XX(21, Mkactivity,  MKACTIVITY)   \
  XX(22, Checkout,    CHECKOUT)     \
  XX(23, Merge,       MERGE)        \
  /* upnp */                        \
  XX(24, Msearch,     M-SEARCH)     \
  XX(25, Notify,      NOTIFY)       \
  XX(26, Subscribe,   SUBSCRIBE)    \
  XX(27, Unsubscribe, UNSUBSCRIBE)  \
  /* RFC-5789 */                    \
  XX(28, Patch,       PATCH)        \
  XX(29, Purge,       PURGE)        \
  /* CalDAV */                      \
  XX(30, Mkcalendar,  MKCALENDAR)   \
  /* RFC-2068, section 19.6.1.2 */  \
  XX(31, Link,        LINK)         \
  XX(32, Unlink,      UNLINK)       \

////////////////////////////////////////////////////////////////////////////////

#define XX(num, name, string) (name)
DEFINE_ENUM(EMethod,
    YT_HTTP_METHOD_MAP(XX)
);
#undef XX

#define XX(num, name, string) ((name) (num))
DEFINE_ENUM(EStatusCode,
    YT_HTTP_STATUS_MAP(XX)
);
#undef XX

////////////////////////////////////////////////////////////////////////////////

//! {Protocol}://{User}@{Host}:{Port}{Path}?{RawQuery}
struct TUrlRef
{
    TNullable<ui16> Port;

    TStringBuf Protocol;
    TStringBuf User;
    //! If host is IPv6 address, Host field will cointain address without square brackets.
    //! E.g. http://[::1]:80/ -> Host == "::1"
    TStringBuf Host;
    TStringBuf PortStr;
    TStringBuf Path;
    TStringBuf RawQuery;
};

TUrlRef ParseUrl(TStringBuf url);

////////////////////////////////////////////////////////////////////////////////

class THeaders
    : public virtual TRefCounted
{
public:
    void Add(const TString& header, const TString& value);
    void Set(const TString& header, const TString& value);

    const TString* Find(const TString& header) const;

    //! Returns first header value, if any. Throws otherwise.
    const TString& Get(const TString& header) const;

    const std::vector<TString>& GetAll(const TString& header) const;

    void WriteTo(IOutputStream* out, const THashSet<TString>* filtered = nullptr) const;

private:
    struct TEntry {
        TString OriginalHeaderName;
        std::vector<TString> Values;
    };

    yhash<TString, TEntry> Raw_;

    void ValidateValue(TStringBuf header, TStringBuf value);
};

DEFINE_REFCOUNTED_TYPE(THeaders)

////////////////////////////////////////////////////////////////////////////////

struct IRequest
    : public virtual TRefCounted
    , public virtual NConcurrency::IAsyncZeroCopyInputStream
{
    virtual std::pair<int, int> GetVersion() = 0;

    virtual EMethod GetMethod() = 0;

    virtual const TUrlRef& GetUrl() = 0;

    virtual const THeadersPtr& GetHeaders() = 0;
};

DEFINE_REFCOUNTED_TYPE(IRequest)

////////////////////////////////////////////////////////////////////////////////

struct IResponseWriter
    : public virtual TRefCounted
    , public virtual NConcurrency::IAsyncOutputStream
{
    virtual const THeadersPtr& GetHeaders() = 0;
    virtual const THeadersPtr& GetTrailers() = 0;

    virtual void WriteHeaders(EStatusCode status) = 0;

    virtual TFuture<void> WriteBody(const TSharedRef& smallBody) = 0;
};

DEFINE_REFCOUNTED_TYPE(IResponseWriter)

////////////////////////////////////////////////////////////////////////////////

struct IResponse
    : public virtual TRefCounted
    , public virtual NConcurrency::IAsyncZeroCopyInputStream
{
    virtual EStatusCode GetStatusCode() = 0;

    void CheckStatusOrThrow();

    virtual const THeadersPtr& GetHeaders() = 0;
    virtual const THeadersPtr& GetTrailers() = 0;
};

DEFINE_REFCOUNTED_TYPE(IResponse)

////////////////////////////////////////////////////////////////////////////////

struct IHttpHandler
    : public virtual TRefCounted
{
    virtual void HandleHttp(
        const IRequestPtr& req,
        const IResponseWriterPtr& rsp) = 0;
};

DEFINE_REFCOUNTED_TYPE(IHttpHandler)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHttp
} // namespace NYT
