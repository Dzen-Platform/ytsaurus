#include "fetcher.h"
#include "private.h"
#include "chunk_replica.h"
#include "input_chunk.h"
#include "config.h"

#include <yt/ytlib/api/native_client.h>

#include <yt/ytlib/chunk_client/chunk_scraper.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>
#include <yt/ytlib/node_tracker_client/channel.h>

#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/string.h>

#include <yt/core/rpc/retrying_channel.h>

namespace NYT {
namespace NChunkClient {

using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NRpc;
using namespace NObjectClient;
using namespace NApi;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

DEFINE_REFCOUNTED_TYPE(IFetcherChunkScraper)

////////////////////////////////////////////////////////////////////////////////

class TFetcherChunkScraper
    : public IFetcherChunkScraper
{
public:
    TFetcherChunkScraper(
        const TChunkScraperConfigPtr config,
        const IInvokerPtr invoker,
        TThrottlerManagerPtr throttlerManager,
        INativeClientPtr client,
        TNodeDirectoryPtr nodeDirectory,
        const NLogging::TLogger& logger)
        : Config_(config)
        , Invoker_(invoker)
        , ThrottlerManager_(throttlerManager)
        , Client_(client)
        , NodeDirectory_(nodeDirectory)
        , Logger(logger)
    { }

    virtual TFuture<void> ScrapeChunks(const yhash_set<TInputChunkPtr>& chunkSpecs) override
    {
        return BIND(&TFetcherChunkScraper::DoScrapeChunks, MakeStrong(this))
            .AsyncVia(Invoker_)
            .Run(chunkSpecs);
    }

    virtual i64 GetUnavailableChunkCount() const override
    {
        return UnavailableFetcherChunkCount_;
    }

private:
    struct TFetcherChunkDescriptor
    {
        SmallVector<NChunkClient::TInputChunkPtr, 1> ChunkSpecs;
        bool IsWaiting = true;
    };

    const TChunkScraperConfigPtr Config_;
    const IInvokerPtr Invoker_;
    const TThrottlerManagerPtr ThrottlerManager_;
    const INativeClientPtr Client_;
    const TNodeDirectoryPtr NodeDirectory_;
    const NLogging::TLogger Logger;

    TChunkScraperPtr Scraper_;

    yhash<TChunkId, TFetcherChunkDescriptor> ChunkMap_;
    int UnavailableFetcherChunkCount_ = 0;
    TPromise<void> BatchLocatedPromise_ = NewPromise<void>();

    int ChunkLocatedCallCount_ = 0;

    TFuture<void> DoScrapeChunks(const yhash_set<TInputChunkPtr>& chunkSpecs)
    {
        yhash_set<TChunkId> chunkIds;
        ChunkMap_.clear();
        for (const auto& chunkSpec : chunkSpecs) {
            const auto& chunkId = chunkSpec->ChunkId();
            chunkIds.insert(chunkId);
            ChunkMap_[chunkId].ChunkSpecs.push_back(chunkSpec);
        }
        UnavailableFetcherChunkCount_ = chunkIds.size();

        Scraper_ = New<TChunkScraper>(
            Config_,
            Invoker_,
            ThrottlerManager_,
            Client_,
            NodeDirectory_,
            std::move(chunkIds),
            BIND(&TFetcherChunkScraper::OnChunkLocated, MakeWeak(this)),
            Logger);
        Scraper_->Start();

        BatchLocatedPromise_ = NewPromise<void>();
        return BatchLocatedPromise_;
    }

    void OnChunkLocated(const TChunkId& chunkId, const TChunkReplicaList& replicas)
    {
        ++ChunkLocatedCallCount_;
        if (ChunkLocatedCallCount_ >= Config_->MaxChunksPerRequest) {
            ChunkLocatedCallCount_ = 0;
            LOG_DEBUG("Located another batch of chunks (Count: %v, UnavailableFetcherChunkCount: %v)",
                Config_->MaxChunksPerRequest,
                UnavailableFetcherChunkCount_);
        }

        LOG_TRACE("Fetcher chunk is located (ChunkId: %v, Replicas: %v)",
            chunkId,
            replicas);

        if (replicas.empty()) {
            return;
        }

        auto it = ChunkMap_.find(chunkId);
        YCHECK(it != ChunkMap_.end());

        auto& description = it->second;
        YCHECK(!description.ChunkSpecs.empty());

        if (!description.IsWaiting)
            return;

        description.IsWaiting = false;

        LOG_TRACE("Fetcher chunk is available (ChunkId: %v, Replicas: %v)",
            chunkId,
            replicas);

        // Update replicas in place for all input chunks with current chunkId.
        for (auto& chunkSpec : description.ChunkSpecs) {
            chunkSpec->SetReplicaList(replicas);
        }

        --UnavailableFetcherChunkCount_;
        YCHECK(UnavailableFetcherChunkCount_ >= 0);

        if (UnavailableFetcherChunkCount_ == 0) {
            // Wait for all scraper callbacks to finish before session completion.
            BatchLocatedPromise_.SetFrom(Scraper_->Stop());
        }
    }
};

DEFINE_REFCOUNTED_TYPE(TFetcherChunkScraper)

////////////////////////////////////////////////////////////////////////////////

IFetcherChunkScraperPtr CreateFetcherChunkScraper(
    const TChunkScraperConfigPtr config,
    const IInvokerPtr invoker,
    TThrottlerManagerPtr throttlerManager,
    INativeClientPtr client,
    TNodeDirectoryPtr nodeDirectory,
    const NLogging::TLogger& logger)
{
    return New<TFetcherChunkScraper>(
        config,
        invoker,
        throttlerManager,
        client,
        nodeDirectory,
        logger);
}

////////////////////////////////////////////////////////////////////////////////

TFetcherBase::TFetcherBase(
    TFetcherConfigPtr config,
    TNodeDirectoryPtr nodeDirectory,
    IInvokerPtr invoker,
    TRowBufferPtr rowBuffer,
    IFetcherChunkScraperPtr chunkScraper,
    INativeClientPtr client,
    const NLogging::TLogger& logger)
    : Config_(std::move(config))
    , NodeDirectory_(std::move(nodeDirectory))
    , Invoker_(invoker)
    , RowBuffer_(std::move(rowBuffer))
    , ChunkScraper_(std::move(chunkScraper))
    , Logger(logger)
    , Client_(std::move(client))
{ }

void TFetcherBase::AddChunk(TInputChunkPtr chunk)
{
    YCHECK(UnfetchedChunkIndexes_.insert(static_cast<int>(Chunks_.size())).second);
    Chunks_.push_back(chunk);
}

TFuture<void> TFetcherBase::Fetch()
{
    BIND(&TFetcherBase::StartFetchingRound, MakeWeak(this))
        .Via(Invoker_)
        .Run();
    return Promise_;
}

void TFetcherBase::StartFetchingRound()
{
    LOG_DEBUG("Start fetching round (UnfetchedChunkCount: %v, DeadNodes: %v, DeadChunks: %v)",
        UnfetchedChunkIndexes_.size(),
        DeadNodes_.size(),
        DeadChunks_.size());

    // Construct address -> chunk* map.
    typedef yhash<TNodeId, std::vector<int> > TNodeIdToChunkIndexes;
    TNodeIdToChunkIndexes nodeIdToChunkIndexes;
    yhash_set<TInputChunkPtr> unavailableChunks;

    for (auto chunkIndex : UnfetchedChunkIndexes_) {
        const auto& chunk = Chunks_[chunkIndex];
        const auto& chunkId = chunk->ChunkId();
        bool chunkAvailable = false;
        const auto replicas = chunk->GetReplicaList();
        for (auto replica : replicas) {
            auto nodeId = replica.GetNodeId();
            if (DeadNodes_.find(nodeId) == DeadNodes_.end() &&
                DeadChunks_.find(std::make_pair(nodeId, chunkId)) == DeadChunks_.end())
            {
                nodeIdToChunkIndexes[nodeId].push_back(chunkIndex);
                chunkAvailable = true;
            }
        }
        if (!chunkAvailable) {
            if (ChunkScraper_) {
                unavailableChunks.insert(chunk);
            } else {
                Promise_.Set(TError(
                    "Unable to fetch info for chunk %v from any of nodes %v",
                    chunkId,
                    MakeFormattableRange(replicas, TChunkReplicaAddressFormatter(NodeDirectory_))));
                return;
            }
        }
    }

    if (!unavailableChunks.empty() && ChunkScraper_) {
        LOG_DEBUG("Found unavailable chunks, starting scraper (UnavailableChunkCount: %v)",
            unavailableChunks.size());
        auto error = WaitFor(ChunkScraper_->ScrapeChunks(std::move(unavailableChunks)));
        LOG_DEBUG("All unavailable chunks are located");
        DeadNodes_.clear();
        DeadChunks_.clear();
        BIND(&TFetcherBase::OnFetchingRoundCompleted, MakeWeak(this))
            .Via(Invoker_)
            .Run(error);
        return;
    }

    UnfetchedChunkIndexes_.clear();

    // Sort nodes by number of chunks (in decreasing order).
    std::vector<TNodeIdToChunkIndexes::iterator> nodeIts;
    for (auto it = nodeIdToChunkIndexes.begin(); it != nodeIdToChunkIndexes.end(); ++it) {
        nodeIts.push_back(it);
    }
    std::sort(
        nodeIts.begin(),
        nodeIts.end(),
        [=] (const TNodeIdToChunkIndexes::iterator& lhs, const TNodeIdToChunkIndexes::iterator& rhs) {
            return lhs->second.size() > rhs->second.size();
        });

    // Pick nodes greedily.
    std::vector<TFuture<void>> asyncResults;
    yhash_set<int> requestedChunkIndexes;
    for (const auto& it : nodeIts) {
        std::vector<int> chunkIndexes;
        for (int chunkIndex : it->second) {
            if (requestedChunkIndexes.find(chunkIndex) == requestedChunkIndexes.end()) {
                YCHECK(requestedChunkIndexes.insert(chunkIndex).second);
                chunkIndexes.push_back(chunkIndex);
            }
        }

        // Send the request, if not empty.
        if (!chunkIndexes.empty()) {
            asyncResults.push_back(FetchFromNode(it->first, std::move(chunkIndexes)));
        }
    }

    Combine(asyncResults).Subscribe(
        BIND(&TFetcherBase::OnFetchingRoundCompleted, MakeWeak(this))
            .Via(Invoker_));
}

IChannelPtr TFetcherBase::GetNodeChannel(TNodeId nodeId)
{
    const auto& descriptor = NodeDirectory_->GetDescriptor(nodeId);
    auto channel = Client_->GetChannelFactory()->CreateChannel(descriptor);
    return CreateRetryingChannel(Config_->NodeChannel, channel);
}

void TFetcherBase::OnChunkFailed(TNodeId nodeId, int chunkIndex, const TError& error)
{
    const auto& chunk = Chunks_[chunkIndex];
    const auto& chunkId = chunk->ChunkId();

    LOG_DEBUG(error, "Error fetching chunk info (ChunkId: %v, Address: %v)",
        chunkId,
        NodeDirectory_->GetDescriptor(nodeId).GetDefaultAddress());

    DeadChunks_.insert(std::make_pair(nodeId, chunkId));
    YCHECK(UnfetchedChunkIndexes_.insert(chunkIndex).second);
}

void TFetcherBase::OnNodeFailed(TNodeId nodeId, const std::vector<int>& chunkIndexes)
{
    LOG_DEBUG("Error fetching chunks from node (Address: %v, ChunkCount: %v)",
        NodeDirectory_->GetDescriptor(nodeId).GetDefaultAddress(),
        chunkIndexes.size());

    DeadNodes_.insert(nodeId);
    UnfetchedChunkIndexes_.insert(chunkIndexes.begin(), chunkIndexes.end());
}

void TFetcherBase::OnFetchingRoundCompleted(const TError& error)
{
    if (!error.IsOK()) {
        LOG_ERROR(error, "Fetching failed");
        Promise_.Set(error);
        return;
    }

    if (UnfetchedChunkIndexes_.empty()) {
        LOG_DEBUG("Fetching complete");
        OnFetchingCompleted();
        Promise_.Set(TError());
        return;
    }

    StartFetchingRound();
}

void TFetcherBase::OnFetchingCompleted()
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
