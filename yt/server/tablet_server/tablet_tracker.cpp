#include "tablet_tracker.h"
#include "private.h"
#include "config.h"
#include "tablet_cell.h"
#include "tablet_cell_bundle.h"
#include "tablet_manager.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>

#include <yt/server/node_tracker_server/config.h>
#include <yt/server/node_tracker_server/node.h>
#include <yt/server/node_tracker_server/node_tracker.h>

#include <yt/server/table_server/table_node.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/concurrency/periodic_executor.h>

namespace NYT {
namespace NTabletServer {

using namespace NConcurrency;
using namespace NObjectServer;
using namespace NTabletServer::NProto;
using namespace NNodeTrackerServer;
using namespace NHydra;
using namespace NHive;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TTabletTracker::TCandidatePool
{
public:
    explicit TCandidatePool(NCellMaster::TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    { }

    TNode* TryAllocate(
        TTabletCell* cell,
        const SmallSet<Stroka, TypicalPeerCount>& forbiddenAddresses)
    {
        const auto& data = GetData(cell->GetCellBundle());
        for (const auto& pair : data.Nodes) {
            auto* node = pair.second;
            if (forbiddenAddresses.count(node->GetDefaultAddress()) == 0) {
                ChargeNode(node);
                return node;
            }
        }
        return nullptr;
    }

private:
    NCellMaster::TBootstrap* const Bootstrap_;

    struct TPerTagData
    {
        //! Key is minus the number of spare slots.
        std::multimap<int, TNode*> Nodes;
        yhash_map<TNode*, std::multimap<int, TNode*>::iterator> NodeToIterator;
    };

    yhash_map<TNullable<Stroka>, TPerTagData> TagToData_;


    void InsertNode(TPerTagData* data, TNode* node)
    {
        if (!IsGood(node)) {
            return;
        }
        int total = node->GetTotalTabletSlots();
        auto tabletManager = Bootstrap_->GetTabletManager();
        int used = tabletManager->GetAssignedTabletCellCount(node->GetDefaultAddress());
        int spare = total - used;
        if (used >= total) {
            return;
        }
        auto it = data->Nodes.insert(std::make_pair(-spare, node));
        YCHECK(data->NodeToIterator.insert(std::make_pair(node, it)).second);
    }

    const TPerTagData& GetData(TTabletCellBundle* cellBundle)
    {
        auto tag = cellBundle->GetNodeTag();
        auto it = TagToData_.find(tag);
        if (it == TagToData_.end()) {
            it = TagToData_.insert(std::make_pair(tag, TPerTagData())).first;
            auto& data = it->second;
            auto nodeTracker = Bootstrap_->GetNodeTracker();
            for (const auto& pair : nodeTracker->Nodes()) {
                auto* node = pair.second;
                if (node->HasTag(tag)) {
                    InsertNode(&data, node);
                }
            }
        }
        return it->second;
    }

    void ChargeNode(TNode* node)
    {
        for (auto& pair : TagToData_) {
            auto& data = pair.second;
            auto it1 = data.NodeToIterator.find(node);
            if (it1 != data.NodeToIterator.end()) {
                auto it2 = it1->second;
                int spare = -it2->first;
                data.Nodes.erase(it2);
                --spare;
                if (spare > 0) {
                    data.NodeToIterator[node] = data.Nodes.insert(std::make_pair(-spare, node));
                }
            }
        }
    }
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
        Config_->CellScanPeriod);
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

    if (!IsEnabled())
        return;

    TCandidatePool pool(Bootstrap_);

    auto tabletManger = Bootstrap_->GetTabletManager();
    for (const auto& pair : tabletManger->TabletCells()) {
        auto* cell = pair.second;
        if (!IsObjectAlive(cell))
            continue;

        ScheduleLeaderReassignment(cell, &pool);
        SchedulePeerAssignment(cell, &pool);
        SchedulePeerRevocation(cell);
    }
}

void TTabletTracker::ScheduleLeaderReassignment(TTabletCell* cell, TCandidatePool* pool)
{
    // Try to move the leader to a good peer.
    if (!IsFailed(cell, cell->GetLeadingPeerId(), Config_->LeaderReassignmentTimeout))
        return;

    auto goodPeerId = FindGoodPeer(cell);
    if (goodPeerId == InvalidPeerId)
        return;

    TReqSetLeadingPeer request;
    ToProto(request.mutable_cell_id(), cell->GetId());
    request.set_peer_id(goodPeerId);

    auto hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    CreateMutation(hydraManager, request)
        ->CommitAndLog(Logger);
}

void TTabletTracker::SchedulePeerAssignment(TTabletCell* cell, TCandidatePool* pool)
{
    const auto& peers = cell->Peers();

    // Don't assign new peers if there's a follower but no leader.
    // Try to promote the follower first.
    bool hasFollower = false;
    bool hasLeader = false;
    for (const auto& peer : peers) {
        auto* node = peer.Node;
        if (!node)
            continue;
        auto* slot = node->FindTabletSlot(cell);
        if (!slot)
            continue;

        auto state = slot->PeerState;
        if (state == EPeerState::Leading || state == EPeerState::LeaderRecovery) {
            hasLeader = true;
        }
        if (state == EPeerState::Following || state == EPeerState::FollowerRecovery) {
            hasFollower = true;
        }
    }

    if (hasFollower && !hasLeader)
        return;

    // Try to assign missing peers.
    TReqAssignPeers request;
    ToProto(request.mutable_cell_id(), cell->GetId());

    SmallSet<Stroka, TypicalPeerCount> forbiddenAddresses;
    for (const auto& peer : peers) {
        if (!peer.Descriptor.IsNull()) {
            forbiddenAddresses.insert(peer.Descriptor.GetDefaultAddress());
        }
    }

    for (TPeerId id = 0; id < static_cast<int>(cell->Peers().size()); ++id) {
        if (!peers[id].Descriptor.IsNull())
            continue;

        auto* node = pool->TryAllocate(cell, forbiddenAddresses);
        if (!node)
            break;

        auto* peerInfo = request.add_peer_infos();
        peerInfo->set_peer_id(id);
        ToProto(peerInfo->mutable_node_descriptor(), node->GetDescriptor());

        forbiddenAddresses.insert(node->GetDefaultAddress());
    }

    if (request.peer_infos_size() == 0)
        return;

    auto hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    CreateMutation(hydraManager, request)
        ->CommitAndLog(Logger);
}

void TTabletTracker::SchedulePeerRevocation(TTabletCell* cell)
{
    // Don't perform failover until enough time has passed since the start.
    if (TInstant::Now() < StartTime_ + Config_->PeerRevocationTimeout)
        return;

    const auto& cellId = cell->GetId();

    // Look for timed out peers.
    TReqRevokePeers request;
    ToProto(request.mutable_cell_id(), cellId);
    for (TPeerId peerId = 0; peerId < cell->Peers().size(); ++peerId) {
        if (IsFailed(cell, peerId, Config_->PeerRevocationTimeout)) {
            request.add_peer_ids(peerId);
        }
    }

    if (request.peer_ids_size() == 0)
        return;

    auto hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    CreateMutation(hydraManager, request)
        ->CommitAndLog(Logger);
}

bool TTabletTracker::IsFailed(const TTabletCell* cell, TPeerId peerId, TDuration timeout)
{
    const auto& peer = cell->Peers()[peerId];
    if (peer.Descriptor.IsNull()) {
        return false;
    }

    auto nodeTracker = Bootstrap_->GetNodeTracker();
    const auto* node = nodeTracker->FindNodeByAddress(peer.Descriptor.GetDefaultAddress());
    if (node) {
        if (node->GetBanned()) {
            return true;
        }

        if (node->GetDecommissioned()) {
            return true;
        }

        if (!node->HasTag(cell->GetCellBundle()->GetNodeTag())) {
            return true;
        }
    }

    if (peer.LastSeenTime + timeout > TInstant::Now()) {
        return false;
    }

    if (peer.Node) {
        return false;
    }

    return true;
}

bool TTabletTracker::IsGood(const TNode* node)
{
    if (!IsObjectAlive(node)) {
        return false;
    }

    if (node->GetAggregatedState() != ENodeState::Online) {
        return false;
    }

    if (node->GetBanned()) {
        return false;
    }

    if (node->GetDecommissioned()) {
        return false;
    }

    return true;
}

int TTabletTracker::FindGoodPeer(const TTabletCell* cell)
{
    for (TPeerId id = 0; id < static_cast<int>(cell->Peers().size()); ++id) {
        const auto& peer = cell->Peers()[id];
        if (IsGood(peer.Node)) {
            return id;
        }
    }
    return InvalidPeerId;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
