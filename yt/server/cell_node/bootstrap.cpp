#include "stdafx.h"
#include "bootstrap.h"
#include "config.h"

#include <core/misc/address.h>
#include <core/misc/ref_counted_tracker.h>

#include <core/concurrency/action_queue.h>

#include <core/bus/server.h>
#include <core/bus/tcp_server.h>
#include <core/bus/config.h>

#include <core/rpc/channel.h>
#include <core/rpc/bus_channel.h>
#include <core/rpc/caching_channel_factory.h>
#include <core/rpc/server.h>
#include <core/rpc/bus_server.h>
#include <core/rpc/redirector_service.h>
#include <core/rpc/throttling_channel.h>

#include <ytlib/orchid/orchid_service.h>

#include <ytlib/monitoring/monitoring_manager.h>
#include <ytlib/monitoring/http_server.h>
#include <ytlib/monitoring/http_integration.h>

#include <core/ytree/ephemeral_node_factory.h>
#include <core/ytree/virtual.h>
#include <core/ytree/yson_file_service.h>
#include <core/ytree/ypath_client.h>

#include <core/profiling/profile_manager.h>

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/chunk_client/chunk_service_proxy.h>
#include <ytlib/chunk_client/client_block_cache.h>

#include <ytlib/api/client.h>
#include <ytlib/api/connection.h>

#include <server/misc/build_attributes.h>

#include <server/data_node/config.h>
#include <server/data_node/ytree_integration.h>
#include <server/data_node/chunk_cache.h>
#include <server/data_node/peer_block_table.h>
#include <server/data_node/peer_block_updater.h>
#include <server/data_node/chunk_store.h>
#include <server/data_node/chunk_cache.h>
#include <server/data_node/chunk_registry.h>
#include <server/data_node/block_store.h>
#include <server/data_node/blob_reader_cache.h>
#include <server/data_node/journal_dispatcher.h>
#include <server/data_node/location.h>
#include <server/data_node/data_node_service.h>
#include <server/data_node/master_connector.h>
#include <server/data_node/session_manager.h>
#include <server/data_node/job.h>
#include <server/data_node/private.h>

#include <server/job_agent/job_controller.h>

#include <server/exec_agent/private.h>
#include <server/exec_agent/config.h>
#include <server/exec_agent/slot_manager.h>
#include <server/exec_agent/supervisor_service.h>
#include <server/exec_agent/job_prober_service.h>
#include <server/exec_agent/environment.h>
#include <server/exec_agent/environment_manager.h>
#include <server/exec_agent/unsafe_environment.h>
#include <server/exec_agent/scheduler_connector.h>
#include <server/exec_agent/job.h>

#include <server/tablet_node/tablet_slot_manager.h>
#include <server/tablet_node/store_flusher.h>
#include <server/tablet_node/store_compactor.h>
#include <server/tablet_node/partition_balancer.h>
#include <server/tablet_node/security_manager.h>

#include <server/query_agent/query_executor.h>
#include <server/query_agent/query_service.h>

#include <server/transaction_server/timestamp_proxy_service.h>

#include <server/object_server/master_cache_service.h>

