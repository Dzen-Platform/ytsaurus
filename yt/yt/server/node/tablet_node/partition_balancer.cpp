#include "partition_balancer.h"
#include "private.h"
#include "sorted_chunk_store.h"
#include "partition.h"
#include "slot_manager.h"
#include "store.h"
#include "tablet.h"
#include "tablet_manager.h"
#include "tablet_slot.h"
#include "structured_logger.h"
#include "yt/yt/library/profiling/sensor.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>
#include <yt/yt/server/node/cluster_node/config.h>
#include <yt/yt/server/node/cluster_node/dynamic_config_manager.h>

#include <yt/yt/server/lib/hydra/hydra_manager.h>
#include <yt/yt/server/lib/hydra/mutation.h>

#include <yt/yt/server/lib/tablet_node/proto/tablet_manager.pb.h>

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/yt/ytlib/chunk_client/fetcher.h>
#include <yt/yt/ytlib/chunk_client/input_chunk.h>
#include <yt/yt/ytlib/chunk_client/throttler_manager.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/ytlib/table_client/samples_fetcher.h>
#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/table_client/row_buffer.h>

#include <yt/yt/ytlib/tablet_client/config.h>
#include <yt/yt/client/table_client/wire_protocol.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/concurrency/async_semaphore.h>
#include <yt/yt/core/concurrency/scheduler.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/profiling/profiler.h>

namespace NYT::NTabletNode {

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NHydra;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletNode::NProto;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletNodeLogger;

////////////////////////////////////////////////////////////////////////////////

class TPartitionBalancer
    : public IPartitionBalancer
{
public:
    explicit TPartitionBalancer(NClusterNode::TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
        , Config_(Bootstrap_->GetConfig()->TabletNode->PartitionBalancer)
        , Semaphore_(New<TAsyncSemaphore>(Config_->MaxConcurrentSamplings))
        , ThrottlerManager_(New<TThrottlerManager>(
            Config_->ChunkLocationThrottler,
            Logger))
    { }

    virtual void Start() override
    {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->SubscribeScanSlot(BIND(&TPartitionBalancer::OnScanSlot, MakeStrong(this)));
    }

private:
    NClusterNode::TBootstrap* const Bootstrap_;
    const TPartitionBalancerConfigPtr Config_;

    const TAsyncSemaphorePtr Semaphore_;
    const TThrottlerManagerPtr ThrottlerManager_;

    const TProfiler Profiler_ = TabletNodeProfiler.WithPrefix("/partition_balancer");
    TCounter ScheduledSplitsCounter_ = Profiler_.Counter("/scheduled_splits");
    TCounter ScheduledMergesCounter_ = Profiler_.Counter("/scheduled_merges");
    TEventTimer ScanTime_ = Profiler_.Timer("/scan_time");

    void OnScanSlot(TTabletSlotPtr slot)
    {
        TEventTimerGuard guard(ScanTime_);

        const auto& dynamicConfigManager = Bootstrap_->GetDynamicConfigManager();
        auto dynamicConfig = dynamicConfigManager->GetConfig()->TabletNode->PartitionBalancer;
        if (!dynamicConfig->Enable) {
            return;
        }

        if (slot->GetAutomatonState() != EPeerState::Leading) {
            return;
        }

        const auto& tabletManager = slot->GetTabletManager();
        for (auto [tabletId, tablet] : tabletManager->Tablets()) {
            ScanTablet(slot, tablet);
        }
    }

    void ScanTablet(TTabletSlotPtr slot, TTablet* tablet)
    {
        if (tablet->GetState() != ETabletState::Mounted) {
            return;
        }

        if (!tablet->IsPhysicallySorted()) {
            return;
        }

        for (const auto& partition : tablet->PartitionList()) {
            ScanPartitionToSample(slot, partition.get());
        }

        if (!tablet->GetConfig()->EnableCompactionAndPartitioning) {
            return;
        }

        int currentMaxOverlappingStoreCount = tablet->GetOverlappingStoreCount();
        int estimatedMaxOverlappingStoreCount = currentMaxOverlappingStoreCount;

        YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
            "Partition balancer started tablet scan for splits (%v, CurrentMosc: %v)",
            tablet->GetLoggingTag(),
            currentMaxOverlappingStoreCount);

        int largestPartitionStoreCount = 0;
        int secondLargestPartitionStoreCount = 0;
        for (const auto& partition : tablet->PartitionList()) {
            int storeCount = partition->Stores().size();
            if (storeCount > largestPartitionStoreCount) {
                secondLargestPartitionStoreCount = largestPartitionStoreCount;
                largestPartitionStoreCount = storeCount;
            } else if (storeCount > secondLargestPartitionStoreCount) {
                secondLargestPartitionStoreCount = storeCount;
            }
        }

        for (const auto& partition : tablet->PartitionList()) {
            ScanPartitionToSplit(
                slot,
                partition.get(),
                &estimatedMaxOverlappingStoreCount,
                secondLargestPartitionStoreCount);
        }

        int maxAllowedOverlappingStoreCount = tablet->GetConfig()->MaxOverlappingStoreCount -
            (estimatedMaxOverlappingStoreCount - currentMaxOverlappingStoreCount);

        YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
            "Partition balancer started tablet scan for merges (%v, "
            "EstimatedMosc: %v, MaxAllowedOsc: %v)",
            tablet->GetLoggingTag(),
            estimatedMaxOverlappingStoreCount,
            maxAllowedOverlappingStoreCount);

        for (const auto& partition : tablet->PartitionList()) {
            ScanPartitionToMerge(slot, partition.get(), maxAllowedOverlappingStoreCount);
        }
    }

    void ScanPartitionToSplit(
        TTabletSlotPtr slot,
        TPartition* partition,
        int* estimatedMaxOverlappingStoreCount,
        int secondLargestPartitionStoreCount)
    {
        auto* tablet = partition->GetTablet();
        const auto& config = tablet->GetConfig();
        int partitionCount = tablet->PartitionList().size();
        i64 actualDataSize = partition->GetCompressedDataSize();
        int estimatedStoresDelta = partition->Stores().size();

        auto Logger = BuildLogger(slot, partition);

        if (tablet->GetConfig()->EnableLsmVerboseLogging) {
            YT_LOG_DEBUG(
                "Scanning partition to split (PartitionIndex: %v of %v, "
                "EstimatedMosc: %v, DataSize: %v, StoreCount: %v, SecondLargestPartitionStoreCount: %v)",
                partition->GetIndex(),
                partitionCount,
                *estimatedMaxOverlappingStoreCount,
                actualDataSize,
                partition->Stores().size(),
                secondLargestPartitionStoreCount);
        }

        if (partition->GetState() != EPartitionState::Normal) {
            YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
                "Will not split partition due to improper partition state (PartitionState: %v)",
                partition->GetState());
            return;
        }


        if (partition->IsImmediateSplitRequested()) {
            if (ValidateSplit(slot, partition, true)) {
                partition->CheckedSetState(EPartitionState::Normal, EPartitionState::Splitting);
                ScheduledSplitsCounter_.Increment();
                DoRunImmediateSplit(slot, partition, Logger);
                // This is inexact to say the least: immediate split is called when we expect that
                // most of the stores will stay intact after splitting by the provided pivots.
                *estimatedMaxOverlappingStoreCount += estimatedStoresDelta;
            }
            return;
        }

        int maxOverlappingStoreCountAfterSplit = estimatedStoresDelta + *estimatedMaxOverlappingStoreCount;
        // If the partition is the largest one, the estimate is incorrect since its stores will move to eden
        // and the partition will no longer contribute to the first summand in (max_partition_size + eden_size).
        // Instead, the second largest partition will.
        if (partition->Stores().size() > secondLargestPartitionStoreCount) {
            maxOverlappingStoreCountAfterSplit -= partition->Stores().size() - secondLargestPartitionStoreCount;
        }

        if (maxOverlappingStoreCountAfterSplit <= config->MaxOverlappingStoreCount &&
            actualDataSize > config->MaxPartitionDataSize)
        {
            int splitFactor = std::min({
                actualDataSize / config->DesiredPartitionDataSize + 1,
                actualDataSize / config->MinPartitionDataSize,
                static_cast<i64>(config->MaxPartitionCount - partitionCount)});

            if (splitFactor > 1 && ValidateSplit(slot, partition, false)) {
                partition->CheckedSetState(EPartitionState::Normal, EPartitionState::Splitting);
                ScheduledSplitsCounter_.Increment();
                YT_LOG_DEBUG("Partition is scheduled for split");
                tablet->GetStructuredLogger()->LogEvent("schedule_partition_split")
                    .Item("partition_id").Value(partition->GetId())
                    // NB: deducible.
                    .Item("split_factor").Value(splitFactor)
                    .Item("data_size").Value(actualDataSize);
                tablet->GetEpochAutomatonInvoker()->Invoke(BIND(
                    &TPartitionBalancer::DoRunSplit,
                    MakeStrong(this),
                    slot,
                    partition,
                    splitFactor,
                    partition->GetTablet(),
                    partition->GetId(),
                    tablet->GetId(),
                    Logger));
                *estimatedMaxOverlappingStoreCount = maxOverlappingStoreCountAfterSplit;
            }
        }
    }

    void ScanPartitionToMerge(TTabletSlotPtr slot, TPartition* partition, int maxAllowedOverlappingStoreCount)
    {
        auto* tablet = partition->GetTablet();
        const auto& config = tablet->GetConfig();
        int partitionCount = tablet->PartitionList().size();
        i64 actualDataSize = partition->GetCompressedDataSize();

        // Maximum data size the partition might have if all chunk stores from Eden go here.
        i64 maxPotentialDataSize = actualDataSize;
        for (const auto& store : tablet->GetEden()->Stores()) {
            if (store->GetType() == EStoreType::SortedChunk) {
                maxPotentialDataSize += store->GetCompressedDataSize();
            }
        }

        auto Logger = BuildLogger(slot, partition);

        YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
            "Scanning partition to merge (PartitionIndex: %v of %v, "
            "DataSize: %v, MaxPotentialDataSize: %v)",
            partition->GetIndex(),
            partitionCount,
            actualDataSize,
            maxPotentialDataSize);

        if (maxPotentialDataSize < config->MinPartitionDataSize && partitionCount > 1) {
            int firstPartitionIndex = partition->GetIndex();
            int lastPartitionIndex = firstPartitionIndex + 1;
            if (lastPartitionIndex == partitionCount) {
                --firstPartitionIndex;
                --lastPartitionIndex;
            }
            int estimatedOverlappingStoreCount = tablet->GetEdenOverlappingStoreCount() +
                tablet->PartitionList()[firstPartitionIndex]->Stores().size() +
                tablet->PartitionList()[lastPartitionIndex]->Stores().size();

            YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
                "Found candidate partitions to merge (FirstPartitionIndex: %v, "
                "LastPartitionIndex: %v, EstimatedOsc: %v, WillRunMerge: %v",
                firstPartitionIndex,
                lastPartitionIndex,
                estimatedOverlappingStoreCount,
                estimatedOverlappingStoreCount < maxAllowedOverlappingStoreCount);

            if (estimatedOverlappingStoreCount <= maxAllowedOverlappingStoreCount) {
                RunMerge(slot, partition, firstPartitionIndex, lastPartitionIndex);
            }
        }
    }

    void ScanPartitionToSample(TTabletSlotPtr slot, TPartition* partition)
    {
        if (partition->GetSamplingRequestTime() > partition->GetSamplingTime() &&
            partition->GetSamplingTime() < TInstant::Now() - Config_->ResamplingPeriod) {
            RunSample(slot, partition);
        }
    }

    bool ValidateSplit(TTabletSlotPtr slot, TPartition* partition, bool immediateSplit) const
    {
        const auto* tablet = partition->GetTablet();

        if (!immediateSplit && TInstant::Now() < partition->GetAllowedSplitTime()) {
            return false;
        }

        auto Logger = BuildLogger(slot, partition);

        if (!tablet->GetConfig()->EnablePartitionSplitWhileEdenPartitioning &&
            tablet->GetEden()->GetState() == EPartitionState::Partitioning)
        {
            YT_LOG_DEBUG("Eden is partitioning, will not split partition (EdenPartitionId: %v)",
                tablet->GetEden()->GetId());
            return false;
        }

        for (const auto& store : partition->Stores()) {
            if (store->GetStoreState() != EStoreState::Persistent) {
                YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
                    "Will not split partition due to improper store state "
                    "(StoreId: %v, StoreState: %v)",
                    store->GetId(),
                    store->GetStoreState());
                return false;
            }
        }

        if (immediateSplit) {
            const auto& pivotKeys = partition->PivotKeysForImmediateSplit();
            YT_VERIFY(!pivotKeys.empty());
            if (pivotKeys[0] != partition->GetPivotKey()) {
                YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
                    "Will not perform immediate partition split: first proposed pivot key "
                    "does not match partition pivot key (PartitionPivotKey: %v, ProposedPivotKey: %v)",
                    partition->GetPivotKey(),
                    pivotKeys[0]);

                partition->PivotKeysForImmediateSplit().clear();
                return false;
            }

            for (int index = 1; index < pivotKeys.size(); ++index) {
                if (pivotKeys[index] <= pivotKeys[index - 1]) {
                    YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
                        "Will not perform immediate partition split: proposed pivots are not sorted");

                    partition->PivotKeysForImmediateSplit().clear();
                    return false;
                }
            }

            if (pivotKeys.back() >= partition->GetNextPivotKey()) {
                YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
                    "Will not perform immediate partition split: last proposed pivot key "
                    "is not less than partition next pivot key (NextPivotKey: %v, ProposedPivotKey: %v)",
                    partition->GetNextPivotKey(),
                    pivotKeys.back());

                partition->PivotKeysForImmediateSplit().clear();
                return false;
            }

            if (pivotKeys.size() <= 1) {
                YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
                    "Will not perform immediate partition split: too few pivot keys");

                partition->PivotKeysForImmediateSplit().clear();
                return false;
            }
        }

        return true;
    }

    void DoRunSplit(
        TTabletSlotPtr slot,
        TPartition* partition,
        int splitFactor,
        TTablet* tablet,
        TPartitionId partitionId,
        TTabletId tabletId,
        NLogging::TLogger Logger)
    {
        YT_LOG_DEBUG("Splitting partition");

        YT_VERIFY(tablet == partition->GetTablet());
        const auto& hydraManager = slot->GetHydraManager();
        const auto& structuredLogger = tablet->GetStructuredLogger();

        YT_LOG_INFO("Partition is eligible for split (SplitFactor: %v)",
            splitFactor);

        try {
            auto rowBuffer = New<TRowBuffer>();
            auto samples = GetPartitionSamples(rowBuffer, slot, partition, Config_->MaxPartitioningSampleCount);
            int sampleCount = static_cast<int>(samples.size());
            int minSampleCount = std::max(Config_->MinPartitioningSampleCount, splitFactor);
            if (sampleCount < minSampleCount) {
                structuredLogger->LogEvent("abort_partition_split")
                    .Item("partition_id").Value(partition->GetId())
                    .Item("reason").Value("too_few_samples")
                    .Item("min_sample_count").Value(minSampleCount)
                    .Item("sample_count").Value(sampleCount);
                THROW_ERROR_EXCEPTION("Too few samples fetched: need %v, got %v",
                    minSampleCount,
                    sampleCount);
            }

            std::vector<TLegacyKey> pivotKeys;
            // Take the pivot of the partition.
            pivotKeys.push_back(partition->GetPivotKey());
            // And add |splitFactor - 1| more keys from samples.
            for (int i = 0; i < splitFactor - 1; ++i) {
                int j = (i + 1) * sampleCount / splitFactor - 1;
                auto key = samples[j];
                if (key > pivotKeys.back()) {
                    pivotKeys.push_back(key);
                }
            }

            if (pivotKeys.size() < 2) {
                structuredLogger->LogEvent("abort_partition_split")
                    .Item("partition_id").Value(partition->GetId())
                    .Item("reason").Value("no_valid_pivots");
                THROW_ERROR_EXCEPTION("No valid pivot keys can be obtained from samples");
            }

            structuredLogger->LogEvent("request_partition_split")
                .Item("partition_id").Value(partition->GetId())
                .Item("immediate").Value(false)
                .Item("pivot_keys").List(pivotKeys);

            TReqSplitPartition request;
            ToProto(request.mutable_tablet_id(), tablet->GetId());
            request.set_mount_revision(tablet->GetMountRevision());
            ToProto(request.mutable_partition_id(), partition->GetId());
            ToProto(request.mutable_pivot_keys(), pivotKeys);

            CreateMutation(hydraManager, request)
                ->CommitAndLog(Logger);
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Partition splitting aborted");
            structuredLogger->LogEvent("backoff_partition_split")
                .Item("partition_id").Value(partition->GetId());
            partition->CheckedSetState(EPartitionState::Splitting, EPartitionState::Normal);
            partition->SetAllowedSplitTime(TInstant::Now() + Config_->SplitRetryDelay);
        }
    }

    void DoRunImmediateSplit(
        TTabletSlotPtr slot,
        TPartition* partition,
        NLogging::TLogger Logger)
    {
        YT_LOG_DEBUG("Splitting partition with provided pivot keys (SplitFactor: %v)",
            partition->PivotKeysForImmediateSplit().size());

        auto* tablet = partition->GetTablet();

        std::vector<TLegacyOwningKey> pivotKeys;
        pivotKeys.swap(partition->PivotKeysForImmediateSplit());

        tablet->GetStructuredLogger()->LogEvent("request_partition_split")
            .Item("partition_id").Value(partition->GetId())
            .Item("immediate").Value(true)
            .Item("pivot_keys").List(pivotKeys);

        const auto& hydraManager = slot->GetHydraManager();
        TReqSplitPartition request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        request.set_mount_revision(tablet->GetMountRevision());
        ToProto(request.mutable_partition_id(), partition->GetId());
        ToProto(request.mutable_pivot_keys(), pivotKeys);

        CreateMutation(hydraManager, request)
            ->CommitAndLog(Logger);
    }

    bool RunMerge(
        TTabletSlotPtr slot,
        TPartition* partition,
        int firstPartitionIndex,
        int lastPartitionIndex)
    {
        auto* tablet = partition->GetTablet();

        for (int index = firstPartitionIndex; index <= lastPartitionIndex; ++index) {
            if (tablet->PartitionList()[index]->GetState() != EPartitionState::Normal) {
                YT_LOG_DEBUG_IF(tablet->GetConfig()->EnableLsmVerboseLogging,
                    "Will not merge partitions due to improper partition state "
                    "(%v, InitialPartitionId: %v, PartitionId: %v, PartitionIndex: %v, PartitionState: %v)",
                    tablet->GetLoggingTag(),
                    partition->GetId(),
                    tablet->PartitionList()[index]->GetId(),
                    index,
                    tablet->PartitionList()[index]->GetState());
                return false;
            }
        }

        for (int index = firstPartitionIndex; index <= lastPartitionIndex; ++index) {
            tablet->PartitionList()[index]->CheckedSetState(EPartitionState::Normal, EPartitionState::Merging);
        }
        ScheduledMergesCounter_.Increment();

        auto Logger = TabletNodeLogger;
        Logger.AddTag("%v, CellId: %v, PartitionIds: %v",
            partition->GetTablet()->GetLoggingTag(),
            slot->GetCellId(),
            MakeFormattableView(
                MakeRange(
                    tablet->PartitionList().data() + firstPartitionIndex,
                    tablet->PartitionList().data() + lastPartitionIndex + 1),
                TPartitionIdFormatter()));

        YT_LOG_INFO("Partitions are eligible for merge");

        tablet->GetStructuredLogger()->LogEvent("request_partitions_merge")
            .Item("initial_partition_id").Value(partition->GetId())
            .Item("first_partition_index").Value(firstPartitionIndex)
            .Item("last_partition_index").Value(lastPartitionIndex);

        const auto& hydraManager = slot->GetHydraManager();

        TReqMergePartitions request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        request.set_mount_revision(tablet->GetMountRevision());
        ToProto(request.mutable_partition_id(), tablet->PartitionList()[firstPartitionIndex]->GetId());
        request.set_partition_count(lastPartitionIndex - firstPartitionIndex + 1);

        CreateMutation(hydraManager, request)
            ->CommitAndLog(Logger);
        return true;
    }


    bool RunSample(TTabletSlotPtr slot, TPartition* partition)
    {
        if (partition->GetState() != EPartitionState::Normal) {
            return false;
        }

        auto guard = TAsyncSemaphoreGuard::TryAcquire(Semaphore_);
        if (!guard) {
            return false;
        }

        partition->CheckedSetState(EPartitionState::Normal, EPartitionState::Sampling);

        auto Logger = BuildLogger(slot, partition);

        YT_LOG_DEBUG("Partition is scheduled for sampling");

        BIND(&TPartitionBalancer::DoRunSample, MakeStrong(this), Passed(std::move(guard)))
            .AsyncVia(partition->GetTablet()->GetEpochAutomatonInvoker())
            .Run(
                slot,
                partition,
                partition->GetTablet(),
                partition->GetId(),
                partition->GetTablet()->GetId(),
                Logger);
        return true;
    }

    void DoRunSample(
        TAsyncSemaphoreGuard /*guard*/,
        TTabletSlotPtr slot,
        TPartition* partition,
        TTablet* tablet,
        TPartitionId partitionId,
        TTabletId tabletId,
        NLogging::TLogger Logger)
    {
        YT_LOG_DEBUG("Sampling partition");

        YT_VERIFY(tablet == partition->GetTablet());
        auto config = tablet->GetConfig();

        const auto& hydraManager = slot->GetHydraManager();

        try {
            auto compressedDataSize = partition->GetCompressedDataSize();
            if (compressedDataSize == 0) {
                THROW_ERROR_EXCEPTION("Empty partition");
            }

            auto uncompressedDataSize = partition->GetUncompressedDataSize();
            auto scaledSamples = static_cast<int>(
                config->SamplesPerPartition * std::max(compressedDataSize, uncompressedDataSize) / compressedDataSize);
            YT_LOG_INFO("Sampling partition (DesiredSampleCount: %v)", scaledSamples);

            auto rowBuffer = New<TRowBuffer>();
            auto samples = GetPartitionSamples(rowBuffer, slot, partition, scaledSamples);
            samples.erase(
                std::unique(samples.begin(), samples.end()),
                samples.end());

            TWireProtocolWriter writer;
            writer.WriteUnversionedRowset(samples);

            TReqUpdatePartitionSampleKeys request;
            ToProto(request.mutable_tablet_id(), tablet->GetId());
            request.set_mount_revision(tablet->GetMountRevision());
            ToProto(request.mutable_partition_id(), partition->GetId());
            request.set_sample_keys(MergeRefsToString(writer.Finish()));

            CreateMutation(hydraManager, request)
                ->CommitAndLog(Logger);
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Partition sampling aborted");
        }

        partition->CheckedSetState(EPartitionState::Sampling, EPartitionState::Normal);
        // NB: Update the timestamp even in case of failure to prevent
        // repeating unsuccessful samplings too rapidly.
        partition->SetSamplingTime(TInstant::Now());
    }


    std::vector<TLegacyKey> GetPartitionSamples(
        const TRowBufferPtr& rowBuffer,
        TTabletSlotPtr slot,
        TPartition* partition,
        int maxSampleCount)
    {
        YT_VERIFY(!partition->IsEden());

        if (maxSampleCount == 0) {
            return std::vector<TLegacyKey>();
        }

        auto Logger = BuildLogger(slot, partition);

        auto* tablet = partition->GetTablet();

        auto nodeDirectory = New<TNodeDirectory>();

        auto chunkScraper = CreateFetcherChunkScraper(
            Config_->ChunkScraper,
            Bootstrap_->GetControlInvoker(),
            ThrottlerManager_,
            Bootstrap_->GetMasterClient(),
            nodeDirectory,
            Logger);

        auto samplesFetcher = New<TSamplesFetcher>(
            Config_->SamplesFetcher,
            ESamplingPolicy::Partitioning,
            maxSampleCount,
            tablet->GetPhysicalSchema()->GetKeyColumns(),
            NTableClient::MaxSampleSize,
            nodeDirectory,
            GetCurrentInvoker(),
            rowBuffer,
            chunkScraper,
            Bootstrap_->GetMasterClient(),
            Logger);

        {
            auto channel = Bootstrap_->GetMasterClient()->GetMasterChannelOrThrow(
                NApi::EMasterChannelKind::Follower,
                CellTagFromId(tablet->GetId()));
            TChunkServiceProxy proxy(channel);

            auto req = proxy.LocateChunks();
            req->SetHeavy(true);

            THashMap<TChunkId, TSortedChunkStorePtr> storeMap;

            auto addStore = [&] (const ISortedStorePtr& store) {
                if (store->GetType() != EStoreType::SortedChunk)
                    return;

                if (store->GetUpperBoundKey() <= partition->GetPivotKey() ||
                    store->GetMinKey() >= partition->GetNextPivotKey())
                    return;

                auto chunkId = store->AsSortedChunk()->GetChunkId();
                YT_VERIFY(chunkId);
                if (storeMap.emplace(chunkId, store->AsSortedChunk()).second) {
                    ToProto(req->add_subrequests(), chunkId);
                }
            };

            auto addStores = [&] (const THashSet<ISortedStorePtr>& stores) {
                for (const auto& store : stores) {
                    addStore(store);
                }
            };

            addStores(partition->Stores());
            addStores(tablet->GetEden()->Stores());

            if (req->subrequests_size() == 0) {
                return std::vector<TLegacyKey>();
            }

            YT_LOG_INFO("Locating partition chunks (ChunkCount: %v)",
                req->subrequests_size());

            auto rspOrError = WaitFor(req->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error locating partition chunks");
            const auto& rsp = rspOrError.Value();
            YT_VERIFY(req->subrequests_size() == rsp->subresponses_size());

            YT_LOG_INFO("Partition chunks located");

            nodeDirectory->MergeFrom(rsp->node_directory());

            for (int index = 0; index < rsp->subresponses_size(); ++index) {
                const auto& subrequest = req->subrequests(index);
                const auto& subresponse = rsp->subresponses(index);

                auto chunkId = FromProto<TChunkId>(subrequest);
                const auto& store = GetOrCrash(storeMap, chunkId);

                NChunkClient::NProto::TChunkSpec chunkSpec;
                ToProto(chunkSpec.mutable_chunk_id(), chunkId);
                *chunkSpec.mutable_replicas() = subresponse.replicas();
                *chunkSpec.mutable_chunk_meta() = store->GetChunkMeta();
                ToProto(chunkSpec.mutable_lower_limit(), TLegacyReadLimit(partition->GetPivotKey()));
                ToProto(chunkSpec.mutable_upper_limit(), TLegacyReadLimit(partition->GetNextPivotKey()));
                chunkSpec.set_erasure_codec(subresponse.erasure_codec());

                auto inputChunk = New<TInputChunk>(chunkSpec);
                samplesFetcher->AddChunk(std::move(inputChunk));
            }
        }

        WaitFor(samplesFetcher->Fetch())
            .ThrowOnError();

        YT_LOG_DEBUG("Samples fetched");

        std::vector<TLegacyKey> samples;
        for (const auto& sample : samplesFetcher->GetSamples()) {
            YT_VERIFY(!sample.Incomplete);
            samples.push_back(sample.Key);
        }

        // NB(psushin): This filtering is typically redundant (except for the first pivot),
        // since fetcher already returns samples within given limits.
        samples.erase(
            std::remove_if(
                samples.begin(),
                samples.end(),
                [&] (TLegacyKey key) {
                    return key <= partition->GetPivotKey() || key >= partition->GetNextPivotKey();
                }),
            samples.end());

        std::sort(samples.begin(), samples.end());
        return samples;
    }


    static NLogging::TLogger BuildLogger(
        const TTabletSlotPtr& slot,
        TPartition* partition)
    {
        return TabletNodeLogger.WithTag("%v, CellId: %v, PartitionId: %v",
            partition->GetTablet()->GetLoggingTag(),
            slot->GetCellId(),
            partition->GetId());
    }
};

IPartitionBalancerPtr CreatePartitionBalancer(NClusterNode::TBootstrap* bootstrap)
{
    return New<TPartitionBalancer>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
