#include "bootstrap.h"
#include "config.h"

#include <yt/server/job_proxy/config.h>

#include <yt/server/misc/build_attributes.h>

#include <yt/server/scheduler/config.h>
#include <yt/server/scheduler/job_prober_service.h>
#include <yt/server/scheduler/job_tracker_service.h>
#include <yt/server/scheduler/private.h>
#include <yt/server/scheduler/scheduler.h>
#include <yt/server/scheduler/scheduler_service.h>

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/connection.h>

#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/cluster_directory.h>

#include <yt/ytlib/hydra/config.h>
#include <yt/ytlib/hydra/peer_channel.h>

#include <yt/ytlib/monitoring/http_integration.h>
#include <yt/ytlib/monitoring/http_server.h>
#include <yt/ytlib/monitoring/monitoring_manager.h>

#include <yt/ytlib/orchid/orchid_service.h>

#include <yt/ytlib/scheduler/config.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/ytlib/transaction_client/remote_timestamp_provider.h>
#include <yt/ytlib/transaction_client/timestamp_provider.h>

#include <yt/ytlib/chunk_client/throttler_manager.h>

#include <yt/core/bus/config.h>
#include <yt/core/bus/server.h>
#include <yt/core/bus/tcp_server.h>

#include <yt/core/concurrency/fair_share_action_queue.h>
#include <yt/core/concurrency/thread_pool.h>

#include <yt/core/misc/address.h>
#include <yt/core/misc/ref_counted_tracker.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/rpc/bus_channel.h>
#include <yt/core/rpc/bus_server.h>
#include <yt/core/rpc/response_keeper.h>
#include <yt/core/rpc/retrying_channel.h>
#include <yt/core/rpc/server.h>

#include <yt/core/ytree/virtual.h>
#include <yt/core/ytree/ypath_client.h>

namespace NYT {
namespace NCellScheduler {

using namespace NBus;
using namespace NElection;
using namespace NHydra;
using namespace NMonitoring;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NOrchid;
using namespace NProfiling;
using namespace NRpc;
using namespace NScheduler;
using namespace NTransactionClient;
using namespace NYTree;
using namespace NConcurrency;
using namespace NHive;
using namespace NApi;

////////////////////////////////////////////////////////////////////////////////

static const NLogging::TLogger Logger("Bootstrap");

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(const INodePtr configNode)
    : ConfigNode_(configNode)
{ }

TBootstrap::~TBootstrap()
{ }

void TBootstrap::Run()
{
    srand(time(nullptr));

    ControlQueue_ = New<TFairShareActionQueue>("Control", TEnumTraits<EControlQueue>::GetDomainNames());

    BIND(&TBootstrap::DoRun, this)
        .AsyncVia(GetControlInvoker())
        .Run()
        .Get()
        .ThrowOnError();

    Sleep(TDuration::Max());
}

void TBootstrap::DoRun()
{
    try {
        Config_ = ConvertTo<TCellSchedulerConfigPtr>(ConfigNode_);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing cell scheduler configuration")
                << ex;
    }

    LocalAddress_ = BuildServiceAddress(
        TAddressResolver::Get()->GetLocalHostName(),
        Config_->RpcPort);

    LOG_INFO("Starting scheduler (LocalAddress: %v, MasterAddresses: %v)",
        LocalAddress_,
        Config_->ClusterConnection->PrimaryMaster->Addresses);

    TConnectionOptions connectionOptions;
    connectionOptions.RetryRequestRateLimitExceeded = true;
    auto connection = CreateConnection(Config_->ClusterConnection, connectionOptions);

    TClientOptions clientOptions;
    clientOptions.User = NSecurityClient::SchedulerUserName;
    MasterClient_ = connection->CreateClient(clientOptions);

    BusServer_ = CreateTcpBusServer(Config_->BusServer);

    RpcServer_ = CreateBusServer(BusServer_);

    HttpServer_.reset(new NHttp::TServer(
        Config_->MonitoringPort,
        Config_->BusServer->BindRetryCount,
        Config_->BusServer->BindRetryBackoff));

    ClusterDirectory_ = New<TClusterDirectory>(MasterClient_->GetConnection());

    Scheduler_ = New<TScheduler>(Config_->Scheduler, this);

    ChunkLocationThrottlerManager_ = New<TThrottlerManager>(
            Config_->Scheduler->ChunkLocationThrottler,
            SchedulerLogger,
            SchedulerProfiler);

    ResponseKeeper_ = New<TResponseKeeper>(
        Config_->ResponseKeeper,
        SchedulerLogger,
        SchedulerProfiler);

    auto monitoringManager = New<TMonitoringManager>();
    monitoringManager->Register(
        "/ref_counted",
        TRefCountedTracker::Get()->GetMonitoringProducer());
    monitoringManager->Start();

    auto orchidFactory = NYTree::GetEphemeralNodeFactory();
    auto orchidRoot = orchidFactory->CreateMap();
    SetNodeByYPath(
        orchidRoot,
        "/monitoring",
        CreateVirtualNode(monitoringManager->GetService()));
    SetNodeByYPath(
        orchidRoot,
        "/profiling",
        CreateVirtualNode(TProfileManager::Get()->GetService()));
    SetNodeByYPath(
        orchidRoot,
        "/config",
        ConfigNode_);
    SetNodeByYPath(
        orchidRoot,
        "/scheduler",
        CreateVirtualNode(
            Scheduler_
            ->GetOrchidService()
            ->Via(GetControlInvoker())
            ->Cached(Config_->OrchidCacheUpdatePeriod)));

    SetBuildAttributes(orchidRoot, "scheduler");

    RpcServer_->RegisterService(CreateOrchidService(
        orchidRoot,
        GetControlInvoker()));

    HttpServer_->Register(
        "/orchid",
        NMonitoring::GetYPathHttpHandler(orchidRoot));

    RpcServer_->RegisterService(CreateSchedulerService(this));
    RpcServer_->RegisterService(CreateJobTrackerService(this));
    RpcServer_->RegisterService(CreateJobProberService(this));

    LOG_INFO("Listening for HTTP requests on port %v", Config_->MonitoringPort);
    HttpServer_->Start();

    LOG_INFO("Listening for RPC requests on port %v", Config_->RpcPort);
    RpcServer_->Configure(Config_->RpcServer);
    RpcServer_->Start();

    Scheduler_->Initialize();
}

TCellSchedulerConfigPtr TBootstrap::GetConfig() const
{
    return Config_;
}

IClientPtr TBootstrap::GetMasterClient() const
{
    return MasterClient_;
}

const Stroka& TBootstrap::GetLocalAddress() const
{
    return LocalAddress_;
}

IInvokerPtr TBootstrap::GetControlInvoker(EControlQueue queue) const
{
    return ControlQueue_->GetInvoker(static_cast<int>(queue));
}

TSchedulerPtr TBootstrap::GetScheduler() const
{
    return Scheduler_;
}

TClusterDirectoryPtr TBootstrap::GetClusterDirectory() const
{
    return ClusterDirectory_;
}

TResponseKeeperPtr TBootstrap::GetResponseKeeper() const
{
    return ResponseKeeper_;
}

TThrottlerManagerPtr TBootstrap::GetChunkLocationThrottlerManager() const
{
    return ChunkLocationThrottlerManager_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellScheduler
} // namespace NYT
