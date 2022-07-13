#include "node.h"

#include "config.h"
#include "data_center.h"
#include "host.h"
#include "node_tracker_log.h"
#include "rack.h"
#include "private.h"
#include "rack.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/server/master/cell_server/cell_base.h>

#include <yt/yt/server/master/chunk_server/chunk.h>
#include <yt/yt/server/master/chunk_server/chunk_manager.h>
#include <yt/yt/server/master/chunk_server/job.h>
#include <yt/yt/server/master/chunk_server/medium.h>
#include <yt/yt/server/master/chunk_server/chunk_location.h>

#include <yt/yt/server/master/object_server/object.h>

#include <yt/yt/server/master/transaction_server/transaction.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/ytlib/node_tracker_client/helpers.h>
#include <yt/yt/ytlib/node_tracker_client/interop.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/misc/arithmetic_formula.h>
#include <yt/yt/core/misc/collection_helpers.h>

#include <yt/yt/core/net/address.h>

#include <atomic>

namespace NYT::NNodeTrackerServer {

using namespace NNet;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NChunkClient;
using namespace NChunkServer;
using namespace NCellarAgent;
using namespace NCellServer;
using namespace NCellMaster;
using namespace NTabletServer;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NCellarClient;
using namespace NCellarNodeTrackerClient::NProto;
using namespace NYTree;
using namespace NProfiling;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = NodeTrackerServerLogger;

////////////////////////////////////////////////////////////////////////////////

TIncrementalHeartbeatCounters::TIncrementalHeartbeatCounters(const TProfiler& profiler)
    : RemovedChunks(profiler.Counter("/removed_chunk_count"))
    , RemovedUnapprovedReplicas(profiler.Counter("/removed_unapproved_replica_count"))
    , ApprovedReplicas(profiler.Counter("/approved_replica_count"))
    , AddedReplicas(profiler.Counter("/added_replica_count"))
    , AddedDestroyedReplicas(profiler.Counter("/added_destroyed_replica_count"))
{ }

////////////////////////////////////////////////////////////////////////////////

void TNode::TCellSlot::Persist(const NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Cell);
    Persist(context, PeerState);
    Persist(context, PeerId);
    // COMPAT(h0pless)
    if (context.GetVersion() < EMasterReign::RemovedIsResponseKeeperWarmingUp) {
        bool isResponseKeeperWarmingUp;
        Persist(context, isResponseKeeperWarmingUp);
    }
    Persist(context, PreloadPendingStoreCount);
    Persist(context, PreloadCompletedStoreCount);
    Persist(context, PreloadFailedStoreCount);
}

////////////////////////////////////////////////////////////////////////////////

TCellNodeStatistics& operator+=(TCellNodeStatistics& lhs, const TCellNodeStatistics& rhs)
{
    for (const auto& [mediumIndex, chunkReplicaCount] : rhs.ChunkReplicaCount) {
        lhs.ChunkReplicaCount[mediumIndex] += chunkReplicaCount;
    }
    lhs.DestroyedChunkReplicaCount += rhs.DestroyedChunkReplicaCount;
    lhs.ChunkPushReplicationQueuesSize += rhs.ChunkPushReplicationQueuesSize;
    lhs.ChunkPullReplicationQueuesSize += rhs.ChunkPullReplicationQueuesSize;
    lhs.PullReplicationChunkCount += rhs.PullReplicationChunkCount;
    return lhs;
}

void ToProto(
    NProto::TReqSetCellNodeDescriptors::TStatistics* protoStatistics,
    const TCellNodeStatistics& statistics)
{
    for (const auto& [mediumIndex, replicaCount] : statistics.ChunkReplicaCount) {
        if (replicaCount != 0) {
            auto* mediumStatistics = protoStatistics->add_medium_statistics();
            mediumStatistics->set_medium_index(mediumIndex);
            mediumStatistics->set_chunk_replica_count(replicaCount);
        }
    }
    protoStatistics->set_destroyed_chunk_replica_count(statistics.DestroyedChunkReplicaCount);
    protoStatistics->set_chunk_push_replication_queues_size(statistics.ChunkPushReplicationQueuesSize);
    protoStatistics->set_chunk_pull_replication_queues_size(statistics.ChunkPullReplicationQueuesSize);
    protoStatistics->set_pull_replication_chunk_count(statistics.PullReplicationChunkCount);
}

void FromProto(
    TCellNodeStatistics* statistics,
    const NProto::TReqSetCellNodeDescriptors::TStatistics& protoStatistics)
{
    statistics->ChunkReplicaCount.clear();
    for (const auto& mediumStatistics : protoStatistics.medium_statistics()) {
        auto mediumIndex = mediumStatistics.medium_index();
        auto replicaCount = mediumStatistics.chunk_replica_count();
        statistics->ChunkReplicaCount[mediumIndex] = replicaCount;
    }
    statistics->DestroyedChunkReplicaCount = protoStatistics.destroyed_chunk_replica_count();
    statistics->ChunkPushReplicationQueuesSize = protoStatistics.chunk_push_replication_queues_size();
    statistics->ChunkPullReplicationQueuesSize = protoStatistics.chunk_pull_replication_queues_size();
    statistics->PullReplicationChunkCount = protoStatistics.pull_replication_chunk_count();
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TReqSetCellNodeDescriptors::TNodeDescriptor* protoDescriptor, const TCellNodeDescriptor& descriptor)
{
    protoDescriptor->set_state(static_cast<int>(descriptor.State));
    ToProto(protoDescriptor->mutable_statistics(), descriptor.Statistics);
}

void FromProto(TCellNodeDescriptor* descriptor, const NProto::TReqSetCellNodeDescriptors::TNodeDescriptor& protoDescriptor)
{
    descriptor->State = ENodeState(protoDescriptor.state());
    descriptor->Statistics = FromProto<TCellNodeStatistics>(protoDescriptor.statistics());
}

////////////////////////////////////////////////////////////////////////////////

TNode::TNode(TObjectId objectId)
    : TObject(objectId)
{
    ChunkPushReplicationQueues_.resize(ReplicationPriorityCount);
    ChunkPullReplicationQueues_.resize(ReplicationPriorityCount);
    ClearSessionHints();
}

int TNode::GetConsistentReplicaPlacementTokenCount(int mediumIndex) const
{
    auto it = ConsistentReplicaPlacementTokenCount_.find(mediumIndex);
    return it == ConsistentReplicaPlacementTokenCount_.end() ? 0 : it->second;
}

void TNode::ComputeAggregatedState()
{
    std::optional<ENodeState> result;
    for (const auto& [cellTag, descriptor] : MulticellDescriptors_) {
        if (result) {
            if (*result != descriptor.State) {
                result = ENodeState::Mixed;
                break;
            }
        } else {
            result = descriptor.State;
        }
    }
    if (AggregatedState_ != *result) {
        AggregatedState_ = *result;
        AggregatedStateChanged_.Fire(this);
    }
}

void TNode::ComputeDefaultAddress()
{
    DefaultAddress_ = NNodeTrackerClient::GetDefaultAddress(GetAddressesOrThrow(EAddressType::InternalRpc));
}

bool TNode::IsDataNode() const
{
    return Flavors_.contains(ENodeFlavor::Data);
}

bool TNode::IsExecNode() const
{
    return Flavors_.contains(ENodeFlavor::Exec);
}

bool TNode::IsTabletNode() const
{
    return Flavors_.contains(ENodeFlavor::Tablet);
}

bool TNode::IsChaosNode() const
{
    return Flavors_.contains(ENodeFlavor::Chaos);
}

bool TNode::IsCellarNode() const
{
    return IsTabletNode() || IsChaosNode();
}

bool TNode::ReportedClusterNodeHeartbeat() const
{
    return ReportedHeartbeats_.contains(ENodeHeartbeatType::Cluster);
}

bool TNode::ReportedDataNodeHeartbeat() const
{
    return ReportedHeartbeats_.contains(ENodeHeartbeatType::Data);
}

bool TNode::ReportedExecNodeHeartbeat() const
{
    return ReportedHeartbeats_.contains(ENodeHeartbeatType::Exec);
}

bool TNode::ReportedCellarNodeHeartbeat() const
{
    return ReportedHeartbeats_.contains(ENodeHeartbeatType::Cellar);
}

bool TNode::ReportedTabletNodeHeartbeat() const
{
    return ReportedHeartbeats_.contains(ENodeHeartbeatType::Tablet);
}

void TNode::ValidateRegistered()
{
    auto state = GetLocalState();
    if (state == ENodeState::Registered || state == ENodeState::Online) {
        return;
    }

    THROW_ERROR_EXCEPTION(NNodeTrackerClient::EErrorCode::InvalidState, "Node is not registered")
        << TErrorAttribute("local_node_state", state);
}

void TNode::SetClusterNodeStatistics(NNodeTrackerClient::NProto::TClusterNodeStatistics&& statistics)
{
    ClusterNodeStatistics_.Swap(&statistics);
}

void TNode::SetExecNodeStatistics(NNodeTrackerClient::NProto::TExecNodeStatistics&& statistics)
{
    ExecNodeStatistics_.Swap(&statistics);
}

void TNode::ComputeFillFactorsAndTotalSpace()
{
    TMediumMap<std::pair<i64, i64>> freeAndUsedSpace;

    for (const auto& location : DataNodeStatistics_.chunk_locations()) {
        auto mediumIndex = location.medium_index();
        auto& space = freeAndUsedSpace[mediumIndex];
        auto& freeSpace = space.first;
        auto& usedSpace = space.second;
        freeSpace += std::max(static_cast<i64>(0), location.available_space() - location.low_watermark_space());
        usedSpace += location.used_space();
    }

    TotalSpace_.clear();

    for (const auto& [mediumIndex, space] : freeAndUsedSpace) {
        auto freeSpace = space.first;
        auto usedSpace = space.second;

        i64 totalSpace = freeSpace + usedSpace;
        FillFactors_[mediumIndex] = (totalSpace == 0)
            ? std::nullopt
            : std::make_optional(usedSpace / std::max<double>(1.0, totalSpace));
        TotalSpace_[mediumIndex] = totalSpace;
    }
}

void TNode::ComputeSessionCount()
{
    SessionCount_.clear();
    for (const auto& location : DataNodeStatistics_.chunk_locations()) {
        auto mediumIndex = location.medium_index();
        if (location.enabled() && !location.full()) {
            SessionCount_[mediumIndex] = SessionCount_[mediumIndex].value_or(0) + location.session_count();
        }
    }
}

TNodeId TNode::GetId() const
{
    return NodeIdFromObjectId(Id_);
}

const TNodeAddressMap& TNode::GetNodeAddresses() const
{
    return NodeAddresses_;
}

void TNode::SetNodeAddresses(const TNodeAddressMap& nodeAddresses)
{
    NodeAddresses_ = nodeAddresses;
    ComputeDefaultAddress();
}

const TAddressMap& TNode::GetAddressesOrThrow(EAddressType addressType) const
{
    return NNodeTrackerClient::GetAddressesOrThrow(NodeAddresses_, addressType);
}

const TString& TNode::GetDefaultAddress() const
{
    return DefaultAddress_;
}

TRack* TNode::GetRack() const
{
    auto* host = GetHost();
    return host ? host->GetRack() : nullptr;
}

TDataCenter* TNode::GetDataCenter() const
{
    auto* rack = GetRack();
    return rack ? rack->GetDataCenter() : nullptr;
}

bool TNode::HasTag(const std::optional<TString>& tag) const
{
    return !tag || Tags_.find(*tag) != Tags_.end();
}

