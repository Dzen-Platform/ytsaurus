#include "chunk_slices_fetcher.h"
#include "data_slice_descriptor.h"
#include "data_slice_fetcher.h"

#include <yt/ytlib/chunk_client/input_chunk_slice.h>
#include <yt/ytlib/chunk_client/input_data_slice.h>

#include <tuple>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

TDataSliceFetcher::TDataSliceFetcher(
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
    : ChunkSliceFetcher_(New<TChunkSliceFetcher>(
        std::move(config),
        chunkSliceSize,
        keyColumns,
        sliceByKeys,
        std::move(nodeDirectory),
        std::move(invoker),
        std::move(scraperCallback),
        std::move(client),
        std::move(rowBuffer),
        logger))
{ }

void TDataSliceFetcher::AddChunk(TInputChunkPtr chunk)
{
    ChunkSliceFetcher_->AddChunk(std::move(chunk));
}

TFuture<void> TDataSliceFetcher::Fetch()
{
    return ChunkSliceFetcher_->Fetch();
}

std::vector<TInputDataSlicePtr> TDataSliceFetcher::GetDataSlices()
{
    return CombineVersionedChunkSlices(ChunkSliceFetcher_->GetChunkSlices());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

