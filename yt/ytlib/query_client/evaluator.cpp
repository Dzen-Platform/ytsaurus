#include "stdafx.h"
#include "evaluator.h"

#include "helpers.h"
#include "private.h"
#include "plan_fragment.h"
#include "query_statistics.h"
#include "config.h"

#ifdef YT_USE_LLVM
#include "evaluation_helpers.h"
#include "folding_profiler.h"
#endif

#include <ytlib/new_table_client/schemaful_writer.h>
#include <ytlib/new_table_client/row_buffer.h>

#include <ytlib/query_client/plan_fragment.pb.h>

#include <core/concurrency/scheduler.h>

#include <core/profiling/scoped_timer.h>

#include <core/misc/sync_cache.h>

#include <core/logging/log.h>

#include <core/tracing/trace_context.h>

#ifdef YT_USE_LLVM

#include <llvm/ADT/FoldingSet.h>

#include <llvm/Support/Threading.h>
#include <llvm/Support/TargetSelect.h>

#endif

#include <mutex>

namespace NYT {
namespace NQueryClient {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

#ifdef YT_USE_LLVM

class TCachedCGQuery
    : public TSyncCacheValueBase<
        llvm::FoldingSetNodeID,
        TCachedCGQuery>
{
public:
    TCachedCGQuery(const llvm::FoldingSetNodeID& id, TCGQueryCallback&& function)
        : TSyncCacheValueBase(id)
        , Function_(std::move(function))
    { }

    TCGQueryCallback GetQueryCallback()
    {
        return Function_;
    }

private:
    TCGQueryCallback Function_;
};

typedef TIntrusivePtr<TCachedCGQuery> TCachedCGQueryPtr;

class TEvaluator::TImpl
    : public TSyncSlruCacheBase<llvm::FoldingSetNodeID, TCachedCGQuery>
{
public:
    explicit TImpl(TExecutorConfigPtr config)
        : TSyncSlruCacheBase(config->CGCache)
    { }

    TQueryStatistics Run(
        const TConstQueryPtr& query,
        ISchemafulReaderPtr reader,
        ISchemafulWriterPtr writer,
        TExecuteQuery executeCallback)
    {
        TRACE_CHILD("QueryClient", "Evaluate") {
            TRACE_ANNOTATION("fragment_id", query->Id);

            auto Logger = BuildLogger(query);

            TQueryStatistics statistics;
            TDuration wallTime;

            try {
                NProfiling::TAggregatingTimingGuard timingGuard(&wallTime);

                TCGVariables fragmentParams;
                auto cgQuery = Codegen(query, fragmentParams);

                LOG_DEBUG("Evaluating plan fragment");

                LOG_DEBUG("Opening writer");
                {
                    NProfiling::TAggregatingTimingGuard timingGuard(&statistics.AsyncTime);
                    WaitFor(writer->Open(query->GetTableSchema()))
                        .ThrowOnError();
                }

                TRowBuffer permanentBuffer;
                TRowBuffer outputBuffer;
                TRowBuffer intermediateBuffer;

                std::vector<TRow> batch;
                batch.reserve(MaxRowsPerWrite);

                TExecutionContext executionContext;
                executionContext.Reader = reader.Get();
                executionContext.Schema = query->JoinClause
                    ? query->JoinClause->SelfTableSchema
                    : query->TableSchema;

                executionContext.LiteralRows = &fragmentParams.LiteralRows;
                executionContext.PermanentBuffer = &permanentBuffer;
                executionContext.OutputBuffer = &outputBuffer;
                executionContext.IntermediateBuffer = &intermediateBuffer;
                executionContext.Writer = writer.Get();
                executionContext.Batch = &batch;
                executionContext.Statistics = &statistics;
                executionContext.InputRowLimit = query->InputRowLimit;
                executionContext.OutputRowLimit = query->OutputRowLimit;
                executionContext.GroupRowLimit = query->OutputRowLimit;
                executionContext.JoinRowLimit = query->OutputRowLimit;
                executionContext.Limit = query->Limit;

                if (query->JoinClause) {
                    auto& joinClause = query->JoinClause.Get();
                    YCHECK(executeCallback);
                    executionContext.EvaluateJoin = GetJoinEvaluator(
                        joinClause,
                        query->Predicate,
                        executeCallback);
                }

                LOG_DEBUG("Evaluating query");
                CallCGQueryPtr(cgQuery, fragmentParams.ConstantsRowBuilder.GetRow(), &executionContext);

                LOG_DEBUG("Flushing writer");
                if (!batch.empty()) {
                    bool shouldNotWait;
                    {
                        NProfiling::TAggregatingTimingGuard timingGuard(&statistics.WriteTime);
                        shouldNotWait = writer->Write(batch);
                    }

                    if (!shouldNotWait) {
                        NProfiling::TAggregatingTimingGuard timingGuard(&statistics.AsyncTime);
                        WaitFor(writer->GetReadyEvent())
                            .ThrowOnError();
                    }
                }

                LOG_DEBUG("Closing writer");
                {
                    NProfiling::TAggregatingTimingGuard timingGuard(&statistics.AsyncTime);
                    WaitFor(writer->Close())
                        .ThrowOnError();
                }

                LOG_DEBUG("Finished evaluating plan fragment (PermanentBufferCapacity: %v, OutputBufferCapacity: %v, IntermediateBufferCapacity: %v)",
                    permanentBuffer.GetCapacity(),
                    outputBuffer.GetCapacity(),
                    intermediateBuffer.GetCapacity());

            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION("Query evaluation failed") << ex;
            }

            statistics.SyncTime = wallTime - statistics.AsyncTime;
            statistics.ExecuteTime = statistics.SyncTime - statistics.ReadTime - statistics.WriteTime;

            TRACE_ANNOTATION("rows_read", statistics.RowsRead);
            TRACE_ANNOTATION("rows_written", statistics.RowsWritten);
            TRACE_ANNOTATION("sync_time", statistics.SyncTime);
            TRACE_ANNOTATION("async_time", statistics.AsyncTime);
            TRACE_ANNOTATION("execute_time", statistics.ExecuteTime);
            TRACE_ANNOTATION("read_time", statistics.ReadTime);
            TRACE_ANNOTATION("write_time", statistics.WriteTime);
            TRACE_ANNOTATION("incomplete_input", statistics.IncompleteInput);
            TRACE_ANNOTATION("incomplete_output", statistics.IncompleteOutput);

            return statistics;
        }        
    }

private:
    TCGQueryCallback Codegen(const TConstQueryPtr& query, TCGVariables& variables)
    {
        llvm::FoldingSetNodeID id;

        auto makeCodegenQuery = Profile(query, &id, &variables, nullptr);

        auto Logger = BuildLogger(query);

        auto cgQuery = Find(id);
        if (!cgQuery) {
            LOG_DEBUG("Codegen cache miss");
            try {
                TRACE_CHILD("QueryClient", "Compile") {
                    LOG_DEBUG("Started compiling fragment");
                    cgQuery = New<TCachedCGQuery>(id, makeCodegenQuery());
                    LOG_DEBUG("Finished compiling fragment");
                    TryInsert(cgQuery, &cgQuery);
                }
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION("Failed to compile a fragment")
                    << ex;
            }
        } else {
            LOG_DEBUG("Codegen cache hit");
        }

        return cgQuery->GetQueryCallback();
    }

    static void CallCGQuery(
        const TCGQueryCallback& cgQuery,
        TRow constants,
        TExecutionContext* executionContext)
    {
#ifndef NDEBUG
        int dummy;
        executionContext->StackSizeGuardHelper = reinterpret_cast<size_t>(&dummy);
#endif
        cgQuery(constants, executionContext);
    }

    void(*volatile CallCGQueryPtr)(
        const TCGQueryCallback& cgQuery,
        TRow constants,
        TExecutionContext* executionContext) = CallCGQuery;

};

#endif

////////////////////////////////////////////////////////////////////////////////

TEvaluator::TEvaluator(TExecutorConfigPtr config)
#ifdef YT_USE_LLVM
    : Impl_(New<TImpl>(std::move(config)))
#endif
{ }

TEvaluator::~TEvaluator()
{ }

TQueryStatistics TEvaluator::RunWithExecutor(
    const TConstQueryPtr& query,
    ISchemafulReaderPtr reader,
    ISchemafulWriterPtr writer,
    TExecuteQuery executeCallback)
{
#ifdef YT_USE_LLVM
    return Impl_->Run(query, std::move(reader), std::move(writer), executeCallback);
#else
    THROW_ERROR_EXCEPTION("Query evaluation is not supported in this build");
#endif
}

TQueryStatistics TEvaluator::Run(
    const TConstQueryPtr& query,
    ISchemafulReaderPtr reader,
    ISchemafulWriterPtr writer)
{
    return RunWithExecutor(query, std::move(reader), std::move(writer), nullptr);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
