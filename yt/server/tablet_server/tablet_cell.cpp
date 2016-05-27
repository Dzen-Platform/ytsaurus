#include "tablet_cell.h"
#include "tablet_cell_bundle.h"
#include "tablet.h"

#include <yt/server/cell_master/serialize.h>

#include <yt/server/transaction_server/transaction.h>

#include <yt/server/object_server/object.h>

#include <yt/ytlib/tablet_client/config.h>

namespace NYT {
namespace NTabletServer {

using namespace NYTree;
using namespace NHive;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

void TTabletCell::TPeer::Persist(NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Descriptor);
    Persist(context, Node);
    Persist(context, LastSeenTime);
}

////////////////////////////////////////////////////////////////////////////////

TTabletCell::TTabletCell(const TTabletCellId& id)
    : TNonversionedObjectBase(id)
    , LeadingPeerId_(0)
    , ConfigVersion_(0)
    , Config_(New<TTabletCellConfig>())
    , PrerequisiteTransaction_(nullptr)
    , CellBundle_(nullptr)
{ }

void TTabletCell::Save(TSaveContext& context) const
{
    TNonversionedObjectBase::Save(context);

    using NYT::Save;
    Save(context, LeadingPeerId_);
    Save(context, Peers_);
    Save(context, ConfigVersion_);
    Save(context, *Config_);
    Save(context, Tablets_);
    Save(context, TotalStatistics_);
    Save(context, PrerequisiteTransaction_);
    Save(context, CellBundle_);
}

void TTabletCell::Load(TLoadContext& context)
{
    TNonversionedObjectBase::Load(context);

    using NYT::Load;
    // COMPAT(babenko)
    if (context.GetVersion() < 400) {
        Load<int>(context);
    }
    // COMPAT(babenko)
    if (context.GetVersion() >= 206) {
        Load(context, LeadingPeerId_);
    }
    Load(context, Peers_);
    Load(context, ConfigVersion_);
    Load(context, *Config_);
    // COMPAT(babenko)
    if (context.GetVersion() < 400) {
        auto options = New<TTabletCellOptions>();
        Load(context, *options);
    }
    Load(context, Tablets_);
    Load(context, TotalStatistics_);
    Load(context, PrerequisiteTransaction_);
    // COMPAT(babenko)
    if (context.GetVersion() >= 400) {
        Load(context, CellBundle_);
    }
}

TPeerId TTabletCell::FindPeerId(const Stroka& address) const
{
    for (auto peerId = 0; peerId < Peers_.size(); ++peerId) {
        const auto& peer = Peers_[peerId];
        if (peer.Descriptor.GetDefaultAddress() == address) {
            return peerId;
        }
    }
    return InvalidPeerId;
}

TPeerId TTabletCell::GetPeerId(const Stroka& address) const
{
    auto peerId = FindPeerId(address);
    YCHECK(peerId != InvalidPeerId);
    return peerId;
}

TPeerId TTabletCell::FindPeerId(TNode* node) const
{
    for (TPeerId peerId = 0; peerId < Peers_.size(); ++peerId) {
        if (Peers_[peerId].Node == node) {
            return peerId;
        }
    }
    return InvalidPeerId;
}

TPeerId TTabletCell::GetPeerId(TNode* node) const
{
    auto peerId = FindPeerId(node);
    YCHECK(peerId != InvalidPeerId);
    return peerId;
}

void TTabletCell::AssignPeer(const TCellPeerDescriptor& descriptor, TPeerId peerId)
{
    auto& peer = Peers_[peerId];
    YCHECK(peer.Descriptor.IsNull());
    YCHECK(!descriptor.IsNull());
    peer.Descriptor = descriptor;
}

void TTabletCell::RevokePeer(TPeerId peerId)
{
    auto& peer = Peers_[peerId];
    YCHECK(!peer.Descriptor.IsNull());
    peer.Descriptor = TCellPeerDescriptor();
    peer.Node = nullptr;
}

void TTabletCell::AttachPeer(TNode* node, TPeerId peerId)
{
    auto& peer = Peers_[peerId];
    YCHECK(peer.Descriptor.GetDefaultAddress() == node->GetDefaultAddress());

    YCHECK(!peer.Node);
    peer.Node = node;
}

void TTabletCell::DetachPeer(TNode* node)
{
    auto peerId = FindPeerId(node);
    if (peerId != InvalidPeerId) {
        Peers_[peerId].Node = nullptr;
    }
}

void TTabletCell::UpdatePeerSeenTime(TPeerId peerId, TInstant when)
{
    auto& peer = Peers_[peerId];
    peer.LastSeenTime = when;
}

ETabletCellHealth TTabletCell::GetHealth() const
{
    const auto& leaderPeer = Peers_[LeadingPeerId_];
    auto* leaderNode = leaderPeer.Node;
    if (!IsObjectAlive(leaderNode)) {
        return Tablets_.empty() ? ETabletCellHealth::Initializing : ETabletCellHealth::Failed;
    }

    const auto* leaderSlot = leaderNode->GetTabletSlot(this);
    if (leaderSlot->PeerState != EPeerState::Leading) {
        return Tablets_.empty() ? ETabletCellHealth::Initializing : ETabletCellHealth::Failed;
    }

    for (auto peerId = 0; peerId < static_cast<int>(Peers_.size()); ++peerId) {
        if (peerId == LeadingPeerId_) {
            continue;
        }
        const auto& peer = Peers_[peerId];
        auto* node = peer.Node;
        if (!IsObjectAlive(node)) {
            return ETabletCellHealth::Degraded;
        }
        const auto* slot = node->GetTabletSlot(this);
        if (slot->PeerState != EPeerState::Following) {
            return ETabletCellHealth::Degraded;
        }
    }

    return ETabletCellHealth::Good;
}

TCellDescriptor TTabletCell::GetDescriptor() const
{
    TCellDescriptor descriptor;
    descriptor.CellId = Id_;
    descriptor.ConfigVersion = ConfigVersion_;
    for (auto peerId = 0; peerId < static_cast<int>(Peers_.size()); ++peerId) {
        descriptor.Peers.push_back(TCellPeerDescriptor(
            Peers_[peerId].Descriptor,
            peerId == LeadingPeerId_));
    }
    return descriptor;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

