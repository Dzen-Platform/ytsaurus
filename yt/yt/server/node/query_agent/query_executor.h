#pragma once

#include "public.h"

#include <yt/server/node/cluster_node/public.h>

#include <yt/server/lib/misc/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/query_client/public.h>
#include <yt/ytlib/query_client/query_common.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/core/actions/future.h>

namespace NYT::NQueryAgent {

////////////////////////////////////////////////////////////////////////////////

NQueryClient::TQueryStatistics ExecuteSubquery(
    TQueryAgentConfigPtr config,
    NQueryClient::TFunctionImplCachePtr functionImplCache,
    NClusterNode::TBootstrap* const bootstrap,
    NQueryClient::TEvaluatorPtr evaluator,
    NQueryClient::TConstQueryPtr query,
    NQueryClient::TConstExternalCGInfoPtr externalCGInfo,
    std::vector<NQueryClient::TDataRanges> dataSources,
    NQueryClient::IUnversionedRowsetWriterPtr writer,
    const NChunkClient::TClientBlockReadOptions& blockReadOptions,
    const NQueryClient::TQueryOptions& options,
    IInvokerPtr invoker,
    TServiceProfilerGuard& profilerGuard);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryAgent

