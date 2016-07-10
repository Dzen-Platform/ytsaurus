#pragma once

#include "public.h"

#include <yt/server/scheduler/public.h>

#include <yt/ytlib/api/public.h>

#include <yt/ytlib/hive/public.h>

#include <yt/ytlib/monitoring/http_server.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/bus/public.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/misc/public.h>


namespace NYT {
namespace NCellScheduler {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EControlQueue,
    (Default)
    (Heartbeat)
);

class TBootstrap
{
public:
    explicit TBootstrap(const NYTree::INodePtr configNode);
    ~TBootstrap();

    TCellSchedulerConfigPtr GetConfig() const;
    NApi::INativeClientPtr GetMasterClient() const;
    NNodeTrackerClient::TAddressMap GetLocalAddresses() const;
    NNodeTrackerClient::TNetworkPreferenceList GetLocalNetworks() const;
    IInvokerPtr GetControlInvoker(EControlQueue queue = EControlQueue::Default) const;
    NScheduler::TSchedulerPtr GetScheduler() const;
    NHiveClient::TClusterDirectoryPtr GetClusterDirectory() const;
    NRpc::TResponseKeeperPtr GetResponseKeeper() const;
    NChunkClient::TThrottlerManagerPtr GetChunkLocationThrottlerManager() const;

    void Run();

private:
    const NYTree::INodePtr ConfigNode_;

    TCellSchedulerConfigPtr Config_;
    NMonitoring::TMonitoringManagerPtr MonitoringManager_;
    std::unique_ptr<NLFAlloc::TLFAllocProfiler> LFAllocProfiler_;
    NConcurrency::TFairShareActionQueuePtr ControlQueue_;
    NBus::IBusServerPtr BusServer_;
    NRpc::IServerPtr RpcServer_;
    std::unique_ptr<NHttp::TServer> HttpServer_;
    NApi::INativeClientPtr MasterClient_;
    NScheduler::TSchedulerPtr Scheduler_;
    NHiveClient::TClusterDirectoryPtr ClusterDirectory_;
    NRpc::TResponseKeeperPtr ResponseKeeper_;
    NChunkClient::TThrottlerManagerPtr ChunkLocationThrottlerManager_;

    void DoRun();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellScheduler
} // namespace NYT
