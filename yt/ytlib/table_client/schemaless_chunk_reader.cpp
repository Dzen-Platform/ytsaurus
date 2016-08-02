#include "cached_versioned_chunk_meta.h"
#include "chunk_reader_base.h"
#include "columnar_chunk_reader_base.h"
#include "config.h"
#include "helpers.h"
#include "legacy_table_chunk_reader.h"
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

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/connection.h>

#include <yt/ytlib/table_chunk_format/public.h>
#include <yt/ytlib/table_chunk_format/column_reader.h>

#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/multi_reader_base.h>
#include <yt/ytlib/chunk_client/reader_factory.h>
#include <yt/ytlib/chunk_client/replication_reader.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/query_client/column_evaluator.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/protobuf_helpers.h>

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

using NChunkClient::TReadLimit;
using NChunkClient::TReadRange;
using NChunkClient::TChannel;
using NChunkClient::NProto::TMiscExt;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

namespace {

void ValidateReadRanges(const std::vector<TReadRange>& readRanges)
{
    YCHECK(!readRanges.empty());

    bool hasRowIndex = false;
    bool hasKey = false;

    for (const auto& range : readRanges) {
        if (range.LowerLimit().HasRowIndex() || range.UpperLimit().HasRowIndex()) {
            hasRowIndex = true;
        }
        if (range.LowerLimit().HasKey() || range.UpperLimit().HasKey()) {
            hasKey = true;
        }
        if (hasRowIndex && hasKey && readRanges.size() > 1) {
            THROW_ERROR_EXCEPTION("Both key ranges and index ranges are not supported when reading multiple ranges");
        }
    }

    if (hasRowIndex) {
        for (int index = 0; index < readRanges.size(); ++index) {
            if ((index > 0 && !readRanges[index].LowerLimit().HasRowIndex()) ||
                ((index < readRanges.size() - 1 && !readRanges[index].UpperLimit().HasRowIndex())))
            {
                THROW_ERROR_EXCEPTION("Overlapping ranges are not supported");
            }
        }

        for (int index = 1; index < readRanges.size(); ++index) {
            const auto& prev = readRanges[index - 1].UpperLimit();
            const auto& next = readRanges[index].LowerLimit();
            if (prev.GetRowIndex() > next.GetRowIndex()) {
                THROW_ERROR_EXCEPTION("Overlapping ranges are not supported");
            }
        }
    }

    if (hasKey) {
        for (int index = 0; index < readRanges.size(); ++index) {
            if ((index > 0 && !readRanges[index].LowerLimit().HasKey()) ||
                ((index < readRanges.size() - 1 && !readRanges[index].UpperLimit().HasKey())))
            {
                THROW_ERROR_EXCEPTION("Overlapping ranges are not supported");
            }
        }

        for (int index = 1; index < readRanges.size(); ++index) {
            const auto& prev = readRanges[index - 1].UpperLimit();
            const auto& next = readRanges[index].LowerLimit();
            if (CompareRows(prev.GetKey(), next.GetKey()) > 0) {
                THROW_ERROR_EXCEPTION("Overlapping ranges are not supported");
            }
        }
    }
}

} // namespace

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
    {
        if (Config_->SamplingRate) {
            RowSampler_ = CreateChunkRowSampler(
                chunkId,
                Config_->SamplingRate.Get(),
                Config_->SamplingSeed.Get(std::random_device()()));
        }
    }

    virtual TNameTablePtr GetNameTable() const override
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
};

////////////////////////////////////////////////////////////////////////////////

class THorizontalSchemalessChunkReader
    : public TChunkReaderBase
    , public TSchemalessChunkReaderBase
{
public:
    THorizontalSchemalessChunkReader(
        const TChunkSpec& chunkSpec,
        TChunkReaderConfigPtr config,
        TChunkReaderOptionsPtr options,
        IChunkReaderPtr underlyingReader,
        TNameTablePtr nameTable,
        IBlockCachePtr blockCache,
        const TKeyColumns& keyColumns,
        const TColumnFilter& columnFilter,
        std::vector<TReadRange> readRanges,
        TNullable<int> partitionTag);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;

    virtual TDataStatistics GetDataStatistics() const override;

private:
    using TSchemalessChunkReaderBase::Config_;

    TNameTablePtr ChunkNameTable_ = New<TNameTable>();

    std::vector<TReadRange> ReadRanges_;

    TNullable<int> PartitionTag_;

    // Maps chunk name table ids into client ids.
    // For filtered out columns ReaderSchemaIndex is -1.
    std::vector<TColumnIdMapping> IdMapping_;

    std::unique_ptr<THorizontalSchemalessBlockReader> BlockReader_;

    int CurrentBlockIndex_ = 0;
    int CurrentRangeIndex_ = 0;

    bool RangeEnded_ = false;

    TChunkMeta ChunkMeta_;
    TBlockMetaExt BlockMetaExt_;

    std::vector<int> BlockIndexes_;

    TFuture<void> InitializeBlockSequence();

    virtual void InitFirstBlock() override;
    virtual void InitNextBlock() override;

    void CreateBlockSequence(int beginIndex, int endIndex);
    void DownloadChunkMeta(std::vector<int> extensionTags, TNullable<int> partitionTag = Null);

    void SkipToCurrrentRange();
    bool InitNextRange();

    void InitializeBlockSequencePartition();
    void InitializeBlockSequenceSorted();
    void InitializeBlockSequenceUnsorted();
};

DEFINE_REFCOUNTED_TYPE(THorizontalSchemalessChunkReader)

////////////////////////////////////////////////////////////////////////////////

