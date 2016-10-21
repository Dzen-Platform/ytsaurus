#include "sorted_merge_job.h"
#include "config.h"
#include "job_detail.h"

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/chunk_client/chunk_spec.h>

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_chunk_reader.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>
#include <yt/ytlib/table_client/schemaless_sorted_merging_reader.h>

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

class TSortedMergeJob
    : public TSimpleJobBase
{
public:
    explicit TSortedMergeJob(IJobHostPtr host)
        : TSimpleJobBase(host)
        , MergeJobSpecExt_(JobSpec_.GetExtension(TMergeJobSpecExt::merge_job_spec_ext))
    { }

    virtual void Initialize() override
    {
        auto config = Host_->GetConfig();

        YCHECK(SchedulerJobSpecExt_.output_specs_size() == 1);
        const auto& outputSpec = SchedulerJobSpecExt_.output_specs(0);

        auto keyColumns = FromProto<TKeyColumns>(MergeJobSpecExt_.key_columns());

        auto nameTable = TNameTable::FromKeyColumns(keyColumns);
        std::vector<ISchemalessMultiChunkReaderPtr> readers;

        for (const auto& inputSpec : SchedulerJobSpecExt_.input_specs()) {
            auto dataSliceDescriptors = FromProto<std::vector<TDataSliceDescriptor>>(inputSpec.data_slice_descriptors());
            auto readerOptions = ConvertTo<NTableClient::TTableReaderOptionsPtr>(TYsonString(inputSpec.table_reader_options()));

            TotalRowCount_ += GetCumulativeRowCount(dataSliceDescriptors);

            auto reader = CreateSchemalessSequentialMultiChunkReader(
                config->JobIO->TableReader,
                readerOptions,
                Host_->GetClient(),
                Host_->LocalDescriptor(),
                Host_->GetBlockCache(),
                Host_->GetInputNodeDirectory(),
                std::move(dataSliceDescriptors),
                nameTable,
                TColumnFilter(),
                keyColumns);

            readers.push_back(reader);
        }

        Reader_ = CreateSchemalessSortedMergingReader(readers, keyColumns.size());

        auto transactionId = FromProto<TTransactionId>(SchedulerJobSpecExt_.output_transaction_id());
        auto chunkListId = FromProto<TChunkListId>(outputSpec.chunk_list_id());
        auto options = ConvertTo<TTableWriterOptionsPtr>(TYsonString(outputSpec.table_writer_options()));
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

private:
    const TMergeJobSpecExt& MergeJobSpecExt_;


    virtual void CreateReader() override
    { }

    virtual void CreateWriter() override
    { }
};

IJobPtr CreateSortedMergeJob(IJobHostPtr host)
{
    return New<TSortedMergeJob>(host);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