TNodeDescriptor TNode::GetDescriptor(EAddressType addressType) const
{
    auto* host = GetHost();
    auto* rack = GetRack();
    auto* dataCenter = GetDataCenter();

    return TNodeDescriptor(
        GetAddressesOrThrow(addressType),
        host ? std::make_optional(host->GetName()) : std::nullopt,
        rack ? std::make_optional(rack->GetName()) : std::nullopt,
        dataCenter ? std::make_optional(dataCenter->GetName()) : std::nullopt,
        std::vector<TString>(Tags_.begin(), Tags_.end()),
        (GetAggregatedState() == ENodeState::Online) ? std::make_optional(TInstant::Now()) : std::nullopt);
}


void TNode::InitializeStates(TCellTag cellTag, const TCellTagList& secondaryCellTags)
{
    auto addCell = [&] (TCellTag someTag) {
        if (MulticellDescriptors_.find(someTag) == MulticellDescriptors_.end()) {
            YT_VERIFY(MulticellDescriptors_.emplace(someTag, TCellNodeDescriptor{ENodeState::Offline, TCellNodeStatistics()}).second);
        }
    };

    addCell(cellTag);
    for (auto secondaryCellTag : secondaryCellTags) {
        addCell(secondaryCellTag);
    }

    LocalStatePtr_ = &MulticellDescriptors_[cellTag].State;

    ComputeAggregatedState();
}

void TNode::RecomputeIOWeights(const IChunkManagerPtr& chunkManager)
{
    IOWeights_.clear();
    for (const auto& statistics : DataNodeStatistics_.media()) {
        auto mediumIndex = statistics.medium_index();
        auto* medium = chunkManager->FindMediumByIndex(mediumIndex);
        if (!medium || medium->GetCache()) {
            continue;
        }
        IOWeights_[mediumIndex] = statistics.io_weight();
    }
}

ENodeState TNode::GetLocalState() const
{
    return *LocalStatePtr_;
}

void TNode::SetLocalState(ENodeState state)
{
    if (*LocalStatePtr_ != state) {
        *LocalStatePtr_ = state;
        ComputeAggregatedState();

        if (state == ENodeState::Unregistered) {
            ClearCellStatistics();
        }
    }
}

void TNode::SetCellDescriptor(TCellTag cellTag, const TCellNodeDescriptor& descriptor)
{
    auto& oldDescriptor = GetOrCrash(MulticellDescriptors_, cellTag);
    auto mustRecomputeState = (oldDescriptor.State != descriptor.State);
    oldDescriptor = descriptor;
    if (mustRecomputeState) {
        ComputeAggregatedState();
    }
}

ENodeState TNode::GetAggregatedState() const
{
    return AggregatedState_;
}

TString TNode::GetLowercaseObjectName() const
{
    return Format("node %v", GetDefaultAddress());
}

TString TNode::GetCapitalizedObjectName() const
{
    return Format("Node %v", GetDefaultAddress());
}

void TNode::Save(TSaveContext& context) const
{
    TObject::Save(context);

    using NYT::Save;
    Save(context, Banned_);
    Save(context, Decommissioned_);
    Save(context, DisableWriteSessions_);
    Save(context, DisableSchedulerJobs_);
    Save(context, DisableTabletCells_);
    Save(context, NodeAddresses_);
    {
        using TMulticellStates = THashMap<NObjectClient::TCellTag, ENodeState>;
        TMulticellStates multicellStates;
        multicellStates.reserve(MulticellDescriptors_.size());
        for (const auto& [cellTag, descriptor] : MulticellDescriptors_) {
            multicellStates.emplace(cellTag, descriptor.State);
        }

        Save(context, multicellStates);
    }
    Save(context, UserTags_);
    Save(context, NodeTags_);
    Save(context, ChunkLocations_);
    Save(context, RegisterTime_);
    Save(context, LastSeenTime_);
    Save(context, ClusterNodeStatistics_);
    Save(context, DataNodeStatistics_);
    Save(context, ExecNodeStatistics_);
    Save(context, JobProxyVersion_);
    Save(context, CellarNodeStatistics_);
    Save(context, Alerts_);
    Save(context, ResourceLimits_);
    Save(context, ResourceUsage_);
    Save(context, ResourceLimitsOverrides_);
    Save(context, Host_);
    Save(context, LeaseTransaction_);
    Save(context, DestroyedReplicas_);

    // The is the replica statistics section; the format is as folows:
    // (replicaCount, mediumIndex) for each medium with non-empty set of replicas
    // 0
    {
        TCompactVector<int, 8> mediumIndexes;
        for (const auto& [mediumIndex, replicas] : Replicas_) {
            if (!replicas.empty()) {
                mediumIndexes.push_back(mediumIndex);
            }
        }
        std::sort(mediumIndexes.begin(), mediumIndexes.end());
        for (auto mediumIndex : mediumIndexes) {
            const auto& replicas = GetOrCrash(Replicas_, mediumIndex);
            TSizeSerializer::Save(context, replicas.size());
            Save(context, mediumIndex);
        }
        TSizeSerializer::Save(context, 0);
    }

    Save(context, UnapprovedReplicas_);
    Save(context, Cellars_);
    Save(context, Annotations_);
    Save(context, Version_);
    Save(context, Flavors_);
    Save(context, ReportedHeartbeats_);
    Save(context, ExecNodeIsNotDataNode_);
    Save(context, ReplicaEndorsements_);
    Save(context, ConsistentReplicaPlacementTokenCount_);
}

void TNode::Load(TLoadContext& context)
{
    TObject::Load(context);

    using NYT::Load;
    Load(context, Banned_);
    Load(context, Decommissioned_);
    Load(context, DisableWriteSessions_);
    Load(context, DisableSchedulerJobs_);
    Load(context, DisableTabletCells_);
    Load(context, NodeAddresses_);

    {
        using TMulticellStates = THashMap<NObjectClient::TCellTag, ENodeState>;
        TMulticellStates multicellStates;
        Load(context, multicellStates);

        MulticellDescriptors_.clear();
        MulticellDescriptors_.reserve(multicellStates.size());
        for (const auto& [cellTag, state] : multicellStates) {
            MulticellDescriptors_.emplace(cellTag, TCellNodeDescriptor{state, TCellNodeStatistics()});
        }
    }

    Load(context, UserTags_);
    Load(context, NodeTags_);

    if (context.GetVersion() >= EMasterReign::ChunkLocation) {
        Load(context, ChunkLocations_);
    }

    Load(context, RegisterTime_);
    Load(context, LastSeenTime_);

    Load(context, ClusterNodeStatistics_);
    Load(context, DataNodeStatistics_);
    Load(context, ExecNodeStatistics_);

    // COMPAT(galtsev)
    if (context.GetVersion() >= EMasterReign::JobProxyBuildVersion) {
        Load(context, JobProxyVersion_);
    }

    Load(context, CellarNodeStatistics_);

    Load(context, Alerts_);
    Load(context, ResourceLimits_);
    Load(context, ResourceUsage_);
    Load(context, ResourceLimitsOverrides_);

    Load(context, Host_);

    Load(context, LeaseTransaction_);

    Load(context, DestroyedReplicas_);

    // NB: This code does not load the replicas per se; it just
    // reserves the appropriate hashtables. Once the snapshot is fully loaded,
    // per-node replica sets get reconstructed from the inverse chunk-to-node mapping.
    // Cf. TNode::Load.
    while (true) {
        auto replicaCount = TSizeSerializer::Load(context);
        if (replicaCount == 0) {
            break;
        }
        auto mediumIndex = Load<int>(context);
        ReserveReplicas(mediumIndex, replicaCount);
    }

    Load(context, UnapprovedReplicas_);
    Load(context, Cellars_);
    Load(context, Annotations_);
    Load(context, Version_);
    if (context.GetVersion() < EMasterReign::ChunkLocation) {
        Load(context, CompatChunkLocationUuids_);
    }
    Load(context, Flavors_);
    // COMPAT(savrus) ENodeHeartbeatType is compatible with ENodeFlavor.
    Load(context, ReportedHeartbeats_);

    // COMPAT(gritukan)
    if (context.GetVersion() >= EMasterReign::ExecNodeIsNotDataNode) {
        Load(context, ExecNodeIsNotDataNode_);
    }

    Load(context, ReplicaEndorsements_);

    Load(context, ConsistentReplicaPlacementTokenCount_);

    // COMPAT(babenko)
    if (context.GetVersion() >= EMasterReign::MediumOverridesViaHeartbeats &&
        context.GetVersion() < EMasterReign::ChunkLocation)
    {
        using TMediumOverrideMap = THashMap<TChunkLocationUuid, int>;
        Load<TMediumOverrideMap>(context);
    }

    ComputeDefaultAddress();
    ResetDestroyedReplicasIterator();
    ComputeFillFactorsAndTotalSpace();
}

TJobPtr TNode::FindJob(TJobId jobId)
{
    auto it = IdToJob_.find(jobId);
    return it == IdToJob_.end() ? nullptr : it->second;
}

void TNode::RegisterJob(const TJobPtr& job)
{
    YT_VERIFY(IdToJob_.emplace(job->GetJobId(), job).second);
}

void TNode::UnregisterJob(const TJobPtr& job)
{
    YT_VERIFY(IdToJob_.erase(job->GetJobId()) == 1);
}

void TNode::ReserveReplicas(int mediumIndex, int sizeHint)
{
    Replicas_[mediumIndex].reserve(sizeHint);
    RandomReplicaIters_[mediumIndex] = Replicas_[mediumIndex].end();
}

bool TNode::AddReplica(TChunkPtrWithIndexes replica)
{
    auto* chunk = replica.GetPtr();
    if (chunk->IsJournal()) {
        DoRemoveJournalReplicas(replica);
    }
    // NB: For journal chunks result is always true.
    return DoAddReplica(replica);
}

bool TNode::RemoveReplica(TChunkPtrWithIndexes replica)
{
    auto* chunk = replica.GetPtr();
    if (chunk->IsJournal()) {
        DoRemoveJournalReplicas(replica);
    } else {
        DoRemoveReplica(replica);
    }
    return UnapprovedReplicas_.erase(replica.ToGenericState()) == 0;
}

bool TNode::HasReplica(TChunkPtrWithIndexes replica) const
{
    auto* chunk = replica.GetPtr();
    if (chunk->IsJournal()) {
        auto replicaIndex = replica.GetReplicaIndex();
        auto mediumIndex = replica.GetMediumIndex();
        for (auto state : TEnumTraits<EChunkReplicaState>::GetDomainValues()) {
            if (DoHasReplica(TChunkPtrWithIndexes(chunk, replicaIndex, mediumIndex, state))) {
                return true;
            }
        }
        return false;
    } else {
        return DoHasReplica(replica);
    }
}

TChunkPtrWithIndexes TNode::PickRandomReplica(int mediumIndex)
{
    auto it = Replicas_.find(mediumIndex);
    if (it == Replicas_.end() || it->second.empty()) {
        return TChunkPtrWithIndexes();
    }

    auto& randomReplicaIt = RandomReplicaIters_[mediumIndex];

    if (randomReplicaIt == it->second.end()) {
        randomReplicaIt = it->second.begin();
    }

    return *(randomReplicaIt++);
}

void TNode::ClearReplicas()
{
    Replicas_.clear();
    UnapprovedReplicas_.clear();
    RandomReplicaIters_.clear();
    ClearDestroyedReplicas();
}

void TNode::AddUnapprovedReplica(TChunkPtrWithIndexes replica, TInstant timestamp)
{
    YT_VERIFY(UnapprovedReplicas_.emplace(replica.ToGenericState(), timestamp).second);
}

bool TNode::HasUnapprovedReplica(TChunkPtrWithIndexes replica) const
{
    return UnapprovedReplicas_.find(replica.ToGenericState()) != UnapprovedReplicas_.end();
}

void TNode::ApproveReplica(TChunkPtrWithIndexes replica)
{
    YT_VERIFY(UnapprovedReplicas_.erase(replica.ToGenericState()) == 1);
    auto* chunk = replica.GetPtr();
    if (chunk->IsJournal()) {
        DoRemoveJournalReplicas(replica);
        YT_VERIFY(DoAddReplica(replica));
    }
}

void TNode::ClearDestroyedReplicas()
{
    DestroyedReplicas_.clear();
    ResetDestroyedReplicasIterator();
}

bool TNode::AddDestroyedReplica(const TChunkIdWithIndexes& replica)
{
    RemoveFromChunkRemovalQueue(replica);

    auto [it, inserted] = DestroyedReplicas_.insert(replica);
    if (!inserted) {
        return false;
    }
    DestroyedReplicasIterator_ = it;
    return true;
}

bool TNode::RemoveDestroyedReplica(const TChunkIdWithIndexes& replica)
{
    if (!DestroyedReplicas_.empty() && *DestroyedReplicasIterator_ == replica) {
        if (DestroyedReplicas_.size() == 1) {
            DestroyedReplicasIterator_ = DestroyedReplicas_.end();
        } else {
            AdvanceDestroyedReplicasIterator();
        }
    }
    return DestroyedReplicas_.erase(replica) > 0;
}

void TNode::AddToChunkRemovalQueue(const TChunkIdWithIndexes& replica)
{
    YT_ASSERT(ReportedDataNodeHeartbeat());

    if (DestroyedReplicas_.contains(replica)) {
        return;
    }

    ChunkRemovalQueue_[replica].set(replica.MediumIndex);
}

void TNode::RemoveFromChunkRemovalQueue(const TChunkIdWithIndexes& replica)
{
    auto it = ChunkRemovalQueue_.find(replica);
    if (it != ChunkRemovalQueue_.end()) {
        it->second.reset(replica.MediumIndex);
        if (it->second.none()) {
            ChunkRemovalQueue_.erase(it);
        }
    }
}

void TNode::AddToChunkPushReplicationQueue(TChunkPtrWithIndexes replica, int targetMediumIndex, int priority)
{
    YT_ASSERT(ReportedDataNodeHeartbeat());
    ChunkPushReplicationQueues_[priority][replica.ToGenericState()].set(targetMediumIndex);
}

void TNode::AddToChunkPullReplicationQueue(TChunkPtrWithIndexes replica, int targetMediumIndex, int priority)
{
    YT_ASSERT(ReportedDataNodeHeartbeat());
    ChunkPullReplicationQueues_[priority][replica.ToGenericState()].set(targetMediumIndex);
}

void TNode::AddToPullReplicationSet(TChunkId chunkId, int targetMediumIndex)
{
    YT_ASSERT(ReportedDataNodeHeartbeat());
    ChunksBeingPulled_[chunkId].set(targetMediumIndex);

}

void TNode::AddTargetReplicationNodeId(TChunkId chunkId, int targetMediumIndex, TNode* node)
{
    YT_ASSERT(ReportedDataNodeHeartbeat());
    if (!PushReplicationTargetNodeIds_[chunkId].emplace(targetMediumIndex, node->GetId()).second) {
        YT_LOG_ALERT("Pull replication is already planned for this chunk to another destination (ChunkId: %v, SourceNodeId: %v, TargetNodeId: %v)",
            chunkId,
            GetId(),
            node->GetId());
    }
}

TNodeId TNode::GetTargetReplicationNodeId(TChunkId chunkId, int targetMediumIndex)
{
    auto it = PushReplicationTargetNodeIds_.find(chunkId);
    if (it == PushReplicationTargetNodeIds_.end()) {
        return InvalidNodeId;
    }

    auto idIt = it->second.find(targetMediumIndex);
    if (idIt == it->second.end()) {
        return InvalidNodeId;
    }
    return idIt->second;
}

void TNode::RemoveTargetReplicationNodeId(TChunkId chunkId, int targetMediumIndex)
{
    auto it = PushReplicationTargetNodeIds_.find(chunkId);
    if (it == PushReplicationTargetNodeIds_.end()) {
        return;
    }

    it->second.erase(targetMediumIndex);
    if (it->second.empty()) {
        PushReplicationTargetNodeIds_.erase(it);
    }
}

void TNode::RemoveFromPullReplicationSet(TChunkId chunkId, int targetMediumIndex)
{
    auto it = ChunksBeingPulled_.find(chunkId);
    if (it == ChunksBeingPulled_.end()) {
        return;
    }

    if (targetMediumIndex == AllMediaIndex) {
        ChunksBeingPulled_.erase(it);
    } else {
        it->second.reset(targetMediumIndex);
        if (it->second.none()) {
            ChunksBeingPulled_.erase(it);
        }
    }
}

void TNode::RemoveFromChunkReplicationQueues(TChunkPtrWithIndexes replica)
{
    auto removeFromQueue = [&] (auto& queue) {
        if (auto it = queue.find(replica.ToGenericState()); it != queue.end()) {
            queue.erase(it);
        }
    };

    for (auto& queue : ChunkPushReplicationQueues_) {
        removeFromQueue(queue);
    }
    for (auto& queue : ChunkPullReplicationQueues_) {
        removeFromQueue(queue);
    }

    auto chunkId = replica.GetPtr()->GetId();
    RemoveFromPullReplicationSet(chunkId, AllMediaIndex);

    if (auto it = PushReplicationTargetNodeIds_.find(chunkId); it != PushReplicationTargetNodeIds_.end()) {
        PushReplicationTargetNodeIds_.erase(it);
    }
}

