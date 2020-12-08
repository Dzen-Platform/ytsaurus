#include "chunk_replicator.h"
#include "private.h"
#include "chunk.h"
#include "chunk_list.h"
#include "chunk_owner_base.h"
#include "chunk_placement.h"
#include "chunk_tree_traverser.h"
#include "chunk_view.h"
#include "job.h"
#include "job_tracker.h"
#include "chunk_scanner.h"
#include "chunk_replica.h"
#include "medium.h"

#include <yt/server/master/cell_master/bootstrap.h>
#include <yt/server/master/cell_master/config.h>
#include <yt/server/master/cell_master/config_manager.h>
#include <yt/server/master/cell_master/hydra_facade.h>
#include <yt/server/master/cell_master/world_initializer.h>
#include <yt/server/master/cell_master/multicell_manager.h>
#include <yt/server/master/cell_master/proto/multicell_manager.pb.h>

#include <yt/server/master/chunk_server/chunk_manager.h>

#include <yt/server/master/cypress_server/node.h>
#include <yt/server/master/cypress_server/cypress_manager.h>

#include <yt/server/master/node_tracker_server/data_center.h>
#include <yt/server/master/node_tracker_server/node.h>
#include <yt/server/master/node_tracker_server/node_directory_builder.h>
#include <yt/server/master/node_tracker_server/node_tracker.h>
#include <yt/server/master/node_tracker_server/rack.h>

#include <yt/server/master/object_server/object.h>

#include <yt/server/master/security_server/account.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>

#include <yt/ytlib/node_tracker_client/helpers.h>
#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/client/object_client/helpers.h>

#include <yt/library/erasure/codec.h>

#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/serialize.h>
#include <yt/core/misc/small_vector.h>
#include <yt/core/misc/string.h>

#include <yt/core/profiling/profiler.h>
#include <yt/core/profiling/timing.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/ytree/ypath_proxy.h>

#include <array>
#include <yt/core/profiling/timing.h>

namespace NYT::NChunkServer {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NHydra;
using namespace NObjectClient;
using namespace NProfiling;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NNodeTrackerServer;
using namespace NObjectServer;
using namespace NChunkServer::NProto;
using namespace NCellMaster;
using namespace NTransactionClient;

using NChunkClient::TLegacyReadLimit;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

TChunkReplicator::TPerMediumChunkStatistics::TPerMediumChunkStatistics()
    : Status(EChunkStatus::None)
    , ReplicaCount{}
    , DecommissionedReplicaCount{}
{ }

////////////////////////////////////////////////////////////////////////////////

TChunkReplicator::TChunkReplicator(
    TChunkManagerConfigPtr config,
    TBootstrap* bootstrap,
    TChunkPlacementPtr chunkPlacement,
    TJobTrackerPtr jobTracker)
    : Config_(config)
    , Bootstrap_(bootstrap)
    , ChunkPlacement_(std::move(chunkPlacement))
    , JobTracker_(std::move(jobTracker))
    , RefreshExecutor_(New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::ChunkMaintenance),
        BIND(&TChunkReplicator::OnRefresh, MakeWeak(this))))
    , BlobRefreshScanner_(std::make_unique<TChunkScanner>(
        Bootstrap_->GetObjectManager(),
        EChunkScanKind::Refresh,
        false /*journal*/))
    , JournalRefreshScanner_(std::make_unique<TChunkScanner>(
        Bootstrap_->GetObjectManager(),
        EChunkScanKind::Refresh,
        true /*journal*/))
    , RequisitionUpdateExecutor_(New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::ChunkMaintenance),
        BIND(&TChunkReplicator::OnRequisitionUpdate, MakeWeak(this))))
    , BlobRequisitionUpdateScanner_(std::make_unique<TChunkScanner>(
        Bootstrap_->GetObjectManager(),
        EChunkScanKind::RequisitionUpdate,
        false /*journal*/))
    , JournalRequisitionUpdateScanner_(std::make_unique<TChunkScanner>(
        Bootstrap_->GetObjectManager(),
        EChunkScanKind::RequisitionUpdate,
        true /*journal*/))
    , FinishedRequisitionTraverseFlushExecutor_(New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::ChunkMaintenance),
        BIND(&TChunkReplicator::OnFinishedRequisitionTraverseFlush, MakeWeak(this))))
    , MissingPartChunkRepairQueueBalancer_(
        Config_->RepairQueueBalancerWeightDecayFactor,
        Config_->RepairQueueBalancerWeightDecayInterval)
    , DecommissionedPartChunkRepairQueueBalancer_(
        Config_->RepairQueueBalancerWeightDecayFactor,
        Config_->RepairQueueBalancerWeightDecayInterval)
    , EnabledCheckExecutor_(New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Periodic),
        BIND(&TChunkReplicator::OnCheckEnabled, MakeWeak(this)),
        Config_->ReplicatorEnabledCheckPeriod))
{
    YT_VERIFY(Config_);
    YT_VERIFY(Bootstrap_);
    YT_VERIFY(ChunkPlacement_);
    YT_VERIFY(JobTracker_);

    for (int i = 0; i < MaxMediumCount; ++i) {
        // We "balance" medium indexes, not the repair queues themselves.
        MissingPartChunkRepairQueueBalancer_.AddContender(i);
        DecommissionedPartChunkRepairQueueBalancer_.AddContender(i);
    }

    for (auto [_, node] : Bootstrap_->GetNodeTracker()->Nodes()) {
        if (node->GetLocalState() != ENodeState::Online) {
            continue;
        }
        for (const auto& replica : node->DestroyedReplicas()) {
            node->AddToChunkRemovalQueue(replica);
        }
    }
}

TChunkReplicator::~TChunkReplicator()
{ }

void TChunkReplicator::Start(
    TChunk* blobFrontChunk,
    int blobChunkCount,
    TChunk* journalFrontChunk,
    int journalChunkCount)
{
    const auto& objectManager = Bootstrap_->GetObjectManager();
    auto epoch = objectManager->GetCurrentEpoch();
    OldestPartMissingChunks_ = TOldestPartMissingChunkSet(TChunkPartLossTimeComparer(epoch));

    BlobRefreshScanner_->Start(blobFrontChunk, blobChunkCount);
    JournalRefreshScanner_->Start(journalFrontChunk, journalChunkCount);
    BlobRequisitionUpdateScanner_->Start(blobFrontChunk, blobChunkCount);
    JournalRequisitionUpdateScanner_->Start(journalFrontChunk, journalChunkCount);
    RefreshExecutor_->Start();
    RequisitionUpdateExecutor_->Start();
    FinishedRequisitionTraverseFlushExecutor_->Start();
    EnabledCheckExecutor_->Start();

    const auto& configManager = Bootstrap_->GetConfigManager();
    configManager->SubscribeConfigChanged(DynamicConfigChangedCallback_);
    OnDynamicConfigChanged();
}

void TChunkReplicator::Stop()
{
    const auto& configManager = Bootstrap_->GetConfigManager();
    configManager->UnsubscribeConfigChanged(DynamicConfigChangedCallback_);

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    const auto& nodeTracker = Bootstrap_->GetNodeTracker();
    for (const auto& nodePair : nodeTracker->Nodes()) {
        const auto* node = nodePair.second;
        for (const auto& jobPair : node->IdToJob()) {
            const auto& job = jobPair.second;
            auto* chunk = chunkManager->FindChunk(job->GetChunkIdWithIndexes().Id);
            if (chunk) {
                chunk->SetJob(nullptr);
            }
        }
    }

    for (const auto& queue : MissingPartChunkRepairQueues_) {
        for (auto chunkWithIndexes : queue) {
            chunkWithIndexes.GetPtr()->SetRepairQueueIterator(
                chunkWithIndexes.GetMediumIndex(),
                EChunkRepairQueue::Missing,
                TChunkRepairQueueIterator());
        }
    }
    MissingPartChunkRepairQueueBalancer_.ResetWeights();

    for (const auto& queue : DecommissionedPartChunkRepairQueues_) {
        for (auto chunkWithIndexes : queue) {
            chunkWithIndexes.GetPtr()->SetRepairQueueIterator(
                chunkWithIndexes.GetMediumIndex(),
                EChunkRepairQueue::Decommissioned,
                TChunkRepairQueueIterator());
        }
    }
    DecommissionedPartChunkRepairQueueBalancer_.ResetWeights();
}

void TChunkReplicator::TouchChunk(TChunk* chunk)
{
    const auto replication = GetChunkAggregatedReplication(chunk);

    for (auto& entry : replication) {
        auto mediumIndex = entry.GetMediumIndex();
        for (auto queue : TEnumTraits<EChunkRepairQueue>::GetDomainValues()) {
            auto repairIt = chunk->GetRepairQueueIterator(mediumIndex, queue);
            if (repairIt == TChunkRepairQueueIterator()) {
                continue;
            }
            auto& chunkRepairQueue = ChunkRepairQueue(mediumIndex, queue);
            chunkRepairQueue.erase(repairIt);
            TChunkPtrWithIndexes chunkWithIndexes(chunk, GenericChunkReplicaIndex, mediumIndex);
            auto newRepairIt = chunkRepairQueue.insert(chunkRepairQueue.begin(), chunkWithIndexes);
            chunk->SetRepairQueueIterator(mediumIndex, queue, newRepairIt);
        }
    }
}

TMediumMap<EChunkStatus> TChunkReplicator::ComputeChunkStatuses(TChunk* chunk)
{
    TMediumMap<EChunkStatus> result;

    auto statistics = ComputeChunkStatistics(chunk);

    for (const auto& [mediumIndex, mediumStatistics] : statistics.PerMediumStatistics) {
        result[mediumIndex] = mediumStatistics.Status;
    }

    return result;
}

TChunkReplicator::TChunkStatistics TChunkReplicator::ComputeChunkStatistics(const TChunk* chunk)
{
    auto result = chunk->IsErasure()
        ? ComputeErasureChunkStatistics(chunk)
        : ComputeRegularChunkStatistics(chunk);

    if (chunk->IsJournal() && chunk->IsSealed()) {
        result.Status |= ECrossMediumChunkStatus::Sealed;
    }

    return result;
}

