#include "chunk_slices_fetcher.h"
#include "row_buffer.h"
#include "private.h"

#include <yt/ytlib/chunk_client/chunk_replica.h>
#include <yt/ytlib/chunk_client/config.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/input_chunk.h>
#include <yt/ytlib/chunk_client/input_chunk_slice.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/rpc/channel.h>

namespace NYT {
namespace NTableClient {

using namespace NConcurrency;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NRpc;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////

TChunkSliceFetcher::TChunkSliceFetcher(
    TFetcherConfigPtr config,
    i64 chunkSliceSize,
    const TKeyColumns& keyColumns,
    bool sliceByKeys,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    IInvokerPtr invoker,
    TScrapeChunksCallback scraperCallback,
    NApi::INativeClientPtr client,
    TRowBufferPtr rowBuffer,
    const NLogging::TLogger& logger)
    : TFetcherBase(
        config,
        nodeDirectory,
        invoker,
        rowBuffer,
        scraperCallback,
        client,
        logger)
    , ChunkSliceSize_(chunkSliceSize)
    , KeyColumns_(keyColumns)
    , SliceByKeys_(sliceByKeys)
{
    YCHECK(ChunkSliceSize_ > 0);
}

TFuture<void> TChunkSliceFetcher::Fetch()
{
    LOG_DEBUG("Started fetching chunk slices (ChunkCount: %v)",
        Chunks_.size());
    return TFetcherBase::Fetch();
}

std::vector<TInputChunkSlicePtr> TChunkSliceFetcher::GetChunkSlices()
{
    std::vector<NChunkClient::TInputChunkSlicePtr> chunkSlices;
    chunkSlices.reserve(SliceCount_);
    for (const auto& slices : SlicesByChunkIndex_) {
        chunkSlices.insert(chunkSlices.end(), slices.begin(), slices.end());
    }
    return chunkSlices;
}

TFuture<void> TChunkSliceFetcher::FetchFromNode(TNodeId nodeId, std::vector<int> chunkIndexes)
{
    return BIND(&TChunkSliceFetcher::DoFetchFromNode, MakeStrong(this), nodeId, Passed(std::move(chunkIndexes)))
        .AsyncVia(Invoker_)
        .Run();
}

TFuture<void> TChunkSliceFetcher::DoFetchFromNode(TNodeId nodeId, const std::vector<int>& chunkIndexes)
{
    TDataNodeServiceProxy proxy(GetNodeChannel(nodeId));
    proxy.SetDefaultTimeout(Config_->NodeRpcTimeout);

    auto req = proxy.GetChunkSlices();
    req->SetHeavy(true);
    req->set_slice_data_size(ChunkSliceSize_);
    req->set_slice_by_keys(SliceByKeys_);
    ToProto(req->mutable_key_columns(), KeyColumns_);
    // TODO(babenko): make configurable
    ToProto(req->mutable_workload_descriptor(), TWorkloadDescriptor(EWorkloadCategory::UserBatch));

    std::vector<int> requestedChunkIndexes;
    int keyColumnCount = KeyColumns_.size();

    for (auto index : chunkIndexes) {
        const auto& chunk = Chunks_[index];

        auto chunkDataSize = chunk->GetUncompressedDataSize();

        if (!chunk->BoundaryKeys()) {
            THROW_ERROR_EXCEPTION("Missing boundary keys in chunk %v", chunk->ChunkId());
        }
        const auto& minKey = chunk->BoundaryKeys()->MinKey;
        const auto& maxKey = chunk->BoundaryKeys()->MaxKey;

        if (chunkDataSize < ChunkSliceSize_ ||
            (SliceByKeys_ && CompareRows(minKey, maxKey, keyColumnCount) == 0))
        {
            auto slice = CreateInputChunkSlice(
                chunk,
                GetStrictKey(minKey, keyColumnCount, RowBuffer_),
                GetStrictKeySuccessor(maxKey, keyColumnCount, RowBuffer_));
            if (SlicesByChunkIndex_.size() <= index) {
                SlicesByChunkIndex_.resize(index + 1, std::vector<NChunkClient::TInputChunkSlicePtr>());
            }
            SlicesByChunkIndex_[index].push_back(slice);
            SliceCount_++;
        } else {
            requestedChunkIndexes.push_back(index);
            auto chunkId = EncodeChunkId(chunk, nodeId);

            auto* sliceRequest = req->add_slice_requests();
            ToProto(sliceRequest->mutable_chunk_id(), chunkId);
            if (chunk->LowerLimit()) {
                ToProto(sliceRequest->mutable_lower_limit(), *chunk->LowerLimit());
            }
            if (chunk->UpperLimit()) {
                ToProto(sliceRequest->mutable_upper_limit(), *chunk->UpperLimit());
            }
            sliceRequest->set_erasure_codec(static_cast<int>(chunk->GetErasureCodec()));
        }
    }

    if (req->slice_requests_size() == 0) {
        return VoidFuture;
    }

    return req->Invoke().Apply(
        BIND(&TChunkSliceFetcher::OnResponse, MakeStrong(this), nodeId, Passed(std::move(requestedChunkIndexes)))
            .AsyncVia(Invoker_));
}

void TChunkSliceFetcher::OnResponse(
    TNodeId nodeId,
    const std::vector<int>& requestedChunkIndexes,
    const TDataNodeServiceProxy::TErrorOrRspGetChunkSlicesPtr& rspOrError)
{
    if (!rspOrError.IsOK()) {
        LOG_WARNING("Failed to get chunk slices from node (Address: %v, NodeId: %v)",
            NodeDirectory_->GetDescriptor(nodeId).GetDefaultAddress(),
            nodeId);

        OnNodeFailed(nodeId, requestedChunkIndexes);

        if (rspOrError.FindMatching(EErrorCode::IncomparableType)) {
            // Any exception thrown here interrupts fetching.
            rspOrError.ThrowOnError();
        }
        return;
    }

    const auto& rsp = rspOrError.Value();
    for (int i = 0; i < requestedChunkIndexes.size(); ++i) {
        int index = requestedChunkIndexes[i];
        const auto& chunk = Chunks_[index];
        const auto& slices = rsp->slices(i);

        if (slices.has_error()) {
            auto error = FromProto<TError>(slices.error());
            OnChunkFailed(nodeId, index, error);
            continue;
        }

        LOG_TRACE("Received %v chunk slices for chunk #%v",
            slices.chunk_slices_size(),
            index);

        if (SlicesByChunkIndex_.size() <= index) {
            SlicesByChunkIndex_.resize(index + 1, std::vector<NChunkClient::TInputChunkSlicePtr>());
        }
        for (const auto& protoChunkSlice : slices.chunk_slices()) {
            auto slice = CreateInputChunkSlice(chunk, RowBuffer_, protoChunkSlice);
            SlicesByChunkIndex_[index].push_back(slice);
            SliceCount_++;
        }
    }
}

////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