void TNode::AddToChunkSealQueue(TChunkPtrWithIndexes replica)
{
    YT_ASSERT(ReportedDataNodeHeartbeat());
    ChunkSealQueue_.insert(replica);
}

void TNode::RemoveFromChunkSealQueue(TChunkPtrWithIndexes replica)
{
    ChunkSealQueue_.erase(replica);
}

void TNode::ClearSessionHints()
{
    HintedUserSessionCount_ .clear();
    HintedReplicationSessionCount_.clear();
    HintedRepairSessionCount_.clear();

    TotalHintedUserSessionCount_ = 0;
    TotalHintedReplicationSessionCount_ = 0;
    TotalHintedRepairSessionCount_ = 0;
}

void TNode::AddSessionHint(int mediumIndex, ESessionType sessionType)
{
    switch (sessionType) {
        case ESessionType::User:
            ++HintedUserSessionCount_[mediumIndex];
            ++TotalHintedUserSessionCount_;
            break;
        case ESessionType::Replication:
            ++HintedReplicationSessionCount_[mediumIndex];
            ++TotalHintedReplicationSessionCount_;
            break;
        case ESessionType::Repair:
            ++HintedRepairSessionCount_[mediumIndex];
            ++TotalHintedRepairSessionCount_;
            break;
        default:
            YT_ABORT();
    }
}

int TNode::GetHintedSessionCount(int mediumIndex, int chunkHostMasterCellCount) const
{
    // Individual chunk host cells are unaware of each other's hinted sessions
    // scheduled to the same node. Take that into account to avoid bursts.
    return SessionCount_.lookup(mediumIndex).value_or(0) +
        chunkHostMasterCellCount * (
            HintedUserSessionCount_.lookup(mediumIndex) +
            HintedReplicationSessionCount_.lookup(mediumIndex) +
            HintedRepairSessionCount_.lookup(mediumIndex));
}

int TNode::GetSessionCount(ESessionType sessionType) const
{
    switch (sessionType) {
        case ESessionType::User:
            return DataNodeStatistics_.total_user_session_count() + TotalHintedUserSessionCount_;
        case ESessionType::Replication:
            return DataNodeStatistics_.total_replication_session_count() + TotalHintedReplicationSessionCount_;
        case ESessionType::Repair:
            return DataNodeStatistics_.total_repair_session_count() + TotalHintedRepairSessionCount_;
        default:
            YT_ABORT();
    }
}

int TNode::GetTotalSessionCount() const
{
    return
        DataNodeStatistics_.total_user_session_count() + TotalHintedUserSessionCount_ +
        DataNodeStatistics_.total_replication_session_count() + TotalHintedReplicationSessionCount_ +
        DataNodeStatistics_.total_repair_session_count() + TotalHintedRepairSessionCount_;
}

TNode::TCellSlot* TNode::FindCellSlot(const TCellBase* cell)
{
    if (auto* cellar = FindCellar(cell->GetCellarType())) {
        auto predicate = [cell] (const auto& slot) {
            return slot.Cell == cell;
        };

        auto it = std::find_if(cellar->begin(), cellar->end(), predicate);
        if (it != cellar->end()) {
            YT_VERIFY(std::find_if(it + 1, cellar->end(), predicate) == cellar->end());
            return &*it;
        }
    }
    return nullptr;
}

TNode::TCellSlot* TNode::GetCellSlot(const TCellBase* cell)
{
    auto* slot = FindCellSlot(cell);
    YT_VERIFY(slot);
    return slot;
}

void TNode::DetachCell(const TCellBase* cell)
{
    if (auto* slot = FindCellSlot(cell)) {
        *slot = TCellSlot();
    }
}

void TNode::ShrinkHashTables()
{
    for (auto& [mediumIndex, replicas] : Replicas_) {
        if (ShrinkHashTable(&replicas)) {
            RandomReplicaIters_[mediumIndex] = replicas.end();
        }
    }
    ShrinkHashTable(&UnapprovedReplicas_);
    ShrinkHashTable(&IdToJob_);
    for (auto& queue : ChunkPushReplicationQueues_) {
        ShrinkHashTable(&queue);
    }
    for (auto& queue : ChunkPullReplicationQueues_) {
        ShrinkHashTable(&queue);
    }
    ShrinkHashTable(&ChunksBeingPulled_);
    ShrinkHashTable(&ChunkRemovalQueue_);
    ShrinkHashTable(&ChunkSealQueue_);
}

void TNode::Reset()
{
    LastGossipState_ = ENodeState::Unknown;
    ClearSessionHints();
    IdToJob_.clear();
    ChunkRemovalQueue_.clear();
    for (auto& queue : ChunkPushReplicationQueues_) {
        queue.clear();
    }
    for (auto& queue : ChunkPullReplicationQueues_) {
        queue.clear();
    }
    ChunksBeingPulled_.clear();
    PushReplicationTargetNodeIds_.clear();
    ChunkSealQueue_.clear();
    FillFactorIterators_.clear();
    LoadFactorIterators_.clear();
    DisableWriteSessionsSentToNode_ = false;
    DisableWriteSessionsReportedByNode_ = false;
    ClearCellStatistics();
    ResetDestroyedReplicasIterator();
}

ui64 TNode::GenerateVisitMark()
{
    static std::atomic<ui64> result(0);
    return ++result;
}

ui64 TNode::GetVisitMark(int mediumIndex)
{
    return VisitMarks_[mediumIndex];
}

void TNode::SetVisitMark(int mediumIndex, ui64 mark)
{
    VisitMarks_[mediumIndex] = mark;
}

void TNode::SetDataNodeStatistics(
    NNodeTrackerClient::NProto::TDataNodeStatistics&& statistics,
    const IChunkManagerPtr& chunkManager)
{
    DataNodeStatistics_.Swap(&statistics);
    ComputeFillFactorsAndTotalSpace();
    ComputeSessionCount();
    RecomputeIOWeights(chunkManager);
}

