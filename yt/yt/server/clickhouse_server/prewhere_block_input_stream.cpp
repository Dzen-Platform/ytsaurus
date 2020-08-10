#include "prewhere_block_input_stream.h"

#include "subquery_spec.h"
#include "host.h"
#include "block_input_stream.h"
#include "query_context.h"

#include <yt/ytlib/chunk_client/data_slice_descriptor.h>

#include <DataStreams/IBlockInputStream.h>
#include <Interpreters/ExpressionActions.h>

namespace NYT::NClickHouseServer {

using namespace DB;
using namespace NChunkClient;
using namespace NTableClient;
using namespace NConcurrency;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

Names ExtractColumnsFromPrewhereInfo(PrewhereInfoPtr prewhereInfo)
{
    Names prewhereColumns;
    if (prewhereInfo->alias_actions) {
        prewhereColumns = prewhereInfo->alias_actions->getRequiredColumns();
    } else {
        prewhereColumns = prewhereInfo->prewhere_actions->getRequiredColumns();
    }
    return prewhereColumns;
}

std::vector<TDataSliceDescriptor> GetFilteredDataSliceDescriptors(std::shared_ptr<TBlockInputStream> blockInputStream)
{
    std::vector<TDataSliceDescriptor> filteredDataSliceDescriptors;
    while (auto block = blockInputStream->read()) {
        if (block.rows() > 0) {
            filteredDataSliceDescriptors.emplace_back(blockInputStream->Reader()->GetCurrentReaderDescriptor());
            blockInputStream->Reader()->SkipCurrentReader();
        }
    }
    return filteredDataSliceDescriptors;
}

std::vector<TDataSliceDescriptor> FilterDataSliceDescriptorsByPrewhereInfo(
    std::vector<TDataSliceDescriptor>&& dataSliceDescriptors,
    PrewhereInfoPtr prewhereInfo,
    TStorageContext* storageContext,
    const TSubquerySpec& subquerySpec,
    const NTracing::TTraceContextPtr& traceContext)
{
    auto prewhereColumns = ExtractColumnsFromPrewhereInfo(prewhereInfo);
    auto* queryContext = storageContext->QueryContext;

    auto Logger = queryContext->Logger;
    YT_LOG_DEBUG(
        "Started executing PREWHERE data slice filtering (PrewhereColumnName: %v, PrewhereColumns: %v)",
        prewhereInfo->prewhere_column_name,
        prewhereColumns);

    auto blockInputStream = CreateBlockInputStream(
        storageContext,
        subquerySpec,
        prewhereColumns,
        traceContext,
        dataSliceDescriptors,
        prewhereInfo);

    return WaitFor(BIND(&GetFilteredDataSliceDescriptors, blockInputStream)
        .AsyncVia(queryContext->Host->GetWorkerInvoker())
        .Run())
        .ValueOrThrow();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

class TPrewhereBlockInputStream
    : public DB::IBlockInputStream
{
public:
    TPrewhereBlockInputStream(
        TStorageContext* storageContext,
        const TSubquerySpec& subquerySpec,
        const DB::Names& columnNames,
        NTracing::TTraceContextPtr traceContext,
        DB::PrewhereInfoPtr prewhereInfo,
        std::vector<NChunkClient::TDataSliceDescriptor> dataSliceDescriptors)
        : StorageContext_(storageContext)
        , QueryContext_(storageContext->QueryContext)
        , SubquerySpec_(subquerySpec)
        , ColumnNames_(columnNames)
        , TraceContext_(traceContext)
        , PrewhereInfo_(std::move(prewhereInfo))
        , Header_(CreateBlockInputStream(
            StorageContext_,
            SubquerySpec_,
            ColumnNames_,
            TraceContext_,
            {},
            PrewhereInfo_)->getHeader())
        , DataSliceDescriptors_(std::move(dataSliceDescriptors))
        , Logger(QueryContext_->Logger)
    { }

    virtual std::string getName() const override
    {
        return "PrewhereBlockInputStream";
    }

    virtual DB::Block getHeader() const override
    {
        return Header_;
    }

    virtual void readPrefixImpl() override
    {
        i64 totalDataWeight = 0;
        for (const auto& dataSliceDescriptor : DataSliceDescriptors_) {
            totalDataWeight += dataSliceDescriptor.ChunkSpecs[0].data_weight_override();
        }

        DataSliceDescriptors_ = NDetail::FilterDataSliceDescriptorsByPrewhereInfo(
            std::move(DataSliceDescriptors_),
            PrewhereInfo_,
            StorageContext_,
            SubquerySpec_,
            TraceContext_);

        i64 filteredDataWeight = 0;
        for (const auto& dataSliceDescriptor : DataSliceDescriptors_) {
            filteredDataWeight += dataSliceDescriptor.ChunkSpecs[0].data_weight_override();
        }
        double droppedRate  = 1.0 - static_cast<double>(filteredDataWeight) / static_cast<double>(totalDataWeight);
        YT_LOG_DEBUG("PREWHERE filtration finished (DroppedRate: %v)", droppedRate);

        BlockInputStream_ = CreateBlockInputStream(
            StorageContext_,
            SubquerySpec_,
            ColumnNames_,
            TraceContext_,
            DataSliceDescriptors_,
            PrewhereInfo_);
        BlockInputStream_->readPrefixImpl();
    }

    virtual void readSuffixImpl() override
    {
        BlockInputStream_->readSuffixImpl();
    }

private:
    TStorageContext* StorageContext_;
    TQueryContext* QueryContext_;
    const TSubquerySpec SubquerySpec_;
    const DB::Names ColumnNames_;
    NTracing::TTraceContextPtr TraceContext_;
    DB::PrewhereInfoPtr PrewhereInfo_;
    DB::Block Header_;

    std::vector<NChunkClient::TDataSliceDescriptor> DataSliceDescriptors_;
    std::shared_ptr<TBlockInputStream> BlockInputStream_;

    TLogger Logger;

    virtual Block readImpl() override
    {
        return BlockInputStream_->read();
    }
};

////////////////////////////////////////////////////////////////////////////////

DB::BlockInputStreamPtr CreatePrewhereBlockInputStream(
    TStorageContext* storageContext,
    const TSubquerySpec& subquerySpec,
    const DB::Names& columnNames,
    const NTracing::TTraceContextPtr& traceContext,
    std::vector<NChunkClient::TDataSliceDescriptor> dataSliceDescriptors,
    DB::PrewhereInfoPtr prewhereInfo)
{
    return std::make_shared<TPrewhereBlockInputStream>(
        storageContext,
        subquerySpec,
        columnNames,
        traceContext,
        prewhereInfo,
        dataSliceDescriptors);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