TChunkReplicator::TChunkStatistics TChunkReplicator::ComputeErasureChunkStatistics(const TChunk* chunk)
{
    TChunkStatistics result;

    auto* codec = NErasure::GetCodec(chunk->GetErasureCodec());

    TMediumMap<std::array<TNodePtrWithIndexesList, ChunkReplicaIndexBound>> decommissionedReplicas;
    TMediumMap<std::array<ui8, RackIndexBound>> perRackReplicaCounters;
    // An arbitrary replica collocated with too may others within a single rack - per medium.
    TMediumIntMap unsafelyPlacedSealedReplicaIndexes;

    TMediumIntMap totalReplicaCounts;
    TMediumIntMap totalDecommissionedReplicaCounts;

    NErasure::TPartIndexSet replicaIndexes;

    bool totallySealed = chunk->IsSealed();

    auto mark = TNode::GenerateVisitMark();

    const auto chunkReplication = GetChunkAggregatedReplication(chunk);
    for (const auto& entry : chunkReplication) {
        auto mediumIndex = entry.GetMediumIndex();
        unsafelyPlacedSealedReplicaIndexes[mediumIndex] = -1;
        totalReplicaCounts[mediumIndex] = 0;
        totalDecommissionedReplicaCounts[mediumIndex] = 0;
    }

    for (auto replica : chunk->StoredReplicas()) {
        auto* node = replica.GetPtr();
        int replicaIndex = replica.GetReplicaIndex();
        int mediumIndex = replica.GetMediumIndex();
        auto& mediumStatistics = result.PerMediumStatistics[mediumIndex];

        replicaIndexes.set(replicaIndex);

        if (chunk->IsJournal() && replica.GetState() != EChunkReplicaState::Sealed) {
            totallySealed = false;
        }

        if (IsReplicaDecommissioned(replica) || node->GetVisitMark(mediumIndex) == mark) {
            ++mediumStatistics.DecommissionedReplicaCount[replicaIndex];
            decommissionedReplicas[mediumIndex][replicaIndex].push_back(replica);
            ++totalDecommissionedReplicaCounts[mediumIndex];
        } else {
            ++mediumStatistics.ReplicaCount[replicaIndex];
            ++totalReplicaCounts[mediumIndex];
        }

        if (!Config_->AllowMultipleErasurePartsPerNode) {
            node->SetVisitMark(mediumIndex, mark);
        }

        if (const auto* rack = node->GetRack()) {
            int rackIndex = rack->GetIndex();
            int maxReplicasPerRack = ChunkPlacement_->GetMaxReplicasPerRack(mediumIndex, chunk);
            if (++perRackReplicaCounters[mediumIndex][rackIndex] > maxReplicasPerRack) {
                // An erasure chunk is considered placed unsafely if some non-null rack
                // contains more replicas than returned by TChunk::GetMaxReplicasPerRack.
                if (!chunk->IsJournal() || replica.GetState() == EChunkReplicaState::Sealed) {
                    unsafelyPlacedSealedReplicaIndexes[mediumIndex] = replicaIndex;
                }
            }
        }
    }

    bool allMediaTransient = true;
    bool allMediaDataPartsOnly = true;
    TMediumMap<NErasure::TPartIndexSet> mediumToErasedIndexes;
    TMediumSet activeMedia;

    const auto& chunkManager = Bootstrap_->GetChunkManager();

    for (const auto& entry : chunkReplication) {
        auto mediumIndex = entry.GetMediumIndex();
        auto* medium = chunkManager->FindMediumByIndex(mediumIndex);
        YT_VERIFY(IsObjectAlive(medium));

        if (medium->GetCache()) {
            continue;
        }

        auto mediumTransient = medium->GetTransient();

        auto replicationPolicy = entry.Policy();

        auto dataPartsOnly = replicationPolicy.GetDataPartsOnly();
        auto mediumReplicationFactor = replicationPolicy.GetReplicationFactor();

        if (mediumReplicationFactor == 0 &&
            totalReplicaCounts[mediumIndex] == 0 &&
            totalDecommissionedReplicaCounts[mediumIndex] == 0)
        {
            // This medium is irrelevant to this chunk.
            continue;
        }

        allMediaTransient = allMediaTransient && mediumTransient;
        allMediaDataPartsOnly = allMediaDataPartsOnly && dataPartsOnly;

        activeMedia.set(mediumIndex);

        ComputeErasureChunkStatisticsForMedium(
            result.PerMediumStatistics[mediumIndex],
            chunk,
            codec,
            replicationPolicy,
            decommissionedReplicas[mediumIndex],
            unsafelyPlacedSealedReplicaIndexes[mediumIndex],
            mediumToErasedIndexes[mediumIndex],
            totallySealed);
    }

    ComputeErasureChunkStatisticsCrossMedia(
        result,
        chunk,
        codec,
        allMediaTransient,
        allMediaDataPartsOnly,
        mediumToErasedIndexes,
        activeMedia,
        replicaIndexes,
        totallySealed);

    return result;
}

void TChunkReplicator::ComputeErasureChunkStatisticsForMedium(
    TPerMediumChunkStatistics& result,
    const TChunk* chunk,
    NErasure::ICodec* codec,
    TReplicationPolicy replicationPolicy,
    std::array<TNodePtrWithIndexesList, ChunkReplicaIndexBound>& decommissionedReplicas,
    int unsafelyPlacedSealedReplicaIndex,
    NErasure::TPartIndexSet& erasedIndexes,
    bool totallySealed)
{
    int replicationFactor = replicationPolicy.GetReplicationFactor();
    YT_VERIFY(0 <= replicationFactor && replicationFactor <= 1);

    int totalPartCount = codec->GetTotalPartCount();
    int dataPartCount = codec->GetDataPartCount();

    for (int index = 0; index < totalPartCount; ++index) {
        int replicaCount = result.ReplicaCount[index];
        int decommissionedReplicaCount = result.DecommissionedReplicaCount[index];
        auto isDataPart = index < dataPartCount;
        auto removalAdvised = replicationFactor == 0 || (!isDataPart && replicationPolicy.GetDataPartsOnly());
        auto targetReplicationFactor = removalAdvised ? 0 : 1;

        if (totallySealed) {
            if (replicaCount >= targetReplicationFactor && decommissionedReplicaCount > 0) {
                result.Status |= EChunkStatus::Overreplicated;
                const auto& replicas = decommissionedReplicas[index];
                result.DecommissionedRemovalReplicas.append(replicas.begin(), replicas.end());
            }

            if (replicaCount > targetReplicationFactor && decommissionedReplicaCount == 0) {
                result.Status |= EChunkStatus::Overreplicated;
                result.BalancingRemovalIndexes.push_back(index);
            }

            if (replicaCount == 0 && decommissionedReplicaCount > 0 && !removalAdvised && decommissionedReplicas.size() > index) {
                const auto& replicas = decommissionedReplicas[index];
                // A replica may be "decommissioned" either because it's node is
                // decommissioned or that node holds another part of the chunk (and that's
                // not allowed by the configuration). Let's distinguish these cases.
                auto isReplicaDecommissioned = [&] (const TNodePtrWithIndexes& replica) {
                    return IsReplicaDecommissioned(replica);
                };
                if (std::all_of(replicas.begin(), replicas.end(), isReplicaDecommissioned)) {
                    result.Status |= (isDataPart ? EChunkStatus::DataDecommissioned : EChunkStatus::ParityDecommissioned);
                } else {
                    result.Status |= EChunkStatus::Underreplicated;
                    result.ReplicationIndexes.push_back(index);
                }
            }
        }

        if (replicaCount == 0 && decommissionedReplicaCount == 0 && !removalAdvised) {
            erasedIndexes.set(index);
            result.Status |= isDataPart ? EChunkStatus::DataMissing : EChunkStatus::ParityMissing;
        }
    }

    if (!codec->CanRepair(erasedIndexes) &&
        erasedIndexes.any()) // This is to avoid flagging chunks with no parity
                             // parts as lost when dataPartsOnly == true.
    {
        result.Status |= EChunkStatus::Lost;
    }

    if (unsafelyPlacedSealedReplicaIndex != -1 &&
        None(result.Status & EChunkStatus::Overreplicated))
    {
        result.Status |= EChunkStatus::UnsafelyPlaced;
        if (result.ReplicationIndexes.empty()) {
            result.ReplicationIndexes.push_back(unsafelyPlacedSealedReplicaIndex);
        }
    }
}

void TChunkReplicator::ComputeErasureChunkStatisticsCrossMedia(
    TChunkStatistics& result,
    const TChunk* chunk,
    NErasure::ICodec* codec,
    bool allMediaTransient,
    bool allMediaDataPartsOnly,
    const TMediumMap<NErasure::TPartIndexSet>& mediumToErasedIndexes,
    const TMediumSet& activeMedia,
    const NErasure::TPartIndexSet& replicaIndexes,
    bool totallySealed)
{
    if (!chunk->IsSealed() && replicaIndexes.count() < chunk->GetReadQuorum()) {
        result.Status |= ECrossMediumChunkStatus::QuorumMissing;
    }

    // In contrast to regular chunks, erasure chunk being "lost" on every medium
    // doesn't mean it's lost for good: across all media, there still may be
    // enough parts to make it repairable.

    std::bitset<MaxMediumCount> transientMedia;
    if (allMediaTransient) {
        transientMedia.flip();
    } else {
        for (const auto& mediumIdAndPtrPair : Bootstrap_->GetChunkManager()->Media()) {
            auto* medium = mediumIdAndPtrPair.second;
            if (medium->GetCache()) {
                continue;
            }

            transientMedia.set(medium->GetIndex(), medium->GetTransient());
        }
    }

    NErasure::TPartIndexSet crossMediumErasedIndexes;
    // Erased indexes as they would look if all transient media were to disappear.
    NErasure::TPartIndexSet crossMediumErasedIndexesNoTransient;
    crossMediumErasedIndexes.flip();
    crossMediumErasedIndexesNoTransient.flip();

    static const NErasure::TPartIndexSet emptySet;

    auto deficient = false;
    for (int mediumIndex = 0; mediumIndex < MaxMediumCount; ++mediumIndex) {
        if (!activeMedia[mediumIndex]) {
            continue;
        }
        auto it = mediumToErasedIndexes.find(mediumIndex);
        const auto& erasedIndexes = it == mediumToErasedIndexes.end() ? emptySet : it->second;
        crossMediumErasedIndexes &= erasedIndexes;
        if (!transientMedia.test(mediumIndex)) {
            crossMediumErasedIndexesNoTransient &= erasedIndexes;
        }

        const auto& mediumStatistics = result.PerMediumStatistics[mediumIndex];
        if (Any(mediumStatistics.Status &
                (EChunkStatus::DataMissing | EChunkStatus::ParityMissing |
                 EChunkStatus::DataDecommissioned | EChunkStatus::ParityDecommissioned)))
        {
            deficient = true;
        }
    }

    int totalPartCount = codec->GetTotalPartCount();
    int dataPartCount = codec->GetDataPartCount();

    bool crossMediaDataMissing = false;
    bool crossMediaParityMissing = false;
    bool precarious = false;
    bool crossMediaLost = false;

    if (crossMediumErasedIndexes.any()) {
        for (int index = 0; index < dataPartCount; ++index) {
            if (crossMediumErasedIndexes.test(index)) {
                crossMediaDataMissing = true;
                break;
            }
        }
        for (int index = dataPartCount; index < totalPartCount; ++index) {
            if (crossMediumErasedIndexes.test(index)) {
                crossMediaParityMissing = true;
                break;
            }
        }

        crossMediaLost = !codec->CanRepair(crossMediumErasedIndexes);
    }

    if (!crossMediaLost && crossMediumErasedIndexesNoTransient.any()) {
        precarious = !codec->CanRepair(crossMediumErasedIndexesNoTransient);
    }

    if (crossMediaLost) {
        result.Status |= ECrossMediumChunkStatus::Lost;
    } else {
        for (const auto& mediumStatistics : result.PerMediumStatistics) {
            if (Any(mediumStatistics.second.Status & EChunkStatus::Lost)) {
                // The chunk is lost on at least one medium.
                result.Status |= ECrossMediumChunkStatus::MediumWiseLost;
                break;
            }
        }
    }

    if (deficient && None(result.Status & ECrossMediumChunkStatus::MediumWiseLost)) {
        result.Status |= ECrossMediumChunkStatus::Deficient;
    }
    if (crossMediaDataMissing) {
        result.Status |= ECrossMediumChunkStatus::DataMissing;
    }
    if (crossMediaParityMissing && !allMediaDataPartsOnly) {
        result.Status |= ECrossMediumChunkStatus::ParityMissing;
    }
    if (precarious && !allMediaTransient) {
        result.Status |= ECrossMediumChunkStatus::Precarious;
    }

    if (totallySealed) {
        // Replicate parts cross-media. Do this even if the chunk is unrepairable:
        // having identical states on all media is just simpler to reason about.
        for (const auto& [mediumIndex, erasedIndexes] : mediumToErasedIndexes) {
            auto& mediumStatistics = result.PerMediumStatistics[mediumIndex];

            for (int index = 0; index < totalPartCount; ++index) {
                if (erasedIndexes.test(index) && // If dataPartsOnly is true, everything beyond dataPartCount will test negative.
                    !crossMediumErasedIndexes.test(index))
                {
                    mediumStatistics.Status |= EChunkStatus::Underreplicated;
                    mediumStatistics.ReplicationIndexes.push_back(index);
                }
            }
        }
    }
}

TChunkReplicator::TChunkStatistics TChunkReplicator::ComputeRegularChunkStatistics(const TChunk* chunk)
{
    TChunkStatistics results;

    TMediumSet hasUnsafelyPlacedReplica;
    TMediumMap<std::array<ui8, RackIndexBound>> perRackReplicaCounters;

    TMediumIntMap replicaCount;
    TMediumIntMap decommissionedReplicaCount;
    TMediumMap<TNodePtrWithIndexesList> decommissionedReplicas;
    int totalReplicaCount = 0;
    int totalDecommissionedReplicaCount = 0;

    TMediumSet hasSealedReplica;
    bool hasSealedReplicas = false;
    bool totallySealed = chunk->IsSealed();

    for (auto replica : chunk->StoredReplicas()) {
        auto mediumIndex = replica.GetMediumIndex();

        if (chunk->IsJournal() && replica.GetState() != EChunkReplicaState::Sealed) {
            totallySealed = false;
        } else {
            hasSealedReplica[mediumIndex] = true;
            hasSealedReplicas = true;
        }

        if (IsReplicaDecommissioned(replica)) {
            ++decommissionedReplicaCount[mediumIndex];
            decommissionedReplicas[mediumIndex].push_back(replica);
            ++totalDecommissionedReplicaCount;
        } else {
            ++replicaCount[mediumIndex];
            ++totalReplicaCount;
        }

        if (const auto* rack = replica.GetPtr()->GetRack()) {
            int rackIndex = rack->GetIndex();
            int maxReplicasPerRack = ChunkPlacement_->GetMaxReplicasPerRack(mediumIndex, chunk, std::nullopt);
            if (++perRackReplicaCounters[mediumIndex][rackIndex] > maxReplicasPerRack) {
                hasUnsafelyPlacedReplica[mediumIndex] = true;
            }
        }
    }

    bool precarious = true;
    bool allMediaTransient = true;
    SmallVector<int, MaxMediumCount> mediaOnWhichLost;
    bool hasMediumOnWhichPresent = false;
    bool hasMediumOnWhichUnderreplicated = false;
    bool hasMediumOnWhichSealedMissing = false;

    const auto& chunkManager = Bootstrap_->GetChunkManager();

    auto replication = GetChunkAggregatedReplication(chunk);
    for (auto entry : replication) {
        auto mediumIndex = entry.GetMediumIndex();
        auto* medium = chunkManager->FindMediumByIndex(mediumIndex);
        YT_VERIFY(IsObjectAlive(medium));

        if (medium->GetCache()) {
            continue;
        }

        auto& mediumStatistics = results.PerMediumStatistics[mediumIndex];
        auto mediumTransient = medium->GetTransient();

        auto mediumReplicationPolicy = entry.Policy();
        auto mediumReplicaCount = replicaCount[mediumIndex];
        auto mediumDecommissionedReplicaCount = decommissionedReplicaCount[mediumIndex];

        // NB: some very counter-intuitive scenarios are possible here.
        // E.g. mediumReplicationFactor == 0, but mediumReplicaCount != 0.
        // This happens when chunk's requisition changes. One should be careful
        // with one's assumptions.
        if (!mediumReplicationPolicy &&
            mediumReplicaCount == 0 &&
            mediumDecommissionedReplicaCount == 0)
        {
            // This medium is irrelevant to this chunk.
            continue;
        }

        ComputeRegularChunkStatisticsForMedium(
            mediumStatistics,
            chunk,
            mediumReplicationPolicy,
            mediumReplicaCount,
            mediumDecommissionedReplicaCount,
            decommissionedReplicas[mediumIndex],
            hasSealedReplica[mediumIndex],
            totallySealed,
            hasUnsafelyPlacedReplica[mediumIndex]);

        allMediaTransient = allMediaTransient && mediumTransient;

        if (Any(mediumStatistics.Status & EChunkStatus::Underreplicated)) {
            hasMediumOnWhichUnderreplicated = true;
        }

        if (Any(mediumStatistics.Status & EChunkStatus::SealedMissing)) {
            hasMediumOnWhichSealedMissing = true;
        }

        if (Any(mediumStatistics.Status & EChunkStatus::Lost)) {
            mediaOnWhichLost.push_back(mediumIndex);
        } else {
            hasMediumOnWhichPresent = true;
            precarious = precarious && mediumTransient;
        }
    }

    ComputeRegularChunkStatisticsCrossMedia(
        results,
        chunk,
        totalReplicaCount,
        totalDecommissionedReplicaCount,
        hasSealedReplicas,
        precarious,
        allMediaTransient,
        mediaOnWhichLost,
        hasMediumOnWhichPresent,
        hasMediumOnWhichUnderreplicated,
        hasMediumOnWhichSealedMissing);

    return results;
}

void TChunkReplicator::ComputeRegularChunkStatisticsForMedium(
    TPerMediumChunkStatistics& result,
    const TChunk* chunk,
    TReplicationPolicy replicationPolicy,
    int replicaCount,
    int decommissionedReplicaCount,
    const TNodePtrWithIndexesList& decommissionedReplicas,
    bool hasSealedReplica,
    bool totallySealed,
    bool hasUnsafelyPlacedReplica)
{
    auto replicationFactor = replicationPolicy.GetReplicationFactor();

    result.ReplicaCount[GenericChunkReplicaIndex] = replicaCount;
    result.DecommissionedReplicaCount[GenericChunkReplicaIndex] = decommissionedReplicaCount;

    if (replicaCount + decommissionedReplicaCount == 0) {
        result.Status |= EChunkStatus::Lost;
    }

    if (chunk->IsSealed()) {
        if (chunk->IsJournal() && replicationFactor > 0 && !hasSealedReplica) {
            result.Status |= EChunkStatus::SealedMissing;
        }

        if (replicaCount < replicationFactor && hasSealedReplica) {
            result.Status |= EChunkStatus::Underreplicated;
        }

        if (totallySealed) {
            if (decommissionedReplicaCount > 0 && replicaCount + decommissionedReplicaCount > replicationFactor) {
                result.Status |= EChunkStatus::Overreplicated;
                result.DecommissionedRemovalReplicas.append(decommissionedReplicas.begin(), decommissionedReplicas.end());
            } else if (replicaCount > replicationFactor) {
                result.Status |= EChunkStatus::Overreplicated;
                result.BalancingRemovalIndexes.push_back(GenericChunkReplicaIndex);
            }
        }
    }

    if (replicationFactor > 1 && hasUnsafelyPlacedReplica && None(result.Status & EChunkStatus::Overreplicated)) {
        result.Status |= EChunkStatus::UnsafelyPlaced;
    }

    if (hasSealedReplica && Any(result.Status & (EChunkStatus::Underreplicated | EChunkStatus::UnsafelyPlaced))) {
        result.ReplicationIndexes.push_back(GenericChunkReplicaIndex);
    }
}

void TChunkReplicator::ComputeRegularChunkStatisticsCrossMedia(
    TChunkStatistics& result,
    const TChunk* chunk,
    int totalReplicaCount,
    int totalDecommissionedReplicaCount,
    bool hasSealedReplicas,
    bool precarious,
    bool allMediaTransient,
    const SmallVector<int, MaxMediumCount>& mediaOnWhichLost,
    bool hasMediumOnWhichPresent,
    bool hasMediumOnWhichUnderreplicated,
    bool hasMediumOnWhichSealedMissing)
{
    if (chunk->IsJournal() &&
        totalReplicaCount + totalDecommissionedReplicaCount < chunk->GetReadQuorum() &&
        !hasSealedReplicas)
    {
        result.Status |= ECrossMediumChunkStatus::QuorumMissing;
    }

    if (!hasMediumOnWhichPresent) {
        result.Status |= ECrossMediumChunkStatus::Lost;
    }

    if (precarious && !allMediaTransient) {
        result.Status |= ECrossMediumChunkStatus::Precarious;
    }

    if (!mediaOnWhichLost.empty() && hasMediumOnWhichPresent) {
        if (hasSealedReplicas) {
            for (auto mediumIndex : mediaOnWhichLost) {
                auto& mediumStatistics = result.PerMediumStatistics[mediumIndex];
                mediumStatistics.Status |= EChunkStatus::Underreplicated;
                mediumStatistics.ReplicationIndexes.push_back(GenericChunkReplicaIndex);
            }
        }
        result.Status |= ECrossMediumChunkStatus::MediumWiseLost;
    } else if (hasMediumOnWhichUnderreplicated || hasMediumOnWhichSealedMissing) {
        result.Status |= ECrossMediumChunkStatus::Deficient;
    }
}

void TChunkReplicator::OnNodeUnregistered(TNode* node)
{
    auto idToJob = node->IdToJob();
    for (const auto& [jobId, job] : idToJob) {
        YT_LOG_DEBUG("Job canceled (JobId: %v)", job->GetJobId());
        JobTracker_->UnregisterJob(job);
    }
    node->Reset();
}

void TChunkReplicator::OnNodeDisposed(TNode* node)
{
    YT_VERIFY(node->IdToJob().empty());
    YT_VERIFY(node->ChunkSealQueue().empty());
    YT_VERIFY(node->ChunkRemovalQueue().empty());
    for (const auto& queue : node->ChunkReplicationQueues()) {
        YT_VERIFY(queue.empty());
    }
}

void TChunkReplicator::OnChunkDestroyed(TChunk* chunk)
{
    GetChunkRefreshScanner(chunk)->OnChunkDestroyed(chunk);
    GetChunkRequisitionUpdateScanner(chunk)->OnChunkDestroyed(chunk);
    ResetChunkStatus(chunk);
    RemoveChunkFromQueuesOnDestroy(chunk);
    CancelChunkJobs(chunk);
}

void TChunkReplicator::OnReplicaRemoved(
    TNode* node,
    TChunkPtrWithIndexes chunkWithIndexes,
    ERemoveReplicaReason reason)
{
    const auto* chunk = chunkWithIndexes.GetPtr();
    TChunkIdWithIndexes chunkIdWithIndexes(
        chunk->GetId(),
        chunkWithIndexes.GetReplicaIndex(),
        chunkWithIndexes.GetMediumIndex());
    node->RemoveFromChunkReplicationQueues(chunkWithIndexes, AllMediaIndex);
    if (reason != ERemoveReplicaReason::ChunkDestroyed) {
        node->RemoveFromChunkRemovalQueue(chunkIdWithIndexes);
    }
    if (chunk->IsJournal()) {
        node->RemoveFromChunkSealQueue(chunkWithIndexes);
    }
}

void TChunkReplicator::ScheduleUnknownReplicaRemoval(
    TNode* node,
    const TChunkIdWithIndexes& chunkIdWithIndexes)
{
    node->AddToChunkRemovalQueue(chunkIdWithIndexes);
}

void TChunkReplicator::ScheduleReplicaRemoval(
    TNode* node,
    TChunkPtrWithIndexes chunkWithIndexes)
{
    TChunkIdWithIndexes chunkIdWithIndexes(
        chunkWithIndexes.GetPtr()->GetId(),
        chunkWithIndexes.GetReplicaIndex(),
        chunkWithIndexes.GetMediumIndex());
    node->AddToChunkRemovalQueue(chunkIdWithIndexes);
}

bool TChunkReplicator::CreateReplicationJob(
    TNode* sourceNode,
    TChunkPtrWithIndexes chunkWithIndexes,
    TMedium* targetMedium,
    TJobPtr* job)
{
    auto* chunk = chunkWithIndexes.GetPtr();
    auto replicaIndex = chunkWithIndexes.GetReplicaIndex();

    if (!IsObjectAlive(chunk)) {
        return true;
    }

    const auto& objectManager = Bootstrap_->GetObjectManager();
    if (chunk->GetScanFlag(EChunkScanKind::Refresh, objectManager->GetCurrentEpoch())) {
        return true;
    }

    if (chunk->IsJobScheduled()) {
        return true;
    }

    int targetMediumIndex = targetMedium->GetIndex();
    const auto replicationFactor = GetChunkAggregatedReplicationFactor(chunk, targetMediumIndex);

    auto statistics = ComputeChunkStatistics(chunk);
    const auto& mediumStatistics = statistics.PerMediumStatistics[targetMediumIndex];
    int replicaCount = mediumStatistics.ReplicaCount[replicaIndex];

    if (Any(statistics.Status & ECrossMediumChunkStatus::Lost)) {
        return true;
    }

    if (replicaCount > replicationFactor) {
        return true;
    }

    int replicasNeeded;
    if (Any(mediumStatistics.Status & EChunkStatus::Underreplicated)) {
        replicasNeeded = replicationFactor - replicaCount;
    } else if (Any(mediumStatistics.Status & EChunkStatus::UnsafelyPlaced)) {
        replicasNeeded = 1;
    } else {
        return true;
    }

    // TODO(babenko): journal replication currently does not support fan-out > 1
    if (chunk->IsJournal()) {
        replicasNeeded = 1;
    }

    auto targetNodes = ChunkPlacement_->AllocateWriteTargets(
        targetMedium,
        chunk,
        replicasNeeded,
        1,
        std::nullopt,
        JobTracker_->GetUnsaturatedInterDCEdgesStartingFrom(sourceNode->GetDataCenter()),
        ESessionType::Replication);
    if (targetNodes.empty()) {
        return false;
    }

    TNodePtrWithIndexesList targetReplicas;
    for (auto* node : targetNodes) {
        targetReplicas.emplace_back(node, replicaIndex, targetMediumIndex);
    }

    *job = TJob::CreateReplicate(
        JobTracker_->GenerateJobId(),
        chunkWithIndexes,
        sourceNode,
        targetReplicas);

    YT_LOG_DEBUG("Replication job scheduled (JobId: %v, Address: %v, ChunkId: %v, TargetAddresses: %v)",
        (*job)->GetJobId(),
        sourceNode->GetDefaultAddress(),
        chunkWithIndexes,
        MakeFormattableView(targetNodes, TNodePtrAddressFormatter()));

    return targetNodes.size() == replicasNeeded;
}

bool TChunkReplicator::CreateBalancingJob(
    TNode* sourceNode,
    TChunkPtrWithIndexes chunkWithIndexes,
    double maxFillFactor,
    TJobPtr* job)
{
    auto* chunk = chunkWithIndexes.GetPtr();

    const auto& objectManager = Bootstrap_->GetObjectManager();
    if (chunk->GetScanFlag(EChunkScanKind::Refresh, objectManager->GetCurrentEpoch())) {
        return true;
    }

    if (chunk->IsJobScheduled()) {
        return true;
    }

    auto replicaIndex = chunkWithIndexes.GetReplicaIndex();
    auto mediumIndex = chunkWithIndexes.GetMediumIndex();

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    auto* medium = chunkManager->GetMediumByIndex(mediumIndex);

    auto* targetNode = ChunkPlacement_->AllocateBalancingTarget(
        medium,
        chunk,
        maxFillFactor,
        JobTracker_->GetUnsaturatedInterDCEdgesStartingFrom(sourceNode->GetDataCenter()));
    if (!targetNode) {
        return false;
    }

    TNodePtrWithIndexesList targetReplicas{
        TNodePtrWithIndexes(targetNode, replicaIndex, mediumIndex)
    };

    *job = TJob::CreateReplicate(
        JobTracker_->GenerateJobId(),
        chunkWithIndexes,
        sourceNode,
        targetReplicas);

    YT_LOG_DEBUG("Balancing job scheduled (JobId: %v, Address: %v, ChunkId: %v, TargetAddress: %v)",
        (*job)->GetJobId(),
        sourceNode->GetDefaultAddress(),
        chunkWithIndexes,
        targetNode->GetDefaultAddress());

    return true;
}

bool TChunkReplicator::CreateRemovalJob(
    TNode* node,
    const TChunkIdWithIndexes& chunkIdWithIndexes,
    TJobPtr* job)
{
    const auto& chunkManager = Bootstrap_->GetChunkManager();
    const auto& objectManager = Bootstrap_->GetObjectManager();

    auto* chunk = chunkManager->FindChunk(chunkIdWithIndexes.Id);
    // NB: Allow more than one job for dead chunks.
    if (IsObjectAlive(chunk)) {
        if (chunk->GetScanFlag(EChunkScanKind::Refresh, objectManager->GetCurrentEpoch())) {
            return true;
        }
        if (chunk->IsJobScheduled()) {
            return true;
        }
    }

    *job = TJob::CreateRemove(
        JobTracker_->GenerateJobId(),
        chunkIdWithIndexes,
        node);

    YT_LOG_DEBUG("Removal job scheduled (JobId: %v, Address: %v, ChunkId: %v)",
        (*job)->GetJobId(),
        node->GetDefaultAddress(),
        chunkIdWithIndexes);

    return true;
}

bool TChunkReplicator::CreateRepairJob(
    EChunkRepairQueue repairQueue,
    TNode* node,
    TChunkPtrWithIndexes chunkWithIndexes,
    TJobPtr* job)
{
    YT_VERIFY(chunkWithIndexes.GetReplicaIndex() == GenericChunkReplicaIndex);

    auto* chunk = chunkWithIndexes.GetPtr();
    int mediumIndex = chunkWithIndexes.GetMediumIndex();

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    auto* medium = chunkManager->GetMediumByIndex(mediumIndex);

    YT_VERIFY(chunk->IsErasure());

    if (!IsObjectAlive(chunk)) {
        return true;
    }

    const auto& objectManager = Bootstrap_->GetObjectManager();
    if (chunk->GetScanFlag(EChunkScanKind::Refresh, objectManager->GetCurrentEpoch())) {
        return true;
    }

    if (chunk->IsJobScheduled()) {
        return true;
    }

    auto codecId = chunk->GetErasureCodec();
    auto* codec = NErasure::GetCodec(codecId);
    auto totalPartCount = codec->GetTotalPartCount();

    auto statistics = ComputeChunkStatistics(chunk);
    const auto& mediumStatistics = statistics.PerMediumStatistics[mediumIndex];

    NErasure::TPartIndexList erasedPartIndexes;
    for (int index = 0; index < totalPartCount; ++index) {
        if (mediumStatistics.ReplicaCount[index] == 0) {
            erasedPartIndexes.push_back(index);
        }
    }

    if (erasedPartIndexes.empty()) {
        return true;
    }

    if (!codec->CanRepair(erasedPartIndexes)) {
        // Can't repair without decommissioned replicas. Use them.
        auto guaranteedRepairablePartCount = codec->GetGuaranteedRepairablePartCount();
        YT_VERIFY(guaranteedRepairablePartCount < static_cast<int>(erasedPartIndexes.size()));

        // Reorder the parts so that the actually erased ones go first and then the decommissioned ones.
        std::partition(
            erasedPartIndexes.begin(),
            erasedPartIndexes.end(),
            [&] (int index) {
                return mediumStatistics.DecommissionedReplicaCount[index] == 0;
            });

        // Try popping decommissioned replicas as long repair cannot be performed.
        do {
            if (mediumStatistics.DecommissionedReplicaCount[erasedPartIndexes.back()] == 0) {
                YT_LOG_ERROR("Erasure chunk has not enough replicas to repair (ChunkId: %v)",
                    chunk->GetId());
                return false;
            }
            erasedPartIndexes.pop_back();
        } while (!codec->CanRepair(erasedPartIndexes));

        std::sort(erasedPartIndexes.begin(), erasedPartIndexes.end());
    }

    auto erasedPartCount = static_cast<int>(erasedPartIndexes.size());

    auto targetNodes = ChunkPlacement_->AllocateWriteTargets(
        medium,
        chunk,
        erasedPartCount,
        erasedPartCount,
        std::nullopt,
        JobTracker_->GetUnsaturatedInterDCEdgesStartingFrom(node->GetDataCenter()),
        ESessionType::Repair);
    if (targetNodes.empty()) {
        return false;
    }

    YT_VERIFY(targetNodes.size() == erasedPartCount);

    TNodePtrWithIndexesList targetReplicas;
    int targetIndex = 0;
    for (auto* node : targetNodes) {
        targetReplicas.emplace_back(node, erasedPartIndexes[targetIndex++], mediumIndex);
    }

    *job = TJob::CreateRepair(
        JobTracker_->GenerateJobId(),
        chunk,
        node,
        targetReplicas,
        GetDynamicConfig()->RepairJobMemoryUsage,
        repairQueue == EChunkRepairQueue::Decommissioned);

    YT_LOG_DEBUG("Repair job scheduled (JobId: %v, Address: %v, ChunkId: %v, Targets: %v, ErasedPartIndexes: %v)",
        (*job)->GetJobId(),
        node->GetDefaultAddress(),
        chunkWithIndexes,
        MakeFormattableView(targetNodes, TNodePtrAddressFormatter()),
        erasedPartIndexes);

    return true;
}

void TChunkReplicator::ScheduleJobs(
    TNode* node,
    TNodeResources* resourceUsage,
    const TNodeResources& resourceLimits,
    std::vector<TJobPtr>* jobsToStart)
{
    if (JobTracker_->IsOverdraft()) {
        return;
    }

    if (!IsReplicatorEnabled()) {
        return;
    }
    
    const auto* nodeDataCenter = node->GetDataCenter();

    int misscheduledReplicationJobs = 0;
    int misscheduledRepairJobs = 0;
    int misscheduledRemovalJobs = 0;

    // NB: Beware of chunks larger than the limit; we still need to be able to replicate them one by one.
    auto hasSpareReplicationResources = [&] () {
        return
            misscheduledReplicationJobs < GetDynamicConfig()->MaxMisscheduledReplicationJobsPerHeartbeat &&
            resourceUsage->replication_slots() < resourceLimits.replication_slots() &&
            (resourceUsage->replication_slots() == 0 || resourceUsage->replication_data_size() < resourceLimits.replication_data_size());
    };

    // NB: Beware of chunks larger than the limit; we still need to be able to repair them one by one.
    auto hasSpareRepairResources = [&] () {
        return
            misscheduledRepairJobs < GetDynamicConfig()->MaxMisscheduledRepairJobsPerHeartbeat &&
            resourceUsage->repair_slots() < resourceLimits.repair_slots() &&
            (resourceUsage->repair_slots() == 0 || resourceUsage->repair_data_size() < resourceLimits.repair_data_size());
    };

    auto hasSpareRemovalResources = [&] () {
        return
            misscheduledRemovalJobs < GetDynamicConfig()->MaxMisscheduledRemovalJobsPerHeartbeat &&
            resourceUsage->removal_slots() < resourceLimits.removal_slots();
    };

    const auto& chunkManager = Bootstrap_->GetChunkManager();

    // Schedule replication jobs.
    for (auto& queue : node->ChunkReplicationQueues()) {
        auto it = queue.begin();
        while (it != queue.end() &&
            hasSpareReplicationResources() &&
            JobTracker_->HasUnsaturatedInterDCEdgeStartingFrom(nodeDataCenter))
        {
            auto jt = it++;
            auto chunkWithIndexes = jt->first;
            auto& mediumIndexSet = jt->second;
            for (int mediumIndex = 0; mediumIndex < mediumIndexSet.size(); ++mediumIndex) {
                if (mediumIndexSet.test(mediumIndex)) {
                    TJobPtr job;
                    auto* medium = chunkManager->GetMediumByIndex(mediumIndex);
                    if (CreateReplicationJob(node, chunkWithIndexes, medium, &job)) {
                        mediumIndexSet.reset(mediumIndex);
                    } else {
                        ++misscheduledReplicationJobs;
                    }
                    JobTracker_->RegisterJob(std::move(job), jobsToStart, resourceUsage);
                }
            }

            if (mediumIndexSet.none()) {
                queue.erase(jt);
            }
        }
    }

    // Schedule repair jobs.
    // NB: the order of the enum items is crucial! Part-missing chunks must
    // be repaired before part-decommissioned chunks.
    for (auto queue : TEnumTraits<EChunkRepairQueue>::GetDomainValues()) {
        TMediumMap<std::pair<TChunkRepairQueue::iterator, TChunkRepairQueue::iterator>> iteratorPerRepairQueue;
        for (int mediumIndex = 0; mediumIndex < MaxMediumCount; ++mediumIndex) {
            auto& chunkRepairQueue = ChunkRepairQueue(mediumIndex, queue);
            if (!chunkRepairQueue.empty()) {
                iteratorPerRepairQueue[mediumIndex] = std::make_pair(chunkRepairQueue.begin(), chunkRepairQueue.end());
            }
        }

        while (hasSpareRepairResources() &&
            JobTracker_->HasUnsaturatedInterDCEdgeStartingFrom(nodeDataCenter))
        {
            auto winner = ChunkRepairQueueBalancer(queue).TakeWinnerIf(
                [&] (int mediumIndex) {
                    // Don't repair chunks on nodes without relevant medium.
                    // In particular, this avoids repairing non-cloud tables in the cloud.
                    const auto it = iteratorPerRepairQueue.find(mediumIndex);
                    return node->HasMedium(mediumIndex)
                        && it != iteratorPerRepairQueue.end()
                        && it->second.first != it->second.second;
                });

            if (!winner) {
                break; // Nothing to repair on relevant media.
            }

            auto mediumIndex = *winner;
            auto& chunkRepairQueue = ChunkRepairQueue(mediumIndex, queue);
            auto chunkIt = iteratorPerRepairQueue[mediumIndex].first++;
            auto chunkWithIndexes = *chunkIt;
            auto* chunk = chunkWithIndexes.GetPtr();
            TJobPtr job;
            if (CreateRepairJob(queue, node, chunkWithIndexes, &job)) {
                chunk->SetRepairQueueIterator(chunkWithIndexes.GetMediumIndex(), queue, TChunkRepairQueueIterator());
                chunkRepairQueue.erase(chunkIt);
                if (job) {
                    ChunkRepairQueueBalancer(queue).AddWeight(
                        *winner,
                        job->ResourceUsage().repair_data_size() * job->TargetReplicas().size());
                }
            } else {
                ++misscheduledRepairJobs;
            }
            JobTracker_->RegisterJob(std::move(job), jobsToStart, resourceUsage);
        }
    }

    // Schedule removal jobs.
    {
        auto& queue = node->ChunkRemovalQueue();
        auto it = queue.begin();
        while (it != queue.end()) {
            if (!hasSpareRemovalResources()) {
                break;
            }

            auto jt = it++;
            auto chunkIdWithIndex = jt->first;
            auto& mediumIndexSet = jt->second;
            for (int mediumIndex = 0; mediumIndex < mediumIndexSet.size(); ++mediumIndex) {
                if (mediumIndexSet.test(mediumIndex)) {
                    TChunkIdWithIndexes chunkIdWithIndexes(
                        chunkIdWithIndex.Id,
                        chunkIdWithIndex.ReplicaIndex,
                        mediumIndex);
                    TJobPtr job;
                    if (CreateRemovalJob(node, chunkIdWithIndexes, &job)) {
                        mediumIndexSet.reset(mediumIndex);
                    } else {
                        ++misscheduledRemovalJobs;
                    }
                    JobTracker_->RegisterJob(std::move(job), jobsToStart, resourceUsage);
                }
            }
            if (mediumIndexSet.none()) {
                queue.erase(jt);
            }
        }
    }

    // Schedule balancing jobs.
    for (const auto& mediumIdAndPtrPair : Bootstrap_->GetChunkManager()->Media()) {
        auto* medium = mediumIdAndPtrPair.second;
        if (medium->GetCache()) {
            continue;
        }

        auto mediumIndex = medium->GetIndex();
        auto sourceFillFactor = node->GetFillFactor(mediumIndex);
        if (!sourceFillFactor) {
            continue; // No storage of this medium on this node.
        }

        double targetFillFactor = *sourceFillFactor - GetDynamicConfig()->MinChunkBalancingFillFactorDiff;
        if (hasSpareReplicationResources() &&
            *sourceFillFactor > GetDynamicConfig()->MinChunkBalancingFillFactor &&
            JobTracker_->HasUnsaturatedInterDCEdgeStartingFrom(nodeDataCenter) &&
            ChunkPlacement_->HasBalancingTargets(
                JobTracker_->GetUnsaturatedInterDCEdgesStartingFrom(node->GetDataCenter()),
                medium,
                targetFillFactor))
        {
            int maxJobs = std::max(0, resourceLimits.replication_slots() - resourceUsage->replication_slots());
            auto chunksToBalance = ChunkPlacement_->GetBalancingChunks(medium, node, maxJobs);
            for (auto chunkWithIndexes : chunksToBalance) {
                if (!hasSpareReplicationResources()) {
                    break;
                }

                TJobPtr job;
                if (!CreateBalancingJob(node, chunkWithIndexes, targetFillFactor, &job)) {
                    ++misscheduledReplicationJobs;
                }
                JobTracker_->RegisterJob(std::move(job), jobsToStart, resourceUsage);
            }
        }
    }
}

void TChunkReplicator::RefreshChunk(TChunk* chunk)
{
    if (!chunk->IsConfirmed()) {
        return;
    }

    if (chunk->IsForeign()) {
        return;
    }

    const auto replication = GetChunkAggregatedReplication(chunk);

    ResetChunkStatus(chunk);
    RemoveChunkFromQueuesOnRefresh(chunk);

    auto allMediaStatistics = ComputeChunkStatistics(chunk);

    auto durabilityRequired = false;

    const auto& chunkManager = Bootstrap_->GetChunkManager();

    for (auto entry : replication) {
        auto mediumIndex = entry.GetMediumIndex();
        auto* medium = chunkManager->FindMediumByIndex(mediumIndex);
        YT_VERIFY(IsObjectAlive(medium));

        // For now, chunk cache-as-medium support is rudimentary, and replicator
        // ignores chunk cache to preserve original behavior.
        if (medium->GetCache()) {
            continue;
        }

        auto& statistics = allMediaStatistics.PerMediumStatistics[mediumIndex];
        if (statistics.Status == EChunkStatus::None) {
            continue;
        }

        auto replicationFactor = entry.Policy().GetReplicationFactor();
        auto durabilityRequiredOnMedium =
            replication.GetVital() &&
            (chunk->IsErasure() || replicationFactor > 1) &&
            !medium->GetTransient();
        durabilityRequired = durabilityRequired || durabilityRequiredOnMedium;

        if (Any(statistics.Status & EChunkStatus::Overreplicated)) {
            OverreplicatedChunks_.insert(chunk);
        }

        if (Any(statistics.Status & EChunkStatus::Underreplicated)) {
            UnderreplicatedChunks_.insert(chunk);
        }

        if (Any(statistics.Status & EChunkStatus::UnsafelyPlaced)) {
            UnsafelyPlacedChunks_.insert(chunk);
        }

        if (!chunk->IsJobScheduled()) {
            if (Any(statistics.Status & EChunkStatus::Overreplicated) &&
                None(allMediaStatistics.Status & (ECrossMediumChunkStatus::Deficient | ECrossMediumChunkStatus::MediumWiseLost)))
            {
                for (auto nodeWithIndexes : statistics.DecommissionedRemovalReplicas) {
                    auto* node = nodeWithIndexes.GetPtr();
                    if (node->GetLocalState() != ENodeState::Online) {
                        continue;
                    }

                    YT_ASSERT(mediumIndex == nodeWithIndexes.GetMediumIndex());
                    TChunkIdWithIndexes chunkIdWithIndexes(chunk->GetId(), nodeWithIndexes.GetReplicaIndex(), nodeWithIndexes.GetMediumIndex());
                    node->AddToChunkRemovalQueue(chunkIdWithIndexes);
                }

                for (int replicaIndex : statistics.BalancingRemovalIndexes) {
                    TChunkPtrWithIndexes chunkWithIndexes(chunk, replicaIndex, mediumIndex);
                    auto* targetNode = ChunkPlacement_->GetRemovalTarget(chunkWithIndexes);
                    if (!targetNode) {
                        continue;
                    }

                    TChunkIdWithIndexes chunkIdWithIndexes(chunk->GetId(), replicaIndex, mediumIndex);
                    targetNode->AddToChunkRemovalQueue(chunkIdWithIndexes);
                }
            }

            // This check may yield true even for lost chunks when cross-medium replication is in progress.
            if (Any(statistics.Status & (EChunkStatus::Underreplicated | EChunkStatus::UnsafelyPlaced))) {
                for (auto replicaIndex : statistics.ReplicationIndexes) {
                    // Cap replica count minus one against the range [0, ReplicationPriorityCount - 1].
                    int replicaCount = statistics.ReplicaCount[replicaIndex];
                    int priority = std::max(std::min(replicaCount - 1, ReplicationPriorityCount - 1), 0);

                    for (auto replica : chunk->StoredReplicas()) {
                        if (chunk->IsJournal() && replica.GetState() != EChunkReplicaState::Sealed) {
                            continue;
                        }

                        if (replica.GetReplicaIndex() != replicaIndex) {
                            continue;
                        }

                        // If chunk is lost on some media, don't match dst medium with
                        // src medium: we want to be able to do cross-medium replication.
                        bool mediumMatches =
                            Any(allMediaStatistics.Status & ECrossMediumChunkStatus::MediumWiseLost) ||
                            mediumIndex == replica.GetMediumIndex();
                        if (!mediumMatches) {
                            continue;
                        }

                        auto* node = replica.GetPtr();
                        if (node->GetLocalState() != ENodeState::Online) {
                            continue;
                        }

                        TChunkPtrWithIndexes chunkWithIndexes(chunk, replica.GetReplicaIndex(), replica.GetMediumIndex());
                        node->AddToChunkReplicationQueue(chunkWithIndexes, mediumIndex, priority);
                    }
                }
            }

            if (None(statistics.Status & EChunkStatus::Lost) && chunk->IsSealed()) {
                TChunkPtrWithIndexes chunkWithIndexes(chunk, GenericChunkReplicaIndex, mediumIndex);
                if (Any(statistics.Status & (EChunkStatus::DataMissing | EChunkStatus::ParityMissing))) {
                    AddToChunkRepairQueue(chunkWithIndexes, EChunkRepairQueue::Missing);
                } else if (Any(statistics.Status & (EChunkStatus::DataDecommissioned | EChunkStatus::ParityDecommissioned))) {
                    AddToChunkRepairQueue(chunkWithIndexes, EChunkRepairQueue::Decommissioned);
                }
            }
        }
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::Sealed)) {
        YT_ASSERT(chunk->IsJournal());
        for (auto replica : chunk->StoredReplicas()) {
            if (replica.GetState() != EChunkReplicaState::Unsealed) {
                continue;
            }

            auto* node = replica.GetPtr();
            if (node->GetLocalState() != ENodeState::Online) {
                continue;
            }

            TChunkPtrWithIndexes chunkWithIndexes(chunk, replica.GetReplicaIndex(), replica.GetMediumIndex());
            node->AddToChunkSealQueue(chunkWithIndexes);
        }
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::Lost)) {
        YT_VERIFY(LostChunks_.insert(chunk).second);
        if (durabilityRequired) {
            YT_VERIFY(LostVitalChunks_.insert(chunk).second);
        }
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::DataMissing)) {
        YT_ASSERT(chunk->IsErasure());
        YT_VERIFY(DataMissingChunks_.insert(chunk).second);
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::ParityMissing)) {
        YT_ASSERT(chunk->IsErasure());
        YT_VERIFY(ParityMissingChunks_.insert(chunk).second);
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::QuorumMissing)) {
        YT_ASSERT(chunk->IsJournal());
        YT_VERIFY(QuorumMissingChunks_.insert(chunk).second);
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::Precarious)) {
        YT_VERIFY(PrecariousChunks_.insert(chunk).second);
        if (durabilityRequired) {
            YT_VERIFY(PrecariousVitalChunks_.insert(chunk).second);
        }
    }

    const auto& objectManager = Bootstrap_->GetObjectManager();
    auto epoch = objectManager->GetCurrentEpoch();
    if (Any(allMediaStatistics.Status & (ECrossMediumChunkStatus::DataMissing | ECrossMediumChunkStatus::ParityMissing))) {
        if (!chunk->GetPartLossTime(epoch)) {
            chunk->SetPartLossTime(GetCpuInstant(), epoch);
        }
        MaybeRememberPartMissingChunk(chunk);
    } else if (chunk->GetPartLossTime(epoch)) {
        chunk->ResetPartLossTime(epoch);
    }
}

