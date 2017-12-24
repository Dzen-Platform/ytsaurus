#include "bootstrap.h"
#include "config.h"
#include "batching_chunk_service.h"
#include "private.h"

#include <yt/server/data_node/blob_reader_cache.h>
#include <yt/server/data_node/block_cache.h>
#include <yt/server/data_node/chunk_block_manager.h>
#include <yt/server/data_node/chunk_cache.h>
#include <yt/server/data_node/chunk_registry.h>
#include <yt/server/data_node/chunk_store.h>
#include <yt/server/data_node/config.h>
#include <yt/server/data_node/data_node_service.h>
#include <yt/server/data_node/job.h>
#include <yt/server/data_node/journal_dispatcher.h>
#include <yt/server/data_node/location.h>
#include <yt/server/data_node/master_connector.h>
#include <yt/server/data_node/peer_block_table.h>
#include <yt/server/data_node/peer_block_updater.h>
#include <yt/server/data_node/private.h>
#include <yt/server/data_node/session_manager.h>
#include <yt/server/data_node/ytree_integration.h>
#include <yt/server/data_node/chunk_meta_manager.h>
#include <yt/server/data_node/skynet_http_handler.h>

#include <yt/server/exec_agent/config.h>
#include <yt/server/exec_agent/job_environment.h>
#include <yt/server/exec_agent/job.h>
#include <yt/server/exec_agent/job_prober_service.h>
#include <yt/server/exec_agent/private.h>
#include <yt/server/exec_agent/scheduler_connector.h>
#include <yt/server/exec_agent/slot_manager.h>
#include <yt/server/exec_agent/supervisor_service.h>

#include <yt/server/job_agent/job_controller.h>
#include <yt/server/job_agent/statistics_reporter.h>

#include <yt/server/misc/address_helpers.h>
#include <yt/server/misc/build_attributes.h>

#include <yt/server/object_server/master_cache_service.h>

#include <yt/server/query_agent/query_executor.h>
#include <yt/server/query_agent/query_service.h>

#include <yt/server/tablet_node/in_memory_manager.h>
#include <yt/server/tablet_node/partition_balancer.h>
#include <yt/server/tablet_node/security_manager.h>
#include <yt/server/tablet_node/slot_manager.h>
#include <yt/server/tablet_node/store_compactor.h>
#include <yt/server/tablet_node/store_flusher.h>
#include <yt/server/tablet_node/store_trimmer.h>

#include <yt/server/transaction_server/timestamp_proxy_service.h>

#include <yt/server/admin_server/admin_service.h>

#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/native_connection.h>

#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/client_block_cache.h>

#include <yt/ytlib/hydra/peer_channel.h>

#include <yt/ytlib/hive/cell_directory_synchronizer.h>

#include <yt/ytlib/misc/workload.h>
#include <yt/ytlib/misc/memory_usage_tracker.h>

#include <yt/ytlib/monitoring/http_integration.h>
#include <yt/ytlib/monitoring/http_server.h>
#include <yt/ytlib/monitoring/monitoring_manager.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/orchid/orchid_service.h>

#include <yt/ytlib/transaction_client/timestamp_provider.h>

#include <yt/ytlib/query_client/column_evaluator.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>
#include <yt/ytlib/node_tracker_client/node_directory_synchronizer.h>

#include <yt/core/bus/config.h>
#include <yt/core/bus/server.h>
#include <yt/core/bus/tcp_server.h>

#include <yt/core/http/server.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/thread_pool.h>

#include <yt/core/net/address.h>

#include <yt/core/misc/collection_helpers.h>
#include <yt/core/misc/core_dumper.h>
#include <yt/core/misc/ref_counted_tracker.h>
#include <yt/core/misc/lfalloc_helpers.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/rpc/bus_channel.h>
#include <yt/core/rpc/bus_server.h>
#include <yt/core/rpc/caching_channel_factory.h>
#include <yt/core/rpc/channel.h>
#include <yt/core/rpc/redirector_service.h>
#include <yt/core/rpc/server.h>

#include <yt/core/ytree/ephemeral_node_factory.h>
#include <yt/core/ytree/virtual.h>

namespace NYT {
namespace NCellNode {

using namespace NAdmin;
using namespace NBus;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NNodeTrackerClient;
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
using namespace NHiveClient;
using namespace NHiveServer;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

static const i64 FootprintMemorySize = 1_GB;

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(TCellNodeConfigPtr config, INodePtr configNode)
    : TBootstrapBase(CellNodeLogger, config)
    , Config(std::move(config))
    , ConfigNode(std::move(configNode))
{ }

TBootstrap::~TBootstrap() = default;

void TBootstrap::Run()
{
    srand(time(nullptr));

    ControlQueue = New<TActionQueue>("Control");

    BIND(&TBootstrap::DoRun, this)
        .AsyncVia(GetControlInvoker())
        .Run()
        .Get()
        .ThrowOnError();

    Sleep(TDuration::Max());
}

void TBootstrap::DoRun()
{
    auto localRpcAddresses = NYT::GetLocalAddresses(Config->Addresses, Config->RpcPort);
    auto localSkynetHttpAddresses = NYT::GetLocalAddresses(Config->Addresses, Config->SkynetHttpPort);

    if (!Config->ClusterConnection->Networks) {
        Config->ClusterConnection->Networks = GetLocalNetworks();
    }

    LOG_INFO("Starting node (LocalAddresses: %v, PrimaryMasterAddresses: %v, NodeTags: %v)",
        GetValues(localRpcAddresses),
        Config->ClusterConnection->PrimaryMaster->Addresses,
        Config->Tags);

    MemoryUsageTracker = std::make_unique<TNodeMemoryTracker>(
        Config->ResourceLimits->Memory,
        std::vector<std::pair<EMemoryCategory, i64>>{
            {EMemoryCategory::TabletStatic, Config->TabletNode->ResourceLimits->TabletStaticMemory},
            {EMemoryCategory::TabletDynamic, Config->TabletNode->ResourceLimits->TabletDynamicMemory}
        },
        Logger,
        TProfiler("/cell_node/memory_usage"));

    {
        auto result = MemoryUsageTracker->TryAcquire(EMemoryCategory::Footprint, FootprintMemorySize);
        THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error reserving footprint memory");
    }

    MasterConnection = CreateNativeConnection(Config->ClusterConnection);

    if (Config->TabletNode->ResourceLimits->Slots > 0) {
        // Requesting latest timestamp enables periodic background time synchronization.
        // For tablet nodes, it is crucial because of non-atomic transactions that require
        // in-sync time for clients.
        MasterConnection->GetTimestampProvider()->GetLatestTimestamp();
    }

    MasterClient = MasterConnection->CreateNativeClient(TClientOptions(NSecurityClient::RootUserName));

    NodeDirectory = New<TNodeDirectory>();

    NodeDirectorySynchronizer = New<TNodeDirectorySynchronizer>(
        Config->NodeDirectorySynchronizer,
        MasterConnection,
        NodeDirectory);
    NodeDirectorySynchronizer->Start();

    QueryThreadPool = New<TThreadPool>(
        Config->QueryAgent->ThreadPoolSize,
        "Query");

    LookupThreadPool = New<TThreadPool>(
        Config->QueryAgent->LookupThreadPoolSize,
        "Lookup");

    TableReplicatorThreadPool = New<TThreadPool>(
        Config->TabletNode->TabletManager->ReplicatorThreadPoolSize,
        "Replicator");

    TransactionTrackerQueue = New<TActionQueue>("TxTracker");

    BusServer = CreateTcpBusServer(Config->BusServer);

    RpcServer = CreateBusServer(BusServer);

    if (!Config->UseNewHttpServer) {
        HttpServer.reset(new NXHttp::TServer(
            Config->MonitoringPort,
            Config->BusServer->BindRetryCount,
            Config->BusServer->BindRetryBackoff));
    } else {
        Config->MonitoringServer->Port = Config->MonitoringPort;
        Config->MonitoringServer->BindRetryCount = Config->BusServer->BindRetryCount;
        Config->MonitoringServer->BindRetryBackoff = Config->BusServer->BindRetryBackoff;
        NewHttpServer = NHttp::CreateServer(
            Config->MonitoringServer);
    }

    auto skynetHttpConfig = New<NHttp::TServerConfig>();
    skynetHttpConfig->Port = Config->SkynetHttpPort;
    skynetHttpConfig->BindRetryCount = Config->BusServer->BindRetryCount;
    skynetHttpConfig->BindRetryBackoff = Config->BusServer->BindRetryBackoff;
    SkynetHttpServer = NHttp::CreateServer(skynetHttpConfig);

    MonitoringManager_ = New<TMonitoringManager>();
    MonitoringManager_->Register(
        "/ref_counted",
        TRefCountedTracker::Get()->GetMonitoringProducer());

    LFAllocProfiler_ = std::make_unique<NLFAlloc::TLFAllocProfiler>();

    auto createBatchingChunkService = [&] (TMasterConnectionConfigPtr config) {
        RpcServer->RegisterService(CreateBatchingChunkService(
            config->CellId,
            Config->BatchingChunkService,
            config,
            MasterConnection->GetChannelFactory()));
    };

    createBatchingChunkService(Config->ClusterConnection->PrimaryMaster);
    for (const auto& config : Config->ClusterConnection->SecondaryMasters) {
        createBatchingChunkService(config);
    }

    BlobReaderCache = New<TBlobReaderCache>(Config->DataNode);

    JournalDispatcher = New<TJournalDispatcher>(Config->DataNode);

    ChunkRegistry = New<TChunkRegistry>(this);

    ChunkMetaManager = New<TChunkMetaManager>(Config->DataNode, this);

    ChunkBlockManager = New<TChunkBlockManager>(Config->DataNode, this);

    BlockCache = CreateServerBlockCache(Config->DataNode, this);

    PeerBlockTable = New<TPeerBlockTable>(Config->DataNode->PeerBlockTable);

    PeerBlockUpdater = New<TPeerBlockUpdater>(Config->DataNode, this);

    SessionManager = New<TSessionManager>(Config->DataNode, this);

    MasterConnector = New<NDataNode::TMasterConnector>(
        Config->DataNode,
        localRpcAddresses,
        localSkynetHttpAddresses,
        Config->Tags,
        this);
    MasterConnector->SubscribePopulateAlerts(BIND(&TBootstrap::PopulateAlerts, this));
    MasterConnector->SubscribeMasterConnected(BIND(&TBootstrap::OnMasterConnected, this));
    MasterConnector->SubscribeMasterDisconnected(BIND(&TBootstrap::OnMasterDisconnected, this));

    if (Config->CoreDumper) {
        CoreDumper = New<TCoreDumper>(Config->CoreDumper);
    }

    ChunkStore = New<NDataNode::TChunkStore>(Config->DataNode, this);

    ChunkCache = New<TChunkCache>(Config->DataNode, this);

    auto createThrottler = [] (TThroughputThrottlerConfigPtr config, const TString& name) {
        auto logger = DataNodeLogger;
        logger.AddTag("Throttler: %v", name);

        auto profiler = NProfiling::TProfiler(
            DataNodeProfiler.GetPathPrefix() + "/" +
            CamelCaseToUnderscoreCase(name));

        return CreateReconfigurableThroughputThrottler(config, logger, profiler);
    };

    TotalInThrottler = createThrottler(Config->DataNode->TotalInThrottler, "TotalIn");
    TotalOutThrottler = createThrottler(Config->DataNode->TotalOutThrottler, "TotalOut");

    ReplicationInThrottler = CreateCombinedThrottler(std::vector<IThroughputThrottlerPtr>{
        TotalInThrottler,
        createThrottler(Config->DataNode->ReplicationInThrottler, "ReplicationIn")
    });
    ReplicationOutThrottler = CreateCombinedThrottler(std::vector<IThroughputThrottlerPtr>{
        TotalOutThrottler,
        createThrottler(Config->DataNode->ReplicationOutThrottler, "ReplicationOut")
    });

    RepairInThrottler = CreateCombinedThrottler(std::vector<IThroughputThrottlerPtr>{
        TotalInThrottler,
        createThrottler(Config->DataNode->RepairInThrottler, "RepairIn")
    });
    RepairOutThrottler = CreateCombinedThrottler(std::vector<IThroughputThrottlerPtr>{
        TotalOutThrottler,
        createThrottler(Config->DataNode->RepairOutThrottler, "RepairOut")
    });

    ArtifactCacheInThrottler = CreateCombinedThrottler(std::vector<IThroughputThrottlerPtr>{
        TotalInThrottler,
        createThrottler(Config->DataNode->ArtifactCacheInThrottler, "ArtifactCacheIn")
    });
    ArtifactCacheOutThrottler = CreateCombinedThrottler(std::vector<IThroughputThrottlerPtr>{
        TotalOutThrottler,
        createThrottler(Config->DataNode->ArtifactCacheOutThrottler, "ArtifactCacheOut")
    });
    SkynetOutThrottler = CreateCombinedThrottler(std::vector<IThroughputThrottlerPtr>{
        TotalOutThrottler,
        createThrottler(Config->DataNode->SkynetOutThrottler, "SkynetOut")
    });

    RpcServer->RegisterService(CreateDataNodeService(Config->DataNode, this));

    auto localAddress = GetDefaultAddress(localRpcAddresses);

    JobProxyConfigTemplate = New<NJobProxy::TJobProxyConfig>();

    // Singletons.
    JobProxyConfigTemplate->FiberStackPoolSizes = Config->FiberStackPoolSizes;
    JobProxyConfigTemplate->AddressResolver = Config->AddressResolver;
    JobProxyConfigTemplate->RpcDispatcher = Config->RpcDispatcher;
    JobProxyConfigTemplate->ChunkClientDispatcher = Config->ChunkClientDispatcher;

    JobProxyConfigTemplate->ClusterConnection = CloneYsonSerializable(Config->ClusterConnection);

    auto patchMasterConnectionConfig = [&] (const TMasterConnectionConfigPtr& config) {
        config->Addresses = {localAddress};
        if (config->RetryTimeout && *config->RetryTimeout > config->RpcTimeout) {
            config->RpcTimeout = *config->RetryTimeout;
        }
        config->RetryTimeout = Null;
        config->RetryAttempts = 1;
    };

    patchMasterConnectionConfig(JobProxyConfigTemplate->ClusterConnection->PrimaryMaster);
    for (const auto& config : JobProxyConfigTemplate->ClusterConnection->SecondaryMasters) {
        patchMasterConnectionConfig(config);
    }

    JobProxyConfigTemplate->SupervisorConnection = New<NBus::TTcpBusClientConfig>();

    JobProxyConfigTemplate->SupervisorConnection->Address = localAddress;

    JobProxyConfigTemplate->SupervisorRpcTimeout = Config->ExecAgent->SupervisorRpcTimeout;

    JobProxyConfigTemplate->HeartbeatPeriod = Config->ExecAgent->JobProxyHeartbeatPeriod;

    JobProxyConfigTemplate->JobEnvironment = Config->ExecAgent->SlotManager->JobEnvironment;

    JobProxyConfigTemplate->Logging = Config->ExecAgent->JobProxyLogging;
    JobProxyConfigTemplate->Tracing = Config->ExecAgent->JobProxyTracing;
    JobProxyConfigTemplate->TestRootFS = Config->ExecAgent->TestRootFS;

    JobProxyConfigTemplate->CoreForwarderTimeout = Config->ExecAgent->CoreForwarderTimeout;

    ExecSlotManager = New<NExecAgent::TSlotManager>(Config->ExecAgent->SlotManager, this);

    JobController = New<TJobController>(Config->ExecAgent->JobController, this);

    auto createExecJob = BIND([this] (
            const NJobAgent::TJobId& jobId,
            const NJobAgent::TOperationId& operationId,
            const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
            NJobTrackerClient::NProto::TJobSpec&& jobSpec) ->
            NJobAgent::IJobPtr
        {
            return NExecAgent::CreateUserJob(
                jobId,
                operationId,
                resourceLimits,
                std::move(jobSpec),
                this);
        });
    JobController->RegisterFactory(NJobAgent::EJobType::Map,               createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::PartitionMap,      createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SortedMerge,       createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::OrderedMerge,      createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::UnorderedMerge,    createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::Partition,         createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SimpleSort,        createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::IntermediateSort,  createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::FinalSort,         createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SortedReduce,      createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::PartitionReduce,   createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::ReduceCombiner,    createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::RemoteCopy,        createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::OrderedMap,        createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::JoinReduce,        createExecJob);

    auto createChunkJob = BIND([this] (
            const NJobAgent::TJobId& jobId,
            const NJobAgent::TOperationId& /*operationId*/,
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

    StatisticsReporter = New<TStatisticsReporter>(
        Config->ExecAgent->StatisticsReporter,
        this);

    RpcServer->RegisterService(CreateJobProberService(this));

    RpcServer->RegisterService(New<TSupervisorService>(this));

    SchedulerConnector = New<TSchedulerConnector>(Config->ExecAgent->SchedulerConnector, this);

    ColumnEvaluatorCache = New<NQueryClient::TColumnEvaluatorCache>(
        New<NQueryClient::TColumnEvaluatorCacheConfig>());

    TabletSlotManager = New<NTabletNode::TSlotManager>(Config->TabletNode, this);
    MasterConnector->SubscribePopulateAlerts(BIND(&NTabletNode::TSlotManager::PopulateAlerts, TabletSlotManager));

    SecurityManager = New<TSecurityManager>(Config->TabletNode->SecurityManager, this);

    InMemoryManager = New<TInMemoryManager>(Config->TabletNode->InMemoryManager, this);

    QueryExecutor = CreateQuerySubexecutor(Config->QueryAgent, this);

    RpcServer->RegisterService(CreateQueryService(Config->QueryAgent, this));

    RpcServer->RegisterService(CreateTimestampProxyService(
        MasterConnection->GetTimestampProvider()));

    MasterCacheService = CreateMasterCacheService(
        Config->MasterCacheService,
        CreateDefaultTimeoutChannel(
            CreatePeerChannel(
                Config->ClusterConnection->PrimaryMaster,
                MasterConnection->GetChannelFactory(),
                EPeerKind::Follower),
            Config->ClusterConnection->PrimaryMaster->RpcTimeout),
        GetCellId());

    OrchidRoot = GetEphemeralNodeFactory(true)->CreateMap();

    SetNodeByYPath(
        OrchidRoot,
        "/monitoring",
        CreateVirtualNode(MonitoringManager_->GetService()));
    SetNodeByYPath(
        OrchidRoot,
        "/profiling",
        CreateVirtualNode(TProfileManager::Get()->GetService()));
    SetNodeByYPath(
        OrchidRoot,
        "/config",
        ConfigNode);
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
        CreateVirtualNode(TabletSlotManager->GetOrchidService()));
    SetNodeByYPath(
        OrchidRoot,
        "/chunk_blocks",
        CreateVirtualNode(ChunkBlockManager->GetOrchidService()));
    SetNodeByYPath(
        OrchidRoot,
        "/job_controller",
        CreateVirtualNode(JobController->GetOrchidService()
            ->Via(GetControlInvoker())));
    SetBuildAttributes(OrchidRoot, "node");

    if (HttpServer) {
        HttpServer->Register(
            "/orchid",
            NMonitoring::GetYPathHttpHandler(OrchidRoot->Via(GetControlInvoker())));
    } else {
        NewHttpServer->AddHandler(
            "/orchid/",
            NMonitoring::GetOrchidYPathHttpHandler(OrchidRoot->Via(GetControlInvoker())));
    }

    if (Config->DataNode->EnableExperimentalSkynetHttpApi) {
        SkynetHttpServer->AddHandler(
            "/read_skynet_part",
            MakeSkynetHttpHandler(this));
    }

    RpcServer->RegisterService(CreateOrchidService(
        OrchidRoot,
        GetControlInvoker()));

    RpcServer->RegisterService(CreateAdminService(GetControlInvoker(), CoreDumper));

    LOG_INFO("Listening for HTTP requests on port %v", Config->MonitoringPort);

    LOG_INFO("Listening for RPC requests on port %v", Config->RpcPort);
    RpcServer->Configure(Config->RpcServer);

    // Do not start subsystems until everything is initialized.
    TabletSlotManager->Initialize();
    ChunkStore->Initialize();
    ChunkCache->Initialize();
    ExecSlotManager->Initialize();
    JobController->Initialize();
    MonitoringManager_->Start();
    PeerBlockUpdater->Start();
    MasterConnector->Start();
    SchedulerConnector->Start();
    StartStoreFlusher(Config->TabletNode, this);
    StartStoreCompactor(Config->TabletNode, this);
    StartStoreTrimmer(Config->TabletNode, this);
    StartPartitionBalancer(Config->TabletNode, this);

    RpcServer->Start();
    if (HttpServer) {
        HttpServer->Start();
    } else {
        NewHttpServer->Start();
    }
    SkynetHttpServer->Start();
}

const TCellNodeConfigPtr& TBootstrap::GetConfig() const
{
    return Config;
}

const IInvokerPtr& TBootstrap::GetControlInvoker() const
{
    return ControlQueue->GetInvoker();
}

const IInvokerPtr& TBootstrap::GetQueryPoolInvoker() const
{
    return QueryThreadPool->GetInvoker();
}

const IInvokerPtr& TBootstrap::GetLookupPoolInvoker() const
{
    return LookupThreadPool->GetInvoker();
}

const IInvokerPtr& TBootstrap::GetTableReplicatorPoolInvoker() const
{
    return TableReplicatorThreadPool->GetInvoker();
}

const IInvokerPtr& TBootstrap::GetTransactionTrackerInvoker() const
{
    return TransactionTrackerQueue->GetInvoker();
}

const INativeClientPtr& TBootstrap::GetMasterClient() const
{
    return MasterClient;
}

const INativeConnectionPtr& TBootstrap::GetMasterConnection() const
{
    return MasterConnection;
}

const IServerPtr& TBootstrap::GetRpcServer() const
{
    return RpcServer;
}

const IMapNodePtr& TBootstrap::GetOrchidRoot() const
{
    return OrchidRoot;
}

const TJobControllerPtr& TBootstrap::GetJobController() const
{
    return JobController;
}

const TStatisticsReporterPtr& TBootstrap::GetStatisticsReporter() const
{
    return StatisticsReporter;
}

const NTabletNode::TSlotManagerPtr& TBootstrap::GetTabletSlotManager() const
{
    return TabletSlotManager;
}

const TSecurityManagerPtr& TBootstrap::GetSecurityManager() const
{
    return SecurityManager;
}

const TInMemoryManagerPtr& TBootstrap::GetInMemoryManager() const
{
    return InMemoryManager;
}

const NExecAgent::TSlotManagerPtr& TBootstrap::GetExecSlotManager() const
{
    return ExecSlotManager;
}

const TChunkStorePtr& TBootstrap::GetChunkStore() const
{
    return ChunkStore;
}

const TChunkCachePtr& TBootstrap::GetChunkCache() const
{
    return ChunkCache;
}

TNodeMemoryTracker* TBootstrap::GetMemoryUsageTracker() const
{
    return MemoryUsageTracker.get();
}

const TChunkRegistryPtr& TBootstrap::GetChunkRegistry() const
{
    return ChunkRegistry;
}

const TSessionManagerPtr& TBootstrap::GetSessionManager() const
{
    return SessionManager;
}

const TChunkBlockManagerPtr& TBootstrap::GetChunkBlockManager() const
{
    return ChunkBlockManager;
}

const TChunkMetaManagerPtr& TBootstrap::GetChunkMetaManager() const
{
    return ChunkMetaManager;
}

const IBlockCachePtr& TBootstrap::GetBlockCache() const
{
    return BlockCache;
}

const TPeerBlockTablePtr& TBootstrap::GetPeerBlockTable() const
{
    return PeerBlockTable;
}

const TBlobReaderCachePtr& TBootstrap::GetBlobReaderCache() const
{
    return BlobReaderCache;
}

const TJournalDispatcherPtr& TBootstrap::GetJournalDispatcher() const
{
    return JournalDispatcher;
}

const TMasterConnectorPtr& TBootstrap::GetMasterConnector() const
{
    return MasterConnector;
}

const TNodeDirectoryPtr& TBootstrap::GetNodeDirectory() const
{
    return NodeDirectory;
}

const NQueryClient::ISubexecutorPtr& TBootstrap::GetQueryExecutor() const
{
    return QueryExecutor;
}

const TCellId& TBootstrap::GetCellId() const
{
    return Config->ClusterConnection->PrimaryMaster->CellId;
}

TCellId TBootstrap::GetCellId(TCellTag cellTag) const
{
    return cellTag == PrimaryMasterCellTag
        ? GetCellId()
        : ReplaceCellTagInId(GetCellId(), cellTag);
}

const NQueryClient::TColumnEvaluatorCachePtr& TBootstrap::GetColumnEvaluatorCache() const
{
    return ColumnEvaluatorCache;
}

const IThroughputThrottlerPtr& TBootstrap::GetReplicationInThrottler() const
{
    return ReplicationInThrottler;
}

const IThroughputThrottlerPtr& TBootstrap::GetReplicationOutThrottler() const
{
    return ReplicationOutThrottler;
}

const IThroughputThrottlerPtr& TBootstrap::GetRepairInThrottler() const
{
    return RepairInThrottler;
}

const IThroughputThrottlerPtr& TBootstrap::GetRepairOutThrottler() const
{
    return RepairOutThrottler;
}

const IThroughputThrottlerPtr& TBootstrap::GetArtifactCacheInThrottler() const
{
    return ArtifactCacheInThrottler;
}

const IThroughputThrottlerPtr& TBootstrap::GetArtifactCacheOutThrottler() const
{
    return ArtifactCacheOutThrottler;
}

const IThroughputThrottlerPtr& TBootstrap::GetSkynetOutThrottler() const
{
    return SkynetOutThrottler;
}

const IThroughputThrottlerPtr& TBootstrap::GetInThrottler(const TWorkloadDescriptor& descriptor) const
{
    switch (descriptor.Category) {
        case EWorkloadCategory::SystemRepair:
            return RepairInThrottler;

        case EWorkloadCategory::SystemReplication:
            return ReplicationInThrottler;

        case EWorkloadCategory::SystemArtifactCacheDownload:
            return ArtifactCacheInThrottler;

        default:
            return TotalInThrottler;
    }
}

const IThroughputThrottlerPtr& TBootstrap::GetOutThrottler(const TWorkloadDescriptor& descriptor) const
{
    switch (descriptor.Category) {
        case EWorkloadCategory::SystemRepair:
            return RepairOutThrottler;

        case EWorkloadCategory::SystemReplication:
            return ReplicationOutThrottler;

        case EWorkloadCategory::SystemArtifactCacheDownload:
            return ArtifactCacheOutThrottler;

        default:
            return TotalOutThrottler;
    }
}

TNetworkPreferenceList TBootstrap::GetLocalNetworks()
{
    return Config->Addresses.empty()
        ? DefaultNetworkPreferences
        : GetIths<0>(Config->Addresses);
}

TJobProxyConfigPtr TBootstrap::BuildJobProxyConfig() const
{
    auto proxyConfig = CloneYsonSerializable(JobProxyConfigTemplate);
    auto localDescriptor = GetMasterConnector()->GetLocalDescriptor();
    proxyConfig->DataCenter = localDescriptor.GetDataCenter();
    proxyConfig->Rack = localDescriptor.GetRack();
    proxyConfig->Addresses = localDescriptor.Addresses();
    return proxyConfig;
}

void TBootstrap::PopulateAlerts(std::vector<TError>* alerts)
{
    // NB: Don't expect IsXXXExceeded helpers to be atomic.
    auto totalUsed = MemoryUsageTracker->GetTotalUsed();
    auto totalLimit = MemoryUsageTracker->GetTotalLimit();
    if (totalUsed > totalLimit) {
        alerts->push_back(TError("Total memory limit exceeded")
            << TErrorAttribute("used", totalUsed)
            << TErrorAttribute("limit", totalLimit));
    }

    for (auto category : TEnumTraits<EMemoryCategory>::GetDomainValues()) {
        auto used = MemoryUsageTracker->GetUsed(category);
        auto limit = MemoryUsageTracker->GetLimit(category);
        if (used > limit) {
            alerts->push_back(TError("Memory limit exceeded for category %Qlv",
                category)
                << TErrorAttribute("used", used)
                << TErrorAttribute("limit", limit));
        }
    }
}

void TBootstrap::OnMasterConnected()
{
    RpcServer->RegisterService(MasterCacheService);
}

void TBootstrap::OnMasterDisconnected()
{
    RpcServer->UnregisterService(MasterCacheService);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellNode
} // namespace NYT
