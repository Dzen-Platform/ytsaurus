#include "admin.h"
#include "box.h"
#include "config.h"
#include "native_connection.h"
#include "private.h"

#include <yt/ytlib/admin/admin_service_proxy.h>

#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/cell_directory_synchronizer.h>

#include <yt/ytlib/hydra/hydra_service_proxy.h>

#include <yt/ytlib/node_tracker_client/node_tracker_service_proxy.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/core/concurrency/scheduler.h>

namespace NYT {
namespace NApi {

////////////////////////////////////////////////////////////////////////////////

using namespace NAdmin;
using namespace NConcurrency;
using namespace NRpc;
using namespace NObjectClient;
using namespace NTabletClient;
using namespace NNodeTrackerClient;
using namespace NHydra;
using namespace NHiveClient;

DECLARE_REFCOUNTED_CLASS(TNativeAdmin)

////////////////////////////////////////////////////////////////////////////////

class TNativeAdmin
    : public IAdmin
{
public:
    TNativeAdmin(
        INativeConnectionPtr connection,
        const TAdminOptions& options)
        : Connection_(std::move(connection))
        , Options_(options)
        // NB: Cannot actually throw.
    {
        Logger.AddTag("Admin: %p", this);
        Y_UNUSED(Options_);
    }

#define DROP_BRACES(...) __VA_ARGS__
#define IMPLEMENT_METHOD(returnType, method, signature, args) \
    virtual TFuture<returnType> method signature override \
    { \
        return Execute( \
            #method, \
            BIND( \
                &TNativeAdmin::Do ## method, \
                MakeStrong(this), \
                DROP_BRACES args)); \
    }

    IMPLEMENT_METHOD(int, BuildSnapshot, (
        const TBuildSnapshotOptions& options),
        (options))
    IMPLEMENT_METHOD(void, GCCollect, (
        const TGCCollectOptions& options),
        (options))
    IMPLEMENT_METHOD(void, KillProcess, (
        const Stroka& address,
        const TKillProcessOptions& options),
        (address, options))
    IMPLEMENT_METHOD(Stroka, WriteCoreDump, (
        const Stroka& address,
        const TWriteCoreDumpOptions& options),
        (address, options))

private:
    const INativeConnectionPtr Connection_;
    const TAdminOptions Options_;

    const IChannelPtr LeaderChannel_;

    NLogging::TLogger Logger = ApiLogger;


    template <class T>
    TFuture<T> Execute(const Stroka& commandName, TCallback<T()> callback)
    {
        return BIND([=, this_ = MakeStrong(this)] () {
                try {
                    LOG_DEBUG("Command started (Command: %v)", commandName);
                    TBox<T> result(callback);
                    LOG_DEBUG("Command completed (Command: %v)", commandName);
                    return result.Unwrap();
                } catch (const std::exception& ex) {
                    LOG_DEBUG(ex, "Command failed (Command: %v)", commandName);
                    throw;
                }
            })
            .AsyncVia(Connection_->GetLightInvoker())
            .Run();
    }

    int DoBuildSnapshot(const TBuildSnapshotOptions& options)
    {
        const auto& cellDirectory = Connection_->GetCellDirectory();

        auto cellDirectorySynchronizer = New<TCellDirectorySynchronizer>(
            New<TCellDirectorySynchronizerConfig>(),
            cellDirectory,
            Connection_->GetPrimaryMasterCellId());

        WaitFor(cellDirectorySynchronizer->Sync())
            .ThrowOnError();

        auto cellId = options.CellId ? options.CellId : Connection_->GetPrimaryMasterCellId();
        auto channel = cellDirectory->GetChannelOrThrow(cellId);

        THydraServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(TDuration::Hours(1)); // effective infinity

        auto req = proxy.ForceBuildSnapshot();
        req->set_set_read_only(options.SetReadOnly);

        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();

        return rsp->snapshot_id();
    }

    void DoGCCollect(const TGCCollectOptions& /*options*/)
    {
        std::vector<TFuture<void>> asyncResults;

        auto collectAtCell = [&] (TCellTag cellTag) {
            auto channel = Connection_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag);
            TObjectServiceProxy proxy(channel);
            proxy.SetDefaultTimeout(Null); // infinity
            auto req = proxy.GCCollect();
            auto asyncResult = req->Invoke().As<void>();
            asyncResults.push_back(asyncResult);
        };

        collectAtCell(Connection_->GetPrimaryMasterCellTag());
        for (auto cellTag : Connection_->GetSecondaryMasterCellTags()) {
            collectAtCell(cellTag);
        }

        WaitFor(Combine(asyncResults))
            .ThrowOnError();
    }

    void DoKillProcess(const Stroka& address, const TKillProcessOptions& options)
    {
        auto channel = Connection_->GetLightChannelFactory()->CreateChannel(address);

        TAdminServiceProxy proxy(channel);
        auto req = proxy.Die();
        req->set_exit_code(options.ExitCode);
        auto asyncResult = req->Invoke().As<void>();
        // NB: this will always throw an error since the service can
        // never reply to the request because it makes _exit immediately.
        // This is an intended behavior.
        WaitFor(asyncResult)
            .ThrowOnError();
    }

    Stroka DoWriteCoreDump(const Stroka& address, const TWriteCoreDumpOptions& options)
    {
        auto channel = Connection_->GetLightChannelFactory()->CreateChannel(address);

        TAdminServiceProxy proxy(channel);
        auto req = proxy.WriteCoreDump();
        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();
        return rsp->path();
    }
};

DEFINE_REFCOUNTED_TYPE(TNativeAdmin)

IAdminPtr CreateNativeAdmin(
    INativeConnectionPtr connection,
    const TAdminOptions& options)
{
    return New<TNativeAdmin>(std::move(connection), options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT
