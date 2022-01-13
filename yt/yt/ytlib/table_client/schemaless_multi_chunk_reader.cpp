#include "schemaless_multi_chunk_reader.h"

#include "cached_versioned_chunk_meta.h"
#include "chunk_reader_base.h"
#include "chunk_state.h"
#include "columnar_chunk_reader_base.h"
#include "config.h"
#include "helpers.h"
#include "hunks.h"
#include "overlapping_reader.h"
#include "private.h"
#include "row_merger.h"
#include "schemaless_block_reader.h"
#include "schemaless_multi_chunk_reader.h"
#include "table_read_spec.h"
#include "versioned_chunk_reader.h"
#include "remote_dynamic_store_reader.h"

#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/ytlib/table_chunk_format/public.h>
#include <yt/yt/ytlib/table_chunk_format/column_reader.h>
#include <yt/yt/ytlib/table_chunk_format/null_column_reader.h>

#include <yt/yt/ytlib/chunk_client/chunk_fragment_reader.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/yt/ytlib/chunk_client/data_source.h>
#include <yt/yt/ytlib/chunk_client/dispatcher.h>
#include <yt/yt/ytlib/chunk_client/helpers.h>
#include <yt/yt/ytlib/chunk_client/multi_reader_manager.h>
#include <yt/yt/ytlib/chunk_client/parallel_reader_memory_manager.h>
#include <yt/yt/ytlib/chunk_client/reader_factory.h>
#include <yt/yt/ytlib/chunk_client/replication_reader.h>

#include <yt/yt/ytlib/node_tracker_client/node_status_directory.h>

#include <yt/yt/ytlib/tablet_client/helpers.h>

#include <yt/yt/ytlib/query_client/column_evaluator.h>

#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/name_table.h>
#include <yt/yt/client/table_client/row_base.h>
#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/unversioned_reader.h>
#include <yt/yt/client/table_client/versioned_reader.h>
#include <yt/yt/client/table_client/row_batch.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/concurrency/scheduler.h>
#include <yt/yt/core/concurrency/action_queue.h>

#include <yt/yt/core/misc/protobuf_helpers.h>
#include <yt/yt/core/misc/numeric_helpers.h>

namespace NYT::NTableClient {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NProfiling;
using namespace NTabletClient;
using namespace NTableChunkFormat;
using namespace NTableChunkFormat::NProto;
using namespace NYPath;
using namespace NYTree;
using namespace NRpc;
using namespace NApi;
using namespace NLogging;

using NChunkClient::TDataSliceDescriptor;
using NChunkClient::TReadLimit;
using NChunkClient::TReadRange;
using NChunkClient::NProto::TMiscExt;
using NChunkClient::TChunkReaderStatistics;

using NYT::FromProto;
using NYT::TRange;

////////////////////////////////////////////////////////////////////////////////

namespace {

////////////////////////////////////////////////////////////////////////////////

TFuture<TColumnarChunkMetaPtr> DownloadChunkMeta(
    IChunkReaderPtr chunkReader,
    const TClientChunkReadOptions& chunkReadOptions,
    std::optional<int> partitionTag)
{
    // Download chunk meta.
    static const std::vector<int> ExtensionTags{
        TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value,
        TProtoExtensionTag<NProto::TTableSchemaExt>::Value,
        TProtoExtensionTag<NProto::TBlockMetaExt>::Value,
        TProtoExtensionTag<NProto::TColumnMetaExt>::Value,
        TProtoExtensionTag<NProto::TNameTableExt>::Value,
        TProtoExtensionTag<NProto::TKeyColumnsExt>::Value
    };

    return chunkReader->GetMeta(
        chunkReadOptions,
        partitionTag,
        ExtensionTags)
        .Apply(BIND([] (const TRefCountedChunkMetaPtr& chunkMeta) {
            return New<TColumnarChunkMeta>(*chunkMeta);
        }));
}

TChunkReaderConfigPtr PatchConfig(TChunkReaderConfigPtr config, i64 memoryEstimate)
{
    if (memoryEstimate > config->WindowSize + config->GroupSize) {
        return config;
    }

    auto newConfig = CloneYsonSerializable(config);
    newConfig->WindowSize = std::max(memoryEstimate / 2, (i64) 1);
    newConfig->GroupSize = std::max(memoryEstimate / 2, (i64) 1);
    return newConfig;
}

std::vector<IReaderFactoryPtr> CreateReaderFactories(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    NNative::IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    std::optional<TNodeId> localNodeId,
    IBlockCachePtr blockCache,
    IClientChunkMetaCachePtr chunkMetaCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
    TNameTablePtr nameTable,
    const TClientChunkReadOptions& chunkReadOptions,
    const TColumnFilter& columnFilter,
    const TSortColumns& sortColumns,
    std::optional<int> partitionTag,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler,
    IMultiReaderMemoryManagerPtr multiReaderMemoryManager,
    int interruptDescriptorKeyLength)
{
    // TODO(gritukan): Pass chunk fragment reader config and batch hunk reader config from controller.
    auto nodeStatusDirectory = CreateTrivialNodeStatusDirectory();
    auto chunkFragmentReaderConfig = New<TChunkFragmentReaderConfig>();
    chunkFragmentReaderConfig->Postprocess();
    auto chunkFragmentReader = CreateChunkFragmentReader(
        std::move(chunkFragmentReaderConfig),
        client,
        std::move(nodeStatusDirectory),
        /*profiler*/ {});

    std::vector<IReaderFactoryPtr> factories;
    for (const auto& dataSliceDescriptor : dataSliceDescriptors) {
        const auto& dataSource = dataSourceDirectory->DataSources()[dataSliceDescriptor.GetDataSourceIndex()];

        auto wrapReader = [=] (ISchemalessChunkReaderPtr chunkReader) {
            return CreateHunkDecodingSchemalessChunkReader(
                New<TBatchHunkReaderConfig>(),
                std::move(chunkReader),
                chunkFragmentReader,
                dataSource.Schema(),
                chunkReadOptions);
        };

        switch (dataSource.GetType()) {
            case EDataSourceType::UnversionedTable: {
                const auto& chunkSpec = dataSliceDescriptor.GetSingleChunk();

                // TODO(ifsmirnov): estimate reader memory for dynamic stores.
                auto memoryEstimate = GetChunkReaderMemoryEstimate(chunkSpec, config);

                auto createChunkReaderFromSpecAsync = BIND([=] (
                    const TChunkSpec& chunkSpec,
                    TChunkReaderMemoryManagerPtr chunkReaderMemoryManager)
                {
                    IChunkReaderPtr remoteReader;
                    try {
                        remoteReader = CreateRemoteReader(
                            chunkSpec,
                            config,
                            options,
                            client,
                            nodeDirectory,
                            localDescriptor,
                            localNodeId,
                            blockCache,
                            chunkMetaCache,
                            trafficMeter,
                            /* nodeStatusDirectory */ nullptr,
                            bandwidthThrottler,
                            rpsThrottler);
                    } catch (const std::exception& ex) {
                        return MakeFuture<ISchemalessChunkReaderPtr>(ex);
                    }

                    auto asyncChunkMeta = DownloadChunkMeta(remoteReader, chunkReadOptions, partitionTag);

                    return asyncChunkMeta.Apply(BIND([=] (const TColumnarChunkMetaPtr& chunkMeta) {
                        TReadRange readRange;
                        // TODO(gritukan): Rethink it after YT-14154.
                        int keyColumnCount = std::max<int>(sortColumns.size(), chunkMeta->GetChunkSchema()->GetKeyColumnCount());
                        if (chunkSpec.has_lower_limit()) {
                            FromProto(&readRange.LowerLimit(), chunkSpec.lower_limit(), /* isUpper */ false, keyColumnCount);
                        }
                        if (chunkSpec.has_upper_limit()) {
                            FromProto(&readRange.UpperLimit(), chunkSpec.upper_limit(), /* isUpper */ true, keyColumnCount);
                        }

                        chunkMeta->RenameColumns(dataSource.ColumnRenameDescriptors());

                        auto chunkState = New<TChunkState>(
                            blockCache,
                            chunkSpec,
                            /*chunkMeta*/ nullptr,
                            NullTimestamp,
                            /*lookupHashTable*/ nullptr,
                            /*performanceCounters*/ nullptr,
                            /*keyComparer*/ nullptr,
                            dataSource.GetVirtualValueDirectory(),
                            /*tableSchema*/ nullptr);
                        chunkState->DataSource = dataSource;

                        return CreateSchemalessRangeChunkReader(
                            std::move(chunkState),
                            std::move(chunkMeta),
                            PatchConfig(config, memoryEstimate),
                            options,
                            remoteReader,
                            nameTable,
                            chunkReadOptions,
                            sortColumns,
                            dataSource.OmittedInaccessibleColumns(),
                            columnFilter.IsUniversal() ? CreateColumnFilter(dataSource.Columns(), nameTable) : columnFilter,
                            readRange,
                            partitionTag,
                            chunkReaderMemoryManager
                                ? chunkReaderMemoryManager
                                : multiReaderMemoryManager->CreateChunkReaderMemoryManager(memoryEstimate),
                            dataSliceDescriptor.VirtualRowIndex,
                            interruptDescriptorKeyLength);
                    }));
                });

                auto createReader = BIND([=] {
                    auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
                    if (TypeFromId(chunkId) == EObjectType::OrderedDynamicTabletStore) {
                        return MakeFuture<IReaderBasePtr>(CreateRetryingRemoteOrderedDynamicStoreReader(
                            chunkSpec,
                            dataSource.Schema(),
                            config->DynamicStoreReader,
                            options,
                            nameTable,
                            client,
                            nodeDirectory,
                            trafficMeter,
                            bandwidthThrottler,
                            rpsThrottler,
                            chunkReadOptions,
                            dataSource.Columns(),
                            multiReaderMemoryManager->CreateChunkReaderMemoryManager(
                                DefaultRemoteDynamicStoreReaderMemoryEstimate),
                            createChunkReaderFromSpecAsync));
                    }

                    return createChunkReaderFromSpecAsync(chunkSpec, nullptr).Apply(
                        BIND([] (const ISchemalessChunkReaderPtr& reader) -> IReaderBasePtr {
                            return reader;
                        })
                    );
                });

                auto canCreateReader = BIND([=] {
                    return multiReaderMemoryManager->GetFreeMemorySize() >= memoryEstimate;
                });

                factories.push_back(CreateReaderFactory(createReader, canCreateReader, dataSliceDescriptor));
                break;
            }

            case EDataSourceType::VersionedTable: {
                auto memoryEstimate = GetDataSliceDescriptorReaderMemoryEstimate(dataSliceDescriptor, config);
                int dataSourceIndex = dataSliceDescriptor.GetDataSourceIndex();
                const auto& dataSource = dataSourceDirectory->DataSources()[dataSourceIndex];
                auto createReader = BIND([=] () -> IReaderBasePtr {
                    return wrapReader(CreateSchemalessMergingMultiChunkReader(
                        config,
                        options,
                        client,
                        localDescriptor,
                        localNodeId,
                        blockCache,
                        chunkMetaCache,
                        nodeDirectory,
                        dataSourceDirectory,
                        dataSliceDescriptor,
                        nameTable,
                        chunkReadOptions,
                        columnFilter.IsUniversal() ? CreateColumnFilter(dataSource.Columns(), nameTable) : columnFilter,
                        trafficMeter,
                        bandwidthThrottler,
                        rpsThrottler));
                });

                auto canCreateReader = BIND([=] {
                    return multiReaderMemoryManager->GetFreeMemorySize() >= memoryEstimate;
                });

                factories.push_back(CreateReaderFactory(createReader, canCreateReader, dataSliceDescriptor));
                break;
            }

            default:
                YT_ABORT();
        }
    }

    return factories;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TSchemalessMultiChunkReader
    : public ISchemalessMultiChunkReader
{
public:
    TSchemalessMultiChunkReader(
        IMultiReaderManagerPtr multiReaderManager,
        TNameTablePtr nameTable,
        const std::vector<TDataSliceDescriptor>& dataSliceDescriptors);

    ~TSchemalessMultiChunkReader();

    IUnversionedRowBatchPtr Read(const TRowBatchReadOptions& options) override;

    i64 GetSessionRowIndex() const override;
    i64 GetTotalRowCount() const override;
    i64 GetTableRowIndex() const override;

    const TNameTablePtr& GetNameTable() const override;

    void Interrupt() override;

    void SkipCurrentReader() override;

    TInterruptDescriptor GetInterruptDescriptor(
        TRange<TUnversionedRow> unreadRows) const override;

    const TDataSliceDescriptor& GetCurrentReaderDescriptor() const override;

    TTimingStatistics GetTimingStatistics() const override
    {
        // We take wait time from multi reader manager as all ready event bookkeeping is delegated to it.
        // Read time is accounted from our own read timer (reacall that multi reader manager deals with chunk readers
        // while Read() is a table reader level methdd).
        auto statistics = MultiReaderManager_->GetTimingStatistics();
        statistics.ReadTime = ReadTimer_.GetElapsedTime();
        statistics.IdleTime -= statistics.ReadTime;
        return statistics;
    }

    TFuture<void> GetReadyEvent() const override
    {
        return MultiReaderManager_->GetReadyEvent();
    }

    TDataStatistics GetDataStatistics() const override
    {
        return MultiReaderManager_->GetDataStatistics();
    }

    TCodecStatistics GetDecompressionStatistics() const override
    {
        return MultiReaderManager_->GetDecompressionStatistics();
    }

    bool IsFetchingCompleted() const override
    {
        return MultiReaderManager_->IsFetchingCompleted();
    }

    std::vector<TChunkId> GetFailedChunkIds() const override
    {
        return MultiReaderManager_->GetFailedChunkIds();
    }

private:
    const IMultiReaderManagerPtr MultiReaderManager_;
    const TNameTablePtr NameTable_;

    ISchemalessChunkReaderPtr CurrentReader_;
    std::atomic<i64> RowIndex_ = 0;
    std::atomic<i64> RowCount_ = -1;

    TInterruptDescriptor FinishedInterruptDescriptor_;

    std::atomic<bool> Finished_ = false;

    TWallTimer ReadTimer_ = TWallTimer(false /*active */);

    void OnReaderSwitched();
};

////////////////////////////////////////////////////////////////////////////////

TSchemalessMultiChunkReader::TSchemalessMultiChunkReader(
    IMultiReaderManagerPtr multiReaderManager,
    TNameTablePtr nameTable,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors)
    : MultiReaderManager_(std::move(multiReaderManager))
    , NameTable_(nameTable)
    , RowCount_(GetCumulativeRowCount(dataSliceDescriptors))
{
    if (dataSliceDescriptors.empty()) {
        Finished_ = true;
    }
    MultiReaderManager_->SubscribeReaderSwitched(BIND(&TSchemalessMultiChunkReader::OnReaderSwitched, MakeWeak(this)));
    MultiReaderManager_->Open();
}

TSchemalessMultiChunkReader::~TSchemalessMultiChunkReader()
{
    const auto& Logger = MultiReaderManager_->GetLogger();
    YT_LOG_DEBUG("Multi chunk reader timing statistics (TimingStatistics: %v)", TSchemalessMultiChunkReader::GetTimingStatistics());
}

IUnversionedRowBatchPtr TSchemalessMultiChunkReader::Read(const TRowBatchReadOptions& options)
{
    auto readGuard = TTimerGuard<TWallTimer>(&ReadTimer_);

    if (!MultiReaderManager_->GetReadyEvent().IsSet() || !MultiReaderManager_->GetReadyEvent().Get().IsOK()) {
        return CreateEmptyUnversionedRowBatch();
    }

    if (Finished_) {
        RowCount_ = RowIndex_.load();
        return nullptr;
    }

    auto batch = CurrentReader_->Read(options);
    if (batch && !batch->IsEmpty()) {
        RowIndex_ += batch->GetRowCount();
        return batch;
    }

    if (!batch) {
        // This must fill read descriptors with values from finished readers.
        auto interruptDescriptor = CurrentReader_->GetInterruptDescriptor({});
        FinishedInterruptDescriptor_.MergeFrom(std::move(interruptDescriptor));
    }

    if (!MultiReaderManager_->OnEmptyRead(!batch)) {
        Finished_ = true;
    }

    return batch ? batch : CreateEmptyUnversionedRowBatch();
}

void TSchemalessMultiChunkReader::OnReaderSwitched()
{
    CurrentReader_ = dynamic_cast<ISchemalessChunkReader*>(MultiReaderManager_->GetCurrentSession().Reader.Get());
    YT_VERIFY(CurrentReader_);
}

i64 TSchemalessMultiChunkReader::GetTotalRowCount() const
{
    return RowCount_;
}

i64 TSchemalessMultiChunkReader::GetSessionRowIndex() const
{
    return RowIndex_;
}

i64 TSchemalessMultiChunkReader::GetTableRowIndex() const
{
    return CurrentReader_ ? CurrentReader_->GetTableRowIndex() : 0;
}

const TNameTablePtr& TSchemalessMultiChunkReader::GetNameTable() const
{
    return NameTable_;
}

void TSchemalessMultiChunkReader::Interrupt()
{
    if (!Finished_.exchange(true)) {
        MultiReaderManager_->Interrupt();
    }
}

void TSchemalessMultiChunkReader::SkipCurrentReader()
{
    if (!MultiReaderManager_->GetReadyEvent().IsSet() || !MultiReaderManager_->GetReadyEvent().Get().IsOK()) {
        return;
    }

    // Pretend that current reader already finished.
    if (!MultiReaderManager_->OnEmptyRead(/* readerFinished */ true)) {
        Finished_ = true;
    }
}

TInterruptDescriptor TSchemalessMultiChunkReader::GetInterruptDescriptor(
    TRange<TUnversionedRow> unreadRows) const
{
    static TRange<TUnversionedRow> emptyRange;
    auto state = MultiReaderManager_->GetUnreadState();

    auto result = FinishedInterruptDescriptor_;
    if (state.CurrentReader) {
        auto chunkReader = dynamic_cast<ISchemalessChunkReader*>(state.CurrentReader.Get());
        YT_VERIFY(chunkReader);
        result.MergeFrom(chunkReader->GetInterruptDescriptor(unreadRows));
    }
    for (const auto& activeReader : state.ActiveReaders) {
        auto chunkReader = dynamic_cast<ISchemalessChunkReader*>(activeReader.Get());
        YT_VERIFY(chunkReader);
        auto interruptDescriptor = chunkReader->GetInterruptDescriptor(emptyRange);
        result.MergeFrom(std::move(interruptDescriptor));
    }
    for (const auto& factory : state.ReaderFactories) {
        result.UnreadDataSliceDescriptors.emplace_back(factory->GetDataSliceDescriptor());
    }
    return result;
}

const TDataSliceDescriptor& TSchemalessMultiChunkReader::GetCurrentReaderDescriptor() const
{
    return CurrentReader_->GetCurrentReaderDescriptor();
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessSequentialMultiReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    NNative::IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    std::optional<TNodeId> localNodeId,
    IBlockCachePtr blockCache,
    IClientChunkMetaCachePtr chunkMetaCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
    TNameTablePtr nameTable,
    const TClientChunkReadOptions& chunkReadOptions,
    const TColumnFilter& columnFilter,
    const TSortColumns& sortColumns,
    std::optional<int> partitionTag,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler,
    NChunkClient::IMultiReaderMemoryManagerPtr multiReaderMemoryManager,
    int interruptDescriptorKeyLength)
{
    if (!multiReaderMemoryManager) {
        multiReaderMemoryManager = CreateParallelReaderMemoryManager(
            TParallelReaderMemoryManagerOptions{
                .TotalReservedMemorySize = config->MaxBufferSize,
                .MaxInitialReaderReservedMemory = config->WindowSize
            },
            NChunkClient::TDispatcher::Get()->GetReaderMemoryManagerInvoker());
    }

    return New<TSchemalessMultiChunkReader>(
        CreateSequentialMultiReaderManager(
            config,
            options,
            CreateReaderFactories(
                config,
                options,
                client,
                localDescriptor,
                localNodeId,
                blockCache,
                chunkMetaCache,
                nodeDirectory,
                dataSourceDirectory,
                dataSliceDescriptors,
                nameTable,
                chunkReadOptions,
                columnFilter,
                sortColumns,
                partitionTag,
                trafficMeter,
                std::move(bandwidthThrottler),
                std::move(rpsThrottler),
                multiReaderMemoryManager,
                interruptDescriptorKeyLength),
            multiReaderMemoryManager),
        nameTable,
        dataSliceDescriptors);
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessParallelMultiReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    NNative::IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    std::optional<TNodeId> localNodeId,
    IBlockCachePtr blockCache,
    IClientChunkMetaCachePtr chunkMetaCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
    TNameTablePtr nameTable,
    const TClientChunkReadOptions& chunkReadOptions,
    const TColumnFilter& columnFilter,
    const TSortColumns& sortColumns,
    std::optional<int> partitionTag,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler,
    NChunkClient::IMultiReaderMemoryManagerPtr multiReaderMemoryManager,
    int interruptDescriptorKeyLength)
{
    if (!multiReaderMemoryManager) {
        multiReaderMemoryManager = CreateParallelReaderMemoryManager(
            TParallelReaderMemoryManagerOptions{
                .TotalReservedMemorySize = config->MaxBufferSize,
                .MaxInitialReaderReservedMemory = config->WindowSize
            },
            NChunkClient::TDispatcher::Get()->GetReaderMemoryManagerInvoker());
    }

    return New<TSchemalessMultiChunkReader>(
        CreateParallelMultiReaderManager(
            config,
            options,
            CreateReaderFactories(
                config,
                options,
                client,
                localDescriptor,
                localNodeId,
                blockCache,
                chunkMetaCache,
                nodeDirectory,
                dataSourceDirectory,
                dataSliceDescriptors,
                nameTable,
                chunkReadOptions,
                columnFilter,
                sortColumns,
                partitionTag,
                trafficMeter,
                std::move(bandwidthThrottler),
                std::move(rpsThrottler),
                multiReaderMemoryManager,
                interruptDescriptorKeyLength),
            multiReaderMemoryManager),
        nameTable,
        dataSliceDescriptors);
}

////////////////////////////////////////////////////////////////////////////////

class TSchemalessMergingMultiChunkReader
    : public ISchemalessMultiChunkReader
{
public:
    static ISchemalessMultiChunkReaderPtr Create(
        TTableReaderConfigPtr config,
        TTableReaderOptionsPtr options,
        NNative::IClientPtr client,
        const TNodeDescriptor& localDescriptor,
        std::optional<TNodeId> localNodeId,
        IBlockCachePtr blockCache,
        IClientChunkMetaCachePtr chunkMetaCache,
        TNodeDirectoryPtr nodeDirectory,
        const TDataSourceDirectoryPtr& dataSourceDirectory,
        const TDataSliceDescriptor& dataSliceDescriptor,
        TNameTablePtr nameTable,
        const TClientChunkReadOptions& chunkReadOptions,
        TColumnFilter columnFilter,
        TTrafficMeterPtr trafficMeter,
        IThroughputThrottlerPtr bandwidthThrottler,
        IThroughputThrottlerPtr rpsThrottler,
        IMultiReaderMemoryManagerPtr multiReaderMemoryManager);

    TFuture<void> GetReadyEvent() const override
    {
        return AnySet(
            std::vector{ErrorPromise_.ToFuture(), UnderlyingReader_->GetReadyEvent()},
            TFutureCombinerOptions{.CancelInputOnShortcut = false});
    }

    TDataStatistics GetDataStatistics() const override
    {
        return UnderlyingReader_->GetDataStatistics();
    }

    TCodecStatistics GetDecompressionStatistics() const override
    {
        return UnderlyingReader_->GetDecompressionStatistics();
    }

    TTimingStatistics GetTimingStatistics() const override
    {
        // TODO(max42): one should make IReaderBase inherit from ITimingReader in order for this to work.
        // return UnderlyingReader_->GetTimingStatistics();

        return {};
    }

    std::vector<TChunkId> GetFailedChunkIds() const override
    {
        // TODO(psushin): every reader must implement this method eventually.
        return {};
    }

    IUnversionedRowBatchPtr Read(const TRowBatchReadOptions& options) override
    {
        MemoryPool_.Clear();

        if (Interrupting_) {
            return nullptr;
        }

        if (ErrorPromise_.IsSet()) {
            return CreateEmptyUnversionedRowBatch();
        }

        SchemafulBatch_ = UnderlyingReader_->Read(options);
        if (SchemafulBatch_) {
            SchemafulRows_ = SchemafulBatch_->MaterializeRows();
        }

        if (!SchemafulBatch_) {
            HasMore_ = false;
            return nullptr;
        }

        if (SchemafulBatch_->IsEmpty()) {
            return CreateEmptyUnversionedRowBatch();
        }

        LastKey_ = GetKeyPrefix(SchemafulRows_.Back(), Schema_->GetKeyColumnCount());

        YT_VERIFY(HasMore_);

        std::vector<TUnversionedRow> schemalessRows;
        schemalessRows.reserve(SchemafulRows_.Size());

        try {
            for (auto schemafulRow : SchemafulRows_) {
                auto schemalessRow = TMutableUnversionedRow::Allocate(&MemoryPool_, SchemaColumnCount_ + SystemColumnCount_);

                int schemalessValueIndex = 0;
                for (int valueIndex = 0; valueIndex < static_cast<int>(schemafulRow.GetCount()); ++valueIndex) {
                    const auto& value = schemafulRow[valueIndex];
                    auto id = IdMapping_[value.Id];

                    if (id >= 0) {
                        schemalessRow[schemalessValueIndex] = value;
                        schemalessRow[schemalessValueIndex].Id = id;
                        ++schemalessValueIndex;
                    }
                }

                schemalessRow.SetCount(SchemaColumnCount_);

                if (Options_->EnableRangeIndex) {
                    *schemalessRow.End() = MakeUnversionedInt64Value(RangeIndex_, RangeIndexId_);
                    schemalessRow.SetCount(schemalessRow.GetCount() + 1);
                }
                if (Options_->EnableTableIndex) {
                    *schemalessRow.End() = MakeUnversionedInt64Value(TableIndex_, TableIndexId_);
                    schemalessRow.SetCount(schemalessRow.GetCount() + 1);
                }

                schemalessRows.push_back(schemalessRow);
            }

            RowIndex_ += schemalessRows.size();
        } catch (const std::exception& ex) {
            SchemafulBatch_.Reset();
            SchemafulRows_ = {};
            ErrorPromise_.Set(ex);
            return CreateEmptyUnversionedRowBatch();
        }

        return CreateBatchFromUnversionedRows(MakeSharedRange(std::move(schemalessRows), MakeStrong(this)));
    }

    TInterruptDescriptor GetInterruptDescriptor(
        TRange<TUnversionedRow> unreadRows) const override
    {
        std::vector<TDataSliceDescriptor> unreadDescriptors;
        std::vector<TDataSliceDescriptor> readDescriptors;

        TLegacyOwningKey firstUnreadKey;
        if (!unreadRows.Empty()) {
            auto firstSchemafulUnreadRow = SchemafulRows_[SchemafulRows_.size() - unreadRows.Size()];
            firstUnreadKey = GetKeyPrefix(firstSchemafulUnreadRow, Schema_->GetKeyColumnCount());
        } else if (LastKey_) {
            firstUnreadKey = GetKeySuccessor(LastKey_);
        }

        if (!unreadRows.Empty() || HasMore_) {
            unreadDescriptors.emplace_back(DataSliceDescriptor_);
        }
        if (LastKey_) {
            readDescriptors.emplace_back(DataSliceDescriptor_);
        }

        YT_VERIFY(firstUnreadKey || readDescriptors.empty());

        if (firstUnreadKey) {
            // TODO: Estimate row count and data size.
            for (auto& descriptor : unreadDescriptors) {
                for (auto& chunk : descriptor.ChunkSpecs) {
                    ToProto(chunk.mutable_lower_limit()->mutable_legacy_key(), firstUnreadKey);
                }
            }
            for (auto& descriptor : readDescriptors) {
                for (auto& chunk : descriptor.ChunkSpecs) {
                    ToProto(chunk.mutable_upper_limit()->mutable_legacy_key(), firstUnreadKey);
                }
            }
        }

        return {std::move(unreadDescriptors), std::move(readDescriptors)};
    }

    void Interrupt() override
    {
        Interrupting_.store(true);
        ErrorPromise_.TrySet();
    }

    void SkipCurrentReader() override
    {
        // Merging reader doesn't support sub-reader skipping.
    }

    bool IsFetchingCompleted() const override
    {
        return false;
    }

    i64 GetSessionRowIndex() const override
    {
        return RowIndex_;
    }

    i64 GetTotalRowCount() const override
    {
        return RowCount_;
    }

    const TNameTablePtr& GetNameTable() const override
    {
        return NameTable_;
    }

    i64 GetTableRowIndex() const override
    {
        // Not supported for versioned data.
        return -1;
    }

    const TDataSliceDescriptor& GetCurrentReaderDescriptor() const override
    {
        YT_ABORT();
    }

    ~TSchemalessMergingMultiChunkReader()
    {
        YT_LOG_DEBUG("Schemaless merging multi chunk reader data statistics (DataStatistics: %v)", TSchemalessMergingMultiChunkReader::GetDataStatistics());
    }

private:
    const TTableReaderOptionsPtr Options_;
    const ISchemafulUnversionedReaderPtr UnderlyingReader_;
    const TDataSliceDescriptor DataSliceDescriptor_;
    const TTableSchemaPtr Schema_;
    const std::vector<int> IdMapping_;
    const TNameTablePtr NameTable_;
    const i64 RowCount_;
    const IMultiReaderMemoryManagerPtr ParallelReaderMemoryManager_;

    // We keep rows received from underlying schemaful reader
    // to define proper lower limit during interrupt.
    IUnversionedRowBatchPtr SchemafulBatch_;
    TRange<TUnversionedRow> SchemafulRows_;

    std::atomic<bool> Interrupting_ = false;

    // We must assume that there is more data if we read nothing to the moment.
    std::atomic<bool> HasMore_ = true;
    TLegacyOwningKey LastKey_;

    i64 RowIndex_ = 0;

    TChunkedMemoryPool MemoryPool_;

    int TableIndexId_ = -1;
    int RangeIndexId_ = -1;
    int TableIndex_ = -1;
    int RangeIndex_ = -1;
    int SystemColumnCount_ = 0;

    // Number of "active" columns in id mapping.
    int SchemaColumnCount_ = 0;

    const TPromise<void> ErrorPromise_ = NewPromise<void>();

    TLogger Logger;

    TSchemalessMergingMultiChunkReader(
        TTableReaderOptionsPtr options,
        ISchemafulUnversionedReaderPtr underlyingReader,
        const TDataSliceDescriptor& dataSliceDescriptor,
        TTableSchemaPtr schema,
        std::vector<int> idMapping,
        TNameTablePtr nameTable,
        i64 rowCount,
        IMultiReaderMemoryManagerPtr parallelReaderMemoryManager,
        TLogger logger)
        : Options_(options)
        , UnderlyingReader_(std::move(underlyingReader))
        , DataSliceDescriptor_(dataSliceDescriptor)
        , Schema_(std::move(schema))
        , IdMapping_(idMapping)
        , NameTable_(nameTable)
        , RowCount_(rowCount)
        , ParallelReaderMemoryManager_(std::move(parallelReaderMemoryManager))
        , Logger(std::move(logger))
    {
        if (!DataSliceDescriptor_.ChunkSpecs.empty()) {
            TableIndex_ = DataSliceDescriptor_.ChunkSpecs.front().table_index();
            RangeIndex_ = DataSliceDescriptor_.ChunkSpecs.front().range_index();
        }

        if (Options_->EnableRangeIndex) {
            ++SystemColumnCount_;
            RangeIndexId_ = NameTable_->GetIdOrRegisterName(RangeIndexColumnName);
        }

        if (Options_->EnableTableIndex) {
            ++SystemColumnCount_;
            TableIndexId_ = NameTable_->GetIdOrRegisterName(TableIndexColumnName);
        }

        for (auto id : IdMapping_) {
            if (id >= 0) {
                ++SchemaColumnCount_;
            }
        }
    }

    DECLARE_NEW_FRIEND();
};

////////////////////////////////////////////////////////////////////////////////

std::tuple<TTableSchemaPtr, TColumnFilter> CreateVersionedReadParameters(
    const TTableSchemaPtr& schema,
    const TColumnFilter& columnFilter)
{
    if (columnFilter.IsUniversal()) {
        return std::make_pair(schema, columnFilter);
    }

    std::vector<NTableClient::TColumnSchema> columns;
    for (int index = 0; index < schema->GetKeyColumnCount(); ++index) {
        columns.push_back(schema->Columns()[index]);
    }

    TColumnFilter::TIndexes columnFilterIndexes;
    for (int index : columnFilter.GetIndexes()) {
        if (index >= schema->GetKeyColumnCount()) {
            columnFilterIndexes.push_back(columns.size());
            columns.push_back(schema->Columns()[index]);
        } else {
            columnFilterIndexes.push_back(index);
        }
    }

    return std::make_tuple(
        New<TTableSchema>(std::move(columns)),
        TColumnFilter(std::move(columnFilterIndexes)));
}

ISchemalessMultiChunkReaderPtr TSchemalessMergingMultiChunkReader::Create(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    NNative::IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    std::optional<TNodeId> localNodeId,
    IBlockCachePtr blockCache,
    IClientChunkMetaCachePtr chunkMetaCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const TDataSliceDescriptor& dataSliceDescriptor,
    TNameTablePtr nameTable,
    const TClientChunkReadOptions& chunkReadOptions,
    TColumnFilter columnFilter,
    TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler,
    IMultiReaderMemoryManagerPtr multiReaderMemoryManager)
{
    if (config->SamplingRate && config->SamplingMode == ESamplingMode::Block) {
        THROW_ERROR_EXCEPTION("Block sampling is not yet supported for sorted dynamic tables");
    }

    auto Logger = TableClientLogger;
    if (chunkReadOptions.ReadSessionId) {
        Logger.AddTag("ReadSessionId: %v", chunkReadOptions.ReadSessionId);
    }

    const auto& dataSource = dataSourceDirectory->DataSources()[dataSliceDescriptor.GetDataSourceIndex()];
    const auto& chunkSpecs = dataSliceDescriptor.ChunkSpecs;

    auto tableSchema = dataSource.Schema();
    YT_VERIFY(tableSchema);
    auto timestamp = dataSource.GetTimestamp();
    auto retentionTimestamp = dataSource.GetRetentionTimestamp();
    const auto& renameDescriptors = dataSource.ColumnRenameDescriptors();

    if (!columnFilter.IsUniversal()) {
        TColumnFilter::TIndexes transformedIndexes;
        for (auto index : columnFilter.GetIndexes()) {
            if (auto* column = tableSchema->FindColumn(nameTable->GetName(index))) {
                auto columnIndex = tableSchema->GetColumnIndex(*column);
                if (std::find(transformedIndexes.begin(), transformedIndexes.end(), columnIndex) ==
                    transformedIndexes.end())
                {
                    transformedIndexes.push_back(columnIndex);
                }
            }
        }
        columnFilter = TColumnFilter(std::move(transformedIndexes));
    }

    ValidateColumnFilter(columnFilter, tableSchema->GetColumnCount());

    auto [versionedReadSchema, versionedColumnFilter] = CreateVersionedReadParameters(
        tableSchema,
        columnFilter);

    std::vector<int> idMapping(versionedReadSchema->GetColumnCount());

    try {
        for (int columnIndex = 0; columnIndex < std::ssize(versionedReadSchema->Columns()); ++columnIndex) {
            const auto& column = versionedReadSchema->Columns()[columnIndex];
            if (versionedColumnFilter.ContainsIndex(columnIndex)) {
                idMapping[columnIndex] = nameTable->GetIdOrRegisterName(column.Name());
            } else {
                // We should skip this column in schemaless reading.
                idMapping[columnIndex] = -1;
            }
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Failed to update name table for schemaless merging multi chunk reader")
            << ex;
    }

    std::vector<TLegacyOwningKey> boundaries;
    boundaries.reserve(chunkSpecs.size());

    auto extractMinKey = [] (const TChunkSpec& chunkSpec) {
        auto type = TypeFromId(FromProto<TChunkId>(chunkSpec.chunk_id()));

        if (chunkSpec.has_lower_limit()) {
            auto limit = FromProto<TLegacyReadLimit>(chunkSpec.lower_limit());
            if (limit.HasLegacyKey()) {
                return limit.GetLegacyKey();
            }
        } else if (IsChunkTabletStoreType(type)) {
            YT_VERIFY(chunkSpec.has_chunk_meta());
            if (FindProtoExtension<NProto::TBoundaryKeysExt>(chunkSpec.chunk_meta().extensions())) {
                auto boundaryKeysExt = GetProtoExtension<NProto::TBoundaryKeysExt>(chunkSpec.chunk_meta().extensions());
                return FromProto<TLegacyOwningKey>(boundaryKeysExt.min());
            }
        }
        return TLegacyOwningKey();
    };

    for (const auto& chunkSpec : chunkSpecs) {
        TLegacyOwningKey minKey = extractMinKey(chunkSpec);
        boundaries.push_back(minKey);
    }

    YT_LOG_DEBUG("Create overlapping range reader (Boundaries: %v, Stores: %v, ColumnFilter: %v)",
        boundaries,
        MakeFormattableView(chunkSpecs, [] (TStringBuilderBase* builder, const TChunkSpec& chunkSpec) {
            FormatValue(builder, FromProto<TChunkId>(chunkSpec.chunk_id()), TStringBuf());
        }),
        columnFilter);

    auto performanceCounters = New<TChunkReaderPerformanceCounters>();

    if (!multiReaderMemoryManager) {
        multiReaderMemoryManager = CreateParallelReaderMemoryManager(
            TParallelReaderMemoryManagerOptions{
                .TotalReservedMemorySize = config->MaxBufferSize,
                .MaxInitialReaderReservedMemory = config->WindowSize
            },
            NChunkClient::TDispatcher::Get()->GetReaderMemoryManagerInvoker());
    }

    auto createVersionedChunkReader = [
        config,
        options,
        client,
        localDescriptor,
        localNodeId,
        blockCache,
        chunkMetaCache,
        nodeDirectory,
        chunkReadOptions,
        chunkSpecs,
        tableSchema,
        versionedReadSchema = versionedReadSchema,
        performanceCounters,
        timestamp,
        trafficMeter,
        bandwidthThrottler,
        rpsThrottler,
        renameDescriptors,
        multiReaderMemoryManager,
        dataSource,
        Logger
    ] (
        const TChunkSpec& chunkSpec,
        const TChunkReaderMemoryManagerPtr& chunkReaderMemoryManager) -> IVersionedReaderPtr
    {
        auto chunkId = NYT::FromProto<TChunkId>(chunkSpec.chunk_id());
        auto replicas = NYT::FromProto<TChunkReplicaList>(chunkSpec.replicas());

        TLegacyReadLimit lowerLimit;
        TLegacyReadLimit upperLimit;

        if (chunkSpec.has_lower_limit()) {
            lowerLimit = NYT::FromProto<TLegacyReadLimit>(chunkSpec.lower_limit());
        }
        if (!lowerLimit.HasLegacyKey() || !lowerLimit.GetLegacyKey()) {
            lowerLimit.SetLegacyKey(MinKey());
        }

        if (chunkSpec.has_upper_limit()) {
            upperLimit = NYT::FromProto<TLegacyReadLimit>(chunkSpec.upper_limit());
        }
        if (!upperLimit.HasLegacyKey() || !upperLimit.GetLegacyKey()) {
            upperLimit.SetLegacyKey(MaxKey());
        }

        if (lowerLimit.HasRowIndex() || upperLimit.HasRowIndex()) {
            THROW_ERROR_EXCEPTION("Row index limit is not supported");
        }

        YT_LOG_DEBUG("Creating versioned chunk reader (ChunkId: %v, Range: <%v : %v>)",
            chunkId,
            lowerLimit,
            upperLimit);

        auto remoteReader = CreateRemoteReader(
            chunkSpec,
            config,
            options,
            client,
            nodeDirectory,
            localDescriptor,
            localNodeId,
            blockCache,
            chunkMetaCache,
            trafficMeter,
            /* nodeStatusDirectory */ nullptr,
            bandwidthThrottler,
            rpsThrottler);

        auto asyncChunkMeta = TCachedVersionedChunkMeta::Load(
            remoteReader,
            chunkReadOptions,
            versionedReadSchema,
            renameDescriptors,
            nullptr /* memoryTracker */);
        auto chunkMeta = WaitFor(asyncChunkMeta)
            .ValueOrThrow();
        auto chunkState = New<TChunkState>(
            blockCache,
            chunkSpec,
            /*chunkMeta*/ nullptr,
            chunkSpec.has_override_timestamp() ? chunkSpec.override_timestamp() : NullTimestamp,
            /*lookupHashTable*/ nullptr,
            performanceCounters,
            /*keyComparer*/ nullptr,
            /*virtualValueDirectory*/ nullptr,
            versionedReadSchema);
        chunkState->DataSource = dataSource;

        return CreateVersionedChunkReader(
            config,
            std::move(remoteReader),
            std::move(chunkState),
            std::move(chunkMeta),
            chunkReadOptions,
            lowerLimit.GetLegacyKey(),
            upperLimit.GetLegacyKey(),
            TColumnFilter(),
            timestamp,
            false,
            chunkReaderMemoryManager
                ? chunkReaderMemoryManager
                : multiReaderMemoryManager->CreateChunkReaderMemoryManager(
                    chunkMeta->Misc().uncompressed_data_size()));
    };

    auto createVersionedReader = [
        config,
        options,
        client,
        blockCache,
        chunkMetaCache,
        nodeDirectory,
        chunkReadOptions,
        chunkSpecs,
        tableSchema,
        columnFilter,
        performanceCounters,
        timestamp,
        trafficMeter,
        bandwidthThrottler,
        rpsThrottler,
        renameDescriptors,
        multiReaderMemoryManager,
        createVersionedChunkReader,
        Logger
    ] (int index) -> IVersionedReaderPtr {
        const auto& chunkSpec = chunkSpecs[index];
        auto chunkId = NYT::FromProto<TChunkId>(chunkSpec.chunk_id());
        auto type = TypeFromId(chunkId);

        if (type == EObjectType::SortedDynamicTabletStore) {
            return CreateRetryingRemoteSortedDynamicStoreReader(
                chunkSpec,
                tableSchema,
                config->DynamicStoreReader,
                client,
                nodeDirectory,
                trafficMeter,
                bandwidthThrottler,
                rpsThrottler,
                chunkReadOptions,
                columnFilter,
                timestamp,
                multiReaderMemoryManager->CreateChunkReaderMemoryManager(
                    DefaultRemoteDynamicStoreReaderMemoryEstimate),
                    BIND(createVersionedChunkReader));
        } else {
            return createVersionedChunkReader(chunkSpec, nullptr);
        }
    };

    struct TSchemalessMergingMultiChunkReaderBufferTag
    { };

    auto rowMerger = std::make_unique<TSchemafulRowMerger>(
        New<TRowBuffer>(TSchemalessMergingMultiChunkReaderBufferTag()),
        versionedReadSchema->GetColumnCount(),
        versionedReadSchema->GetKeyColumnCount(),
        TColumnFilter(),
        client->GetNativeConnection()->GetColumnEvaluatorCache()->Find(versionedReadSchema),
        retentionTimestamp);

    auto schemafulReader = CreateSchemafulOverlappingRangeReader(
        std::move(boundaries),
        std::move(rowMerger),
        createVersionedReader,
        [] (
            const TUnversionedValue* lhsBegin,
            const TUnversionedValue* lhsEnd,
            const TUnversionedValue* rhsBegin,
            const TUnversionedValue* rhsEnd)
        {
            return CompareRows(lhsBegin, lhsEnd, rhsBegin, rhsEnd);
        });

    i64 rowCount = NChunkClient::GetCumulativeRowCount(chunkSpecs);

    return New<TSchemalessMergingMultiChunkReader>(
        std::move(options),
        std::move(schemafulReader),
        dataSliceDescriptor,
        versionedReadSchema,
        std::move(idMapping),
        std::move(nameTable),
        rowCount,
        std::move(multiReaderMemoryManager),
        Logger);
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessMergingMultiChunkReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    NNative::IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    std::optional<TNodeId> localNodeId,
    IBlockCachePtr blockCache,
    IClientChunkMetaCachePtr chunkMetaCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const TDataSliceDescriptor& dataSliceDescriptor,
    TNameTablePtr nameTable,
    const TClientChunkReadOptions& chunkReadOptions,
    const TColumnFilter& columnFilter,
    NChunkClient::TTrafficMeterPtr trafficMeter,
    IThroughputThrottlerPtr bandwidthThrottler,
    IThroughputThrottlerPtr rpsThrottler,
    IMultiReaderMemoryManagerPtr readerMemoryManager)
{
    return TSchemalessMergingMultiChunkReader::Create(
        config,
        options,
        client,
        localDescriptor,
        localNodeId,
        blockCache,
        chunkMetaCache,
        nodeDirectory,
        dataSourceDirectory,
        dataSliceDescriptor,
        nameTable,
        chunkReadOptions,
        columnFilter,
        trafficMeter,
        std::move(bandwidthThrottler),
        std::move(rpsThrottler),
        std::move(readerMemoryManager));
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateAppropriateSchemalessMultiChunkReader(
    const NNative::IClientPtr& client,
    const TTableReaderOptionsPtr& options,
    const TTableReaderConfigPtr& config,
    TTableReadSpec& tableReadSpec,
    const TClientChunkReadOptions& chunkReadOptions,
    bool unordered,
    const TNameTablePtr& nameTable,
    const TColumnFilter& columnFilter,
    const IThroughputThrottlerPtr& bandwidthThrottler,
    const IThroughputThrottlerPtr& rpsThrottler)
{
    const auto& dataSourceDirectory = tableReadSpec.DataSourceDirectory;
    auto& dataSliceDescriptors = tableReadSpec.DataSliceDescriptors;

    // TODO(max42): think about mixing different data sources here.
    // TODO(max42): what about reading several tables together?
    YT_VERIFY(dataSourceDirectory->DataSources().size() == 1);
    const auto& dataSource = dataSourceDirectory->DataSources().front();

    switch (dataSourceDirectory->GetCommonTypeOrThrow()) {
        case EDataSourceType::VersionedTable: {
            YT_VERIFY(dataSliceDescriptors.size() == 1);
            auto& dataSliceDescriptor = dataSliceDescriptors.front();

            auto adjustedColumnFilter = columnFilter.IsUniversal()
                ? CreateColumnFilter(dataSource.Columns(), nameTable)
                : columnFilter;

            return CreateSchemalessMergingMultiChunkReader(
                config,
                options,
                client,
                /*localDescriptor*/ {},
                /*partitionTag*/ std::nullopt,
                client->GetNativeConnection()->GetBlockCache(),
                client->GetNativeConnection()->GetChunkMetaCache(),
                client->GetNativeConnection()->GetNodeDirectory(),
                dataSourceDirectory,
                std::move(dataSliceDescriptor),
                nameTable,
                chunkReadOptions,
                adjustedColumnFilter,
                /*trafficMeter*/ nullptr,
                bandwidthThrottler,
                rpsThrottler);
        }
        case EDataSourceType::UnversionedTable: {
            auto factory = unordered
                ? CreateSchemalessParallelMultiReader
                : CreateSchemalessSequentialMultiReader;
            return factory(
                config,
                options,
                client,
                // Client doesn't have a node descriptor.
                /*localDescriptor*/ {},
                std::nullopt,
                client->GetNativeConnection()->GetBlockCache(),
                client->GetNativeConnection()->GetChunkMetaCache(),
                client->GetNativeConnection()->GetNodeDirectory(),
                dataSourceDirectory,
                std::move(dataSliceDescriptors),
                nameTable,
                chunkReadOptions,
                columnFilter,
                dataSource.Schema()->GetSortColumns(),
                /*partitionTag*/ std::nullopt,
                /*trafficMeter*/ nullptr,
                bandwidthThrottler,
                rpsThrottler,
                /*multiReaderMemoryManager*/ nullptr,
                /*interruptDescriptorKeyLength*/ 0);
        }
        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
