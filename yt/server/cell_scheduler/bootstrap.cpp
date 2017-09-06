#include "bootstrap.h"
#include "config.h"

#include <yt/server/admin_server/admin_service.h>

#include <yt/server/job_proxy/config.h>

#include <yt/server/misc/address_helpers.h>
#include <yt/server/misc/build_attributes.h>

#include <yt/server/scheduler/config.h>
#include <yt/server/scheduler/job_prober_service.h>
#include <yt/server/scheduler/job_tracker_service.h>
#include <yt/server/scheduler/private.h>
#include <yt/server/scheduler/scheduler.h>
#include <yt/server/scheduler/scheduler_service.h>

#include <yt/server/controller_agent/job_spec_service.h>
#include <yt/server/controller_agent/controller_agent.h>

#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/native_connection.h>

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

#include <yt/ytlib/node_tracker_client/node_directory.h>
#include <yt/ytlib/node_tracker_client/node_directory_synchronizer.h>

#include <yt/core/bus/config.h>
#include <yt/core/bus/server.h>
#include <yt/core/bus/tcp_server.h>

#include <yt/core/concurrency/fair_share_action_queue.h>
#include <yt/core/concurrency/thread_pool.h>

#include <yt/core/misc/address.h>
#include <yt/core/misc/core_dumper.h>
#include <yt/core/misc/ref_counted_tracker.h>
#include <yt/core/misc/lfalloc_helpers.h>
#include <yt/core/misc/proc.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/rpc/bus_channel.h>
#include <yt/core/rpc/bus_server.h>
#include <yt/core/rpc/response_keeper.h>
#include <yt/core/rpc/retrying_channel.h>
#include <yt/core/rpc/server.h>

#include <yt/core/tools/tools.h>

#include <yt/core/ytree/virtual.h>
#include <yt/core/ytree/ypath_client.h>

namespace NYT {
namespace NCellScheduler {

using namespace NAdmin;
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
using namespace NHiveClient;
using namespace NApi;
using namespace NNodeTrackerClient;
using namespace NControllerAgent;

////////////////////////////////////////////////////////////////////////////////

static const NLogging::TLogger Logger("Bootstrap");

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(TCellSchedulerConfigPtr config, INodePtr configNode)
    : Config_(std::move(config))
    , ConfigNode_(std::move(configNode))
{ }

TBootstrap::~TBootstrap() = default;

void TBootstrap::Run()
{
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
    LOG_INFO("Starting scheduler (MasterAddresses: %v)",
        Config_->ClusterConnection->PrimaryMaster->Addresses);

    if (Config_->Scheduler->ControlThreadPriority) {
        WaitFor(BIND([priority = *Config_->Scheduler->ControlThreadPriority] {
                auto config = New<TSetThreadPriorityConfig>();
                config->ThreadId = GetCurrentThreadId();
                config->Priority = priority;
                NTools::RunTool<TSetThreadPriorityAsRootTool>(config);
            })
            .AsyncVia(GetControlInvoker())
            .Run())
            .ThrowOnError();
    }

    TNativeConnectionOptions connectionOptions;
    connectionOptions.RetryRequestQueueSizeLimitExceeded = true;
    Connection_ = CreateNativeConnection(Config_->ClusterConnection, connectionOptions);

    TClientOptions clientOptions;
    clientOptions.User = NSecurityClient::SchedulerUserName;
    Client_ = Connection_->CreateNativeClient(clientOptions);

    BusServer_ = CreateTcpBusServer(Config_->BusServer);

    RpcServer_ = CreateBusServer(BusServer_);

    HttpServer_.reset(new NHttp::TServer(
        Config_->MonitoringPort,
        Config_->BusServer->BindRetryCount,
        Config_->BusServer->BindRetryBackoff));

    NodeDirectory_ = New<TNodeDirectory>();

    NodeDirectorySynchronizer_ = New<TNodeDirectorySynchronizer>(
        Config_->NodeDirectorySynchronizer,
        Connection_,
        NodeDirectory_);
    NodeDirectorySynchronizer_->Start();

    ControllerAgent_ = New<TControllerAgent>(Config_->Scheduler, this);

    Scheduler_ = New<TScheduler>(Config_->Scheduler, this);

    ResponseKeeper_ = New<TResponseKeeper>(
        Config_->ResponseKeeper,
        GetControlInvoker(),
        SchedulerLogger,
        SchedulerProfiler);

    if (Config_->CoreDumper) {
        CoreDumper_ = New<TCoreDumper>(Config_->CoreDumper);
    }

    MonitoringManager_ = New<TMonitoringManager>();
    MonitoringManager_->Register(
        "/ref_counted",
        TRefCountedTracker::Get()->GetMonitoringProducer());
    MonitoringManager_->Start();

    LFAllocProfiler_ = std::make_unique<NLFAlloc::TLFAllocProfiler>();

    Scheduler_->Initialize();

    auto orchidRoot = NYTree::GetEphemeralNodeFactory(true)->CreateMap();
    SetNodeByYPath(
        orchidRoot,
        "/monitoring",
        CreateVirtualNode(MonitoringManager_->GetService()));
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
            ->GetOrchidService()));

    SetBuildAttributes(orchidRoot, "scheduler");

    RpcServer_->RegisterService(CreateAdminService(GetControlInvoker(), CoreDumper_));

    RpcServer_->RegisterService(CreateOrchidService(
        orchidRoot,
        GetControlInvoker()));

    HttpServer_->Register(
        "/orchid",
        NMonitoring::GetYPathHttpHandler(orchidRoot));

    RpcServer_->RegisterService(CreateSchedulerService(this));
    RpcServer_->RegisterService(CreateJobTrackerService(this));
    RpcServer_->RegisterService(CreateJobProberService(this));
    RpcServer_->RegisterService(CreateJobSpecsService(this));

    LOG_INFO("Listening for HTTP requests on port %v", Config_->MonitoringPort);
    HttpServer_->Start();

    LOG_INFO("Listening for RPC requests on port %v", Config_->RpcPort);
    RpcServer_->Configure(Config_->RpcServer);
    RpcServer_->Start();
}

const TCellSchedulerConfigPtr& TBootstrap::GetConfig() const
{
    return Config_;
}

const INativeClientPtr& TBootstrap::GetMasterClient() const
{
    return Client_;
}

TAddressMap TBootstrap::GetLocalAddresses() const
{
    return NYT::GetLocalAddresses(Config_->Addresses, Config_->RpcPort);
}

TNetworkPreferenceList TBootstrap::GetLocalNetworks() const
{
    return Config_->Addresses.empty()
        ? DefaultNetworkPreferences
        : GetIths<0>(Config_->Addresses);
}

IInvokerPtr TBootstrap::GetControlInvoker(EControlQueue queue) const
{
    return ControlQueue_->GetInvoker(static_cast<int>(queue));
}

const TSchedulerPtr& TBootstrap::GetScheduler() const
{
    return Scheduler_;
}

const TControllerAgentPtr& TBootstrap::GetControllerAgent() const
{
    return ControllerAgent_;
}

const TNodeDirectoryPtr& TBootstrap::GetNodeDirectory() const
{
    return NodeDirectory_;
}

const TResponseKeeperPtr& TBootstrap::GetResponseKeeper() const
{
    return ResponseKeeper_;
}

const TCoreDumperPtr& TBootstrap::GetCoreDumper() const
{
    return CoreDumper_;
}

INativeConnectionPtr TBootstrap::FindRemoteConnection(TCellTag cellTag)
{
    if (cellTag == Connection_->GetCellTag()) {
        return Connection_;
    }

    auto remoteConnection = Connection_->GetClusterDirectory()->FindConnection(cellTag);
    if (!remoteConnection) {
        return nullptr;
    }

    return dynamic_cast<INativeConnection*>(remoteConnection.Get());
}

INativeConnectionPtr TBootstrap::GetRemoteConnectionOrThrow(TCellTag cellTag)
{
    auto connection = FindRemoteConnection(cellTag);
    if (!connection) {
        THROW_ERROR_EXCEPTION("Cannot find cluster with cell tag %v", cellTag);
    }
    return connection;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellScheduler
} // namespace NYT
