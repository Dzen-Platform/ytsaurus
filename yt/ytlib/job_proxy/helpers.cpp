#include "helpers.h"

#include "private.h"

#include <yt/ytlib/query_client/evaluator.h>
#include <yt/ytlib/query_client/functions_cache.h>
#include <yt/ytlib/query_client/query_statistics.h>
#include <yt/ytlib/query_client/config.h>
#include <yt/ytlib/query_client/query.h>

#include <yt/ytlib/scheduler/job.pb.h>

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_writer.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/protobuf_helpers.h>


namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

using namespace NScheduler::NProto;
using namespace NQueryClient;
using namespace NTableClient;
using namespace NConcurrency;
using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

static NLogging::TLogger& Logger = JobProxyClientLogger;

////////////////////////////////////////////////////////////////////////////////

void RunQuery(
    const TQuerySpec& querySpec,
    const TSchemalessReaderFactory& readerFactory,
    const TSchemalessWriterFactory& writerFactory,
    const TNullable<TString>& udfDirectory)
{
    auto query = FromProto<TConstQueryPtr>(querySpec.query());
    auto resultSchema = query->GetTableSchema();
    auto resultNameTable = TNameTable::FromSchema(resultSchema);
    auto schemalessWriter = writerFactory(resultNameTable);

    WaitFor(schemalessWriter->Open())
        .ThrowOnError();

    auto writer = CreateSchemafulWriterAdapter(schemalessWriter);

    auto externalCGInfo = New<TExternalCGInfo>();
    FromProto(&externalCGInfo->Functions, querySpec.external_functions());

    auto functionGenerators = New<TFunctionProfilerMap>();
    auto aggregateGenerators = New<TAggregateProfilerMap>();
    MergeFrom(functionGenerators.Get(), *BuiltinFunctionCG);
    MergeFrom(aggregateGenerators.Get(), *BuiltinAggregateCG);
    if (udfDirectory) {
        FetchJobImplementations(
            functionGenerators,
            aggregateGenerators,
            externalCGInfo,
            *udfDirectory);
    }

    auto evaluator = New<TEvaluator>(New<TExecutorConfig>());
    auto reader = CreateSchemafulReaderAdapter(readerFactory, query->GetReadSchema());

    LOG_INFO("Reading, evaluating query and writing");
    evaluator->Run(query, reader, writer, functionGenerators, aggregateGenerators, true);
}

////////////////////////////////////////////////////////////////////////////////

std::vector<TDataSliceDescriptor> UnpackDataSliceDescriptors(const TTableInputSpec& inputTableSpec)
{
    if (inputTableSpec.chunk_specs_size() > 0) {
        return FromProto<std::vector<TDataSliceDescriptor>>(
            inputTableSpec.chunk_specs(),
            inputTableSpec.chunk_spec_count_per_data_slice());
    } else {
        // COMPAT(psushin).
        return FromProto<std::vector<TDataSliceDescriptor>>(inputTableSpec.data_slice_descriptors());
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
