#include "chunk_writer_pool.h"
#include "config.h"
#include "in_memory_manager.h"
#include "partition.h"
#include "private.h"
#include "slot_manager.h"
#include "sorted_chunk_store.h"
#include "store_compactor.h"
#include "store_manager.h"
#include "tablet.h"
#include "tablet_manager.h"
#include "tablet_profiling.h"
#include "tablet_reader.h"
#include "tablet_slot.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/server/tablet_server/tablet_manager.pb.h>

#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/native_transaction.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/config.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/table_client/versioned_chunk_writer.h>
#include <yt/ytlib/table_client/versioned_reader.h>
#include <yt/ytlib/table_client/versioned_row.h>

#include <yt/ytlib/tablet_client/config.h>

#include <yt/ytlib/transaction_client/timestamp_provider.h>
#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/action.h>

#include <yt/core/concurrency/thread_pool.h>
#include <yt/core/concurrency/async_semaphore.h>
#include <yt/core/concurrency/scheduler.h>

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>
#include <yt/core/profiling/profile_manager.h>

#include <yt/core/ytree/helpers.h>

#include <yt/core/misc/finally.h>
#include <yt/core/misc/heap.h>

namespace NYT {
namespace NTabletNode {

using namespace NApi;
using namespace NChunkClient;
using namespace NConcurrency;
using namespace NHydra;
using namespace NObjectClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletNode::NProto;
using namespace NTransactionClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static const size_t MaxRowsPerRead = 65536;
static const size_t MaxRowsPerWrite = 65536;

////////////////////////////////////////////////////////////////////////////////

/*!
 * Ultimately, the goal of the compactor is to control the overlapping store count
 * by performing compactions and partitionings. A compaction operates within a partition,
 * replacing a set of stores with a newly baked one. A partitioning operates on the Eden,
 * replacing a set of Eden stores with a set of partition-bound stores.
 */
class TStoreCompactor
    : public TRefCounted
{
public:
    TStoreCompactor(
       TTabletNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , ThreadPool_(New<TThreadPool>(Config_->StoreCompactor->ThreadPoolSize, "StoreCompact"))
        , Profiler("/tablet_node/store_compactor")
        , PartitioningSemaphore_(New<TProfiledAsyncSemaphore>(Config_->StoreCompactor->MaxConcurrentPartitionings, Profiler, "/running_partitionings"))
        , CompactionSemaphore_(New<TProfiledAsyncSemaphore>(Config_->StoreCompactor->MaxConcurrentCompactions, Profiler, "/running_compactions"))
        , FeasiblePartitioningsCounter_("/feasible_partitionings")
        , FeasibleCompactionsCounter_("/feasible_compactions")
        , ScheduledPartitioningsCounter_("/scheduled_partitionings")
        , ScheduledCompactionsCounter_("/scheduled_compactions")
    { }

    void Start()
    {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->SubscribeBeginSlotScan(BIND(&TStoreCompactor::OnBeginSlotScan, MakeStrong(this)));
        slotManager->SubscribeScanSlot(BIND(&TStoreCompactor::OnScanSlot, MakeStrong(this)));
        slotManager->SubscribeEndSlotScan(BIND(&TStoreCompactor::OnEndSlotScan, MakeStrong(this)));
    }

private:
    const TTabletNodeConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;

    TThreadPoolPtr ThreadPool_;

    const NProfiling::TProfiler Profiler;
    TAsyncSemaphorePtr PartitioningSemaphore_;
    TAsyncSemaphorePtr CompactionSemaphore_;
    NProfiling::TSimpleCounter FeasiblePartitioningsCounter_;
    NProfiling::TSimpleCounter FeasibleCompactionsCounter_;
    NProfiling::TSimpleCounter ScheduledPartitioningsCounter_;
    NProfiling::TSimpleCounter ScheduledCompactionsCounter_;
    const NProfiling::TTagId CompactionTag_ = NProfiling::TProfileManager::Get()->RegisterTag("method", "compaction");
    const NProfiling::TTagId PartitioningTag_ = NProfiling::TProfileManager::Get()->RegisterTag("method", "partitioning");

    struct TTask
    {
        TTabletSlotPtr Slot;
        IInvokerPtr Invoker;

        TTabletId Tablet;
        TPartitionId Partition;
        std::vector<TStoreId> Stores;

        // Overlapping stores slack for the task.
        // That is, the remaining number of stores in the partition till
        // the tablet hits MOSC limit.
        // Small values indicate that the tablet is in a critical state.
        ui64 Slack = 0;
        // Guaranteed effect on the slack if this task will be done.
        // This is a conservative estimate.
        ui64 Effect = 0;
        ui64 Random = RandomNumber<size_t>();

        TTask() = default;
        TTask(const TTask&) = delete;
        TTask& operator=(const TTask&) = delete;
        TTask(TTask&&) = delete;
        TTask& operator=(TTask&&) = delete;

        auto MakeComparableValue()
        {
            return std::make_tuple(Slack, -Effect, -Stores.size(), Random);
        }
    };

    // Variables below contain per-iteration state for slot scan.
    TSpinLock ScanSpinLock_;
    bool ScanForPartitioning_;
    bool ScanForCompactions_;
    std::vector<std::unique_ptr<TTask>> PartitioningCandidates_;
    std::vector<std::unique_ptr<TTask>> CompactionCandidates_;

    // Variables below are actually scheduled tasks.
    TSpinLock TaskSpinLock_;
    std::vector<std::unique_ptr<TTask>> PartitioningTasks_;
    size_t PartitioningTaskIndex_ = 0; // First unscheduled task.
    std::vector<std::unique_ptr<TTask>> CompactionTasks_;
    size_t CompactionTaskIndex_ = 0; // First unscheduled task.

    void OnBeginSlotScan()
    {
        // NB: Strictly speaking, redundant.
        auto guard = Guard(ScanSpinLock_);

        // Save some scheduling resources by skipping unnecessary work.
        ScanForPartitioning_ = PartitioningSemaphore_->IsReady();
        ScanForCompactions_ = CompactionSemaphore_->IsReady();
        PartitioningCandidates_.clear(); // Though must be clear already.
        CompactionCandidates_.clear(); // Though must be clear already.
    }

    void OnScanSlot(const TTabletSlotPtr& slot)
    {
        const auto& tagIdList = slot->GetTagIdList();
        PROFILE_TIMING("/scan_time", tagIdList) {
            OnScanSlotImpl(slot, tagIdList);
        }
    }

    void OnScanSlotImpl(const TTabletSlotPtr& slot, const NProfiling::TTagIdList& tagIdList)
    {
        if (slot->GetAutomatonState() != EPeerState::Leading) {
            return;
        }

        const auto& tabletManager = slot->GetTabletManager();
        for (const auto& pair : tabletManager->Tablets()) {
            ScanTablet(slot.Get(), pair.second);
        }
    }

    void OnEndSlotScan()
    {
        // NB: Strictly speaking, redundant.
        auto guard = Guard(ScanSpinLock_);

        if (ScanForPartitioning_) {
            PickMorePartitionings(guard);
            ScheduleMorePartitionings();
        }

        if (ScanForCompactions_) {
            PickMoreCompactions(guard);
            ScheduleMoreCompactions();
        }
    }

    void ScanTablet(TTabletSlot* slot, TTablet* tablet)
    {
        if (tablet->GetState() != ETabletState::Mounted) {
            return;
        }

        if (!tablet->IsPhysicallySorted()) {
            return;
        }

        const auto& config = tablet->GetConfig();
        if (!config->EnableCompactionAndPartitioning) {
            return;
        }

        if (config->InMemoryMode != EInMemoryMode::None && Bootstrap_->GetTabletSlotManager()->IsOutOfMemory()) {
            return;
        }

        ScanEdenForPartitioning(slot, tablet->GetEden());
        ScanPartitionForCompaction(slot, tablet->GetEden());

        for (auto& partition : tablet->PartitionList()) {
            ScanPartitionForCompaction(slot, partition.get());
        }
    }

    bool ScanEdenForPartitioning(TTabletSlot* slot, TPartition* eden)
    {
        if (!ScanForPartitioning_ || eden->GetState() != EPartitionState::Normal) {
            return false;
        }

        const auto* tablet = eden->GetTablet();

        auto stores = PickStoresForPartitioning(eden);
        if (stores.empty()) {
            return false;
        }

        auto candidate = std::make_unique<TTask>();
        candidate->Slot = slot;
        candidate->Invoker = tablet->GetEpochAutomatonInvoker();
        candidate->Tablet = tablet->GetId();
        candidate->Partition = eden->GetId();
        candidate->Stores = std::move(stores);
        // We aim to improve OSC; partitioning unconditionally improves OSC (given at least two stores).
        // So we consider how constrained is the tablet, and how many stores we consider for partitioning.
        const int overlappingStoreLimit = tablet->GetConfig()->MaxOverlappingStoreCount;
        const int overlappingStoreCount = tablet->GetOverlappingStoreCount();
        candidate->Slack = static_cast<size_t>(std::max(0, overlappingStoreLimit - overlappingStoreCount));
        candidate->Effect = candidate->Stores.size() - 1;

        {
            auto guard = Guard(ScanSpinLock_);
            PartitioningCandidates_.push_back(std::move(candidate));
        }

        return true;
    }

    bool ScanPartitionForCompaction(TTabletSlot* slot, TPartition* partition)
    {
        if (!ScanForCompactions_ || partition->GetState() != EPartitionState::Normal) {
            return false;
        }

        const auto* tablet = partition->GetTablet();

        auto stores = PickStoresForCompaction(partition);
        if (stores.empty()) {
            return false;
        }

        auto candidate = std::make_unique<TTask>();
        candidate->Slot = slot;
        candidate->Invoker = tablet->GetEpochAutomatonInvoker();
        candidate->Tablet = tablet->GetId();
        candidate->Partition = partition->GetId();
        candidate->Stores = std::move(stores);
        // We aim to improve OSC; compaction improves OSC _only_ if the partition contributes towards OSC.
        // So we consider how constrained is the partition, and how many stores we consider for compaction.
        const int overlappingStoreLimit = tablet->GetConfig()->MaxOverlappingStoreCount;
        const int overlappingStoreCount = tablet->GetOverlappingStoreCount();
        if (partition->IsEden()) {
            candidate->Slack = static_cast<size_t>(std::max(0, overlappingStoreLimit - overlappingStoreCount));
            candidate->Effect = candidate->Stores.size() - 1;
        } else {
            // For critical partitions, this is equivalent to MOSC-OSC; for unconstrained -- includes extra slack.
            const int edenStoreCount = static_cast<int>(tablet->GetEden()->Stores().size());
            const int partitionStoreCount = static_cast<int>(partition->Stores().size());
            candidate->Slack = static_cast<size_t>(std::max(0, overlappingStoreLimit - edenStoreCount - partitionStoreCount));
            if (tablet->GetCriticalPartitionCount() == 1 && edenStoreCount + partitionStoreCount == overlappingStoreCount) {
                candidate->Effect = candidate->Stores.size() - 1;
            }
        }

        {
            auto guard = Guard(ScanSpinLock_);
            CompactionCandidates_.push_back(std::move(candidate));
        }

        return true;
    }


    std::vector<TStoreId> PickStoresForPartitioning(TPartition* eden)
    {
        std::vector<TStoreId> finalists;

        const auto* tablet = eden->GetTablet();
        const auto& storeManager = tablet->GetStoreManager();
        const auto& config = tablet->GetConfig();

        std::vector<TSortedChunkStorePtr> candidates;

        for (const auto& store : eden->Stores()) {
            if (!storeManager->IsStoreCompactable(store)) {
                continue;
            }

            auto candidate = store->AsSortedChunk();
            candidates.push_back(candidate);

            if (IsCompactionForced(candidate) ||
                IsPeriodicCompactionNeeded(candidate) ||
                IsStoreOutOfTabletRange(candidate, tablet))
            {
                finalists.push_back(candidate->GetId());
            }

            if (finalists.size() >= config->MaxPartitioningStoreCount) {
                break;
            }
        }

        // Check for forced candidates.
        if (!finalists.empty()) {
            return finalists;
        }

        // Sort by decreasing data size.
        std::sort(
            candidates.begin(),
            candidates.end(),
            [] (const TSortedChunkStorePtr& lhs, const TSortedChunkStorePtr& rhs) {
                return lhs->GetCompressedDataSize() > rhs->GetCompressedDataSize();
            });

        i64 dataSizeSum = 0;
        int bestStoreCount = -1;
        for (int i = 0; i < candidates.size(); ++i) {
            dataSizeSum += candidates[i]->GetCompressedDataSize();
            int storeCount = i + 1;
            if (storeCount >= config->MinPartitioningStoreCount &&
                storeCount <= config->MaxPartitioningStoreCount &&
                dataSizeSum >= config->MinPartitioningDataSize &&
                // Ignore max_partitioning_data_size limit for a minimal set of stores.
                (dataSizeSum <= config->MaxPartitioningDataSize || storeCount == config->MinPartitioningStoreCount))
            {
                // Prefer to partition more data.
                bestStoreCount = storeCount;
            }
        }

        if (bestStoreCount > 0) {
            finalists.reserve(bestStoreCount);
            for (int i = 0; i < bestStoreCount; ++i) {
                finalists.push_back(candidates[i]->GetId());
            }
        }

        return finalists;
    }

    std::vector<TStoreId> PickStoresForCompaction(TPartition* partition)
    {
        std::vector<TStoreId> finalists;

        const auto* tablet = partition->GetTablet();
        const auto& storeManager = tablet->GetStoreManager();
        const auto& config = tablet->GetConfig();

        // XXX(savrus) Disabled. Hotfix for YT-5828
#if 0
        // Don't compact partitions (excluding Eden) whose data size exceeds the limit.
        // Let Partition Balancer do its job.
        if (!partition->IsEden() && partition->GetCompressedDataSize() > config->MaxCompactionDataSize) {
            return std::vector<TSortedChunkStorePtr>();
        }
#endif

        std::vector<TSortedChunkStorePtr> candidates;

        for (const auto& store : partition->Stores()) {
            if (!storeManager->IsStoreCompactable(store)) {
                continue;
            }

            // Don't compact large Eden stores.
            if (partition->IsEden() && store->GetCompressedDataSize() >= config->MinPartitioningDataSize) {
                continue;
            }

            auto candidate = store->AsSortedChunk();
            candidates.push_back(candidate);

            if (IsCompactionForced(candidate) ||
                IsPeriodicCompactionNeeded(candidate) ||
                IsStoreOutOfTabletRange(candidate, tablet))
            {
                finalists.push_back(candidate->GetId());
            }

            if (finalists.size() >= config->MaxCompactionStoreCount) {
                break;
            }
        }

        // Check for forced candidates.
        if (!finalists.empty()) {
            return finalists;
        }

        // Sort by increasing data size.
        std::sort(
            candidates.begin(),
            candidates.end(),
            [] (TSortedChunkStorePtr lhs, TSortedChunkStorePtr rhs) {
                return lhs->GetCompressedDataSize() < rhs->GetCompressedDataSize();
            });

        // Partition is critical if it contributes towards the OSC, and MOSC is reached.
        bool criticalPartition = false;
        int overlappingStoreCount;
        if (partition->IsEden()) {
            overlappingStoreCount = tablet->GetOverlappingStoreCount();
        } else {
            overlappingStoreCount = partition->Stores().size() + tablet->GetEden()->Stores().size();
        }
        criticalPartition = overlappingStoreCount >= config->MaxOverlappingStoreCount;

        for (int i = 0; i < candidates.size(); ++i) {
            i64 dataSizeSum = 0;
            int j = i;
            while (j < candidates.size()) {
                int storeCount = j - i;
                if (storeCount > config->MaxCompactionStoreCount) {
                   break;
                }
                i64 dataSize = candidates[j]->GetCompressedDataSize();
                if (!criticalPartition &&
                    dataSize > config->CompactionDataSizeBase &&
                    dataSizeSum > 0 && dataSize > dataSizeSum * config->CompactionDataSizeRatio) {
                    break;
                }
                dataSizeSum += dataSize;
                ++j;
            }

            int storeCount = j - i;
            if (storeCount >= config->MinCompactionStoreCount) {
                finalists.reserve(storeCount);
                while (i < j) {
                    finalists.push_back(candidates[i]->GetId());
                    ++i;
                }
                break;
            }
        }

        return finalists;
    }

    static TTimestamp ComputeMajorTimestamp(
        TPartition* partition,
        const std::vector<TSortedChunkStorePtr>& stores)
    {
        auto result = MaxTimestamp;
        auto handleStore = [&] (const ISortedStorePtr& store) {
            result = std::min(result, store->GetMinTimestamp());
        };

        auto* tablet = partition->GetTablet();
        auto* eden = tablet->GetEden();

        for (const auto& store : eden->Stores()) {
            handleStore(store);
        }

        for (const auto& store : partition->Stores()) {
            if (store->GetType() == EStoreType::SortedChunk) {
                if (std::find(stores.begin(), stores.end(), store->AsSortedChunk()) == stores.end()) {
                    handleStore(store);
                }
            }
        }

        return result;
    }

    void PickMoreTasks(
        std::vector<std::unique_ptr<TTask>>* candidates,
        std::vector<std::unique_ptr<TTask>>* tasks,
        size_t* index,
        NProfiling::TSimpleCounter& counter)
    {
        if (candidates->empty()) {
            return;
        }

        Profiler.Update(counter, candidates->size());

        auto&& comparer = [] (const std::unique_ptr<TTask>& lhs, const std::unique_ptr<TTask>& rhs) -> bool {
            return lhs->MakeComparableValue() < rhs->MakeComparableValue();
        };

        MakeHeap(
            candidates->begin(),
            candidates->end(),
            std::move(comparer));

        {
            auto guard = Guard(TaskSpinLock_);
            tasks->swap(*candidates);
            *index = tasks->size();
        }
        candidates->clear();
    }

    void PickMorePartitionings(TGuard<TSpinLock>&)
    {
        PickMoreTasks(
            &PartitioningCandidates_,
            &PartitioningTasks_,
            &PartitioningTaskIndex_,
            FeasiblePartitioningsCounter_);
    }

    void PickMoreCompactions(TGuard<TSpinLock>&)
    {
        PickMoreTasks(
            &CompactionCandidates_,
            &CompactionTasks_,
            &CompactionTaskIndex_,
            FeasibleCompactionsCounter_);
    }

    void ScheduleMoreTasks(
        std::vector<std::unique_ptr<TTask>>* tasks,
        size_t* index,
        const TAsyncSemaphorePtr& semaphore,
        NProfiling::TSimpleCounter& counter,
        void (TStoreCompactor::*action)(TAsyncSemaphoreGuard, const TTask*))
    {
        auto guard = Guard(TaskSpinLock_);

        size_t scheduled = 0;

        while (true) {
            if (*index == 0) {
                break;
            }

            auto semaphoreGuard = TAsyncSemaphoreGuard::TryAcquire(semaphore);
            if (!semaphoreGuard) {
                break;
            }

            auto&& comparer = [] (const std::unique_ptr<TTask>& lhs, const std::unique_ptr<TTask>& rhs) -> bool {
                return lhs->MakeComparableValue() < rhs->MakeComparableValue();
            };

            ExtractHeap(
                tasks->begin(),
                tasks->begin() + *index,
                comparer);

            --(*index);
            auto&& task = tasks->at(*index);

            auto invoker = task->Invoker;
            invoker->Invoke(BIND(action, MakeStrong(this), Passed(std::move(semaphoreGuard)), Owned(task.release())));

            ++scheduled;
        }

        if (scheduled > 0) {
            Profiler.Increment(counter, scheduled);
        }
    }

    void ScheduleMorePartitionings()
    {
        ScheduleMoreTasks(
            &PartitioningTasks_,
            &PartitioningTaskIndex_,
            PartitioningSemaphore_,
            ScheduledPartitioningsCounter_,
            &TStoreCompactor::PartitionEden);
    }

    void ScheduleMoreCompactions()
    {
        ScheduleMoreTasks(
            &CompactionTasks_,
            &CompactionTaskIndex_,
            CompactionSemaphore_,
            ScheduledCompactionsCounter_,
            &TStoreCompactor::CompactPartition);
    }

    void PartitionEden(TAsyncSemaphoreGuard guard, const TTask* task)
    {
        auto sessionId = TReadSessionId();
        NLogging::TLogger Logger(TabletNodeLogger);
        Logger.AddTag("TabletId: %v, ReadSessionId: %v",
            task->Tablet,
            sessionId);

        auto doneGuard = Finally([&] {
            ScheduleMorePartitionings();
        });

        const auto& slot = task->Slot;
        const auto& tabletManager = slot->GetTabletManager();
        auto* tablet = tabletManager->FindTablet(task->Tablet);
        if (!tablet) {
            LOG_DEBUG("Tablet is missing, aborting partitioning");
            return;
        }

        const auto& slotManager = Bootstrap_->GetTabletSlotManager();
        auto tabletSnapshot = slotManager->FindTabletSnapshot(task->Tablet);
        if (!tabletSnapshot) {
            LOG_DEBUG("Tablet snapshot is missing, aborting partitioning");
            return;
        }

        auto* eden = tablet->GetEden();
        if (eden->GetId() != task->Partition) {
            LOG_DEBUG("Eden is missing, aborting partitioning");
            return;
        }

        if (eden->GetState() != EPartitionState::Normal) {
            LOG_DEBUG("Eden is in improper state, aborting partitioning (EdenState: %v)", eden->GetState());
            return;
        }

        const auto& storeManager = tablet->GetStoreManager();

        std::vector<TSortedChunkStorePtr> stores;
        stores.reserve(task->Stores.size());
        for (const auto& storeId : task->Stores) {
            auto store = tablet->FindStore(storeId);
            if (!store || !eden->Stores().has(store->AsSorted())) {
                LOG_DEBUG("Eden store is missing, aborting partitioning (StoreId: %v)", storeId);
                return;
            }
            auto typedStore = store->AsSortedChunk();
            YCHECK(typedStore);
            if (typedStore->GetCompactionState() != EStoreCompactionState::None) {
                LOG_DEBUG("Eden store is in improper state, aborting partitioning (StoreId: %v, CompactionState: %v)",
                    storeId,
                    typedStore->GetCompactionState());
                return;
            }
            stores.push_back(std::move(typedStore));
        }

        std::vector<TOwningKey> pivotKeys;
        for (const auto& partition : tablet->PartitionList()) {
            pivotKeys.push_back(partition->GetPivotKey());
        }

        YCHECK(tablet->GetPivotKey() == pivotKeys[0]);

        eden->CheckedSetState(EPartitionState::Normal, EPartitionState::Partitioning);

        try {
            i64 dataSize = 0;
            for (const auto& store : stores) {
                dataSize += store->GetCompressedDataSize();
                storeManager->BeginStoreCompaction(store);
            }

            auto timestampProvider = Bootstrap_
                ->GetMasterClient()
                ->GetNativeConnection()
                ->GetTimestampProvider();
            auto currentTimestamp = WaitFor(timestampProvider->GenerateTimestamps())
                .ValueOrThrow();

            auto beginInstant = TInstant::Now();
            eden->SetCompactionTime(beginInstant);

            LOG_INFO("Eden partitioning started (Slack: %v, Effect: %v, PartitionCount: %v, DataSize: %v, "
                "ChunkCount: %v, CurrentTimestamp: %llx)",
                task->Slack,
                task->Effect,
                pivotKeys.size(),
                dataSize,
                stores.size(),
                currentTimestamp);

            auto reader = CreateVersionedTabletReader(
                tabletSnapshot,
                std::vector<ISortedStorePtr>(stores.begin(), stores.end()),
                tablet->GetPivotKey(),
                tablet->GetNextPivotKey(),
                currentTimestamp,
                MinTimestamp, // NB: No major compaction during Eden partitioning.
                TWorkloadDescriptor(EWorkloadCategory::SystemTabletPartitioning),
                sessionId,
                stores.size());

            INativeTransactionPtr transaction;
            {
                LOG_INFO("Creating Eden partitioning transaction");

                TTransactionStartOptions options;
                options.AutoAbort = false;
                auto attributes = CreateEphemeralAttributes();
                attributes->Set("title", Format("Eden partitioning: tablet %v",
                    tabletSnapshot->TabletId));
                options.Attributes = std::move(attributes);

                auto asyncTransaction = Bootstrap_->GetMasterClient()->StartNativeTransaction(
                    NTransactionClient::ETransactionType::Master,
                    options);
                transaction = WaitFor(asyncTransaction)
                    .ValueOrThrow();

                LOG_INFO("Eden partitioning transaction created (TransactionId: %v)",
                    transaction->GetId());

                Logger.AddTag("TransactionId: %v", transaction->GetId());
            }

            auto asyncResult =
                BIND(
                    &TStoreCompactor::DoPartitionEden,
                    MakeStrong(this),
                    reader,
                    tabletSnapshot,
                    transaction,
                    pivotKeys,
                    tablet->GetNextPivotKey(),
                    Logger)
                .AsyncVia(ThreadPool_->GetInvoker())
                .Run();

            std::vector<IVersionedMultiChunkWriterPtr> writers;
            int rowCount;
            std::tie(writers, rowCount) = WaitFor(asyncResult)
                .ValueOrThrow();

            auto endInstant = TInstant::Now();

            // We can release semaphore, because we are no longer actively using resources.
            guard.Release();
            doneGuard.Release();

            NTabletServer::NProto::TReqUpdateTabletStores actionRequest;
            ToProto(actionRequest.mutable_tablet_id(), tablet->GetId());
            actionRequest.set_mount_revision(tablet->GetMountRevision());

            TStoreIdList storeIdsToRemove;
            for (const auto& store : stores) {
                auto* descriptor = actionRequest.add_stores_to_remove();
                auto storeId = store->GetId();
                ToProto(descriptor->mutable_store_id(), storeId);
                storeIdsToRemove.push_back(storeId);
            }

            // TODO(sandello): Move specs?
            TStoreIdList storeIdsToAdd;
            for (const auto& writer : writers) {
                for (const auto& chunkSpec : writer->GetWrittenChunksMasterMeta()) {
                    auto* descriptor = actionRequest.add_stores_to_add();
                    descriptor->set_store_type(static_cast<int>(EStoreType::SortedChunk));
                    descriptor->mutable_store_id()->CopyFrom(chunkSpec.chunk_id());
                    descriptor->mutable_chunk_meta()->CopyFrom(chunkSpec.chunk_meta());
                    storeIdsToAdd.push_back(FromProto<TStoreId>(chunkSpec.chunk_id()));
                }

                tabletSnapshot->PerformanceCounters->PartitioningDataWeightCount += writer->GetDataStatistics().data_weight();

                ProfileDiskPressure(
                    tabletSnapshot,
                    writer->GetDataStatistics(),
                    PartitioningTag_);
            }

            LOG_INFO("Eden partitioning completed (RowCount: %v, StoreIdsToAdd: %v, StoreIdsToRemove: %v, WallTime: %v)",
                rowCount,
                storeIdsToAdd,
                storeIdsToRemove,
                endInstant - beginInstant);

            auto actionData = MakeTransactionActionData(actionRequest);
            transaction->AddAction(Bootstrap_->GetMasterClient()->GetNativeConnection()->GetPrimaryMasterCellId(), actionData);
            transaction->AddAction(slot->GetCellId(), actionData);

            WaitFor(tabletManager->CommitTabletStoresUpdateTransaction(tablet, transaction))
                .ThrowOnError();

            for (const auto& store : stores) {
                storeManager->EndStoreCompaction(store);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error partitioning Eden, backing off");

            for (const auto& store : stores) {
                storeManager->BackoffStoreCompaction(store);
            }
        }

        eden->CheckedSetState(EPartitionState::Partitioning, EPartitionState::Normal);
    }

    std::tuple<std::vector<IVersionedMultiChunkWriterPtr>, int> DoPartitionEden(
        const IVersionedReaderPtr& reader,
        const TTabletSnapshotPtr& tabletSnapshot,
        const ITransactionPtr& transaction,
        const std::vector<TOwningKey>& pivotKeys,
        const TOwningKey& nextTabletPivotKey,
        NLogging::TLogger Logger)
    {
        auto writerConfig = CloneYsonSerializable(tabletSnapshot->WriterConfig);
        writerConfig->MinUploadReplicationFactor = writerConfig->UploadReplicationFactor;
        writerConfig->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemTabletPartitioning);
        auto writerOptions = CloneYsonSerializable(tabletSnapshot->WriterOptions);
        writerOptions->ValidateResourceUsageIncrease = false;

        int writerPoolSize = std::min(
            static_cast<int>(pivotKeys.size()),
            Config_->StoreCompactor->PartitioningWriterPoolSize);
        TChunkWriterPool writerPool(
            Bootstrap_->GetInMemoryManager(),
            tabletSnapshot,
            writerPoolSize,
            writerConfig,
            writerOptions,
            Bootstrap_->GetMasterClient(),
            transaction->GetId());

        std::vector<TVersionedRow> writeRows;
        writeRows.reserve(MaxRowsPerWrite);

        int currentPartitionIndex = 0;
        TOwningKey currentPivotKey;
        TOwningKey nextPivotKey;

        int currentPartitionRowCount = 0;
        int readRowCount = 0;
        int writeRowCount = 0;
        IVersionedMultiChunkWriterPtr currentWriter;

        auto ensurePartitionStarted = [&] () {
            if (currentWriter)
                return;

            LOG_INFO("Started writing partition (PartitionIndex: %v, Keys: %v .. %v)",
                currentPartitionIndex,
                currentPivotKey,
                nextPivotKey);

            currentWriter = writerPool.AllocateWriter();
        };

        auto flushOutputRows = [&] () {
            if (writeRows.empty())
                return;

            writeRowCount += writeRows.size();

            ensurePartitionStarted();
            if (!currentWriter->Write(writeRows)) {
                WaitFor(currentWriter->GetReadyEvent())
                    .ThrowOnError();
            }

            writeRows.clear();
        };

        auto writeOutputRow = [&] (TVersionedRow row) {
            if (writeRows.size() == writeRows.capacity()) {
                flushOutputRows();
            }
            writeRows.push_back(row);
            ++currentPartitionRowCount;
        };

        auto flushPartition = [&] () {
            flushOutputRows();

            if (currentWriter) {
                LOG_INFO("Finished writing partition (PartitionIndex: %v, RowCount: %v)",
                    currentPartitionIndex,
                    currentPartitionRowCount);

                writerPool.ReleaseWriter(currentWriter);
                currentWriter.Reset();
            }

            currentPartitionRowCount = 0;
            ++currentPartitionIndex;
        };

        std::vector<TVersionedRow> readRows;
        readRows.reserve(MaxRowsPerRead);
        int currentRowIndex = 0;

        auto peekInputRow = [&] () -> TVersionedRow {
            if (currentRowIndex == readRows.size()) {
                // readRows will be invalidated, must flush writeRows.
                flushOutputRows();
                currentRowIndex = 0;
                while (true) {
                    if (!reader->Read(&readRows)) {
                        return TVersionedRow();
                    }
                    readRowCount += readRows.size();
                    if (!readRows.empty())
                        break;
                    WaitFor(reader->GetReadyEvent())
                        .ThrowOnError();
                }
            }
            return readRows[currentRowIndex];
        };

        auto skipInputRow = [&] () {
            ++currentRowIndex;
        };

        WaitFor(reader->Open())
            .ThrowOnError();

        for (auto it = pivotKeys.begin(); it != pivotKeys.end(); ++it) {
            currentPivotKey = *it;
            nextPivotKey = it == pivotKeys.end() - 1 ? nextTabletPivotKey : *(it + 1);

            while (true) {
                auto row = peekInputRow();
                if (!row) {
                    break;
                }

                // NB: pivot keys can be of arbitrary schema and length.
                YCHECK(CompareRows(currentPivotKey.Begin(), currentPivotKey.End(), row.BeginKeys(), row.EndKeys()) <= 0);

                if (CompareRows(nextPivotKey.Begin(), nextPivotKey.End(), row.BeginKeys(), row.EndKeys()) <= 0) {
                    break;
                }

                skipInputRow();
                writeOutputRow(row);
            }

            flushPartition();
        }

        YCHECK(readRowCount == writeRowCount);

        return std::make_tuple(writerPool.GetAllWriters(), readRowCount);
    }

    void CompactPartition(TAsyncSemaphoreGuard guard, const TTask* task)
    {
        auto sessionId = TReadSessionId();
        NLogging::TLogger Logger(TabletNodeLogger);
        Logger.AddTag("TabletId: %v, ReadSessionId: %v",
            task->Tablet,
            sessionId);

        auto doneGuard = Finally([&] {
            ScheduleMoreCompactions();
        });

        const auto& slot = task->Slot;
        const auto& tabletManager = slot->GetTabletManager();
        auto* tablet = tabletManager->FindTablet(task->Tablet);
        if (!tablet) {
            LOG_DEBUG("Tablet is missing, aborting compaction");
            return;
        }

        const auto& slotManager = Bootstrap_->GetTabletSlotManager();
        auto tabletSnapshot = slotManager->FindTabletSnapshot(task->Tablet);
        if (!tabletSnapshot) {
            LOG_DEBUG("Tablet snapshot is missing, aborting compaction");
            return;
        }

        auto* partition = tablet->GetEden()->GetId() == task->Partition
            ? tablet->GetEden()
            : tablet->FindPartition(task->Partition);
        if (!partition) {
            LOG_DEBUG("Partition is missing, aborting compaction");
            return;
        }

        if (partition->GetState() != EPartitionState::Normal) {
            LOG_DEBUG("Partition is in improper state, aborting compaction (PartitionState: %v)", partition->GetState());
            return;
        }

        const auto& storeManager = tablet->GetStoreManager();

        std::vector<TSortedChunkStorePtr> stores;
        stores.reserve(task->Stores.size());
        for (const auto& storeId : task->Stores) {
            auto store = tablet->FindStore(storeId);
            if (!store || !partition->Stores().has(store->AsSorted())) {
                LOG_DEBUG("Partition store is missing, aborting compaction (StoreId: %v)", storeId);
                return;
            }
            auto typedStore = store->AsSortedChunk();
            YCHECK(typedStore);
            if (typedStore->GetCompactionState() != EStoreCompactionState::None) {
                LOG_DEBUG("Partition store is in improper state, aborting compaction (StoreId: %v, CompactionState: %v)",
                    storeId,
                    typedStore->GetCompactionState());
                return;
            }
            stores.push_back(std::move(typedStore));
        }

        Logger.AddTag("Eden: %v, PartitionRange: %v .. %v",
            partition->IsEden(),
            partition->GetPivotKey(),
            partition->GetNextPivotKey());

        partition->CheckedSetState(EPartitionState::Normal, EPartitionState::Compacting);

        try {
            i64 dataSize = 0;
            for (const auto& store : stores) {
                dataSize += store->GetCompressedDataSize();
                storeManager->BeginStoreCompaction(store);
            }

            auto timestampProvider = Bootstrap_
                ->GetMasterClient()
                ->GetNativeConnection()
                ->GetTimestampProvider();
            auto currentTimestamp = WaitFor(timestampProvider->GenerateTimestamps())
                .ValueOrThrow();

            auto beginInstant = TInstant::Now();
            partition->SetCompactionTime(beginInstant);

            auto majorTimestamp = ComputeMajorTimestamp(partition, stores);
            auto retainedTimestamp = InstantToTimestamp(TimestampToInstant(currentTimestamp).first - tablet->GetConfig()->MinDataTtl).first;
            majorTimestamp = std::min(majorTimestamp, retainedTimestamp);

            LOG_INFO("Partition compaction started (Slack: %v, Effect: %v, DataSize: %v, ChunkCount: %v, "
                "CurrentTimestamp: %llx, MajorTimestamp: %llx, RetainedTimestamp: %llx)",
                task->Slack,
                task->Effect,
                dataSize,
                stores.size(),
                currentTimestamp,
                majorTimestamp,
                retainedTimestamp);

            auto reader = CreateVersionedTabletReader(
                tabletSnapshot,
                std::vector<ISortedStorePtr>(stores.begin(), stores.end()),
                tablet->GetPivotKey(),
                tablet->GetNextPivotKey(),
                currentTimestamp,
                majorTimestamp,
                TWorkloadDescriptor(EWorkloadCategory::SystemTabletCompaction),
                sessionId,
                stores.size());

            INativeTransactionPtr transaction;
            {
                LOG_INFO("Creating partition compaction transaction");

                TTransactionStartOptions options;
                options.AutoAbort = false;
                auto attributes = CreateEphemeralAttributes();
                attributes->Set("title", Format("Partition compaction: tablet %v",
                    tabletSnapshot->TabletId));
                options.Attributes = std::move(attributes);

                auto asyncTransaction = Bootstrap_->GetMasterClient()->StartNativeTransaction(
                    NTransactionClient::ETransactionType::Master,
                    options);
                transaction = WaitFor(asyncTransaction)
                    .ValueOrThrow();

                LOG_INFO("Partition compaction transaction created (TransactionId: %v)",
                    transaction->GetId());

                Logger.AddTag("TransactionId: %v", transaction->GetId());
            }

            auto asyncResult =
                BIND(
                    &TStoreCompactor::DoCompactPartition,
                    MakeStrong(this),
                    reader,
                    tabletSnapshot,
                    transaction,
                    partition->IsEden(),
                    Logger)
                .AsyncVia(ThreadPool_->GetInvoker())
                .Run();

            IVersionedMultiChunkWriterPtr writer;
            int rowCount;
            std::tie(writer, rowCount) = WaitFor(asyncResult)
                .ValueOrThrow();

            auto endInstant = TInstant::Now();

            // We can release semaphore, because we are no longer actively using resources.
            guard.Release();
            doneGuard.Release();

            NTabletServer::NProto::TReqUpdateTabletStores actionRequest;
            ToProto(actionRequest.mutable_tablet_id(), tablet->GetId());
            actionRequest.set_mount_revision(tablet->GetMountRevision());
            actionRequest.set_retained_timestamp(retainedTimestamp);

            TStoreIdList storeIdsToRemove;
            for (const auto& store : stores) {
                auto* descriptor = actionRequest.add_stores_to_remove();
                auto storeId = store->GetId();
                ToProto(descriptor->mutable_store_id(), storeId);
                storeIdsToRemove.push_back(storeId);
            }

            // TODO(sandello): Move specs?
            TStoreIdList storeIdsToAdd;
            for (const auto& chunkSpec : writer->GetWrittenChunksMasterMeta()) {
                auto* descriptor = actionRequest.add_stores_to_add();
                descriptor->set_store_type(static_cast<int>(EStoreType::SortedChunk));
                descriptor->mutable_store_id()->CopyFrom(chunkSpec.chunk_id());
                descriptor->mutable_chunk_meta()->CopyFrom(chunkSpec.chunk_meta());
                storeIdsToAdd.push_back(FromProto<TStoreId>(chunkSpec.chunk_id()));

            }

            tabletSnapshot->PerformanceCounters->CompactionDataWeightCount += writer->GetDataStatistics().data_weight();

            ProfileDiskPressure(
                tabletSnapshot,
                writer->GetDataStatistics(),
                CompactionTag_);

            LOG_INFO("Partition compaction completed (RowCount: %v, StoreIdsToAdd: %v, StoreIdsToRemove: %v, WallTime: %v)",
                rowCount,
                storeIdsToAdd,
                storeIdsToRemove,
                endInstant - beginInstant);

            auto actionData = MakeTransactionActionData(actionRequest);
            transaction->AddAction(Bootstrap_->GetMasterClient()->GetNativeConnection()->GetPrimaryMasterCellId(), actionData);
            transaction->AddAction(slot->GetCellId(), actionData);

            WaitFor(tabletManager->CommitTabletStoresUpdateTransaction(tablet, transaction))
                .ThrowOnError();

            for (const auto& store : stores) {
                storeManager->EndStoreCompaction(store);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error compacting partition, backing off");

            for (const auto& store : stores) {
                storeManager->BackoffStoreCompaction(store);
            }
        }

        partition->CheckedSetState(EPartitionState::Compacting, EPartitionState::Normal);
    }

    std::tuple<IVersionedMultiChunkWriterPtr, int> DoCompactPartition(
        const IVersionedReaderPtr& reader,
        const TTabletSnapshotPtr& tabletSnapshot,
        const ITransactionPtr& transaction,
        bool isEden,
        NLogging::TLogger Logger)
    {
        auto writerConfig = CloneYsonSerializable(tabletSnapshot->WriterConfig);
        writerConfig->MinUploadReplicationFactor = writerConfig->UploadReplicationFactor;
        writerConfig->WorkloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemTabletCompaction);
        auto writerOptions = CloneYsonSerializable(tabletSnapshot->WriterOptions);
        writerOptions->ChunksEden = isEden;
        writerOptions->ValidateResourceUsageIncrease = false;

        TChunkWriterPool writerPool(
            Bootstrap_->GetInMemoryManager(),
            tabletSnapshot,
            1,
            writerConfig,
            writerOptions,
            Bootstrap_->GetMasterClient(),
            transaction->GetId());
        auto writer = writerPool.AllocateWriter();

        WaitFor(reader->Open())
            .ThrowOnError();

        WaitFor(writer->Open())
            .ThrowOnError();

        std::vector<TVersionedRow> rows;
        rows.reserve(MaxRowsPerRead);

        int readRowCount = 0;
        int writeRowCount = 0;

        while (reader->Read(&rows)) {
            readRowCount += rows.size();

            if (rows.empty()) {
                WaitFor(reader->GetReadyEvent())
                    .ThrowOnError();
                continue;
            }

            writeRowCount += rows.size();
            if (!writer->Write(rows)) {
                WaitFor(writer->GetReadyEvent())
                    .ThrowOnError();
            }
        }

        WaitFor(writer->Close())
            .ThrowOnError();

        YCHECK(readRowCount == writeRowCount);

        return std::make_tuple(writer, readRowCount);
    }

    static bool IsCompactionForced(const TSortedChunkStorePtr& store)
    {
        const auto& config = store->GetTablet()->GetConfig();
        if (!config->ForcedCompactionRevision) {
            return false;
        }

        ui64 revision = CounterFromId(store->GetId());
        if (revision > *config->ForcedCompactionRevision) {
            return false;
        }

        return true;
    }

    static bool IsPeriodicCompactionNeeded(const TSortedChunkStorePtr& store)
    {
        const auto& config = store->GetTablet()->GetConfig();
        if (!config->AutoCompactionPeriod) {
            return false;
        }

        if (TInstant::Now() < store->GetCreationTime() + *config->AutoCompactionPeriod) {
            return false;
        }

        return true;
    }

    static bool IsStoreOutOfTabletRange(const TSortedChunkStorePtr& store, const TTablet* tablet)
    {
        if (store->GetMinKey() < tablet->GetPivotKey()) {
            return true;
        }

        if (store->GetMaxKey() >= tablet->GetNextPivotKey()) {
            return true;
        }

        return false;
    }
};

void StartStoreCompactor(
    TTabletNodeConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
{
    if (config->EnableStoreCompactor) {
        New<TStoreCompactor>(config, bootstrap)->Start();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
