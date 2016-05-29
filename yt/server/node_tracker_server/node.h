#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/chunk_server/chunk_replica.h>
#include <yt/server/chunk_server/public.h>

#include <yt/server/hydra/entity_map.h>

#include <yt/server/tablet_server/public.h>

#include <yt/server/transaction_server/public.h>

#include <yt/server/object_server/object.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>
#include <yt/ytlib/node_tracker_client/node_tracker_service.pb.h>

#include <yt/core/misc/nullable.h>
#include <yt/core/misc/property.h>
#include <yt/core/misc/ref_tracked.h>

namespace NYT {
namespace NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ENodeState,
    // Not registered.
    ((Offline)     (0))
    // Registered but did not report the first heartbeat yet.
    ((Registered)  (1))
    // Registered and reported the first heartbeat.
    ((Online)      (2))
    // Unregistered and placed into disposal queue.
    ((Unregistered)(3))
    // Indicates that state varies across cells.
    ((Mixed)       (4))
);

class TNode
    : public NObjectServer::TObjectBase
    , public TRefTracked<TNode>
{
public:
    // Import third-party types into the scope.
    typedef NChunkServer::TChunkPtrWithIndex TChunkPtrWithIndex;
    typedef NChunkServer::TChunkId TChunkId;
    typedef NChunkServer::TChunk TChunk;
    typedef NChunkServer::TJobPtr TJobPtr;

    // Transient properties.
    DEFINE_BYVAL_RW_PROPERTY(ui64, VisitMark);
    DEFINE_BYVAL_RW_PROPERTY(double, IOWeight);

    using TMulticellStates = yhash_map<NObjectClient::TCellTag, ENodeState>;
    DEFINE_BYREF_RW_PROPERTY(TMulticellStates, MulticellStates);
    DEFINE_BYVAL_RW_PROPERTY(ENodeState*, LocalStatePtr);

    DEFINE_BYREF_RW_PROPERTY(std::vector<Stroka>, UserTags);
    DEFINE_BYREF_RW_PROPERTY(std::vector<Stroka>, NodeTags);

    DEFINE_BYVAL_RW_PROPERTY(TInstant, RegisterTime);
    DEFINE_BYVAL_RW_PROPERTY(TInstant, LastSeenTime);

    DEFINE_BYREF_RW_PROPERTY(NNodeTrackerClient::NProto::TNodeStatistics, Statistics);
    DEFINE_BYREF_RW_PROPERTY(std::vector<TError>, Alerts);

    DEFINE_BYREF_RW_PROPERTY(NNodeTrackerClient::NProto::TNodeResources, ResourceLimits);
    DEFINE_BYREF_RW_PROPERTY(NNodeTrackerClient::NProto::TNodeResources, ResourceUsage);
    DEFINE_BYREF_RW_PROPERTY(NNodeTrackerClient::NProto::TNodeResourceLimitsOverrides, ResourceLimitsOverrides);

    DEFINE_BYVAL_RO_PROPERTY(TRack*, Rack);

    // Lease tracking.
    DEFINE_BYVAL_RW_PROPERTY(NTransactionServer::TTransaction*, LeaseTransaction);

    // Chunk Manager stuff.
    DEFINE_BYVAL_RO_PROPERTY(bool, Banned);
    DEFINE_BYVAL_RO_PROPERTY(bool, Decommissioned);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<NChunkServer::TFillFactorToNodeIterator>, FillFactorIterator);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<NChunkServer::TLoadFactorToNodeIterator>, LoadFactorIterator);

    // NB: Randomize replica hashing to avoid collisions during balancing.
    using TReplicaSet = yhash_set<TChunkPtrWithIndex>;
    DEFINE_BYREF_RO_PROPERTY(TReplicaSet, StoredReplicas);
    DEFINE_BYREF_RO_PROPERTY(TReplicaSet, CachedReplicas);
    
    //! Maps replicas to the leader timestamp when this replica was registered by a client.
    typedef yhash_map<TChunkPtrWithIndex, TInstant> TUnapprovedReplicaMap;
    DEFINE_BYREF_RW_PROPERTY(TUnapprovedReplicaMap, UnapprovedReplicas);

    DEFINE_BYREF_RW_PROPERTY(yhash_set<TJobPtr>, Jobs);

    //! Indexed by priority.
    typedef std::vector<yhash_set<NChunkServer::TChunkPtrWithIndex>> TChunkReplicationQueues;
    DEFINE_BYREF_RW_PROPERTY(TChunkReplicationQueues, ChunkReplicationQueues);

    typedef yhash_set<NChunkClient::TChunkIdWithIndex> TChunkRemovalQueue;
    DEFINE_BYREF_RW_PROPERTY(TChunkRemovalQueue, ChunkRemovalQueue);

    typedef yhash_set<TChunk*> TChunkSealQueue;
    DEFINE_BYREF_RW_PROPERTY(TChunkSealQueue, ChunkSealQueue);

    // Tablet Manager stuff.
    struct TTabletSlot
    {
        NTabletServer::TTabletCell* Cell = nullptr;
        NHydra::EPeerState PeerState = NHydra::EPeerState::None;
        int PeerId = NHydra::InvalidPeerId;

        void Persist(NCellMaster::TPersistenceContext& context);
    };

    using TTabletSlotList = SmallVector<TTabletSlot, NTabletClient::TypicalPeerCount>;
    DEFINE_BYREF_RW_PROPERTY(TTabletSlotList, TabletSlots);

