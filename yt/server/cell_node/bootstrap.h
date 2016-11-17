#pragma once

#include "public.h"

#include <yt/server/chunk_server/public.h>

#include <yt/server/exec_agent/public.h>

#include <yt/server/data_node/public.h>

#include <yt/server/job_agent/public.h>

#include <yt/server/job_proxy/public.h>

#include <yt/server/tablet_node/public.h>

#include <yt/server/hive/public.h>

#include <yt/ytlib/api/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/misc/public.h>

#include <yt/ytlib/monitoring/http_server.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/query_client/public.h>

#include <yt/core/bus/public.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/throughput_throttler.h>

#include <yt/core/rpc/public.h>

#include <yt/core/ytree/public.h>

#include <yt/core/misc/public.h>
#include <yt/ytlib/hive/config.h>

namespace NYT {
namespace NCellNode {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
{
public:
    explicit TBootstrap(NYTree::INodePtr configNode);
    ~TBootstrap();

    const TCellNodeConfigPtr& GetConfig() const;
    const IInvokerPtr& GetControlInvoker() const;
    const IInvokerPtr& GetQueryPoolInvoker() const;
    const IInvokerPtr& GetTableReplicatorPoolInvoker() const;
    const NApi::INativeClientPtr& GetMasterClient() const;
    const NRpc::IServerPtr& GetRpcServer() const;
    const NRpc::IChannelFactoryPtr& GetTabletChannelFactory() const;
    const NYTree::IMapNodePtr& GetOrchidRoot() const;
    const NJobAgent::TJobControllerPtr& GetJobController() const;
    const NJobAgent::TStatisticsReporterPtr& GetStatisticsReporter() const;
    const NTabletNode::TSlotManagerPtr& GetTabletSlotManager() const;
    const NTabletNode::TSecurityManagerPtr& GetSecurityManager() const;
    const NTabletNode::TInMemoryManagerPtr& GetInMemoryManager() const;
    const NExecAgent::TSlotManagerPtr& GetExecSlotManager() const;
    const NJobProxy::TJobProxyConfigPtr& GetJobProxyConfig() const;
    TNodeMemoryTracker* GetMemoryUsageTracker() const;
    const NDataNode::TChunkStorePtr& GetChunkStore() const;
    const NDataNode::TChunkCachePtr& GetChunkCache() const;
    const NDataNode::TChunkRegistryPtr& GetChunkRegistry() const;
    const NDataNode::TSessionManagerPtr& GetSessionManager() const;
    const NDataNode::TChunkMetaManagerPtr& GetChunkMetaManager() const;
    const NDataNode::TChunkBlockManagerPtr& GetChunkBlockManager() const;
    const NChunkClient::IBlockCachePtr& GetBlockCache() const;
    const NDataNode::TPeerBlockTablePtr& GetPeerBlockTable() const;
    const NDataNode::TBlobReaderCachePtr& GetBlobReaderCache() const;
    const NDataNode::TJournalDispatcherPtr& GetJournalDispatcher() const;
    const NDataNode::TMasterConnectorPtr& GetMasterConnector() const;
    const NHiveClient::TClusterDirectoryPtr& GetClusterDirectory();
    const NQueryClient::TColumnEvaluatorCachePtr& GetColumnEvaluatorCache() const;
    const NQueryClient::ISubexecutorPtr& GetQueryExecutor() const;
    const NNodeTrackerClient::TNodeDirectoryPtr& GetNodeDirectory() const;

    const NConcurrency::IThroughputThrottlerPtr& GetReplicationInThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetReplicationOutThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetRepairInThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetRepairOutThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetArtifactCacheInThrottler() const;
    const NConcurrency::IThroughputThrottlerPtr& GetArtifactCacheOutThrottler() const;

    const NConcurrency::IThroughputThrottlerPtr& GetInThrottler(const TWorkloadDescriptor& descriptor) const;
    const NConcurrency::IThroughputThrottlerPtr& GetOutThrottler(const TWorkloadDescriptor& descriptor) const;

