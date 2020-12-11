#include "chunk_slice_fetcher.h"
#include "private.h"

#include <yt/client/chunk_client/chunk_replica.h>
#include <yt/ytlib/chunk_client/config.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/input_chunk.h>
#include <yt/ytlib/chunk_client/input_chunk_slice.h>
#include <yt/ytlib/chunk_client/legacy_data_slice.h>
#include <yt/ytlib/chunk_client/key_set.h>

#include <yt/client/table_client/row_buffer.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/ytlib/tablet_client/helpers.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/rpc/channel.h>

namespace NYT::NTableClient {

using namespace NConcurrency;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NRpc;
using namespace NObjectClient;
using namespace NTabletClient;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

//! Fetches slices for a bunch of table chunks by requesting
//! them directly from data nodes.
class TChunkSliceFetcher
    : public IChunkSliceFetcher
    , public NChunkClient::TFetcherBase
{
public:
    TChunkSliceFetcher(
        TChunkSliceFetcherConfigPtr config,
        NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
        IInvokerPtr invoker,
        IFetcherChunkScraperPtr chunkScraper,
        NApi::NNative::IClientPtr client,
        TRowBufferPtr rowBuffer,
        const NLogging::TLogger& logger)
        : TFetcherBase(
            config,
            nodeDirectory,
            invoker,
            chunkScraper,
            client,
            logger)
        , Config_(std::move(config))
        , RowBuffer_(std::move(rowBuffer))
    { }

    virtual void AddChunk(TInputChunkPtr chunk) override
    {
        YT_UNIMPLEMENTED();
    }

    virtual void AddDataSliceForSlicing(
        TLegacyDataSlicePtr dataSlice,
        i64 sliceDataWeight,
        int keyColumnCount,
        bool sliceByKeys)
    {
        YT_VERIFY(sliceDataWeight > 0);

        auto chunk = dataSlice->GetSingleUnversionedChunkOrThrow();
        TFetcherBase::AddChunk(chunk);

        YT_VERIFY(dataSlice->ChunkSlices.size() == 1);

        // Note that we do not patch chunk slices limits anywhere in chunk pool as it is
        // a part of data slice physical representation. In future it is going to become
        // hidden in physical data reigstry.
        //
        // As a consequence, by this moment limit in chunk slice may be longer than needed,
        // so we copy chunk slice for internal chunk slice fetcher needs and replace
        // chunk slice limits with data slice limits which are already proper (i.e. have length
        // of #keyColumnCount).
        //
        // This logic fixes test_scheduler_reduce.py::TestSchedulerReduceCommands::test_column_filter.

        auto chunkSliceCopy = CreateInputChunkSlice(*dataSlice->ChunkSlices[0]);
        chunkSliceCopy->LegacyLowerLimit() = dataSlice->LegacyLowerLimit();
        chunkSliceCopy->LegacyUpperLimit() = dataSlice->LegacyUpperLimit();
        chunkSliceCopy->LowerLimit() = dataSlice->LowerLimit();
        chunkSliceCopy->UpperLimit() = dataSlice->UpperLimit();

        auto dataSliceCopy = CreateUnversionedInputDataSlice(chunkSliceCopy);

        TChunkSliceRequest chunkSliceRequest {
            .ChunkSliceDataWeight = sliceDataWeight,
            .KeyColumnCount = keyColumnCount,
            .SliceByKeys = sliceByKeys,
            .DataSlice = dataSliceCopy,
        };
        YT_VERIFY(ChunkToChunkSliceRequest_.emplace(chunk, chunkSliceRequest).second);
    }

    virtual TFuture<void> Fetch() override
    {
        YT_LOG_DEBUG("Started fetching chunk slices (ChunkCount: %v)",
            Chunks_.size());
        return TFetcherBase::Fetch();
    }

    virtual std::vector<NChunkClient::TInputChunkSlicePtr> GetChunkSlices() override
    {
        std::vector<NChunkClient::TInputChunkSlicePtr> chunkSlices;
        chunkSlices.reserve(SliceCount_);
        for (const auto& slices : SlicesByChunkIndex_) {
            chunkSlices.insert(chunkSlices.end(), slices.begin(), slices.end());
        }
        return chunkSlices;
    }

private:
    const TChunkSliceFetcherConfigPtr Config_;
    const NTableClient::TRowBufferPtr RowBuffer_;

    //! All slices fetched so far.
    std::vector<std::vector<NChunkClient::TInputChunkSlicePtr>> SlicesByChunkIndex_;

    //! Number of slices in SlicesByChunkIndex_.
    i64 SliceCount_ = 0;

    struct TChunkSliceRequest
    {
        i64 ChunkSliceDataWeight;
        int KeyColumnCount;
        bool SliceByKeys;
        TLegacyDataSlicePtr DataSlice;
    };
    THashMap<TInputChunkPtr, TChunkSliceRequest> ChunkToChunkSliceRequest_;

    virtual void OnFetchingStarted() override
    {
        SlicesByChunkIndex_.resize(Chunks_.size());
    }

    virtual TFuture<void> FetchFromNode(
        NNodeTrackerClient::TNodeId nodeId,
        std::vector<int> chunkIndexes) override
    {
        return BIND(&TChunkSliceFetcher::DoFetchFromNode, MakeStrong(this), nodeId, Passed(std::move(chunkIndexes)))
            .AsyncVia(Invoker_)
            .Run();
    }

    TFuture<void> DoFetchFromNode(
        NNodeTrackerClient::TNodeId nodeId,
        const std::vector<int>& chunkIndexes)
    {
        TDataNodeServiceProxy proxy(GetNodeChannel(nodeId));
        proxy.SetDefaultTimeout(Config_->NodeRpcTimeout);

        std::vector<TFuture<void>> futures;

        TDataNodeServiceProxy::TReqGetChunkSlicesPtr req;
        std::vector<int> requestedChunkIndexes;

        auto createRequest = [&] {
            req = proxy.GetChunkSlices();
            req->SetHeavy(true);
            req->SetMultiplexingBand(EMultiplexingBand::Heavy);
            // TODO(babenko): make configurable
            ToProto(req->mutable_workload_descriptor(), TWorkloadDescriptor(EWorkloadCategory::UserBatch));
        };

        auto flushBatch = [&] {
            if (req->slice_requests_size() > 0) {
                futures.push_back(
                    req->Invoke().Apply(
                        BIND(&TChunkSliceFetcher::OnResponse, MakeStrong(this), nodeId, Passed(std::move(requestedChunkIndexes)))
                            .AsyncVia(Invoker_)));

                requestedChunkIndexes.clear();
                createRequest();
            }
        };

        createRequest();

        for (auto chunkIndex : chunkIndexes) {
            const auto& chunk = Chunks_[chunkIndex];
            const auto& sliceRequest = ChunkToChunkSliceRequest_[chunk];

            auto chunkDataSize = chunk->GetUncompressedDataSize();

            if (!chunk->BoundaryKeys()) {
                THROW_ERROR_EXCEPTION("Missing boundary keys in chunk %v", chunk->ChunkId());
            }
            const auto& minKey = chunk->BoundaryKeys()->MinKey;
            const auto& maxKey = chunk->BoundaryKeys()->MaxKey;
            auto chunkSliceDataWeight = sliceRequest.ChunkSliceDataWeight;
            auto sliceByKeys = sliceRequest.SliceByKeys;
            auto keyColumnCount = sliceRequest.KeyColumnCount;
            auto chunkType = TypeFromId(chunk->ChunkId());

            if (chunkDataSize < chunkSliceDataWeight ||
                IsDynamicTabletStoreType(chunkType) ||
                (sliceByKeys && CompareRows(minKey, maxKey, keyColumnCount) == 0))
            {
                YT_VERIFY(chunkIndex < SlicesByChunkIndex_.size());
                auto chunkSlice = sliceRequest.DataSlice->ChunkSlices[0];
                SlicesByChunkIndex_[chunkIndex].push_back(chunkSlice);
                SliceCount_++;
            } else {
                requestedChunkIndexes.push_back(chunkIndex);
                auto chunkId = EncodeChunkId(chunk, nodeId);

                auto* protoSliceRequest = req->add_slice_requests();
                ToProto(protoSliceRequest->mutable_chunk_id(), chunkId);

                if (sliceRequest.DataSlice->IsLegacy) {
                    ToProto(protoSliceRequest->mutable_lower_limit(), sliceRequest.DataSlice->LegacyLowerLimit());
                    ToProto(protoSliceRequest->mutable_upper_limit(), sliceRequest.DataSlice->LegacyUpperLimit());
                } else {
                    ToProto(protoSliceRequest->mutable_lower_limit(), sliceRequest.DataSlice->LowerLimit());
                    ToProto(protoSliceRequest->mutable_upper_limit(), sliceRequest.DataSlice->UpperLimit());
                }

                // TODO(max42, gritukan): this field seems useless. Consider dropping it here and in proto message.
                protoSliceRequest->set_erasure_codec(static_cast<int>(chunk->GetErasureCodec()));
                protoSliceRequest->set_slice_data_weight(chunkSliceDataWeight);
                protoSliceRequest->set_slice_by_keys(sliceByKeys);
                protoSliceRequest->set_key_column_count(keyColumnCount);
            }

            if (req->slice_requests_size() >= Config_->MaxSlicesPerFetch) {
                flushBatch();
            }
        }

        flushBatch();

        return AllSucceeded(futures);
    }

    void OnResponse(
        NNodeTrackerClient::TNodeId nodeId,
        const std::vector<int>& requestedChunkIndexes,
        const NChunkClient::TDataNodeServiceProxy::TErrorOrRspGetChunkSlicesPtr& rspOrError)
    {
        if (!rspOrError.IsOK()) {
            YT_LOG_INFO("Failed to get chunk slices from node (Address: %v, NodeId: %v)",
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

        YT_VERIFY(rsp->Attachments().size() == 1);
        TKeySetReader keysReader(rsp->Attachments()[0]);
        auto keys = keysReader.GetKeys();

        for (int i = 0; i < requestedChunkIndexes.size(); ++i) {
            int index = requestedChunkIndexes[i];
            const auto& chunk = Chunks_[index];
            const auto& sliceRequest = ChunkToChunkSliceRequest_[chunk];
            const auto& sliceResponse = rsp->slice_responses(i);

            if (sliceResponse.has_error()) {
                auto error = FromProto<TError>(sliceResponse.error());

                if (error.FindMatching(EErrorCode::IncompatibleKeyColumns)) {
                    // Any exception thrown here interrupts fetching.
                    error.ThrowOnError();
                }

                OnChunkFailed(nodeId, index, error);
                continue;
            }

            YT_LOG_TRACE("Received %v chunk slices for chunk #%v",
                sliceResponse.chunk_slices_size(),
                index);

            YT_VERIFY(index < SlicesByChunkIndex_.size());

            auto comparator = TComparator(std::vector<ESortOrder>(sliceRequest.KeyColumnCount, ESortOrder::Ascending));

            const auto& originalChunkSlice = sliceRequest.DataSlice->ChunkSlices[0];

            for (const auto& protoChunkSlice : sliceResponse.chunk_slices()) {
                TInputChunkSlicePtr chunkSlice;
                if (sliceRequest.DataSlice->IsLegacy) {
                    chunkSlice = New<TInputChunkSlice>(*originalChunkSlice, RowBuffer_, protoChunkSlice, keys);
                } else {
                    chunkSlice = New<TInputChunkSlice>(*originalChunkSlice, comparator, RowBuffer_, protoChunkSlice, keys);
                }
                SlicesByChunkIndex_[index].push_back(chunkSlice);
                SliceCount_++;
            }
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

IChunkSliceFetcherPtr CreateChunkSliceFetcher(
    TChunkSliceFetcherConfigPtr config,
    TNodeDirectoryPtr nodeDirectory,
    IInvokerPtr invoker,
    IFetcherChunkScraperPtr chunkScraper,
    NApi::NNative::IClientPtr client,
    TRowBufferPtr rowBuffer,
    const NLogging::TLogger& logger)
{
    return New<TChunkSliceFetcher>(
        config,
        nodeDirectory,
        invoker,
        chunkScraper,
        client,
        rowBuffer,
        logger);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