public:
    TNode(
        const NObjectServer::TObjectId& objectId,
        const TAddressMap& addresses);
    explicit TNode(const NObjectServer::TObjectId& objectId);

    TNodeId GetId() const;

    TNodeDescriptor GetDescriptor() const;

    const TAddressMap& GetAddresses() const;
    void SetAddresses(const TAddressMap& addresses);
    const Stroka& GetDefaultAddress() const;

    //! Gets the local state by dereferencing local state pointer.
    ENodeState GetLocalState() const;
    //! If states are same for all cells then returns this common value.
    //! Otherwise returns "mixed" state.
    ENodeState GetAggregatedState() const;
    //! Sets the local state by dereferencing local state pointer.
    void SetLocalState(ENodeState state) const;

    std::vector<Stroka> GetTags() const;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    // Chunk Manager stuff.
    void ReserveStoredReplicas(int sizeHint);
    void ReserveCachedReplicas(int sizeHint);
    bool AddReplica(TChunkPtrWithIndex replica, bool cached);
    //! Retruns |true| if this was an approved non-cached replica.
    bool RemoveReplica(TChunkPtrWithIndex replica, bool cached);
    bool HasReplica(TChunkPtrWithIndex, bool cached) const;
    TChunkPtrWithIndex PickRandomReplica();
    void ClearReplicas();

    void AddUnapprovedReplica(TChunkPtrWithIndex replica, TInstant timestamp);
    bool HasUnapprovedReplica(TChunkPtrWithIndex replica) const;
    void ApproveReplica(TChunkPtrWithIndex replica);

    void AddToChunkRemovalQueue(const NChunkClient::TChunkIdWithIndex& replica);
    void RemoveFromChunkRemovalQueue(const NChunkClient::TChunkIdWithIndex& replica);

    void AddToChunkReplicationQueue(TChunkPtrWithIndex replica, int priority);
    void RemoveFromChunkReplicationQueues(TChunkPtrWithIndex replica);

    void AddToChunkSealQueue(TChunk* chunk);
    void RemoveFromChunkSealQueue(TChunk* chunk);

    void ClearSessionHints();

    void AddSessionHint(NChunkClient::ESessionType sessionType);

    int GetSessionCount(NChunkClient::ESessionType sessionType) const;
    int GetTotalSessionCount() const;

    int GetTotalTabletSlots() const;

    double GetFillFactor() const;
    double GetLoadFactor() const;
    bool IsFull() const;

    TTabletSlot* FindTabletSlot(const NTabletServer::TTabletCell* cell);
    TTabletSlot* GetTabletSlot(const NTabletServer::TTabletCell* cell);

    void DetachTabletCell(const NTabletServer::TTabletCell* cell);

    void InitTabletSlots();
    void ClearTabletSlots();

    void ShrinkHashTables();

    void Reset();

    static ui64 GenerateVisitMark();

private:
    TAddressMap Addresses_;

    int HintedUserSessionCount_;
    int HintedReplicationSessionCount_;
    int HintedRepairSessionCount_;

    TReplicaSet::iterator RandomReplicaIt_;

    void Init();

    static TChunkPtrWithIndex ToGeneric(TChunkPtrWithIndex replica);
    static NChunkClient::TChunkIdWithIndex ToGeneric(const NChunkClient::TChunkIdWithIndex& replica);

    bool AddStoredReplica(TChunkPtrWithIndex replica);
    bool RemoveStoredReplica(TChunkPtrWithIndex replica);
    bool ContainsStoredReplica(TChunkPtrWithIndex replica) const;

    bool AddCachedReplica(TChunkPtrWithIndex replica);
    bool RemoveCachedReplica(TChunkPtrWithIndex replica);
    bool ContainsCachedReplica(TChunkPtrWithIndex replica) const;

    // Private accessors for TNodeTracker.
    friend class TNodeTracker;
    void SetRack(TRack* rack);
    void SetBanned(bool value);
    void SetDecommissioned(bool value);

};

////////////////////////////////////////////////////////////////////////////////

struct TNodePtrAddressFormatter
{
    void operator()(TStringBuilder* builder, TNode* node) const
    {
        builder->AppendString(node->GetDefaultAddress());
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
