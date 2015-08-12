#include "stdafx.h"

#include "samples_fetcher.h"

#include <ytlib/chunk_client/chunk_spec.h>
#include <ytlib/chunk_client/config.h>
#include <ytlib/chunk_client/data_node_service_proxy.h>
#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/private.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/scheduler/config.h>

#include <core/concurrency/scheduler.h>

#include <core/misc/protobuf_helpers.h>

#include <core/rpc/channel.h>

#include <core/logging/log.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NRpc;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////

TSamplesFetcher::TSamplesFetcher(
    TFetcherConfigPtr config,
    i64 desiredSampleCount,
    const TKeyColumns& keyColumns,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    IInvokerPtr invoker,
    const NLogging::TLogger& logger)
    : TFetcherBase(config, nodeDirectory, invoker, logger)
    , KeyColumns_(keyColumns)
    , DesiredSampleCount_(desiredSampleCount)
{
    YCHECK(DesiredSampleCount_ > 0);
}

void TSamplesFetcher::AddChunk(TRefCountedChunkSpecPtr chunk)
{
    i64 chunkDataSize;
    GetStatistics(*chunk, &chunkDataSize);
    TotalDataSize_ += chunkDataSize;

    TFetcherBase::AddChunk(chunk);
}

TFuture<void> TSamplesFetcher::Fetch()
{
    LOG_DEBUG("Started fetching chunk samples (ChunkCount: %v, DesiredSampleCount: %v)",
        Chunks_.size(),
        DesiredSampleCount_);

    if (TotalDataSize_ < DesiredSampleCount_) {
        SizeBetweenSamples_ = 1;
    } else {
        SizeBetweenSamples_ = TotalDataSize_ / DesiredSampleCount_;
    }

    return TFetcherBase::Fetch();
}

const std::vector<TOwningKey>& TSamplesFetcher::GetSamples() const
{
    return Samples_;
}

TFuture<void> TSamplesFetcher::FetchFromNode(TNodeId nodeId, std::vector<int> chunkIndexes)
{
    return BIND(&TSamplesFetcher::DoFetchFromNode, MakeWeak(this), nodeId, Passed(std::move(chunkIndexes)))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();
}

void TSamplesFetcher::DoFetchFromNode(TNodeId nodeId, std::vector<int> chunkIndexes)
{
    TDataNodeServiceProxy proxy(GetNodeChannel(nodeId));
    proxy.SetDefaultTimeout(Config_->NodeRpcTimeout);

    auto req = proxy.GetTableSamples();
    NYT::ToProto(req->mutable_key_columns(), KeyColumns_);

    i64 currentSize = SizeBetweenSamples_;
    i64 currentSampleCount = 0;

    std::vector<int> requestedChunkIndexes;

    for (auto index : chunkIndexes) {
        const auto& chunk = Chunks_[index];

        i64 chunkDataSize;
        GetStatistics(*chunk, &chunkDataSize);

        currentSize += chunkDataSize;
        i64 sampleCount = currentSize / SizeBetweenSamples_;

        if (sampleCount > currentSampleCount) {
            requestedChunkIndexes.push_back(index);
            auto chunkId = EncodeChunkId(*chunk, nodeId);

            auto* sampleRequest = req->add_sample_requests();
            ToProto(sampleRequest->mutable_chunk_id(), chunkId);
            sampleRequest->set_sample_count(sampleCount - currentSampleCount);
            if (chunk->has_lower_limit() && chunk->lower_limit().has_key()) {
                sampleRequest->set_lower_key(chunk->lower_limit().key());
            }
            if (chunk->has_upper_limit() && chunk->upper_limit().has_key()) {
                sampleRequest->set_upper_key(chunk->upper_limit().key());
            }
            currentSampleCount = sampleCount;
        }
    }

    if (req->sample_requests_size() == 0)
        return;

    auto rspOrError = WaitFor(req->Invoke());

    if (!rspOrError.IsOK()) {
        LOG_WARNING("Failed to get samples from node (Address: %v, NodeId: %v)",
            NodeDirectory_->GetDescriptor(nodeId).GetDefaultAddress(),
            nodeId);
        OnNodeFailed(nodeId, requestedChunkIndexes);
        return;
    }

    const auto& rsp = rspOrError.Value();
    for (int index = 0; index < requestedChunkIndexes.size(); ++index) {
        const auto& sampleResponse = rsp->sample_responses(index);

        if (sampleResponse.has_error()) {
            auto error = FromProto<TError>(sampleResponse.error());
            OnChunkFailed(nodeId, requestedChunkIndexes[index], error);
            continue;
        }

        LOG_TRACE("Received %v samples for chunk #%v",
            sampleResponse.keys_size(),
            requestedChunkIndexes[index]);

        for (const auto& sample : sampleResponse.keys()) {
            Samples_.push_back(FromProto<TOwningKey>(sample));
        }
    }
}

////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