void TNode::ValidateNotBanned()
{
    if (Banned_) {
        THROW_ERROR_EXCEPTION("Node %v is banned", GetDefaultAddress());
    }
}

bool TNode::HasMedium(int mediumIndex) const
{
    const auto& locations = DataNodeStatistics_.chunk_locations();
    auto it = std::find_if(
        locations.begin(),
        locations.end(),
        [=] (const auto& location) {
            return location.medium_index() == mediumIndex;
        });
    return it != locations.end();
}

std::optional<double> TNode::GetFillFactor(int mediumIndex) const
{
    return FillFactors_.lookup(mediumIndex);
}

std::optional<double> TNode::GetLoadFactor(int mediumIndex, int chunkHostMasterCellCount) const
{
    // NB: Avoid division by zero.
    return SessionCount_.lookup(mediumIndex)
        ? std::make_optional(
            static_cast<double>(GetHintedSessionCount(mediumIndex, chunkHostMasterCellCount)) /
            std::max(IOWeights_.lookup(mediumIndex), 0.000000001))
        : std::nullopt;
}

TNode::TFillFactorIterator TNode::GetFillFactorIterator(int mediumIndex) const
{
    return FillFactorIterators_.lookup(mediumIndex);
}

void TNode::SetFillFactorIterator(int mediumIndex, TFillFactorIterator iter)
{
    FillFactorIterators_[mediumIndex] = iter;
}

TNode::TLoadFactorIterator TNode::GetLoadFactorIterator(int mediumIndex) const
{
    return LoadFactorIterators_.lookup(mediumIndex);
}

void TNode::SetLoadFactorIterator(int mediumIndex, TLoadFactorIterator iter)
{
    LoadFactorIterators_[mediumIndex] = iter;
}

bool TNode::IsWriteEnabled(int mediumIndex) const
{
    return IOWeights_.lookup(mediumIndex) > 0;
}

bool TNode::DoAddReplica(TChunkPtrWithIndexes replica)
{
    auto mediumIndex = replica.GetMediumIndex();
    auto [it, inserted] = Replicas_[mediumIndex].insert(replica);
    if (!inserted) {
        return false;
    }
    RandomReplicaIters_[mediumIndex] = it;
    return true;
}

bool TNode::DoRemoveReplica(TChunkPtrWithIndexes replica)
{
    auto mediumIndex = replica.GetMediumIndex();
    if (Replicas_.find(mediumIndex) == Replicas_.end()) {
        return false;
    }
    auto& randomReplicaIt = RandomReplicaIters_[mediumIndex];
    auto& mediumStoredReplicas = Replicas_[mediumIndex];
    if (randomReplicaIt != mediumStoredReplicas.end() &&
        *randomReplicaIt == replica)
    {
        ++randomReplicaIt;
    }
    return mediumStoredReplicas.erase(replica) == 1;
}

void TNode::DoRemoveJournalReplicas(TChunkPtrWithIndexes replica)
{
    auto* chunk = replica.GetPtr();
    auto replicaIndex = replica.GetReplicaIndex();
    auto mediumIndex = replica.GetMediumIndex();
    for (auto state : TEnumTraits<EChunkReplicaState>::GetDomainValues()) {
        DoRemoveReplica(TChunkPtrWithIndexes(chunk, replicaIndex, mediumIndex, state));
    }
}

bool TNode::DoHasReplica(TChunkPtrWithIndexes replica) const
{
    auto mediumIndex = replica.GetMediumIndex();
    auto it = Replicas_.find(mediumIndex);
    if (it == Replicas_.end()) {
        return false;
    }
    return it->second.find(replica) != it->second.end();
}

void TNode::SetHost(THost* host)
{
    if (Host_) {
        Host_->RemoveNode(this);
    }

    Host_ = host;

    if (Host_) {
        Host_->AddNode(this);
    }
}

bool TNode::GetEffectiveDisableWriteSessions() const
{
    return DisableWriteSessions_ || DisableWriteSessionsSentToNode_ || DisableWriteSessionsReportedByNode_;
}

void TNode::SetDisableWriteSessions(bool value)
{
    DisableWriteSessions_ = value;
}

void TNode::SetDisableWriteSessionsSentToNode(bool value)
{
    DisableWriteSessionsSentToNode_ = value;
}

void TNode::SetDisableWriteSessionsReportedByNode(bool value)
{
    DisableWriteSessionsReportedByNode_ = value;
}

bool TNode::IsValidWriteTarget() const
{
    // NB: this may be called in mutations so be sure to only rely on persistent state.
    return WasValidWriteTarget(EWriteTargetValidityChange::None);
}

bool TNode::WasValidWriteTarget(EWriteTargetValidityChange reason) const
{
    // NB: this may be called in mutations so be sure to only rely on persistent state.
    auto reportedDataNodeHeartbeat = ReportedDataNodeHeartbeat();
    auto decommissioned = GetDecommissioned();
    auto disableWriteSessions = GetDisableWriteSessions();

    switch (reason) {
        case EWriteTargetValidityChange::None:
            break;

        case EWriteTargetValidityChange::ReportedDataNodeHeartbeat:
            reportedDataNodeHeartbeat = !reportedDataNodeHeartbeat;
            break;

        case EWriteTargetValidityChange::Decommissioned:
            decommissioned = !decommissioned;
            break;

        case EWriteTargetValidityChange::WriteSessionsDisabled:
            disableWriteSessions = !disableWriteSessions;
            break;

        default:
            YT_ABORT();
    }

    return
        reportedDataNodeHeartbeat &&
        !decommissioned &&
        !disableWriteSessions;
}

void TNode::SetNodeTags(const std::vector<TString>& tags)
{
    ValidateNodeTags(tags);
    NodeTags_ = tags;
    RebuildTags();
}