void TChunkReplicator::ResetChunkStatus(TChunk* chunk)
{
    LostChunks_.erase(chunk);
    LostVitalChunks_.erase(chunk);
    PrecariousChunks_.erase(chunk);
    PrecariousVitalChunks_.erase(chunk);

    UnderreplicatedChunks_.erase(chunk);
    OverreplicatedChunks_.erase(chunk);
    UnsafelyPlacedChunks_.erase(chunk);

    if (chunk->IsErasure()) {
        DataMissingChunks_.erase(chunk);
        ParityMissingChunks_.erase(chunk);
        OldestPartMissingChunks_.erase(chunk);
    }

    if (chunk->IsJournal()) {
        QuorumMissingChunks_.erase(chunk);
    }
}

void TChunkReplicator::MaybeRememberPartMissingChunk(TChunk* chunk)
{
    const auto& objectManager = Bootstrap_->GetObjectManager();
    auto epoch = objectManager->GetCurrentEpoch();

    YT_ASSERT(chunk->GetPartLossTime(epoch));

    // A chunk from an earlier epoch couldn't have made it to OldestPartMissingChunks_.
    YT_VERIFY(OldestPartMissingChunks_.empty() || (*OldestPartMissingChunks_.begin())->GetPartLossTime(epoch));

    if (OldestPartMissingChunks_.size() >= GetDynamicConfig()->MaxOldestPartMissingChunks) {
        return;
    }

    if (OldestPartMissingChunks_.empty()) {
        OldestPartMissingChunks_.insert(chunk);
        return;
    }

    auto* mostRecentPartMissingChunk = *OldestPartMissingChunks_.rbegin();
    auto mostRecentPartLossTime = mostRecentPartMissingChunk->GetPartLossTime(epoch);

    if (chunk->GetPartLossTime(epoch) >= mostRecentPartLossTime) {
        return;
    }

    OldestPartMissingChunks_.erase(--OldestPartMissingChunks_.end());
    OldestPartMissingChunks_.insert(chunk);
}

void TChunkReplicator::RemoveChunkFromQueuesOnRefresh(TChunk* chunk)
{
    for (auto replica : chunk->StoredReplicas()) {
        auto* node = replica.GetPtr();

        // Remove from replication queue.
        TChunkPtrWithIndexes chunkWithIndexes(chunk, replica.GetReplicaIndex(), replica.GetMediumIndex());
        node->RemoveFromChunkReplicationQueues(chunkWithIndexes, AllMediaIndex);

        // Remove from removal queue.
        TChunkIdWithIndexes chunkIdWithIndexes(chunk->GetId(), replica.GetReplicaIndex(), replica.GetMediumIndex());
        node->RemoveFromChunkRemovalQueue(chunkIdWithIndexes);
    }

    const auto& requisition = chunk->GetAggregatedRequisition(GetChunkRequisitionRegistry());
    for (const auto& entry : requisition) {
        const auto& mediumIndex = entry.MediumIndex;
        const auto* medium = Bootstrap_->GetChunkManager()->FindMediumByIndex(mediumIndex);
        if (medium->GetCache()) {
            continue;
        }

        // Remove from repair queue.
        TChunkPtrWithIndexes chunkWithIndexes(chunk, GenericChunkReplicaIndex, mediumIndex);
        RemoveFromChunkRepairQueues(chunkWithIndexes);
    }
}

void TChunkReplicator::RemoveChunkFromQueuesOnDestroy(TChunk* chunk)
{
    // Remove chunk from replication and seal queues.
    for (auto replica : chunk->StoredReplicas()) {
        auto* node = replica.GetPtr();
        TChunkPtrWithIndexes chunkWithIndexes(chunk, replica.GetReplicaIndex(), replica.GetMediumIndex());
        // NB: Keep existing removal requests to workaround the following scenario:
        // 1) the last strong reference to a chunk is released while some ephemeral references
        //    remain; the chunk becomes a zombie;
        // 2) a node sends a heartbeat reporting addition of the chunk;
        // 3) master receives the heartbeat and puts the chunk into the removal queue
        //    without (sic!) registering a replica;
        // 4) the last ephemeral reference is dropped, the chunk is being removed;
        //    at this point we must preserve its removal request in the queue.
        node->RemoveFromChunkReplicationQueues(chunkWithIndexes, AllMediaIndex);
        node->RemoveFromChunkSealQueue(chunkWithIndexes);
    }

    // Remove chunk from repair queues.
    if (chunk->IsErasure()) {
        const auto& requisition = chunk->GetAggregatedRequisition(GetChunkRequisitionRegistry());
        for (const auto& entry : requisition) {
            auto mediumIndex = entry.MediumIndex;
            TChunkPtrWithIndexes chunkPtrWithIndexes(chunk, GenericChunkReplicaIndex, mediumIndex);
            RemoveFromChunkRepairQueues(chunkPtrWithIndexes);
        }
    }
}

void TChunkReplicator::CancelChunkJobs(TChunk* chunk)
{
    auto job = chunk->GetJob();
    if (job) {
        YT_LOG_DEBUG("Job canceled (JobId: %v)", job->GetJobId());
        JobTracker_->UnregisterJob(job);
    }
}

bool TChunkReplicator::IsReplicaDecommissioned(TNodePtrWithIndexes replica)
{
    auto* node = replica.GetPtr();
    return node->GetDecommissioned();
}

TChunkReplication TChunkReplicator::GetChunkAggregatedReplication(const TChunk* chunk)
{
    const auto& chunkManager = Bootstrap_->GetChunkManager();
    auto result = chunk->GetAggregatedReplication(GetChunkRequisitionRegistry());
    for (auto& entry : result) {
        YT_VERIFY(entry.Policy());

        auto* medium = chunkManager->FindMediumByIndex(entry.GetMediumIndex());
        YT_VERIFY(IsObjectAlive(medium));
        auto cap = medium->Config()->MaxReplicationFactor;

        entry.Policy().SetReplicationFactor(std::min(cap, entry.Policy().GetReplicationFactor()));
    }

    // A chunk may happen to have replicas stored on a medium it's not supposed
    // to have replicas on. (This is common when chunks are being relocated from
    // one medium to another.) Add corresponding entries to the aggregated
    // replication so that such media aren't overlooked.
    for (auto replica : chunk->StoredReplicas()) {
        auto mediumIndex = replica.GetMediumIndex();
        if (!result.Contains(mediumIndex)) {
            result.Set(mediumIndex, TReplicationPolicy(), false /*eraseEmpty*/);
        }
    }

    return result;
}

int TChunkReplicator::GetChunkAggregatedReplicationFactor(const TChunk* chunk, int mediumIndex)
{
    auto result = chunk->GetAggregatedReplicationFactor(mediumIndex, GetChunkRequisitionRegistry());

    auto* medium = Bootstrap_->GetChunkManager()->FindMediumByIndex(mediumIndex);
    YT_VERIFY(IsObjectAlive(medium));
    auto cap = medium->Config()->MaxReplicationFactor;

    return std::min(cap, result);
}

void TChunkReplicator::ScheduleChunkRefresh(TChunk* chunk)
{
    if (!IsObjectAlive(chunk)) {
        return;
    }

    if (chunk->IsForeign()) {
        return;
    }

    GetChunkRefreshScanner(chunk)->EnqueueChunk(chunk);
}

void TChunkReplicator::ScheduleNodeRefresh(TNode* node)
{
    const auto& chunkManager = Bootstrap_->GetChunkManager();

    for (const auto& [mediumIndex, replicas] : node->Replicas()) {
        const auto* medium = chunkManager->FindMediumByIndex(mediumIndex);
        if (!medium || medium->GetCache()) {
            continue;
        }
        for (auto replica : replicas) {
            ScheduleChunkRefresh(replica.GetPtr());
        }
    }
}

void TChunkReplicator::ScheduleGlobalChunkRefresh(
    TChunk* blobFrontChunk,
    int blobChunkCount,
    TChunk* journalFrontChunk,
    int journalChunkCount)
{
    BlobRefreshScanner_->ScheduleGlobalScan(blobFrontChunk, blobChunkCount);
    JournalRefreshScanner_->ScheduleGlobalScan(journalFrontChunk, journalChunkCount);
}

