#pragma once

#include "public.h"

#include <core/concurrency/throughput_throttler.h>
#include <core/concurrency/action_queue.h>

#include <core/bus/public.h>

#include <core/rpc/public.h>

#include <core/ytree/public.h>

#include <ytlib/monitoring/http_server.h>

#include <ytlib/chunk_client/public.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/api/public.h>

#include <ytlib/query_client/public.h>

#include <server/data_node/public.h>

#include <server/chunk_server/public.h>

#include <server/job_agent/public.h>

#include <server/exec_agent/public.h>

#include <server/job_proxy/public.h>

#include <server/tablet_node/public.h>

namespace NYT {
namespace NCellNode {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
{
public:
    TBootstrap(
        const Stroka& configFileName,
        TCellNodeConfigPtr config);
    ~TBootstrap();

    TCellNodeConfigPtr GetConfig() const;
    IInvokerPtr GetControlInvoker() const;
    IInvokerPtr GetQueryPoolInvoker() const;
    IInvokerPtr GetBoundedConcurrencyQueryPoolInvoker() const;
    IInvokerPtr GetBoundedConcurrencyReadPoolInvoker() const;
    NApi::IClientPtr GetMasterClient() const;
    NRpc::IServerPtr GetRpcServer() const;
    NRpc::IChannelFactoryPtr GetTabletChannelFactory() const;
    NYTree::IMapNodePtr GetOrchidRoot() const;
    NJobAgent::TJobTrackerPtr GetJobController() const;
    NTabletNode::TSlotManagerPtr GetTabletSlotManager() const;
    NTabletNode::TSecurityManagerPtr GetSecurityManager() const;
    NExecAgent::TSlotManagerPtr GetExecSlotManager() const;
    NExecAgent::TEnvironmentManagerPtr GetEnvironmentManager() const;
    NJobProxy::TJobProxyConfigPtr GetJobProxyConfig() const;
    TNodeMemoryTracker* GetMemoryUsageTracker();
    NDataNode::TChunkStorePtr GetChunkStore() const;
    NDataNode::TChunkCachePtr GetChunkCache() const;
    NDataNode::TChunkRegistryPtr GetChunkRegistry() const;
    NDataNode::TSessionManagerPtr GetSessionManager() const;
    NDataNode::TBlockStorePtr GetBlockStore() const;
    NChunkClient::IBlockCachePtr GetBlockCache() const;
    NDataNode::TPeerBlockTablePtr GetPeerBlockTable() const;
    NDataNode::TBlobReaderCachePtr GetBlobReaderCache() const;
    NDataNode::TJournalDispatcherPtr GetJournalDispatcher() const;
    NDataNode::TMasterConnectorPtr GetMasterConnector() const;
    NQueryClient::IExecutorPtr GetQueryExecutor() const;

    NConcurrency::IThroughputThrottlerPtr GetReplicationInThrottler() const;
    NConcurrency::IThroughputThrottlerPtr GetReplicationOutThrottler() const;
    NConcurrency::IThroughputThrottlerPtr GetRepairInThrottler() const;
    NConcurrency::IThroughputThrottlerPtr GetRepairOutThrottler() const;

    NConcurrency::IThroughputThrottlerPtr GetInThrottler(NChunkClient::EWriteSessionType sessionType) const;
    NConcurrency::IThroughputThrottlerPtr GetOutThrottler(NChunkClient::EWriteSessionType sessionType) const;
    NConcurrency::IThroughputThrottlerPtr GetOutThrottler(NChunkClient::EReadSessionType sessionType) const;

    const NNodeTrackerClient::TAddressMap& GetLocalAddresses() const;
    NNodeTrackerClient::TNodeDescriptor GetLocalDescriptor() const;

    const TGuid& GetCellId() const;

    void Run();

private:
    Stroka ConfigFileName;
    TCellNodeConfigPtr Config;

    NConcurrency::TActionQueuePtr ControlQueue;

    NConcurrency::TThreadPoolPtr QueryThreadPool;
    IInvokerPtr BoundedConcurrencyQueryPoolInvoker;
    IInvokerPtr BoundedConcurrencyReadPoolInvoker;

    NBus::IBusServerPtr BusServer;
    NApi::IClientPtr MasterClient;
    NRpc::IServerPtr RpcServer;
    std::unique_ptr<NHttp::TServer> HttpServer;
    NRpc::IChannelFactoryPtr TabletChannelFactory;
    NYTree::IMapNodePtr OrchidRoot;
    NJobAgent::TJobTrackerPtr JobController;
    NExecAgent::TSlotManagerPtr ExecSlotManager;
    NExecAgent::TEnvironmentManagerPtr EnvironmentManager;
    NJobProxy::TJobProxyConfigPtr JobProxyConfig;
    TMemoryUsageTracker<EMemoryConsumer> MemoryUsageTracker;
    NExecAgent::TSchedulerConnectorPtr SchedulerConnector;
    NDataNode::TChunkStorePtr ChunkStore;
    NDataNode::TChunkCachePtr ChunkCache;
    NDataNode::TChunkRegistryPtr ChunkRegistry;
    NDataNode::TSessionManagerPtr SessionManager;
    NDataNode::TBlockStorePtr BlockStore;
    NChunkClient::IBlockCachePtr BlockCache;
    NDataNode::TPeerBlockTablePtr PeerBlockTable;
    NDataNode::TPeerBlockUpdaterPtr PeerBlockUpdater;
    NDataNode::TBlobReaderCachePtr BlobReaderCache;
    NDataNode::TJournalDispatcherPtr JournalDispatcher;
    NDataNode::TMasterConnectorPtr MasterConnector;

    NConcurrency::IThroughputThrottlerPtr ReplicationInThrottler;
    NConcurrency::IThroughputThrottlerPtr ReplicationOutThrottler;
    NConcurrency::IThroughputThrottlerPtr RepairInThrottler;
    NConcurrency::IThroughputThrottlerPtr RepairOutThrottler;

    NTabletNode::TSlotManagerPtr TabletSlotManager;
    NTabletNode::TSecurityManagerPtr SecurityManager;

    NQueryClient::IExecutorPtr QueryExecutor;

    NNodeTrackerClient::TAddressMap LocalAddresses;


    void DoRun();
    void InitNodeAddresses();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellNode
} // namespace NYT
