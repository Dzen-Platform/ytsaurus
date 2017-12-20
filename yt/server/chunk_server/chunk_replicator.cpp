#include "chunk_replicator.h"
#include "private.h"
#include "chunk.h"
#include "chunk_list.h"
#include "chunk_owner_base.h"
#include "chunk_placement.h"
#include "chunk_tree_traverser.h"
#include "job.h"
#include "chunk_scanner.h"
#include "chunk_replica.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/config.h>
#include <yt/server/cell_master/config_manager.h>
#include <yt/server/cell_master/hydra_facade.h>
#include <yt/server/cell_master/world_initializer.h>
#include <yt/server/cell_master/multicell_manager.h>
#include <yt/server/cell_master/multicell_manager.pb.h>

#include <yt/server/chunk_server/chunk_manager.h>

#include <yt/server/cypress_server/node.h>
#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/node_tracker_server/data_center.h>
#include <yt/server/node_tracker_server/node.h>
#include <yt/server/node_tracker_server/node_directory_builder.h>
#include <yt/server/node_tracker_server/node_tracker.h>
#include <yt/server/node_tracker_server/rack.h>

#include <yt/server/object_server/object.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>

#include <yt/ytlib/node_tracker_client/helpers.h>
#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/serialize.h>
#include <yt/core/misc/small_vector.h>
#include <yt/core/misc/string.h>

#include <yt/core/profiling/profiler.h>
#include <yt/core/profiling/timing.h>

#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/throughput_throttler.h>

#include <yt/core/ytree/ypath_proxy.h>

#include <array>
#include <yt/core/profiling/timing.h>

namespace NYT {
namespace NChunkServer {

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

using NChunkClient::TReadLimit;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ChunkServerLogger;
static const auto& Profiler = ChunkServerProfiler;

static NProfiling::TAggregateCounter RefreshTimeCounter("/refresh_time");
static NProfiling::TAggregateCounter PropertiesUpdateTimeCounter("/properties_update_time");

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
    TChunkPlacementPtr chunkPlacement)
    : Config_(config)
    , Bootstrap_(bootstrap)
    , ChunkPlacement_(chunkPlacement)
    , ChunkRefreshDelay_(DurationToCpuDuration(Config_->ChunkRefreshDelay))
    , RefreshExecutor_(New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::ChunkMaintenance),
        BIND(&TChunkReplicator::OnRefresh, MakeWeak(this)),
        Config_->ChunkRefreshPeriod))
    , RefreshScanner_(std::make_unique<TChunkScanner>(
        Bootstrap_->GetObjectManager(),
        EChunkScanKind::Refresh))
    , PropertiesUpdateExecutor_(New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::ChunkMaintenance),
        BIND(&TChunkReplicator::OnPropertiesUpdate, MakeWeak(this)),
        Config_->ChunkPropertiesUpdatePeriod))
    , PropertiesUpdateScanner_(std::make_unique<TChunkScanner>(
        Bootstrap_->GetObjectManager(),
        EChunkScanKind::PropertiesUpdate))
    , ChunkRepairQueueBalancer_(
        Config_->RepairQueueBalancerWeightDecayFactor,
        Config_->RepairQueueBalancerWeightDecayInterval)
    , EnabledCheckExecutor_(New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Default),
        BIND(&TChunkReplicator::OnCheckEnabled, MakeWeak(this)),
        Config_->ReplicatorEnabledCheckPeriod))
    , JobThrottler_(CreateReconfigurableThroughputThrottler(
        Config_->JobThrottler,
        ChunkServerLogger,
        NProfiling::TProfiler(ChunkServerProfiler.GetPathPrefix() + "/job_throttler")))
{
    YCHECK(Config_);
    YCHECK(Bootstrap_);
    YCHECK(ChunkPlacement_);

    for (int i = 0; i < MaxMediumCount; ++i) {
        // We "balance" medium indexes, not the repair queues themselves.
        ChunkRepairQueueBalancer_.AddContender(i);
    }

    InitInterDCEdges();
}

void TChunkReplicator::Start(TChunk* frontChunk, int chunkCount)
{
    RefreshScanner_->Start(frontChunk, chunkCount);
    PropertiesUpdateScanner_->Start(frontChunk, chunkCount);
    RefreshExecutor_->Start();
    PropertiesUpdateExecutor_->Start();
    EnabledCheckExecutor_->Start();
}

void TChunkReplicator::Stop()
{
    const auto& nodeTracker = Bootstrap_->GetNodeTracker();
    for (const auto& pair : nodeTracker->Nodes()) {
        auto* node = pair.second;
        node->Jobs().clear();
    }

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    for (const auto& pair : JobMap_) {
        const auto& job = pair.second;
        auto* chunk = chunkManager->FindChunk(job->GetChunkIdWithIndexes().Id);
        if (chunk) {
            chunk->SetJob(nullptr);
        }
    }

    for (const auto& queue : ChunkRepairQueues_) {
        for (auto chunkWithIndexes : queue) {
            chunkWithIndexes.GetPtr()->SetRepairQueueIterator(chunkWithIndexes.GetMediumIndex(), TChunkRepairQueueIterator());
        }
    }

    ChunkRepairQueueBalancer_.ResetWeights();
}

void TChunkReplicator::TouchChunk(TChunk* chunk)
{
    for (int mediumIndex = 0; mediumIndex < MaxMediumCount; ++mediumIndex) {
        auto repairIt = chunk->GetRepairQueueIterator(mediumIndex);
        if (repairIt == TChunkRepairQueueIterator()) {
            continue;
        }
        auto& chunkRepairQueue = ChunkRepairQueues_[mediumIndex];
        chunkRepairQueue.erase(repairIt);
        TChunkPtrWithIndexes chunkWithIndexes(chunk, GenericChunkReplicaIndex, mediumIndex);
        auto newRepairIt = chunkRepairQueue.insert(chunkRepairQueue.begin(), chunkWithIndexes);
        chunk->SetRepairQueueIterator(mediumIndex, newRepairIt);
    }
}

TJobPtr TChunkReplicator::FindJob(const TJobId& id)
{
    auto it = JobMap_.find(id);
    return it == JobMap_.end() ? nullptr : it->second;
}

TPerMediumArray<EChunkStatus> TChunkReplicator::ComputeChunkStatuses(TChunk* chunk)
{
    TPerMediumArray<EChunkStatus> result;
    result.fill(EChunkStatus::None);

    auto statistics = ComputeChunkStatistics(chunk);

    auto resultIt = result.begin();
    for (const auto& stats : statistics.PerMediumStatistics) {
        *resultIt++ = stats.Status;
    }

    return result;
}

TChunkReplicator::TChunkStatistics TChunkReplicator::ComputeChunkStatistics(TChunk* chunk)
{
    switch (TypeFromId(chunk->GetId())) {
        case EObjectType::Chunk:
            return ComputeRegularChunkStatistics(chunk);
        case EObjectType::ErasureChunk:
            return ComputeErasureChunkStatistics(chunk);
        case EObjectType::JournalChunk:
            return ComputeJournalChunkStatistics(chunk);
        default:
            Y_UNREACHABLE();
    }
}

TChunkReplicator::TChunkStatistics TChunkReplicator::ComputeRegularChunkStatistics(TChunk* chunk)
{
    TChunkStatistics result;

    auto replicationFactors = chunk->ComputeReplicationFactors();

    TPerMediumArray<bool> hasUnsafelyPlacedReplicas{};
    TPerMediumArray<std::array<ui8, RackIndexBound>> perRackReplicaCounters{};

    TPerMediumIntArray replicaCount{};
    TPerMediumIntArray decommissionedReplicaCount{};
    TPerMediumArray<TNodePtrWithIndexesList> decommissionedReplicas;

    for (auto replica : chunk->StoredReplicas()) {
        auto mediumIndex = replica.GetMediumIndex();
        if (IsReplicaDecommissioned(replica)) {
            ++decommissionedReplicaCount[mediumIndex];
            decommissionedReplicas[mediumIndex].push_back(replica);
        } else {
            ++replicaCount[mediumIndex];
        }
        const auto* rack = replica.GetPtr()->GetRack();

        if (rack) {
            int rackIndex = rack->GetIndex();
            int maxReplicasPerRack = ChunkPlacement_->GetMaxReplicasPerRack(mediumIndex, chunk, Null);
            if (++perRackReplicaCounters[mediumIndex][rackIndex] > maxReplicasPerRack) {
                hasUnsafelyPlacedReplicas[mediumIndex] = true;
            }
        }
    }

    bool precarious = true;
    bool allMediaTransient = true;
    SmallVector<int, MaxMediumCount> mediaOnWhichLost;
    SmallVector<int, MaxMediumCount> mediaOnWhichPresent;
    for (const auto& mediumIdAndPtrPair : Bootstrap_->GetChunkManager()->Media()) {
        auto* medium = mediumIdAndPtrPair.second;
        if (medium->GetCache()) {
            continue;
        }

        auto mediumIndex = medium->GetIndex();
        auto& mediumStatistics = result.PerMediumStatistics[mediumIndex];
        auto mediumTransient = medium->GetTransient();

        auto mediumReplicationFactor = replicationFactors[mediumIndex];
        auto mediumReplicaCount = replicaCount[mediumIndex];
        auto mediumDecommissionedReplicaCount = decommissionedReplicaCount[mediumIndex];

        // NB: some very counter-intuitive scenarios are possible here.
        // E.g. mediumReplicationFactor == 0, but mediumReplicaCount != 0.
        // This happens when medium-related properties of a chunk change.
        // One should be careful about one's assumptions.

        if (mediumReplicationFactor == 0 &&
            mediumReplicaCount == 0 &&
            mediumDecommissionedReplicaCount == 0)
        {
            // This medium is irrelevant to this chunk.
            continue;
        }

        ComputeRegularChunkStatisticsForMedium(
            mediumStatistics,
            mediumReplicationFactor,
            mediumReplicaCount,
            mediumDecommissionedReplicaCount,
            decommissionedReplicas[mediumIndex],
            hasUnsafelyPlacedReplicas[mediumIndex]);

        allMediaTransient = allMediaTransient && mediumTransient;

        if (Any(mediumStatistics.Status & EChunkStatus::Lost)) {
            mediaOnWhichLost.push_back(mediumIndex);
        } else {
            mediaOnWhichPresent.push_back(mediumIndex);
            precarious = precarious && mediumTransient;
        }
    }

    // Intra-medium replication has been dealt above.
    // The only cross-medium thing left do is to kickstart replication of chunks
    // lost on one medium but not on another.
    ComputeRegularChunkStatisticsCrossMedia(
        result,
        precarious,
        allMediaTransient,
        mediaOnWhichLost,
        mediaOnWhichPresent.size());

    return result;
}

void TChunkReplicator::ComputeRegularChunkStatisticsForMedium(
    TPerMediumChunkStatistics& result,
    int replicationFactor,
    int replicaCount,
    int decommissionedReplicaCount,
    const TNodePtrWithIndexesList& decommissionedReplicas,
    bool hasUnsafelyPlacedReplicas)
{
    result.ReplicaCount[GenericChunkReplicaIndex] = replicaCount;
    result.DecommissionedReplicaCount[GenericChunkReplicaIndex] = decommissionedReplicaCount;

    if (replicaCount + decommissionedReplicaCount == 0) {
        result.Status |= EChunkStatus::Lost;
    }

    if (replicaCount < replicationFactor && replicaCount + decommissionedReplicaCount > 0) {
        result.Status |= EChunkStatus::Underreplicated;
    }

    if (replicaCount == replicationFactor && decommissionedReplicaCount > 0) {
        result.Status |= EChunkStatus::Overreplicated;
        result.DecommissionedRemovalReplicas.append(decommissionedReplicas.begin(), decommissionedReplicas.end());
    }

    if (replicaCount > replicationFactor) {
        result.Status |= EChunkStatus::Overreplicated;
        result.BalancingRemovalIndexes.push_back(GenericChunkReplicaIndex);
    }

    if (replicationFactor > 1 && hasUnsafelyPlacedReplicas && None(result.Status & EChunkStatus::Overreplicated)) {
        result.Status |= EChunkStatus::UnsafelyPlaced;
    }

    if (Any(result.Status & (EChunkStatus::Underreplicated | EChunkStatus::UnsafelyPlaced)) &&
        None(result.Status & EChunkStatus::Overreplicated) &&
        replicaCount + decommissionedReplicaCount > 0)
    {
        result.ReplicationIndexes.push_back(GenericChunkReplicaIndex);
    }
}

void TChunkReplicator::ComputeRegularChunkStatisticsCrossMedia(
    TChunkStatistics& result,
    bool precarious,
    bool allMediaTransient,
    const SmallVector<int, MaxMediumCount>& mediaOnWhichLost,
    int mediaOnWhichPresentCount)
{
    if (mediaOnWhichPresentCount == 0) {
        result.Status |= ECrossMediumChunkStatus::Lost;
    }
    if (precarious && !allMediaTransient) {
        result.Status |= ECrossMediumChunkStatus::Precarious;
    }

    if (!mediaOnWhichLost.empty() && mediaOnWhichPresentCount > 0) {
        for (auto mediumIndex : mediaOnWhichLost) {
            auto& mediumStatistics = result.PerMediumStatistics[mediumIndex];
            mediumStatistics.Status |= EChunkStatus::Underreplicated;
            mediumStatistics.ReplicationIndexes.push_back(GenericChunkReplicaIndex);
        }
        result.Status |= ECrossMediumChunkStatus::MediumWiseLost;
    }
}

TChunkReplicator::TChunkStatistics TChunkReplicator::ComputeErasureChunkStatistics(TChunk* chunk)
{
    TChunkStatistics result;

    auto* codec = NErasure::GetCodec(chunk->GetErasureCodec());

    TPerMediumArray<std::array<TNodePtrWithIndexesList, ChunkReplicaIndexBound>> decommissionedReplicas{};
    TPerMediumArray<std::array<ui8, RackIndexBound>> perRackReplicaCounters{};
    // An arbitrary replica collocated with too may others within a single rack - per medium.
    TPerMediumIntArray unsafelyPlacedReplicaIndexes;
    unsafelyPlacedReplicaIndexes.fill(-1);

    TPerMediumIntArray totalReplicaCounts;
    totalReplicaCounts.fill(0);
    TPerMediumIntArray totalDecommissionedReplicaCounts;
    totalDecommissionedReplicaCounts.fill(0);

    auto mark = TNode::GenerateVisitMark();

    for (auto replica : chunk->StoredReplicas()) {
        auto* node = replica.GetPtr();
        int replicaIndex = replica.GetReplicaIndex();
        int mediumIndex = replica.GetMediumIndex();
        auto& mediumStatistics = result.PerMediumStatistics[mediumIndex];
        if (IsReplicaDecommissioned(replica) || node->GetVisitMark(mediumIndex) == mark) {
            ++mediumStatistics.DecommissionedReplicaCount[replicaIndex];
            decommissionedReplicas[mediumIndex][replicaIndex].push_back(replica);
            ++totalDecommissionedReplicaCounts[mediumIndex];
        } else {
            ++mediumStatistics.ReplicaCount[replicaIndex];
            ++totalReplicaCounts[mediumIndex];
        }
        node->SetVisitMark(mediumIndex, mark);
        const auto* rack = node->GetRack();
        if (rack) {
            int rackIndex = rack->GetIndex();
            int maxReplicasPerRack = ChunkPlacement_->GetMaxReplicasPerRack(mediumIndex, chunk);
            if (++perRackReplicaCounters[mediumIndex][rackIndex] > maxReplicasPerRack) {
                // A erasure chunk is considered placed unsafely if some non-null rack
                // contains more replicas than returned by TChunk::GetMaxReplicasPerRack.
                unsafelyPlacedReplicaIndexes[mediumIndex] = replicaIndex;
            }
        }
    }

    bool allMediaTransient = true;
    bool allMediaDataPartsOnly = true;
    TPerMediumArray<NErasure::TPartIndexSet> mediumToErasedIndexes{};
    TMediumSet activeMedia;

    auto chunkProperties = chunk->ComputeProperties();

    for (const auto& mediumIdAndPtrPair : Bootstrap_->GetChunkManager()->Media()) {
        auto* medium = mediumIdAndPtrPair.second;
        if (medium->GetCache()) {
            continue;
        }

        auto mediumIndex = medium->GetIndex();
        auto mediumTransient = medium->GetTransient();

        auto dataPartsOnly = chunkProperties[mediumIndex].GetDataPartsOnly();
        auto mediumReplicationFactor = chunkProperties[mediumIndex].GetReplicationFactor();

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
            codec,
            mediumReplicationFactor,
            decommissionedReplicas[mediumIndex],
            unsafelyPlacedReplicaIndexes[mediumIndex],
            mediumToErasedIndexes[mediumIndex],
            dataPartsOnly);
    }

    ComputeErasureChunkStatisticsCrossMedia(
        result,
        codec,
        allMediaTransient,
        allMediaDataPartsOnly,
        mediumToErasedIndexes,
        activeMedia);

    return result;
}

void TChunkReplicator::ComputeErasureChunkStatisticsForMedium(
    TPerMediumChunkStatistics& result,
    NErasure::ICodec* codec,
    int replicationFactor,
    std::array<TNodePtrWithIndexesList, ChunkReplicaIndexBound>& decommissionedReplicas,
    int unsafelyPlacedReplicaIndex,
    NErasure::TPartIndexSet& erasedIndexes,
    bool dataPartsOnly)
{
    Y_ASSERT(0 <= replicationFactor && replicationFactor <= 1);

    int totalPartCount = codec->GetTotalPartCount();
    int dataPartCount = codec->GetDataPartCount();

    for (int index = 0; index < totalPartCount; ++index) {
        int replicaCount = result.ReplicaCount[index];
        int decommissionedReplicaCount = result.DecommissionedReplicaCount[index];
        auto isDataPart = index < dataPartCount;
        auto removalAdvised = replicationFactor == 0 || (!isDataPart && dataPartsOnly);
        auto targetReplicationFactor = removalAdvised ? 0 : 1;

        if (replicaCount >= targetReplicationFactor && decommissionedReplicaCount > 0) {
            result.Status |= EChunkStatus::Overreplicated;
            const auto& replicas = decommissionedReplicas[index];
            result.DecommissionedRemovalReplicas.append(replicas.begin(), replicas.end());
        }

        if (replicaCount > targetReplicationFactor && decommissionedReplicaCount == 0) {
            result.Status |= EChunkStatus::Overreplicated;
            result.BalancingRemovalIndexes.push_back(index);
        }

        if (replicaCount == 0 && decommissionedReplicaCount > 0 && !removalAdvised) {
            result.Status |= EChunkStatus::Underreplicated;
            result.ReplicationIndexes.push_back(index);
        }

        if (replicaCount == 0 && decommissionedReplicaCount == 0 && !removalAdvised) {
            erasedIndexes.set(index);
            if (isDataPart) {
                result.Status |= EChunkStatus::DataMissing;
            } else {
                result.Status |= EChunkStatus::ParityMissing;
            }
        }
    }

    if (!codec->CanRepair(erasedIndexes) &&
        erasedIndexes.any()) // This is to avoid flagging chunks with no parity
                             // parts as lost when dataPartsOnly == true.
    {
        result.Status |= EChunkStatus::Lost;
    }

    if (unsafelyPlacedReplicaIndex != -1 && None(result.Status & EChunkStatus::Overreplicated)) {
        result.Status |= EChunkStatus::UnsafelyPlaced;
        if (None(result.Status & EChunkStatus::Overreplicated) && result.ReplicationIndexes.empty()) {
            result.ReplicationIndexes.push_back(unsafelyPlacedReplicaIndex);
        }
    }
}

void TChunkReplicator::ComputeErasureChunkStatisticsCrossMedia(
    TChunkStatistics& result,
    NErasure::ICodec* codec,
    bool allMediaTransient,
    bool allMediaDataPartsOnly,
    const TPerMediumArray<NErasure::TPartIndexSet>& mediumToErasedIndexes,
    const TMediumSet& activeMedia)
{
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

    for (int mediumIndex = 0; mediumIndex < MaxMediumCount; ++mediumIndex) {
        if (!activeMedia[mediumIndex]) {
            continue;
        }
        const auto& erasedIndexes = mediumToErasedIndexes[mediumIndex];
        crossMediumErasedIndexes &= erasedIndexes;
        if (!transientMedia.test(mediumIndex)) {
            crossMediumErasedIndexesNoTransient &= erasedIndexes;
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
            if (Any(mediumStatistics.Status & EChunkStatus::Lost)) {
                // The chunk is lost on at least one medium.
                result.Status |= ECrossMediumChunkStatus::MediumWiseLost;
                break;
            }
        }
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

    // Replicate parts cross-media. Do this even if the chunk is unrepairable:
    // having identical states on all media is just simpler to reason about.
    int mediumIndex = 0;
    for (const auto& mediumErasedIndexes : mediumToErasedIndexes) {
        const auto& erasedIndexes = mediumErasedIndexes;
        auto& mediumStatistics = result.PerMediumStatistics[mediumIndex];

        for (int index = 0; index < totalPartCount; ++index) {
            if (erasedIndexes.test(index) && // If dataPartsOnly is true, everything beyond dataPartCount will test negative.
                !crossMediumErasedIndexes.test(index))
            {
                mediumStatistics.Status |= EChunkStatus::Underreplicated;
                mediumStatistics.ReplicationIndexes.push_back(index);
            }
        }

        ++mediumIndex;
    }
}

TChunkReplicator::TChunkStatistics TChunkReplicator::ComputeJournalChunkStatistics(TChunk* chunk)
{
    // NB: Journal chunks always have a single configured medium.
    auto properties = chunk->ComputeProperties();
    int mediumIndex = -1;
    for (int index = 0; index < MaxMediumCount; ++index) {
        if (properties[index]) {
            mediumIndex = index;
            break;
        }
    }
    YCHECK(mediumIndex >= 0);

    TChunkStatistics results;
    auto& result = results.PerMediumStatistics[mediumIndex];

    int replicationFactor = chunk->ComputeReplicationFactor(mediumIndex);
    int readQuorum = chunk->GetReadQuorum();

    int replicaCount = 0;
    int decommissionedReplicaCount = 0;
    int sealedReplicaCount = 0;
    int unsealedReplicaCount = 0;
    TNodePtrWithIndexesList decommissionedReplicas;
    std::array<ui8, RackIndexBound> perRackReplicaCounters{};
    bool hasUnsafelyPlacedReplicas = false;

    for (auto replica : chunk->StoredReplicas()) {
        if (replica.GetReplicaIndex() == SealedChunkReplicaIndex) {
            ++sealedReplicaCount;
        } else {
            ++unsealedReplicaCount;
        }
        if (IsReplicaDecommissioned(replica)) {
            ++decommissionedReplicaCount;
            decommissionedReplicas.push_back(replica);
        } else {
            ++replicaCount;
        }
        const auto* rack = replica.GetPtr()->GetRack();
        if (rack) {
            int rackIndex = rack->GetIndex();
            int maxReplicasPerRack = ChunkPlacement_->GetMaxReplicasPerRack(mediumIndex, chunk, Null);
            if (++perRackReplicaCounters[rackIndex] > maxReplicasPerRack) {
                // A journal chunk is considered placed unsafely if some non-null rack
                // contains more replicas than returned by TChunk::GetMaxReplicasPerRack.
                hasUnsafelyPlacedReplicas = true;
            }
        }
    }

    result.ReplicaCount[GenericChunkReplicaIndex] = replicaCount;
    result.DecommissionedReplicaCount[GenericChunkReplicaIndex] = decommissionedReplicaCount;

    if (replicaCount + decommissionedReplicaCount == 0) {
        result.Status |= EChunkStatus::Lost;
    }

    if (chunk->IsSealed()) {
        result.Status |= EChunkStatus::Sealed;

        if (replicaCount < replicationFactor && sealedReplicaCount > 0) {
            result.Status |= EChunkStatus::Underreplicated;
            result.ReplicationIndexes.push_back(GenericChunkReplicaIndex);
        }

        if (replicaCount == replicationFactor && decommissionedReplicaCount > 0 && unsealedReplicaCount == 0) {
            result.Status |= EChunkStatus::Overreplicated;
            result.DecommissionedRemovalReplicas.append(decommissionedReplicas.begin(), decommissionedReplicas.end());
        }

        if (replicaCount > replicationFactor && unsealedReplicaCount == 0) {
            result.Status |= EChunkStatus::Overreplicated;
            result.BalancingRemovalIndexes.push_back(GenericChunkReplicaIndex);
        }
    }

    if (replicaCount + decommissionedReplicaCount < readQuorum && sealedReplicaCount == 0) {
        result.Status |= EChunkStatus::QuorumMissing;
    }

    if (hasUnsafelyPlacedReplicas) {
        result.Status |= EChunkStatus::UnsafelyPlaced;
    }

    if (Any(result.Status & (EChunkStatus::Underreplicated | EChunkStatus::UnsafelyPlaced)) &&
        None(result.Status & EChunkStatus::Overreplicated) &&
        sealedReplicaCount > 0)
    {
        result.ReplicationIndexes.push_back(GenericChunkReplicaIndex);
    }

    if (Any(result.Status & EChunkStatus::Lost)) {
        // Copy 'lost' flag from medium-specific status to cross-media status.
        results.Status |= ECrossMediumChunkStatus::Lost;
    }

    return results;
}

void TChunkReplicator::ScheduleJobs(
    TNode* node,
    const std::vector<TJobPtr>& runningJobs,
    std::vector<TJobPtr>* jobsToStart,
    std::vector<TJobPtr>* jobsToAbort,
    std::vector<TJobPtr>* jobsToRemove)
{
    UpdateInterDCEdgeCapacities(); // Pull capacity changes, react on DC removal (if any).

    ProcessExistingJobs(
        node,
        runningJobs,
        jobsToAbort,
        jobsToRemove);

    ScheduleNewJobs(
        node,
        jobsToStart,
        jobsToAbort);
}

void TChunkReplicator::OnNodeRegistered(TNode* /*node*/)
{ }

void TChunkReplicator::OnNodeUnregistered(TNode* node)
{
    for (const auto& job : node->Jobs()) {
        LOG_DEBUG("Job canceled (JobId: %v)", job->GetJobId());
        UnregisterJob(job, EJobUnregisterFlags::ScheduleChunkRefresh);
    }
    node->Reset();
}

void TChunkReplicator::OnNodeDisposed(TNode* node)
{
    YCHECK(node->Jobs().empty());
    YCHECK(node->ChunkSealQueue().empty());
    YCHECK(node->ChunkRemovalQueue().empty());
    for (const auto& queue : node->ChunkReplicationQueues()) {
        YCHECK(queue.empty());
    }
}

void TChunkReplicator::OnChunkDestroyed(TChunk* chunk)
{
    RefreshScanner_->OnChunkDestroyed(chunk);
    PropertiesUpdateScanner_->OnChunkDestroyed(chunk);
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

void TChunkReplicator::ProcessExistingJobs(
    TNode* node,
    const std::vector<TJobPtr>& currentJobs,
    std::vector<TJobPtr>* jobsToAbort,
    std::vector<TJobPtr>* jobsToRemove)
{
    const auto& address = node->GetDefaultAddress();

    for (const auto& job : currentJobs) {
        const auto& jobId = job->GetJobId();
        YCHECK(CellTagFromId(jobId) == Bootstrap_->GetCellTag());
        YCHECK(TypeFromId(jobId) == EObjectType::MasterJob);
        switch (job->GetState()) {
            case EJobState::Running:
            case EJobState::Waiting: {
                if (TInstant::Now() - job->GetStartTime() > Config_->JobTimeout) {
                    jobsToAbort->push_back(job);
                    LOG_WARNING("Job timed out (JobId: %v, Address: %v, Duration: %v)",
                        jobId,
                        address,
                        TInstant::Now() - job->GetStartTime());
                    break;
                }

                switch (job->GetState()) {
                    case EJobState::Running:
                        LOG_DEBUG("Job is running (JobId: %v, Address: %v)",
                            jobId,
                            address);
                        break;

                    case EJobState::Waiting:
                        LOG_DEBUG("Job is waiting (JobId: %v, Address: %v)",
                            jobId,
                            address);
                        break;

                    default:
                        Y_UNREACHABLE();
                }
                break;
            }

            case EJobState::Completed:
            case EJobState::Failed:
            case EJobState::Aborted: {
                jobsToRemove->push_back(job);
                switch (job->GetState()) {
                    case EJobState::Completed:
                        LOG_DEBUG("Job completed (JobId: %v, Address: %v)",
                            jobId,
                            address);
                        break;

                    case EJobState::Failed:
                        LOG_WARNING(job->Error(), "Job failed (JobId: %v, Address: %v)",
                            jobId,
                            address);
                        break;

                    case EJobState::Aborted:
                        LOG_WARNING(job->Error(), "Job aborted (JobId: %v, Address: %v)",
                            jobId,
                            address);
                        break;

                    default:
                        Y_UNREACHABLE();
                }
                UnregisterJob(job);
                break;
            }

            default:
                Y_UNREACHABLE();
        }
    }

    // Check for missing jobs
    yhash_set<TJobPtr> currentJobSet(currentJobs.begin(), currentJobs.end());
    std::vector<TJobPtr> missingJobs;
    for (const auto& job : node->Jobs()) {
        if (currentJobSet.find(job) == currentJobSet.end()) {
            missingJobs.push_back(job);
            LOG_WARNING("Job is missing (JobId: %v, Address: %v)",
                job->GetJobId(),
                address);
        }
    }

    for (const auto& job : missingJobs) {
        UnregisterJob(job);
    }
}

TJobId TChunkReplicator::GenerateJobId()
{
    return MakeRandomId(EObjectType::MasterJob, Bootstrap_->GetCellTag());
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
    auto replicationFactor = chunk->ComputeReplicationFactor(targetMediumIndex);
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
        Null,
        UnsaturatedInterDCEdges[sourceNode->GetDataCenter()],
        ESessionType::Replication);
    if (targetNodes.empty()) {
        return false;
    }

    TNodePtrWithIndexesList targetReplicas;
    for (auto* node : targetNodes) {
        targetReplicas.emplace_back(node, replicaIndex, targetMediumIndex);
    }

    *job = TJob::CreateReplicate(
        GenerateJobId(),
        chunkWithIndexes,
        sourceNode,
        targetReplicas);

    LOG_DEBUG("Replication job scheduled (JobId: %v, Address: %v, ChunkId: %v, TargetAddresses: %v)",
        (*job)->GetJobId(),
        sourceNode->GetDefaultAddress(),
        chunkWithIndexes,
        MakeFormattableRange(targetNodes, TNodePtrAddressFormatter()));

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
        UnsaturatedInterDCEdges[sourceNode->GetDataCenter()]);
    if (!targetNode) {
        return false;
    }

    TNodePtrWithIndexesList targetReplicas{
        TNodePtrWithIndexes(targetNode, replicaIndex, mediumIndex)
    };

    *job = TJob::CreateReplicate(
        GenerateJobId(),
        chunkWithIndexes,
        sourceNode,
        targetReplicas);

    LOG_DEBUG("Balancing job scheduled (JobId: %v, Address: %v, ChunkId: %v, TargetAddress: %v)",
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
        GenerateJobId(),
        chunkIdWithIndexes,
        node);

    LOG_DEBUG("Removal job scheduled (JobId: %v, Address: %v, ChunkId: %v)",
        (*job)->GetJobId(),
        node->GetDefaultAddress(),
        chunkIdWithIndexes);

    return true;
}

bool TChunkReplicator::CreateRepairJob(
    TNode* node,
    TChunkPtrWithIndexes chunkWithIndexes,
    TJobPtr* job)
{
    YCHECK(chunkWithIndexes.GetReplicaIndex() == GenericChunkReplicaIndex);

    auto* chunk = chunkWithIndexes.GetPtr();
    int mediumIndex = chunkWithIndexes.GetMediumIndex();

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    auto* medium = chunkManager->GetMediumByIndex(mediumIndex);

    YCHECK(chunk->IsErasure());

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
        if (mediumStatistics.ReplicaCount[index] == 0 &&
            mediumStatistics.DecommissionedReplicaCount[index] == 0)
        {
            erasedPartIndexes.push_back(index);
        }
    }

    int erasedPartCount = static_cast<int>(erasedPartIndexes.size());
    if (erasedPartCount == 0) {
        return true;
    }

    auto targetNodes = ChunkPlacement_->AllocateWriteTargets(
        medium,
        chunk,
        erasedPartCount,
        erasedPartCount,
        Null,
        UnsaturatedInterDCEdges[node->GetDataCenter()],
        ESessionType::Repair);
    if (targetNodes.empty()) {
        return false;
    }

    YCHECK(targetNodes.size() == erasedPartCount);

    TNodePtrWithIndexesList targetReplicas;
    int targetIndex = 0;
    for (auto* node : targetNodes) {
        targetReplicas.emplace_back(node, erasedPartIndexes[targetIndex++], mediumIndex);
    }

    *job = TJob::CreateRepair(
        GenerateJobId(),
        chunk,
        node,
        targetReplicas,
        Config_->RepairJobMemoryUsage);

    LOG_DEBUG("Repair job scheduled (JobId: %v, Address: %v, ChunkId: %v, Targets: %v, ErasedPartIndexes: %v)",
        (*job)->GetJobId(),
        node->GetDefaultAddress(),
        chunkWithIndexes,
        MakeFormattableRange(targetNodes, TNodePtrAddressFormatter()),
        erasedPartIndexes);

    return true;
}

bool TChunkReplicator::CreateSealJob(
    TNode* node,
    TChunkPtrWithIndexes chunkWithIndexes,
    TJobPtr* job)
{
    YCHECK(chunkWithIndexes.GetReplicaIndex() == GenericChunkReplicaIndex);

    auto* chunk = chunkWithIndexes.GetPtr();
    YCHECK(chunk->IsJournal());
    YCHECK(chunk->IsSealed());

    if (!IsObjectAlive(chunk)) {
        return true;
    }

    if (chunk->IsJobScheduled()) {
        return true;
    }

    // NB: Seal jobs can be started even if chunk refresh is scheduled.

    if (chunk->StoredReplicas().size() < chunk->GetReadQuorum()) {
        return true;
    }

    *job = TJob::CreateSeal(
        GenerateJobId(),
        chunkWithIndexes,
        node);

    LOG_DEBUG("Seal job scheduled (JobId: %v, Address: %v, ChunkId: %v)",
        (*job)->GetJobId(),
        node->GetDefaultAddress(),
        chunkWithIndexes);

    return true;
}

void TChunkReplicator::ScheduleNewJobs(
    TNode* node,
    std::vector<TJobPtr>* jobsToStart,
    std::vector<TJobPtr>* jobsToAbort)
{
    if (JobThrottler_->IsOverdraft()) {
        return;
    }

    const auto& resourceLimits = node->ResourceLimits();
    auto& resourceUsage = node->ResourceUsage();
    const auto* nodeDataCenter = node->GetDataCenter();

    auto registerJob = [&] (const TJobPtr& job) {
        if (job) {
            resourceUsage += job->ResourceUsage();
            jobsToStart->push_back(job);
            RegisterJob(job);
            JobThrottler_->Acquire(1);
        }
    };

    int misscheduledReplicationJobs = 0;
    int misscheduledRepairJobs = 0;
    int misscheduledSealJobs = 0;
    int misscheduledRemovalJobs = 0;

    // NB: Beware of chunks larger than the limit; we still need to be able to replicate them one by one.
    auto hasSpareReplicationResources = [&] () {
        return
            misscheduledReplicationJobs < Config_->MaxMisscheduledReplicationJobsPerHeartbeat &&
            resourceUsage.replication_slots() < resourceLimits.replication_slots() &&
            (resourceUsage.replication_slots() == 0 || resourceUsage.replication_data_size() < resourceLimits.replication_data_size());
    };

    // NB: Beware of chunks larger than the limit; we still need to be able to repair them one by one.
    auto hasSpareRepairResources = [&] () {
        return
            misscheduledRepairJobs < Config_->MaxMisscheduledRepairJobsPerHeartbeat &&
            resourceUsage.repair_slots() < resourceLimits.repair_slots() &&
            (resourceUsage.repair_slots() == 0 || resourceUsage.repair_data_size() < resourceLimits.repair_data_size());
    };

    auto hasSpareSealResources = [&] () {
        return
            misscheduledSealJobs < Config_->MaxMisscheduledSealJobsPerHeartbeat &&
            resourceUsage.seal_slots() < resourceLimits.seal_slots();
    };

    auto hasSpareRemovalResources = [&] () {
        return
            misscheduledRemovalJobs < Config_->MaxMisscheduledRemovalJobsPerHeartbeat &&
            resourceUsage.removal_slots() < resourceLimits.removal_slots();
    };

    if (IsEnabled()) {
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        // Schedule replication jobs.
        for (auto& queue : node->ChunkReplicationQueues()) {
            auto it = queue.begin();
            while (it != queue.end() &&
                   hasSpareReplicationResources() &&
                   HasUnsaturatedInterDCEdgeStartingFrom(nodeDataCenter))
            {
                auto jt = it++;
                const auto& chunkWithIndexes = jt->first;
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
                        registerJob(std::move(job));
                    }
                }

                if (mediumIndexSet.none()) {
                    queue.erase(jt);
                }
            }
        }

        // Schedule repair jobs.
        {
            TPerMediumArray<TChunkRepairQueue::iterator> iteratorPerRepairQueue = {};
            std::transform(
                ChunkRepairQueues_.begin(),
                ChunkRepairQueues_.end(),
                iteratorPerRepairQueue.begin(),
                [] (TChunkRepairQueue& repairQueue) {
                    return repairQueue.begin();
                });

            while (hasSpareRepairResources() &&
                   HasUnsaturatedInterDCEdgeStartingFrom(nodeDataCenter))
            {
                auto winner = ChunkRepairQueueBalancer_.TakeWinnerIf(
                    [&] (int mediumIndex) {
                        // Don't repair chunks on nodes without relevant medium.
                        // In particular, this avoids repairing non-cloud tables in the cloud.
                        return node->HasMedium(mediumIndex) && iteratorPerRepairQueue[mediumIndex] != ChunkRepairQueues_[mediumIndex].end();
                    });

                if (!winner) {
                    break; // Nothing to repair on relevant media.
                }

                auto mediumIndex = *winner;
                auto& chunkRepairQueue = ChunkRepairQueues_[mediumIndex];
                auto chunkIt = iteratorPerRepairQueue[mediumIndex]++;
                auto chunkWithIndexes = *chunkIt;
                auto* chunk = chunkWithIndexes.GetPtr();
                TJobPtr job;
                if (CreateRepairJob(node, chunkWithIndexes, &job)) {
                    chunk->SetRepairQueueIterator(chunkWithIndexes.GetMediumIndex(), TChunkRepairQueueIterator());
                    chunkRepairQueue.erase(chunkIt);
                    if (job) {
                        ChunkRepairQueueBalancer_.AddWeight(
                            *winner,
                            job->ResourceUsage().repair_data_size() * job->TargetReplicas().size());
                    }
                } else {
                    ++misscheduledRepairJobs;
                }
                registerJob(std::move(job));
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
                const auto& chunkIdWithIndex = jt->first;
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
                        registerJob(std::move(job));
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

            double targetFillFactor = *sourceFillFactor - Config_->MinBalancingFillFactorDiff;
            if (hasSpareReplicationResources() &&
                *sourceFillFactor > Config_->MinBalancingFillFactor &&
                ChunkPlacement_->HasBalancingTargets(medium, targetFillFactor) &&
                HasUnsaturatedInterDCEdgeStartingFrom(nodeDataCenter))
            {
                int maxJobs = std::max(0, resourceLimits.replication_slots() - resourceUsage.replication_slots());
                auto chunksToBalance = ChunkPlacement_->GetBalancingChunks(medium, node, maxJobs);
                for (auto chunkWithIndexes : chunksToBalance) {
                    if (!hasSpareReplicationResources()) {
                        break;
                    }

                    TJobPtr job;
                    if (!CreateBalancingJob(node, chunkWithIndexes, targetFillFactor, &job)) {
                        ++misscheduledReplicationJobs;
                    }
                    registerJob(std::move(job));
                }
            }
        }
    }

    // Schedule seal jobs.
    // NB: This feature is active regardless of replicator state.
    {
        auto& queue = node->ChunkSealQueue();
        auto it = queue.begin();
        while (it != queue.end() && hasSpareSealResources()) {
            auto jt = it++;
            auto* chunk = jt->first;
            auto& mediumIndexSet = jt->second;
            for (int mediumIndex = 0; mediumIndex < mediumIndexSet.size(); ++mediumIndex) {
                if (mediumIndexSet.test(mediumIndex)) {
                    TChunkPtrWithIndexes chunkWithIndexes(
                        chunk,
                        GenericChunkReplicaIndex,
                        mediumIndex);
                    TJobPtr job;
                    if (CreateSealJob(node, chunkWithIndexes, &job)) {
                        mediumIndexSet.reset(mediumIndex);
                    } else {
                        ++misscheduledRepairJobs;
                    }
                    registerJob(std::move(job));
                }
            }
            if (mediumIndexSet.none()) {
                queue.erase(jt);
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

    ResetChunkStatus(chunk);
    RemoveChunkFromQueuesOnRefresh(chunk);

    auto allMediaStatistics = ComputeChunkStatistics(chunk);

    auto durabilityRequired = false;

    for (const auto& mediumIdAndPtrPair : Bootstrap_->GetChunkManager()->Media()) {
        auto* medium = mediumIdAndPtrPair.second;
        // For now, chunk cache-as-medium support is rudimentary, and replicator
        // ignores chunk cache to preserve original behavior.
        if (medium->GetCache()) {
            continue;
        }

        auto mediumIndex = medium->GetIndex();

        auto& statistics = allMediaStatistics.PerMediumStatistics[mediumIndex];
        if (statistics.Status == EChunkStatus::None) {
            continue;
        }

        auto replicationFactor = chunk->ComputeReplicationFactor(mediumIndex);
        auto durabilityRequiredOnMedium =
            chunk->ComputeVital() && (chunk->IsErasure() || replicationFactor > 1);
        durabilityRequired = durabilityRequired || durabilityRequiredOnMedium;

        if (Any(statistics.Status & EChunkStatus::Overreplicated)) {
            OverreplicatedChunks_.insert(chunk);
        }

        if (Any(statistics.Status & EChunkStatus::Underreplicated)) {
            UnderreplicatedChunks_.insert(chunk);
        }

        if (Any(statistics.Status & EChunkStatus::QuorumMissing)) {
            QuorumMissingChunks_.insert(chunk);
        }

        if (Any(statistics.Status & EChunkStatus::UnsafelyPlaced)) {
            UnsafelyPlacedChunks_.insert(chunk);
        }

        if (!chunk->IsJobScheduled()) {
            if (Any(statistics.Status & EChunkStatus::Overreplicated) &&
                None(allMediaStatistics.Status & ECrossMediumChunkStatus::MediumWiseLost))
            {
                for (auto nodeWithIndexes : statistics.DecommissionedRemovalReplicas) {
                    Y_ASSERT(mediumIndex == nodeWithIndexes.GetMediumIndex());
                    int replicaIndex = nodeWithIndexes.GetReplicaIndex();
                    TChunkIdWithIndexes chunkIdWithIndexes(chunk->GetId(), replicaIndex, mediumIndex);
                    auto* node = nodeWithIndexes.GetPtr();
                    if (node->GetLocalState() == ENodeState::Online) {
                        node->AddToChunkRemovalQueue(chunkIdWithIndexes);
                    }
                }

                for (int replicaIndex : statistics.BalancingRemovalIndexes) {
                    TChunkPtrWithIndexes chunkWithIndexes(chunk, replicaIndex, mediumIndex);
                    TChunkIdWithIndexes chunkIdWithIndexes(chunk->GetId(), replicaIndex, mediumIndex);
                    auto* targetNode = ChunkPlacement_->GetRemovalTarget(chunkWithIndexes);
                    if (targetNode) {
                        targetNode->AddToChunkRemovalQueue(chunkIdWithIndexes);
                    }
                }
            }

            // This check may yield true even for lost chunks when cross-medium replication is in progress.
            if (Any(statistics.Status & (EChunkStatus::Underreplicated | EChunkStatus::UnsafelyPlaced))) {
                for (auto replicaIndex : statistics.ReplicationIndexes) {
                    // Cap replica count minus one against the range [0, ReplicationPriorityCount - 1].
                    int replicaCount = statistics.ReplicaCount[replicaIndex];
                    int priority = std::max(std::min(replicaCount - 1, ReplicationPriorityCount - 1), 0);

                    for (auto replica : chunk->StoredReplicas()) {
                        TChunkPtrWithIndexes chunkWithIndexes(chunk, replica.GetReplicaIndex(), replica.GetMediumIndex());

                        // If chunk is lost on some media, don't match dst medium with
                        // src medium: we want to be able to do cross-medium replication.
                        bool mediumMatches =
                            Any(allMediaStatistics.Status & ECrossMediumChunkStatus::MediumWiseLost) ||
                            mediumIndex == replica.GetMediumIndex();

                        if (mediumMatches &&
                            (chunk->IsRegular() ||
                             chunk->IsErasure() && replica.GetReplicaIndex() == replicaIndex ||
                             chunk->IsJournal() && replica.GetReplicaIndex() == SealedChunkReplicaIndex))
                        {
                            auto* node = replica.GetPtr();
                            if (node->GetLocalState() == ENodeState::Online) {
                                node->AddToChunkReplicationQueue(chunkWithIndexes, mediumIndex, priority);
                            }
                        }
                    }
                }
            }

            if (Any(statistics.Status & EChunkStatus::Sealed)) {
                Y_ASSERT(chunk->IsJournal());
                for (auto replica : chunk->StoredReplicas()) {
                    if (replica.GetMediumIndex() == mediumIndex &&
                        replica.GetReplicaIndex() == UnsealedChunkReplicaIndex)
                    {
                        auto* node = replica.GetPtr();
                        if (node->GetLocalState() == ENodeState::Online) {
                            TChunkPtrWithIndexes chunkWithIndexes(chunk, GenericChunkReplicaIndex, mediumIndex);
                            node->AddToChunkSealQueue(chunkWithIndexes);
                        }
                    }
                }
            }

            if (Any(statistics.Status & (EChunkStatus::DataMissing | EChunkStatus::ParityMissing)) &&
                None(statistics.Status & EChunkStatus::Lost))
            {
                TChunkPtrWithIndexes chunkWithIndexes(chunk, GenericChunkReplicaIndex, mediumIndex);
                AddToChunkRepairQueue(chunkWithIndexes);
            }
        }
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::Lost)) {
        YCHECK(LostChunks_.insert(chunk).second);
        if (durabilityRequired) {
            YCHECK(LostVitalChunks_.insert(chunk).second);
        }
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::DataMissing)) {
        Y_ASSERT(chunk->IsErasure());
        YCHECK(DataMissingChunks_.insert(chunk).second);
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::ParityMissing)) {
        Y_ASSERT(chunk->IsErasure());
        YCHECK(ParityMissingChunks_.insert(chunk).second);
    }

    if (Any(allMediaStatistics.Status & ECrossMediumChunkStatus::Precarious)) {
        YCHECK(PrecariousChunks_.insert(chunk).second);
        if (durabilityRequired) {
            YCHECK(PrecariousVitalChunks_.insert(chunk).second);
        }
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
    }

    if (chunk->IsJournal()) {
        QuorumMissingChunks_.erase(chunk);
    }
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

    for (const auto& pair : Bootstrap_->GetChunkManager()->Media()) {
        auto* medium = pair.second;
        if (medium->GetCache()) {
            continue;
        }

        // Remove from repair queue.
        auto mediumIndex = medium->GetIndex();
        TChunkPtrWithIndexes chunkWithIndexes(chunk, GenericChunkReplicaIndex, mediumIndex);
        RemoveFromChunkRepairQueue(chunkWithIndexes);
    }
}

void TChunkReplicator::RemoveChunkFromQueuesOnDestroy(TChunk* chunk)
{
    // Remove chunk from replication and seal queues.
    for (auto replica : chunk->StoredReplicas()) {
        auto* node = replica.GetPtr();
        TChunkPtrWithIndexes chunkWithIndexes(chunk, replica.GetReplicaIndex(), replica.GetMediumIndex());
        // NB: Keep existing removal requests to workaround the following scenario:
        // 1) the last strong reference to a chunk is released while some weak references
        //    remain; the chunk becomes a zombie;
        // 2) a node sends a heartbeat reporting addition of the chunk;
        // 3) master receives the heartbeat and puts the chunk into the removal queue
        //    without (sic!) registering a replica;
        // 4) the last weak reference is dropped, the chunk is being removed;
        //    at this point we must preserve its removal request in the queue.
        node->RemoveFromChunkReplicationQueues(chunkWithIndexes, AllMediaIndex);
        node->RemoveFromChunkSealQueue(chunkWithIndexes);
    }

    // Remove chunk from repair queues.
    if (chunk->IsErasure()) {
        for (int mediumIndex = 0; mediumIndex < MaxMediumCount; ++mediumIndex) {
            TChunkPtrWithIndexes chunkPtrWithIndexes(chunk, GenericChunkReplicaIndex, mediumIndex);
            RemoveFromChunkRepairQueue(chunkPtrWithIndexes);
        }
    }
}

void TChunkReplicator::CancelChunkJobs(TChunk* chunk)
{
    auto job = chunk->GetJob();
    if (job) {
        LOG_DEBUG("Job canceled (JobId: %v)", job->GetJobId());
        UnregisterJob(job, EJobUnregisterFlags::UnregisterFromNode);
    }
}

bool TChunkReplicator::IsReplicaDecommissioned(TNodePtrWithIndexes replica)
{
    auto* node = replica.GetPtr();
    return node->GetDecommissioned();
}

void TChunkReplicator::ScheduleChunkRefresh(TChunk* chunk)
{
    if (!IsObjectAlive(chunk)) {
        return;
    }

    if (chunk->IsForeign()) {
        return;
    }

    RefreshScanner_->EnqueueChunk(chunk);
}

void TChunkReplicator::ScheduleNodeRefresh(TNode* node)
{
    const auto& chunkManager = Bootstrap_->GetChunkManager();
    for (int mediumIndex = 0; mediumIndex < MaxMediumCount; ++mediumIndex) {
        const auto* medium = chunkManager->FindMediumByIndex(mediumIndex);
        if (!medium || medium->GetCache()) {
            continue;
        }
        const auto& replicas = node->Replicas()[mediumIndex];
        for (auto replica : replicas) {
            ScheduleChunkRefresh(replica.GetPtr());
        }
    }
}

void TChunkReplicator::OnRefresh()
{
    int totalCount = 0;
    int aliveCount = 0;
    NProfiling::TWallTimer timer;

    LOG_DEBUG("Incremental chunk refresh iteration started");

    PROFILE_AGGREGATED_TIMING (RefreshTimeCounter) {
        auto deadline = GetCpuInstant() - ChunkRefreshDelay_;
        while (totalCount < Config_->MaxChunksPerRefresh &&
               RefreshScanner_->HasUnscannedChunk(deadline))
        {
            if (timer.GetElapsedTime() > Config_->MaxTimePerRefresh) {
                break;
            }

            ++totalCount;
            auto* chunk = RefreshScanner_->DequeueChunk();
            if (!chunk) {
                continue;
            }

            RefreshChunk(chunk);
            ++aliveCount;
        }
    }

    LOG_DEBUG("Incremental chunk refresh iteration completed (TotalCount: %v, AliveCount: %v)",
        totalCount,
        aliveCount);
}

bool TChunkReplicator::IsEnabled()
{
    return Enabled_.Get(false);
}

void TChunkReplicator::OnCheckEnabled()
{
    const auto& worldInitializer = Bootstrap_->GetWorldInitializer();
    if (!worldInitializer->IsInitialized()) {
        return;
    }

    try {
        if (Bootstrap_->IsPrimaryMaster()) {
            OnCheckEnabledPrimary();
        } else {
            OnCheckEnabledSecondary();
        }
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Error updating chunk ```replicator state, disabling until the next attempt");
        Enabled_ = false;
    }
}

void TChunkReplicator::OnCheckEnabledPrimary()
{
    if (!Bootstrap_->GetConfigManager()->GetConfig()->EnableChunkReplicator) {
        if (!Enabled_ || *Enabled_) {
            LOG_INFO("Chunk replicator is disabled, see //sys/@config");
        }
        Enabled_ = false;
        return;
    }

    const auto& nodeTracker = Bootstrap_->GetNodeTracker();
    int needOnline = Config_->SafeOnlineNodeCount;
    int gotOnline = nodeTracker->GetOnlineNodeCount();
    if (gotOnline < needOnline) {
        if (!Enabled_ || *Enabled_) {
            LOG_INFO("Chunk replicator disabled: too few online nodes, needed >= %v but got %v",
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
    int needLostChunkCount = Config_->SafeLostChunkCount;
    if (gotChunkCount > 0) {
        double needFraction = Config_->SafeLostChunkFraction;
        double gotFraction = (double) gotLostChunkCount / gotChunkCount;
        if (gotFraction > needFraction) {
            if (!Enabled_ || *Enabled_) {
                LOG_INFO("Chunk replicator disabled: too many lost chunks, fraction needed <= %v but got %v",
                    needFraction,
                    gotFraction);
            }
            Enabled_ = false;
            return;
        }
    }

    if (gotLostChunkCount > needLostChunkCount) {
        if (!Enabled_ || *Enabled_) {
            LOG_INFO("Chunk replicator disabled: too many lost chunks, needed <= %v but got %v",
                needLostChunkCount,
                gotLostChunkCount);
        }
        Enabled_ = false;
        return;
    }

    if (!Enabled_ || !*Enabled_) {
        LOG_INFO("Chunk replicator enabled");
    }
    Enabled_ = true;
}

void TChunkReplicator::OnCheckEnabledSecondary()
{
    const auto& multicellManager = Bootstrap_->GetMulticellManager();
    auto channel = multicellManager->GetMasterChannelOrThrow(Bootstrap_->GetPrimaryCellTag(), EPeerKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto req = TYPathProxy::Get("//sys/@chunk_replicator_enabled");
    auto rsp = WaitFor(proxy.Execute(req))
        .ValueOrThrow();

    auto value = ConvertTo<bool>(TYsonString(rsp->value()));
    if (!Enabled_ || value != *Enabled_) {
        if (value) {
            LOG_INFO("Chunk replicator enabled at primary master");
        } else {
            LOG_INFO("Chunk replicator disabled at primary master");
        }
        Enabled_ = value;
    }
}

int TChunkReplicator::GetRefreshQueueSize() const
{
    return RefreshScanner_->GetQueueSize();
}

int TChunkReplicator::GetPropertiesUpdateQueueSize() const
{
    return PropertiesUpdateScanner_->GetQueueSize();
}

void TChunkReplicator::SchedulePropertiesUpdate(TChunkTree* chunkTree)
{
    switch (chunkTree->GetType()) {
        case EObjectType::Chunk:
        case EObjectType::ErasureChunk:
            // Erasure chunks have no RF but still can update Vital.
            SchedulePropertiesUpdate(chunkTree->AsChunk());
            break;

        case EObjectType::ChunkList:
            SchedulePropertiesUpdate(chunkTree->AsChunkList());
            break;

        default:
            Y_UNREACHABLE();
    }
}

void TChunkReplicator::SchedulePropertiesUpdate(TChunkList* chunkList)
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
            , RootId_(Root_->GetId())
        { }

        void Run()
        {
            auto callbacks = CreatePreemptableChunkTraverserCallbacks(
                Bootstrap_,
                NCellMaster::EAutomatonThreadQueue::ChunkPropertiesUpdateTraverser);
            TraverseChunkTree(std::move(callbacks), this, Root_);
        }

    private:
        TBootstrap* const Bootstrap_;
        const TChunkReplicatorPtr Owner_;
        TChunkList* Root_;
        const TChunkListId RootId_;

        virtual bool OnChunk(
            TChunk* chunk,
            i64 /*rowIndex*/,
            const TReadLimit& /*startLimit*/,
            const TReadLimit& /*endLimit*/) override
        {
            Owner_->SchedulePropertiesUpdate(chunk);
            return true;
        }

        virtual void OnFinish(const TError& error) override
        {
            if (!error.IsOK()) {
                // Try restarting.
                const auto& chunkManager = Bootstrap_->GetChunkManager();
                Root_ = chunkManager->FindChunkList(RootId_);
                if (!IsObjectAlive(Root_)) {
                    return;
                }

                Run();
            }
        }
    };

    New<TVisitor>(Bootstrap_, this, chunkList)->Run();
}

void TChunkReplicator::SchedulePropertiesUpdate(TChunk* chunk)
{
    if (!IsObjectAlive(chunk)) {
        return;
    }

    PropertiesUpdateScanner_->EnqueueChunk(chunk);
}

void TChunkReplicator::OnPropertiesUpdate()
{
    if (!Bootstrap_->GetHydraFacade()->GetHydraManager()->IsActiveLeader()) {
        return;
    }

    TReqUpdateChunkProperties request;
    request.set_cell_tag(Bootstrap_->GetCellTag());

    int totalCount = 0;
    int aliveCount = 0;
    NProfiling::TWallTimer timer;

    LOG_DEBUG("Chunk properties update iteration started");

    PROFILE_AGGREGATED_TIMING (PropertiesUpdateTimeCounter) {
        while (totalCount < Config_->MaxChunksPerPropertiesUpdate &&
               PropertiesUpdateScanner_->HasUnscannedChunk())
        {
            if (timer.GetElapsedTime() > Config_->MaxTimePerPropertiesUpdate) {
                break;
            }

            ++totalCount;
            auto* chunk = PropertiesUpdateScanner_->DequeueChunk();
            if (!chunk) {
                continue;
            }

            UpdateChunkProperties(chunk, &request);
            ++aliveCount;
        }
    }

    LOG_DEBUG("Chunk properties update iteration completed (TotalCount: %v, AliveCount: %v, UpdateCount: %v)",
        totalCount,
        aliveCount,
        request.updates_size());

    if (request.updates_size() > 0) {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto asyncResult = chunkManager
            ->CreateUpdateChunkPropertiesMutation(request)
            ->CommitAndLog(Logger);
        Y_UNUSED(WaitFor(asyncResult));
    }
}

void TChunkReplicator::UpdateChunkProperties(TChunk* chunk, TReqUpdateChunkProperties* request)
{
    const auto& oldProperties = chunk->LocalProperties();
    auto newProperties = ComputeChunkProperties(chunk);
    if (newProperties != oldProperties) {
        auto* update = request->add_updates();
        ToProto(update->mutable_chunk_id(), chunk->GetId());
        update->set_vital(newProperties.GetVital());
        int mediumIndex = 0;
        for (const auto& mediumProperties : newProperties) {
            auto* mediumUpdate = update->add_medium_updates();
            mediumUpdate->set_medium_index(mediumIndex);
            mediumUpdate->set_replication_factor(mediumProperties.GetReplicationFactor());
            mediumUpdate->set_data_parts_only(mediumProperties.GetDataPartsOnly());
            ++mediumIndex;
        }
    }
}

TChunkProperties TChunkReplicator::ComputeChunkProperties(TChunk* chunk)
{
    bool found = false;
    TChunkProperties properties;
    // Below, properties of this chunk's owners are combined together. Since
    // 'data parts only' flags are combined by ANDing, we should start with
    // true to avoid affecting the result.
    for (auto& mediumProperties : properties) {
        mediumProperties.SetDataPartsOnly(true);
    }

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

    // Put seeds into the queue.
    for (auto* parent : chunk->Parents()) {
        auto* adjustedParent = FollowParentLinks(parent);
        if (adjustedParent) {
            enqueue(adjustedParent);
        }
    }

    // The main BFS loop.
    while (frontIndex < queue.size()) {
        auto* chunkList = queue[frontIndex++];

        // Examine owners, if any.
        for (const auto* owningNode : chunkList->TrunkOwningNodes()) {
            // Overloaded; MAXes replication factors, ANDs "data-part-only"s,
            // ORs vitalities.
            properties |= owningNode->Properties();
            found = true;
        }

        // Proceed to parents.
        for (auto* parent : chunkList->Parents()) {
            auto* adjustedParent = FollowParentLinks(parent);
            if (adjustedParent) {
                enqueue(adjustedParent);
            }
        }
    }

    if (chunk->IsErasure()) {
        static_assert(MinReplicationFactor <= 1 && 1 <= MaxReplicationFactor,
                     "Replication factor limits are incorrect.");
        for (auto& mediumProperties : properties) {
            if (mediumProperties) {
                mediumProperties.SetReplicationFactor(1);
            }
        }
    }

    Y_ASSERT(!found || properties.IsValid());

    return found ? properties : chunk->LocalProperties();
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

void TChunkReplicator::RegisterJob(const TJobPtr& job)
{
    YCHECK(JobMap_.insert(std::make_pair(job->GetJobId(), job)).second);
    UpdateJobCountGauge(job->GetType(), +1);
    YCHECK(job->GetNode()->Jobs().insert(job).second);

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    auto chunkId = job->GetChunkIdWithIndexes().Id;
    auto* chunk = chunkManager->FindChunk(chunkId);
    if (chunk) {
        chunk->SetJob(job);
    }

    UpdateInterDCEdgeConsumption(job, job->GetNode()->GetDataCenter(), +1);
}

void TChunkReplicator::UnregisterJob(const TJobPtr& job, EJobUnregisterFlags flags)
{
    YCHECK(JobMap_.erase(job->GetJobId()) == 1);
    UpdateJobCountGauge(job->GetType(), -1);

    if (Any(flags & EJobUnregisterFlags::UnregisterFromNode)) {
        YCHECK(job->GetNode()->Jobs().erase(job) == 1);
    }

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    auto chunkId = job->GetChunkIdWithIndexes().Id;
    auto* chunk = chunkManager->FindChunk(chunkId);
    if (chunk) {
        chunk->SetJob(nullptr);
        if (Any(flags & EJobUnregisterFlags::ScheduleChunkRefresh)) {
            ScheduleChunkRefresh(chunk);
        }
    }

    UpdateInterDCEdgeConsumption(job, job->GetNode()->GetDataCenter(), -1);
}

void TChunkReplicator::UpdateJobCountGauge(EJobType jobType, int delta)
{
    switch (jobType) {
        case EJobType::ReplicateChunk:
        case EJobType::RemoveChunk:
        case EJobType::RepairChunk:
        case EJobType::SealChunk:
            JobCounters_[jobType] += delta;
            break;
        default:
            Y_UNREACHABLE();
    }
}

void TChunkReplicator::HandleNodeDataCenterChange(TNode* node, TDataCenter* oldDataCenter)
{
    Y_ASSERT(node->GetDataCenter() != oldDataCenter);

    for (const auto& job : node->Jobs()) {
        UpdateInterDCEdgeConsumption(job, oldDataCenter, -1);
        UpdateInterDCEdgeConsumption(job, node->GetDataCenter(), +1);
    }
}

void TChunkReplicator::AddToChunkRepairQueue(TChunkPtrWithIndexes chunkWithIndexes)
{
    Y_ASSERT(chunkWithIndexes.GetReplicaIndex() == GenericChunkReplicaIndex);
    auto* chunk = chunkWithIndexes.GetPtr();
    int mediumIndex = chunkWithIndexes.GetMediumIndex();
    YCHECK(chunk->GetRepairQueueIterator(mediumIndex) == TChunkRepairQueueIterator());
    auto& chunkRepairQueue = ChunkRepairQueues_[mediumIndex];
    auto it = chunkRepairQueue.insert(chunkRepairQueue.end(), chunkWithIndexes);
    chunk->SetRepairQueueIterator(mediumIndex, it);
}

void TChunkReplicator::RemoveFromChunkRepairQueue(TChunkPtrWithIndexes chunkWithIndexes)
{
    Y_ASSERT(chunkWithIndexes.GetReplicaIndex() == GenericChunkReplicaIndex);
    auto* chunk = chunkWithIndexes.GetPtr();
    int mediumIndex = chunkWithIndexes.GetMediumIndex();
    auto it = chunk->GetRepairQueueIterator(mediumIndex);
    if (it != TChunkRepairQueueIterator()) {
        ChunkRepairQueues_[mediumIndex].erase(it);
        chunk->SetRepairQueueIterator(mediumIndex, TChunkRepairQueueIterator());
    }
}

void TChunkReplicator::InitInterDCEdges()
{
    UpdateInterDCEdgeCapacities();
    UpdateUnsaturatedInterDCEdges();
}

void TChunkReplicator::UpdateInterDCEdgeCapacities()
{
    if (GetCpuInstant() - InterDCEdgeCapacitiesLastUpdateTime <= Config_->InterDCLimits->GetUpdateInterval()) {
        return;
    }

    // This will soon be replaced by getting capacities from node tracker.

    InterDCEdgeCapacities_.clear();

    auto capacities = Config_->InterDCLimits->GetCapacities();
    auto secondaryCellCount = Bootstrap_->GetSecondaryCellTags().size();
    secondaryCellCount = std::max<int>(secondaryCellCount, 1);

    const auto& nodeTracker = Bootstrap_->GetNodeTracker();

    auto updateForSrcDC = [&] (const TDataCenter* srcDataCenter, const TNullable<TString>& srcDataCenterName) {
        auto& interDCEdgeCapacities = InterDCEdgeCapacities_[srcDataCenter];
        const auto& newInterDCEdgeCapacities = capacities[srcDataCenterName];

        auto updateForDstDC = [&] (const TDataCenter* dstDataCenter, const TNullable<TString>& dstDataCenterName) {
            auto it = newInterDCEdgeCapacities.find(dstDataCenterName);
            if (it != newInterDCEdgeCapacities.end()) {
                interDCEdgeCapacities[dstDataCenter] = it->second / secondaryCellCount;
            }
        };

        updateForDstDC(nullptr, Null);
        for (const auto& pair : nodeTracker->DataCenters()) {
            if (IsObjectAlive(pair.second)) {
                updateForDstDC(pair.second, pair.second->GetName());
            }
        }
    };

    updateForSrcDC(nullptr, Null);
    for (const auto& pair : nodeTracker->DataCenters()) {
        if (IsObjectAlive(pair.second)) {
            updateForSrcDC(pair.second, pair.second->GetName());
        }
    }

    InterDCEdgeCapacitiesLastUpdateTime = GetCpuInstant();
}

void TChunkReplicator::UpdateUnsaturatedInterDCEdges()
{
    UnsaturatedInterDCEdges.clear();

    const auto& nodeTracker = Bootstrap_->GetNodeTracker();

    const auto defaultCapacity =
        Config_->InterDCLimits->GetDefaultCapacity() / std::max<int>(Bootstrap_->GetSecondaryCellTags().size(), 1);

    auto updateForSrcDC = [&] (const TDataCenter* srcDataCenter) {
        auto& interDCEdgeConsumption = InterDCEdgeConsumption_[srcDataCenter];
        const auto& interDCEdgeCapacities = InterDCEdgeCapacities_[srcDataCenter];

        auto updateForDstDC = [&] (const TDataCenter* dstDataCenter) {
            if (interDCEdgeConsumption.Value(dstDataCenter, 0) <
                interDCEdgeCapacities.Value(dstDataCenter, defaultCapacity))
            {
                UnsaturatedInterDCEdges[srcDataCenter].insert(dstDataCenter);
            }
        };

        updateForDstDC(nullptr);
        for (const auto& pair : nodeTracker->DataCenters()) {
            if (IsObjectAlive(pair.second)) {
                updateForDstDC(pair.second);
            }
        }
    };

    updateForSrcDC(nullptr);
    for (const auto& pair : nodeTracker->DataCenters()) {
        if (IsObjectAlive(pair.second)) {
            updateForSrcDC(pair.second);
        }
    }
}

void TChunkReplicator::UpdateInterDCEdgeConsumption(
    const TJobPtr& job,
    const TDataCenter* srcDataCenter,
    int sizeMultiplier)
{
    if (job->GetType() != EJobType::ReplicateChunk &&
        job->GetType() != EJobType::RepairChunk)
    {
        return;
    }

    auto& interDCEdgeConsumption = InterDCEdgeConsumption_[srcDataCenter];
    const auto& interDCEdgeCapacities = InterDCEdgeCapacities_[srcDataCenter];

    const auto defaultCapacity =
        Config_->InterDCLimits->GetDefaultCapacity() / std::max<int>(Bootstrap_->GetSecondaryCellTags().size(), 1);

    for (const auto& nodePtrWithIndexes : job->TargetReplicas()) {
        const auto* dstDataCenter = nodePtrWithIndexes.GetPtr()->GetDataCenter();

        i64 chunkPartSize = 0;
        switch (job->GetType()) {
            case EJobType::ReplicateChunk:
                chunkPartSize = job->ResourceUsage().replication_data_size();
                break;
            case EJobType::RepairChunk:
                chunkPartSize = job->ResourceUsage().repair_data_size();
                break;
            default:
                Y_UNREACHABLE();
        }

        auto& consumption = interDCEdgeConsumption[dstDataCenter];
        consumption += sizeMultiplier * chunkPartSize;

        if (consumption < interDCEdgeCapacities.Value(dstDataCenter, defaultCapacity)) {
            UnsaturatedInterDCEdges[srcDataCenter].insert(dstDataCenter);
        } else {
            auto it = UnsaturatedInterDCEdges.find(srcDataCenter);
            if (it != UnsaturatedInterDCEdges.end()) {
                it->second.erase(dstDataCenter);
                // Don't do UnsaturatedInterDCEdges.erase(it) here - the memory
                // saving is negligible, but the slowdown may be noticeable. Plus,
                // the removal is very likely to be undone by a soon-to-follow insertion.
            }
        }
    }
}

bool TChunkReplicator::HasUnsaturatedInterDCEdgeStartingFrom(const TDataCenter* srcDataCenter)
{
    return !UnsaturatedInterDCEdges[srcDataCenter].empty();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
