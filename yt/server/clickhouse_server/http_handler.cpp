#include "http_handler.h"

#include "storage.h"

#include <server/HTTPHandler.h>
#include <server/NotFoundHandler.h>
#include <server/PingRequestHandler.h>
#include <server/RootRequestHandler.h>

#include <common/logger_useful.h>

#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/URI.h>

namespace NYT::NClickHouseServer {

using namespace DB;

////////////////////////////////////////////////////////////////////////////////

namespace {

bool IsHead(const Poco::Net::HTTPServerRequest& request)
{
    return request.getMethod() == Poco::Net::HTTPRequest::HTTP_HEAD;
}

bool IsGet(const Poco::Net::HTTPServerRequest& request)
{
    return request.getMethod() == Poco::Net::HTTPRequest::HTTP_GET;
}

bool IsPost(const Poco::Net::HTTPServerRequest& request)
{
    return request.getMethod() == Poco::Net::HTTPRequest::HTTP_POST;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class THttpHandlerFactory
    : public Poco::Net::HTTPRequestHandlerFactory
{
private:
    TBootstrap* Bootstrap_;
    IServer& Server;
    Poco::Logger* Log;

public:
    THttpHandlerFactory(TBootstrap* bootstrap, IServer& server)
        : Bootstrap_(bootstrap)
        , Server(server)
        , Log(&Logger::get("HTTPHandlerFactory"))
    {}

    Poco::Net::HTTPRequestHandler* createRequestHandler(
        const Poco::Net::HTTPServerRequest& request) override;
};

////////////////////////////////////////////////////////////////////////////////

Poco::Net::HTTPRequestHandler* THttpHandlerFactory::createRequestHandler(
    const Poco::Net::HTTPServerRequest& request)
{
    class THttpHandler
        : public DB::HTTPHandler
    {
    public:
        THttpHandler(TBootstrap* bootstrap, DB::IServer& server)
            : DB::HTTPHandler(server)
            , Bootstrap_(bootstrap)
        { }

        virtual void customizeContext(DB::Context& context) override
        {
            SetupHostContext(Bootstrap_, context);
        }

    private:
        TBootstrap* const Bootstrap_;
    };

    const Poco::URI uri(request.getURI());

    LOG_INFO(Log, "HTTP Request. "
        << "Method: " << request.getMethod()
        << ", URI: " << uri.toString()
        << ", Address: " << request.clientAddress().toString()
        << ", User-Agent: " << (request.has("User-Agent") ? request.get("User-Agent") : "none"));

    // Light health-checking requests
    if (IsHead(request) || IsGet(request)) {
        if (uri == "/") {
            return new RootRequestHandler(Server);
        }
        if (uri == "/ping") {
            return new PingRequestHandler(Server);
        }
    }

    // Query execution
    // HTTPHandler executes query in read-only mode for GET requests
    if (IsGet(request) || IsPost(request)) {
        if ((uri.getPath() == "/") ||
            (uri.getPath() == "/query")) {
            auto* handler = new THttpHandler(Bootstrap_, Server);
            return handler;
        }
    }

    return new NotFoundHandler();
}

////////////////////////////////////////////////////////////////////////////////

Poco::Net::HTTPRequestHandlerFactory::Ptr CreateHttpHandlerFactory(TBootstrap* bootstrap, IServer& server)
{
    return new THttpHandlerFactory(bootstrap, server);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
