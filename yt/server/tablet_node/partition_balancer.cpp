#include "stdafx.h"
#include "partition_balancer.h"
#include "config.h"
#include "tablet_slot.h"
#include "slot_manager.h"
#include "tablet_manager.h"
#include "tablet.h"
#include "partition.h"
#include "store.h"
#include "chunk_store.h"
#include "private.h"

#include <core/concurrency/scheduler.h>
#include <core/concurrency/async_semaphore.h>

#include <core/logging/log.h>

#include <ytlib/tablet_client/config.h>

#include <ytlib/new_table_client/unversioned_row.h>
#include <ytlib/new_table_client/samples_fetcher.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/chunk_client/chunk_spec.h>
#include <ytlib/chunk_client/chunk_service_proxy.h>

#include <ytlib/api/client.h>

#include <server/hydra/hydra_manager.h>
#include <server/hydra/mutation.h>

#include <server/tablet_node/tablet_manager.pb.h>

#include <server/cell_node/bootstrap.h>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NHydra;
using namespace NVersionedTableClient;
using namespace NNodeTrackerClient;
using namespace NChunkClient;
using namespace NTabletNode::NProto;

////////////////////////////////////////////////////////////////////////////////

class TPartitionBalancer
    : public TRefCounted
{
public:
    TPartitionBalancer(
        TPartitionBalancerConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , Semaphore_(Config_->MaxConcurrentSamplings)
    {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->SubscribeScanSlot(BIND(&TPartitionBalancer::OnScanSlot, MakeStrong(this)));
    }

private:
    TPartitionBalancerConfigPtr Config_;
    NCellNode::TBootstrap* Bootstrap_;
    TAsyncSemaphore Semaphore_;


    void OnScanSlot(TTabletSlotPtr slot)
    {
        if (slot->GetAutomatonState() != EPeerState::Leading) {
            return;
        }

        auto tabletManager = slot->GetTabletManager();
        for (const auto& pair : tabletManager->Tablets()) {
            auto* tablet = pair.second;
            ScanTablet(slot, tablet);
        }
    }

    void ScanTablet(TTabletSlotPtr slot, TTablet* tablet)
    {
        if (tablet->GetState() != ETabletState::Mounted) {
            return;
        }
        
        for (const auto& partition : tablet->Partitions()) {
            ScanPartition(slot, partition.get());
        }
    }

    void ScanPartition(TTabletSlotPtr slot, TPartition* partition)
    {
        auto* tablet = partition->GetTablet();

        const auto& config = tablet->GetConfig();

        int partitionCount = tablet->Partitions().size();

        i64 actualDataSize = partition->GetUncompressedDataSize();

        // Maximum data size the partition might have if all chunk stores from Eden go here.
        i64 maxPotentialDataSize = actualDataSize;
        for (const auto& store : tablet->GetEden()->Stores()) {
            if (store->GetType() == EStoreType::Chunk) {
                maxPotentialDataSize += store->GetUncompressedDataSize();
            }
        }

        if (actualDataSize > config->MaxPartitionDataSize) {
            int splitFactor = std::min(std::min(
                actualDataSize / config->DesiredPartitionDataSize + 1,
                actualDataSize / config->MinPartitioningDataSize),
                static_cast<i64>(config->MaxPartitionCount - partitionCount));
            if (splitFactor > 1) {
                RunSplit(partition, splitFactor);
            }
        }
        
        if (maxPotentialDataSize < config->MinPartitionDataSize && partitionCount > 1) {
            int firstPartitionIndex = partition->GetIndex();
            int lastPartitionIndex = firstPartitionIndex + 1;
            if (lastPartitionIndex == partitionCount) {
                --firstPartitionIndex;
                --lastPartitionIndex;
            }
            RunMerge(partition, firstPartitionIndex, lastPartitionIndex);
        }

        if (partition->GetSamplingRequestTime() > partition->GetSamplingTime() &&
            partition->GetSamplingTime() < TInstant::Now() - Config_->ResamplingPeriod)
        {
            RunSample(partition);
        }
    }


    void RunSplit(TPartition* partition, int splitFactor)
    {
        if (partition->GetState() != EPartitionState::Normal) {
            return;
        }

        for (const auto& store : partition->Stores()) {
            if (store->GetStoreState() != EStoreState::Persistent) {
                return;
            }
        }

        partition->CheckedSetState(EPartitionState::Normal, EPartitionState::Splitting);

        BIND(&TPartitionBalancer::DoRunSplit, MakeStrong(this))
            .AsyncVia(partition->GetTablet()->GetEpochAutomatonInvoker())
            .Run(partition, splitFactor);
    }

    void DoRunSplit(TPartition* partition, int splitFactor)
    {
        auto Logger = BuildLogger(partition);

        auto* tablet = partition->GetTablet();
        auto slot = tablet->GetSlot();
        auto hydraManager = slot->GetHydraManager();

        LOG_INFO("Partition is eligible for split (SplitFactor: %v)",
            splitFactor);

        try {
            auto samples = GetPartitionSamples(partition, Config_->MaxPartitioningSampleCount);
            int sampleCount = static_cast<int>(samples.size());
            int minSampleCount = std::max(Config_->MinPartitioningSampleCount, splitFactor);
            if (sampleCount < minSampleCount) {
                THROW_ERROR_EXCEPTION("Too few samples fetched: need %v, got %v",
                    minSampleCount,
                    sampleCount);
            }

            std::vector<TOwningKey> pivotKeys;
            // Take the pivot of the partition.
            pivotKeys.push_back(partition->GetPivotKey());
            // And add |splitFactor - 1| more keys from samples.
            for (int i = 0; i < splitFactor - 1; ++i) {
                int j = (i + 1) * sampleCount / splitFactor - 1;
                const auto& key = samples[j];
                if (key > pivotKeys.back()) {
                    pivotKeys.push_back(key);
                }
            }

            if (pivotKeys.size() < 2) {
                THROW_ERROR_EXCEPTION("No valid pivot keys can be obtained from samples");
            }

            TReqSplitPartition request;
            ToProto(request.mutable_tablet_id(), tablet->GetTabletId());
            ToProto(request.mutable_partition_id(), partition->GetId());
            ToProto(request.mutable_pivot_keys(), pivotKeys);

            CreateMutation(hydraManager, request)
                ->Commit()
                .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TMutationResponse>& error) {
                    if (!error.IsOK()) {
                        LOG_ERROR(error, "Error committing partition split mutation");
                    }
                }));
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Partitioning aborted");
            partition->CheckedSetState(EPartitionState::Splitting, EPartitionState::Normal);
        }
    }


    void RunMerge(
        TPartition* partition,
        int firstPartitionIndex,
        int lastPartitionIndex)
    {
        auto* tablet = partition->GetTablet();

        for (int index = firstPartitionIndex; index <= lastPartitionIndex; ++index) {
            if (tablet->Partitions()[index]->GetState() != EPartitionState::Normal) {
                return;
            }
        }

        for (int index = firstPartitionIndex; index <= lastPartitionIndex; ++index) {
            tablet->Partitions()[index]->CheckedSetState(EPartitionState::Normal, EPartitionState::Merging);
        }

        auto Logger = TabletNodeLogger;
        Logger.AddTag("TabletId: %v, PartitionIds: [%v]",
            partition->GetTablet()->GetTabletId(),
            JoinToString(ConvertToStrings(
                tablet->Partitions().begin() + firstPartitionIndex,
                tablet->Partitions().begin() + lastPartitionIndex + 1,
                [] (const std::unique_ptr<TPartition>& partition) {
                     return ToString(partition->GetId());
                })));

        LOG_INFO("Partition is eligible for merge");

        auto slot = tablet->GetSlot();
        auto hydraManager = slot->GetHydraManager();

        TReqMergePartitions request;
        ToProto(request.mutable_tablet_id(), tablet->GetTabletId());
        ToProto(request.mutable_partition_id(), tablet->Partitions()[firstPartitionIndex]->GetId());
        request.set_partition_count(lastPartitionIndex - firstPartitionIndex + 1);

        CreateMutation(hydraManager, request)
            ->Commit()
            .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TMutationResponse>& error) {
                if (!error.IsOK()) {
                    LOG_ERROR(error, "Error committing partition merge mutation");
                }
            }));
    }


    void RunSample(TPartition* partition)
    {
        if (partition->GetState() != EPartitionState::Normal) {
            return;
        }

        auto guard = TAsyncSemaphoreGuard::TryAcquire(&Semaphore_);
        if (!guard) {
            return;
        }

        partition->CheckedSetState(EPartitionState::Normal, EPartitionState::Sampling);

        BIND(&TPartitionBalancer::DoRunSample, MakeStrong(this), Passed(std::move(guard)))
            .AsyncVia(partition->GetTablet()->GetEpochAutomatonInvoker())
            .Run(partition);
    }

    void DoRunSample(TAsyncSemaphoreGuard /* guard */, TPartition* partition)
    {
        auto Logger = BuildLogger(partition);

        auto* tablet = partition->GetTablet();
        auto config = tablet->GetConfig();

        auto slot = tablet->GetSlot();
        auto hydraManager = slot->GetHydraManager();

        LOG_INFO("Sampling partition (DesiredSampleCount: %v)",
            config->SamplesPerPartition);

        try {
            auto samples = GetPartitionSamples(partition, config->SamplesPerPartition);
            samples.erase(
                std::unique(samples.begin(), samples.end()),
                samples.end());

            TReqUpdatePartitionSampleKeys request;
            ToProto(request.mutable_tablet_id(), tablet->GetTabletId());
            ToProto(request.mutable_partition_id(), partition->GetId());
            ToProto(request.mutable_sample_keys(), samples);

            CreateMutation(hydraManager, request)
                ->Commit()
                .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TMutationResponse>& error) {
                    if (!error.IsOK()) {
                        LOG_ERROR(error, "Error committing sample keys update mutation");
                    }
                }));
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Partition sampling aborted");
        }

        partition->CheckedSetState(EPartitionState::Sampling, EPartitionState::Normal);
        // NB: Update the timestamp even in case of failure to prevent
        // repeating unsuccessful samplings too rapidly.
        partition->SetSamplingTime(TInstant::Now());
    }


    std::vector<TOwningKey> GetPartitionSamples(
        TPartition* partition,
        int maxSampleCount)
    {
        YCHECK(!partition->IsEden());

        if (maxSampleCount == 0) {
            return std::vector<TOwningKey>();
        }

        auto Logger = BuildLogger(partition);

        auto* tablet = partition->GetTablet();

        auto nodeDirectory = New<TNodeDirectory>();

        auto fetcher = New<TSamplesFetcher>(
            Config_->SamplesFetcher,
            maxSampleCount,
            tablet->KeyColumns(),
            std::numeric_limits<i64>::max(),
            nodeDirectory,
            GetCurrentInvoker(),
            Logger);

        {
            auto channel = Bootstrap_->GetMasterClient()->GetMasterChannel(NApi::EMasterChannelKind::LeaderOrFollower);
            TChunkServiceProxy proxy(channel);

            auto req = proxy.LocateChunks();

            yhash_map<TChunkId, TChunkStorePtr> storeMap;

            auto addStore = [&] (IStorePtr store) {
                if (store->GetType() != EStoreType::Chunk)
                    return;

                if (store->GetMaxKey() <= partition->GetPivotKey() ||
                    store->GetMinKey() >= partition->GetNextPivotKey())
                    return;

                const auto& chunkId = store->GetId();
                YCHECK(storeMap.insert(std::make_pair(chunkId, store->AsChunk())).second);
                ToProto(req->add_chunk_ids(), chunkId);
            };

            auto addStores = [&] (const yhash_set<IStorePtr>& stores) {
                for (const auto& store : stores) {
                    addStore(store);
                }
            };

            addStores(partition->Stores());
            addStores(tablet->GetEden()->Stores());

            LOG_INFO("Locating partition chunks (ChunkCount: %v)",
                storeMap.size());

            auto rsp = WaitFor(req->Invoke())
                .ValueOrThrow();

            LOG_INFO("Partition chunks located");

            nodeDirectory->MergeFrom(rsp->node_directory());

            for (const auto& chunkInfo : rsp->chunks()) {
                auto chunkId = FromProto<TChunkId>(chunkInfo.chunk_id());
                auto storeIt = storeMap.find(chunkId);
                YCHECK(storeIt != storeMap.end());
                auto store = storeIt->second;
                auto chunkSpec = New<TRefCountedChunkSpec>();
                chunkSpec->mutable_chunk_id()->CopyFrom(chunkInfo.chunk_id());
                chunkSpec->mutable_replicas()->MergeFrom(chunkInfo.replicas());
                chunkSpec->mutable_chunk_meta()->CopyFrom(store->GetChunkMeta());
                ToProto(chunkSpec->mutable_lower_limit(), TReadLimit(partition->GetPivotKey()));
                ToProto(chunkSpec->mutable_upper_limit(), TReadLimit(partition->GetNextPivotKey()));
                fetcher->AddChunk(chunkSpec);
            }
        }

        WaitFor(fetcher->Fetch())
            .ThrowOnError();

        std::vector<TOwningKey> samples;
        for (const auto& sample : fetcher->GetSamples()) {
            YCHECK(!sample.Incomplete);
            samples.push_back(sample.Key);
        }

        // NB(psushin): This filtering is typically redundant (except for the first pivot), 
        // since fetcher already returns samples within given limits.
        samples.erase(
            std::remove_if(
                samples.begin(),
                samples.end(),
                [&] (const TOwningKey& key) {
                    return key <= partition->GetPivotKey() || key >= partition->GetNextPivotKey();
                }),
            samples.end());

        std::sort(samples.begin(), samples.end());
        return samples;
    }


    static NLogging::TLogger BuildLogger(TPartition* partition)
    {
        auto logger = TabletNodeLogger;
        logger.AddTag("TabletId: %v, PartitionId: %v",
            partition->GetTablet()->GetTabletId(),
            partition->GetId());
        return logger;
    }
};

void StartPartitionBalancer(
    TPartitionBalancerConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
{
    New<TPartitionBalancer>(config, bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
