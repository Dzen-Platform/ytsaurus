#include "bootstrap.h"

#include "master_connector.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>
#include <yt/yt/server/node/cluster_node/config.h>
#include <yt/yt/server/node/cluster_node/dynamic_config_manager.h>

#include <yt/yt/server/node/data_node/legacy_master_connector.h>

#include <yt/yt/server/node/tablet_node/security_manager.h>

#include <yt/yt/server/lib/cellar_agent/bootstrap_proxy.h>
#include <yt/yt/server/lib/cellar_agent/cellar_manager.h>

namespace NYT::NCellarNode {

using namespace NApi::NNative;
using namespace NCellarAgent;
using namespace NCellarClient;
using namespace NClusterNode;
using namespace NConcurrency;
using namespace NElection;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NRpc;
using namespace NSecurityServer;

////////////////////////////////////////////////////////////////////////////////

class TCellarBootstrapProxy
    : public ICellarBootstrapProxy
{
public:
    explicit TCellarBootstrapProxy(
        IBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    { }

    TCellId GetCellId() const override
    {
        return Bootstrap_->GetCellId();
    }

    IClientPtr GetMasterClient() const override
    {
        return Bootstrap_->GetMasterClient();
    }

    TNetworkPreferenceList GetLocalNetworks() const override
    {
        return Bootstrap_->GetLocalNetworks();
    }

    IInvokerPtr GetControlInvoker() const override
    {
        return Bootstrap_->GetControlInvoker();
    }

    IInvokerPtr GetTransactionTrackerInvoker() const override
    {
        return Bootstrap_->GetTransactionTrackerInvoker();
    }

    IServerPtr GetRpcServer() const override
    {
        return Bootstrap_->GetRpcServer();
    }

    IResourceLimitsManagerPtr GetResourceLimitsManager() const override
    {
        return Bootstrap_->GetResourceLimitsManager();
    }

    void ScheduleCellarHeartbeat(bool immediately) const override
    {
        Bootstrap_->ScheduleCellarHeartbeat(immediately);
    }

private:
    IBootstrap* const Bootstrap_;
};

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
    : public IBootstrap
    , public NClusterNode::TBootstrapForward
{
public:
    explicit TBootstrap(NClusterNode::IBootstrap* bootstrap)
        : TBootstrapForward(bootstrap)
        , ClusterNodeBootstrap_(bootstrap)
    { }

    void Initialize() override
    {
        GetDynamicConfigManager()
            ->SubscribeConfigChanged(BIND(&TBootstrap::OnDynamicConfigChanged, this));

        TransactionTrackerQueue_ = New<TActionQueue>("TxTracker");

        // TODO(gritukan): Move TSecurityManager from Tablet Node.
        ResourceLimitsManager_ = New<NTabletNode::TSecurityManager>(GetConfig()->TabletNode->SecurityManager, this);

        // COMPAT(savrus)
        auto getCellarManagerConfig = [&] {
            auto& config = GetConfig()->CellarNode->CellarManager;

            if (!ClusterNodeBootstrap_->IsTabletNode()) {
                return config;
            }

            for (const auto& [type, _] : config->Cellars) {
                if (type == ECellarType::Tablet) {
                    return config;
                }
            }

            auto cellarConfig = New<TCellarConfig>();
            cellarConfig->Size = GetConfig()->TabletNode->ResourceLimits->Slots;
            cellarConfig->Occupant = New<TCellarOccupantConfig>();
            cellarConfig->Occupant->Snapshots = GetConfig()->TabletNode->Snapshots;
            cellarConfig->Occupant->Changelogs = GetConfig()->TabletNode->Changelogs;
            cellarConfig->Occupant->HydraManager = GetConfig()->TabletNode->HydraManager;
            cellarConfig->Occupant->ElectionManager = GetConfig()->TabletNode->ElectionManager;
            cellarConfig->Occupant->HiveManager = GetConfig()->TabletNode->HiveManager;
            cellarConfig->Occupant->TransactionSupervisor = GetConfig()->TabletNode->TransactionSupervisor;
            cellarConfig->Occupant->ResponseKeeper = GetConfig()->TabletNode->HydraManager->ResponseKeeper;

            auto cellarManagerConfig = CloneYsonSerializable(config);
            cellarManagerConfig->Cellars.insert({ECellarType::Tablet, std::move(cellarConfig)});
            return cellarManagerConfig;
        };

        auto cellarBootstrapProxy = New<TCellarBootstrapProxy>(this);
        CellarManager_ = CreateCellarManager(getCellarManagerConfig(), std::move(cellarBootstrapProxy));

        MasterConnector_ = CreateMasterConnector(this);

        CellarManager_->Initialize();
    }

    void Run() override
    {
        MasterConnector_->Initialize();
    }

    const IInvokerPtr& GetTransactionTrackerInvoker() const override
    {
        return TransactionTrackerQueue_->GetInvoker();
    }

    const IResourceLimitsManagerPtr& GetResourceLimitsManager() const override
    {
        return ResourceLimitsManager_;
    }

    const ICellarManagerPtr& GetCellarManager() const override
    {
        return CellarManager_;
    }

    const IMasterConnectorPtr& GetMasterConnector() const override
    {
        return MasterConnector_;
    }

    void ScheduleCellarHeartbeat(bool immediately) const override
    {
        if (!IsConnected()) {
            return;
        }

        if (UseNewHeartbeats()) {
            for (auto masterCellTag : GetMasterCellTags()) {
                MasterConnector_->ScheduleHeartbeat(masterCellTag, immediately);
            }
        } else {
            // Old heartbeats are heavy, so we send out-of-order heartbeat to primary master cell only.
            auto primaryCellTag = CellTagFromId(GetCellId());
            GetLegacyMasterConnector()->ScheduleNodeHeartbeat(primaryCellTag, immediately);
        }
    }

private:
    NClusterNode::IBootstrap* const ClusterNodeBootstrap_;

    TActionQueuePtr TransactionTrackerQueue_;

    IResourceLimitsManagerPtr ResourceLimitsManager_;

    ICellarManagerPtr CellarManager_;

    IMasterConnectorPtr MasterConnector_;

    void OnDynamicConfigChanged(
        const TClusterNodeDynamicConfigPtr& /*oldConfig*/,
        const TClusterNodeDynamicConfigPtr& newConfig)
    {
        // COMPAT(savrus)
        auto getCellarManagerConfig = [&] {
            auto& config = newConfig->CellarNode->CellarManager;
            if (!newConfig->TabletNode->Slots) {
                return config;
            } else {
                for (const auto& [type, _] : config->Cellars) {
                    if (type == ECellarType::Tablet) {
                        return config;
                    }
                }

                auto cellarManagerConfig = CloneYsonSerializable(config);
                auto cellarConfig = New<TCellarDynamicConfig>();
                cellarConfig->Size = newConfig->TabletNode->Slots;
                cellarManagerConfig->Cellars.insert({ECellarType::Tablet, std::move(cellarConfig)});
                return cellarManagerConfig;
            }
        };

        CellarManager_->Reconfigure(getCellarManagerConfig());

        ResourceLimitsManager_->Reconfigure(newConfig->TabletNode->SecurityManager);
    }
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IBootstrap> CreateBootstrap(NClusterNode::IBootstrap* bootstrap)
{
    return std::make_unique<TBootstrap>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellarNode
