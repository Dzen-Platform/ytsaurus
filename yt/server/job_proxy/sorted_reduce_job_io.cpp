﻿#include "stdafx.h"
#include "sorted_reduce_job_io.h"
#include "config.h"
#include "user_job_io_detail.h"
#include "job.h"

#include <ytlib/new_table_client/name_table.h>
#include <ytlib/new_table_client/schemaless_chunk_reader.h>
#include <ytlib/new_table_client/schemaless_sorted_merging_reader.h>

namespace NYT {
namespace NJobProxy {

using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NVersionedTableClient;
using namespace NChunkClient;
using namespace NTransactionClient;
using namespace NChunkClient::NProto;
using namespace NJobTrackerClient::NProto;

////////////////////////////////////////////////////////////////////

class TSortedReduceJobIO
    : public TUserJobIOBase
{
public:
    explicit TSortedReduceJobIO(IJobHost* host)
        : TUserJobIOBase(host)
    { }

    virtual ISchemalessMultiChunkReaderPtr DoCreateReader(
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        YCHECK(nameTable->GetSize() == 0 && columnFilter.All);

        const auto& jobSpec = Host_->GetJobSpec();
        const auto& jobSpecExt = jobSpec.GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        auto keyColumns = FromProto<Stroka>(jobSpecExt.key_columns());

        std::vector<ISchemalessMultiChunkReaderPtr> readers;
        nameTable = TNameTable::FromKeyColumns(keyColumns);
        auto options = New<TMultiChunkReaderOptions>();

        for (const auto& inputSpec : SchedulerJobSpec_.input_specs()) {
            // ToDo(psushin): validate that input chunks are sorted.
            std::vector<TChunkSpec> chunks(inputSpec.chunks().begin(), inputSpec.chunks().end());

            auto reader = CreateSchemalessSequentialMultiChunkReader(
                JobIOConfig_->TableReader,
                options,
                Host_->GetMasterChannel(),
                Host_->GetBlockCache(),
                Host_->GetNodeDirectory(),
                chunks,
                nameTable,
                columnFilter,
                keyColumns);

            readers.push_back(reader);
        }

        return CreateSchemalessSortedMergingReader(readers, keyColumns.size());
    }

    virtual ISchemalessMultiChunkWriterPtr DoCreateWriter(
        TTableWriterOptionsPtr options,
        const TChunkListId& chunkListId,
        const TTransactionId& transactionId,
        const TKeyColumns& keyColumns) override
    {
        return CreateTableWriter(options, chunkListId, transactionId, keyColumns);
    }
};

std::unique_ptr<IUserJobIO> CreateSortedReduceJobIO(IJobHost* host)
{
    return std::unique_ptr<IUserJobIO>(new TSortedReduceJobIO(host));
}

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
