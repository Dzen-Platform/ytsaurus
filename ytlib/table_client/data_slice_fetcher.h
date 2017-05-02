#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/data_node_service_proxy.h>
#include <yt/ytlib/chunk_client/fetcher.h>

#include <yt/ytlib/node_tracker_client/public.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

//! Fetches data slices for a bunch of table chunks by requesting
//! them directly from data nodes.
class TDataSliceFetcher
    : public TRefCounted
{
public:
    TDataSliceFetcher(
        NChunkClient::TFetcherConfigPtr config,
        i64 chunkSliceSize,
        const TKeyColumns& keyColumns,
        bool sliceByKeys,
        NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
        IInvokerPtr invoker,
        NChunkClient::TScrapeChunksCallback scraperCallback,
        NApi::INativeClientPtr client,
        NTableClient::TRowBufferPtr rowBuffer,
        const NLogging::TLogger& logger);

    void AddChunk(NChunkClient::TInputChunkPtr chunk);
    TFuture<void> Fetch();
    std::vector<NChunkClient::TInputDataSlicePtr> GetDataSlices();

private:
    const IChunkSliceFetcherPtr ChunkSliceFetcher_;
};

DEFINE_REFCOUNTED_TYPE(TDataSliceFetcher)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
