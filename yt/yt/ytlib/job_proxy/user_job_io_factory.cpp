#include "user_job_io_factory.h"

#include "job_spec_helper.h"
#include "helpers.h"

#include <yt/yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_options.h>
#include <yt/yt/ytlib/chunk_client/data_slice_descriptor.h>
#include <yt/yt/ytlib/chunk_client/data_source.h>
#include <yt/yt/ytlib/chunk_client/dispatcher.h>
#include <yt/yt/ytlib/chunk_client/parallel_reader_memory_manager.h>

#include <yt/yt/ytlib/job_tracker_client/proto/job.pb.h>
#include <yt/yt/ytlib/job_tracker_client/public.h>

#include <yt/yt/ytlib/scheduler/proto/job.pb.h>

#include <yt/yt/ytlib/table_client/partitioner.h>
#include <yt/yt/ytlib/table_client/partition_sort_reader.h>
#include <yt/yt/ytlib/table_client/schemaless_multi_chunk_reader.h>
#include <yt/yt/ytlib/table_client/schemaless_chunk_writer.h>
#include <yt/yt/ytlib/table_client/sorted_merging_reader.h>

#include <yt/yt/client/api/public.h>

#include <yt/yt_proto/yt/client/chunk_client/proto/chunk_spec.pb.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/table_client/column_sort_schema.h>
#include <yt/yt/client/table_client/name_table.h>

#include <yt/yt/core/ytree/convert.h>

#include <vector>

namespace NYT::NJobProxy {

using namespace NApi;
using namespace NChunkClient;
using namespace NJobTrackerClient;
using namespace NJobTrackerClient::NProto;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NScheduler::NProto;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NYson;
using namespace NYTree;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

namespace {

ISchemalessMultiChunkWriterPtr CreateTableWriter(
    NNative::IClientPtr client,
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    TString localHostName,
    TChunkListId chunkListId,
    TTransactionId transactionId,
    TTableSchemaPtr tableSchema,
    const TChunkTimestamps& chunkTimestamps,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr throttler,
    const std::optional<TDataSink>& dataSink)
{
    auto nameTable = New<TNameTable>();
    nameTable->SetEnableColumnNameValidation();

    return CreateSchemalessMultiChunkWriter(
        std::move(config),
        std::move(options),
        std::move(nameTable),
        std::move(tableSchema),
        TLegacyOwningKey(),
        std::move(client),
        std::move(localHostName),
        CellTagFromId(chunkListId),
        transactionId,
        dataSink,
        chunkListId,
        chunkTimestamps,
        std::move(trafficMeter),
        std::move(throttler));
}

ISchemalessMultiChunkReaderPtr CreateRegularReader(
    const IJobSpecHelperPtr& jobSpecHelper,
    NNative::IClientPtr client,
    const TNodeDescriptor& nodeDescriptor,
    bool isParallel,
    TNameTablePtr nameTable,
    const TColumnFilter& columnFilter,
    const TClientChunkReadOptions& chunkReadOptions,
    IBlockCachePtr blockCache,
    IClientChunkMetaCachePtr chunkMetaCache,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler,
    IMultiReaderMemoryManagerPtr multiReaderMemoryManager)
{
    const auto& schedulerJobSpecExt = jobSpecHelper->GetSchedulerJobSpecExt();
    std::vector<TDataSliceDescriptor> dataSliceDescriptors;
    for (const auto& inputSpec : schedulerJobSpecExt.input_table_specs()) {
        auto descriptors = UnpackDataSliceDescriptors(inputSpec);
        dataSliceDescriptors.insert(dataSliceDescriptors.end(), descriptors.begin(), descriptors.end());
    }

    auto dataSourceDirectory = jobSpecHelper->GetDataSourceDirectory();

    auto options = ConvertTo<TTableReaderOptionsPtr>(TYsonString(schedulerJobSpecExt.table_reader_options()));

    auto createReader = isParallel
        ? CreateSchemalessParallelMultiReader
        : CreateSchemalessSequentialMultiReader;
    const auto& tableReaderConfig = jobSpecHelper->GetJobIOConfig()->TableReader;
    return createReader(
        tableReaderConfig,
        std::move(options),
        std::move(client),
        nodeDescriptor,
        std::move(blockCache),
        std::move(chunkMetaCache),
        dataSourceDirectory,
        std::move(dataSliceDescriptors),
        std::move(nameTable),
        chunkReadOptions,
        columnFilter,
        /*partitionTag*/ std::nullopt,
        std::move(trafficMeter),
        std::move(bandwidthThrottler),
        std::move(rpsThrottler),
        multiReaderMemoryManager->CreateMultiReaderMemoryManager(tableReaderConfig->MaxBufferSize),
        /*interruptDescriptorKeyLength*/ 0);
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

struct TUserJobIOFactoryBase
    : public IUserJobIOFactory
{
    TUserJobIOFactoryBase(
        IJobSpecHelperPtr jobSpecHelper,
        const TClientChunkReadOptions& chunkReadOptions,
        TString localHostName,
        IBlockCachePtr blockCache,
        IClientChunkMetaCachePtr chunkMetaCache,
        TTrafficMeterPtr trafficMeter,
        IThroughputThrottlerPtr inBandwidthThrottler,
        IThroughputThrottlerPtr outBandwidthThrottler,
        IThroughputThrottlerPtr outRpsThrottler)
        : JobSpecHelper_(std::move(jobSpecHelper))
        , ChunkReadOptions_(chunkReadOptions)
        , LocalHostName_(std::move(localHostName))
        , BlockCache_(std::move(blockCache))
        , ChunkMetaCache_(std::move(chunkMetaCache))
        , TrafficMeter_(std::move(trafficMeter))
        , InBandwidthThrottler_(std::move(inBandwidthThrottler))
        , OutBandwidthThrottler_(std::move(outBandwidthThrottler))
        , OutRpsThrottler_(std::move(outRpsThrottler))
    { }

    void Initialize()
    {
        // Initialize parallel reader memory manager.
        {
            auto totalReaderMemoryLimit = GetTotalReaderMemoryLimit();
            TParallelReaderMemoryManagerOptions parallelReaderMemoryManagerOptions{
                .TotalReservedMemorySize = totalReaderMemoryLimit,
                .MaxInitialReaderReservedMemory = totalReaderMemoryLimit
            };
            MultiReaderMemoryManager_ = CreateParallelReaderMemoryManager(
                parallelReaderMemoryManagerOptions,
                NChunkClient::TDispatcher::Get()->GetReaderMemoryManagerInvoker());
        }
    }

    ISchemalessMultiChunkWriterPtr CreateWriter(
        NApi::NNative::IClientPtr client,
        TTableWriterConfigPtr config,
        TTableWriterOptionsPtr options,
        TChunkListId chunkListId,
        TTransactionId transactionId,
        TTableSchemaPtr tableSchema,
        const TChunkTimestamps& chunkTimestamps,
        const std::optional<TDataSink>& dataSink) override
    {
        return CreateTableWriter(
            std::move(client),
            std::move(config),
            std::move(options),
            LocalHostName_,
            chunkListId,
            transactionId,
            std::move(tableSchema),
            chunkTimestamps,
            TrafficMeter_,
            OutBandwidthThrottler_,
            dataSink);
    }

protected:
    const IJobSpecHelperPtr JobSpecHelper_;
    const TClientChunkReadOptions ChunkReadOptions_;
    const TString LocalHostName_;
    const IBlockCachePtr BlockCache_;
    const IClientChunkMetaCachePtr ChunkMetaCache_;
    const TTrafficMeterPtr TrafficMeter_;
    const IThroughputThrottlerPtr InBandwidthThrottler_;
    const IThroughputThrottlerPtr OutBandwidthThrottler_;
    const IThroughputThrottlerPtr OutRpsThrottler_;
    IMultiReaderMemoryManagerPtr MultiReaderMemoryManager_;

    virtual i64 GetTotalReaderMemoryLimit() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TMapJobIOFactory
    : public TUserJobIOFactoryBase
{
public:
    TMapJobIOFactory(
        IJobSpecHelperPtr jobSpecHelper,
        bool useParallelReader,
        const TClientChunkReadOptions& chunkReadOptions,
        TString localHostName,
        IBlockCachePtr blockCache,
        IClientChunkMetaCachePtr chunkMetaCache,
        TTrafficMeterPtr trafficMeter,
        IThroughputThrottlerPtr inBandwidthThrottler,
        IThroughputThrottlerPtr outBandwidthThrottler,
        IThroughputThrottlerPtr outRpsThrottler)
        : TUserJobIOFactoryBase(
            std::move(jobSpecHelper),
            chunkReadOptions,
            std::move(localHostName),
            std::move(blockCache),
            std::move(chunkMetaCache),
            std::move(trafficMeter),
            std::move(inBandwidthThrottler),
            std::move(outBandwidthThrottler),
            std::move(outRpsThrottler))
        , UseParallelReader_(useParallelReader)
    {
        TUserJobIOFactoryBase::Initialize();
    }

    ISchemalessMultiChunkReaderPtr CreateReader(
        NNative::IClientPtr client,
        const TNodeDescriptor& nodeDescriptor,
        TClosure /*onNetworkReleased*/,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        return CreateRegularReader(
            JobSpecHelper_,
            std::move(client),
            nodeDescriptor,
            UseParallelReader_,
            std::move(nameTable),
            columnFilter,
            ChunkReadOptions_,
            BlockCache_,
            ChunkMetaCache_,
            TrafficMeter_,
            InBandwidthThrottler_,
            OutRpsThrottler_,
            MultiReaderMemoryManager_);
    }

protected:
    i64 GetTotalReaderMemoryLimit() const override
    {
        return JobSpecHelper_->GetJobIOConfig()->TableReader->MaxBufferSize;
    }

private:
    const bool UseParallelReader_;
};

////////////////////////////////////////////////////////////////////////////////

class TSortedReduceJobIOFactory
    : public TUserJobIOFactoryBase
{
public:
    TSortedReduceJobIOFactory(
        IJobSpecHelperPtr jobSpecHelper,
        bool interruptAtKeyEdge,
        const TClientChunkReadOptions& chunkReadOptions,
        TString localHostName,
        IBlockCachePtr blockCache,
        IClientChunkMetaCachePtr chunkMetaCache,
        TTrafficMeterPtr trafficMeter,
        IThroughputThrottlerPtr inBandwidthThrottler,
        IThroughputThrottlerPtr outBandwidthThrottler,
        IThroughputThrottlerPtr outRpsThrottler)
        : TUserJobIOFactoryBase(
            std::move(jobSpecHelper),
            chunkReadOptions,
            std::move(localHostName),
            std::move(blockCache),
            std::move(chunkMetaCache),
            std::move(trafficMeter),
            std::move(inBandwidthThrottler),
            std::move(outBandwidthThrottler),
            std::move(outRpsThrottler))
        , InterruptAtKeyEdge_(interruptAtKeyEdge)
    {
        TUserJobIOFactoryBase::Initialize();
    }

    ISchemalessMultiChunkReaderPtr CreateReader(
        NNative::IClientPtr client,
        const TNodeDescriptor& nodeDescriptor,
        TClosure /*onNetworkReleased*/,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        YT_VERIFY(nameTable->GetSize() == 0 && columnFilter.IsUniversal());

        const auto& reduceJobSpecExt = JobSpecHelper_->GetJobSpec().GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        auto keyColumns = FromProto<TKeyColumns>(reduceJobSpecExt.key_columns());
        auto sortColumns = FromProto<TSortColumns>(reduceJobSpecExt.sort_columns());

        // COMPAT(gritukan)
        if (sortColumns.empty()) {
            for (const auto& keyColumn : keyColumns) {
                sortColumns.push_back({keyColumn, ESortOrder::Ascending});
            }
        }

        std::vector<ISchemalessMultiChunkReaderPtr> primaryReaders;
        nameTable = TNameTable::FromSortColumns(sortColumns);
        const auto& schedulerJobSpecExt = JobSpecHelper_->GetSchedulerJobSpecExt();
        auto options = ConvertTo<TTableReaderOptionsPtr>(TYsonString(
            schedulerJobSpecExt.table_reader_options()));

        // We must always enable table index to merge rows with the same index in the proper order.
        options->EnableTableIndex = true;

        auto dataSourceDirectory = JobSpecHelper_->GetDataSourceDirectory();

        for (const auto& inputSpec : schedulerJobSpecExt.input_table_specs()) {
            // ToDo(psushin): validate that input chunks are sorted.
            auto dataSliceDescriptors = UnpackDataSliceDescriptors(inputSpec);

            const auto& tableReaderConfig = JobSpecHelper_->GetJobIOConfig()->TableReader;
            auto reader = CreateSchemalessSequentialMultiReader(
                tableReaderConfig,
                options,
                client,
                nodeDescriptor,
                BlockCache_,
                ChunkMetaCache_,
                dataSourceDirectory,
                std::move(dataSliceDescriptors),
                nameTable,
                ChunkReadOptions_,
                columnFilter,
                /* partitionTag */ std::nullopt,
                TrafficMeter_,
                InBandwidthThrottler_,
                OutRpsThrottler_,
                MultiReaderMemoryManager_->CreateMultiReaderMemoryManager(tableReaderConfig->MaxBufferSize),
                sortColumns.size());

            primaryReaders.emplace_back(reader);
        }

        const auto foreignKeyColumnCount = reduceJobSpecExt.join_key_column_count();
        std::vector<ISchemalessMultiChunkReaderPtr> foreignReaders;

        for (const auto& inputSpec : schedulerJobSpecExt.foreign_input_table_specs()) {
            auto dataSliceDescriptors = UnpackDataSliceDescriptors(inputSpec);

            const auto& tableReaderConfig = JobSpecHelper_->GetJobIOConfig()->TableReader;
            auto reader = CreateSchemalessSequentialMultiReader(
                tableReaderConfig,
                options,
                client,
                nodeDescriptor,
                BlockCache_,
                ChunkMetaCache_,
                dataSourceDirectory,
                std::move(dataSliceDescriptors),
                nameTable,
                ChunkReadOptions_,
                columnFilter,
                /* partitionTag */ std::nullopt,
                TrafficMeter_,
                InBandwidthThrottler_,
                OutRpsThrottler_,
                MultiReaderMemoryManager_->CreateMultiReaderMemoryManager(tableReaderConfig->MaxBufferSize));

            foreignReaders.emplace_back(reader);
        }

        auto sortComparator = GetComparator(sortColumns);
        auto reduceComparator = sortComparator.Trim(reduceJobSpecExt.reduce_key_column_count());
        auto joinComparator = sortComparator.Trim(foreignKeyColumnCount);
        return CreateSortedJoiningReader(
            primaryReaders,
            sortComparator,
            reduceComparator,
            foreignReaders,
            joinComparator,
            InterruptAtKeyEdge_);
    }

protected:
    i64 GetTotalReaderMemoryLimit() const override
    {
        auto readerMemoryLimit = JobSpecHelper_->GetJobIOConfig()->TableReader->MaxBufferSize;
        const auto& schedulerJobSpecExt = JobSpecHelper_->GetSchedulerJobSpecExt();
        auto readerCount = schedulerJobSpecExt.input_table_specs_size() + schedulerJobSpecExt.foreign_input_table_specs_size();
        return readerMemoryLimit * readerCount;
    }

private:
    const bool InterruptAtKeyEdge_;
};

////////////////////////////////////////////////////////////////////////////////

class TPartitionMapJobIOFactory
    : public TUserJobIOFactoryBase
{
public:
    explicit TPartitionMapJobIOFactory(
        IJobSpecHelperPtr jobSpecHelper,
        const TClientChunkReadOptions& chunkReadOptions,
        TString localHostName,
        IBlockCachePtr blockCache,
        IClientChunkMetaCachePtr chunkMetaCache,
        TTrafficMeterPtr trafficMeter,
        IThroughputThrottlerPtr inBandwidthThrottler,
        IThroughputThrottlerPtr outBandwidthThrottler,
        IThroughputThrottlerPtr outRpsThrottler)
        : TUserJobIOFactoryBase(
            std::move(jobSpecHelper),
            chunkReadOptions,
            std::move(localHostName),
            std::move(blockCache),
            std::move(chunkMetaCache),
            std::move(trafficMeter),
            std::move(inBandwidthThrottler),
            std::move(outBandwidthThrottler),
            std::move(outRpsThrottler))
    {
        TUserJobIOFactoryBase::Initialize();
    }

    ISchemalessMultiChunkReaderPtr CreateReader(
        NNative::IClientPtr client,
        const TNodeDescriptor& nodeDescriptor,
        TClosure /*onNetworkReleased*/,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        const auto& partitionJobSpecExt = JobSpecHelper_->GetJobSpec().GetExtension(TPartitionJobSpecExt::partition_job_spec_ext);

        return CreateRegularReader(
            JobSpecHelper_,
            std::move(client),
            nodeDescriptor,
            /*isParallel*/ !partitionJobSpecExt.use_sequential_reader(),
            std::move(nameTable),
            columnFilter,
            ChunkReadOptions_,
            BlockCache_,
            ChunkMetaCache_,
            TrafficMeter_,
            InBandwidthThrottler_,
            OutRpsThrottler_,
            MultiReaderMemoryManager_);
    }

    ISchemalessMultiChunkWriterPtr CreateWriter(
        NNative::IClientPtr client,
        TTableWriterConfigPtr config,
        TTableWriterOptionsPtr options,
        TChunkListId chunkListId,
        TTransactionId transactionId,
        TTableSchemaPtr tableSchema,
        const TChunkTimestamps& chunkTimestamps,
        const std::optional<TDataSink>& dataSink) override
    {
        const auto& jobSpec = JobSpecHelper_->GetJobSpec();
        const auto& jobSpecExt = jobSpec.GetExtension(TPartitionJobSpecExt::partition_job_spec_ext);
        auto partitioner = CreatePartitioner(jobSpecExt);

        // We pass partitioning columns through schema but input stream is not sorted.
        options->ValidateSorted = false;

        // TODO(max42): currently ReturnBoundaryKeys are set exactly for the writers
        // that correspond to the map-sink edge. Think more about how this may be done properly.
        if (!options->ReturnBoundaryKeys) {
            auto keyColumns = FromProto<TKeyColumns>(jobSpecExt.sort_key_columns());
            auto sortColumns = FromProto<TSortColumns>(jobSpecExt.sort_columns());
            // COMPAT(gritukan)
            if (sortColumns.empty()) {
                for (const auto& keyColumn : keyColumns) {
                    sortColumns.push_back(TColumnSortSchema{
                        .Name = keyColumn,
                        .SortOrder = ESortOrder::Ascending
                    });
                }
            }

            auto nameTable = TNameTable::FromKeyColumns(keyColumns);
            nameTable->SetEnableColumnNameValidation();
            if (tableSchema->Columns().empty()) {
                tableSchema = TTableSchema::FromSortColumns(sortColumns);
            }

            // This writer is used for partitioning.
            return CreatePartitionMultiChunkWriter(
                std::move(config),
                std::move(options),
                std::move(nameTable),
                std::move(tableSchema),
                std::move(client),
                LocalHostName_,
                CellTagFromId(chunkListId),
                transactionId,
                chunkListId,
                std::move(partitioner),
                dataSink,
                TrafficMeter_,
                OutBandwidthThrottler_);
        } else {
            // This writer is used for mapper output tables.
            return CreateTableWriter(
                std::move(client),
                std::move(config),
                std::move(options),
                LocalHostName_,
                chunkListId,
                transactionId,
                std::move(tableSchema),
                chunkTimestamps,
                TrafficMeter_,
                OutBandwidthThrottler_,
                dataSink);
        }
    }

protected:
    i64 GetTotalReaderMemoryLimit() const override
    {
        return JobSpecHelper_->GetJobIOConfig()->TableReader->MaxBufferSize;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TPartitionReduceJobIOFactory
    : public TUserJobIOFactoryBase
{
public:
    TPartitionReduceJobIOFactory(
        IJobSpecHelperPtr jobSpecHelper,
        const TClientChunkReadOptions& chunkReadOptions,
        TString localHostName,
        IBlockCachePtr blockCache,
        IClientChunkMetaCachePtr chunkMetaCache,
        TTrafficMeterPtr trafficMeter,
        IThroughputThrottlerPtr inBandwidthThrottler,
        IThroughputThrottlerPtr outBandwidthThrottler,
        IThroughputThrottlerPtr outRpsThrottler)
        : TUserJobIOFactoryBase(
            std::move(jobSpecHelper),
            chunkReadOptions,
            std::move(localHostName),
            std::move(blockCache),
            std::move(chunkMetaCache),
            std::move(trafficMeter),
            std::move(inBandwidthThrottler),
            std::move(outBandwidthThrottler),
            std::move(outRpsThrottler))
    {
        TUserJobIOFactoryBase::Initialize();
    }

    ISchemalessMultiChunkReaderPtr CreateReader(
        NNative::IClientPtr client,
        const TNodeDescriptor& /*nodeDescriptor*/,
        TClosure onNetworkReleased,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter) override
    {
        YT_VERIFY(nameTable->GetSize() == 0 && columnFilter.IsUniversal());

        const auto& schedulerJobSpecExt = JobSpecHelper_->GetSchedulerJobSpecExt();

        YT_VERIFY(schedulerJobSpecExt.input_table_specs_size() == 1);

        const auto& inputSpec = schedulerJobSpecExt.input_table_specs(0);
        auto dataSliceDescriptors = UnpackDataSliceDescriptors(inputSpec);
        auto dataSourceDirectory = JobSpecHelper_->GetDataSourceDirectory();

        const auto& reduceJobSpecExt = JobSpecHelper_->GetJobSpec().GetExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        auto keyColumns = FromProto<TKeyColumns>(reduceJobSpecExt.key_columns());
        auto sortColumns = FromProto<TSortColumns>(reduceJobSpecExt.sort_columns());

        // COMPAT(gritukan)
        if (sortColumns.empty()) {
            for (const auto& keyColumn : keyColumns) {
                sortColumns.push_back({keyColumn, ESortOrder::Ascending});
            }
        }

        nameTable = TNameTable::FromKeyColumns(keyColumns);

        std::optional<int> partitionTag;
        if (schedulerJobSpecExt.has_partition_tag()) {
            partitionTag = schedulerJobSpecExt.partition_tag();
        } else if (reduceJobSpecExt.has_partition_tag()) {
            partitionTag = reduceJobSpecExt.partition_tag();
        }
        YT_VERIFY(partitionTag);

        auto comparator = GetComparator(FromProto<TSortColumns>(reduceJobSpecExt.sort_columns()));

        return CreatePartitionSortReader(
            JobSpecHelper_->GetJobIOConfig()->TableReader,
            std::move(client),
            BlockCache_,
            ChunkMetaCache_,
            GetComparator(sortColumns),
            nameTable,
            onNetworkReleased,
            dataSourceDirectory,
            std::move(dataSliceDescriptors),
            schedulerJobSpecExt.input_row_count(),
            schedulerJobSpecExt.is_approximate(),
            *partitionTag,
            ChunkReadOptions_,
            TrafficMeter_,
            InBandwidthThrottler_,
            OutRpsThrottler_,
            MultiReaderMemoryManager_);
    }

protected:
    i64 GetTotalReaderMemoryLimit() const override
    {
        return JobSpecHelper_->GetJobIOConfig()->TableReader->MaxBufferSize;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TVanillaJobIOFactory
    : public TUserJobIOFactoryBase
{
public:
    TVanillaJobIOFactory(
        IJobSpecHelperPtr jobSpecHelper,
        const TClientChunkReadOptions& chunkReadOptions,
        TString localHostName,
        IBlockCachePtr blockCache,
        IClientChunkMetaCachePtr chunkMetaCache,
        TTrafficMeterPtr trafficMeter,
        IThroughputThrottlerPtr inBandwidthThrottler,
        IThroughputThrottlerPtr outBandwidthThrottler,
        IThroughputThrottlerPtr outRpsThrottler)
        : TUserJobIOFactoryBase(
            std::move(jobSpecHelper),
            chunkReadOptions,
            std::move(localHostName),
            std::move(blockCache),
            std::move(chunkMetaCache),
            std::move(trafficMeter),
            std::move(inBandwidthThrottler),
            std::move(outBandwidthThrottler),
            std::move(outRpsThrottler))
    { }

    ISchemalessMultiChunkReaderPtr CreateReader(
        NNative::IClientPtr /*client*/,
        const TNodeDescriptor& /*nodeDescriptor*/,
        TClosure /*onNetworkReleased*/,
        TNameTablePtr /*nameTable*/,
        const TColumnFilter& /*columnFilter*/) override
    {
        return nullptr;
    }

protected:
    i64 GetTotalReaderMemoryLimit() const override
    {
        return 0;
    }
};

////////////////////////////////////////////////////////////////////////////////

IUserJobIOFactoryPtr CreateUserJobIOFactory(
    const IJobSpecHelperPtr& jobSpecHelper,
    const TClientChunkReadOptions& chunkReadOptions,
    TString localHostName,
    IBlockCachePtr blockCache,
    IClientChunkMetaCachePtr chunkMetaCache,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr inBandwidthThrottler,
    IThroughputThrottlerPtr outBandwidthThrottler,
    IThroughputThrottlerPtr outRpsThrottler)
{
    const auto jobType = jobSpecHelper->GetJobType();
    switch (jobType) {
        case EJobType::Map:
            return New<TMapJobIOFactory>(
                jobSpecHelper,
                true,
                chunkReadOptions,
                std::move(localHostName),
                std::move(blockCache),
                std::move(chunkMetaCache),
                std::move(trafficMeter),
                std::move(inBandwidthThrottler),
                std::move(outBandwidthThrottler),
                std::move(outRpsThrottler));

        case EJobType::OrderedMap:
            return New<TMapJobIOFactory>(
                jobSpecHelper,
                false,
                chunkReadOptions,
                std::move(localHostName),
                std::move(blockCache),
                std::move(chunkMetaCache),
                std::move(trafficMeter),
                std::move(inBandwidthThrottler),
                std::move(outBandwidthThrottler),
                std::move(outRpsThrottler));

        case EJobType::SortedReduce:
            return New<TSortedReduceJobIOFactory>(
                jobSpecHelper,
                true,
                chunkReadOptions,
                std::move(localHostName),
                std::move(blockCache),
                std::move(chunkMetaCache),
                std::move(trafficMeter),
                std::move(inBandwidthThrottler),
                std::move(outBandwidthThrottler),
                std::move(outRpsThrottler));

        case EJobType::JoinReduce:
            return New<TSortedReduceJobIOFactory>(
                jobSpecHelper,
                false,
                chunkReadOptions,
                std::move(localHostName),
                std::move(blockCache),
                std::move(chunkMetaCache),
                std::move(trafficMeter),
                std::move(inBandwidthThrottler),
                std::move(outBandwidthThrottler),
                std::move(outRpsThrottler));

        case EJobType::PartitionMap:
            return New<TPartitionMapJobIOFactory>(
                jobSpecHelper,
                chunkReadOptions,
                std::move(localHostName),
                std::move(blockCache),
                std::move(chunkMetaCache),
                std::move(trafficMeter),
                std::move(inBandwidthThrottler),
                std::move(outBandwidthThrottler),
                std::move(outRpsThrottler));

        // ToDo(psushin): handle separately to form job result differently.
        case EJobType::ReduceCombiner:
        case EJobType::PartitionReduce:
            return New<TPartitionReduceJobIOFactory>(
                jobSpecHelper,
                chunkReadOptions,
                std::move(localHostName),
                std::move(blockCache),
                std::move(chunkMetaCache),
                std::move(trafficMeter),
                std::move(inBandwidthThrottler),
                std::move(outBandwidthThrottler),
                std::move(outRpsThrottler));

        case EJobType::Vanilla:
            return New<TVanillaJobIOFactory>(
                jobSpecHelper,
                chunkReadOptions,
                std::move(localHostName),
                std::move(blockCache),
                std::move(chunkMetaCache),
                std::move(trafficMeter),
                std::move(inBandwidthThrottler),
                std::move(outBandwidthThrottler),
                std::move(outRpsThrottler));

        default:
            THROW_ERROR_EXCEPTION(
                "Job has an invalid type %Qlv while a user job is expected",
                jobType);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
