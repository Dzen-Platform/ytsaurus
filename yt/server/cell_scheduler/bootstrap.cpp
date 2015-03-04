#include "stdafx.h"
#include "bootstrap.h"
#include "config.h"

#include <core/misc/address.h>
#include <core/misc/ref_counted_tracker.h>

#include <core/concurrency/action_queue.h>
#include <core/concurrency/throughput_throttler.h>

#include <core/bus/server.h>
#include <core/bus/tcp_server.h>
#include <core/bus/config.h>

#include <core/rpc/server.h>
#include <core/rpc/bus_server.h>
#include <core/rpc/retrying_channel.h>
#include <core/rpc/bus_channel.h>
#include <core/rpc/response_keeper.h>

#include <core/ytree/virtual.h>
#include <core/ytree/ypath_client.h>
#include <core/ytree/yson_file_service.h>

#include <core/profiling/profile_manager.h>

#include <ytlib/api/connection.h>
#include <ytlib/api/client.h>

#include <ytlib/hydra/peer_channel.h>
#include <ytlib/hydra/config.h>

#include <ytlib/orchid/orchid_service.h>

#include <ytlib/monitoring/monitoring_manager.h>
#include <ytlib/monitoring/http_server.h>
#include <ytlib/monitoring/http_integration.h>

#include <ytlib/scheduler/config.h>

#include <ytlib/transaction_client/transaction_manager.h>
#include <ytlib/transaction_client/timestamp_provider.h>
#include <ytlib/transaction_client/remote_timestamp_provider.h>

#include <ytlib/hive/cell_directory.h>
#include <ytlib/hive/cluster_directory.h>

#include <ytlib/security_client/public.h>

#include <server/misc/build_attributes.h>

#include <server/job_proxy/config.h>

#include <server/scheduler/scheduler.h>
#include <server/scheduler/scheduler_service.h>
#include <server/scheduler/job_tracker_service.h>
#include <server/scheduler/job_prober_service.h>
#include <server/scheduler/config.h>
#include <server/scheduler/private.h>

namespace NYT {
namespace NCellScheduler {

using namespace NBus;
using namespace NElection;
using namespace NHydra;
using namespace NMonitoring;
using namespace NObjectClient;
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

TBootstrap::TBootstrap(
    const Stroka& configFileName,
    TCellSchedulerConfigPtr config)
    : ConfigFileName_(configFileName)
    , Config_(config)
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
    LocalAddress_ = BuildServiceAddress(
        TAddressResolver::Get()->GetLocalHostName(),
        Config_->RpcPort);

    LOG_INFO("Starting scheduler (LocalAddress: %v, MasterAddresses: [%v])",
        LocalAddress_,
        JoinToString(Config_->ClusterConnection->Master->Addresses));

    auto isRetriableError = BIND([] (const TError& error) -> bool {
        auto code = error.GetCode();
        if (code == NSecurityClient::EErrorCode::RequestRateLimitExceeded) {
            return true;
        }
        return IsRetriableError(error);
    });

    auto connection = CreateConnection(Config_->ClusterConnection, isRetriableError);

    TClientOptions clientOptions;
    clientOptions.User = NSecurityClient::SchedulerUserName;
    MasterClient_ = connection->CreateClient(clientOptions);

    BusServer_ = CreateTcpBusServer(TTcpBusServerConfig::CreateTcp(Config_->RpcPort));

    RpcServer_ = CreateBusServer(BusServer_);

    HttpServer_.reset(new NHttp::TServer(Config_->MonitoringPort));

    ClusterDirectory_ = New<TClusterDirectory>(MasterClient_->GetConnection());

    Scheduler_ = New<TScheduler>(Config_->Scheduler, this);

    ChunkLocationThrottler_ = CreateLimitedThrottler(Config_->Scheduler->ChunkLocationThrottler);

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
        CreateVirtualNode(NYTree::CreateYsonFileService(ConfigFileName_)));
    SetNodeByYPath(
        orchidRoot,
        "/scheduler",
        CreateVirtualNode(
            Scheduler_
            ->GetOrchidService()
            ->Via(GetControlInvoker())
            ->Cached(Config_->OrchidCacheExpirationTime)));

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

IThroughputThrottlerPtr TBootstrap::GetChunkLocationThrottler() const
{
    return ChunkLocationThrottler_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellScheduler
} // namespace NYT
