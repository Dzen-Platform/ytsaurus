#include "chunk_replica_cache.h"

#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/ytlib/cell_master_client/cell_directory.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/core/concurrency/periodic_executor.h>

namespace NYT::NChunkClient {

using namespace NConcurrency;
using namespace NObjectClient;
using namespace NNodeTrackerClient;
using namespace NApi;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

class TChunkReplicaCache
    : public IChunkReplicaCache
{
public:
    explicit TChunkReplicaCache(NApi::NNative::IConnectionPtr connection)
        : Connection_(connection)
        , Config_(connection->GetConfig()->ChunkReplicaCache)
        , NodeDirectory_(connection->GetNodeDirectory())
        , Logger(connection->GetLogger())
        , ExpirationExecutor_(New<TPeriodicExecutor>(
            connection->GetInvoker(),
            BIND(&TChunkReplicaCache::OnExpirationSweep, MakeWeak(this)),
            Config_->ExpirationTime))
    {
        ExpirationExecutor_->Start();
    }

    std::vector<TFuture<TAllyReplicasInfo>> GetReplicas(
        const std::vector<TChunkId>& chunkIds) override
    {
        std::vector<TFuture<TAllyReplicasInfo>> futures(chunkIds.size());
        std::vector<int> missingIndices;
        auto now = TInstant::Now();

        {
            auto mapGuard = ReaderGuard(EntriesLock_);
            for (int index = 0; index < std::ssize(chunkIds); ++index) {
                auto chunkId = chunkIds[index];
                auto it = Entries_.find(chunkId);
                if (it == Entries_.end()) {
                    missingIndices.push_back(index);
                } else {
                    auto& entry = *it->second;
                    auto entryGuard = Guard(entry.Lock);
                    entry.LastAccessTime = now;
                    futures[index] = entry.Promise.ToFuture();
                }
            }
        }

        std::vector<TPromise<TAllyReplicasInfo>> promises(chunkIds.size());
        THashMap<TCellTag, std::vector<int>> cellTagToStillMissingIndices;

        if (!missingIndices.empty()) {
            auto mapGuard = WriterGuard(EntriesLock_);
            for (int index = 0; index < std::ssize(chunkIds); ++index) {
                auto chunkId = chunkIds[index];
                auto it = Entries_.find(chunkId);
                if (it == Entries_.end()) {
                    cellTagToStillMissingIndices[CellTagFromId(chunkId)].push_back(index);
                    it = EmplaceOrCrash(Entries_, chunkId, std::make_unique<TEntry>());
                    auto& entry = *it->second;
                    entry.LastAccessTime = now;
                    entry.Promise = NewPromise<TAllyReplicasInfo>();
                    promises[index] = entry.Promise;
                }
                auto& entry = *it->second;
                auto entryGuard = Guard(entry.Lock);
                futures[index] = entry.Promise.ToFuture();
            }
        }

        if (!cellTagToStillMissingIndices.empty()) {
            auto connection = Connection_.Lock();
            if (!connection) {
                return futures;
            }

            for (auto& [cellTag, stillMissingIndicies] : cellTagToStillMissingIndices) {
                try {
                    auto channel = connection->GetMasterCellDirectory()->GetMasterChannelOrThrow(EMasterChannelKind::Follower, cellTag);

                    TChunkServiceProxy proxy(std::move(channel));
                    TChunkServiceProxy::TReqLocateChunksPtr currentReq;

                    std::vector<TChunkId> currentChunkIds;
                    std::vector<TPromise<TAllyReplicasInfo>> currentPromises;

                    auto flushCurrent = [&, cellTag = cellTag] {
                        if (!currentReq) {
                            return;
                        }

                        YT_LOG_DEBUG("Locating chunks (CellTag: %v, ChunkIds: %v)",
                            cellTag,
                            currentChunkIds);

                        currentReq->Invoke().Subscribe(
                            BIND(&TChunkReplicaCache::OnChunksLocated, MakeStrong(this), cellTag, std::move(currentChunkIds), std::move(currentPromises)));

                        currentReq.Reset();
                    };

                    for (auto index : stillMissingIndicies) {
                        auto chunkId = chunkIds[index];
                        currentChunkIds.push_back(chunkId);
                        currentPromises.push_back(promises[index]);

                        if (!currentReq) {
                            currentReq = proxy.LocateChunks();
                            currentReq->SetResponseHeavy(true);
                        }

                        ToProto(currentReq->add_subrequests(), chunkId);

                        if (std::ssize(currentChunkIds) >= Config_->MaxChunksPerLocate) {
                            flushCurrent();
                        }
                    }

                    flushCurrent();
                } catch (const std::exception& ex) {
                    // NB: GetMasterChannelOrThrow above may throw.

                    auto error = TError(ex);
                    for (auto index : stillMissingIndicies) {
                        promises[index].Set(error);
                    }

                    // Errors must not be sticky; evict promises.
                    auto mapGuard = WriterGuard(EntriesLock_);
                    for (auto index : stillMissingIndicies) {
                        auto chunkId = chunkIds[index];
                        if (auto it = Entries_.find(chunkId); it != Entries_.end()) {
                            auto& entry = *it->second;
                            auto entryGuard = Guard(entry.Lock);
                            if (entry.Promise == promises[index]) {
                                entryGuard.Release();
                                Entries_.erase(it);
                            }
                        }
                    }
                }
            }
        }

        return futures;
    }

    void DiscardReplicas(
        TChunkId chunkId,
        const TFuture<TAllyReplicasInfo>& future) override
    {
        auto now = TInstant::Now();
        auto mapGuard = WriterGuard(EntriesLock_);
        auto it = Entries_.find(chunkId);
        if (it != Entries_.end()) {
            auto& entry = *it->second;
            auto entryGuard = Guard(entry.Lock);
            entry.LastAccessTime = now;
            if (entry.Promise.ToFuture() == future) {
                entryGuard.Release();
                Entries_.erase(it);
                YT_LOG_DEBUG("Chunk replicas discarded (ChunkId: %v)",
                    chunkId);
            }
        }
    }

    void UpdateReplicas(
        TChunkId chunkId,
        const TAllyReplicasInfo& replicas) override
    {
        auto now = TInstant::Now();

        auto update = [&] (TEntry& entry) {
            entry.Promise = MakePromise(replicas);
            entry.LastAccessTime = now;

            YT_LOG_DEBUG("Chunk replicas updated (ChunkId: %v, Replicas: %v, Revision: %llx)",
                chunkId,
                MakeFormattableView(replicas.Replicas, TChunkReplicaAddressFormatter(NodeDirectory_)),
                replicas.Revision);
        };

        auto tryUpdate = [&] (TEntry& entry) {
            auto entryGuard = Guard(entry.Lock);

            auto oldRevision = NHydra::NullRevision;
            if (auto optionalExistingReplicas = entry.Promise.TryGet()) {
                if (optionalExistingReplicas->IsOK()) {
                    oldRevision = optionalExistingReplicas->Value().Revision;
                }
            }

            if (oldRevision >= replicas.Revision) {
                return false;
            }

            update(entry);

            return true;
        };

        {
            auto mapGuard = ReaderGuard(EntriesLock_);
            auto it = Entries_.find(chunkId);
            if (it != Entries_.end() && tryUpdate(*it->second)) {
                return;
            }
        }

        {
            auto mapGuard = WriterGuard(EntriesLock_);
            auto it = Entries_.find(chunkId);
            if (it == Entries_.end()) {
                it = EmplaceOrCrash(Entries_, chunkId, std::make_unique<TEntry>());
                update(*it->second);
            } else {
                tryUpdate(*it->second);
            }
        }
    }

    void RegisterReplicas(
        TChunkId chunkId,
        const TChunkReplicaWithMediumList& replicas) override
    {
        UpdateReplicas(
            chunkId,
            TAllyReplicasInfo{
                .Replicas = replicas,
                .Revision = 1 // must be larger than NullRevision
            });
    }

private:
    const TWeakPtr<NApi::NNative::IConnection> Connection_;
    const TChunkReplicaCacheConfigPtr Config_;
    const TNodeDirectoryPtr NodeDirectory_;
    const NLogging::TLogger Logger;

    const TPeriodicExecutorPtr ExpirationExecutor_;

    struct TEntry
    {
        YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, Lock);
        TInstant LastAccessTime;
        TPromise<TAllyReplicasInfo> Promise;
    };

