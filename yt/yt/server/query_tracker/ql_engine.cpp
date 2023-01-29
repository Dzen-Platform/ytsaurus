#include "ql_engine.h"

#include "handler_base.h"

#include <yt/yt/ytlib/query_tracker_client/records/query.record.h>

#include <yt/yt/ytlib/hive/cluster_directory.h>

#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/core/ytree/convert.h>
#include <yt/yt/core/ytree/attributes.h>

namespace NYT::NQueryTracker {

using namespace NQueryTrackerClient;
using namespace NApi;
using namespace NYPath;
using namespace NHiveClient;
using namespace NYTree;

///////////////////////////////////////////////////////////////////////////////

class TQlQueryHandler
    : public TQueryHandlerBase
{
public:
    TQlQueryHandler(
        const NApi::IClientPtr& stateClient,
        const NYPath::TYPath& stateRoot,
        const TEngineConfigBasePtr& config,
        const NQueryTrackerClient::NRecords::TActiveQuery& activeQuery,
        const NApi::IClientPtr& queryClient)
        : TQueryHandlerBase(stateClient, stateRoot, config, activeQuery)
        , Query_(activeQuery.Query)
        , QueryClient_(queryClient)
    { }

    void Start() override
    {
        YT_LOG_DEBUG("Starting QL query");
        AsyncQueryResult_ = QueryClient_->SelectRows(Query_);
        AsyncQueryResult_.Subscribe(BIND(&TQlQueryHandler::OnQueryFinish, MakeWeak(this)).Via(GetCurrentInvoker()));
    }

    void Abort() override
    {
        // Nothing smarter than that for now.
        AsyncQueryResult_.Cancel(TError("Query aborted"));
    }

    void Detach() override
    {
        // Nothing smarter than that for now.
        AsyncQueryResult_.Cancel(TError("Query detached"));
    }

private:
    TString Query_;
    NApi::IClientPtr QueryClient_;

    TFuture<TSelectRowsResult> AsyncQueryResult_;

    void OnQueryFinish(const TErrorOr<TSelectRowsResult>& queryResultOrError)
    {
        if (queryResultOrError.FindMatching(NYT::EErrorCode::Canceled)) {
            return;
        }
        if (!queryResultOrError.IsOK()) {
            OnQueryFailed(queryResultOrError);
            return;
        }
        OnQueryCompleted({queryResultOrError.Value().Rowset});
    }
};

class TQlEngine
    : public IQueryEngine
{
public:
    TQlEngine(const IClientPtr& stateClient, const TYPath& stateRoot)
        : StateClient_(stateClient)
        , StateRoot_(stateRoot)
        , ClusterDirectory_(DynamicPointerCast<NNative::IConnection>(StateClient_->GetConnection())->GetClusterDirectory())
    { }

    IQueryHandlerPtr StartOrAttachQuery(NRecords::TActiveQuery activeQuery) override
    {
        auto settings = ConvertToAttributes(activeQuery.Settings);
        auto cluster = settings->Find<TString>("cluster");
        if (!cluster) {
            THROW_ERROR_EXCEPTION("Missing required setting \"cluster\"");
        }
        auto queryClient = ClusterDirectory_->GetConnectionOrThrow(*cluster)->CreateClient(TClientOptions{.User = activeQuery.User});
        return New<TQlQueryHandler>(StateClient_, StateRoot_, Config_, activeQuery, queryClient);
    }

    void OnDynamicConfigChanged(const TEngineConfigBasePtr& config) override
    {
        Config_ = config;
    }

private:
    IClientPtr StateClient_;
    TYPath StateRoot_;
    TEngineConfigBasePtr Config_;
    TClusterDirectoryPtr ClusterDirectory_;
};

IQueryEnginePtr CreateQlEngine(const IClientPtr& stateClient, const TYPath& stateRoot)
{
    return New<TQlEngine>(stateClient, stateRoot);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient
