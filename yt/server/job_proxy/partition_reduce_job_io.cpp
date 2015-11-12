﻿#include <yt/core/misc/common.h>
#include "partition_reduce_job_io.h"
#include "config.h"
#include "user_job_io_detail.h"
#include "job.h"

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_partition_sort_reader.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

namespace NYT {
namespace NJobProxy {

using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NChunkClient::NProto;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////

class TPartitionReduceJobIO
    : public TUserJobIOBase
{
public:
    TPartitionReduceJobIO(IJobHost* host)
        : TUserJobIOBase(host)
    { 
        const auto& reduceJobSpecExt = Host_->GetJobSpec().GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        ReduceKeyColumnCount_ = reduceJobSpecExt.reduce_key_column_count();
    }

    virtual int GetReduceKeyColumnCount() const override
    {
        return ReduceKeyColumnCount_;
    }

    virtual ISchemalessMultiChunkReaderPtr DoCreateReader(
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        YCHECK(nameTable->GetSize() == 0 && columnFilter.All);
        YCHECK(SchedulerJobSpec_.input_specs_size() == 1);

        const auto& inputSpec = SchedulerJobSpec_.input_specs(0);
        std::vector<TChunkSpec> chunks(
            inputSpec.chunks().begin(),
            inputSpec.chunks().end());

        const auto& reduceJobSpecExt = Host_->GetJobSpec().GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        auto keyColumns = FromProto<Stroka>(reduceJobSpecExt.key_columns());
        nameTable = TNameTable::FromKeyColumns(keyColumns);

        return CreateSchemalessPartitionSortReader(
            JobIOConfig_->TableReader,
            Host_->GetClient(),
            Host_->GetBlockCache(),
            Host_->GetNodeDirectory(),
            keyColumns,
            nameTable,
            BIND(&IJobHost::ReleaseNetwork, Host_),
            chunks,
            SchedulerJobSpec_.input_row_count(),
            SchedulerJobSpec_.is_approximate());
    }

    virtual ISchemalessMultiChunkWriterPtr DoCreateWriter(
        TTableWriterOptionsPtr options,
        const TChunkListId& chunkListId,
        const TTransactionId& transactionId,
        const TKeyColumns& keyColumns) override
    {
        return CreateTableWriter(options, chunkListId, transactionId, keyColumns);
    }

    virtual void PopulateResult(TSchedulerJobResultExt* schedulerJobResult) override
    {
        TUserJobIOBase::PopulateResult(schedulerJobResult);

        auto& writer = Writers_.front();

        // Partition reduce may come as intermediate job (reduce-combiner),
        // so we return written chunks to scheduler.
        writer->GetNodeDirectory()->DumpTo(schedulerJobResult->mutable_node_directory());
        ToProto(schedulerJobResult->mutable_chunks(), writer->GetWrittenChunks());
    }

private:
    int ReduceKeyColumnCount_;
};

std::unique_ptr<IUserJobIO> CreatePartitionReduceJobIO(IJobHost* host)
{
    return std::unique_ptr<IUserJobIO>(new TPartitionReduceJobIO(host));
}

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
