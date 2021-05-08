#include "ordered_chunk_store.h"
#include "tablet.h"
#include "versioned_chunk_meta_manager.h"

#include <yt/yt/server/lib/tablet_node/proto/tablet_manager.pb.h>

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/yt/ytlib/chunk_client/chunk_spec.h>

#include <yt/yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/yt/ytlib/chunk_client/cache_reader.h>

#include <yt/yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/table_client/cached_versioned_chunk_meta.h>
#include <yt/yt/ytlib/table_client/schemaful_chunk_reader.h>
#include <yt/yt/ytlib/table_client/chunk_state.h>

#include <yt/yt/client/table_client/unversioned_reader.h>
#include <yt/yt/client/table_client/row_batch.h>
#include <yt/yt/client/table_client/row_buffer.h>

#include <yt/yt/core/misc/protobuf_helpers.h>

#include <yt/yt/core/concurrency/throughput_throttler.h>

namespace NYT::NTabletNode {

using namespace NYTree;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NApi;
using namespace NDataNode;
using namespace NConcurrency;

using NChunkClient::NProto::TDataStatistics;

////////////////////////////////////////////////////////////////////////////////

struct TOrderedChunkStoreReaderTag
{ };

class TOrderedChunkStore::TReader
    : public ISchemafulUnversionedReader
{
public:
    TReader(
        ISchemafulUnversionedReaderPtr underlyingReader,
        bool enableTabletIndex,
        bool enableRowIndex,
        const TIdMapping& idMapping,
        int tabletIndex,
        i64 lowerRowIndex,
        TChunkReaderPerformanceCountersPtr performanceCounters)
        : UnderlyingReader_(std::move(underlyingReader))
        , TabletIndex_(tabletIndex)
        , EnableTabletIndex_(enableTabletIndex)
        , EnableRowIndex_(enableRowIndex)
        , IdMapping_(idMapping)
        , CurrentRowIndex_(lowerRowIndex)
        , Pool_(TOrderedChunkStoreReaderTag())
        , PerformanceCounters_(std::move(performanceCounters))
    { }

    virtual IUnversionedRowBatchPtr Read(const TRowBatchReadOptions& options) override
    {
        auto batch = UnderlyingReader_->Read(options);
        if (!batch) {
            return nullptr;
        }

        auto rows = batch->MaterializeRows();
        std::vector<TUnversionedRow> updatedRows;
        updatedRows.reserve(rows.size());

        Pool_.Clear();

        i64 dataWeight = 0;
        for (auto row : rows) {
            int updatedColumnCount =
                row.GetCount() +
                (EnableTabletIndex_ ? 1 : 0) +
                (EnableRowIndex_ ? 1 : 0);
            auto updatedRow = TMutableUnversionedRow::Allocate(&Pool_, updatedColumnCount);

            auto* updatedValue = updatedRow.Begin();

            if (EnableTabletIndex_) {
                *updatedValue++ = MakeUnversionedInt64Value(TabletIndex_, 0);
            }

            if (EnableRowIndex_) {
                *updatedValue++ = MakeUnversionedInt64Value(CurrentRowIndex_, 1);
            }

            for (const auto& value : row) {
                *updatedValue = value;
                updatedValue->Id = IdMapping_[updatedValue->Id];
                ++updatedValue;
            }

            dataWeight += GetDataWeight(updatedRow);
            updatedRows.push_back(updatedRow);
            ++CurrentRowIndex_;
        }

        PerformanceCounters_->StaticChunkRowReadCount += updatedRows.size();
        PerformanceCounters_->StaticChunkRowReadDataWeightCount += dataWeight;

        return CreateBatchFromUnversionedRows(MakeSharedRange(std::move(updatedRows), MakeStrong(this)));
    }

    virtual TFuture<void> GetReadyEvent() const override
    {
        return UnderlyingReader_->GetReadyEvent();
    }

    virtual TDataStatistics GetDataStatistics() const override
    {
        return UnderlyingReader_->GetDataStatistics();
    }

    virtual TCodecStatistics GetDecompressionStatistics() const override
    {
        return UnderlyingReader_->GetDecompressionStatistics();
    }

    virtual bool IsFetchingCompleted() const override
    {
        return false;
    }

    virtual std::vector<TChunkId> GetFailedChunkIds() const override
    {
        return {};
    }

private:
    const ISchemafulUnversionedReaderPtr UnderlyingReader_;
    const int TabletIndex_;
    const bool EnableTabletIndex_;
    const bool EnableRowIndex_;
    const TIdMapping IdMapping_;

    i64 CurrentRowIndex_;

    TChunkedMemoryPool Pool_;
    TChunkReaderPerformanceCountersPtr PerformanceCounters_;
};

////////////////////////////////////////////////////////////////////////////////

TOrderedChunkStore::TOrderedChunkStore(
    NClusterNode::TBootstrap* bootstrap,
    TTabletManagerConfigPtr config,
    TStoreId id,
    TTablet* tablet,
    const NTabletNode::NProto::TAddStoreDescriptor* addStoreDescriptor,
    IBlockCachePtr blockCache,
    IChunkRegistryPtr chunkRegistry,
    IChunkBlockManagerPtr chunkBlockManager,
    IVersionedChunkMetaManagerPtr chunkMetaManager,
    NNative::IClientPtr client,
    const TNodeDescriptor& localDescriptor)
    : TChunkStoreBase(
        bootstrap,
        config,
        /*storeId*/ id,
        /*chunkId*/ id,
        NullTimestamp,
        tablet,
        addStoreDescriptor,
        blockCache,
        chunkRegistry,
        chunkBlockManager,
        chunkMetaManager,
        client,
        localDescriptor)
{
    if (addStoreDescriptor) {
        YT_VERIFY(addStoreDescriptor->has_starting_row_index());
        SetStartingRowIndex(addStoreDescriptor->starting_row_index());
    }
}

TOrderedChunkStorePtr TOrderedChunkStore::AsOrderedChunk()
{
    return this;
}

EStoreType TOrderedChunkStore::GetType() const
{
    return EStoreType::OrderedChunk;
}

ISchemafulUnversionedReaderPtr TOrderedChunkStore::CreateReader(
    const TTabletSnapshotPtr& tabletSnapshot,
    int tabletIndex,
    i64 lowerRowIndex,
    i64 upperRowIndex,
    const TColumnFilter& columnFilter,
    const TClientChunkReadOptions& chunkReadOptions,
    std::optional<EWorkloadCategory> workloadCategory)
{
    TReadLimit lowerLimit;
    lowerRowIndex = std::min(std::max(lowerRowIndex, StartingRowIndex_), StartingRowIndex_ + GetRowCount());
    lowerLimit.SetRowIndex(lowerRowIndex - StartingRowIndex_);

    TReadLimit upperLimit;
    upperRowIndex = std::min(std::max(upperRowIndex, StartingRowIndex_), StartingRowIndex_ + GetRowCount());
    upperLimit.SetRowIndex(upperRowIndex - StartingRowIndex_);

    TReadRange readRange(lowerLimit, upperLimit);

    TColumnFilter valueColumnFilter;
    if (!columnFilter.IsUniversal()) {
        TColumnFilter::TIndexes valueColumnFilterIndexes;
        auto keyColumnCount = tabletSnapshot->QuerySchema->GetKeyColumnCount();
        for (auto index : columnFilter.GetIndexes()) {
            if (index >= keyColumnCount) {
                valueColumnFilterIndexes.push_back(index - keyColumnCount);
            }
        }
        valueColumnFilter = TColumnFilter(std::move(valueColumnFilterIndexes));
    }

    auto querySchema = tabletSnapshot->QuerySchema->Filter(columnFilter);
    auto readSchema = tabletSnapshot->PhysicalSchema->Filter(valueColumnFilter);

    bool enableTabletIndex = columnFilter.ContainsIndex(0);
    bool enableRowIndex = columnFilter.ContainsIndex(1);

    TIdMapping idMapping;
    for (const auto& readColumn : readSchema->Columns()) {
        idMapping.push_back(querySchema->GetColumnIndex(readColumn.Name()));
    }

    // Fast lane: check for in-memory reads.
    if (auto reader = TryCreateCacheBasedReader(
        chunkReadOptions,
        readRange,
        readSchema,
        enableTabletIndex,
        enableRowIndex,
        tabletIndex,
        lowerRowIndex,
        idMapping))
    {
        return reader;
    }

    auto chunkReader = GetReaders(workloadCategory).ChunkReader;
    auto asyncChunkMeta = ChunkMetaManager_->GetMeta(
        chunkReader,
        Schema_,
        chunkReadOptions);
    auto chunkMeta = WaitFor(asyncChunkMeta)
        .ValueOrThrow();

    auto chunkState = New<TChunkState>(GetBlockCache());

    auto underlyingReader = CreateSchemafulChunkReader(
        chunkState,
        chunkMeta,
        ReaderConfig_,
        std::move(chunkReader),
        chunkReadOptions,
        readSchema,
        /* sortColumns */ {},
        {readRange});

    return New<TReader>(
        std::move(underlyingReader),
        enableTabletIndex,
        enableRowIndex,
        idMapping,
        tabletIndex,
        lowerRowIndex,
        PerformanceCounters_);
}

void TOrderedChunkStore::Save(TSaveContext& context) const
{
    TStoreBase::Save(context);
    TOrderedStoreBase::Save(context);
}

void TOrderedChunkStore::Load(TLoadContext& context)
{
    TStoreBase::Load(context);
    TOrderedStoreBase::Load(context);
}

TKeyComparer TOrderedChunkStore::GetKeyComparer()
{
    return TKeyComparer();
}

ISchemafulUnversionedReaderPtr TOrderedChunkStore::TryCreateCacheBasedReader(
    const TClientChunkReadOptions& chunkReadOptions,
    const TReadRange& readRange,
    const TTableSchemaPtr& readSchema,
    bool enableTabletIndex,
    bool enableRowIndex,
    int tabletIndex,
    i64 lowerRowIndex,
    const TIdMapping& idMapping)
{
    auto chunkState = FindPreloadedChunkState();
    if (!chunkState) {
        return nullptr;
    }

    auto chunkReader = CreateCacheReader(
        chunkState->ChunkMeta->GetChunkId(),
        chunkState->BlockCache);

    auto underlyingReader = CreateSchemafulChunkReader(
        chunkState,
        chunkState->ChunkMeta,
        ReaderConfig_,
        std::move(chunkReader),
        chunkReadOptions,
        readSchema,
        /* sortColumns */ {},
        {readRange});

    return New<TReader>(
        std::move(underlyingReader),
        enableTabletIndex,
        enableRowIndex,
        idMapping,
        tabletIndex,
        lowerRowIndex,
        PerformanceCounters_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
