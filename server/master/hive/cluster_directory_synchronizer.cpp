#include "cluster_directory_synchronizer.h"
#include "private.h"

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/ytree/ypath_client.h>

#include <yt/server/lib/hive/config.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/master_ypath_proxy.h>

#include <yt/ytlib/api/native/config.h>
#include <yt/ytlib/api/native/connection.h>
#include <yt/ytlib/api/native/rpc_helpers.h>

#include <yt/ytlib/hive/cluster_directory.h>

#include <yt/server/master/cell_master/bootstrap.h>
#include <yt/server/master/cell_master/multicell_manager.h>
#include <yt/server/master/cell_master/hydra_facade.h>

#include <yt/server/master/object_server/object_manager.h>
#include <yt/server/master/object_server/object_proxy.h>

namespace NYT::NHiveServer {

using namespace NConcurrency;
using namespace NYTree;
using namespace NApi;
using namespace NApi::NNative;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = HiveServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TClusterDirectorySynchronizer::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TClusterDirectorySynchronizerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap,
        const NHiveClient::TClusterDirectoryPtr& clusterDirectory)
        : Bootstrap_(bootstrap)
        , SyncExecutor_(New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::ClusterDirectorySynchronizer),
            BIND(&TImpl::OnSync, MakeWeak(this)),
            config->SyncPeriod))
        , ObjectManager_(Bootstrap_->GetObjectManager())
        , MulticellManager_(Bootstrap_->GetMulticellManager())
        , CellTag_(MulticellManager_->GetPrimaryCellTag())
        , ClusterDirectory_(clusterDirectory)
        , Config_(config)
    { }

    void Start()
    {
        auto guard = Guard(SpinLock_);
        DoStart();
    }

    void Stop()
    {
        auto guard = Guard(SpinLock_);
        DoStop();
    }

    TFuture<void> Sync(bool force)
    {
        auto guard = Guard(SpinLock_);
        if (Stopped_) {
            return MakeFuture(TError("Cluster directory synchronizer is stopped"));
        }
        DoStart(force);
        return SyncPromise_.ToFuture();
    }

    DEFINE_SIGNAL(void(const TError&), Synchronized);

private:
    NCellMaster::TBootstrap* Bootstrap_;
    const TPeriodicExecutorPtr SyncExecutor_;
    const NObjectServer::TObjectManagerPtr ObjectManager_;
    const NCellMaster::TMulticellManagerPtr MulticellManager_;
    const NObjectClient::TCellTag CellTag_;
    const NHiveClient::TClusterDirectoryPtr ClusterDirectory_;
    const TClusterDirectorySynchronizerConfigPtr Config_;

    TSpinLock SpinLock_;
    bool Started_ = false;
    bool Stopped_= false;
    TPromise<void> SyncPromise_ = NewPromise<void>();

    void DoStart(bool force = false)
    {
        if (Started_) {
            if (force) {
                SyncExecutor_->ScheduleOutOfBand();
            }
            return;
        }
        Started_ = true;
        SyncExecutor_->Start();
        SyncExecutor_->ScheduleOutOfBand();
    }

    void DoStop()
    {
        if (Stopped_) {
            return;
        }
        Stopped_ = true;
        SyncExecutor_->Stop();
    }

    void DoSync()
    {
        try {
            auto req = NObjectClient::TMasterYPathProxy::GetClusterMeta();
            req->set_populate_cluster_directory(true);

            TMasterReadOptions options{
                EMasterChannelKind::Cache,
                Config_->ExpireAfterSuccessfulUpdateTime,
                Config_->ExpireAfterFailedUpdateTime,
                1
            };

            SetBalancingHeader(req, Bootstrap_->GetClusterConnection()->GetConfig(), options);
            SetCachingHeader(req, Bootstrap_->GetClusterConnection()->GetConfig(), options);

            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            if (multicellManager->IsSecondaryMaster()) {
                auto channel = MulticellManager_->FindMasterChannel(CellTag_, NHydra::EPeerKind::Follower);
                NObjectClient::TObjectServiceProxy proxy(channel);
                auto batchReq = proxy.ExecuteBatch();
                batchReq->AddRequest(req, "get_cluster_meta");
                auto batchRes = WaitFor(batchReq->Invoke())
                    .ValueOrThrow();

                auto res = batchRes->GetResponse<NObjectClient::TMasterYPathProxy::TRspGetClusterMeta>(0)
                    .ValueOrThrow();

                ClusterDirectory_->UpdateDirectory(res->cluster_directory());
            } else {
                auto res = WaitFor(ExecuteVerb(ObjectManager_->GetMasterProxy(), req))
                    .ValueOrThrow();

                ClusterDirectory_->UpdateDirectory(res->cluster_directory());
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error updating cluster directory")
                << ex;
        }
    }

    void OnSync()
    {
        TError error;
        try {
            DoSync();
            Synchronized_.Fire(TError());
        } catch (const std::exception& ex) {
            error = TError(ex);
            Synchronized_.Fire(error);
            YT_LOG_DEBUG(error);
        }

        auto guard = Guard(SpinLock_);
        auto syncPromise = NewPromise<void>();
        std::swap(syncPromise, SyncPromise_);
        guard.Release();
        syncPromise.Set(error);
    }
};

TClusterDirectorySynchronizer::TClusterDirectorySynchronizer(
    const TClusterDirectorySynchronizerConfigPtr& config,
    NCellMaster::TBootstrap* bootstrap,
    const NHiveClient::TClusterDirectoryPtr& clusterDirectory)
    : Impl_(New<TImpl>(config, bootstrap, clusterDirectory))
{ }

void TClusterDirectorySynchronizer::Start()
{
    Impl_->Start();
}

void TClusterDirectorySynchronizer::Stop()
{
    Impl_->Stop();
}

TFuture<void> TClusterDirectorySynchronizer::Sync(bool force)
{
    return Impl_->Sync(force);
}

DELEGATE_SIGNAL(TClusterDirectorySynchronizer, void(const TError&), Synchronized, *Impl_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveServer
