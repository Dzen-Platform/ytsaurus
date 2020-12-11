#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/fetcher.h>
#include <yt/ytlib/chunk_client/data_node_service_proxy.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/core/misc/phoenix.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct IChunkSliceFetcher
    : public virtual NChunkClient::IFetcher
{
    // TODO(max42): return data slices here.
    virtual std::vector<NChunkClient::TInputChunkSlicePtr> GetChunkSlices() = 0;

    // TODO(max42): interface should accept abstract data slice. Particular implementation
    // should hold a pointer to a physical data registry.
    virtual void AddDataSliceForSlicing(
        NChunkClient::TLegacyDataSlicePtr dataSlice,
        i64 sliceDataWeight,
        int keyColumnCount,
        bool sliceByKeys) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkSliceFetcher);

////////////////////////////////////////////////////////////////////////////////

IChunkSliceFetcherPtr CreateChunkSliceFetcher(
    NChunkClient::TChunkSliceFetcherConfigPtr config,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    IInvokerPtr invoker,
    NChunkClient::IFetcherChunkScraperPtr chunkScraper,
    NApi::NNative::IClientPtr client,
    NTableClient::TRowBufferPtr rowBuffer,
    const NLogging::TLogger& logger);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