namespace NYT {
namespace NCellNode {

using namespace NBus;
using namespace NChunkClient;
using namespace NChunkServer;
using namespace NElection;
using namespace NHydra;
using namespace NMonitoring;
using namespace NOrchid;
using namespace NProfiling;
using namespace NRpc;
using namespace NYTree;
using namespace NConcurrency;
using namespace NScheduler;
using namespace NJobAgent;
using namespace NExecAgent;
using namespace NJobProxy;
using namespace NDataNode;
using namespace NTabletNode;
using namespace NQueryAgent;
using namespace NApi;
using namespace NTransactionServer;

////////////////////////////////////////////////////////////////////////////////

static NLogging::TLogger Logger("Bootstrap");
static const i64 FootprintMemorySize = (i64) 1024 * 1024 * 1024;

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(
    const Stroka& configFileName,
    TCellNodeConfigPtr config)
    : ConfigFileName(configFileName)
    , Config(config)
    , MemoryUsageTracker(Config->ExecAgent->JobController->ResourceLimits->Memory, "/cell_node")
{ }

TBootstrap::~TBootstrap()
{ }

void TBootstrap::Run()
{
    srand(time(nullptr));

    ControlQueue = New<TActionQueue>("Control");

    auto result = BIND(&TBootstrap::DoRun, this)
        .AsyncVia(GetControlInvoker())
        .Run()
        .Get();
    THROW_ERROR_EXCEPTION_IF_FAILED(result);

    Sleep(TDuration::Max());
}

void TBootstrap::DoRun()
{
    {
        auto addresses = Config->Addresses;
        if (addresses.find(NNodeTrackerClient::DefaultNetworkName) == addresses.end()) {
            addresses[NNodeTrackerClient::DefaultNetworkName] = TAddressResolver::Get()->GetLocalHostName();
        }
        for (auto& pair : addresses) {
            pair.second = BuildServiceAddress(pair.second, Config->RpcPort);
        }
        LocalDescriptor = NNodeTrackerClient::TNodeDescriptor(addresses);
    }

    LOG_INFO("Starting node (LocalDescriptor: %v, MasterAddresses: [%v])",
        LocalDescriptor,
        JoinToString(Config->ClusterConnection->Master->Addresses));

    {
        auto result = MemoryUsageTracker.TryAcquire(
            EMemoryConsumer::Footprint,
            FootprintMemorySize);
        if (!result.IsOK()) {
            THROW_ERROR_EXCEPTION("Error allocating footprint memory")
                << result;
        }
    }

    auto clusterConnection = CreateConnection(Config->ClusterConnection);
    MasterClient = clusterConnection->CreateClient(GetRootClientOptions());

    QueryThreadPool = New<TThreadPool>(
        Config->QueryAgent->ThreadPoolSize,
        "Query");
    BoundedConcurrencyQueryPoolInvoker = CreateBoundedConcurrencyInvoker(
        QueryThreadPool->GetInvoker(),
        Config->QueryAgent->MaxConcurrentRequests);

    BusServer = CreateTcpBusServer(TTcpBusServerConfig::CreateTcp(Config->RpcPort));

    RpcServer = CreateBusServer(BusServer);

    HttpServer.reset(new NHttp::TServer(Config->MonitoringPort));

    TabletChannelFactory = CreateCachingChannelFactory(GetBusChannelFactory());

    auto monitoringManager = New<TMonitoringManager>();
    monitoringManager->Register(
        "/ref_counted",
        TRefCountedTracker::Get()->GetMonitoringProducer());

    auto throttlingMasterChannel = CreateThrottlingChannel(
        Config->MasterCacheService,
        MasterClient->GetMasterChannel(EMasterChannelKind::Leader));
    RpcServer->RegisterService(CreateRedirectorService(
        TServiceId(NChunkClient::TChunkServiceProxy::GetServiceName(), GetCellId()),
        throttlingMasterChannel));

    BlobReaderCache = New<TBlobReaderCache>(Config->DataNode);

    JournalDispatcher = New<TJournalDispatcher>(this, Config->DataNode);

    ChunkRegistry = New<TChunkRegistry>(this);

    BlockStore = New<TBlockStore>(Config->DataNode, this);

    UncompressedBlockCache = CreateClientBlockCache(
        Config->DataNode->UncompressedBlockCache,
        NProfiling::TProfiler(DataNodeProfiler.GetPathPrefix() + "/uncompressed_block_cache"));

    PeerBlockTable = New<TPeerBlockTable>(Config->DataNode->PeerBlockTable);

    PeerBlockUpdater = New<TPeerBlockUpdater>(Config->DataNode, this);

    SessionManager = New<TSessionManager>(Config->DataNode, this);

    MasterConnector = New<NDataNode::TMasterConnector>(Config->DataNode, this);

    ChunkStore = New<NDataNode::TChunkStore>(Config->DataNode, this);

    ChunkCache = New<TChunkCache>(Config->DataNode, this);

    auto createThrottler = [] (TThroughputThrottlerConfigPtr config, const Stroka& name) -> IThroughputThrottlerPtr {
        auto logger = DataNodeLogger;
        logger.AddTag("Throttler: %v", name);

        auto profiler = NProfiling::TProfiler(
            DataNodeProfiler.GetPathPrefix() + "/" +
            CamelCaseToUnderscoreCase(name));

        return CreateLimitedThrottler(config, logger, profiler);
    };
    ReplicationInThrottler = createThrottler(Config->DataNode->ReplicationInThrottler, "ReplicationIn");
    ReplicationOutThrottler = createThrottler(Config->DataNode->ReplicationOutThrottler, "ReplicationOut");
    RepairInThrottler = createThrottler(Config->DataNode->RepairInThrottler, "RepairIn");
    RepairOutThrottler = createThrottler(Config->DataNode->RepairOutThrottler, "RepairOut");

    RpcServer->RegisterService(CreateDataNodeService(Config->DataNode, this));

    JobProxyConfig = New<NJobProxy::TJobProxyConfig>();

    JobProxyConfig->MemoryWatchdogPeriod = Config->ExecAgent->MemoryWatchdogPeriod;
    JobProxyConfig->BlockIOWatchdogPeriod = Config->ExecAgent->BlockIOWatchdogPeriod;

    JobProxyConfig->Logging = Config->ExecAgent->JobProxyLogging;
    JobProxyConfig->Tracing = Config->ExecAgent->JobProxyTracing;

    JobProxyConfig->MemoryLimitMultiplier = Config->ExecAgent->MemoryLimitMultiplier;

    JobProxyConfig->ForceEnableAccounting = Config->ExecAgent->ForceEnableAccounting;
    JobProxyConfig->EnableCGroupMemoryHierarchy = Config->ExecAgent->EnableCGroupMemoryHierarchy;

    JobProxyConfig->IopsThreshold = Config->ExecAgent->IopsThreshold;

    JobProxyConfig->SandboxName = SandboxDirectoryName;
    JobProxyConfig->AddressResolver = Config->AddressResolver;
    JobProxyConfig->SupervisorConnection = New<NBus::TTcpBusClientConfig>();
    JobProxyConfig->SupervisorConnection->Address = LocalDescriptor.GetInterconnectAddress();
    JobProxyConfig->SupervisorRpcTimeout = Config->ExecAgent->SupervisorRpcTimeout;
    // TODO(babenko): consider making this priority configurable
    JobProxyConfig->SupervisorConnection->Priority = 6;
    JobProxyConfig->CellId = GetCellId();

    ExecSlotManager = New<TSlotManager>(Config->ExecAgent->SlotManager, this);

    JobController = New<TJobController>(Config->ExecAgent->JobController, this);

    auto createExecJob = BIND([this] (
            const NJobAgent::TJobId& jobId,
            const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
            NJobTrackerClient::NProto::TJobSpec&& jobSpec) ->
            NJobAgent::IJobPtr
        {
            return NExecAgent::CreateUserJob(
                    jobId,
                    resourceLimits,
                    std::move(jobSpec),
                    this);
        });
    JobController->RegisterFactory(NJobAgent::EJobType::Map,             createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::PartitionMap,    createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SortedMerge,     createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::OrderedMerge,    createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::UnorderedMerge,  createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::Partition,       createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SimpleSort,      createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::PartitionSort,   createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SortedReduce,    createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::PartitionReduce, createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::ReduceCombiner,  createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::RemoteCopy,      createExecJob);

    auto createChunkJob = BIND([this] (
            const NJobAgent::TJobId& jobId,
            const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
            NJobTrackerClient::NProto::TJobSpec&& jobSpec) ->
            NJobAgent::IJobPtr
        {
            return NDataNode::CreateChunkJob(
                    jobId,
                    std::move(jobSpec),
                    resourceLimits,
                    Config->DataNode,
                    this);
        });
    JobController->RegisterFactory(NJobAgent::EJobType::RemoveChunk,     createChunkJob);
    JobController->RegisterFactory(NJobAgent::EJobType::ReplicateChunk,  createChunkJob);
    JobController->RegisterFactory(NJobAgent::EJobType::RepairChunk,     createChunkJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SealChunk,       createChunkJob);

    RpcServer->RegisterService(CreateJobProberService(this));

    RpcServer->RegisterService(New<TSupervisorService>(this));

    EnvironmentManager = New<TEnvironmentManager>(Config->ExecAgent->EnvironmentManager);
    EnvironmentManager->Register("unsafe", CreateUnsafeEnvironmentBuilder());

    SchedulerConnector = New<TSchedulerConnector>(Config->ExecAgent->SchedulerConnector, this);

    TabletSlotManager = New<TTabletSlotManager>(Config->TabletNode, this);

    SecurityManager = New<TSecurityManager>(Config->TabletNode->SecurityManager, this);

    QueryExecutor = CreateQueryExecutor(Config->QueryAgent, this);

    RpcServer->RegisterService(CreateQueryService(Config->QueryAgent, this));

    RpcServer->RegisterService(CreateTimestampProxyService(
        clusterConnection->GetTimestampProvider()));

    RpcServer->RegisterService(CreateMasterCacheService(
        Config->MasterCacheService,
        MasterClient->GetMasterChannel(EMasterChannelKind::Leader),
        GetCellId()));

    OrchidRoot = GetEphemeralNodeFactory()->CreateMap();
    SetNodeByYPath(
        OrchidRoot,
        "/monitoring",
        CreateVirtualNode(monitoringManager->GetService()));
    SetNodeByYPath(
        OrchidRoot,
        "/profiling",
        CreateVirtualNode(TProfileManager::Get()->GetService()));
    SetNodeByYPath(
        OrchidRoot,
        "/config",
        CreateVirtualNode(CreateYsonFileService(ConfigFileName)));
    SetNodeByYPath(
        OrchidRoot,
        "/stored_chunks",
        CreateVirtualNode(CreateStoredChunkMapService(ChunkStore)
            ->Via(GetControlInvoker())));
    SetNodeByYPath(
        OrchidRoot,
        "/cached_chunks",
        CreateVirtualNode(CreateCachedChunkMapService(ChunkCache)
            ->Via(GetControlInvoker())));
    SetNodeByYPath(
        OrchidRoot,
        "/tablet_cells",
        CreateVirtualNode(
            TabletSlotManager->GetOrchidService()
            ->Via(GetControlInvoker())
            ->Cached(Config->OrchidCacheExpirationTime)));
    SetBuildAttributes(OrchidRoot, "node");

    HttpServer->Register(
        "/orchid",
        NMonitoring::GetYPathHttpHandler(OrchidRoot->Via(GetControlInvoker())));

    RpcServer->RegisterService(CreateOrchidService(
        OrchidRoot,
        GetControlInvoker()));

    LOG_INFO("Listening for HTTP requests on port %v", Config->MonitoringPort);

    LOG_INFO("Listening for RPC requests on port %v", Config->RpcPort);
    RpcServer->Configure(Config->RpcServer);

    // Do not start subsystems until everything is initialized.
    TabletSlotManager->Initialize();
    BlockStore->Initialize();
    ChunkStore->Initialize();
    ChunkCache->Initialize();
    JournalDispatcher->Initialize();
    ExecSlotManager->Initialize(Config->ExecAgent->JobController->ResourceLimits->UserSlots);
    monitoringManager->Start();
    PeerBlockUpdater->Start();
    MasterConnector->Start();
    SchedulerConnector->Start();
    StartStoreFlusher(Config->TabletNode, this);
    StartStoreCompactor(Config->TabletNode, this);
    StartPartitionBalancer(Config->TabletNode->PartitionBalancer, this);

    RpcServer->Start();
    HttpServer->Start();
}

TCellNodeConfigPtr TBootstrap::GetConfig() const
{
    return Config;
}

IInvokerPtr TBootstrap::GetControlInvoker() const
{
    return ControlQueue->GetInvoker();
}

IInvokerPtr TBootstrap::GetQueryPoolInvoker() const
{
    return QueryThreadPool->GetInvoker();
}

IInvokerPtr TBootstrap::GetBoundedConcurrencyQueryPoolInvoker() const
{
    return BoundedConcurrencyQueryPoolInvoker;
}

IClientPtr TBootstrap::GetMasterClient() const
{
    return MasterClient;
}

IServerPtr TBootstrap::GetRpcServer() const
{
    return RpcServer;
}

IChannelFactoryPtr TBootstrap::GetTabletChannelFactory() const
{
    return TabletChannelFactory;
}

IMapNodePtr TBootstrap::GetOrchidRoot() const
{
    return OrchidRoot;
}

TJobTrackerPtr TBootstrap::GetJobController() const
{
    return JobController;
}

TTabletSlotManagerPtr TBootstrap::GetTabletSlotManager() const
{
    return TabletSlotManager;
}

TSecurityManagerPtr TBootstrap::GetSecurityManager() const
{
    return SecurityManager;
}

TSlotManagerPtr TBootstrap::GetExecSlotManager() const
{
    return ExecSlotManager;
}

TEnvironmentManagerPtr TBootstrap::GetEnvironmentManager() const
{
    return EnvironmentManager;
}

TJobProxyConfigPtr TBootstrap::GetJobProxyConfig() const
{
    return JobProxyConfig;
}

NDataNode::TChunkStorePtr TBootstrap::GetChunkStore() const
{
    return ChunkStore;
}

TChunkCachePtr TBootstrap::GetChunkCache() const
{
    return ChunkCache;
}

TNodeMemoryTracker* TBootstrap::GetMemoryUsageTracker()
{
    return &MemoryUsageTracker;
}

TChunkRegistryPtr TBootstrap::GetChunkRegistry() const
{
    return ChunkRegistry;
}

TSessionManagerPtr TBootstrap::GetSessionManager() const
{
    return SessionManager;
}

TBlockStorePtr TBootstrap::GetBlockStore() const
{
    return BlockStore;
}

IBlockCachePtr TBootstrap::GetUncompressedBlockCache() const
{
    return UncompressedBlockCache;
}

TPeerBlockTablePtr TBootstrap::GetPeerBlockTable() const
{
    return PeerBlockTable;
}

TBlobReaderCachePtr TBootstrap::GetBlobReaderCache() const
{
    return BlobReaderCache;
}

TJournalDispatcherPtr TBootstrap::GetJournalDispatcher() const
{
    return JournalDispatcher;
}

NDataNode::TMasterConnectorPtr TBootstrap::GetMasterConnector() const
{
    return MasterConnector;
}

NQueryClient::IExecutorPtr TBootstrap::GetQueryExecutor() const
{
    return QueryExecutor;
}

const NNodeTrackerClient::TNodeDescriptor& TBootstrap::GetLocalDescriptor() const
{
    return LocalDescriptor;
}

const TGuid& TBootstrap::GetCellId() const
{
    return Config->ClusterConnection->Master->CellId;
}

IThroughputThrottlerPtr TBootstrap::GetReplicationInThrottler() const
{
    return ReplicationInThrottler;
}

IThroughputThrottlerPtr TBootstrap::GetReplicationOutThrottler() const
{
    return ReplicationOutThrottler;
}

IThroughputThrottlerPtr TBootstrap::GetRepairInThrottler() const
{
    return RepairInThrottler;
}

IThroughputThrottlerPtr TBootstrap::GetRepairOutThrottler() const
{
    return RepairOutThrottler;
}

IThroughputThrottlerPtr TBootstrap::GetInThrottler(EWriteSessionType sessionType) const
{
    switch (sessionType) {
        case EWriteSessionType::User:
            return GetUnlimitedThrottler();

        case EWriteSessionType::Repair:
            return RepairInThrottler;

        case EWriteSessionType::Replication:
            return ReplicationInThrottler;

        default:
            YUNREACHABLE();
    }
}

IThroughputThrottlerPtr TBootstrap::GetOutThrottler(EWriteSessionType sessionType) const
{
    switch (sessionType) {
        case EWriteSessionType::User:
            return GetUnlimitedThrottler();

        case EWriteSessionType::Replication:
            return ReplicationOutThrottler;

        case EWriteSessionType::Repair:
            return RepairOutThrottler;

        default:
            YUNREACHABLE();
    }
}

IThroughputThrottlerPtr TBootstrap::GetOutThrottler(EReadSessionType sessionType) const
{
    switch (sessionType) {
        case EReadSessionType::User:
            return GetUnlimitedThrottler();

        case EReadSessionType::Replication:
            return ReplicationOutThrottler;

        case EReadSessionType::Repair:
            return RepairOutThrottler;

        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellNode
} // namespace NYT
