#pragma once

#include "public.h"

#include <yt/server/chunk_server/public.h>

#include <yt/server/cypress_server/public.h>

#include <yt/server/hive/public.h>

#include <yt/server/hydra/public.h>

#include <yt/server/journal_server/public.h>

#include <yt/server/node_tracker_server/public.h>

#include <yt/server/object_server/public.h>

#include <yt/server/security_server/public.h>

#include <yt/server/tablet_server/public.h>

#include <yt/server/transaction_server/public.h>

#include <yt/server/journal_server/public.h>

#include <yt/server/transaction_server/public.h>

#include <yt/ytlib/election/public.h>

#include <yt/ytlib/monitoring/http_server.h>
#include <yt/ytlib/monitoring/public.h>

#include <yt/ytlib/hive/public.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/rpc/public.h>

#include <yt/core/misc/public.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
{
public:
    explicit TBootstrap(NYTree::INodePtr configNode);
    ~TBootstrap();

    const TCellMasterConfigPtr& GetConfig() const;

    bool IsPrimaryMaster() const;
    bool IsSecondaryMaster() const;
    bool IsMulticell() const;

    const NObjectClient::TCellId& GetCellId() const;
    NObjectClient::TCellId GetCellId(NObjectClient::TCellTag cellTag) const;
    NObjectClient::TCellTag GetCellTag() const;

    const NObjectClient::TCellId& GetPrimaryCellId() const;
    NObjectClient::TCellTag GetPrimaryCellTag() const;

    const NObjectClient::TCellTagList& GetSecondaryCellTags() const;

    const TMulticellManagerPtr& GetMulticellManager() const;
    const NRpc::IServerPtr& GetRpcServer() const;
    const NRpc::IChannelPtr GetLocalRpcChannel() const;
    const NElection::TCellManagerPtr& GetCellManager() const;
    const NHydra::IChangelogStoreFactoryPtr& GetChangelogStoreFactory() const;
    const NHydra::ISnapshotStorePtr& GetSnapshotStore() const;
    const NNodeTrackerServer::TNodeTrackerPtr& GetNodeTracker() const;
    const NTransactionServer::TTransactionManagerPtr& GetTransactionManager() const;
    const NHiveServer::TTransactionSupervisorPtr& GetTransactionSupervisor() const;
    const NCypressServer::TCypressManagerPtr& GetCypressManager() const;
    const THydraFacadePtr& GetHydraFacade() const;
    const TWorldInitializerPtr& GetWorldInitializer() const;
    const NObjectServer::TObjectManagerPtr& GetObjectManager() const;
    const NChunkServer::TChunkManagerPtr& GetChunkManager() const;
    const NJournalServer::TJournalManagerPtr& GetJournalManager() const;
    const NSecurityServer::TSecurityManagerPtr& GetSecurityManager() const;
    const NTabletServer::TTabletManagerPtr& GetTabletManager() const;
    const NHiveServer::THiveManagerPtr& GetHiveManager() const;
    const NHiveClient::TCellDirectoryPtr& GetCellDirectory() const;
    const IInvokerPtr& GetControlInvoker() const;

    const NNodeTrackerClient::INodeChannelFactoryPtr& GetLightNodeChannelFactory() const;
    const NNodeTrackerClient::INodeChannelFactoryPtr& GetHeavyNodeChannelFactory() const;

    void Initialize();
    void Run();
    void TryLoadSnapshot(const Stroka& fileName, bool dump);

private:
    const NYTree::INodePtr ConfigNode_;
    TCellMasterConfigPtr Config_;

    bool PrimaryMaster_ = false;
    bool SecondaryMaster_ = false;
    bool Multicell_ = false;

    NObjectClient::TCellId CellId_;
    NObjectClient::TCellTag CellTag_;
    NObjectClient::TCellId PrimaryCellId_;
    NObjectClient::TCellTag PrimaryCellTag_;
    NObjectClient::TCellTagList SecondaryCellTags_;

    TMulticellManagerPtr MulticellManager_;
    NRpc::IServerPtr RpcServer_;
    NRpc::IChannelPtr LocalRpcChannel_;
    NMonitoring::TMonitoringManagerPtr MonitoringManager_;
    std::unique_ptr<NLFAlloc::TLFAllocProfiler> LFAllocProfiler_;
    std::unique_ptr<NHttp::TServer> HttpServer_;
    NElection::TCellManagerPtr CellManager_;
    NHydra::IChangelogStoreFactoryPtr ChangelogStoreFactory_;
    NHydra::ISnapshotStorePtr SnapshotStore_;
    NNodeTrackerServer::TNodeTrackerPtr NodeTracker_;
    NTransactionServer::TTransactionManagerPtr TransactionManager_;
    NHiveServer::TTransactionSupervisorPtr TransactionSupervisor_;
    NCypressServer::TCypressManagerPtr CypressManager_;
    THydraFacadePtr HydraFacade_;
    TWorldInitializerPtr WorldInitializer_;
    NObjectServer::TObjectManagerPtr ObjectManager_;
    NChunkServer::TChunkManagerPtr ChunkManager_;
    NJournalServer::TJournalManagerPtr JournalManager_;
    NSecurityServer::TSecurityManagerPtr SecurityManager_;
    NTabletServer::TTabletManagerPtr TabletManager_;
    NHiveServer::THiveManagerPtr HiveManager_;
    NHiveClient::TCellDirectoryPtr CellDirectory_;
    NHiveServer::TCellDirectorySynchronizerPtr CellDirectorySynchronizer_;
    NConcurrency::TActionQueuePtr ControlQueue_;

    NNodeTrackerClient::INodeChannelFactoryPtr LightNodeChannelFactory_;
    NNodeTrackerClient::INodeChannelFactoryPtr HeavyNodeChannelFactory_;

    static NElection::TPeerId ComputePeerId(
        NElection::TCellConfigPtr config,
        const Stroka& localAddress);

    void DoInitialize();
    void DoRun();
    void DoLoadSnapshot(const Stroka& fileName, bool dump);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
