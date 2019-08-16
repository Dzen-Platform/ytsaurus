#include "chunk_spec_fetcher.h"

#include <yt/ytlib/api/native/client.h>

#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/client/chunk_client/public.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/misc/protobuf_helpers.h>

namespace NYT {
namespace NChunkClient {

using namespace NApi;
using namespace NApi::NNative;
using namespace NNodeTrackerClient;
using namespace NLogging;
using namespace NYPath;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NConcurrency;

using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

TChunkSpecFetcher::TChunkSpecFetcher(
    const NApi::NNative::IClientPtr& client,
    const TNodeDirectoryPtr& nodeDirectory,
    const IInvokerPtr& invoker,
    int maxChunksPerFetch,
    int maxChunksPerLocateRequest,
    const std::function<void(const TChunkOwnerYPathProxy::TReqFetchPtr&)>& initializeFetchRequest,
    const TLogger& logger,
    bool skipUnavailableChunks)
    : Client_(client)
    , NodeDirectory_(nodeDirectory)
    , Invoker_(invoker)
    , MaxChunksPerFetch_(maxChunksPerFetch)
    , MaxChunksPerLocateRequest_(maxChunksPerLocateRequest)
    , InitializeFetchRequest_(initializeFetchRequest)
    , Logger(logger)
    , SkipUnavailableChunks_(skipUnavailableChunks)
{ }

void TChunkSpecFetcher::Add(
    TObjectId objectId,
    TCellTag externalCellTag,
    i64 chunkCount,
    const std::vector<TReadRange>& ranges)
{
    auto& state = GetCellState(externalCellTag);

    auto oldReqCount = state.ReqCount;

    for (int rangeIndex = 0; rangeIndex < static_cast<int>(ranges.size()); ++rangeIndex) {
        for (i64 index = 0; index < (chunkCount + MaxChunksPerFetch_ - 1) / MaxChunksPerFetch_; ++index) {
            auto adjustedRange = ranges[rangeIndex];
            auto chunkCountLowerLimit = index * MaxChunksPerFetch_;
            if (adjustedRange.LowerLimit().HasChunkIndex()) {
                chunkCountLowerLimit = std::max(chunkCountLowerLimit, adjustedRange.LowerLimit().GetChunkIndex());
            }
            adjustedRange.LowerLimit().SetChunkIndex(chunkCountLowerLimit);

            auto chunkCountUpperLimit = (index + 1) * MaxChunksPerFetch_;
            if (adjustedRange.UpperLimit().HasChunkIndex()) {
                chunkCountUpperLimit = std::min(chunkCountUpperLimit, adjustedRange.UpperLimit().GetChunkIndex());
            }
            adjustedRange.UpperLimit().SetChunkIndex(chunkCountUpperLimit);

            auto req = TChunkOwnerYPathProxy::Fetch(FromObjectId(objectId));
            AddCellTagToSyncWith(req, objectId);
            InitializeFetchRequest_(req.Get());
            ToProto(req->mutable_ranges(), std::vector<NChunkClient::TReadRange>{adjustedRange});
            state.BatchReq->AddRequest(req, "fetch");
            ++state.ReqCount;
            state.RangeIndices.push_back(rangeIndex);
            state.TableIndices.push_back(TableIndex_);
        }
    }

    YT_LOG_DEBUG("Table added for chunk spec fetching (ObjectId: %v, ExternalCellTag: %v, ChunkCount: %v, RangeCount: %v, "
        "TableIndex: %v, ReqCount: %v)",
        objectId,
        externalCellTag,
        chunkCount,
        ranges.size(),
        TableIndex_,
        state.ReqCount - oldReqCount);

    ++TableIndex_;
}

TChunkSpecFetcher::TCellState& TChunkSpecFetcher::GetCellState(TCellTag cellTag)
{
    auto it = CellTagToState_.find(cellTag);
    if (it == CellTagToState_.end()) {
        it = CellTagToState_.insert({cellTag, TCellState()}).first;
        auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Follower, cellTag);
        TObjectServiceProxy proxy(channel);
        it->second.BatchReq = proxy.ExecuteBatch();
    }
    return it->second;
}

TFuture<void> TChunkSpecFetcher::Fetch()
{
    return BIND(&TChunkSpecFetcher::DoFetch, MakeWeak(this))
        .AsyncVia(Invoker_)
        .Run();
}

void TChunkSpecFetcher::DoFetch()
{
    YT_LOG_INFO("Fetching chunk specs (CellCount: %v)", CellTagToState_.size());
    std::vector<TFuture<void>> asyncResults;
    for (auto& [cellTag, cellState] : CellTagToState_) {
        asyncResults.emplace_back(BIND(&TChunkSpecFetcher::DoFetchFromCell, MakeWeak(this), cellTag)
            .AsyncVia(Invoker_)
            .Run());
    }
    WaitFor(Combine(asyncResults))
        .ThrowOnError();
    YT_LOG_INFO("Finished processing chunk specs");

    if (!ForeignChunkSpecs_.empty()) {
        YT_LOG_INFO("Locating foreign chunks (ForeignChunkCount: %v)", ForeignChunkSpecs_.size());
        LocateChunks(Client_, MaxChunksPerLocateRequest_, ForeignChunkSpecs_, NodeDirectory_, Logger, SkipUnavailableChunks_);
        YT_LOG_INFO("Finished locating foreign chunks");
    }

    for (auto& [cellTag, cellState] : CellTagToState_) {
        for (auto& chunkSpec : cellState.ChunkSpecs) {
            ChunkSpecs_.emplace_back().Swap(&chunkSpec);
        }
    }

    YT_LOG_INFO("Chunks fetched (ChunkCount: %v)", ChunkSpecs_.size());
}

void TChunkSpecFetcher::DoFetchFromCell(TCellTag cellTag)
{
    auto& cellState = CellTagToState_[cellTag];

    YT_LOG_DEBUG("Fetching chunk specs from cell (CellTag: %v, FetchRequestCount: %v)", cellTag, cellState.ReqCount);

    auto batchRspOrError = WaitFor(cellState.BatchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        GetCumulativeError(batchRspOrError),
        "Error fetching chunk specs from cell %v",
        cellTag);

    const auto& batchRsp = batchRspOrError.Value();
    auto rspsOrError = batchRsp->GetResponses<TChunkOwnerYPathProxy::TRspFetch>("fetch");

    std::vector<NProto::TChunkSpec*> foreignChunkSpecs;

    for (int resultIndex = 0; resultIndex < static_cast<int>(rspsOrError.size()); ++resultIndex) {
        auto& rsp = rspsOrError[resultIndex].Value();
        for (auto& chunkSpec : *rsp->mutable_chunks()) {
            chunkSpec.set_table_index(cellState.TableIndices[resultIndex]);
            chunkSpec.set_range_index(cellState.RangeIndices[resultIndex]);
            cellState.ChunkSpecs.emplace_back().Swap(&chunkSpec);
        }
        if (NodeDirectory_) {
            NodeDirectory_->MergeFrom(rsp->node_directory());
        }
    }

    for (auto& chunkSpec : cellState.ChunkSpecs) {
        auto chunkId = NYT::FromProto<TChunkId>(chunkSpec.chunk_id());
        auto chunkCellTag = CellTagFromId(chunkId);
        if (chunkCellTag != cellTag) {
            ForeignChunkSpecs_.push_back(&chunkSpec);
        }
    }
    YT_LOG_DEBUG("Finished processing cell chunk spec fetch results (CellTag: %v, FetchedChunkCount: %v, ForeignChunkCount: %v)",
        cellTag,
        cellState.ChunkSpecs.size(),
        foreignChunkSpecs.size());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
