#include "health_checker.h"

#include "config.h"
#include "query_context.h"
#include "helpers.h"
#include "yt/server/clickhouse_server/private.h"

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/intrusive_ptr.h>

#include <yt/core/profiling/profile_manager.h>

#include <Interpreters/ClientInfo.h>
#include <Interpreters/executeQuery.h>

#include <Core/Types.h>


namespace NYT::NClickHouseServer {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ClickHouseYtLogger;

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

std::vector<NProfiling::TTagId> RegisterQueryTags(size_t queryCount)
{
    std::vector<NProfiling::TTagId> queryTags;
    for (size_t queryIndex = 0; queryIndex < queryCount; ++queryIndex) {
        queryTags.emplace_back(NProfiling::TProfileManager::Get()->RegisterTag("query_index", queryIndex));
    }
    return queryTags;
}

DB::Context PrepareContextForQuery(
    DB::Context* databaseContext,
    const TString& dataBaseUser,
    TDuration timeout,
    THost* host)
{
    DB::Context contextForQuery = *databaseContext;

    contextForQuery.setUser(dataBaseUser,
        /*password =*/"",
        Poco::Net::SocketAddress());

    auto settings = contextForQuery.getSettings();
    settings.max_execution_time = Poco::Timespan(timeout.Seconds(), timeout.MicroSecondsOfSecond());
    contextForQuery.setSettings(settings);

    auto queryId = TQueryId::Create();

    auto& clientInfo = contextForQuery.getClientInfo();
    clientInfo.initial_user = clientInfo.current_user;
    clientInfo.query_kind = DB::ClientInfo::QueryKind::INITIAL_QUERY;
    clientInfo.initial_query_id = ToString(queryId);

    contextForQuery.makeQueryContext();

    NTracing::TSpanContext spanContext{NTracing::TTraceId::Create(),
        NTracing::InvalidSpanId,
        /*sampled =*/false,
        /*debug =*/false};

    auto traceContext =
        New<NTracing::TTraceContext>(spanContext, /*spanName =*/"HealthCheckerQuery");

    SetupHostContext(host, contextForQuery, queryId, std::move(traceContext));

    return contextForQuery;
}

void ValidateQueryResult(DB::BlockIO& blockIO)
{
    auto stream = blockIO.getInputStream();
    size_t totalRowCount = 0;
    while (auto block = stream->read()) {
        totalRowCount += block.rows();
    }
    YT_LOG_DEBUG("Health checker query result validated (TotalRowCount: %v)", totalRowCount);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail


void THealthChecker::ExecuteQuery(const TString& query)
{
    auto context = NDetail::PrepareContextForQuery(DatabaseContext_, DatabaseUser_, Config_->Timeout, Host_);
    auto blockIO = DB::executeQuery(query, context, true /* internal */);
    NDetail::ValidateQueryResult(blockIO);
}

THealthChecker::THealthChecker(
    THealthCheckerConfigPtr config,
    TString dataBaseUser,
    DB::Context* databaseContext,
    THost* host)
    : Config_(std::move(config))
    , DatabaseUser_(std::move(dataBaseUser))
    , DatabaseContext_(databaseContext)
    , Host_(host)
    , ActionQueue_(New<TActionQueue>("HealthChecker"))
    , PeriodicExecutor_(New<TPeriodicExecutor>(
        ActionQueue_->GetInvoker(),
        BIND(&THealthChecker::ExecuteQueries, MakeWeak(this)),
        Config_->Period))
{
    RegisterNewUser(DatabaseContext_->getAccessControlManager(), DatabaseUser_);

    for (int i = 0; i < Config_->Queries.size(); ++i) {
        QueryIndexToStatus_.push_back(ClickHouseYtProfiler
            .WithTag("query_index", ToString(i))
            .Gauge("/health_checker/success"));
    }
}

void THealthChecker::Start()
{
    YT_LOG_DEBUG("Health checker started (Period: %v, QueryCount: %v)",
        Config_->Period,
        Config_->Queries.size());
    PeriodicExecutor_->Start();
}

void THealthChecker::ExecuteQueries()
{
    for (size_t queryIndex = 0; queryIndex < Config_->Queries.size(); ++queryIndex) {
        const auto& query = Config_->Queries[queryIndex];
        YT_LOG_DEBUG("Executing health checker query (Index: %v, Query: %v)", queryIndex, query);

        auto error = WaitFor(BIND(&THealthChecker::ExecuteQuery, MakeWeak(this), query)
            .AsyncVia(ActionQueue_->GetInvoker())
            .Run()
            .WithTimeout(Config_->Timeout));

        if (error.IsOK()) {
            YT_LOG_DEBUG("Health checker query successfully executed (Index: %v, Query: %v)",
                queryIndex,
                query);
        } else {
            YT_LOG_WARNING(error,
                "Health checker query failed (Index: %v, Query: %v)",
                queryIndex,
                query);
        }

        QueryIndexToStatus_[queryIndex].Update(error.IsOK() ? 1.0 : 0.0);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