THorizontalSchemalessChunkReader::THorizontalSchemalessChunkReader(
    const TChunkSpec& chunkSpec,
    TChunkReaderConfigPtr config,
    TChunkReaderOptionsPtr options,
    IChunkReaderPtr underlyingReader,
    TNameTablePtr nameTable,
    IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns,
    const TColumnFilter& columnFilter,
    std::vector<TReadRange> readRanges,
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
    , ReadRanges_(readRanges)
    , PartitionTag_(std::move(partitionTag))
{
    if (ReadRanges_.size() == 1) {
        LOG_DEBUG("Reading single range (Range: %v)", ReadRanges_[0]);
    } else {
        LOG_DEBUG("Reading multiple ranges (RangeCount: %v)", ReadRanges_.size());
    }

    // Ready event must be set only when all initialization is finished and 
    // RowIndex_ is set into proper value.
    ReadyEvent_ = BIND(&THorizontalSchemalessChunkReader::InitializeBlockSequence, MakeStrong(this))
        .AsyncVia(NChunkClient::TDispatcher::Get()->GetReaderInvoker())
        .Run()
        .Apply(BIND([this, this_ = MakeStrong(this)] () {
            if (InitFirstBlockNeeded_) {
                InitFirstBlock();
                InitFirstBlockNeeded_ = false;
            }
        }));
}

TFuture<void> THorizontalSchemalessChunkReader::InitializeBlockSequence()
{
    YCHECK(ChunkSpec_.chunk_meta().version() == static_cast<int>(ETableChunkFormat::SchemalessHorizontal));
    YCHECK(BlockIndexes_.empty());

    InitializeSystemColumnIds();

    if (PartitionTag_) {
        InitializeBlockSequencePartition();
    } else {
        if (KeyColumns_.empty() &&
            ReadRanges_.size() == 1 &&
            !ReadRanges_[0].LowerLimit().HasKey() &&
            !ReadRanges_[0].UpperLimit().HasKey())
        {
            InitializeBlockSequenceUnsorted();
        } else {
            InitializeBlockSequenceSorted();
        }
    }

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

void THorizontalSchemalessChunkReader::DownloadChunkMeta(std::vector<int> extensionTags, TNullable<int> partitionTag)
{
    extensionTags.push_back(TProtoExtensionTag<TMiscExt>::Value);
    extensionTags.push_back(TProtoExtensionTag<TBlockMetaExt>::Value);
    extensionTags.push_back(TProtoExtensionTag<TNameTableExt>::Value);
    auto asynChunkMeta = UnderlyingReader_->GetMeta(
        Config_->WorkloadDescriptor,
        partitionTag,
        extensionTags);
    ChunkMeta_ = WaitFor(asynChunkMeta)
        .ValueOrThrow();

    BlockMetaExt_ = GetProtoExtension<TBlockMetaExt>(ChunkMeta_.extensions());

    auto nameTableExt = GetProtoExtension<TNameTableExt>(ChunkMeta_.extensions());
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

void THorizontalSchemalessChunkReader::InitializeBlockSequenceSorted()
{
    std::vector<int> extensionTags = {
        TProtoExtensionTag<TKeyColumnsExt>::Value,
    };

    DownloadChunkMeta(extensionTags);

    auto misc = GetProtoExtension<TMiscExt>(ChunkMeta_.extensions());
    if (!misc.sorted()) {
        THROW_ERROR_EXCEPTION("Requested a sorted read for an unsorted chunk");
    }

    auto keyColumnsExt = GetProtoExtension<TKeyColumnsExt>(ChunkMeta_.extensions());
    TKeyColumns chunkKeyColumns = NYT::FromProto<TKeyColumns>(keyColumnsExt);

    ValidateKeyColumns(KeyColumns_, chunkKeyColumns);

    if (KeyColumns_.empty()) {
        KeyColumns_ = chunkKeyColumns;
    }

    int lastIndex = -1;
    std::vector<TBlockFetcher::TBlockInfo> blocks;
    for (const auto& readRange : ReadRanges_) {
        int beginIndex = std::max(
            ApplyLowerRowLimit(BlockMetaExt_, readRange.LowerLimit()),
            ApplyLowerKeyLimit(BlockMetaExt_, readRange.LowerLimit()));
        int endIndex = std::min(
            ApplyUpperRowLimit(BlockMetaExt_, readRange.UpperLimit()),
            ApplyUpperKeyLimit(BlockMetaExt_, readRange.UpperLimit()));

        if (beginIndex == lastIndex) {
            ++beginIndex;
        }
        YCHECK(beginIndex > lastIndex);

        for (int index = beginIndex; index < endIndex; ++index) {
            BlockIndexes_.push_back(index);
            lastIndex = index;
        }
    }
}

void THorizontalSchemalessChunkReader::InitializeBlockSequencePartition()
{
    YCHECK(ReadRanges_.size() == 1);
    YCHECK(ReadRanges_[0].LowerLimit().IsTrivial());
    YCHECK(ReadRanges_[0].UpperLimit().IsTrivial());

    DownloadChunkMeta(std::vector<int>(), PartitionTag_);
    CreateBlockSequence(0, BlockMetaExt_.blocks_size());
}

void THorizontalSchemalessChunkReader::InitializeBlockSequenceUnsorted()
{
    YCHECK(ReadRanges_.size() == 1);

    DownloadChunkMeta(std::vector<int>());

    CreateBlockSequence(
        ApplyLowerRowLimit(BlockMetaExt_, ReadRanges_[0].LowerLimit()),
        ApplyUpperRowLimit(BlockMetaExt_, ReadRanges_[0].UpperLimit()));
}

void THorizontalSchemalessChunkReader::CreateBlockSequence(int beginIndex, int endIndex)
{
    for (int index = beginIndex; index < endIndex; ++index) {
        BlockIndexes_.push_back(index);
    }
}

TDataStatistics THorizontalSchemalessChunkReader::GetDataStatistics() const
{
    auto dataStatistics = TChunkReaderBase::GetDataStatistics();
    dataStatistics.set_row_count(RowCount_);
    return dataStatistics;
}

void THorizontalSchemalessChunkReader::InitFirstBlock()
{

    int blockIndex = BlockIndexes_[CurrentBlockIndex_];
    const auto& blockMeta = BlockMetaExt_.blocks(blockIndex);

    CheckBlockUpperLimits(blockMeta, ReadRanges_[CurrentRangeIndex_].UpperLimit());

    YCHECK(CurrentBlock_ && CurrentBlock_.IsSet());
    BlockReader_.reset(new THorizontalSchemalessBlockReader(
        CurrentBlock_.Get().ValueOrThrow(),
        blockMeta,
        IdMapping_,
        KeyColumns_.size(),
        SystemColumnCount_));

    RowIndex_ = blockMeta.chunk_row_count() - blockMeta.row_count();

    SkipToCurrrentRange();
}

void THorizontalSchemalessChunkReader::InitNextBlock()
{
    ++CurrentBlockIndex_;
    InitFirstBlock();
}

void THorizontalSchemalessChunkReader::SkipToCurrrentRange()
{
    int blockIndex = BlockIndexes_[CurrentBlockIndex_];
    CheckBlockUpperLimits(BlockMetaExt_.blocks(blockIndex), ReadRanges_[CurrentRangeIndex_].UpperLimit());

    const auto& lowerLimit = ReadRanges_[CurrentRangeIndex_].LowerLimit();

    if (lowerLimit.HasRowIndex() && RowIndex_ < lowerLimit.GetRowIndex()) {
        YCHECK(BlockReader_->SkipToRowIndex(lowerLimit.GetRowIndex() - RowIndex_));
        RowIndex_ = lowerLimit.GetRowIndex();
    }

    if (lowerLimit.HasKey()) {
        auto blockRowIndex = BlockReader_->GetRowIndex();
        YCHECK(BlockReader_->SkipToKey(lowerLimit.GetKey()));
        RowIndex_ += BlockReader_->GetRowIndex() - blockRowIndex;
    }
}

bool THorizontalSchemalessChunkReader::InitNextRange()
{
    RangeEnded_ = false;
    ++CurrentRangeIndex_;

    if (CurrentRangeIndex_ == ReadRanges_.size()) {
        LOG_DEBUG("Upper limit %v reached", ToString(ReadRanges_.back()));
        return false;
    }

    const auto& lowerLimit = ReadRanges_[CurrentRangeIndex_].LowerLimit();
    const auto& blockMeta = BlockMetaExt_.blocks(BlockIndexes_[CurrentBlockIndex_]);

    if ((lowerLimit.HasRowIndex() && blockMeta.chunk_row_count() <= lowerLimit.GetRowIndex()) ||
        (lowerLimit.HasKey() && CompareRows(FromProto<TOwningKey>(blockMeta.last_key()), lowerLimit.GetKey()) < 0))
    {
        BlockReader_.reset();
        return OnBlockEnded();
    } else {
        SkipToCurrrentRange();
        return true;
    }
}

bool THorizontalSchemalessChunkReader::Read(std::vector<TUnversionedRow>* rows)
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

    if (RangeEnded_) {
        return InitNextRange();
    }

    if (BlockEnded_) {
        BlockReader_.reset();
        return OnBlockEnded();
    }

    i64 dataWeight = 0;
    while (rows->size() < rows->capacity() && dataWeight < Config_->MaxDataSizePerRead) {
        if ((CheckRowLimit_ && RowIndex_ >= ReadRanges_[CurrentRangeIndex_].UpperLimit().GetRowIndex()) ||
            (CheckKeyLimit_ && CompareRows(BlockReader_->GetKey(), ReadRanges_[CurrentRangeIndex_].UpperLimit().GetKey()) >= 0))
        {
            RangeEnded_ = true;
            return true;
        }

        if (!RowSampler_ || RowSampler_->ShouldTakeRow(GetTableRowIndex())) {
            auto row = BlockReader_->GetRow(&MemoryPool_);
            if (Options_->EnableRangeIndex) {
                *row.End() = MakeUnversionedInt64Value(ChunkSpec_.range_index(), RangeIndexId_);
                ++row.GetHeader()->Count;
            }
            if (Options_->EnableTableIndex) {
                *row.End() = MakeUnversionedInt64Value(ChunkSpec_.table_index(), TableIndexId_);
                ++row.GetHeader()->Count;
            }
            if (Options_->EnableRowIndex) {
                *row.End() = MakeUnversionedInt64Value(GetTableRowIndex(), RowIndexId_);
                ++row.GetHeader()->Count;
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

////////////////////////////////////////////////////////////////////////////////

class TColumnarSchemalessChunkReader
    : public TSchemalessChunkReaderBase
    , public TColumnarRangeChunkReaderBase
{
public:
    TColumnarSchemalessChunkReader(
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
        LowerLimit_ = readRange.LowerLimit();
        UpperLimit_ = readRange.UpperLimit();

        ReadyEvent_ = BIND(&TColumnarSchemalessChunkReader::InitializeBlockSequence, MakeStrong(this))
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

        while (rows->size() < rows->capacity()) {
            ResetExhaustedColumns();

            // Define how many to read.
            i64 rowLimit = std::min(HardUpperRowIndex_ - RowIndex_, static_cast<i64>(rows->capacity() - rows->size()));
            for (const auto& column : Columns_) {
                rowLimit = std::min(column.ColumnReader->GetReadyUpperRowIndex() - RowIndex_, rowLimit);
            }
            YCHECK(rowLimit > 0);

            std::vector<ui32> schemalessColumnCount(rowLimit, 0);
            if (SchemalessReader_) {
                SchemalessReader_->GetValueCounts(TMutableRange<ui32>(
                    schemalessColumnCount.data(),
                    schemalessColumnCount.size()));
            }

            int rangeBegin = rows->size();
            for (i64 index = 0; index < rowLimit; ++index) {
                auto row = TMutableUnversionedRow::Allocate(
                    &Pool_,
                    SchemaColumnReaders_.size() + schemalessColumnCount[index] + SystemColumnCount_);
                rows->push_back(row);
            }

            auto range = TMutableRange<TMutableUnversionedRow>(
                static_cast<TMutableUnversionedRow*>(rows->data() + rangeBegin),
                static_cast<TMutableUnversionedRow*>(rows->data() + rangeBegin + rowLimit));

            // Read values.
            for (auto& columnReader : SchemaColumnReaders_) {
                columnReader->ReadValues(range);
            }
            if (SchemalessReader_) {
                SchemalessReader_->ReadValues(range);
            }

            // Append system columns.
            for (i64 index = 0; index < rowLimit; ++index) {
                auto row = range[index];
                if (Options_->EnableRangeIndex) {
                    *row.End() = MakeUnversionedInt64Value(ChunkSpec_.range_index(), RangeIndexId_);
                    ++row.GetHeader()->Count;
                }
                if (Options_->EnableTableIndex) {
                    *row.End() = MakeUnversionedInt64Value(ChunkSpec_.table_index(), TableIndexId_);
                    ++row.GetHeader()->Count;
                }
                if (Options_->EnableRowIndex) {
                    *row.End() = MakeUnversionedInt64Value(
                        GetTableRowIndex() + index,
                        RowIndexId_);
                    ++row.GetHeader()->Count;
                }
            }

            if (RowIndex_ + rowLimit > SafeUpperRowIndex_) {
                i64 index = std::max(SafeUpperRowIndex_ - RowIndex_, i64(0));
                for (; index < rowLimit; ++index) {
                    if (range[index] >= UpperLimit_.GetKey()) {
                        Completed_ = true;
                        range = range.Slice(range.Begin(), range.Begin() + index);
                        rows->resize(rows->size() - rowLimit + index);
                        break;
                    }
                }
            } else if (RowIndex_ + rowLimit == HardUpperRowIndex_) {
                Completed_ = true;
            }

            RowIndex_ += range.Size();
            if (Completed_ || !TryFetchNextRow()) {
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

    virtual TDataStatistics GetDataStatistics() const override
    {
        auto dataStatistics = TColumnarRangeChunkReaderBase::GetDataStatistics();
        dataStatistics.set_row_count(RowCount_);
        return dataStatistics;
    }

private:
    std::vector<std::unique_ptr<IUnversionedColumnReader>> SchemaColumnReaders_;
    std::unique_ptr<ISchemalessColumnReader> SchemalessReader_ = nullptr;

    bool Completed_ = false;

    TChunkedMemoryPool Pool_;

    void InitializeBlockSequence()
    {
        YCHECK(ChunkSpec_.chunk_meta().version() == static_cast<int>(ETableChunkFormat::UnversionedColumnar));
        InitializeSystemColumnIds();

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

        auto chunkNameTable = FromProto<TNameTablePtr>(GetProtoExtension<TNameTableExt>(
            chunkMeta.extensions()));

        ChunkMeta_ = New<TColumnarChunkMeta>(std::move(chunkMeta));

        bool sortedRead = UpperLimit_.HasKey() ||
                          LowerLimit_.HasKey() ||
                          !KeyColumns_.empty();

        if (sortedRead && !ChunkMeta_->Misc().sorted()) {
            THROW_ERROR_EXCEPTION("Requested a sorted read for an unsorted chunk");
        }

        ValidateKeyColumns(KeyColumns_, ChunkMeta_->ChunkSchema().GetKeyColumns());
        if (sortedRead && KeyColumns_.empty()) {
            KeyColumns_ = ChunkMeta_->ChunkSchema().GetKeyColumns();
        }

        if (UpperLimit_.HasKey() || LowerLimit_.HasKey()) {
            ChunkMeta_->InitBlockLastKeys(KeyColumns_.size());
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
                } else if (chunkColumnId < KeyColumns_.size()) {
                    THROW_ERROR_EXCEPTION("All key columns must be included in column filter for sorted read");
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

            Columns_.emplace_back(columnReader.get(), columnIndex);
            SchemaColumnReaders_.emplace_back(std::move(columnReader));
        }

        if (readSchemalessColumns) {
            SchemalessReader_ = CreateSchemalessColumnReader(
                ChunkMeta_->ColumnMeta().columns(ChunkMeta_->ChunkSchema().Columns().size()),
                schemalessIdMapping,
                SchemaColumnReaders_.size());

            Columns_.emplace_back(
                SchemalessReader_.get(),
                ChunkMeta_->ChunkSchema().Columns().size());
        }

        InitLowerRowIndex();
        InitUpperRowIndex();

        if (LowerRowIndex_ < HardUpperRowIndex_) {
            // We must continue initialization and set RowIndex_ before
            // ReadyEvent is set for the first time.
            InitBlockFetcher();
            WaitFor(RequestFirstBlocks())
                .ThrowOnError();

            ResetExhaustedColumns();
            Initialize(MakeRange(SchemaColumnReaders_.data(), KeyColumns_.size()));
            RowIndex_ = LowerRowIndex_;

            if (RowIndex_ >= HardUpperRowIndex_) {
                Completed_ = true;
            }
        } else {
            Completed_ = true;
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

ISchemalessChunkReaderPtr CreateSchemalessChunkReader(
    const TChunkSpec& chunkSpec,
    TChunkReaderConfigPtr config,
    TChunkReaderOptionsPtr options,
    NChunkClient::IChunkReaderPtr underlyingReader,
    TNameTablePtr nameTable,
    NChunkClient::IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns,
    const TColumnFilter& columnFilter,
    std::vector<NChunkClient::TReadRange> readRanges,
    TNullable<int> partitionTag)
{
    auto type = EChunkType(chunkSpec.chunk_meta().type());
    YCHECK(type == EChunkType::Table);

    ValidateReadRanges(readRanges);

    auto formatVersion = ETableChunkFormat(chunkSpec.chunk_meta().version());

    switch (formatVersion) {
        case ETableChunkFormat::SchemalessHorizontal:
            return New<THorizontalSchemalessChunkReader>(
                chunkSpec,
                config,
                options,
                underlyingReader,
                nameTable,
                blockCache,
                keyColumns,
                columnFilter,
                std::move(readRanges),
                std::move(partitionTag));

        case ETableChunkFormat::Old: {
            YCHECK(readRanges.size() <= 1);
            YCHECK(!partitionTag);

            return New<TLegacyTableChunkReader>(
                chunkSpec,
                config,
                options,
                columnFilter,
                nameTable,
                keyColumns,
                underlyingReader,
                blockCache);
        }

        case ETableChunkFormat::UnversionedColumnar:
            YCHECK(readRanges.size() <= 1);
            return New<TColumnarSchemalessChunkReader>(
                chunkSpec,
                config,
                options,
                underlyingReader,
                nameTable,
                blockCache,
                keyColumns,
                columnFilter,
                readRanges.empty() ? TReadRange() : readRanges.front());

        default:
            YUNREACHABLE();
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
    IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const std::vector<TChunkSpec>& chunkSpecs,
    TNameTablePtr nameTable,
    TColumnFilter columnFilter,
    const TKeyColumns& keyColumns,
    TNullable<int> partitionTag,
    IThroughputThrottlerPtr throttler)
{
    std::vector<IReaderFactoryPtr> factories;
    for (const auto& chunkSpec : chunkSpecs) {
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
            std::vector<TReadRange>(1, std::move(range)),
            partitionTag);
        };

        factories.emplace_back(CreateReaderFactory(createReader, memoryEstimate));
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
        IClientPtr client,
        const TNodeDescriptor& localDescriptor,
        IBlockCachePtr blockCache,
        TNodeDirectoryPtr nodeDirectory,
        const std::vector<TChunkSpec>& chunkSpecs,
        TNameTablePtr nameTable,
        TColumnFilter columnFilter,
        const TKeyColumns& keyColumns,
        TNullable<int> partitionTag,
        IThroughputThrottlerPtr throttler);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;

    virtual i64 GetSessionRowIndex() const override;

    virtual i64 GetTotalRowCount() const override;

    virtual TNameTablePtr GetNameTable() const override;

    virtual TKeyColumns GetKeyColumns() const override;

    virtual i64 GetTableRowIndex() const override;

private:
    const TNameTablePtr NameTable_;
    const TKeyColumns KeyColumns_;

    ISchemalessChunkReaderPtr CurrentReader_;
    std::atomic<i64> RowIndex_ = {0};
    std::atomic<i64> RowCount_ = {-1};

    using TBase::ReadyEvent_;
    using TBase::CurrentSession_;

    virtual void OnReaderSwitched() override;
};

////////////////////////////////////////////////////////////////////////////////

template <class TBase>
TSchemalessMultiChunkReader<TBase>::TSchemalessMultiChunkReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const std::vector<TChunkSpec>& chunkSpecs,
    TNameTablePtr nameTable,
    TColumnFilter columnFilter,
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
            chunkSpecs,
            nameTable,
            columnFilter,
            keyColumns,
            partitionTag,
            throttler))
    , NameTable_(nameTable)
    , KeyColumns_(keyColumns)
    , RowCount_(GetCumulativeRowCount(chunkSpecs))
{ }

template <class TBase>
bool TSchemalessMultiChunkReader<TBase>::Read(std::vector<TUnversionedRow>* rows)
{
    rows->clear();
    if (!ReadyEvent_.IsSet() || !ReadyEvent_.Get().IsOK()) {
        return true;
    }

    // Nothing to read.
    if (!CurrentReader_) {
        return false;
    }

    bool readerFinished = !CurrentReader_->Read(rows);
    if (!rows->empty()) {
        RowIndex_ += rows->size();
        return true;
    }

    if (TBase::OnEmptyRead(readerFinished)) {
        return true;
    } else {
        RowCount_ = RowIndex_.load();
        CurrentReader_ = nullptr;
        return false;
    }
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
TNameTablePtr TSchemalessMultiChunkReader<TBase>::GetNameTable() const
{
    return NameTable_;
}

template <class TBase>
TKeyColumns TSchemalessMultiChunkReader<TBase>::GetKeyColumns() const
{
    return KeyColumns_;
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessSequentialMultiChunkReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const std::vector<TChunkSpec>& chunkSpecs,
    TNameTablePtr nameTable,
    TColumnFilter columnFilter,
    const TKeyColumns& keyColumns,
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
        chunkSpecs,
        nameTable,
        columnFilter,
        keyColumns,
        partitionTag,
        throttler);

    reader->Open();
    return reader;
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessParallelMultiChunkReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const std::vector<TChunkSpec>& chunkSpecs,
    TNameTablePtr nameTable,
    TColumnFilter columnFilter,
    const TKeyColumns& keyColumns,
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
        chunkSpecs,
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
        IClientPtr client,
        const TNodeDescriptor& localDescriptor,
        IBlockCachePtr blockCache,
        TNodeDirectoryPtr nodeDirectory,
        const std::vector<TChunkSpec>& chunkSpecs,
        TNameTablePtr nameTable,
        TColumnFilter columnFilter,
        const TTableSchema& tableSchema,
        IThroughputThrottlerPtr throttler);

    virtual TFuture<void> GetReadyEvent() override;
    virtual bool Read(std::vector<TUnversionedRow>* rows) override;
    virtual TDataStatistics GetDataStatistics() const override;
    virtual std::vector<TChunkId> GetFailedChunkIds() const override;
    virtual bool IsFetchingCompleted() const override;
    virtual i64 GetSessionRowIndex() const override;
    virtual i64 GetTotalRowCount() const override;
    virtual TNameTablePtr GetNameTable() const override;
    virtual TKeyColumns GetKeyColumns() const override;
    virtual i64 GetTableRowIndex() const override;

private:
    const ISchemafulReaderPtr UnderlyingReader_;
    const TKeyColumns KeyColumns_;
    const TNameTablePtr NameTable_;

    i64 RowIndex_ = 0;
    const i64 RowCount_;

    TSchemalessMergingMultiChunkReader(
        ISchemafulReaderPtr underlyingReader,
        TKeyColumns keyColumns,
        TNameTablePtr nameTable,
        i64 rowCount);

    DECLARE_NEW_FRIEND();
};

////////////////////////////////////////////////////////////////////////////////

TSchemalessMergingMultiChunkReader::TSchemalessMergingMultiChunkReader(
    ISchemafulReaderPtr underlyingReader,
    TKeyColumns keyColumns,
    TNameTablePtr nameTable,
    i64 rowCount)
    : UnderlyingReader_(std::move(underlyingReader))
    , KeyColumns_(keyColumns)
    , NameTable_(nameTable)
    , RowCount_(rowCount)
{ }

ISchemalessMultiChunkReaderPtr TSchemalessMergingMultiChunkReader::Create(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const std::vector<TChunkSpec>& chunkSpecs,
    TNameTablePtr nameTable,
    TColumnFilter columnFilter,
    const TTableSchema& tableSchema,
    IThroughputThrottlerPtr throttler)
{
    try {
        for (const auto& column : tableSchema.Columns()) {
            nameTable->RegisterName(column.Name);
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Failed to update name table for schemaless merging reader") 
            << ex;
    }

    std::vector<TOwningKey> boundaries;
    boundaries.reserve(chunkSpecs.size());

    for (const auto& chunkSpec : chunkSpecs) {
        auto boundaryKeysExt = GetProtoExtension<TBoundaryKeysExt>(chunkSpec.chunk_meta().extensions());
        auto minKey = WidenKey(NYT::FromProto<TOwningKey>(boundaryKeysExt.min()), tableSchema.GetKeyColumnCount());
        boundaries.push_back(minKey);
    }

    auto performanceCounters = New<TChunkReaderPerformanceCounters>();

    auto createReader = [
        config,
        options,
        client,
        localDescriptor,
        blockCache,
        nodeDirectory,
        chunkSpecs,
        nameTable,
        columnFilter,
        tableSchema,
        performanceCounters
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
            tableSchema);
        auto chunkMeta = WaitFor(asyncChunkMeta)
            .ValueOrThrow();

        return CreateVersionedChunkReader(
            config,
            std::move(chunkReader),
            blockCache,
            std::move(chunkMeta),
            lowerLimit,
            upperLimit,
            columnFilter,
            performanceCounters,
            AsyncLastCommittedTimestamp);
    };

    struct TSchemalessMergingMultiChunkReaderBufferTag
    { };

    auto rowMerger = New<TSchemafulRowMerger>(
        New<TRowBuffer>(TSchemalessMergingMultiChunkReaderBufferTag()),
        tableSchema.Columns().size(),
        tableSchema.GetKeyColumnCount(),
        columnFilter,
        client->GetConnection()->GetColumnEvaluatorCache()->Find(tableSchema));

    auto reader = CreateSchemafulOverlappingRangeReader(
        std::move(boundaries),
        std::move(rowMerger),
        createReader,
        [] (
            const TUnversionedValue* lhsBegin,
            const TUnversionedValue* lhsEnd,
            const TUnversionedValue* rhsBegin,
            const TUnversionedValue* rhsEnd)
        {
            return CompareRows(lhsBegin, lhsEnd, rhsBegin, rhsEnd);
        });

    i64 rowCount = GetCumulativeRowCount(chunkSpecs);

    return New<TSchemalessMergingMultiChunkReader>(
        std::move(reader),
        tableSchema.GetKeyColumns(),
        nameTable,
        rowCount);
}

bool TSchemalessMergingMultiChunkReader::Read(std::vector<TUnversionedRow>* rows)
{
    bool result = UnderlyingReader_->Read(rows);
    RowIndex_ += rows->size();
    return result;
}

TFuture<void> TSchemalessMergingMultiChunkReader::GetReadyEvent()
{
    return UnderlyingReader_->GetReadyEvent();
}

TDataStatistics TSchemalessMergingMultiChunkReader::GetDataStatistics() const
{
    TDataStatistics dataStatistics;
    return dataStatistics;
}

std::vector<TChunkId> TSchemalessMergingMultiChunkReader::GetFailedChunkIds() const
{
    return std::vector<TChunkId>();
}

bool TSchemalessMergingMultiChunkReader::IsFetchingCompleted() const
{
    return false;
}

i64 TSchemalessMergingMultiChunkReader::GetTotalRowCount() const
{
    return RowCount_;
}

i64 TSchemalessMergingMultiChunkReader::GetSessionRowIndex() const
{
    return RowIndex_;
}

i64 TSchemalessMergingMultiChunkReader::GetTableRowIndex() const
{
    return 0;
}

TNameTablePtr TSchemalessMergingMultiChunkReader::GetNameTable() const
{
    return NameTable_;
}

TKeyColumns TSchemalessMergingMultiChunkReader::GetKeyColumns() const
{
    return KeyColumns_;
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessMergingMultiChunkReader(
    TTableReaderConfigPtr config,
    TTableReaderOptionsPtr options,
    IClientPtr client,
    const TNodeDescriptor& localDescriptor,
    IBlockCachePtr blockCache,
    TNodeDirectoryPtr nodeDirectory,
    const std::vector<TChunkSpec>& chunkSpecs,
    TNameTablePtr nameTable,
    TColumnFilter columnFilter,
    const TTableSchema& tableSchema,
    IThroughputThrottlerPtr throttler)
{
    return TSchemalessMergingMultiChunkReader::Create(
        config,
        options,
        client,
        localDescriptor,
        blockCache,
        nodeDirectory,
        chunkSpecs,
        nameTable,
        columnFilter,
        tableSchema,
        throttler);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
