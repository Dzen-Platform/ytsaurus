#include "simple_sort_job.h"
#include "private.h"
#include "config.h"
#include "job_detail.h"

#include <yt/ytlib/chunk_client/chunk_spec.h>

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>
#include <yt/ytlib/table_client/schemaless_sorting_reader.h>

#include <yt/core/misc/common.h>

namespace NYT {
namespace NJobProxy {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NScheduler::NProto;
using namespace NTransactionClient;
using namespace NTableClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TSimpleSortJob
    : public TSimpleJobBase
{
public:
    explicit TSimpleSortJob(IJobHost* host)
        : TSimpleJobBase(host)
        , SortJobSpecExt_(JobSpec_.GetExtension(TSortJobSpecExt::sort_job_spec_ext))
    {
        auto config = host->GetConfig();

        auto keyColumns = FromProto<Stroka>(SortJobSpecExt_.key_columns());
        auto nameTable = TNameTable::FromKeyColumns(keyColumns);

        YCHECK(SchedulerJobSpecExt_.input_specs_size() == 1);
        const auto& inputSpec = SchedulerJobSpecExt_.input_specs(0);
        std::vector<TChunkSpec> chunkSpecs(inputSpec.chunks().begin(), inputSpec.chunks().end());
        TotalRowCount_ = GetCumulativeRowCount(chunkSpecs);

        auto reader = CreateSchemalessParallelMultiChunkReader(
            config->JobIO->TableReader,
            New<TMultiChunkReaderOptions>(),
            host->GetClient(),
            host->GetBlockCache(),
            host->GetNodeDirectory(),
            chunkSpecs,
            nameTable);

        Reader_ = CreateSchemalessSortingReader(reader, nameTable, keyColumns);

        auto transactionId = FromProto<TTransactionId>(SchedulerJobSpecExt_.output_transaction_id());
        const auto& outputSpec = SchedulerJobSpecExt_.output_specs(0);
        auto chunkListId = FromProto<TChunkListId>(outputSpec.chunk_list_id());
        auto options = ConvertTo<TTableWriterOptionsPtr>(TYsonString(outputSpec.table_writer_options()));

        Writer_ = CreateSchemalessMultiChunkWriter(
            config->JobIO->TableWriter,
            options,
            nameTable,
            keyColumns,
            TOwningKey(),
            host->GetClient(),
            transactionId,
            chunkListId);
    }

private:
    const TSortJobSpecExt& SortJobSpecExt_;

    virtual void CreateReader() override
    { }

    virtual void CreateWriter() override
    { }
};

IJobPtr CreateSimpleSortJob(IJobHost* host)
{
    return New<TSimpleSortJob>(host);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