    const NObjectClient::TCellId& GetCellId() const;
    NObjectClient::TCellId GetCellId(NObjectClient::TCellTag cellTag) const;
    NNodeTrackerClient::TAddressMap GetLocalAddresses();
    NNodeTrackerClient::TNetworkPreferenceList GetLocalNetworks();

    void Run();

private:
    const NYTree::INodePtr ConfigNode;

    TCellNodeConfigPtr Config;

    NConcurrency::TActionQueuePtr ControlQueue;

    NConcurrency::TThreadPoolPtr QueryThreadPool;

    NConcurrency::TThreadPoolPtr TableReplicatorThreadPool;

    NMonitoring::TMonitoringManagerPtr MonitoringManager_;
    std::unique_ptr<NLFAlloc::TLFAllocProfiler> LFAllocProfiler_;
    NBus::IBusServerPtr BusServer;
    NApi::INativeConnectionPtr MasterConnection;
    NApi::INativeClientPtr MasterClient;
    NHiveServer::TCellDirectorySynchronizerPtr CellDirectorySynchronizer;
    NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory;
    NNodeTrackerClient::TNodeDirectorySynchronizerPtr NodeDirectorySynchronizer;
    NRpc::IServerPtr RpcServer;
    NRpc::IServicePtr MasterCacheService;
    std::unique_ptr<NHttp::TServer> HttpServer;
    NRpc::IChannelFactoryPtr TabletChannelFactory;
    NYTree::IMapNodePtr OrchidRoot;
    NJobAgent::TJobControllerPtr JobController;
    NJobAgent::TStatisticsReporterPtr StatisticsReporter;
    NExecAgent::TSlotManagerPtr ExecSlotManager;
    NJobProxy::TJobProxyConfigPtr JobProxyConfig;
    std::unique_ptr<TMemoryUsageTracker<NNodeTrackerClient::EMemoryCategory>> MemoryUsageTracker;
    NExecAgent::TSchedulerConnectorPtr SchedulerConnector;
    NDataNode::TChunkStorePtr ChunkStore;
    NDataNode::TChunkCachePtr ChunkCache;
    NDataNode::TChunkRegistryPtr ChunkRegistry;
    NDataNode::TSessionManagerPtr SessionManager;
    NDataNode::TChunkMetaManagerPtr ChunkMetaManager;
    NDataNode::TChunkBlockManagerPtr ChunkBlockManager;
    NChunkClient::IBlockCachePtr BlockCache;
    NDataNode::TPeerBlockTablePtr PeerBlockTable;
    NDataNode::TPeerBlockUpdaterPtr PeerBlockUpdater;
    NDataNode::TBlobReaderCachePtr BlobReaderCache;
    NDataNode::TJournalDispatcherPtr JournalDispatcher;
    NDataNode::TMasterConnectorPtr MasterConnector;
    NHiveClient::TClusterDirectoryPtr ClusterDirectory;
    NHiveClient::TClusterDirectorySynchronizerPtr ClusterDirectorySynchronizer;

    NConcurrency::IThroughputThrottlerPtr TotalInThrottler;
    NConcurrency::IThroughputThrottlerPtr TotalOutThrottler;
    NConcurrency::IThroughputThrottlerPtr ReplicationInThrottler;
    NConcurrency::IThroughputThrottlerPtr ReplicationOutThrottler;
    NConcurrency::IThroughputThrottlerPtr RepairInThrottler;
    NConcurrency::IThroughputThrottlerPtr RepairOutThrottler;
    NConcurrency::IThroughputThrottlerPtr ArtifactCacheInThrottler;
    NConcurrency::IThroughputThrottlerPtr ArtifactCacheOutThrottler;

    NTabletNode::TSlotManagerPtr TabletSlotManager;
    NTabletNode::TSecurityManagerPtr SecurityManager;
    NTabletNode::TInMemoryManagerPtr InMemoryManager;

    NQueryClient::TColumnEvaluatorCachePtr ColumnEvaluatorCache;
    NQueryClient::ISubexecutorPtr QueryExecutor;


    void DoRun();
    void PopulateAlerts(std::vector<TError>* alerts);
    NObjectClient::TCellId ToRedirectorCellId(const NObjectClient::TCellId& cellId);

    void OnMasterConnected();
    void OnMasterDisconnected();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
