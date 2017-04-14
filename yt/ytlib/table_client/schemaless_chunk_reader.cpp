#include "cached_versioned_chunk_meta.h"
#include "chunk_reader_base.h"
#include "columnar_chunk_reader_base.h"
#include "config.h"
#include "helpers.h"
#include "name_table.h"
#include "overlapping_reader.h"
#include "private.h"
#include "row_buffer.h"
#include "row_merger.h"
#include "row_sampler.h"
#include "schema.h"
#include "schemaful_reader.h"
#include "schemaless_block_reader.h"
#include "schemaless_chunk_reader.h"
#include "versioned_chunk_reader.h"

#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/native_client.h>

#include <yt/ytlib/table_chunk_format/public.h>
#include <yt/ytlib/table_chunk_format/column_reader.h>
#include <yt/ytlib/table_chunk_format/null_column_reader.h>

#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/data_source.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/multi_reader_base.h>
#include <yt/ytlib/chunk_client/reader_factory.h>
#include <yt/ytlib/chunk_client/replication_reader.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/query_client/column_evaluator.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/numeric_helpers.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NTableChunkFormat;
using namespace NTableChunkFormat::NProto;
using namespace NProto;
using namespace NYPath;
using namespace NYTree;
using namespace NRpc;
using namespace NApi;

using NChunkClient::TDataSliceDescriptor;
using NChunkClient::TReadLimit;
using NChunkClient::TReadRange;
using NChunkClient::TChannel;
using NChunkClient::NProto::TMiscExt;

using NYT::FromProto;
using NYT::TRange;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TableClientLogger;

////////////////////////////////////////////////////////////////////////////////

class TSchemalessChunkReaderBase
    : public virtual ISchemalessChunkReader
{
public:
    TSchemalessChunkReaderBase(
        const TChunkSpec& chunkSpec,
        TChunkReaderConfigPtr config,
        TChunkReaderOptionsPtr options,
        const TChunkId& chunkId,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter,
        const TKeyColumns& keyColumns)
        : ChunkSpec_(chunkSpec)
        , Config_(config)
        , Options_(options)
        , NameTable_(nameTable)
        , ColumnFilter_(columnFilter)
        , KeyColumns_(keyColumns)
        , SystemColumnCount_(GetSystemColumnCount(options))
        , Logger(NLogging::TLogger(TableClientLogger)
            .AddTag("ChunkReaderId: %v", TGuid::Create())
            .AddTag("ChunkId: %v", chunkId))
    {
        if (Config_->SamplingRate) {
            RowSampler_ = CreateChunkRowSampler(
                chunkId,
                Config_->SamplingRate.Get(),
                Config_->SamplingSeed.Get(std::random_device()()));
        }
    }

    virtual const TNameTablePtr& GetNameTable() const override
    {
        return NameTable_;
    }

    virtual TKeyColumns GetKeyColumns() const override
    {
        return KeyColumns_;
    }

    virtual i64 GetTableRowIndex() const override
    {
        return ChunkSpec_.table_row_index() + RowIndex_;
    }

    virtual std::vector<TDataSliceDescriptor> GetUnreadDataSliceDescriptors(
        const TRange<TUnversionedRow>& unreadRows) const override
    {
        Y_UNREACHABLE();
    }

protected:
    const TChunkSpec ChunkSpec_;

    TChunkReaderConfigPtr Config_;
    TChunkReaderOptionsPtr Options_;
    const TNameTablePtr NameTable_;

    const TColumnFilter ColumnFilter_;
    TKeyColumns KeyColumns_;

    i64 RowIndex_ = 0;
    i64 RowCount_ = 0;

    std::unique_ptr<IRowSampler> RowSampler_;
    const int SystemColumnCount_;

    int RowIndexId_ = -1;
    int RangeIndexId_ = -1;
    int TableIndexId_ = -1;

    NLogging::TLogger Logger;

    void InitializeSystemColumnIds()
    {
        try {
            if (Options_->EnableRowIndex) {
                RowIndexId_ = NameTable_->GetIdOrRegisterName(RowIndexColumnName);
            }

            if (Options_->EnableRangeIndex) {
                RangeIndexId_ = NameTable_->GetIdOrRegisterName(RangeIndexColumnName);
            }

            if (Options_->EnableTableIndex) {
                TableIndexId_ = NameTable_->GetIdOrRegisterName(TableIndexColumnName);
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Failed to add system columns to name table for schemaless chunk reader")
                << ex;
        }
    }

    std::vector<TDataSliceDescriptor> GetUnreadDataSliceDescriptorsImpl(
        const TRange<TUnversionedRow>& unreadRows,
        const TMiscExt& misc,
        const NProto::TBlockMetaExt& blockMeta,
        const TChunkSpec& chunkSpec,
        const TReadLimit& lowerLimit,
        const TReadLimit& upperLimit,
        const TKeyColumns& keyColumns,
        i64 rowIndex) const
    {
        i64 rowCount = 0;
        std::vector<TDataSliceDescriptor> unreadDescriptors;

        // Verify row index is in the chunk range
        YCHECK(unreadRows.Size() <= rowIndex);
        rowIndex -= unreadRows.Size();
        i64 lowerRowIndex = lowerLimit.HasRowIndex() ? lowerLimit.GetRowIndex() : 0;
        i64 upperRowIndex = upperLimit.HasRowIndex() ? upperLimit.GetRowIndex() : misc.row_count();
        if (!upperLimit.HasRowIndex() && upperLimit.HasKey()) {
            auto it = std::upper_bound(
                blockMeta.blocks().begin(),
                blockMeta.blocks().end(),
                upperLimit.GetKey(),
                [] (const TOwningKey& key, const TBlockMeta& block) -> bool {
                    auto lastKey = FromProto<TOwningKey>(block.last_key());
                    return key < lastKey;
                });
            if (it != blockMeta.blocks().end()) {
                upperRowIndex = it->chunk_row_count();
            }
        }
        YCHECK(upperRowIndex <= misc.row_count());
        YCHECK(rowIndex >= lowerRowIndex);
        if (rowIndex >= upperRowIndex) {
            return unreadDescriptors;
        }

        // Verify the first unread key is in the chunk range
        auto lowerKey = lowerLimit.HasKey() ? lowerLimit.GetKey() : TOwningKey();
        auto lastChunkKey = FromProto<TOwningKey>(blockMeta.blocks().rbegin()->last_key());
        auto upperKey = upperLimit.HasKey() ? upperLimit.GetKey() : lastChunkKey;
        TOwningKey firstUnreadKey;
        if (!unreadRows.Empty()) {
            firstUnreadKey = GetKeyPrefix(unreadRows[0], keyColumns.size());
        }
        YCHECK(
            !firstUnreadKey || (
                (!lowerKey || CompareRows(firstUnreadKey, lowerKey) >= 0) &&
                (!upperKey || CompareRows(firstUnreadKey, upperKey) <= 0)));

        unreadDescriptors.emplace_back(TDataSliceDescriptor(chunkSpec));

        // Check if whole chunk is unread
        if (rowIndex == lowerRowIndex) {
            return unreadDescriptors;
        }

        auto& chunk = unreadDescriptors[0].ChunkSpecs[0];
        chunk.mutable_lower_limit()->set_row_index(std::max(rowIndex, lowerRowIndex));
        if (firstUnreadKey) {
            ToProto(chunk.mutable_lower_limit()->mutable_key(), firstUnreadKey);
        }
        rowCount = upperRowIndex - rowIndex;

        chunk.set_row_count_override(rowCount);
        i64 dataSize = DivCeil(misc.uncompressed_data_size(), misc.row_count()) * rowCount;
        YCHECK(dataSize > 0);
        chunk.set_uncompressed_data_size_override(dataSize);
        return unreadDescriptors;
    }

};

////////////////////////////////////////////////////////////////////////////////

class THorizontalSchemalessChunkReaderBase
    : public TChunkReaderBase
    , public TSchemalessChunkReaderBase
{
public:
    THorizontalSchemalessChunkReaderBase(
        const TChunkSpec& chunkSpec,
        TChunkReaderConfigPtr config,
        TChunkReaderOptionsPtr options,
        IChunkReaderPtr underlyingReader,
        TNameTablePtr nameTable,
        IBlockCachePtr blockCache,
        const TKeyColumns& keyColumns,
        const TColumnFilter& columnFilter,
        TNullable<int> partitionTag);

    virtual TDataStatistics GetDataStatistics() const override;

protected:
    using TSchemalessChunkReaderBase::Config_;
    using TSchemalessChunkReaderBase::Logger;

    TNameTablePtr ChunkNameTable_ = New<TNameTable>();

    int ChunkKeyColumnCount_ = 0;

    TNullable<int> PartitionTag_;

    int CurrentBlockIndex_ = 0;

    // Maps chunk name table ids into client id.
    // For filtered out columns maps id to -1.
    std::vector<TColumnIdMapping> IdMapping_;

    std::unique_ptr<THorizontalSchemalessBlockReader> BlockReader_;

    TChunkMeta ChunkMeta_;
    NProto::TBlockMetaExt BlockMetaExt_;

    std::vector<int> BlockIndexes_;

    virtual void DoInitializeBlockSequence() = 0;

    void DownloadChunkMeta(std::vector<int> extensionTags, TNullable<int> partitionTag = Null);

    TFuture<void> InitializeBlockSequence();
};

////////////////////////////////////////////////////////////////////////////////

THorizontalSchemalessChunkReaderBase::THorizontalSchemalessChunkReaderBase(
    const TChunkSpec& chunkSpec,
    TChunkReaderConfigPtr config,
    TChunkReaderOptionsPtr options,
    IChunkReaderPtr underlyingReader,
    TNameTablePtr nameTable,
    IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns,
    const TColumnFilter& columnFilter,
    TNullable<int> partitionTag)
    : TChunkReaderBase(
        config,
        underlyingReader,
        blockCache)
    , TSchemalessChunkReaderBase(
        chunkSpec,
        config,
        options,
        underlyingReader->GetChunkId(),
        nameTable,
        columnFilter,
        keyColumns)
    , PartitionTag_(std::move(partitionTag))
{ }

TFuture<void> THorizontalSchemalessChunkReaderBase::InitializeBlockSequence()
{
    YCHECK(BlockIndexes_.empty());

    InitializeSystemColumnIds();

    DoInitializeBlockSequence();

    LOG_DEBUG("Reading %v blocks", BlockIndexes_.size());

    std::vector<TBlockFetcher::TBlockInfo> blocks;
    for (int blockIndex : BlockIndexes_) {
        YCHECK(blockIndex < BlockMetaExt_.blocks_size());
        auto& blockMeta = BlockMetaExt_.blocks(blockIndex);
        TBlockFetcher::TBlockInfo blockInfo;
        blockInfo.Index = blockMeta.block_index();
        blockInfo.UncompressedDataSize = blockMeta.uncompressed_size();
        blockInfo.Priority = blocks.size();
        blocks.push_back(blockInfo);
    }

    return DoOpen(std::move(blocks), GetProtoExtension<TMiscExt>(ChunkMeta_.extensions()));
}

void THorizontalSchemalessChunkReaderBase::DownloadChunkMeta(std::vector<int> extensionTags, TNullable<int> partitionTag)
{
    extensionTags.push_back(TProtoExtensionTag<TMiscExt>::Value);
    extensionTags.push_back(TProtoExtensionTag<NProto::TBlockMetaExt>::Value);
    extensionTags.push_back(TProtoExtensionTag<NProto::TNameTableExt>::Value);
    auto asynChunkMeta = UnderlyingReader_->GetMeta(
        Config_->WorkloadDescriptor,
        partitionTag,
        extensionTags);
    ChunkMeta_ = WaitFor(asynChunkMeta)
        .ValueOrThrow();

    YCHECK(ChunkMeta_.version() == static_cast<int>(ETableChunkFormat::SchemalessHorizontal));

    BlockMetaExt_ = GetProtoExtension<NProto::TBlockMetaExt>(ChunkMeta_.extensions());

    auto nameTableExt = GetProtoExtension<NProto::TNameTableExt>(ChunkMeta_.extensions());
    try {
        FromProto(&ChunkNameTable_, nameTableExt);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION(
            EErrorCode::CorruptedNameTable,
            "Failed to deserialize name table for schemaless chunk reader")
            << TErrorAttribute("chunk_id", UnderlyingReader_->GetChunkId())
            << ex;
    }

    IdMapping_.reserve(ChunkNameTable_->GetSize());

    if (ColumnFilter_.All) {
        try {
            for (int chunkNameId = 0; chunkNameId < ChunkNameTable_->GetSize(); ++chunkNameId) {
                auto name = ChunkNameTable_->GetName(chunkNameId);
                auto id = NameTable_->GetIdOrRegisterName(name);
                IdMapping_.push_back({chunkNameId, id});
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Failed to update name table for schemaless chunk reader")
                << TErrorAttribute("chunk_id", UnderlyingReader_->GetChunkId())
                << ex;
        }
    } else {
        for (int chunkNameId = 0; chunkNameId < ChunkNameTable_->GetSize(); ++chunkNameId) {
            IdMapping_.push_back({chunkNameId, -1});
        }

        for (auto id : ColumnFilter_.Indexes) {
            auto name = NameTable_->GetName(id);
            auto chunkNameId = ChunkNameTable_->FindId(name);
            if (chunkNameId) {
                IdMapping_[chunkNameId.Get()] = {chunkNameId.Get(), id};
            }
        }
    }
}

TDataStatistics THorizontalSchemalessChunkReaderBase::GetDataStatistics() const
{
    auto dataStatistics = TChunkReaderBase::GetDataStatistics();
    dataStatistics.set_row_count(RowCount_);
    return dataStatistics;
}

////////////////////////////////////////////////////////////////////////////////

class THorizontalSchemalessRangeChunkReader
    : public THorizontalSchemalessChunkReaderBase
{
public:
    THorizontalSchemalessRangeChunkReader(
        const TChunkSpec& chunkSpec,
        TChunkReaderConfigPtr config,
        TChunkReaderOptionsPtr options,
        IChunkReaderPtr underlyingReader,
        TNameTablePtr nameTable,
        IBlockCachePtr blockCache,
        const TKeyColumns& keyColumns,
        const TColumnFilter& columnFilter,
        const TReadRange& readRange,
        TNullable<int> partitionTag);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;
    virtual std::vector<TDataSliceDescriptor> GetUnreadDataSliceDescriptors(const TRange<TUnversionedRow>& unreadRows) const override;

private:
    TReadRange ReadRange_;

    virtual void DoInitializeBlockSequence() override;

    virtual void InitFirstBlock() override;
    virtual void InitNextBlock() override;

    void CreateBlockSequence(int beginIndex, int endIndex);

    void InitializeBlockSequencePartition();
    void InitializeBlockSequenceSorted();
    void InitializeBlockSequenceUnsorted();
};

DEFINE_REFCOUNTED_TYPE(THorizontalSchemalessRangeChunkReader)

////////////////////////////////////////////////////////////////////////////////

THorizontalSchemalessRangeChunkReader::THorizontalSchemalessRangeChunkReader(
    const TChunkSpec& chunkSpec,
    TChunkReaderConfigPtr config,
    TChunkReaderOptionsPtr options,
    IChunkReaderPtr underlyingReader,
    TNameTablePtr nameTable,
    IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns,
    const TColumnFilter& columnFilter,
    const TReadRange& readRange,
    TNullable<int> partitionTag)
    : THorizontalSchemalessChunkReaderBase(
        chunkSpec,
        std::move(config),
        std::move(options),
        std::move(underlyingReader),
        std::move(nameTable),
        std::move(blockCache),
        keyColumns,
        columnFilter,
        std::move(partitionTag))
    , ReadRange_(readRange)
{
    LOG_DEBUG("Reading range %v", ReadRange_);

    // Ready event must be set only when all initialization is finished and
    // RowIndex_ is set into proper value.
    // Must be called after the object is constructed and vtable initialized.
    ReadyEvent_ = BIND(&THorizontalSchemalessRangeChunkReader::InitializeBlockSequence, MakeStrong(this))
        .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
        .Run()
        .Apply(BIND([this, this_ = MakeStrong(this)] () {
            if (InitFirstBlockNeeded_) {
                InitFirstBlock();
                InitFirstBlockNeeded_ = false;
            }
        }));
}

void THorizontalSchemalessRangeChunkReader::DoInitializeBlockSequence()
{
    if (PartitionTag_) {
        InitializeBlockSequencePartition();
    } else {
        bool readSorted = ReadRange_.LowerLimit().HasKey() || ReadRange_.UpperLimit().HasKey() || !KeyColumns_.empty();
        if (readSorted) {
            InitializeBlockSequenceSorted();
        } else {
            InitializeBlockSequenceUnsorted();
        }
    }
}

void THorizontalSchemalessRangeChunkReader::InitializeBlockSequenceSorted()
{
    std::vector<int> extensionTags = {
        TProtoExtensionTag<NProto::TKeyColumnsExt>::Value,
    };

    DownloadChunkMeta(extensionTags);

    auto misc = GetProtoExtension<TMiscExt>(ChunkMeta_.extensions());
    if (!misc.sorted()) {
        THROW_ERROR_EXCEPTION("Requested a sorted read for an unsorted chunk");
    }

    auto keyColumnsExt = GetProtoExtension<NProto::TKeyColumnsExt>(ChunkMeta_.extensions());
    TKeyColumns chunkKeyColumns = NYT::FromProto<TKeyColumns>(keyColumnsExt);
    ChunkKeyColumnCount_ = chunkKeyColumns.size();

    ValidateKeyColumns(KeyColumns_, chunkKeyColumns, Options_->DynamicTable);

    if (KeyColumns_.empty()) {
        KeyColumns_ = chunkKeyColumns;
    }

    TNullable<int> keyColumnCount;
    if (Options_->DynamicTable) {
        keyColumnCount = KeyColumns_.size();
    }

    int beginIndex = std::max(
        ApplyLowerRowLimit(BlockMetaExt_, ReadRange_.LowerLimit()),
        ApplyLowerKeyLimit(BlockMetaExt_, ReadRange_.LowerLimit(), keyColumnCount));
    int endIndex = std::min(
        ApplyUpperRowLimit(BlockMetaExt_, ReadRange_.UpperLimit()),
        ApplyUpperKeyLimit(BlockMetaExt_, ReadRange_.UpperLimit(), keyColumnCount));

    CreateBlockSequence(beginIndex, endIndex);
}

void THorizontalSchemalessRangeChunkReader::InitializeBlockSequencePartition()
{
    YCHECK(ReadRange_.LowerLimit().IsTrivial());
    YCHECK(ReadRange_.UpperLimit().IsTrivial());

    DownloadChunkMeta(std::vector<int>(), PartitionTag_);
    CreateBlockSequence(0, BlockMetaExt_.blocks_size());
}

void THorizontalSchemalessRangeChunkReader::InitializeBlockSequenceUnsorted()
{
    DownloadChunkMeta(std::vector<int>());

    CreateBlockSequence(
        ApplyLowerRowLimit(BlockMetaExt_, ReadRange_.LowerLimit()),
        ApplyUpperRowLimit(BlockMetaExt_, ReadRange_.UpperLimit()));
}

void THorizontalSchemalessRangeChunkReader::CreateBlockSequence(int beginIndex, int endIndex)
{
    for (int index = beginIndex; index < endIndex; ++index) {
        BlockIndexes_.push_back(index);
    }
}

void THorizontalSchemalessRangeChunkReader::InitFirstBlock()
{
    int blockIndex = BlockIndexes_[CurrentBlockIndex_];
    const auto& blockMeta = BlockMetaExt_.blocks(blockIndex);

    YCHECK(CurrentBlock_ && CurrentBlock_.IsSet());
    BlockReader_.reset(new THorizontalSchemalessBlockReader(
        CurrentBlock_.Get().ValueOrThrow(),
        blockMeta,
        IdMapping_,
        ChunkKeyColumnCount_,
        KeyColumns_.size(),
        SystemColumnCount_));

    RowIndex_ = blockMeta.chunk_row_count() - blockMeta.row_count();

    int keyColumnCount = std::max(ChunkKeyColumnCount_, static_cast<int>(KeyColumns_.size()));
    CheckBlockUpperLimits(BlockMetaExt_.blocks(blockIndex), ReadRange_.UpperLimit(), keyColumnCount);

    const auto& lowerLimit = ReadRange_.LowerLimit();

    if (lowerLimit.HasRowIndex() && RowIndex_ < lowerLimit.GetRowIndex()) {
        YCHECK(BlockReader_->SkipToRowIndex(lowerLimit.GetRowIndex() - RowIndex_));
        RowIndex_ = lowerLimit.GetRowIndex();
    }

    if (lowerLimit.HasKey()) {
        auto blockRowIndex = BlockReader_->GetRowIndex();
        YCHECK(BlockReader_->SkipToKey(lowerLimit.GetKey().Get()));
        RowIndex_ += BlockReader_->GetRowIndex() - blockRowIndex;
    }
}

void THorizontalSchemalessRangeChunkReader::InitNextBlock()
{
    ++CurrentBlockIndex_;
    InitFirstBlock();
}

bool THorizontalSchemalessRangeChunkReader::Read(std::vector<TUnversionedRow>* rows)
{
    YCHECK(rows->capacity() > 0);

    MemoryPool_.Clear();
    rows->clear();

    if (!BeginRead()) {
        // Not ready yet.
        return true;
    }

    if (!BlockReader_) {
        // Nothing to read from chunk.
        return false;
    }

    if (BlockEnded_) {
        BlockReader_.reset();
        return OnBlockEnded();
    }

    i64 dataWeight = 0;
    while (rows->size() < rows->capacity() && dataWeight < Config_->MaxDataSizePerRead) {
        if ((CheckRowLimit_ && RowIndex_ >= ReadRange_.UpperLimit().GetRowIndex()) ||
            (CheckKeyLimit_ && CompareRows(BlockReader_->GetKey(), ReadRange_.UpperLimit().GetKey()) >= 0))
        {
            BlockEnded_ = true;
            return true;
        }

        if (!RowSampler_ || RowSampler_->ShouldTakeRow(GetTableRowIndex())) {
            auto row = BlockReader_->GetRow(&MemoryPool_);
            if (Options_->EnableRangeIndex) {
                *row.End() = MakeUnversionedInt64Value(ChunkSpec_.range_index(), RangeIndexId_);
                row.SetCount(row.GetCount() + 1);
            }
            if (Options_->EnableTableIndex) {
                *row.End() = MakeUnversionedInt64Value(ChunkSpec_.table_index(), TableIndexId_);
                row.SetCount(row.GetCount() + 1);
            }
            if (Options_->EnableRowIndex) {
                *row.End() = MakeUnversionedInt64Value(GetTableRowIndex(), RowIndexId_);
                row.SetCount(row.GetCount() + 1);
            }

            rows->push_back(row);
            dataWeight += GetDataWeight(rows->back());
            ++RowCount_;
        }
        ++RowIndex_;

        if (!BlockReader_->NextRow()) {
            BlockEnded_ = true;
            return true;
        }
    }

    return true;
}

std::vector<TDataSliceDescriptor> THorizontalSchemalessRangeChunkReader::GetUnreadDataSliceDescriptors(
    const TRange<TUnversionedRow>& unreadRows) const
{
    if (BlockIndexes_.size() == 0) {
        return std::vector<TDataSliceDescriptor>();
    }
    return GetUnreadDataSliceDescriptorsImpl(
        unreadRows,
        GetProtoExtension<TMiscExt>(ChunkMeta_.extensions()),
        BlockMetaExt_,
        ChunkSpec_,
        ReadRange_.LowerLimit(),
        ReadRange_.UpperLimit(),
        KeyColumns_,
        RowIndex_);
}

////////////////////////////////////////////////////////////////////////////////

class THorizontalSchemalessLookupChunkReader
    : public THorizontalSchemalessChunkReaderBase
{
public:
    THorizontalSchemalessLookupChunkReader(
        const TChunkSpec& chunkSpec,
        TChunkReaderConfigPtr config,
        TChunkReaderOptionsPtr options,
        NChunkClient::IChunkReaderPtr underlyingReader,
        TNameTablePtr nameTable,
        NChunkClient::IBlockCachePtr blockCache,
        const TKeyColumns& keyColumns,
        const TColumnFilter& columnFilter,
        const TSharedRange<TKey>& keys,
        TChunkReaderPerformanceCountersPtr performanceCounters,
        TNullable<int> partitionTag = Null);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;

private:
    const TSharedRange<TKey> Keys_;
    const TChunkReaderPerformanceCountersPtr PerformanceCounters_;
    std::vector<bool> KeyFilterTest_;

    virtual void DoInitializeBlockSequence() override;

    virtual void InitFirstBlock() override;
    virtual void InitNextBlock() override;

    bool DoRead(std::vector<TUnversionedRow>* rows);

};

DEFINE_REFCOUNTED_TYPE(THorizontalSchemalessLookupChunkReader)

////////////////////////////////////////////////////////////////////////////////

THorizontalSchemalessLookupChunkReader::THorizontalSchemalessLookupChunkReader(
    const TChunkSpec& chunkSpec,
    TChunkReaderConfigPtr config,
    TChunkReaderOptionsPtr options,
    NChunkClient::IChunkReaderPtr underlyingReader,
    TNameTablePtr nameTable,
    NChunkClient::IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns,
    const TColumnFilter& columnFilter,
    const TSharedRange<TKey>& keys,
    TChunkReaderPerformanceCountersPtr performanceCounters,
    TNullable<int> partitionTag)
    : THorizontalSchemalessChunkReaderBase(
        chunkSpec,
        std::move(config),
        std::move(options),
        std::move(underlyingReader),
        std::move(nameTable),
        std::move(blockCache),
        keyColumns,
        columnFilter,
        std::move(partitionTag))
    , Keys_(keys)
    , PerformanceCounters_(std::move(performanceCounters))
    , KeyFilterTest_(Keys_.Size(), true)
{
    // Ready event must be set only when all initialization is finished and
    // RowIndex_ is set into proper value.
    // Must be called after the object is constructed and vtable initialized.
    ReadyEvent_ = BIND(&THorizontalSchemalessLookupChunkReader::InitializeBlockSequence, MakeStrong(this))
        .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
        .Run()
        .Apply(BIND([this, this_ = MakeStrong(this)] () {
            if (InitFirstBlockNeeded_) {
                InitFirstBlock();
                InitFirstBlockNeeded_ = false;
            }
        }));
}

void THorizontalSchemalessLookupChunkReader::DoInitializeBlockSequence()
{
    std::vector<int> extensionTags = {
        TProtoExtensionTag<TKeyColumnsExt>::Value,
    };

    DownloadChunkMeta(extensionTags, PartitionTag_);

    auto misc = GetProtoExtension<TMiscExt>(ChunkMeta_.extensions());
    if (!misc.sorted()) {
        THROW_ERROR_EXCEPTION("Requested lookup for an unsorted chunk");
    }
    if (!misc.unique_keys()) {
        THROW_ERROR_EXCEPTION("Requested lookup for a chunk without unique_keys restriction");
    }

    auto keyColumnsExt = GetProtoExtension<TKeyColumnsExt>(ChunkMeta_.extensions());
    TKeyColumns chunkKeyColumns = NYT::FromProto<TKeyColumns>(keyColumnsExt);
    ChunkKeyColumnCount_ = chunkKeyColumns.size();

    ValidateKeyColumns(KeyColumns_, chunkKeyColumns, Options_->DynamicTable);

    for (const auto& key : Keys_) {
        TReadLimit readLimit;
        readLimit.SetKey(TOwningKey(key));

        int index = ApplyLowerKeyLimit(BlockMetaExt_, readLimit, KeyColumns_.size());
        if (index == BlockMetaExt_.blocks_size()) {
            break;
        }

        if (BlockIndexes_.empty() || BlockIndexes_.back() != index) {
            BlockIndexes_.push_back(index);
        }
    }
}

bool THorizontalSchemalessLookupChunkReader::Read(std::vector<TUnversionedRow>* rows)
{
    bool result = DoRead(rows);
    if (PerformanceCounters_) {
        PerformanceCounters_->StaticChunkRowLookupCount += rows->size();
    }
    return result;
}

bool THorizontalSchemalessLookupChunkReader::DoRead(std::vector<TUnversionedRow>* rows)
{
    YCHECK(rows->capacity() > 0);

    MemoryPool_.Clear();
    rows->clear();

    if (!BeginRead()) {
        // Not ready yet.
        return true;
    }

    if (!BlockReader_) {
        // Nothing to read from chunk.
        if (RowCount_ == Keys_.Size()) {
            return false;
        }

        while (rows->size() < rows->capacity() && RowCount_ < Keys_.Size()) {
            rows->push_back(TUnversionedRow());
            ++RowCount_;
        }
        return true;
    }

    if (BlockEnded_) {
        BlockReader_.reset();
        OnBlockEnded();
        return true;
    }

    while (rows->size() < rows->capacity()) {
        if (RowCount_ == Keys_.Size()) {
            BlockEnded_ = true;
            return true;
        }

        if (!KeyFilterTest_[RowCount_]) {
            rows->push_back(TUnversionedRow());
        } else {
            const auto& key = Keys_[RowCount_];
            if (!BlockReader_->SkipToKey(key)) {
                BlockEnded_ = true;
                return true;
            }

            if (key == BlockReader_->GetKey()) {
                auto row = BlockReader_->GetRow(&MemoryPool_);
                rows->push_back(row);

                int blockIndex = BlockIndexes_[CurrentBlockIndex_];
                const auto& blockMeta = BlockMetaExt_.blocks(blockIndex);
                RowIndex_ = blockMeta.chunk_row_count() - blockMeta.row_count() + BlockReader_->GetRowIndex();
            } else {
                rows->push_back(TUnversionedRow());
            }
        }
        ++RowCount_;
    }

    return true;
}

void THorizontalSchemalessLookupChunkReader::InitFirstBlock()
{
    int blockIndex = BlockIndexes_[CurrentBlockIndex_];
    const auto& blockMeta = BlockMetaExt_.blocks(blockIndex);

    BlockReader_.reset(new THorizontalSchemalessBlockReader(
        CurrentBlock_.Get().ValueOrThrow(),
        blockMeta,
        IdMapping_,
        ChunkKeyColumnCount_,
        KeyColumns_.size(),
        SystemColumnCount_));
}

void THorizontalSchemalessLookupChunkReader::InitNextBlock()
{
    ++CurrentBlockIndex_;
    InitFirstBlock();
}

////////////////////////////////////////////////////////////////////////////////

class TColumnarSchemalessRangeChunkReader
    : public TSchemalessChunkReaderBase
    , public TColumnarRangeChunkReaderBase
{
public:
    TColumnarSchemalessRangeChunkReader(
        const TChunkSpec& chunkSpec,
        TChunkReaderConfigPtr config,
        TChunkReaderOptionsPtr options,
        IChunkReaderPtr underlyingReader,
        TNameTablePtr nameTable,
        IBlockCachePtr blockCache,
        const TKeyColumns& keyColumns,
        const TColumnFilter& columnFilter,
        const TReadRange& readRange)
        : TSchemalessChunkReaderBase(
            chunkSpec,
            config,
            options,
            underlyingReader->GetChunkId(),
            nameTable,
            columnFilter,
            keyColumns)
        , TColumnarRangeChunkReaderBase(
            config,
            underlyingReader,
            blockCache)
    {
        LOG_DEBUG("Reading range %v", readRange);

        LowerLimit_ = readRange.LowerLimit();
        UpperLimit_ = readRange.UpperLimit();

        ReadyEvent_ = BIND(&TColumnarSchemalessRangeChunkReader::InitializeBlockSequence, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
            .Run();
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        YCHECK(rows->capacity() > 0);
        rows->clear();
        Pool_.Clear();

        if (!ReadyEvent_.IsSet() || !ReadyEvent_.Get().IsOK()) {
            return true;
        }

        if (Completed_) {
            return false;
        }

        i64 dataWeight = 0;
        while (rows->size() < rows->capacity()) {
            ResetExhaustedColumns();

            // Define how many to read.
            i64 rowLimit = static_cast<i64>(rows->capacity() - rows->size());

            // Each read must be fully below or fully above SafeUpperRowLimit,
            // to determine if we should read and validate keys.
            if (RowIndex_ < SafeUpperRowIndex_) {
                rowLimit = std::min(SafeUpperRowIndex_ - RowIndex_, rowLimit);
            } else {
                rowLimit = std::min(HardUpperRowIndex_ - RowIndex_, rowLimit);
            }

            for (const auto& column : Columns_) {
                rowLimit = std::min(column.ColumnReader->GetReadyUpperRowIndex() - RowIndex_, rowLimit);
            }

            YCHECK(rowLimit > 0);

            if (!LowerKeyLimitReached_) {
                auto keys = ReadKeys(rowLimit);

                i64 deltaIndex = 0;
                for (; deltaIndex < rowLimit; ++deltaIndex) {
                    if (keys[deltaIndex] >= LowerLimit_.GetKey()) {
                        break;
                    }
                }

                rowLimit -= deltaIndex;
                RowIndex_ += deltaIndex;

                // Rewind row column readers to proper row index.
                for (auto& reader : RowColumnReaders_) {
                    reader->SkipToRowIndex(RowIndex_);
                }
                if (SchemalessReader_) {
                    SchemalessReader_->SkipToRowIndex(RowIndex_);
                }

                LowerKeyLimitReached_ = (rowLimit > 0);

                // We could have overcome upper limit, we must check it.
                if (RowIndex_ >= SafeUpperRowIndex_ && UpperLimit_.HasKey()) {
                    auto keyRange = MakeRange(keys.data() + deltaIndex, keys.data() + keys.size());
                    while (rowLimit > 0 && keyRange[rowLimit - 1] >= UpperLimit_.GetKey()) {
                        --rowLimit;
                        Completed_ = true;
                    }
                }

            } else if (RowIndex_ >= SafeUpperRowIndex_ && UpperLimit_.HasKey()) {
                auto keys = ReadKeys(rowLimit);
                while (rowLimit > 0 && keys[rowLimit - 1] >= UpperLimit_.GetKey()) {
                    --rowLimit;
                    Completed_ = true;
                }
            } else {
                // We do not read keys, so we must skip rows for key readers.
                for (auto& reader : KeyColumnReaders_) {
                    reader->SkipToRowIndex(RowIndex_ + rowLimit);
                }
            }

            dataWeight += ReadRows(rowLimit, rows);

            RowIndex_ += rowLimit;

            if (RowIndex_ == HardUpperRowIndex_) {
                Completed_ = true;
            }

            if (Completed_ || !TryFetchNextRow() ||
                dataWeight > TSchemalessChunkReaderBase::Config_->MaxDataSizePerRead)
            {
                break;
            }
        }

        if (RowSampler_) {
            i64 insertIndex = 0;

            std::vector<TUnversionedRow>& rowsRef = *rows;
            for (i64 rowIndex = 0; rowIndex < rows->size(); ++rowIndex) {
                i64 tableRowIndex = ChunkSpec_.table_row_index() + RowIndex_ - rows->size() + rowIndex;
                if (RowSampler_->ShouldTakeRow(tableRowIndex)) {
                    rowsRef[insertIndex] = rowsRef[rowIndex];
                    ++insertIndex;
                }
            }
            rows->resize(insertIndex);
        }

        RowCount_ += rows->size();

        return true;
    }

    virtual std::vector<TDataSliceDescriptor> GetUnreadDataSliceDescriptors(
        const TRange<TUnversionedRow>& unreadRows) const override
    {
        if (Completed_ && unreadRows.Size() == 0) {
            return std::vector<TDataSliceDescriptor>();
        }
        return GetUnreadDataSliceDescriptorsImpl(
            unreadRows,
            ChunkMeta_->Misc(),
            ChunkMeta_->BlockMeta(),
            ChunkSpec_,
            LowerLimit_,
            UpperLimit_,
            KeyColumns_,
            RowIndex_);
    }

    virtual TDataStatistics GetDataStatistics() const override
    {
        auto dataStatistics = TColumnarRangeChunkReaderBase::GetDataStatistics();
        dataStatistics.set_row_count(RowCount_);
        return dataStatistics;
    }

private:
    std::vector<IUnversionedColumnReader*> RowColumnReaders_;
    std::vector<IUnversionedColumnReader*> KeyColumnReaders_;

    ISchemalessColumnReader* SchemalessReader_ = nullptr;

    bool Completed_ = false;
    bool LowerKeyLimitReached_ = false;

    TChunkedMemoryPool Pool_;

    void InitializeBlockSequence()
    {
        YCHECK(ChunkSpec_.chunk_meta().version() == static_cast<int>(ETableChunkFormat::UnversionedColumnar));
        InitializeSystemColumnIds();

        TNameTablePtr chunkNameTable;

        if (Options_->DynamicTable) {
            auto chunkMeta = ChunkSpec_.chunk_meta();
            ChunkMeta_ = New<TColumnarChunkMeta>(std::move(chunkMeta));
            chunkNameTable = TNameTable::FromSchema(ChunkMeta_->ChunkSchema());
        } else {
            // Download chunk meta.
            std::vector<int> extensionTags = {
                TProtoExtensionTag<TMiscExt>::Value,
                TProtoExtensionTag<TTableSchemaExt>::Value,
                TProtoExtensionTag<TBlockMetaExt>::Value,
                TProtoExtensionTag<TColumnMetaExt>::Value,
                TProtoExtensionTag<TNameTableExt>::Value
            };

            auto asynChunkMeta = UnderlyingReader_->GetMeta(
                TColumnarRangeChunkReaderBase::Config_->WorkloadDescriptor,
                Null,
                extensionTags);
            auto chunkMeta = WaitFor(asynChunkMeta)
                .ValueOrThrow();

            chunkNameTable = FromProto<TNameTablePtr>(GetProtoExtension<TNameTableExt>(
                chunkMeta.extensions()));

            ChunkMeta_ = New<TColumnarChunkMeta>(std::move(chunkMeta));
        }

        // Minimum prefix of key columns, that must be included in column filter.
        int minKeyColumnCount = 0;
        if (UpperLimit_.HasKey()) {
            minKeyColumnCount = std::max(minKeyColumnCount, UpperLimit_.GetKey().GetCount());
        }
        if (LowerLimit_.HasKey()) {
            minKeyColumnCount = std::max(minKeyColumnCount, LowerLimit_.GetKey().GetCount());
        }
        bool sortedRead = minKeyColumnCount > 0 || !KeyColumns_.empty();

        if (sortedRead && !ChunkMeta_->Misc().sorted()) {
            THROW_ERROR_EXCEPTION("Requested a sorted read for an unsorted chunk");
        }

        ValidateKeyColumns(
            KeyColumns_,
            ChunkMeta_->ChunkSchema().GetKeyColumns(),
            Options_->DynamicTable);

        // Cannot read more key columns than stored in chunk, even if range keys are longer.
        minKeyColumnCount = std::min(minKeyColumnCount, ChunkMeta_->ChunkSchema().GetKeyColumnCount());

        if (UpperLimit_.HasKey() || LowerLimit_.HasKey()) {
            ChunkMeta_->InitBlockLastKeys(KeyColumns_.empty()
                ? ChunkMeta_->ChunkSchema().GetKeyColumns()
                : KeyColumns_);
        }

        // Define columns to read.
        std::vector<TColumnIdMapping> schemalessIdMapping;
        schemalessIdMapping.resize(chunkNameTable->GetSize(), {-1, -1});

        std::vector<int> schemaColumnIndexes;
        bool readSchemalessColumns = false;
        if (ColumnFilter_.All) {
            for (int index = 0; index < ChunkMeta_->ChunkSchema().Columns().size(); ++index) {
                schemaColumnIndexes.push_back(index);
            }

            for (
                int chunkColumnId = ChunkMeta_->ChunkSchema().Columns().size();
                chunkColumnId < chunkNameTable->GetSize();
                ++chunkColumnId)
            {
                readSchemalessColumns = true;
                schemalessIdMapping[chunkColumnId].ChunkSchemaIndex = chunkColumnId;
                schemalessIdMapping[chunkColumnId].ReaderSchemaIndex = NameTable_->GetIdOrRegisterName(
                    chunkNameTable->GetName(chunkColumnId));
            }
        } else {
            auto filterIndexes = yhash_set<int>(ColumnFilter_.Indexes.begin(), ColumnFilter_.Indexes.end());
            for (int chunkColumnId = 0; chunkColumnId < chunkNameTable->GetSize(); ++chunkColumnId) {
                auto nameTableIndex = NameTable_->GetIdOrRegisterName(chunkNameTable->GetName(chunkColumnId));
                if (filterIndexes.has(nameTableIndex)) {
                    if (chunkColumnId < ChunkMeta_->ChunkSchema().Columns().size()) {
                        schemaColumnIndexes.push_back(chunkColumnId);
                    } else {
                        readSchemalessColumns = true;
                        schemalessIdMapping[chunkColumnId].ChunkSchemaIndex = chunkColumnId;
                        schemalessIdMapping[chunkColumnId].ReaderSchemaIndex = nameTableIndex;

                    }
                }
            }
        }

        // Create column readers.
        for (int valueIndex = 0; valueIndex < schemaColumnIndexes.size(); ++valueIndex) {
            auto columnIndex = schemaColumnIndexes[valueIndex];
            auto columnReader = CreateUnversionedColumnReader(
                ChunkMeta_->ChunkSchema().Columns()[columnIndex],
                ChunkMeta_->ColumnMeta().columns(columnIndex),
                valueIndex,
                NameTable_->GetIdOrRegisterName(ChunkMeta_->ChunkSchema().Columns()[columnIndex].Name));

            RowColumnReaders_.emplace_back(columnReader.get());
            Columns_.emplace_back(std::move(columnReader), columnIndex);
        }

        if (readSchemalessColumns) {
            auto columnReader = CreateSchemalessColumnReader(
                ChunkMeta_->ColumnMeta().columns(ChunkMeta_->ChunkSchema().Columns().size()),
                schemalessIdMapping);
            SchemalessReader_ = columnReader.get();

            Columns_.emplace_back(
                std::move(columnReader),
                ChunkMeta_->ChunkSchema().Columns().size());
        }

        for (int keyIndex = 0; keyIndex < minKeyColumnCount; ++keyIndex) {
            auto columnReader = CreateUnversionedColumnReader(
                ChunkMeta_->ChunkSchema().Columns()[keyIndex],
                ChunkMeta_->ColumnMeta().columns(keyIndex),
                keyIndex,
                keyIndex); // Column id doesn't really matter.
            KeyColumnReaders_.emplace_back(columnReader.get());

            Columns_.emplace_back(std::move(columnReader), keyIndex);
        }

        for (int keyIndex = minKeyColumnCount; keyIndex < KeyColumns_.size(); ++keyIndex) {
            auto columnReader = CreateUnversionedNullColumnReader(
                keyIndex,
                keyIndex);
            KeyColumnReaders_.emplace_back(columnReader.get());

            Columns_.emplace_back(std::move(columnReader), -1);
        }

        InitLowerRowIndex();
        InitUpperRowIndex();

        LOG_DEBUG("Initialized row index limits (LowerRowIndex: %v, SafeUpperRowIndex: %v, HardUpperRowIndex: %v)",
            LowerRowIndex_,
            SafeUpperRowIndex_,
            HardUpperRowIndex_);

        if (LowerRowIndex_ < HardUpperRowIndex_) {
            // We must continue initialization and set RowIndex_ before
            // ReadyEvent is set for the first time.
            InitBlockFetcher();
            WaitFor(RequestFirstBlocks())
                .ThrowOnError();

            ResetExhaustedColumns();
            Initialize(MakeRange(KeyColumnReaders_));
            RowIndex_ = LowerRowIndex_;
            LowerKeyLimitReached_ = !LowerLimit_.HasKey();

            LOG_DEBUG("Initialized start row index (LowerKeyLimitReached: %v, RowIndex: %v)",
                LowerKeyLimitReached_,
                RowIndex_);

            if (RowIndex_ >= HardUpperRowIndex_) {
                Completed_ = true;
            }
        } else {
            Completed_ = true;
        }
    }

    std::vector<TKey> ReadKeys(i64 rowCount)
    {
        std::vector<TKey> keys;

        for (i64 index = 0; index < rowCount; ++index) {
            auto key = TMutableKey::Allocate(
                &Pool_,
                KeyColumnReaders_.size());
            key.SetCount(KeyColumnReaders_.size());
            keys.push_back(key);
        }

        auto range = TMutableRange<TMutableKey>(
            static_cast<TMutableKey*>(keys.data()),
            static_cast<TMutableKey*>(keys.data() + rowCount));

        for (auto& columnReader : KeyColumnReaders_) {
            columnReader->ReadValues(range);
        }
        return keys;
    }

    //! Returns read data weight.
    i64 ReadRows(i64 rowCount, std::vector<TUnversionedRow>* rows)
    {
        std::vector<ui32> schemalessColumnCount(rowCount, 0);
        if (SchemalessReader_) {
            SchemalessReader_->GetValueCounts(TMutableRange<ui32>(
                schemalessColumnCount.data(),
                schemalessColumnCount.size()));
        }

        int rangeBegin = rows->size();
        for (i64 index = 0; index < rowCount; ++index) {
            auto row = TMutableUnversionedRow::Allocate(
                &Pool_,
                RowColumnReaders_.size() + schemalessColumnCount[index] + SystemColumnCount_);
            row.SetCount(RowColumnReaders_.size());
            rows->push_back(row);
        }

        auto range = TMutableRange<TMutableUnversionedRow>(
            static_cast<TMutableUnversionedRow*>(rows->data() + rangeBegin),
            static_cast<TMutableUnversionedRow*>(rows->data() + rangeBegin + rowCount));

        // Read values.
        for (auto& columnReader : RowColumnReaders_) {
            columnReader->ReadValues(range);
        }

        if (SchemalessReader_) {
            SchemalessReader_->ReadValues(range);
        }

        i64 dataWeight = 0;

        // Append system columns.
        for (i64 index = 0; index < rowCount; ++index) {
            auto row = range[index];
            if (Options_->EnableRangeIndex) {
                *row.End() = MakeUnversionedInt64Value(ChunkSpec_.range_index(), RangeIndexId_);
                row.SetCount(row.GetCount() + 1);
            }
            if (Options_->EnableTableIndex) {
                *row.End() = MakeUnversionedInt64Value(ChunkSpec_.table_index(), TableIndexId_);
                row.SetCount(row.GetCount() + 1);
            }
            if (Options_->EnableRowIndex) {
                *row.End() = MakeUnversionedInt64Value(
                    GetTableRowIndex() + index,
                    RowIndexId_);
                row.SetCount(row.GetCount() + 1);
            }

            dataWeight += GetDataWeight(row);
        }

        return dataWeight;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TColumnarSchemalessLookupChunkReader
    : public TSchemalessChunkReaderBase
    , public TColumnarLookupChunkReaderBase
{
public:
    TColumnarSchemalessLookupChunkReader(
        const TChunkSpec& chunkSpec,
        TChunkReaderConfigPtr config,
        TChunkReaderOptionsPtr options,
        IChunkReaderPtr underlyingReader,
        TNameTablePtr nameTable,
        IBlockCachePtr blockCache,
        const TKeyColumns& keyColumns,
        const TColumnFilter& columnFilter,
        const TSharedRange<TKey>& keys,
        TChunkReaderPerformanceCountersPtr performanceCounters)
        : TSchemalessChunkReaderBase(
            chunkSpec,
            config,
            options,
            underlyingReader->GetChunkId(),
            nameTable,
            columnFilter,
            keyColumns)
        , TColumnarLookupChunkReaderBase(
            config,
            underlyingReader,
            blockCache)
        , PerformanceCounters_(std::move(performanceCounters))
    {
        Keys_ = keys;

        ReadyEvent_ = BIND(&TColumnarSchemalessLookupChunkReader::InitializeBlockSequence, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
            .Run();
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        rows->clear();
        Pool_.Clear();

        if (!ReadyEvent_.IsSet() || !ReadyEvent_.Get().IsOK()) {
            return true;
        }

        if (NextKeyIndex_ == Keys_.Size()) {
            return false;
        }

        i64 DataWeight = 0;
        while (rows->size() < rows->capacity()) {
            ResetExhaustedColumns();

            if (RowIndexes_[NextKeyIndex_] < ChunkMeta_->Misc().row_count()) {
                const auto& key = Keys_[NextKeyIndex_];

                YCHECK(key.GetCount() == KeyColumnReaders_.size());

                // Reading row.
                i64 lowerRowIndex = KeyColumnReaders_[0]->GetCurrentRowIndex();
                i64 upperRowIndex = KeyColumnReaders_[0]->GetBlockUpperRowIndex();
                for (int i = 0; i < KeyColumnReaders_.size(); ++i) {
                    std::tie(lowerRowIndex, upperRowIndex) = KeyColumnReaders_[i]->GetEqualRange(
                        key[i],
                        lowerRowIndex,
                        upperRowIndex);
                }

                if (upperRowIndex == lowerRowIndex) {
                    // Key does not exist.
                    rows->push_back(TMutableUnversionedRow());
                } else {
                    // Key can be present in exactly one row.
                    YCHECK(upperRowIndex == lowerRowIndex + 1);
                    i64 rowIndex = lowerRowIndex;

                    rows->push_back(ReadRow(rowIndex));
                }
            } else {
                // Key oversteps chunk boundaries.
                rows->push_back(TMutableUnversionedRow());
            }

            DataWeight += GetDataWeight(rows->back());

            if (++NextKeyIndex_ == Keys_.Size() || !TryFetchNextRow() || DataWeight > TSchemalessChunkReaderBase::Config_->MaxDataSizePerRead) {
                break;
            }
        }

        if (PerformanceCounters_) {
            PerformanceCounters_->StaticChunkRowLookupCount += rows->size();
        }

        RowCount_ += rows->size();

        return true;
    }

    virtual TDataStatistics GetDataStatistics() const override
    {
        auto dataStatistics = TColumnarChunkReaderBase::GetDataStatistics();
        dataStatistics.set_row_count(RowCount_);
        return dataStatistics;
    }

private:
    const TChunkReaderPerformanceCountersPtr PerformanceCounters_;

    std::vector<IUnversionedColumnReader*> RowColumnReaders_;
    std::vector<IUnversionedColumnReader*> KeyColumnReaders_;

    ISchemalessColumnReader* SchemalessReader_ = nullptr;

    TChunkedMemoryPool Pool_;

    TMutableUnversionedRow ReadRow(i64 rowIndex)
    {
        ui32 schemalessColumnCount = 0;

        if (SchemalessReader_) {
            SchemalessReader_->SkipToRowIndex(rowIndex);
            SchemalessReader_->GetValueCounts(TMutableRange<ui32>(&schemalessColumnCount, 1));
        }

        auto row = TMutableUnversionedRow::Allocate(&Pool_, RowColumnReaders_.size() + schemalessColumnCount + SystemColumnCount_);
        row.SetCount(RowColumnReaders_.size());

        // Read values.
        auto range = TMutableRange<TMutableUnversionedRow>(&row, 1);

        for (auto& columnReader : RowColumnReaders_) {
            columnReader->SkipToRowIndex(rowIndex);
            columnReader->ReadValues(range);
        }

        if (SchemalessReader_) {
            SchemalessReader_->ReadValues(range);
        }

        return row;
    }

    void InitializeBlockSequence()
    {
        YCHECK(ChunkSpec_.chunk_meta().version() == static_cast<int>(ETableChunkFormat::UnversionedColumnar));
        InitializeSystemColumnIds();

        TNameTablePtr chunkNameTable;

        if (Options_->DynamicTable) {
            auto chunkMeta = ChunkSpec_.chunk_meta();
            ChunkMeta_ = New<TColumnarChunkMeta>(std::move(chunkMeta));
            chunkNameTable = TNameTable::FromSchema(ChunkMeta_->ChunkSchema());
        } else {
            // Download chunk meta.
            std::vector<int> extensionTags = {
                TProtoExtensionTag<TMiscExt>::Value,
                TProtoExtensionTag<TTableSchemaExt>::Value,
                TProtoExtensionTag<TBlockMetaExt>::Value,
                TProtoExtensionTag<TColumnMetaExt>::Value,
                TProtoExtensionTag<TNameTableExt>::Value
            };

            auto asynChunkMeta = UnderlyingReader_->GetMeta(
                TColumnarLookupChunkReaderBase::Config_->WorkloadDescriptor,
                Null,
                extensionTags);
            auto chunkMeta = WaitFor(asynChunkMeta)
                .ValueOrThrow();

            chunkNameTable = FromProto<TNameTablePtr>(GetProtoExtension<TNameTableExt>(
                chunkMeta.extensions()));

            ChunkMeta_ = New<TColumnarChunkMeta>(std::move(chunkMeta));
        }

        if (!ChunkMeta_->Misc().sorted()) {
            THROW_ERROR_EXCEPTION("Requested a sorted read for an unsorted chunk");
        }

        ValidateKeyColumns(
            KeyColumns_,
            ChunkMeta_->ChunkSchema().GetKeyColumns(),
            Options_->DynamicTable);

        ChunkMeta_->InitBlockLastKeys(KeyColumns_);

        // Create key column readers.
        KeyColumnReaders_.resize(KeyColumns_.size());
        for (int keyColumnIndex = 0; keyColumnIndex < ChunkMeta_->ChunkSchema().GetKeyColumnCount(); ++keyColumnIndex) {
            auto columnReader = CreateUnversionedColumnReader(
                ChunkMeta_->ChunkSchema().Columns()[keyColumnIndex],
                ChunkMeta_->ColumnMeta().columns(keyColumnIndex),
                keyColumnIndex,
                keyColumnIndex);

            KeyColumnReaders_[keyColumnIndex] = columnReader.get();
            Columns_.emplace_back(std::move(columnReader), keyColumnIndex);
        }
        for (int keyColumnIndex = ChunkMeta_->ChunkSchema().GetKeyColumnCount(); keyColumnIndex < KeyColumns_.size(); ++keyColumnIndex) {
            auto columnReader = CreateUnversionedNullColumnReader(
                keyColumnIndex,
                keyColumnIndex);

            KeyColumnReaders_[keyColumnIndex] = columnReader.get();
            Columns_.emplace_back(std::move(columnReader), -1);
        }

        // Define columns to read.
        std::vector<TColumnIdMapping> schemalessIdMapping;
        schemalessIdMapping.resize(chunkNameTable->GetSize(), {-1, -1});

        std::vector<int> schemaColumnIndexes;
        bool readSchemalessColumns = false;
        if (ColumnFilter_.All) {
            for (int index = 0; index < ChunkMeta_->ChunkSchema().Columns().size(); ++index) {
                schemaColumnIndexes.push_back(index);
            }

            for (
                int chunkColumnId = ChunkMeta_->ChunkSchema().Columns().size();
                chunkColumnId < chunkNameTable->GetSize();
                ++chunkColumnId) {
                readSchemalessColumns = true;
                schemalessIdMapping[chunkColumnId].ChunkSchemaIndex = chunkColumnId;
                schemalessIdMapping[chunkColumnId].ReaderSchemaIndex = NameTable_->GetIdOrRegisterName(
                    chunkNameTable->GetName(chunkColumnId));
            }
        } else {
            auto filterIndexes = yhash_set<int>(ColumnFilter_.Indexes.begin(), ColumnFilter_.Indexes.end());
            for (int chunkColumnId = 0; chunkColumnId < chunkNameTable->GetSize(); ++chunkColumnId) {
                auto nameTableIndex = NameTable_->GetIdOrRegisterName(chunkNameTable->GetName(chunkColumnId));
                if (filterIndexes.has(nameTableIndex)) {
                    if (chunkColumnId < ChunkMeta_->ChunkSchema().Columns().size()) {
                        schemaColumnIndexes.push_back(chunkColumnId);
                    } else {
                        readSchemalessColumns = true;
                        schemalessIdMapping[chunkColumnId].ChunkSchemaIndex = chunkColumnId;
                        schemalessIdMapping[chunkColumnId].ReaderSchemaIndex = nameTableIndex;

                    }
                }
            }
        }

        // Create column readers.
        for (int valueIndex = 0; valueIndex < schemaColumnIndexes.size(); ++valueIndex) {
            auto columnIndex = schemaColumnIndexes[valueIndex];
            if (columnIndex < ChunkMeta_->ChunkSchema().GetKeyColumnCount()) {
                RowColumnReaders_.push_back(KeyColumnReaders_[columnIndex]);
            } else {
                auto columnReader = CreateUnversionedColumnReader(
                    ChunkMeta_->ChunkSchema().Columns()[columnIndex],
                    ChunkMeta_->ColumnMeta().columns(columnIndex),
                    valueIndex,
                    NameTable_->GetIdOrRegisterName(ChunkMeta_->ChunkSchema().Columns()[columnIndex].Name));

                RowColumnReaders_.emplace_back(columnReader.get());
                Columns_.emplace_back(std::move(columnReader), columnIndex);
            }
        }

        if (readSchemalessColumns) {
            auto columnReader = CreateSchemalessColumnReader(
                ChunkMeta_->ColumnMeta().columns(ChunkMeta_->ChunkSchema().Columns().size()),
                schemalessIdMapping);
            SchemalessReader_ = columnReader.get();

            Columns_.emplace_back(
                std::move(columnReader),
                ChunkMeta_->ChunkSchema().Columns().size());
        }

        Initialize();

        // NB: We must complete initialization before ReadyEvent is set in the constructor.
        WaitFor(RequestFirstBlocks())
            .ThrowOnError();
    }
};

////////////////////////////////////////////////////////////////////////////////

ISchemalessChunkReaderPtr CreateSchemalessChunkReader(
    const NChunkClient::NProto::TChunkSpec& chunkSpec,
    TChunkReaderConfigPtr config,
    TChunkReaderOptionsPtr options,
    NChunkClient::IChunkReaderPtr underlyingReader,
    TNameTablePtr nameTable,
    NChunkClient::IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns,
    const TColumnFilter& columnFilter,
    const TReadRange& readRange,
    TNullable<int> partitionTag)
{
    ETableChunkFormat formatVersion = ETableChunkFormat::SchemalessHorizontal;
    if (chunkSpec.has_chunk_meta()) {
        auto type = EChunkType(chunkSpec.chunk_meta().type());
        YCHECK(type == EChunkType::Table);
        formatVersion = ETableChunkFormat(chunkSpec.chunk_meta().version());
    }

    switch (formatVersion) {
        case ETableChunkFormat::SchemalessHorizontal:
            return New<THorizontalSchemalessRangeChunkReader>(
                chunkSpec,
                config,
                options,
                underlyingReader,
                nameTable,
                blockCache,
                keyColumns,
                columnFilter,
                readRange,
                std::move(partitionTag));

        case ETableChunkFormat::UnversionedColumnar:
            return New<TColumnarSchemalessRangeChunkReader>(
                chunkSpec,
                config,
                options,
                underlyingReader,
                nameTable,
                blockCache,
                keyColumns,
                columnFilter,
                readRange);

        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessChunkReaderPtr CreateSchemalessChunkReader(
    const NChunkClient::NProto::TChunkSpec& chunkSpec,
    TChunkReaderConfigPtr config,
    TChunkReaderOptionsPtr options,
    NChunkClient::IChunkReaderPtr underlyingReader,
    TNameTablePtr nameTable,
    NChunkClient::IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns,
    const TColumnFilter& columnFilter,
    const TSharedRange<TKey>& keys,
    TChunkReaderPerformanceCountersPtr performanceCounters,
    TNullable<int> partitionTag)
{
    ETableChunkFormat formatVersion = ETableChunkFormat::SchemalessHorizontal;
    if (chunkSpec.has_chunk_meta()) {
        auto type = EChunkType(chunkSpec.chunk_meta().type());
        YCHECK(type == EChunkType::Table);
        formatVersion = ETableChunkFormat(chunkSpec.chunk_meta().version());
    }

    switch (formatVersion) {
        case ETableChunkFormat::SchemalessHorizontal:
            return New<THorizontalSchemalessLookupChunkReader>(
                chunkSpec,
                std::move(config),
                std::move(options),
                std::move(underlyingReader),
                std::move(nameTable),
                std::move(blockCache),
                keyColumns,
                columnFilter,
                keys,
                std::move(performanceCounters),
                std::move(partitionTag));

        case ETableChunkFormat::UnversionedColumnar:
            return New<TColumnarSchemalessLookupChunkReader>(
                chunkSpec,
                std::move(config),
                std::move(options),
                std::move(underlyingReader),
                std::move(nameTable),
                std::move(blockCache),
                keyColumns,
                columnFilter,
                keys,
                std::move(performanceCounters));

        default:
            THROW_ERROR_EXCEPTION(
                "This operation is not supported for chunks in %Qv format",
                formatVersion);
    }
}

////////////////////////////////////////////////////////////////////////////////

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
    INativeClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
    TNameTablePtr nameTable,
    const TColumnFilter& columnFilter,
    const TKeyColumns& keyColumns,
    TNullable<int> partitionTag,
    IThroughputThrottlerPtr throttler)
{
    std::vector<IReaderFactoryPtr> factories;
    for (const auto& dataSliceDescriptor : dataSliceDescriptors) {
        const auto& dataSource = dataSourceDirectory->DataSources()[dataSliceDescriptor.GetDataSourceIndex()];

        switch (dataSource.GetType()) {
            case EDataSourceType::UnversionedTable: {
                const auto& chunkSpec = dataSliceDescriptor.GetSingleChunk();

                auto memoryEstimate = GetChunkReaderMemoryEstimate(chunkSpec, config);
                auto createReader = [=] () {
                    auto remoteReader = CreateRemoteReader(
                        chunkSpec,
                        config,
                        options,
                        client,
                        nodeDirectory,
                        localDescriptor,
                        blockCache,
                        throttler);

                    using NYT::FromProto;
                    auto channel = chunkSpec.has_channel()
                        ? FromProto<TChannel>(chunkSpec.channel())
                        : TChannel::Universal();

                    TReadRange range = {
                        chunkSpec.has_lower_limit() ? TReadLimit(chunkSpec.lower_limit()) : TReadLimit(),
                        chunkSpec.has_upper_limit() ? TReadLimit(chunkSpec.upper_limit()) : TReadLimit()
                    };

                    return CreateSchemalessChunkReader(
                        chunkSpec,
                        PatchConfig(config, memoryEstimate),
                        options,
                        remoteReader,
                        nameTable,
                        blockCache,
                        keyColumns,
                        columnFilter.All ? CreateColumnFilter(channel, nameTable) : columnFilter,
                        range,
                        partitionTag);
                };

                factories.emplace_back(CreateReaderFactory(createReader, memoryEstimate, dataSliceDescriptor));
                break;
            }

            case EDataSourceType::VersionedTable: {
                auto memoryEstimate = GetDataSliceDescriptorReaderMemoryEstimate(dataSliceDescriptor, config);
                auto createReader = [=] () {

                    YCHECK(!dataSliceDescriptor.ChunkSpecs.empty());
                    using NYT::FromProto;
                    auto channel = dataSliceDescriptor.ChunkSpecs[0].has_channel()
                        ? FromProto<TChannel>(dataSliceDescriptor.ChunkSpecs[0].channel())
                        : TChannel::Universal();

                    return CreateSchemalessMergingMultiChunkReader(
                        config,
                        options,
                        client,
                        localDescriptor,
                        blockCache,
                        nodeDirectory,
                        dataSourceDirectory,
                        dataSliceDescriptor,
                        nameTable,
                        columnFilter.All ? CreateColumnFilter(channel, nameTable) : columnFilter,
                        throttler);
                };

                factories.emplace_back(CreateReaderFactory(createReader, memoryEstimate, dataSliceDescriptor));
                break;
            }

            default:
                Y_UNREACHABLE();
        }
    }

    return factories;
}

////////////////////////////////////////////////////////////////////////////////

template <class TBase>
class TSchemalessMultiChunkReader
    : public ISchemalessMultiChunkReader
    , public TBase
{
public:
    TSchemalessMultiChunkReader(
        TTableReaderConfigPtr config,
        TTableReaderOptionsPtr options,
        INativeClientPtr client,
        const TNodeDescriptor& localDescriptor,
        IBlockCachePtr blockCache,
        TNodeDirectoryPtr nodeDirectory,
        const TDataSourceDirectoryPtr& dataSourceDirectory,
        const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
        TNameTablePtr nameTable,
        const TColumnFilter& columnFilter,
        const TKeyColumns& keyColumns,
        TNullable<int> partitionTag,
        IThroughputThrottlerPtr throttler);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;

    virtual i64 GetSessionRowIndex() const override;

    virtual i64 GetTotalRowCount() const override;

    virtual const TNameTablePtr& GetNameTable() const override;

    virtual TKeyColumns GetKeyColumns() const override;

    virtual i64 GetTableRowIndex() const override;

    virtual void Interrupt() override;

    virtual std::vector<TDataSliceDescriptor> GetUnreadDataSliceDescriptors(
        const TRange<TUnversionedRow>& unreadRows) const override;

private:
    const TNameTablePtr NameTable_;
    const TKeyColumns KeyColumns_;

    ISchemalessChunkReaderPtr CurrentReader_;
    std::atomic<i64> RowIndex_ = {0};
    std::atomic<i64> RowCount_ = {-1};

    std::atomic<bool> Finished_ = {false};

    using TBase::ReadyEvent_;
    using TBase::CurrentSession_;

    virtual void OnReaderSwitched() override;
};

////////////////////////////////////////////////////////////////////////////////

template <class TBase>
TSchemalessMultiChunkReader<TBase>::TSchemalessMultiChunkReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    INativeClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
    TNameTablePtr nameTable,
    const TColumnFilter& columnFilter,
    const TKeyColumns& keyColumns,
    TNullable<int> partitionTag,
    IThroughputThrottlerPtr throttler)
    : TBase(
        config,
        options,
        CreateReaderFactories(
            config,
            options,
            client,
            localDescriptor,
            blockCache,
            nodeDirectory,
            dataSourceDirectory,
            dataSliceDescriptors,
            nameTable,
            columnFilter,
            keyColumns,
            partitionTag,
            throttler))
    , NameTable_(nameTable)
    , KeyColumns_(keyColumns)
    , RowCount_(GetCumulativeRowCount(dataSliceDescriptors))
{
    if (dataSliceDescriptors.empty()) {
        Finished_ = true;
    }
}

template <class TBase>
bool TSchemalessMultiChunkReader<TBase>::Read(std::vector<TUnversionedRow>* rows)
{
    rows->clear();

    if (Finished_) {
        RowCount_ = RowIndex_.load();
        return false;
    }

    if (!ReadyEvent_.IsSet() || !ReadyEvent_.Get().IsOK()) {
        return true;
    }

    bool readerFinished = !CurrentReader_->Read(rows);
    if (!rows->empty()) {
        RowIndex_ += rows->size();
        return true;
    }

    if (!TBase::OnEmptyRead(readerFinished)) {
        Finished_ = true;
    }

    return true;
}

template <class TBase>
void TSchemalessMultiChunkReader<TBase>::OnReaderSwitched()
{
    CurrentReader_ = dynamic_cast<ISchemalessChunkReader*>(CurrentSession_.Reader.Get());
    YCHECK(CurrentReader_);
}

template <class TBase>
i64 TSchemalessMultiChunkReader<TBase>::GetTotalRowCount() const
{
    return RowCount_;
}

template <class TBase>
i64 TSchemalessMultiChunkReader<TBase>::GetSessionRowIndex() const
{
    return RowIndex_;
}

template <class TBase>
i64 TSchemalessMultiChunkReader<TBase>::GetTableRowIndex() const
{
    return CurrentReader_ ? CurrentReader_->GetTableRowIndex() : 0;
}

template <class TBase>
const TNameTablePtr& TSchemalessMultiChunkReader<TBase>::GetNameTable() const
{
    return NameTable_;
}

template <class TBase>
TKeyColumns TSchemalessMultiChunkReader<TBase>::GetKeyColumns() const
{
    return KeyColumns_;
}

template <class TBase>
void TSchemalessMultiChunkReader<TBase>::Interrupt()
{
    if (!Finished_) {
        Finished_ = true;
        TBase::OnInterrupt();
    }
}

template <class TBase>
std::vector<TDataSliceDescriptor> TSchemalessMultiChunkReader<TBase>::GetUnreadDataSliceDescriptors(
    const TRange<TUnversionedRow>& unreadRows) const
{
    static TRange<TUnversionedRow> emptyRange;
    auto state = TBase::GetUnreadState();

    std::vector<TDataSliceDescriptor> result;
    if (state.CurrentReader) {
        auto chunkReader = dynamic_cast<ISchemalessChunkReader*>(state.CurrentReader.Get());
        YCHECK(chunkReader);
        result = chunkReader->GetUnreadDataSliceDescriptors(unreadRows);
    }
    for (const auto& activeReader : state.ActiveReaders) {
        auto chunkReader = dynamic_cast<ISchemalessChunkReader*>(activeReader.Get());
        YCHECK(chunkReader);
        auto unreadChunks = chunkReader->GetUnreadDataSliceDescriptors(emptyRange);
        std::move(unreadChunks.begin(), unreadChunks.end(), std::back_inserter(result));
    }
    for (const auto& factory : state.ReaderFactories) {
        result.emplace_back(factory->GetDataSliceDescriptor());
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessSequentialMultiReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    INativeClientPtr client,
    const TNodeDescriptor &localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
    TNameTablePtr nameTable,
    const TColumnFilter& columnFilter,
    const TKeyColumns &keyColumns,
    TNullable<int> partitionTag,
    IThroughputThrottlerPtr throttler)
{
    auto reader = New<TSchemalessMultiChunkReader<TSequentialMultiReaderBase>>(
        config,
        options,
        client,
        localDescriptor,
        blockCache,
        nodeDirectory,
        dataSourceDirectory,
        dataSliceDescriptors,
        nameTable,
        columnFilter,
        keyColumns,
        partitionTag,
        throttler);

    reader->Open();
    return reader;
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessParallelMultiReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    INativeClientPtr client,
    const TNodeDescriptor &localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const std::vector<TDataSliceDescriptor>& dataSliceDescriptors,
    TNameTablePtr nameTable,
    const TColumnFilter& columnFilter,
    const TKeyColumns &keyColumns,
    TNullable<int> partitionTag,
    IThroughputThrottlerPtr throttler)
{
    auto reader = New<TSchemalessMultiChunkReader<TParallelMultiReaderBase>>(
        config,
        options,
        client,
        localDescriptor,
        blockCache,
        nodeDirectory,
        dataSourceDirectory,
        dataSliceDescriptors,
        nameTable,
        columnFilter,
        keyColumns,
        partitionTag,
        throttler);

    reader->Open();
    return reader;
}

////////////////////////////////////////////////////////////////////////////////

class TSchemalessMergingMultiChunkReader
    : public ISchemalessMultiChunkReader
{
public:
    static ISchemalessMultiChunkReaderPtr Create(
        TTableReaderConfigPtr config,
        TTableReaderOptionsPtr options,
        INativeClientPtr client,
        const TNodeDescriptor& localDescriptor,
        IBlockCachePtr blockCache,
        TNodeDirectoryPtr nodeDirectory,
        const TDataSourceDirectoryPtr& dataSourceDirectory,
        const TDataSliceDescriptor& dataSliceDescriptor,
        TNameTablePtr nameTable,
        TColumnFilter columnFilter,
        IThroughputThrottlerPtr throttler);

    virtual TFuture<void> GetReadyEvent() override
    {
        auto promise = NewPromise<void>();
        promise.TrySetFrom(ErrorPromise_.ToFuture());
        promise.TrySetFrom(UnderlyingReader_->GetReadyEvent());
        return promise.ToFuture();
    }

    virtual TDataStatistics GetDataStatistics() const override
    {
        return UnderlyingReader_->GetDataStatistics();
    }

    virtual std::vector<TChunkId> GetFailedChunkIds() const override
    {
        // ToDo(psushin): every reader must implement this method eventually.
        return std::vector<TChunkId>();
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        rows->clear();
        SchemafulRows_.clear();
        MemoryPool_.Clear();

        if (Interrupting_) {
            return false;
        }

        if (ErrorPromise_.IsSet()) {
            return true;
        }

        SchemafulRows_.reserve(rows->capacity());
        HasMore_ = UnderlyingReader_->Read(&SchemafulRows_);
        if (SchemafulRows_.empty()) {
            return HasMore_;
        }

        LastKey_ = GetKeyPrefix(SchemafulRows_.back(), Schema_.GetKeyColumnCount());

        YCHECK(HasMore_);

        try {
            for (int index = 0; index < SchemafulRows_.size(); ++index) {
                auto schemalessRow = TMutableUnversionedRow::Allocate(&MemoryPool_, SchemaColumnCount_ + SystemColumnCount_);
                auto schemafulRow = SchemafulRows_[index];

                int schemalessValueIndex = 0;
                for (int valueIndex = 0; valueIndex < schemafulRow.GetCount(); ++valueIndex) {
                    const auto& value = schemafulRow[valueIndex];
                    auto id = IdMapping_[value.Id];

                    if (id >= 0) {
                        ValidateDataValue(value);
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

                rows->push_back(schemalessRow);
            }

            RowIndex_ += rows->size();
        } catch (const std::exception& ex) {
            SchemafulRows_.clear();
            rows->clear();

            ErrorPromise_.Set(ex);
        }

        return true;
    }

    virtual std::vector<TDataSliceDescriptor> GetUnreadDataSliceDescriptors(
        const TRange<TUnversionedRow>& unreadRows) const override
    {
        std::vector<TDataSliceDescriptor> unreadDescriptors;
        TOwningKey firstUnreadKey;

        if (unreadRows.Empty()) {
            if (!HasMore_) {
                // Return the empty vector.
                return unreadDescriptors;
            }
            unreadDescriptors.emplace_back(DataSliceDescriptor_);
            if (!LastKey_) {
                // Return the whole data slice descriptor.
                return unreadDescriptors;
            }
            firstUnreadKey = GetKeySuccessor(LastKey_);
        } else {
            unreadDescriptors.emplace_back(DataSliceDescriptor_);
            auto firstSchemafulUnreadRow = SchemafulRows_[SchemafulRows_.size() - unreadRows.Size()];
            firstUnreadKey = GetKeyPrefix(firstSchemafulUnreadRow, Schema_.GetKeyColumnCount());
        }

        for (auto& descriptor : unreadDescriptors) {
            for (auto& chunk : descriptor.ChunkSpecs) {
                ToProto(chunk.mutable_lower_limit()->mutable_key(), firstUnreadKey);
                // TODO: Estimate row count and data size.
            }
        }
        return unreadDescriptors;
    }

    virtual void Interrupt() override
    {
        Interrupting_ = true;
        ErrorPromise_.TrySet(TError());
    }

    virtual bool IsFetchingCompleted() const override
    {
        return false;
    }

    virtual i64 GetSessionRowIndex() const override
    {
        return RowIndex_;
    }

    virtual i64 GetTotalRowCount() const override
    {
        return RowCount_;
    }

    virtual const TNameTablePtr& GetNameTable() const override
    {
        return NameTable_;
    }

    virtual TKeyColumns GetKeyColumns() const override
    {
        return KeyColumns_;
    }

    virtual i64 GetTableRowIndex() const override
    {
        // Versioned data don't have table row index;
        return 0;
    }

private:
    const TTableReaderOptionsPtr Options_;
    const ISchemafulReaderPtr UnderlyingReader_;
    const TDataSliceDescriptor DataSliceDescriptor_;
    const TTableSchema Schema_;
    const std::vector<int> IdMapping_;
    const TNameTablePtr NameTable_;
    const i64 RowCount_;

    // We keep rows received from underlying schemaful reader
    // to define proper lower limit during interrupt.
    std::vector<TUnversionedRow> SchemafulRows_;

    std::atomic<bool> Interrupting_ = {false};
    std::atomic<bool> HasMore_ = {false};
    TOwningKey LastKey_;

    i64 RowIndex_ = 0;

    TChunkedMemoryPool MemoryPool_;

    int TableIndexId_ = -1;
    int RangeIndexId_ = -1;
    int TableIndex_ = -1;
    int RangeIndex_ = -1;
    int SystemColumnCount_ = 0;

    // Number of "active" columns in id mapping.
    int SchemaColumnCount_ = 0;

    // Columns that output row stream is sorted by. May not coincide with schema key columns,
    // because some column may be filtered out by the column filter.
    TKeyColumns KeyColumns_;

    TPromise<void> ErrorPromise_ = NewPromise<void>();

    TSchemalessMergingMultiChunkReader(
        TTableReaderOptionsPtr options,
        ISchemafulReaderPtr underlyingReader,
        const TDataSliceDescriptor& dataSliceDescriptor,
        TTableSchema schema,
        std::vector<int> idMapping,
        TNameTablePtr nameTable,
        i64 rowCount)
        : Options_(options)
        , UnderlyingReader_(std::move(underlyingReader))
        , DataSliceDescriptor_(dataSliceDescriptor)
        , Schema_(std::move(schema))
        , IdMapping_(idMapping)
        , NameTable_(nameTable)
        , RowCount_(rowCount)
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

        for(auto id : IdMapping_) {
            if (id >= 0) {
                ++SchemaColumnCount_;
            }
        }

        for (int index = 0; index < Schema_.GetKeyColumnCount(); ++index) {
            if (IdMapping_[index] < 0) {
                break;
            }

            KeyColumns_.push_back(Schema_.Columns()[index].Name);
        }
    }

    DECLARE_NEW_FRIEND();
};

////////////////////////////////////////////////////////////////////////////////

std::pair<TTableSchema, TColumnFilter> CreateVersionedReadParameters(
    const TTableSchema& schema,
    const TColumnFilter& columnFilter)
{
    if (columnFilter.All) {
        return std::make_pair(schema, columnFilter);
    }

    std::vector<NTableClient::TColumnSchema> columns;
    for (int index = 0; index < schema.GetKeyColumnCount(); ++index) {
        columns.push_back(schema.Columns()[index]);
    }

    TColumnFilter newColumnFilter;
    newColumnFilter.All = false;

    for (int index : columnFilter.Indexes) {
        if (index >= schema.GetKeyColumnCount()) {
            newColumnFilter.Indexes.push_back(columns.size());
            columns.push_back(schema.Columns()[index]);
        } else {
            newColumnFilter.Indexes.push_back(index);
        }
    }

    return std::make_pair(TTableSchema(std::move(columns)), std::move(newColumnFilter));
}

ISchemalessMultiChunkReaderPtr TSchemalessMergingMultiChunkReader::Create(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    INativeClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const TDataSliceDescriptor& dataSliceDescriptor,
    TNameTablePtr nameTable,
    TColumnFilter columnFilter,
    IThroughputThrottlerPtr throttler)
{
    const auto& dataSource = dataSourceDirectory->DataSources()[dataSliceDescriptor.GetDataSourceIndex()];
    const auto& chunkSpecs = dataSliceDescriptor.ChunkSpecs;

    YCHECK(dataSource.Schema());
    const auto& tableSchema = *dataSource.Schema();
    auto timestamp = dataSource.GetTimestamp();

    // Convert name table column filter to schema column filter.
    for (auto& index : columnFilter.Indexes) {
        index = tableSchema.GetColumnIndex(nameTable->GetName(index));
    }

    TTableSchema versionedReadSchema;
    TColumnFilter versionedColumnFilter;
    std::tie(versionedReadSchema, versionedColumnFilter) = CreateVersionedReadParameters(
        tableSchema,
        columnFilter);

    std::vector<int> idMapping(versionedReadSchema.GetColumnCount());

    try {
        for (int columnIndex = 0; columnIndex < versionedReadSchema.Columns().size(); ++columnIndex) {
            const auto& column = versionedReadSchema.Columns()[columnIndex];
            if (versionedColumnFilter.Contains(columnIndex)) {
                idMapping[columnIndex] = nameTable->GetIdOrRegisterName(column.Name);
            } else {
                // We should skip this column in schemaless reading.
                idMapping[columnIndex] = -1;
            }
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Failed to update name table for schemaless merging multi chunk reader")
            << ex;
    }

    std::vector<TOwningKey> boundaries;
    boundaries.reserve(chunkSpecs.size());

    for (const auto& chunkSpec : chunkSpecs) {
        YCHECK(chunkSpec.has_chunk_meta());
        TOwningKey minKey;
        if (chunkSpec.has_lower_limit()) {
            auto limit = NYT::FromProto<TReadLimit>(chunkSpec.lower_limit());
            minKey = limit.GetKey();
        } else if (FindProtoExtension<NProto::TBoundaryKeysExt>(chunkSpec.chunk_meta().extensions())) {
            auto boundaryKeysExt = GetProtoExtension<NProto::TBoundaryKeysExt>(chunkSpec.chunk_meta().extensions());
            minKey = NYT::FromProto<TOwningKey>(boundaryKeysExt.min());
        }
        boundaries.push_back(minKey);
    }

    LOG_DEBUG("Create overlapping range reader (Boundaries: %v, Chunks: %v, ColumnFilter: %v)",
        boundaries,
        MakeFormattableRange(chunkSpecs, [] (TStringBuilder* builder, const TChunkSpec& chunkSpec) {
            FormatValue(builder, FromProto<TChunkId>(chunkSpec.chunk_id()), TStringBuf());
        }),
        columnFilter);

    auto performanceCounters = New<TChunkReaderPerformanceCounters>();

    auto createVersionedReader = [
        config,
        options,
        client,
        localDescriptor,
        blockCache,
        nodeDirectory,
        chunkSpecs,
        versionedReadSchema,
        performanceCounters,
        timestamp
    ] (int index) -> IVersionedReaderPtr {
        const auto& chunkSpec = chunkSpecs[index];
        auto chunkId = NYT::FromProto<TChunkId>(chunkSpec.chunk_id());
        auto replicas = NYT::FromProto<TChunkReplicaList>(chunkSpec.replicas());

        TReadLimit lowerLimit;
        TReadLimit upperLimit;

        if (chunkSpec.has_lower_limit()) {
            lowerLimit = NYT::FromProto<TReadLimit>(chunkSpec.lower_limit());
        } else {
            lowerLimit.SetKey(MinKey());
        }

        if (chunkSpec.has_upper_limit()) {
            upperLimit = NYT::FromProto<TReadLimit>(chunkSpec.upper_limit());
        } else {
            upperLimit.SetKey(MaxKey());
        }

        if (lowerLimit.HasRowIndex() || upperLimit.HasRowIndex()) {
            THROW_ERROR_EXCEPTION("Row index limit is not supported");
        }

        LOG_DEBUG("Create versioned chunk reader (ChunkId: %v, Range: <%v : %v>)",
            chunkId,
            lowerLimit.GetKey(),
            upperLimit.GetKey());

        auto chunkReader = CreateReplicationReader(
            config,
            options,
            client,
            nodeDirectory,
            localDescriptor,
            chunkId,
            replicas,
            blockCache);

        auto asyncChunkMeta = TCachedVersionedChunkMeta::Load(
            chunkReader,
            config->WorkloadDescriptor,
            versionedReadSchema);
        auto chunkMeta = WaitFor(asyncChunkMeta)
            .ValueOrThrow();

        return CreateVersionedChunkReader(
            config,
            std::move(chunkReader),
            blockCache,
            std::move(chunkMeta),
            lowerLimit.GetKey(),
            upperLimit.GetKey(),
            TColumnFilter(),
            performanceCounters,
            timestamp,
            false);
    };

    struct TSchemalessMergingMultiChunkReaderBufferTag
    { };

    auto rowMerger = std::make_unique<TSchemafulRowMerger>(
        New<TRowBuffer>(TSchemalessMergingMultiChunkReaderBufferTag()),
        versionedReadSchema.Columns().size(),
        versionedReadSchema.GetKeyColumnCount(),
        TColumnFilter(),
        client->GetNativeConnection()->GetColumnEvaluatorCache()->Find(versionedReadSchema));

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
        rowCount);
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessMergingMultiChunkReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    INativeClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const TDataSourceDirectoryPtr& dataSourceDirectory,
    const TDataSliceDescriptor& dataSliceDescriptor,
    TNameTablePtr nameTable,
    const TColumnFilter& columnFilter,
    IThroughputThrottlerPtr throttler)
{
    return TSchemalessMergingMultiChunkReader::Create(
        config,
        options,
        client,
        localDescriptor,
        blockCache,
        nodeDirectory,
        dataSourceDirectory,
        dataSliceDescriptor,
        nameTable,
        columnFilter,
        throttler);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
