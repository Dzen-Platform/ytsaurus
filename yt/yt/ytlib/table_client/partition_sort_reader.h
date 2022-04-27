#pragma once

#include "public.h"
#include "schemaless_multi_chunk_reader.h"

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/client/api/public.h>

#include <yt/yt/client/table_client/comparator.h>

#include <yt/yt/core/rpc/public.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreatePartitionSortReader(
    NChunkClient::TMultiChunkReaderConfigPtr config,
    NApi::NNative::IClientPtr client,
    NChunkClient::IBlockCachePtr blockCache,
    NChunkClient::IClientChunkMetaCachePtr chunkMetaCache,
    TComparator comparator,
    TNameTablePtr nameTable,
    TClosure onNetworkReleased,
    NChunkClient::TDataSourceDirectoryPtr dataSourceDirectory,
    std::vector<NChunkClient::TDataSliceDescriptor> dataSliceDescriptors,
    i64 estimatedRowCount,
    bool isApproximate,
    int partitionTag,
    NChunkClient::TClientChunkReadOptions chunkReadOptions,
    NChunkClient::TTrafficMeterPtr trafficMeter,
    NConcurrency::IThroughputThrottlerPtr bandwidthThrottler = NConcurrency::GetUnlimitedThrottler(),
    NConcurrency::IThroughputThrottlerPtr rpsThrottler = NConcurrency::GetUnlimitedThrottler(),
    NChunkClient::IMultiReaderMemoryManagerPtr multiReaderMemoryManager = nullptr);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