void TNode::SetUserTags(const std::vector<TString>& tags)
{
    ValidateNodeTags(tags);
    UserTags_ = tags;
    RebuildTags();
}

void TNode::RebuildTags()
{
    Tags_.clear();
    Tags_.insert(UserTags_.begin(), UserTags_.end());
    Tags_.insert(NodeTags_.begin(), NodeTags_.end());
    Tags_.insert(TString(GetServiceHostName(GetDefaultAddress())));
    if (auto* rack = GetRack()) {
        Tags_.insert(rack->GetName());
    }
    if (auto* dataCenter = GetDataCenter()) {
        Tags_.insert(dataCenter->GetName());
    }
    if (auto* host = GetHost()) {
        Tags_.insert(host->GetName());
    }
}

void TNode::SetResourceUsage(const NNodeTrackerClient::NProto::TNodeResources& resourceUsage)
{
    ResourceUsage_ = resourceUsage;
}

void TNode::SetResourceLimits(const NNodeTrackerClient::NProto::TNodeResources& resourceLimits)
{
    ResourceLimits_ = resourceLimits;
}

void TNode::InitCellars()
{
    YT_VERIFY(Cellars_.empty());

    for (auto cellarType : TEnumTraits<ECellarType>::GetDomainValues()) {
        if (int size = GetTotalSlotCount(cellarType); size > 0) {
            Cellars_[cellarType].resize(size);
        }
    }
}

void TNode::ClearCellars()
{
    Cellars_.clear();
}

void TNode::UpdateCellarSize(ECellarType cellarType, int newSize)
{
    if (newSize == 0) {
        Cellars_.erase(cellarType);
    } else {
        Cellars_[cellarType].resize(newSize);
    }
}

TNode::TCellar* TNode::FindCellar(ECellarType cellarType)
{
    if (auto it = Cellars_.find(cellarType)) {
        return &it->second;
    }
    return nullptr;
}

const TNode::TCellar* TNode::FindCellar(ECellarType cellarType) const
{
    if (auto it = Cellars_.find(cellarType)) {
        return &it->second;
    }
    return nullptr;
}

TNode::TCellar& TNode::GetCellar(ECellarType cellarType)
{
    auto* cellar = FindCellar(cellarType);
    YT_VERIFY(cellar);
    return *cellar;
}

const TNode::TCellar& TNode::GetCellar(ECellarType cellarType) const
{
    auto* cellar = FindCellar(cellarType);
    YT_VERIFY(cellar);
    return *cellar;
}

int TNode::GetCellarSize(ECellarType cellarType) const
{
    if (auto it = Cellars_.find(cellarType)) {
        return it->second.size();
    }
    return 0;
}

void TNode::SetCellarNodeStatistics(
    ECellarType cellarType,
    TCellarNodeStatistics&& statistics)
{
    CellarNodeStatistics_[cellarType].Swap(&statistics);
}

void TNode::RemoveCellarNodeStatistics(ECellarType cellarType)
{
    CellarNodeStatistics_.erase(cellarType);
}

int TNode::GetAvailableSlotCount(ECellarType cellarType) const
{
    if (const auto& it = CellarNodeStatistics_.find(cellarType)) {
        return it->second.available_cell_slots();
    }

    return 0;
}

int TNode::GetTotalSlotCount(ECellarType cellarType) const
{
    if (const auto& it = CellarNodeStatistics_.find(cellarType)) {
        return
            it->second.used_cell_slots() +
            it->second.available_cell_slots();
    }

    return 0;
}

TCellNodeStatistics TNode::ComputeCellStatistics() const
{
    TCellNodeStatistics result = TCellNodeStatistics();
    for (const auto& [mediumIndex, replicas] :  Replicas_) {
        result.ChunkReplicaCount[mediumIndex] = replicas.size();
    }
    result.DestroyedChunkReplicaCount = std::ssize(DestroyedReplicas_);
    for (const auto& queue : ChunkPushReplicationQueues_) {
        result.ChunkPushReplicationQueuesSize += std::ssize(queue);
    }
    for (const auto& queue : ChunkPullReplicationQueues_) {
        result.ChunkPullReplicationQueuesSize += std::ssize(queue);
    }
    result.PullReplicationChunkCount += std::ssize(ChunksBeingPulled_);
    return result;
}

TCellNodeStatistics TNode::ComputeClusterStatistics() const
{
    // Local (primary) cell statistics aren't stored in MulticellStatistics_.
    TCellNodeStatistics result = ComputeCellStatistics();

    for (const auto& [cellTag, descriptor] : MulticellDescriptors_) {
        result += descriptor.Statistics;
    }
    return result;
}

void TNode::ClearCellStatistics()
{
    for (auto& [_, descriptor] : MulticellDescriptors_) {
        descriptor.Statistics = TCellNodeStatistics();
    }
}

void TNode::AdvanceDestroyedReplicasIterator()
{
    YT_VERIFY(!DestroyedReplicas_.empty() && DestroyedReplicasIterator_ != DestroyedReplicas_.end());
    ++DestroyedReplicasIterator_;
    if (DestroyedReplicasIterator_ == DestroyedReplicas_.end()) {
        DestroyedReplicasIterator_ = DestroyedReplicas_.begin();
    }
}

void TNode::ResetDestroyedReplicasIterator()
{
    DestroyedReplicasIterator_ = DestroyedReplicas_.begin();
}

bool TNode::TCellSlot::IsWarmedUp() const
{
    return
        PreloadPendingStoreCount == 0 &&
        PreloadFailedStoreCount == 0 &&
        (PeerState == EPeerState::Leading || PeerState == EPeerState::Following);
}

////////////////////////////////////////////////////////////////////////////////

void TNodePtrAddressFormatter::operator()(TStringBuilderBase* builder, TNode* node) const
{
    builder->AppendString(node->GetDefaultAddress());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNodeTrackerServer
