#pragma once

#include "public.h"
#include "query_common.h"

#include <yt/client/ypath/public.h>

#include <yt/core/rpc/public.h>

#include <yt/core/actions/future.h>

namespace NYT::NQueryClient {

////////////////////////////////////////////////////////////////////////////////

using TExecuteQueryCallback = std::function<TFuture<void>(
    const TQueryPtr& query,
    TDataRanges dataRanges,
    IUnversionedRowsetWriterPtr writer)>;

////////////////////////////////////////////////////////////////////////////////

struct IExecutor
    : public virtual TRefCounted
{
    virtual TFuture<TQueryStatistics> Execute(
        TConstQueryPtr query,
        const std::vector<NTabletClient::TTableMountInfoPtr>& mountInfos,
        TConstExternalCGInfoPtr externalCGInfo,
        TDataRanges dataSource,
        IUnversionedRowsetWriterPtr writer,
        const NChunkClient::TClientBlockReadOptions& blockReadOptions,
        const TQueryOptions& options) = 0;

};

DEFINE_REFCOUNTED_TYPE(IExecutor)

////////////////////////////////////////////////////////////////////////////////

struct IPrepareCallbacks
{
    virtual ~IPrepareCallbacks() = default;

    //! Returns the initial split for a given path.
    virtual TFuture<TDataSplit> GetInitialSplit(
        const NYPath::TYPath& path,
        TTimestamp timestamp) = 0;
};

////////////////////////////////////////////////////////////////////////////////

using TJoinSubqueryEvaluator = std::function<ISchemafulReaderPtr(std::vector<TRow>, TRowBufferPtr)>;
using TJoinSubqueryProfiler = std::function<TJoinSubqueryEvaluator(TQueryPtr, TConstJoinClausePtr)>;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient

