#pragma once

#include "public.h"
#include "callbacks.h"

#include <yt/core/profiling/profiler.h>

#include <yt/ytlib/node_tracker_client/public.h>

namespace NYT::NQueryClient {

////////////////////////////////////////////////////////////////////////////////

class TEvaluator
    : public TIntrinsicRefCounted
{
public:
    explicit TEvaluator(
        TExecutorConfigPtr config,
        const NProfiling::TProfiler& profiler = NProfiling::TProfiler(),
        NNodeTrackerClient::TNodeMemoryTracker* memoryTracker = nullptr);

    TQueryStatistics Run(
        TConstBaseQueryPtr fragment,
        ISchemafulReaderPtr reader,
        IUnversionedRowsetWriterPtr writer,
        TJoinSubqueryProfiler joinProfiler,
        TConstFunctionProfilerMapPtr functionProfilers,
        TConstAggregateProfilerMapPtr aggregateProfilers,
        const TQueryBaseOptions& options);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TEvaluator)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient

