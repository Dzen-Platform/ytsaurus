#include "chunk_replicator.h"
#include "private.h"
#include "chunk.h"
#include "chunk_list.h"
#include "chunk_owner_base.h"
#include "chunk_placement.h"
#include "chunk_tree_traversing.h"
#include "job.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/config.h>
#include <yt/server/cell_master/hydra_facade.h>
#include <yt/server/cell_master/world_initializer.h>
#include <yt/server/cell_master/multicell_manager.h>
#include <yt/server/cell_master/multicell_manager.pb.h>

#include <yt/server/chunk_server/chunk_manager.h>

#include <yt/server/cypress_server/node.h>
#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/node_tracker_server/node.h>
#include <yt/server/node_tracker_server/node_directory_builder.h>
#include <yt/server/node_tracker_server/node_tracker.h>

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
    , JobThrottler_(
        CreateReconfigurableThroughputThrottler(
            Config_->JobThrottler,
            ChunkServerLogger,
            NProfiling::TProfiler(ChunkServerProfiler.GetPathPrefix() + "/job_throttler")))
{
    YCHECK(Config_);
    YCHECK(Bootstrap_);
    YCHECK(ChunkPlacement_);
}

void TChunkReplicator::Start()
{
    RefreshExecutor_ = New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::ChunkMaintenance),
        BIND(&TChunkReplicator::OnRefresh, MakeWeak(this)),
        Config_->ChunkRefreshPeriod);
    RefreshExecutor_->Start();

    PropertiesUpdateExecutor_ = New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::ChunkMaintenance),
        BIND(&TChunkReplicator::OnPropertiesUpdate, MakeWeak(this)),
        Config_->ChunkPropertiesUpdatePeriod);
    PropertiesUpdateExecutor_->Start();

    EnabledCheckExecutor_ = New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::Default),
        BIND(&TChunkReplicator::OnCheckEnabled, MakeWeak(this)),
        Config_->ReplicatorEnabledCheckPeriod);
    EnabledCheckExecutor_->Start();
}

void TChunkReplicator::Stop()
{
    auto nodeTracker = Bootstrap_->GetNodeTracker();
    for (const auto& pair : nodeTracker->Nodes()) {
        auto* node = pair.second;
        node->Jobs().clear();
    }

    RefreshExecutor_.Reset();
    PropertiesUpdateExecutor_.Reset();
    EnabledCheckExecutor_.Reset();
}

void TChunkReplicator::TouchChunk(TChunk* chunk)
{
    auto repairIt = chunk->GetRepairQueueIterator();
    if (repairIt) {
        auto mediumIndex = (*repairIt)->second;
        ChunkRepairQueue_.erase(*repairIt);
        auto newRepairIt = ChunkRepairQueue_.insert(ChunkRepairQueue_.begin(), {chunk, mediumIndex});
        chunk->SetRepairQueueIterator(newRepairIt);
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
    TPerMediumArray<std::array<ui8, MaxRackCount + 1>> perRackReplicaCounters{};

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
            int maxReplicasPerRack = std::min(
                Config_->MaxReplicasPerRack,
                chunk->GetMaxReplicasPerRack(mediumIndex, Null));
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
    // The only inter-medium thing left do is to kickstart replication of chunks
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
    TPerMediumArray<std::array<ui8, MaxRackCount + 1>> perRackReplicaCounters{};
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
        if (IsReplicaDecommissioned(replica) || node->GetVisitMark() == mark) {
            ++mediumStatistics.DecommissionedReplicaCount[replicaIndex];
            decommissionedReplicas[mediumIndex][replicaIndex].push_back(replica);
            ++totalDecommissionedReplicaCounts[mediumIndex];
        } else {
            ++mediumStatistics.ReplicaCount[replicaIndex];
            ++totalReplicaCounts[mediumIndex];
        }
        node->SetVisitMark(mark);
        const auto* rack = node->GetRack();
        if (rack) {
            int rackIndex = rack->GetIndex();
            int maxReplicasPerRack = std::min(
                Config_->MaxReplicasPerRack,
                chunk->GetMaxReplicasPerRack(mediumIndex, Null));
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
        mediumToErasedIndexes);

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
    const TPerMediumArray<NErasure::TPartIndexSet>& mediumToErasedIndexes)
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
    TChunkStatistics results;
    // Journal chunks never use non-default media.
    auto& result = results.PerMediumStatistics[DefaultMediumIndex];

    auto replicationFactors = chunk->ComputeReplicationFactors();
    int replicationFactor = replicationFactors[DefaultMediumIndex];
    int readQuorum = chunk->GetReadQuorum();
    int maxReplicasPerRack = std::min(Config_->MaxReplicasPerRack, chunk->GetMaxReplicasPerRack(DefaultMediumIndex, Null));

    int replicaCount = 0;
    int decommissionedReplicaCount = 0;
    int sealedReplicaCount = 0;
    int unsealedReplicaCount = 0;
    TNodePtrWithIndexesList decommissionedReplicas;
    std::array<ui8, MaxRackCount + 1> perRackReplicaCounters{};
    bool hasUnsafelyPlacedReplicas = false;

    for (auto replica : chunk->StoredReplicas()) {
        Y_ASSERT(replica.GetMediumIndex() == DefaultMediumIndex);

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
    ResetChunkStatus(chunk);
    // NB: Keep existing removal requests to workaround the following scenario:
    // 1) the last strong reference to a chunk is released while some weak references
    //    remain; the chunk becomes a zombie;
    // 2) a node sends a heartbeat reporting addition of the chunk;
    // 3) master receives the heartbeat and puts the chunk into the removal queue
    //    without (sic!) registering a replica;
    // 4) the last weak reference is dropped, the chunk is being removed;
    //    at this point we must preserve its removal request in the queue.
    RemoveChunkFromQueues(chunk, AllMediaIndex, false);
    CancelChunkJobs(chunk);
}

void TChunkReplicator::OnReplicaRemoved(
    TNode* node,
    TChunkPtrWithIndexes chunkWithIndexes,
    ERemoveReplicaReason reason)
{
    RemoveReplicaFromQueues(
        chunkWithIndexes.GetPtr(),
        TNodePtrWithIndexes(node, chunkWithIndexes.GetReplicaIndex(), chunkWithIndexes.GetMediumIndex()),
        reason != ERemoveReplicaReason::ChunkDestroyed);
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
    TJobPtr* job)
{
    auto* chunk = chunkWithIndexes.GetPtr();
    int replicaIndex = chunkWithIndexes.GetReplicaIndex();
    int mediumIndex = chunkWithIndexes.GetMediumIndex();

    if (!IsObjectAlive(chunk)) {
        return true;
    }

    if (chunk->GetRefreshScheduled()) {
        return true;
    }

    if (chunk->IsJobScheduled()) {
        return true;
    }

    auto replicationFactor = chunk->ComputeReplicationFactor(mediumIndex);
    auto statistics = ComputeChunkStatistics(chunk);
    const auto& mediumStatistics = statistics.PerMediumStatistics[mediumIndex];
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
        mediumIndex,
        chunk,
        replicasNeeded,
        1,
        Null,
        ESessionType::Replication);
    if (targetNodes.empty()) {
        return false;
    }

    *job = TJob::CreateReplicate(
        GenerateJobId(),
        chunkWithIndexes,
        sourceNode,
        targetNodes);

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
    auto mediumIndex = chunkWithIndexes.GetMediumIndex();
    if (chunk->GetRefreshScheduled()) {
        return true;
    }

    if (chunk->IsJobScheduled()) {
        return true;
    }

    auto* targetNode = ChunkPlacement_->AllocateBalancingTarget(mediumIndex, chunk, maxFillFactor);
    if (!targetNode) {
        return false;
    }

    *job = TJob::CreateReplicate(
        GenerateJobId(),
        chunkWithIndexes,
        sourceNode,
        TNodeList(1, targetNode));

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
    auto chunkManager = Bootstrap_->GetChunkManager();
    auto* chunk = chunkManager->FindChunk(chunkIdWithIndexes.Id);
    // NB: Allow more than one job for dead chunks.
    if (IsObjectAlive(chunk)) {
        if (chunk->GetRefreshScheduled()) {
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
    TChunk* chunk,
    int mediumIndex,
    TJobPtr* job)
{
    YCHECK(chunk->IsErasure());

    if (!IsObjectAlive(chunk)) {
        return true;
    }

    if (chunk->GetRefreshScheduled()) {
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

    NErasure::TPartIndexList erasedIndexes;
    for (int index = 0; index < totalPartCount; ++index) {
        if (mediumStatistics.ReplicaCount[index] == 0 &&
            mediumStatistics.DecommissionedReplicaCount[index] == 0)
        {
            erasedIndexes.push_back(index);
        }
    }

    int erasedIndexCount = static_cast<int>(erasedIndexes.size());
    if (erasedIndexCount == 0) {
        return true;
    }

    auto targetNodes = ChunkPlacement_->AllocateWriteTargets(
        mediumIndex,
        chunk,
        erasedIndexCount,
        erasedIndexCount,
        Null,
        ESessionType::Repair);
    if (targetNodes.empty()) {
        return false;
    }

    *job = TJob::CreateRepair(
        GenerateJobId(),
        chunk,
        mediumIndex,
        node,
        targetNodes,
        erasedIndexes,
        Config_->RepairJobMemoryUsage);

    LOG_DEBUG("Repair job scheduled (JobId: %v, Address: %v, ChunkId: %v, TargetAddresses: %v, ErasedIndexes: %v)",
        (*job)->GetJobId(),
        node->GetDefaultAddress(),
        chunk->GetId(),
        MakeFormattableRange(targetNodes, TNodePtrAddressFormatter()),
        erasedIndexes);

    return true;
}

bool TChunkReplicator::CreateSealJob(
    TNode* node,
    TChunk* chunk,
    TJobPtr* job)
{
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
        chunk,
        node);

    LOG_DEBUG("Seal job scheduled (JobId: %v, Address: %v, ChunkId: %v)",
        (*job)->GetJobId(),
        node->GetDefaultAddress(),
        chunk->GetId());

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

    auto registerJob = [&] (const TJobPtr& job) {
        if (job) {
            resourceUsage += job->ResourceUsage();
            jobsToStart->push_back(job);
            RegisterJob(job);
            JobThrottler_->Acquire(1);
        }
    };

    // NB: Beware of chunks larger than the limit; we still need to be able to replicate them one by one.
    auto hasSpareReplicationResources = [&] () {
        return
            resourceUsage.replication_slots() < resourceLimits.replication_slots() &&
            (resourceUsage.replication_slots() == 0 || resourceUsage.replication_data_size() < resourceLimits.replication_data_size());
    };

    // NB: Beware of chunks larger than the limit; we still need to be able to repair them one by one.
    auto hasSpareRepairResources = [&] () {
        return
            resourceUsage.repair_slots() < resourceLimits.repair_slots() &&
            (resourceUsage.repair_slots() == 0 || resourceUsage.repair_data_size() < resourceLimits.repair_data_size());
    };

    auto hasSpareSealResources = [&] () {
        return resourceUsage.seal_slots() < resourceLimits.seal_slots();
    };

    if (IsEnabled()) {
        // Schedule replication jobs.
        for (auto& queue : node->ChunkReplicationQueues()) {
            auto it = queue.begin();
            while (it != queue.end() && hasSpareReplicationResources()) {
                auto jt = it++;
                const auto& chunkWithIndex = jt->first;
                auto& mediumIndexSet = jt->second;
                for (int mediumIndex = 0; mediumIndex < mediumIndexSet.size(); ++mediumIndex) {
                    if (mediumIndexSet.test(mediumIndex)) {
                        TChunkPtrWithIndexes chunkWithIndexes(
                            chunkWithIndex.GetPtr(),
                            chunkWithIndex.GetIndex(),
                            mediumIndex);

                        TJobPtr job;
                        if (CreateReplicationJob(node, chunkWithIndexes, &job)) {
                            mediumIndexSet.reset(mediumIndex);
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
            auto it = ChunkRepairQueue_.begin();
            while (it != ChunkRepairQueue_.end() && hasSpareRepairResources()) {
                auto jt = it++;
                auto* chunk = (*jt).first;
                auto mediumIndex = (*jt).second;

                TJobPtr job;
                if (CreateRepairJob(node, chunk, mediumIndex, &job)) {
                    chunk->SetRepairQueueIterator(Null);
                    ChunkRepairQueue_.erase(jt);
                }
                registerJob(std::move(job));
            }
        }

        // Schedule removal jobs.
        {
            auto& queue = node->ChunkRemovalQueue();
            auto it = queue.begin();
            while (it != queue.end()) {
                if (resourceUsage.removal_slots() >= resourceLimits.removal_slots()) {
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
                ChunkPlacement_->HasBalancingTargets(mediumIndex, targetFillFactor))
            {
                int maxJobs = std::max(0, resourceLimits.replication_slots() - resourceUsage.replication_slots());
                auto chunksToBalance =
                    ChunkPlacement_->GetBalancingChunks(mediumIndex, node, maxJobs);
                for (auto chunkWithIndexes : chunksToBalance) {
                    if (!hasSpareReplicationResources()) {
                        break;
                    }

                    TJobPtr job;
                    CreateBalancingJob(node, chunkWithIndexes, targetFillFactor, &job);
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
            auto* chunk = *jt;

            TJobPtr job;
            if (CreateSealJob(node, chunk, &job)) {
                queue.erase(jt);
            }
            registerJob(std::move(job));
        }
    }
}

void TChunkReplicator::RefreshChunk(TChunk* chunk)
{
    if (!chunk->IsConfirmed()) {
        return;
    }

    ResetChunkStatus(chunk);

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
            // This medium is irrelevant for this chunk. Continue.
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
            RemoveChunkFromQueues(chunk, mediumIndex, true);

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

            // This check may yield true even for lost chunks when inter-medium replication is in progress.
            if (Any(statistics.Status & (EChunkStatus::Underreplicated | EChunkStatus::UnsafelyPlaced))) {
                for (auto replicaIndex : statistics.ReplicationIndexes) {
                    TChunkPtrWithIndexes chunkWithIndexes(chunk, replicaIndex, mediumIndex);

                    // Cap replica count minus one against the range [0, ReplicationPriorityCount - 1].
                    int replicaCount = statistics.ReplicaCount[replicaIndex];
                    int priority = std::max(std::min(replicaCount - 1, ReplicationPriorityCount - 1), 0);

                    for (auto replica : chunk->StoredReplicas()) {
                        // If chunk is lost on some media, don't match dst medium with
                        // src medium: we want to be able to do inter-medium replication.
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
                                node->AddToChunkReplicationQueue(chunkWithIndexes, priority);
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
                            node->AddToChunkSealQueue(chunk);
                        }
                    }
                }
            }

            if (Any(statistics.Status & (EChunkStatus::DataMissing | EChunkStatus::ParityMissing)) &&
                None(statistics.Status & EChunkStatus::Lost))
            {
                AddToChunkRepairQueue(chunk, mediumIndex);
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

void TChunkReplicator::RemoveChunkFromQueues(TChunk* chunk, int mediumIndex, bool dropRemovals)
{
    for (auto storedReplica : chunk->StoredReplicas()) {
        auto* node = storedReplica.GetPtr();
        // Replica's medium index is irrelevant: it signifies the medium on which
        // that replica currently is. What we need is purging the chunk from
        // queues where *destination* medium index is #mediumIndex.
        TNodePtrWithIndexes nodeWithIndexes(node, storedReplica.GetReplicaIndex(), mediumIndex);
        RemoveReplicaFromQueues(chunk, nodeWithIndexes, dropRemovals);
    }

    if (chunk->IsErasure()) {
        RemoveFromChunkRepairQueue(chunk);
    }
}

void TChunkReplicator::RemoveReplicaFromQueues(TChunk* chunk, TNodePtrWithIndexes nodeWithIndexes, bool dropRemovals)
{
    auto* node = nodeWithIndexes.GetPtr();
    TChunkPtrWithIndexes chunkWithIndexes(
        chunk,
        nodeWithIndexes.GetReplicaIndex(),
        nodeWithIndexes.GetMediumIndex());
    TChunkIdWithIndexes chunkIdWithIndexes(
        chunk->GetId(),
        nodeWithIndexes.GetReplicaIndex(),
        nodeWithIndexes.GetMediumIndex());
    if (dropRemovals) {
        node->RemoveFromChunkRemovalQueue(chunkIdWithIndexes);
    }
    node->RemoveFromChunkReplicationQueues(chunkWithIndexes);
    node->RemoveFromChunkSealQueue(chunk);
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

void TChunkReplicator::ScheduleChunkRefresh(const TChunkId& chunkId)
{
    auto chunkManager = Bootstrap_->GetChunkManager();
    auto* chunk = chunkManager->FindChunk(chunkId);
    ScheduleChunkRefresh(chunk);
}

void TChunkReplicator::ScheduleChunkRefresh(TChunk* chunk)
{
    if (!IsObjectAlive(chunk)) {
        return;
    }

    if (chunk->GetRefreshScheduled()) {
        return;
    }

    if (chunk->IsForeign()) {
        return;
    }

    TRefreshEntry entry;
    entry.Chunk = chunk;
    entry.When = GetCpuInstant() + ChunkRefreshDelay_;
    RefreshList_.push_back(entry);
    chunk->SetRefreshScheduled(true);

    auto objectManager = Bootstrap_->GetObjectManager();
    objectManager->WeakRefObject(chunk);
}

void TChunkReplicator::ScheduleNodeRefresh(TNode* node)
{
    const auto& replicas = node->StoredReplicas();
    for (const auto& mediumReplicas : replicas) {
        for (const auto& replica : mediumReplicas) {
            ScheduleChunkRefresh(replica.GetPtr());
        }
    }
}

void TChunkReplicator::OnRefresh()
{
    if (RefreshList_.empty()) {
        return;
    }

    auto objectManager = Bootstrap_->GetObjectManager();

    int totalCount = 0;
    int aliveCount = 0;
    PROFILE_TIMING ("/incremental_refresh_time") {
        auto chunkManager = Bootstrap_->GetChunkManager();
        auto now = GetCpuInstant();
        for (int i = 0; i < Config_->MaxChunksPerRefresh; ++i) {
            if (RefreshList_.empty()) {
                break;
            }

            const auto& entry = RefreshList_.front();
            if (entry.When > now) {
                break;
            }

            auto* chunk = entry.Chunk;
            RefreshList_.pop_front();
            ++totalCount;

            if (IsObjectAlive(chunk)) {
                ++aliveCount;
                chunk->SetRefreshScheduled(false);
                RefreshChunk(chunk);
            }

            objectManager->WeakUnrefObject(chunk);
        }
    }

    LOG_DEBUG("Incremental chunk refresh completed (TotalCount: %v, AliveCount: %v)",
        totalCount,
        aliveCount);
}

bool TChunkReplicator::IsEnabled()
{
    return Enabled_;
}

void TChunkReplicator::OnCheckEnabled()
{
    auto worldInitializer = Bootstrap_->GetWorldInitializer();
    if (!worldInitializer->IsInitialized()) {
        Enabled_ = false;
        return;
    }

    try {
        if (Bootstrap_->IsPrimaryMaster()) {
            OnCheckEnabledPrimary();
        } else {
            OnCheckEnabledSecondary();
        }
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Error updating replicator state, disabling until the next attempt");
        Enabled_ = false;
    }
}

void TChunkReplicator::OnCheckEnabledPrimary()
{
    auto cypressManager = Bootstrap_->GetCypressManager();
    auto resolver = cypressManager->CreateResolver();
    auto sysNode = resolver->ResolvePath("//sys");
    if (sysNode->Attributes().Get<bool>("disable_chunk_replicator", false)) {
        if (Enabled_) {
            LOG_INFO("Chunk replicator is disabled by //sys/@disable_chunk_replicator setting");
        }
        Enabled_ = false;
        return;
    }

    auto nodeTracker = Bootstrap_->GetNodeTracker();
    int needOnline = Config_->SafeOnlineNodeCount;
    int gotOnline = nodeTracker->GetOnlineNodeCount();
    if (gotOnline < needOnline) {
        if (Enabled_) {
            LOG_INFO("Chunk replicator disabled: too few online nodes, needed >= %v but got %v",
                needOnline,
                gotOnline);
        }
        Enabled_ = false;
        return;
    }

    auto multicellManager = Bootstrap_->GetMulticellManager();
    auto statistics = multicellManager->ComputeClusterStatistics();
    int gotChunkCount = statistics.chunk_count();
    int gotLostChunkCount = statistics.lost_vital_chunk_count();
    int needLostChunkCount = Config_->SafeLostChunkCount;
    if (gotChunkCount > 0) {
        double needFraction = Config_->SafeLostChunkFraction;
        double gotFraction = (double) gotLostChunkCount / gotChunkCount;
        if (gotFraction > needFraction) {
            if (Enabled_) {
                LOG_INFO("Chunk replicator disabled: too many lost chunks, fraction needed <= %v but got %v",
                    needFraction,
                    gotFraction);
            }
            Enabled_ = false;
            return;
        }
    }

    if (gotLostChunkCount > needLostChunkCount) {
        if (Enabled_) {
            LOG_INFO("Chunk replicator disabled: too many lost chunks, needed <= %v but got %v",
                needLostChunkCount,
                gotLostChunkCount);
        }
        Enabled_ = false;
        return;
    }

    if (!Enabled_) {
        LOG_INFO("Chunk replicator enabled");
    }
    Enabled_ = true;
}

void TChunkReplicator::OnCheckEnabledSecondary()
{
    auto multicellManager = Bootstrap_->GetMulticellManager();
    auto channel = multicellManager->GetMasterChannelOrThrow(Bootstrap_->GetPrimaryCellTag(), EPeerKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto req = TYPathProxy::Get("//sys/@chunk_replicator_enabled");
    auto rsp = WaitFor(proxy.Execute(req))
        .ValueOrThrow();

    auto value = ConvertTo<bool>(TYsonString(rsp->value()));
    if (value != Enabled_) {
        if (value) {
            LOG_INFO("Chunk replicator enabled at primary master");
        } else {
            LOG_INFO("Chunk replicator disabled at primary master");
        }
        Enabled_ = value;
    }
}

int TChunkReplicator::GetRefreshListSize() const
{
    return static_cast<int>(RefreshList_.size());
}

int TChunkReplicator::GetPropertiesUpdateListSize() const
{
    return static_cast<int>(PropertiesUpdateList_.size());
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
        { }

        void Run()
        {
            TraverseChunkTree(CreatePreemptableChunkTraverserCallbacks(Bootstrap_), this, Root_);
        }

    private:
        TBootstrap* const Bootstrap_;
        const TChunkReplicatorPtr Owner_;
        TChunkList* const Root_;

        virtual bool OnChunk(
            TChunk* chunk,
            i64 /*rowIndex*/,
            const TReadLimit& /*startLimit*/,
            const TReadLimit& /*endLimit*/) override
        {
            Owner_->SchedulePropertiesUpdate(chunk);
            return true;
        }

        virtual void OnError(const TError& /*error*/) override
        { }

        virtual void OnFinish() override
        { }

    };

    New<TVisitor>(Bootstrap_, this, chunkList)->Run();
}

void TChunkReplicator::SchedulePropertiesUpdate(TChunk* chunk)
{
    if (!IsObjectAlive(chunk)) {
        return;
    }

    if (chunk->GetPropertiesUpdateScheduled()) {
        return;
    }

    PropertiesUpdateList_.push_back(chunk);
    chunk->SetPropertiesUpdateScheduled(true);

    auto objectManager = Bootstrap_->GetObjectManager();
    objectManager->WeakRefObject(chunk);
}

void TChunkReplicator::OnPropertiesUpdate()
{
    if (PropertiesUpdateList_.empty()) {
        return;
    }

    if (!Bootstrap_->GetHydraFacade()->GetHydraManager()->IsActiveLeader()) {
        return;
    }

    auto chunkManager = Bootstrap_->GetChunkManager();
    auto objectManager = Bootstrap_->GetObjectManager();

    TReqUpdateChunkProperties request;
    request.set_cell_tag(Bootstrap_->GetCellTag());

    // Extract up to MaxChunksPerPropertiesUpdate objects and post a mutation.
    int totalCount = 0;
    int aliveCount = 0;
    PROFILE_TIMING ("/properties_update_time") {
        for (int i = 0; i < Config_->MaxChunksPerPropertiesUpdate; ++i) {
            if (PropertiesUpdateList_.empty()) {
                break;
            }

            auto* chunk = PropertiesUpdateList_.front();
            PropertiesUpdateList_.pop_front();
            ++totalCount;

            if (IsObjectAlive(chunk)) {
                ++aliveCount;
                chunk->SetPropertiesUpdateScheduled(false);
                auto newProperties = ComputeChunkProperties(chunk);
                auto oldProperties = chunk->GetLocalProperties();
                if (newProperties != oldProperties) {
                    auto* update = request.add_updates();
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

            objectManager->WeakUnrefObject(chunk);
        }
    }

    if (request.updates_size() == 0) {
        return;
    }

    LOG_DEBUG("Starting chunk properties update (TotalCount: %v, AliveCount: %v, UpdateCount: %v)",
        totalCount,
        aliveCount,
        request.updates_size());

    auto asyncResult = chunkManager
        ->CreateUpdateChunkPropertiesMutation(request)
        ->CommitAndLog(Logger);
    WaitFor(asyncResult);
}

TChunkProperties TChunkReplicator::ComputeChunkProperties(TChunk* chunk)
{
    bool parentsVisited = false;
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
        for (const auto* owningNode : chunkList->OwningNodes()) {
            if (owningNode->IsTrunk()) {
                parentsVisited = true;

                // Overloaded; MAXes replication factors, ANDs "data-part-only"s,
                // ORs vitalities.
                properties |= owningNode->Properties();
            }
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
            // This never actually throws as 1 is always a valid replication count.
            if (mediumProperties) {
                mediumProperties.SetReplicationFactorOrThrow(1);
            }
        }
    }

    Y_ASSERT(!parentsVisited || properties.Validate());

    return parentsVisited
        ? properties
        : chunk->GetLocalProperties();
}

TChunkList* TChunkReplicator::FollowParentLinks(TChunkList* chunkList)
{
    while (chunkList->OwningNodes().empty()) {
        const auto& parents = chunkList->Parents();
        size_t parentCount = parents.size();
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
    YCHECK(job->GetNode()->Jobs().insert(job).second);

    auto chunkManager = Bootstrap_->GetChunkManager();
    auto chunkId = job->GetChunkIdWithIndexes().Id;
    auto* chunk = chunkManager->FindChunk(chunkId);
    if (chunk) {
        chunk->SetJob(job);
    }
}

void TChunkReplicator::UnregisterJob(const TJobPtr& job, EJobUnregisterFlags flags)
{
    YCHECK(JobMap_.erase(job->GetJobId()) == 1);

    if (Any(flags & EJobUnregisterFlags::UnregisterFromNode)) {
        YCHECK(job->GetNode()->Jobs().erase(job) == 1);
    }

    auto chunkManager = Bootstrap_->GetChunkManager();
    auto chunkId = job->GetChunkIdWithIndexes().Id;
    auto* chunk = chunkManager->FindChunk(chunkId);
    if (chunk) {
        chunk->SetJob(nullptr);
        if (Any(flags & EJobUnregisterFlags::ScheduleChunkRefresh)) {
            ScheduleChunkRefresh(chunk);
        }
    }
}

void TChunkReplicator::AddToChunkRepairQueue(TChunk* chunk, int mediumIndex)
{
    Y_ASSERT(!chunk->GetRepairQueueIterator());
    auto it = ChunkRepairQueue_.insert(ChunkRepairQueue_.end(), {chunk, mediumIndex});
    chunk->SetRepairQueueIterator(it);
}

void TChunkReplicator::RemoveFromChunkRepairQueue(TChunk* chunk)
{
    auto it = chunk->GetRepairQueueIterator();
    if (it) {
        ChunkRepairQueue_.erase(*it);
        chunk->SetRepairQueueIterator(Null);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
