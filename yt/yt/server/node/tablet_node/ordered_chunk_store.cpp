#include "ordered_chunk_store.h"
#include "tablet.h"
#include "versioned_chunk_meta_manager.h"

#include <yt/server/lib/tablet_node/proto/tablet_manager.pb.h>

#include <yt/server/lib/tablet_node/config.h>

#include <yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/ytlib/chunk_client/chunk_spec.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/ytlib/table_client/cached_versioned_chunk_meta.h>
#include <yt/ytlib/table_client/schemaful_chunk_reader.h>
#include <yt/ytlib/table_client/chunk_state.h>

#include <yt/client/table_client/unversioned_reader.h>
#include <yt/client/table_client/unversioned_row_batch.h>
#include <yt/client/table_client/row_buffer.h>

#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/concurrency/throughput_throttler.h>

namespace NYT::NTabletNode {

using namespace NYTree;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NApi;
using namespace NDataNode;
using namespace NConcurrency;

using NTabletNode::NProto::TAddStoreDescriptor;
using NChunkClient::NProto::TDataStatistics;

////////////////////////////////////////////////////////////////////////////////

struct TOrderedChunkStoreReaderTag
{ };

using TIdMapping = SmallVector<int, TypicalColumnCount>;

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
        i64 lowerRowIndex)
        : UnderlyingReader_(std::move(underlyingReader))
        , TabletIndex_(tabletIndex)
        , EnableTabletIndex_(enableTabletIndex)
        , EnableRowIndex_(enableRowIndex)
        , IdMapping_(idMapping)
        , CurrentRowIndex_(lowerRowIndex)
        , Pool_(TOrderedChunkStoreReaderTag())
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

            updatedRows.push_back(updatedRow);
            ++CurrentRowIndex_;
        }

        return CreateBatchFromUnversionedRows(MakeSharedRange(std::move(updatedRows), this));
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

};

////////////////////////////////////////////////////////////////////////////////

TOrderedChunkStore::TOrderedChunkStore(
    TTabletManagerConfigPtr config,
    TStoreId id,
    TTablet* tablet,
    IBlockCachePtr blockCache,
    TChunkRegistryPtr chunkRegistry,
    TChunkBlockManagerPtr chunkBlockManager,
    TVersionedChunkMetaManagerPtr chunkMetaManager,
    NNative::IClientPtr client,
    const TNodeDescriptor& localDescriptor)
    : TChunkStoreBase(
        config,
        id /*storeId*/,
        id /*chunkId*/,
        NullTimestamp,
        tablet,
        blockCache,
        chunkRegistry,
        chunkBlockManager,
        chunkMetaManager,
        client,
        localDescriptor)
{ }

void TOrderedChunkStore::Initialize(const TAddStoreDescriptor* descriptor)
{
    TChunkStoreBase::Initialize(descriptor);
    if (descriptor) {
        YT_VERIFY(descriptor->has_starting_row_index());
        SetStartingRowIndex(descriptor->starting_row_index());
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
    const TClientBlockReadOptions& blockReadOptions,
    IThroughputThrottlerPtr throttler)
{
    auto chunkReader = GetReaders(throttler).ChunkReader;
    auto asyncChunkMeta = ChunkMetaManager_->GetMeta(
        chunkReader,
        *Schema_,
        blockReadOptions);
    auto chunkMeta = WaitFor(asyncChunkMeta)
        .ValueOrThrow();

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

    auto chunkState = New<TChunkState>(
        GetBlockCache(),
        NChunkClient::NProto::TChunkSpec(),
        nullptr,
        NullTimestamp,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    auto underlyingReader = CreateSchemafulChunkReader(
        chunkState,
        chunkMeta,
        ReaderConfig_,
        std::move(chunkReader),
        blockReadOptions,
        readSchema,
        TKeyColumns(),
        {readRange});

    return New<TReader>(
        std::move(underlyingReader),
        enableTabletIndex,
        enableRowIndex,
        idMapping,
        tabletIndex,
        lowerRowIndex);
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

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