    // TODO(babenko): maybe implement sharding
    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, EntriesLock_);
    THashMap<TChunkId, std::unique_ptr<TEntry>> Entries_;


    void OnChunksLocated(
        TCellTag cellTag,
        const std::vector<TChunkId>& chunkIds,
        const std::vector<TPromise<TAllyReplicasInfo>>& promises,
        const TChunkServiceProxy::TErrorOrRspLocateChunksPtr& rspOrError)
    {
        auto connection = Connection_.Lock();
        if (!connection) {
            return;
        }

        if (rspOrError.IsOK()) {
            YT_LOG_WARNING("Chunks located (CellTag: %v, ChunkCount: %v)",
                cellTag,
                std::ssize(promises));

            const auto& rsp = rspOrError.Value();

            NodeDirectory_->MergeFrom(rsp->node_directory());

            for (int index = 0; index < std::ssize(chunkIds); ++index) {
                const auto& subresponse = rsp->subresponses(index);
                auto replicas = TAllyReplicasInfo::FromChunkReplicas(
                    FromProto<TChunkReplicaList>(subresponse.replicas()),
                    rsp->revision());
                promises[index].TrySet(std::move(replicas));
            }
        } else {
            YT_LOG_WARNING(rspOrError, "Error locating chunks (CellTag: %v)",
                cellTag);

            {
                auto mapGuard = ReaderGuard(EntriesLock_);
                for (int index = 0; index < std::ssize(chunkIds); ++index) {
                    auto chunkId = chunkIds[index];
                    auto it = Entries_.find(chunkId);
                    if (it == Entries_.end()) {
                        continue;
                    }
                    Entries_.erase(it);
                }
            }

            auto error = TError(rspOrError);
            for (const auto& promise : promises) {
                promise.TrySet(error);
            }
        }
    }

    void OnExpirationSweep()
    {
        YT_LOG_DEBUG("Started expired chunk replica sweep");

        std::vector<TChunkId> expiredChunkIds;
        auto deadline = TInstant::Now() - Config_->ExpirationTime;
        int totalChunkCount;

        {
            auto mapGuard = ReaderGuard(EntriesLock_);
            totalChunkCount = std::ssize(Entries_);
            for (const auto& [chunkId, entry] : Entries_) {
                auto entryGuard = Guard(entry->Lock);
                if (entry->LastAccessTime < deadline) {
                    expiredChunkIds.push_back(chunkId);
                }
            }
        }

        if (!expiredChunkIds.empty()) {
            auto mapGuard = WriterGuard(EntriesLock_);
            for (auto chunkId : expiredChunkIds) {
                Entries_.erase(chunkId);
            }
        }

        YT_LOG_DEBUG("Finished expired chunk replica sweep (TotalChunkCount: %v, ExpiredChunkCount: %v)",
            totalChunkCount,
            expiredChunkIds.size());
    }
};

////////////////////////////////////////////////////////////////////////////////

IChunkReplicaCachePtr CreateChunkReplicaCache(NApi::NNative::IConnectionPtr connection)
{
    return New<TChunkReplicaCache>(std::move(connection));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
