#include "user_job_io_factory.h"

#include "job_spec_helper.h"

#include <yt/ytlib/api/public.h>

#include <yt/ytlib/chunk_client/chunk_spec.pb.h>
#include <yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/ytlib/chunk_client/data_slice_descriptor.h>
#include <yt/ytlib/chunk_client/data_source.h>

#include <yt/ytlib/job_tracker_client/job.pb.h>
#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/scheduler/job.pb.h>

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/partitioner.h>
#include <yt/ytlib/table_client/schemaless_chunk_reader.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>
#include <yt/ytlib/table_client/schemaless_partition_sort_reader.h>
#include <yt/ytlib/table_client/schemaless_sorted_merging_reader.h>

#include <yt/core/ytree/convert.h>

#include <vector>

namespace NYT {
namespace NJobProxy {

using namespace NApi;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NJobTrackerClient;
using namespace NJobTrackerClient::NProto;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NScheduler::NProto;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NYson;
using namespace NYTree;

namespace {

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkWriterPtr CreateTableWriter(
    const IJobSpecHelperPtr& jobSpecHelper,
    INativeClientPtr client,
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    const TChunkListId& chunkListId,
    const TTransactionId& transactionId,
    const TTableSchema& tableSchema,
    const TChunkTimestamps& chunkTimestamps)
{
    auto nameTable = New<TNameTable>();
    nameTable->SetEnableColumnNameValidation();

    return CreateSchemalessMultiChunkWriter(
        std::move(config),
        std::move(options),
        std::move(nameTable),
        tableSchema,
        TOwningKey(),
        std::move(client),
        CellTagFromId(chunkListId),
        transactionId,
        chunkListId,
        chunkTimestamps);
}

ISchemalessMultiChunkReaderPtr CreateTableReader(
    const IJobSpecHelperPtr& jobSpecHelper,
    INativeClientPtr client,
    const TNodeDescriptor& nodeDescriptor,
    TTableReaderOptionsPtr options,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    std::vector<NChunkClient::  TDataSliceDescriptor> dataSliceDescriptors,
    TNameTablePtr nameTable,
    const TColumnFilter& columnFilter,
    bool isParallel)
{
    if (isParallel) {
        return CreateSchemalessParallelMultiReader(
            jobSpecHelper->GetJobIOConfig()->TableReader,
            std::move(options),
            std::move(client),
            nodeDescriptor,
            GetNullBlockCache(),
            jobSpecHelper->GetInputNodeDirectory(),
            dataSourceDirectory,
            std::move(dataSliceDescriptors),
            std::move(nameTable),
            columnFilter);
    } else {
        return CreateSchemalessSequentialMultiReader(
            jobSpecHelper->GetJobIOConfig()->TableReader,
            std::move(options),
            std::move(client),
            nodeDescriptor,
            GetNullBlockCache(),
            jobSpecHelper->GetInputNodeDirectory(),
            dataSourceDirectory,
            std::move(dataSliceDescriptors),
            std::move(nameTable),
            columnFilter);
    }
}

ISchemalessMultiChunkReaderPtr CreateRegularReader(
    const IJobSpecHelperPtr& jobSpecHelper,
    INativeClientPtr client,
    const TNodeDescriptor& nodeDescriptor,
    bool isParallel,
    TNameTablePtr nameTable,
    const TColumnFilter& columnFilter)
{
    const auto& schedulerJobSpecExt = jobSpecHelper->GetSchedulerJobSpecExt();
    std::vector<NChunkClient::TDataSliceDescriptor> dataSliceDescriptors;
    for (const auto& inputSpec : schedulerJobSpecExt.input_table_specs()) {
        for (const auto& descriptor : inputSpec.data_slice_descriptors()) {
            auto dataSliceDescriptor = FromProto<NChunkClient::TDataSliceDescriptor>(descriptor);
            dataSliceDescriptors.push_back(std::move(dataSliceDescriptor));
        }
    }

    auto dataSourceDirectory = FromProto<TDataSourceDirectoryPtr>(schedulerJobSpecExt.data_source_directory());

    auto options = ConvertTo<TTableReaderOptionsPtr>(TYsonString(
        schedulerJobSpecExt.table_reader_options()));

    return CreateTableReader(
        jobSpecHelper,
        std::move(client),
        std::move(nodeDescriptor),
        std::move(options),
        dataSourceDirectory,
        std::move(dataSliceDescriptors),
        std::move(nameTable),
        columnFilter,
        isParallel);
}

////////////////////////////////////////////////////////////////////////////////

class TMapJobIOFactory
    : public IUserJobIOFactory
{
public:
    TMapJobIOFactory(IJobSpecHelperPtr jobSpecHelper, bool useParallelReader)
        : JobSpecHelper_(std::move(jobSpecHelper))
        , UseParallelReader_(useParallelReader)
    { }

    virtual ISchemalessMultiChunkReaderPtr CreateReader(
        INativeClientPtr client,
        const TNodeDescriptor& nodeDescriptor,
        TClosure onNetworkReleased,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        return CreateRegularReader(
            JobSpecHelper_,
            std::move(client),
            nodeDescriptor,
            UseParallelReader_,
            std::move(nameTable),
            columnFilter);
    }

    virtual ISchemalessMultiChunkWriterPtr CreateWriter(
        NApi::INativeClientPtr client,
        TTableWriterConfigPtr config,
        TTableWriterOptionsPtr options,
        const TChunkListId& chunkListId,
        const TTransactionId& transactionId,
        const TTableSchema& tableSchema,
        const TChunkTimestamps& chunkTimestamps) override
    {
        return CreateTableWriter(
            JobSpecHelper_,
            std::move(client),
            std::move(config),
            std::move(options),
            chunkListId,
            transactionId,
            tableSchema,
            chunkTimestamps);
    }

private:
    const IJobSpecHelperPtr JobSpecHelper_;
    const bool UseParallelReader_;
};

////////////////////////////////////////////////////////////////////////////////

class TSortedReduceJobIOFactory
    : public IUserJobIOFactory
{
public:
    TSortedReduceJobIOFactory(IJobSpecHelperPtr jobSpecHelper, bool interruptAtKeyEdge)
        : JobSpecHelper_(jobSpecHelper)
        , InterruptAtKeyEdge_(interruptAtKeyEdge)
    { }

    virtual ISchemalessMultiChunkReaderPtr CreateReader(
        INativeClientPtr client,
        const TNodeDescriptor& nodeDescriptor,
        TClosure onNetworkReleased,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        YCHECK(nameTable->GetSize() == 0 && columnFilter.All);

        const auto& reduceJobSpecExt = JobSpecHelper_->GetJobSpec().GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        auto keyColumns = FromProto<TKeyColumns>(reduceJobSpecExt.key_columns());

        std::vector<ISchemalessMultiChunkReaderPtr> primaryReaders;
        nameTable = TNameTable::FromKeyColumns(keyColumns);
        const auto& schedulerJobSpecExt = JobSpecHelper_->GetSchedulerJobSpecExt();
        auto options = ConvertTo<TTableReaderOptionsPtr>(TYsonString(
            schedulerJobSpecExt.table_reader_options()));

        auto dataSourceDirectory = FromProto<TDataSourceDirectoryPtr>(schedulerJobSpecExt.data_source_directory());

        for (const auto& inputSpec : schedulerJobSpecExt.input_table_specs()) {
            // ToDo(psushin): validate that input chunks are sorted.
            auto dataSliceDescriptors = FromProto<std::vector<NChunkClient::TDataSliceDescriptor>>(inputSpec.data_slice_descriptors());

            auto reader = CreateSchemalessSequentialMultiReader(
                JobSpecHelper_->GetJobIOConfig()->TableReader,
                options,
                client,
                nodeDescriptor,
                GetNullBlockCache(),
                JobSpecHelper_->GetInputNodeDirectory(),
                dataSourceDirectory,
                std::move(dataSliceDescriptors),
                nameTable,
                columnFilter,
                keyColumns);

            primaryReaders.emplace_back(reader);
        }

        const auto foreignKeyColumnCount = reduceJobSpecExt.join_key_column_count();
        std::vector<ISchemalessMultiChunkReaderPtr> foreignReaders;
        keyColumns.resize(foreignKeyColumnCount);

        for (const auto& inputSpec : schedulerJobSpecExt.foreign_input_table_specs()) {
            auto dataSliceDescriptors = FromProto<std::vector<NChunkClient::TDataSliceDescriptor>>(inputSpec.data_slice_descriptors());

            auto reader = CreateSchemalessSequentialMultiReader(
                JobSpecHelper_->GetJobIOConfig()->TableReader,
                options,
                client,
                nodeDescriptor,
                GetNullBlockCache(),
                JobSpecHelper_->GetInputNodeDirectory(),
                dataSourceDirectory,
                std::move(dataSliceDescriptors),
                nameTable,
                columnFilter,
                keyColumns);

            foreignReaders.emplace_back(reader);
        }

        auto readerFactory = InterruptAtKeyEdge_ ? CreateSchemalessSortedJoiningReader : CreateSchemalessJoinReduceJoiningReader;

        const auto primaryKeyColumnCount = reduceJobSpecExt.key_columns_size();
        const auto reduceKeyColumnCount = reduceJobSpecExt.reduce_key_column_count();
        return readerFactory(primaryReaders, primaryKeyColumnCount, reduceKeyColumnCount, foreignReaders, foreignKeyColumnCount);
    }

    virtual NTableClient::ISchemalessMultiChunkWriterPtr CreateWriter(
        INativeClientPtr client,
        TTableWriterConfigPtr config,
        TTableWriterOptionsPtr options,
        const TChunkListId& chunkListId,
        const TTransactionId& transactionId,
        const TTableSchema& tableSchema,
        const TChunkTimestamps& chunkTimestamps) override
    {
        return CreateTableWriter(
            JobSpecHelper_,
            std::move(client),
            std::move(config),
            std::move(options),
            chunkListId,
            transactionId,
            tableSchema,
            chunkTimestamps);
    }

private:
    const IJobSpecHelperPtr JobSpecHelper_;
    const bool InterruptAtKeyEdge_;
};

////////////////////////////////////////////////////////////////////////////////

class TPartitionMapJobIOFactory
    : public IUserJobIOFactory
{
public:
    explicit TPartitionMapJobIOFactory(IJobSpecHelperPtr jobSpecHelper)
        : JobSpecHelper_(jobSpecHelper)
    { }

    virtual ISchemalessMultiChunkReaderPtr CreateReader(
        INativeClientPtr client,
        const TNodeDescriptor& nodeDescriptor,
        TClosure onNetworkReleased,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        // NB(psushin): don't use parallel readers here to minimize nondetermenistics
        // behaviour in mapper, that may lead to huge problems in presence of lost jobs.
        return CreateRegularReader(
            JobSpecHelper_,
            std::move(client),
            nodeDescriptor,
            false,
            std::move(nameTable),
            columnFilter);
    }

    virtual NTableClient::ISchemalessMultiChunkWriterPtr CreateWriter(
        INativeClientPtr client,
        TTableWriterConfigPtr config,
        TTableWriterOptionsPtr options,
        const TChunkListId& chunkListId,
        const TTransactionId& transactionId,
        const TTableSchema& tableSchema,
        const TChunkTimestamps& /* chunkTimestamps */) override
    {
        const auto& jobSpec = JobSpecHelper_->GetJobSpec();
        const auto& jobSpecExt = jobSpec.GetExtension(TPartitionJobSpecExt::partition_job_spec_ext);
        auto partitioner = CreateHashPartitioner(
            jobSpecExt.partition_count(),
            jobSpecExt.reduce_key_column_count());
        auto keyColumns = FromProto<TKeyColumns>(jobSpecExt.sort_key_columns());

        auto nameTable = TNameTable::FromKeyColumns(keyColumns);
        nameTable->SetEnableColumnNameValidation();

        // We pass partitioning columns through schema but input stream is not sorted.
        options->ValidateSorted = false;

        return CreatePartitionMultiChunkWriter(
            config,
            options,
            nameTable,
            TTableSchema::FromKeyColumns(keyColumns),
            std::move(client),
            CellTagFromId(chunkListId),
            transactionId,
            chunkListId,
            std::move(partitioner));
    }

private:
    const IJobSpecHelperPtr JobSpecHelper_;
};

////////////////////////////////////////////////////////////////////////////////

class TPartitionReduceJobIOFactory
    : public IUserJobIOFactory
{
public:
    explicit TPartitionReduceJobIOFactory(IJobSpecHelperPtr jobSpecHelper)
        : JobSpecHelper_(jobSpecHelper)
    { }

    virtual ISchemalessMultiChunkReaderPtr CreateReader(
        INativeClientPtr client,
        const TNodeDescriptor& nodeDescriptor,
        TClosure onNetworkReleased,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        YCHECK(nameTable->GetSize() == 0 && columnFilter.All);

        const auto& schedulerJobSpecExt = JobSpecHelper_->GetSchedulerJobSpecExt();

        YCHECK(schedulerJobSpecExt.input_table_specs_size() == 1);

        const auto& inputSpec = schedulerJobSpecExt.input_table_specs(0);
        auto dataSliceDescriptors = FromProto<std::vector<NChunkClient::TDataSliceDescriptor>>(inputSpec.data_slice_descriptors());
        auto dataSourceDirectory = FromProto<TDataSourceDirectoryPtr>(schedulerJobSpecExt.data_source_directory());

        const auto& reduceJobSpecExt = JobSpecHelper_->GetJobSpec().GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        auto keyColumns = FromProto<TKeyColumns>(reduceJobSpecExt.key_columns());
        nameTable = TNameTable::FromKeyColumns(keyColumns);

        YCHECK(reduceJobSpecExt.has_partition_tag());

        return CreateSchemalessPartitionSortReader(
            JobSpecHelper_->GetJobIOConfig()->TableReader,
            std::move(client),
            GetNullBlockCache(),
            JobSpecHelper_->GetInputNodeDirectory(),
            keyColumns,
            nameTable,
            onNetworkReleased,
            dataSourceDirectory,
            std::move(dataSliceDescriptors),
            schedulerJobSpecExt.input_row_count(),
            schedulerJobSpecExt.is_approximate(),
            reduceJobSpecExt.partition_tag());
    }

    virtual NTableClient::ISchemalessMultiChunkWriterPtr CreateWriter(
        INativeClientPtr client,
        TTableWriterConfigPtr config,
        TTableWriterOptionsPtr options,
        const TChunkListId& chunkListId,
        const TTransactionId& transactionId,
        const TTableSchema& tableSchema,
        const TChunkTimestamps& chunkTimestamps) override
    {
        return CreateTableWriter(
            JobSpecHelper_,
            std::move(client),
            std::move(config),
            std::move(options),
            chunkListId,
            transactionId,
            tableSchema,
            chunkTimestamps);
    }

private:
    const IJobSpecHelperPtr JobSpecHelper_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

IUserJobIOFactoryPtr CreateUserJobIOFactory(const IJobSpecHelperPtr& jobSpecHelper)
{
    const auto jobType = jobSpecHelper->GetJobType();
    switch (jobType) {
        case EJobType::Map:
            return New<TMapJobIOFactory>(jobSpecHelper, true);

        case EJobType::OrderedMap:
            return New<TMapJobIOFactory>(jobSpecHelper, false);

        case EJobType::SortedReduce:
            return New<TSortedReduceJobIOFactory>(jobSpecHelper, true);

        case EJobType::JoinReduce:
            return New<TSortedReduceJobIOFactory>(jobSpecHelper, false);

        case EJobType::PartitionMap:
            return New<TPartitionMapJobIOFactory>(jobSpecHelper);

        // ToDo(psushin): handle separately to form job result differently.
        case EJobType::ReduceCombiner:
        case EJobType::PartitionReduce:
            return New<TPartitionReduceJobIOFactory>(jobSpecHelper);

        default:
            THROW_ERROR_EXCEPTION(
                "Job has an invalid type %Qlv while a user job is expected",
                jobType);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
