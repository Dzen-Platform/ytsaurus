#pragma once

#include "public.h"

#include <yt/core/actions/public.h>

namespace NYT {
namespace NHttp {

////////////////////////////////////////////////////////////////////////////////

const char* const DefaultContentType = "application/json";

//! Formats a canonical "Internal Server Error" (500) response.
Stroka FormatInternalServerErrorResponse(const Stroka& body = "",
    const Stroka& type = DefaultContentType);
//! Formats a canonical "Not Implemented" (501) response.
Stroka FormatNotImplementedResponse(const Stroka& body = "",
    const Stroka& type = DefaultContentType);
///! Formats a canonical "Bad Gateway" (502) response.
Stroka FormatBadGatewayResponse(const Stroka& body = "",
    const Stroka& type = DefaultContentType);
//! Formats a canonical "Service Unavailable" (503) response.
Stroka FormatServiceUnavailableResponse(const Stroka& body = "",
    const Stroka& type = DefaultContentType);
//! Formats a canonical "Gateway Timeout" (504) response.
Stroka FormatGatewayTimeoutResponse(const Stroka& body = "",
    const Stroka& type = DefaultContentType);
//! Formats a canonical "Bad Request" (400) response.
Stroka FormatBadRequestResponse(const Stroka& body = "",
    const Stroka& type = DefaultContentType);
//! Formats a canonical "Not Found" (404) response.
Stroka FormatNotFoundResponse(const Stroka& body = "",
    const Stroka& type = DefaultContentType);
//! Formats a canonical "See Other" (303) response.
Stroka FormatRedirectResponse(const Stroka& location);
//! Formats a canonical "OK" (200) response.
Stroka FormatOKResponse(const Stroka& body = "",
    const Stroka& type = DefaultContentType);

////////////////////////////////////////////////////////////////////////////////

//! A simple JSON-HTTP server.
/*
 * This class provides a simple HTTP server that invokes custom handlers
 * for certain requests. It is assumed that every generated response is a JSON.
 *
 * You can specify either a synchronous or an asynchronous
 * handler for a given path prefix.
 *
 * Synchronous handlers have precedence over asynchronous ones and they are invoked
 * within the main server loop. Asynchronous handlers are implemented using deferred
 * responses. Asynchronous responses are sent to the client when the main loop becomes idle.
 */
class TServer
    : public TNonCopyable
{
public:
    typedef TCallback<Stroka(const Stroka&)> TSyncHandler;
    typedef TCallback<TFuture<Stroka>(const Stroka&)> TAsyncHandler;

public:
    explicit TServer(int port);
    explicit TServer(int port, int bindRetryCount, TDuration bindRetryBackoff);
    ~TServer();

    void Register(const Stroka& prefix, TSyncHandler handler);
    void Register(const Stroka& prefix, TAsyncHandler handler);

    void Start();
    void Stop();

private:
    class TImpl;
    std::unique_ptr<TImpl> Impl;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NHttp
} // namespace NYT
