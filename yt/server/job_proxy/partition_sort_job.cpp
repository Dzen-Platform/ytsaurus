#include "partition_sort_job.h"
#include "private.h"
#include "config.h"
#include "job_detail.h"

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>
#include <yt/ytlib/table_client/schemaless_partition_sort_reader.h>

#include <yt/ytlib/object_client/helpers.h>

namespace NYT {
namespace NJobProxy {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NScheduler::NProto;
using namespace NTransactionClient;
using namespace NTableClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = JobProxyLogger;

////////////////////////////////////////////////////////////////////////////////

class TPartitionSortJob
    : public TSimpleJobBase
{
public:
    explicit TPartitionSortJob(IJobHostPtr host)
        : TSimpleJobBase(host)
        , SortJobSpecExt_(JobSpec_.GetExtension(TSortJobSpecExt::sort_job_spec_ext))
    {
        YCHECK(SortJobSpecExt_.has_partition_tag());
    }

    virtual void Initialize() override
    {
        auto config = Host_->GetConfig();

        auto keyColumns = FromProto<TKeyColumns>(SortJobSpecExt_.key_columns());
        auto nameTable = TNameTable::FromKeyColumns(keyColumns);

        TotalRowCount_ = SchedulerJobSpecExt_.input_row_count();

        YCHECK(SchedulerJobSpecExt_.input_table_specs_size() == 1);
        const auto& inputSpec = SchedulerJobSpecExt_.input_table_specs(0);
        std::vector<TChunkSpec> chunkSpecs(inputSpec.chunks().begin(), inputSpec.chunks().end());

        Reader_ = CreateSchemalessPartitionSortReader(
            config->JobIO->TableReader,
            Host_->GetClient(),
            Host_->GetBlockCache(),
            Host_->GetInputNodeDirectory(),
            keyColumns,
            nameTable,
            BIND(&IJobHost::ReleaseNetwork, Host_),
            chunkSpecs,
            TotalRowCount_,
            SchedulerJobSpecExt_.is_approximate(),
            SortJobSpecExt_.partition_tag());

        YCHECK(SchedulerJobSpecExt_.output_table_specs_size() == 1);

        const auto& outputSpec = SchedulerJobSpecExt_.output_table_specs(0);
        auto transactionId = FromProto<TTransactionId>(SchedulerJobSpecExt_.output_transaction_id());
        auto chunkListId = FromProto<TChunkListId>(outputSpec.chunk_list_id());
        auto options = ConvertTo<TTableWriterOptionsPtr>(TYsonString(outputSpec.table_writer_options()));
        options->ExplodeOnValidationError = true;
        options->ValidateKeyWeight = true;

        auto schema = FromProto<TTableSchema>(outputSpec.table_schema());

        Writer_ = CreateSchemalessMultiChunkWriter(
            config->JobIO->TableWriter,
            options,
            nameTable,
            schema,
            TOwningKey(),
            Host_->GetClient(),
            CellTagFromId(chunkListId),
            transactionId,
            chunkListId);
    }

    virtual double GetProgress() const override
    {
        auto total = TotalRowCount_;
        if (total == 0) {
            LOG_WARNING("GetProgress: empty total");
            return 0;
        } else {
            // Split progress evenly between reading and writing.
            double progress =
                0.5 * Reader_->GetDataStatistics().row_count() / total +
                0.5 * Writer_->GetDataStatistics().row_count() / total;
            LOG_DEBUG("GetProgress: %lf", progress);
            return progress;
        }
    }

private:
    const TSortJobSpecExt& SortJobSpecExt_;


    virtual void CreateReader() override
    { }

    virtual void CreateWriter() override
    { }
};

IJobPtr CreatePartitionSortJob(IJobHostPtr host)
{
    return New<TPartitionSortJob>(host);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