void TChunkReplicator::OnRefresh()
{
    if (!GetDynamicConfig()->EnableChunkRefresh) {
        YT_LOG_DEBUG("Chunk refresh disabled; see //sys/@config");
        return;
    }

    YT_LOG_DEBUG("Chunk refresh iteration started");

    auto deadline = GetCpuInstant() - DurationToCpuDuration(GetDynamicConfig()->ChunkRefreshDelay);

    auto doRefreshChunks = [&] (
        const std::unique_ptr<TChunkScanner>& scanner,
        int* const totalCount,
        int* const aliveCount,
        int maxChunksPerRefresh,
        TDuration maxTimePerRefresh)
    {
        NProfiling::TWallTimer timer;

        while (*totalCount < maxChunksPerRefresh && scanner->HasUnscannedChunk(deadline)) {
            if (timer.GetElapsedTime() > maxTimePerRefresh) {
                break;
            }

            ++(*totalCount);
            auto* chunk = scanner->DequeueChunk();
            if (!chunk) {
                continue;
            }

            RefreshChunk(chunk);
            ++(*aliveCount);
        }
    };

    int totalBlobCount = 0;
    int totalJournalCount = 0;
    int aliveBlobCount = 0;
    int aliveJournalCount = 0;

    YT_PROFILE_TIMING("/chunk_server/refresh_time") {
        doRefreshChunks(
            BlobRefreshScanner_,
            &totalBlobCount,
            &aliveBlobCount,
            GetDynamicConfig()->MaxBlobChunksPerRefresh,
            GetDynamicConfig()->MaxTimePerBlobChunkRefresh);
        doRefreshChunks(
            JournalRefreshScanner_,
            &totalJournalCount,
            &aliveJournalCount,
            GetDynamicConfig()->MaxJournalChunksPerRefresh,
            GetDynamicConfig()->MaxTimePerJournalChunkRefresh);
    }

    YT_LOG_DEBUG("Chunk refresh iteration completed (TotalBlobCount: %v, AliveBlobCount: %v, TotalJournalCount: %v, AliveJournalCount: %v)",
        totalBlobCount,
        aliveBlobCount,
        totalJournalCount,
        aliveJournalCount);
}

bool TChunkReplicator::IsReplicatorEnabled()
{
    return Enabled_.value_or(false);
}

bool TChunkReplicator::IsRefreshEnabled()
{
    return GetDynamicConfig()->EnableChunkRefresh;
}

bool TChunkReplicator::IsRequisitionUpdateEnabled()
{
    return GetDynamicConfig()->EnableChunkRequisitionUpdate;
}

void TChunkReplicator::OnCheckEnabled()
{
    const auto& worldInitializer = Bootstrap_->GetWorldInitializer();
    if (!worldInitializer->IsInitialized()) {
        return;
    }

    try {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (multicellManager->IsPrimaryMaster()) {
            OnCheckEnabledPrimary();
        } else {
            OnCheckEnabledSecondary();
        }
    } catch (const std::exception& ex) {
        YT_LOG_ERROR(ex, "Error updating chunk replicator state, disabling until the next attempt");
        Enabled_ = false;
    }
}

void TChunkReplicator::OnCheckEnabledPrimary()
{
    if (!GetDynamicConfig()->EnableChunkReplicator) {
        if (!Enabled_ || *Enabled_) {
            YT_LOG_INFO("Chunk replicator is disabled, see //sys/@config");
        }
        Enabled_ = false;
        return;
    }

    const auto& nodeTracker = Bootstrap_->GetNodeTracker();
    int needOnline = GetDynamicConfig()->SafeOnlineNodeCount;
    int gotOnline = nodeTracker->GetOnlineNodeCount();
    if (gotOnline < needOnline) {
        if (!Enabled_ || *Enabled_) {
            YT_LOG_INFO("Chunk replicator disabled: too few online nodes, needed >= %v but got %v",
                needOnline,
                gotOnline);
        }
        Enabled_ = false;
        return;
    }

    const auto& multicellManager = Bootstrap_->GetMulticellManager();
    auto statistics = multicellManager->ComputeClusterStatistics();
    int gotChunkCount = statistics.chunk_count();
    int gotLostChunkCount = statistics.lost_vital_chunk_count();
    int needLostChunkCount = GetDynamicConfig()->SafeLostChunkCount;
    if (gotChunkCount > 0) {
        double needFraction = GetDynamicConfig()->SafeLostChunkFraction;
        double gotFraction = (double) gotLostChunkCount / gotChunkCount;
        if (gotFraction > needFraction) {
            if (!Enabled_ || *Enabled_) {
                YT_LOG_INFO("Chunk replicator disabled: too many lost chunks, fraction needed <= %v but got %v",
                    needFraction,
                    gotFraction);
            }
            Enabled_ = false;
            return;
        }
    }

    if (gotLostChunkCount > needLostChunkCount) {
        if (!Enabled_ || *Enabled_) {
            YT_LOG_INFO("Chunk replicator disabled: too many lost chunks, needed <= %v but got %v",
                needLostChunkCount,
                gotLostChunkCount);
        }
        Enabled_ = false;
        return;
    }

    if (!Enabled_ || !*Enabled_) {
        YT_LOG_INFO("Chunk replicator enabled");
    }
    Enabled_ = true;
}

void TChunkReplicator::OnCheckEnabledSecondary()
{
    const auto& multicellManager = Bootstrap_->GetMulticellManager();
    auto primaryCellTag = multicellManager->GetPrimaryCellTag();
    auto channel = multicellManager->GetMasterChannelOrThrow(primaryCellTag, EPeerKind::Leader);

    TObjectServiceProxy proxy(channel);

    auto req = TYPathProxy::Get("//sys/@chunk_replicator_enabled");
    auto rsp = WaitFor(proxy.Execute(req))
        .ValueOrThrow();

    auto value = ConvertTo<bool>(TYsonString(rsp->value()));
    if (!Enabled_ || value != *Enabled_) {
        if (value) {
            YT_LOG_INFO("Chunk replicator enabled at primary master");
        } else {
            YT_LOG_INFO("Chunk replicator disabled at primary master");
        }
        Enabled_ = value;
    }
}

int TChunkReplicator::GetBlobRefreshQueueSize() const
{
    return BlobRefreshScanner_->GetQueueSize();
}

int TChunkReplicator::GetJournalRefreshQueueSize() const
{
    return JournalRefreshScanner_->GetQueueSize();
}

int TChunkReplicator::GetBlobRequisitionUpdateQueueSize() const
{
    return BlobRequisitionUpdateScanner_->GetQueueSize();
}

int TChunkReplicator::GetJournalRequisitionUpdateQueueSize() const
{
    return JournalRequisitionUpdateScanner_->GetQueueSize();
}

void TChunkReplicator::ScheduleRequisitionUpdate(TChunkList* chunkList)
{
    class TVisitor
        : public IChunkVisitor
    {
    public:
        TVisitor(
            NCellMaster::TBootstrap* bootstrap,
            TChunkReplicatorPtr owner,
            TChunkList* root)
            : Bootstrap_(bootstrap)
            , Owner_(std::move(owner))
            , Root_(root)
        { }

        void Run()
        {
            YT_VERIFY(IsObjectAlive(Root_));
            auto callbacks = CreateAsyncChunkTraverserContext(
                Bootstrap_,
                NCellMaster::EAutomatonThreadQueue::ChunkRequisitionUpdateTraverser);
            TraverseChunkTree(std::move(callbacks), this, Root_);
        }

    private:
        TBootstrap* const Bootstrap_;
        const TChunkReplicatorPtr Owner_;
        TChunkList* const Root_;

        virtual bool OnChunk(
            TChunk* chunk,
            std::optional<i64> /*rowIndex*/,
            std::optional<int> /*tabletIndex*/,
            const TLegacyReadLimit& /*startLimit*/,
            const TLegacyReadLimit& /*endLimit*/,
            TTransactionId /*timestampTransactionId*/) override
        {
            Owner_->ScheduleRequisitionUpdate(chunk);
            return true;
        }

        virtual bool OnChunkView(TChunkView* /*chunkView*/) override
        {
            return false;
        }

        virtual bool OnDynamicStore(
            TDynamicStore* /*dynamicStore*/,
            const NChunkClient::TLegacyReadLimit& /*startLimit*/,
            const NChunkClient::TLegacyReadLimit& /*endLimit*/) override
        {
            return true;
        }

        virtual void OnFinish(const TError& error) override
        {
            if (!error.IsOK()) {
                // Try restarting.
                Run();
            } else {
                Owner_->ConfirmChunkListRequisitionTraverseFinished(Root_);
            }
        }
    };

    New<TVisitor>(Bootstrap_, this, chunkList)->Run();
}

void TChunkReplicator::ScheduleRequisitionUpdate(TChunk* chunk)
{
    if (!IsObjectAlive(chunk)) {
        return;
    }

    GetChunkRequisitionUpdateScanner(chunk)->EnqueueChunk(chunk);
}

void TChunkReplicator::OnRequisitionUpdate()
{
    if (!Bootstrap_->GetHydraFacade()->GetHydraManager()->IsActiveLeader()) {
        return;
    }

    if (!GetDynamicConfig()->EnableChunkRequisitionUpdate) {
        YT_LOG_DEBUG("Chunk requisition update disabled; see //sys/@config");
        return;
    }

    TReqUpdateChunkRequisition request;
    const auto& multicellManager = Bootstrap_->GetMulticellManager();
    request.set_cell_tag(multicellManager->GetCellTag());

    YT_LOG_DEBUG("Chunk requisition update iteration started");

    TmpRequisitionRegistry_.Clear();

    auto doUpdateChunkRequisition = [&] (
        const std::unique_ptr<TChunkScanner>& scanner,
        int* const totalCount,
        int* const aliveCount,
        int maxChunksPerRequisitionUpdate,
        TDuration maxTimePerRequisitionUpdate)
    {
        NProfiling::TWallTimer timer;

        while (*totalCount < maxChunksPerRequisitionUpdate && scanner->HasUnscannedChunk()) {
            if (timer.GetElapsedTime() > maxTimePerRequisitionUpdate) {
                break;
            }

            ++(*totalCount);
            auto* chunk = scanner->DequeueChunk();
            if (!chunk) {
                continue;
            }

            ComputeChunkRequisitionUpdate(chunk, &request);
            ++(*aliveCount);
        }
    };

    int totalBlobCount = 0;
    int aliveBlobCount = 0;
    int totalJournalCount = 0;
    int aliveJournalCount = 0;

    YT_PROFILE_TIMING("/chunk_server/requisition_update_time") {
        ClearChunkRequisitionCache();
        doUpdateChunkRequisition(
            BlobRequisitionUpdateScanner_,
            &totalBlobCount,
            &aliveBlobCount,
            GetDynamicConfig()->MaxBlobChunksPerRequisitionUpdate,
            GetDynamicConfig()->MaxTimePerBlobChunkRequisitionUpdate);
        doUpdateChunkRequisition(
            JournalRequisitionUpdateScanner_,
            &totalJournalCount,
            &aliveJournalCount,
            GetDynamicConfig()->MaxJournalChunksPerRequisitionUpdate,
            GetDynamicConfig()->MaxTimePerJournalChunkRequisitionUpdate);
    }

    FillChunkRequisitionDict(&request, TmpRequisitionRegistry_);

    YT_LOG_DEBUG("Chunk requisition update iteration completed (TotalBlobCount: %v, AliveBlobCount: %v, TotalJournalCount: %v, AliveJournalCount: %v, UpdateCount: %v)",
        totalBlobCount,
        aliveBlobCount,
        totalJournalCount,
        aliveJournalCount,
        request.updates_size());

    if (request.updates_size() > 0) {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto asyncResult = chunkManager
            ->CreateUpdateChunkRequisitionMutation(request)
            ->CommitAndLog(Logger);
        Y_UNUSED(WaitFor(asyncResult));
    }
}

void TChunkReplicator::ComputeChunkRequisitionUpdate(TChunk* chunk, TReqUpdateChunkRequisition* request)
{
    auto oldGlobalRequisitionIndex = chunk->GetLocalRequisitionIndex();
    auto newRequisition = ComputeChunkRequisition(chunk);
    auto* globalRegistry = GetChunkRequisitionRegistry();
    auto newGlobalRequisitionIndex = globalRegistry->Find(newRequisition);
    if (!newGlobalRequisitionIndex || *newGlobalRequisitionIndex != oldGlobalRequisitionIndex) {
        auto* update = request->add_updates();
        ToProto(update->mutable_chunk_id(), chunk->GetId());
        // Don't mix up true (global) and temporary (ephemeral) requisition indexes.
        auto newTmpRequisitionIndex = TmpRequisitionRegistry_.GetOrCreateIndex(newRequisition);
        update->set_chunk_requisition_index(newTmpRequisitionIndex);
    }
}

TChunkRequisition TChunkReplicator::ComputeChunkRequisition(const TChunk* chunk)
{
    if (CanServeRequisitionFromCache(chunk)) {
        return GetRequisitionFromCache(chunk);
    }

    bool found = false;
    TChunkRequisition requisition;

    // Unique number used to distinguish already visited chunk lists.
    auto mark = TChunkList::GenerateVisitMark();

    // BFS queue. Try to avoid allocations.
    SmallVector<TChunkList*, 64> queue;
    size_t frontIndex = 0;

    auto enqueue = [&] (TChunkList* chunkList) {
        if (chunkList->GetVisitMark() != mark) {
            chunkList->SetVisitMark(mark);
            queue.push_back(chunkList);
        }
    };

    auto enqueueAdjustedParent = [&] (TChunkList* parent) {
        auto* adjustedParent = FollowParentLinks(parent);
        if (adjustedParent) {
            enqueue(adjustedParent);
        }
    };

    // Put seeds into the queue.
    for (auto [parent, cardinality] : chunk->Parents()) {
        switch (parent->GetType()) {
            case EObjectType::ChunkList:
                enqueueAdjustedParent(parent->AsChunkList());
                break;

            case EObjectType::ChunkView:
                for (auto* chunkViewParent : parent->AsChunkView()->Parents()) {
                    enqueueAdjustedParent(chunkViewParent);
                }
                break;

            default:
                YT_ABORT();
        }
    }

    // The main BFS loop.
    while (frontIndex < queue.size()) {
        auto* chunkList = queue[frontIndex++];

        // Examine owners, if any.
        for (const auto* owningNode : chunkList->TrunkOwningNodes()) {
            auto* account = owningNode->GetAccount();
            if (account) {
                requisition.AggregateWith(owningNode->Replication(), account, true);
            }

            found = true;
        }
        // Proceed to parents.
        for (auto* parent : chunkList->Parents()) {
            enqueueAdjustedParent(parent);
        }
    }

    if (chunk->IsErasure()) {
        static_assert(MinReplicationFactor <= 1 && 1 <= MaxReplicationFactor,
                     "Replication factor limits are incorrect.");
        requisition.ForceReplicationFactor(1);
    }

    if (found) {
        YT_ASSERT(requisition.ToReplication().IsValid());
    } else {
        // Chunks that aren't linked to any trunk owner are assigned empty requisition.
        // This doesn't mean the replicator will act upon it, though, as the chunk will
        // remember its last non-empty aggregated requisition.
        requisition = GetChunkRequisitionRegistry()->GetRequisition(EmptyChunkRequisitionIndex);
    }

    CacheRequisition(chunk, requisition);

    return requisition;
}

void TChunkReplicator::ClearChunkRequisitionCache()
{
    ChunkRequisitionCache_.LastChunkParents.clear();
    ChunkRequisitionCache_.LastChunkUpdatedRequisition = std::nullopt;
    ChunkRequisitionCache_.LastErasureChunkUpdatedRequisition = std::nullopt;
}

bool TChunkReplicator::CanServeRequisitionFromCache(const TChunk* chunk)
{
    if (chunk->IsStaged() || chunk->Parents() != ChunkRequisitionCache_.LastChunkParents) {
        return false;
    }

    return chunk->IsErasure()
        ? ChunkRequisitionCache_.LastErasureChunkUpdatedRequisition.operator bool()
        : ChunkRequisitionCache_.LastChunkUpdatedRequisition.operator bool();
}

TChunkRequisition TChunkReplicator::GetRequisitionFromCache(const TChunk* chunk)
{
    return chunk->IsErasure()
        ? *ChunkRequisitionCache_.LastErasureChunkUpdatedRequisition
        : *ChunkRequisitionCache_.LastChunkUpdatedRequisition;
}

void TChunkReplicator::CacheRequisition(const TChunk* chunk, const TChunkRequisition& requisition)
{
    if (chunk->IsStaged()) {
        return;
    }

    if (ChunkRequisitionCache_.LastChunkParents != chunk->Parents()) {
        ClearChunkRequisitionCache();
        ChunkRequisitionCache_.LastChunkParents = chunk->Parents();
    }

    if (chunk->IsErasure()) {
        ChunkRequisitionCache_.LastErasureChunkUpdatedRequisition = requisition;
    } else {
        ChunkRequisitionCache_.LastChunkUpdatedRequisition = requisition;
    }
}

void TChunkReplicator::ConfirmChunkListRequisitionTraverseFinished(TChunkList* chunkList)
{
    auto chunkListId = chunkList->GetId();
    YT_LOG_DEBUG("Chunk list requisition traverse finished (ChunkListId: %v)",
        chunkListId);
    ChunkListIdsWithFinishedRequisitionTraverse_.push_back(chunkListId);
}

void TChunkReplicator::OnFinishedRequisitionTraverseFlush()
{
    if (!Bootstrap_->GetHydraFacade()->GetHydraManager()->IsActiveLeader()) {
        return;
    }

    if (ChunkListIdsWithFinishedRequisitionTraverse_.empty()) {
        return;
    }

    YT_LOG_DEBUG("Flushing finished chunk lists requisition traverse confirmations (Count: %v)",
        ChunkListIdsWithFinishedRequisitionTraverse_.size());

    TReqConfirmChunkListsRequisitionTraverseFinished request;
    ToProto(request.mutable_chunk_list_ids(), ChunkListIdsWithFinishedRequisitionTraverse_);
    ChunkListIdsWithFinishedRequisitionTraverse_.clear();

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    auto asyncResult = chunkManager
        ->CreateConfirmChunkListsRequisitionTraverseFinishedMutation(request)
        ->CommitAndLog(Logger);
    Y_UNUSED(WaitFor(asyncResult));
}

TChunkList* TChunkReplicator::FollowParentLinks(TChunkList* chunkList)
{
    while (chunkList->TrunkOwningNodes().Empty()) {
        const auto& parents = chunkList->Parents();
        auto parentCount = parents.Size();
        if (parentCount == 0) {
            return nullptr;
        }
        if (parentCount > 1) {
            break;
        }
        chunkList = *parents.begin();
    }
    return chunkList;
}

void TChunkReplicator::AddToChunkRepairQueue(TChunkPtrWithIndexes chunkWithIndexes, EChunkRepairQueue queue)
{
    YT_ASSERT(chunkWithIndexes.GetReplicaIndex() == GenericChunkReplicaIndex);
    YT_ASSERT(chunkWithIndexes.GetState() == EChunkReplicaState::Generic);
    auto* chunk = chunkWithIndexes.GetPtr();
    int mediumIndex = chunkWithIndexes.GetMediumIndex();
    YT_VERIFY(chunk->GetRepairQueueIterator(mediumIndex, queue) == TChunkRepairQueueIterator());
    auto& chunkRepairQueue = ChunkRepairQueue(mediumIndex, queue);
    auto it = chunkRepairQueue.insert(chunkRepairQueue.end(), chunkWithIndexes);
    chunk->SetRepairQueueIterator(mediumIndex, queue, it);
}

void TChunkReplicator::RemoveFromChunkRepairQueues(TChunkPtrWithIndexes chunkWithIndexes)
{
    YT_ASSERT(chunkWithIndexes.GetReplicaIndex() == GenericChunkReplicaIndex);
    YT_ASSERT(chunkWithIndexes.GetState() == EChunkReplicaState::Generic);
    auto* chunk = chunkWithIndexes.GetPtr();
    int mediumIndex = chunkWithIndexes.GetMediumIndex();
    for (auto queue : TEnumTraits<EChunkRepairQueue>::GetDomainValues()) {
        auto it = chunk->GetRepairQueueIterator(mediumIndex, queue);
        if (it != TChunkRepairQueueIterator()) {
            ChunkRepairQueue(mediumIndex, queue).erase(it);
            chunk->SetRepairQueueIterator(mediumIndex, queue, TChunkRepairQueueIterator());
        }
    }
}

const std::unique_ptr<TChunkScanner>& TChunkReplicator::GetChunkRefreshScanner(TChunk* chunk) const
{
    return chunk->IsJournal() ? JournalRefreshScanner_ : BlobRefreshScanner_;
}

const std::unique_ptr<TChunkScanner>& TChunkReplicator::GetChunkRequisitionUpdateScanner(TChunk* chunk) const
{
    return chunk->IsJournal() ? JournalRequisitionUpdateScanner_ : BlobRequisitionUpdateScanner_;
}

TChunkRequisitionRegistry* TChunkReplicator::GetChunkRequisitionRegistry()
{
    return Bootstrap_->GetChunkManager()->GetChunkRequisitionRegistry();
}

TChunkRepairQueue& TChunkReplicator::ChunkRepairQueue(int mediumIndex, EChunkRepairQueue queue)
{
    return ChunkRepairQueues(queue)[mediumIndex];
}

std::array<TChunkRepairQueue, MaxMediumCount>& TChunkReplicator::ChunkRepairQueues(EChunkRepairQueue queue)
{
    switch (queue) {
        case EChunkRepairQueue::Missing:
            return MissingPartChunkRepairQueues_;
        case EChunkRepairQueue::Decommissioned:
            return DecommissionedPartChunkRepairQueues_;
        default:
            YT_ABORT();
    }
}

TDecayingMaxMinBalancer<int, double>& TChunkReplicator::ChunkRepairQueueBalancer(EChunkRepairQueue queue)
{
    switch (queue) {
        case EChunkRepairQueue::Missing:
            return MissingPartChunkRepairQueueBalancer_;
        case EChunkRepairQueue::Decommissioned:
            return DecommissionedPartChunkRepairQueueBalancer_;
        default:
            YT_ABORT();
    }
}

const TDynamicChunkManagerConfigPtr& TChunkReplicator::GetDynamicConfig()
{
    const auto& configManager = Bootstrap_->GetConfigManager();
    return configManager->GetConfig()->ChunkManager;
}

void TChunkReplicator::OnDynamicConfigChanged(TDynamicClusterConfigPtr /*oldConfig*/)
{
    RefreshExecutor_->SetPeriod(GetDynamicConfig()->ChunkRefreshPeriod);
    RequisitionUpdateExecutor_->SetPeriod(GetDynamicConfig()->ChunkRequisitionUpdatePeriod);
    FinishedRequisitionTraverseFlushExecutor_->SetPeriod(GetDynamicConfig()->FinishedChunkListsRequisitionTraverseFlushPeriod);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
