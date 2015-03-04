#include "stdafx.h"
#include "tablet_tracker.h"
#include "tablet_manager.h"
#include "tablet_cell.h"
#include "config.h"
#include "private.h"

#include <core/concurrency/periodic_executor.h>

#include <server/node_tracker_server/node_tracker.h>
#include <server/node_tracker_server/node.h>

#include <server/table_server/table_node.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/hydra_facade.h>

namespace NYT {
namespace NTabletServer {

using namespace NConcurrency;
using namespace NObjectServer;
using namespace NTabletServer::NProto;
using namespace NNodeTrackerServer;
using namespace NHydra;

////////////////////////////////////////////////////////////////////////////////

static const auto CellsScanPeriod = TDuration::Seconds(3);
static const auto& Logger = TabletServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TTabletTracker::TCandidatePool
{
public:
    explicit TCandidatePool(NCellMaster::TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    {
        auto nodeTracker = Bootstrap_->GetNodeTracker();
        auto tabletManager = Bootstrap_->GetTabletManager();
        for (const auto& pair : nodeTracker->Nodes()) {
            auto* node = pair.second;
            int total = node->GetTotalTabletSlots();
            int used = tabletManager->GetAssignedTabletCellCount(node->GetAddress());
            int spare = total - used;
            if (used < total) {
                MinusSpareSlotsToNode_.insert(std::make_pair(-spare, node));
            }
        }
    }

    TNode* TryAllocate(
        TTabletCell* cell,
        const SmallSet<Stroka, TypicalCellSize>& forbiddenAddresses)
    {
        for (auto it = MinusSpareSlotsToNode_.begin(); it != MinusSpareSlotsToNode_.end(); ++it) {
            int spare = it->first;
            auto* node = it->second;
            if (forbiddenAddresses.count(node->GetAddress()) == 0) {
                MinusSpareSlotsToNode_.erase(it);
                --spare;
                if (spare > 0) {
                    MinusSpareSlotsToNode_.insert(std::make_pair(-spare, node));
                }
                return node;
            }
        }
        return nullptr;
    }

private:
    NCellMaster::TBootstrap* Bootstrap_;
    // NB: "Minus" is to avoid iterating backwards and converting reserve iterator to forward iterator
    // in call to erase.
    std::multimap<int, TNode*> MinusSpareSlotsToNode_;

};

////////////////////////////////////////////////////////////////////////////////

TTabletTracker::TTabletTracker(
    TTabletManagerConfigPtr config,
    NCellMaster::TBootstrap* bootstrap)
    : Config_(config)
    , Bootstrap_(bootstrap)
{
    YCHECK(Config_);
    YCHECK(Bootstrap_);
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(), AutomatonThread);
}

void TTabletTracker::Start()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    StartTime_ = TInstant::Now();

    YCHECK(!PeriodicExecutor_);
    PeriodicExecutor_ = New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(),
        BIND(&TTabletTracker::ScanCells, MakeWeak(this)),
        CellsScanPeriod);
    PeriodicExecutor_->Start();
}

void TTabletTracker::Stop()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (PeriodicExecutor_) {
        PeriodicExecutor_->Stop();
        PeriodicExecutor_.Reset();
    }
}

bool TTabletTracker::IsEnabled()
{
    // This method also logs state changes.

    auto nodeTracker = Bootstrap_->GetNodeTracker();

    int needOnline = Config_->SafeOnlineNodeCount;
    int gotOnline = nodeTracker->GetOnlineNodeCount();

    if (gotOnline < needOnline) {
        if (!LastEnabled_ || *LastEnabled_) {
            LOG_INFO("Tablet tracker disabled: too few online nodes, needed >= %v but got %v",
                needOnline,
                gotOnline);
            LastEnabled_ = false;
        }
        return false;
    }

    if (!LastEnabled_ || !*LastEnabled_) {
        LOG_INFO("Tablet tracker enabled");
        LastEnabled_ = true;
    }

    return true;
}

void TTabletTracker::ScanCells()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    TCandidatePool pool(Bootstrap_);

    auto tabletManger = Bootstrap_->GetTabletManager();
    for (const auto& pair : tabletManger->TabletCells()) {
        auto* cell = pair.second;
        if (!IsObjectAlive(cell))
            continue;

        SchedulePeerStart(cell, &pool);
        SchedulePeerFailover(cell);
    }
}

void TTabletTracker::SchedulePeerStart(TTabletCell* cell, TCandidatePool* pool)
{
    if (!IsEnabled())
        return;

    TReqAssignPeers request;
    ToProto(request.mutable_cell_id(), cell->GetId());

    const auto& peers = cell->Peers();

    SmallSet<Stroka, TypicalCellSize> forbiddenAddresses;
    for (const auto& peer : peers) {
        if (peer.Descriptor) {
            forbiddenAddresses.insert(peer.Descriptor->GetDefaultAddress());
        }
    }

    for (TPeerId peerId = 0; peerId < static_cast<int>(peers.size()); ++peerId) {
        if (peers[peerId].Descriptor)
            continue;

        auto* node = pool->TryAllocate(cell, forbiddenAddresses);
        if (!node)
            break;

        auto* peerInfo = request.add_peer_infos();
        peerInfo->set_peer_id(peerId);
        ToProto(peerInfo->mutable_node_descriptor(), node->GetDescriptor());
        forbiddenAddresses.insert(node->GetAddress());
    }

    if (request.peer_infos_size() == 0)
        return;

    auto hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    CreateMutation(hydraManager, request)
        ->Commit()
        .Subscribe(BIND([] (const TErrorOr<TMutationResponse>& error) {
            if (!error.IsOK()) {
                LOG_WARNING(error, "Error committing peer assignment mutation");
            }
        }));
}

void TTabletTracker::SchedulePeerFailover(TTabletCell* cell)
{
    // Don't perform failover until enough time has passed since the start.
    if (TInstant::Now() < StartTime_ + Config_->PeerFailoverTimeout)
        return;

    const auto& cellId = cell->GetId();

    // Look for timed out peers.
    TReqRevokePeers request;
    ToProto(request.mutable_cell_id(), cellId);
    for (TPeerId peerId = 0; peerId < cell->Peers().size(); ++peerId) {
        if (IsFailoverNeeded(cell, peerId)) {
            request.add_peer_ids(peerId);
        }
    }

    if (request.peer_ids_size() == 0)
        return;

    auto hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    CreateMutation(hydraManager, request)
        ->Commit()
        .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TMutationResponse>& error) {
            if (!error.IsOK()) {
                LOG_WARNING(error, "Error committing peer revocation mutation");
            }
        }));
}

bool TTabletTracker::IsFailoverNeeded(TTabletCell* cell, TPeerId peerId)
{
    const auto& peer = cell->Peers()[peerId];
    if (!peer.Descriptor) {
        return false;
    }

    if (peer.Node) {
        return false;
    }

    if (peer.LastSeenTime > TInstant::Now() - Config_->PeerFailoverTimeout) {
        return false;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
