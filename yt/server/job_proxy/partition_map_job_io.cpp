#include "partition_map_job_io.h"
#include "job.h"
#include "user_job_io_detail.h"

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/scheduler/config.h>

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/partitioner.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>

namespace NYT {
namespace NJobProxy {

using namespace NTableClient;
using namespace NTransactionClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NScheduler;
using namespace NScheduler::NProto;

////////////////////////////////////////////////////////////////////

class TPartitionMapJobIO
    : public TUserJobIOBase
{
public:
    explicit TPartitionMapJobIO(IJobHostPtr host)
        : TUserJobIOBase(host)
    { }

    virtual ISchemalessMultiChunkWriterPtr DoCreateWriter(
        TTableWriterOptionsPtr options,
        const TChunkListId& chunkListId,
        const TTransactionId& transactionId,
        // Key columns for partitioner come from job spec extension.
        const TKeyColumns& /* keyColumns */) override
    {
        const auto& jobSpec = Host_->GetJobSpec();
        const auto& jobSpecExt = jobSpec.GetExtension(TPartitionJobSpecExt::partition_job_spec_ext);
        auto partitioner = CreateHashPartitioner(
            jobSpecExt.partition_count(),
            jobSpecExt.reduce_key_column_count());
        auto keyColumns = FromProto<TKeyColumns>(jobSpecExt.sort_key_columns());

        auto nameTable = TNameTable::FromKeyColumns(keyColumns);
        nameTable->SetEnableColumnNameValidation();
        
        return CreatePartitionMultiChunkWriter(
            JobIOConfig_->TableWriter,
            options,
            nameTable,
            keyColumns,
            Host_->GetClient(),
            CellTagFromId(chunkListId),
            transactionId,
            chunkListId,
            std::move(partitioner));
    }

    virtual ISchemalessMultiChunkReaderPtr DoCreateReader(
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        // ToDo(psushin): don't use parallel readers here to minimize nondetermenistics
        // behaviour in mapper, that may lead to huge problems in presence of lost jobs.
        return CreateRegularReader(false, std::move(nameTable), columnFilter);
    }

    virtual void PopulateResult(TSchedulerJobResultExt* schedulerJobResult) override
    {
        // Don't call base class method, no need to fill boundary keys.

        YCHECK(Writers_.size() == 1);
        auto& writer = Writers_.front();
        writer->GetNodeDirectory()->DumpTo(schedulerJobResult->mutable_output_node_directory());
        ToProto(schedulerJobResult->mutable_output_chunks(), writer->GetWrittenChunksMasterMeta());
    }

};

std::unique_ptr<IUserJobIO> CreatePartitionMapJobIO(IJobHostPtr host)
{
    return std::unique_ptr<IUserJobIO>(new TPartitionMapJobIO(host));
}

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
