#include "tcp_handler.h"

#include "helpers.h"
#include "query_context.h"

#include <Poco/Util/LayeredConfiguration.h>
#include <Server/TCPHandler.h>

#include <util/string/cast.h>
#include <util/string/split.h>

namespace NYT::NClickHouseServer {

using namespace DB;
using namespace NTracing;

static const auto& Logger = ClickHouseYtLogger;

////////////////////////////////////////////////////////////////////////////////

class TTcpHandlerFactory
    : public Poco::Net::TCPServerConnectionFactory
{
private:
    THost* Host_;
    IServer& Server;

public:
    TTcpHandlerFactory(THost* host, IServer& server)
        : Host_(host)
        , Server(server)
    { }

    Poco::Net::TCPServerConnection* createConnection(const Poco::Net::StreamSocket& socket) override;
};

////////////////////////////////////////////////////////////////////////////////

Poco::Net::TCPServerConnection* TTcpHandlerFactory::createConnection(const Poco::Net::StreamSocket& socket)
{
    class TTcpHandler
        : public DB::TCPHandler
    {
    public:
        TTcpHandler(THost* host, DB::IServer& server, const Poco::Net::StreamSocket& socket)
            : DB::TCPHandler(server, socket, false)
            , Host_(host)
        { }

        virtual void customizeContext(DB::Context& context) override
        {
            context.getClientInfo().current_user = context.getClientInfo().initial_user;
            TQueryId queryId;
            TSpanContext parentSpan;
            auto& clientInfo = context.getClientInfo();

            TTraceContextPtr traceContext;

            // For secondary queries, query id looks like <query_id>@<parent_trace_id>@<parent_span_id>@<parent_sampled>.
            // Parent trace id is the same as client info initial_query_id.
            if (static_cast<int>(context.getClientInfo().query_kind) == 2 /* secondary query */) {
                auto requestCompositeQueryId = clientInfo.current_query_id;
                auto requestInitialQueryId = clientInfo.initial_query_id;
                YT_LOG_DEBUG("Parsing composite query id and initial query id (RequestCompositeQueryId: %v, RequestInitialQueryId: %v)",
                    requestCompositeQueryId,
                    requestInitialQueryId);
                std::vector<TString> parts;
                StringSplitter(requestCompositeQueryId).Split('@').AddTo(&parts);
                YT_VERIFY(parts.size() == 4);
                YT_VERIFY(TQueryId::FromString(parts[0], &queryId));
                YT_VERIFY(TTraceId::FromString(parts[1], &parentSpan.TraceId));
                YT_VERIFY(TryIntFromString<16>(parts[2], parentSpan.SpanId));
                auto requestSampled = parts[3];
                YT_VERIFY(requestSampled == "T" || requestSampled == "F");
                context.getClientInfo().current_query_id = parts[0];
                YT_LOG_INFO(
                    "Query is secondary; composite query id successfully decomposed, actual query id substituted into the context "
                    "(CompositeQueryId: %v, QueryId: %v, ParentTraceId: %v, ParentSpanId: %" PRIx64 ", ParentSampled: %v)",
                    requestCompositeQueryId,
                    queryId,
                    parentSpan.TraceId,
                    parentSpan.SpanId,
                    requestSampled);
                traceContext = New<TTraceContext>(parentSpan, "TcpHandler");
                if (requestSampled == "T") {
                    traceContext->SetSampled();
                }
            } else {
                // TODO(max42): support.
                THROW_ERROR_EXCEPTION("Queries via native TCP protocol are not supported (CHYT-342)");
            }

            YT_LOG_DEBUG("Registering new user (UserName: %v)", context.getClientInfo().current_user);
            RegisterNewUser(context.getAccessControlManager(), TString(context.getClientInfo().current_user));
            YT_LOG_DEBUG("User registered");

            SetupHostContext(Host_, context, queryId, std::move(traceContext));
        }

    private:
        THost* const Host_;
    };

    return new TTcpHandler(Host_, Server, socket);
}

////////////////////////////////////////////////////////////////////////////////

Poco::Net::TCPServerConnectionFactory::Ptr CreateTcpHandlerFactory(THost* host, IServer& server)
{
    return new TTcpHandlerFactory(host, server);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
