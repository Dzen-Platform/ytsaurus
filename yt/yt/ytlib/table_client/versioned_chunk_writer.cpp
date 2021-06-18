#include "versioned_chunk_writer.h"

#include "chunk_meta_extensions.h"
#include "config.h"
#include "private.h"
#include "row_merger.h"
#include "versioned_block_writer.h"

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/ytlib/table_client/helpers.h>

#include <yt/yt/ytlib/table_chunk_format/column_writer.h>
#include <yt/yt/ytlib/table_chunk_format/data_block_writer.h>
#include <yt/yt/ytlib/table_chunk_format/timestamp_writer.h>

#include <yt/yt/ytlib/chunk_client/block_cache.h>
#include <yt/yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/yt/ytlib/chunk_client/chunk_writer.h>
#include <yt/yt/ytlib/chunk_client/config.h>
#include <yt/yt/ytlib/chunk_client/dispatcher.h>
#include <yt/yt/ytlib/chunk_client/deferred_chunk_meta.h>
#include <yt/yt/ytlib/chunk_client/encoding_chunk_writer.h>
#include <yt/yt/ytlib/chunk_client/encoding_writer.h>
#include <yt/yt/ytlib/chunk_client/multi_chunk_writer_base.h>

#include <yt/yt/client/table_client/unversioned_row.h>
#include <yt/yt/client/table_client/versioned_writer.h>
#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/schema.h>

#include <yt/yt/core/misc/range.h>
#include <yt/yt/core/misc/random.h>

#include <util/generic/ylimits.h>

namespace NYT::NTableClient {

using namespace NTableChunkFormat;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NRpc;
using namespace NTransactionClient;
using namespace NObjectClient;
using namespace NApi;
using namespace NTableClient::NProto;

using NYT::TRange;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static constexpr i64 MinRowRangeDataWeight = 64_KB;

////////////////////////////////////////////////////////////////////////////////

class TVersionedChunkWriterBase
    : public IVersionedChunkWriter
{
public:
    TVersionedChunkWriterBase(
        TChunkWriterConfigPtr config,
        TChunkWriterOptionsPtr options,
        TTableSchemaPtr schema,
        IChunkWriterPtr chunkWriter,
        IBlockCachePtr blockCache)
        : Logger(TableClientLogger.WithTag("ChunkWriterId: %v", TGuid::Create()))
        , Config_(config)
        , Schema_(schema)
        , EncodingChunkWriter_(New<TEncodingChunkWriter>(
            std::move(config),
            std::move(options),
            std::move(chunkWriter),
            std::move(blockCache),
            Logger))
        , LastKey_(static_cast<TUnversionedValue*>(nullptr), static_cast<TUnversionedValue*>(nullptr))
        , MinTimestamp_(MaxTimestamp)
        , MaxTimestamp_(MinTimestamp)
        , RandomGenerator_(RandomNumber<ui64>())
        , SamplingThreshold_(static_cast<ui64>(MaxFloor<ui64>() * Config_->SampleRate))
        , SamplingRowMerger_(New<TRowBuffer>(TVersionedChunkWriterBaseTag()), Schema_)
#if 0
        , KeyFilter_(Config_->MaxKeyFilterSize, Config_->KeyFilterFalsePositiveRate)
#endif
    { }

    virtual TFuture<void> GetReadyEvent() override
    {
        return EncodingChunkWriter_->GetReadyEvent();
    }

    virtual i64 GetRowCount() const override
    {
        return RowCount_;
    }

    virtual bool Write(TRange<TVersionedRow> rows) override
    {
        if (rows.Empty()) {
            return EncodingChunkWriter_->IsReady();
        }

        SamplingRowMerger_.Reset();

        if (RowCount_ == 0) {
            auto firstRow = rows.Front();
            ToProto(
                BoundaryKeysExt_.mutable_min(),
                TLegacyOwningKey(firstRow.BeginKeys(), firstRow.EndKeys()));
            EmitSample(firstRow);
        }

        DoWriteRows(rows);

        auto lastRow = rows.Back();
        LastKey_ = TLegacyOwningKey(lastRow.BeginKeys(), lastRow.EndKeys());

        return EncodingChunkWriter_->IsReady();
    }

    virtual TFuture<void> Close() override
    {
        // psushin@ forbids empty chunks :)
        YT_VERIFY(RowCount_ > 0);

        return BIND(&TVersionedChunkWriterBase::DoClose, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

    virtual i64 GetMetaSize() const override
    {
        // Other meta parts are negligible.
        return BlockMetaExtSize_ + SamplesExtSize_;
    }

    virtual bool IsCloseDemanded() const override
    {
        return EncodingChunkWriter_->IsCloseDemanded();
    }

    virtual TDeferredChunkMetaPtr GetMeta() const override
    {
        return EncodingChunkWriter_->GetMeta();
    }

    virtual TChunkId GetChunkId() const override
    {
        return EncodingChunkWriter_->GetChunkId();
    }

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override
    {
        return EncodingChunkWriter_->GetDataStatistics();
    }

    virtual TCodecStatistics GetCompressionStatistics() const override
    {
        return EncodingChunkWriter_->GetCompressionStatistics();
    }

    virtual i64 GetDataWeight() const override
    {
        return DataWeight_;
    }

protected:
    const NLogging::TLogger Logger;

    const TChunkWriterConfigPtr Config_;
    const TTableSchemaPtr Schema_;

    TEncodingChunkWriterPtr EncodingChunkWriter_;

    TLegacyOwningKey LastKey_;

    TBlockMetaExt BlockMetaExt_;
    i64 BlockMetaExtSize_ = 0;

    TSamplesExt SamplesExt_;
    i64 SamplesExtSize_ = 0;

    i64 DataWeight_ = 0;

    TBoundaryKeysExt BoundaryKeysExt_;

    i64 RowCount_ = 0;

    TTimestamp MinTimestamp_;
    TTimestamp MaxTimestamp_;

    TRandomGenerator RandomGenerator_;
    const ui64 SamplingThreshold_;

    struct TVersionedChunkWriterBaseTag { };
    TSamplingRowMerger SamplingRowMerger_;

    NProto::TColumnarStatisticsExt ColumnarStatisticsExt_;

#if 0
    TBloomFilterBuilder KeyFilter_;
#endif

    virtual void DoClose() = 0;
    virtual void DoWriteRows(TRange<TVersionedRow> rows) = 0;
    virtual EChunkFormat GetChunkFormat() const = 0;

    void FillCommonMeta(TChunkMeta* meta) const
    {
        meta->set_type(ToProto<int>(EChunkType::Table));
        meta->set_format(ToProto<int>(GetChunkFormat()));

        SetProtoExtension(meta->mutable_extensions(), BoundaryKeysExt_);
    }

    virtual void PrepareChunkMeta()
    {
        ToProto(BoundaryKeysExt_.mutable_max(), LastKey_);

        auto meta = EncodingChunkWriter_->GetMeta();
        FillCommonMeta(meta.Get());
        SetProtoExtension(meta->mutable_extensions(), ToProto<TTableSchemaExt>(Schema_));
        SetProtoExtension(meta->mutable_extensions(), BlockMetaExt_);
        SetProtoExtension(meta->mutable_extensions(), SamplesExt_);
        SetProtoExtension(meta->mutable_extensions(), ColumnarStatisticsExt_);

#if 0
        if (KeyFilter_.IsValid()) {
            KeyFilter_.Shrink();
            //FIXME: write bloom filter to chunk.
        }
#endif

        auto& miscExt = EncodingChunkWriter_->MiscExt();
        miscExt.set_sorted(true);
        miscExt.set_row_count(RowCount_);
        miscExt.set_data_weight(DataWeight_);
    }

    void EmitSampleRandomly(TVersionedRow row)
    {
        if (RandomGenerator_.Generate<ui64>() < SamplingThreshold_) {
            EmitSample(row);
        }
    }

    void EmitSample(TVersionedRow row)
    {
        auto mergedRow = SamplingRowMerger_.MergeRow(row);
        ToProto(SamplesExt_.add_entries(), mergedRow);
        SamplesExtSize_ += SamplesExt_.entries(SamplesExt_.entries_size() - 1).length();
    }

    static void ValidateRowsOrder(
        TVersionedRow row,
        const TUnversionedValue* beginPrevKey,
        const TUnversionedValue* endPrevKey)
    {
        YT_VERIFY(
            !beginPrevKey && !endPrevKey ||
            CompareRows(beginPrevKey, endPrevKey, row.BeginKeys(), row.EndKeys()) < 0);
    }

    static void ValidateRowDataWeight(TVersionedRow row, i64 dataWeight)
    {
        if (dataWeight > MaxServerVersionedRowDataWeight) {
            THROW_ERROR_EXCEPTION("Versioned row data weight is too large")
                << TErrorAttribute("key", RowToKey(row))
                << TErrorAttribute("actual_data_weight", dataWeight)
                << TErrorAttribute("max_data_weight", MaxServerVersionedRowDataWeight);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

class TSimpleVersionedChunkWriter
    : public TVersionedChunkWriterBase
{
public:
    TSimpleVersionedChunkWriter(
        TChunkWriterConfigPtr config,
        TChunkWriterOptionsPtr options,
        TTableSchemaPtr schema,
        IChunkWriterPtr chunkWriter,
        IBlockCachePtr blockCache)
        : TVersionedChunkWriterBase(
            std::move(config),
            std::move(options),
            std::move(schema),
            std::move(chunkWriter),
            std::move(blockCache))
        , BlockWriter_(new TSimpleVersionedBlockWriter(Schema_))
    { }

    virtual i64 GetCompressedDataSize() const override
    {
        return
            EncodingChunkWriter_->GetDataStatistics().compressed_data_size() +
            (BlockWriter_ ? BlockWriter_->GetBlockSize() : 0);
    }

private:
    std::unique_ptr<TSimpleVersionedBlockWriter> BlockWriter_;

    virtual void DoWriteRows(TRange<TVersionedRow> rows) override
    {
        if (rows.Empty()) {
            return;
        }

        //FIXME: insert key into bloom filter.
        //KeyFilter_.Insert(GetFarmFingerprint(rows.front().BeginKeys(), rows.front().EndKeys()));
        auto firstRow = rows.Front();

        WriteRow(firstRow, LastKey_.Begin(), LastKey_.End());
        FinishBlockIfLarge(firstRow);

        int rowCount = static_cast<int>(rows.Size());
        for (int index = 1; index < rowCount; ++index) {
            //KeyFilter_.Insert(GetFarmFingerprint(rows[i].BeginKeys(), rows[i].EndKeys()));
            WriteRow(rows[index], rows[index - 1].BeginKeys(), rows[index - 1].EndKeys());
            FinishBlockIfLarge(rows[index]);
        }
    }

    static void ValidateRow(
        TVersionedRow row,
        i64 dataWeight,
        const TUnversionedValue* beginPrevKey,
        const TUnversionedValue* endPrevKey)
    {
        ValidateRowsOrder(row, beginPrevKey, endPrevKey);
        ValidateRowDataWeight(row, dataWeight);

        if (row.GetWriteTimestampCount() > MaxTimestampCountPerRow) {
            THROW_ERROR_EXCEPTION("Too many write timestamps in a versioned row")
                << TErrorAttribute("key", RowToKey(row));
        }
        if (row.GetDeleteTimestampCount() > MaxTimestampCountPerRow) {
            THROW_ERROR_EXCEPTION("Too many delete timestamps in a versioned row")
                << TErrorAttribute("key", RowToKey(row));
        }
    }

    void WriteRow(
        TVersionedRow row,
        const TUnversionedValue* beginPreviousKey,
        const TUnversionedValue* endPreviousKey)
    {
        EmitSampleRandomly(row);
        auto rowWeight = NTableClient::GetDataWeight(row);

        ValidateRow(row, rowWeight, beginPreviousKey, endPreviousKey);

        ++RowCount_;
        DataWeight_ += rowWeight;

        UpdateColumnarStatistics(ColumnarStatisticsExt_, row);

        BlockWriter_->WriteRow(row);
    }

    void FinishBlockIfLarge(TVersionedRow row)
    {
        if (BlockWriter_->GetBlockSize() < Config_->BlockSize) {
            return;
        }

        FinishBlock(row.BeginKeys(), row.EndKeys());
        BlockWriter_.reset(new TSimpleVersionedBlockWriter(Schema_));
    }

    void FinishBlock(const TUnversionedValue* beginKey, const TUnversionedValue* endKey)
    {
        auto block = BlockWriter_->FlushBlock();
        block.Meta.set_chunk_row_count(RowCount_);
        block.Meta.set_block_index(BlockMetaExt_.blocks_size());
        ToProto(block.Meta.mutable_last_key(), beginKey, endKey);

        YT_VERIFY(block.Meta.uncompressed_size() > 0);

        BlockMetaExtSize_ += block.Meta.ByteSizeLong();

        BlockMetaExt_.add_blocks()->Swap(&block.Meta);
        EncodingChunkWriter_->WriteBlock(std::move(block.Data));

        MaxTimestamp_ = std::max(MaxTimestamp_, BlockWriter_->GetMaxTimestamp());
        MinTimestamp_ = std::min(MinTimestamp_, BlockWriter_->GetMinTimestamp());
    }

    virtual void PrepareChunkMeta() override
    {
        TVersionedChunkWriterBase::PrepareChunkMeta();

        auto& miscExt = EncodingChunkWriter_->MiscExt();
        miscExt.set_min_timestamp(MinTimestamp_);
        miscExt.set_max_timestamp(MaxTimestamp_);
    }

    virtual void DoClose() override
    {
        if (BlockWriter_->GetRowCount() > 0) {
            FinishBlock(LastKey_.Begin(), LastKey_.End());
        }

        PrepareChunkMeta();

        EncodingChunkWriter_->Close();
    }

    virtual EChunkFormat GetChunkFormat() const override
    {
        return TSimpleVersionedBlockWriter::FormatVersion;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TColumnarVersionedChunkWriter
    : public TVersionedChunkWriterBase
{
public:
    TColumnarVersionedChunkWriter(
        TChunkWriterConfigPtr config,
        TChunkWriterOptionsPtr options,
        TTableSchemaPtr schema,
        IChunkWriterPtr chunkWriter,
        IBlockCachePtr blockCache)
        : TVersionedChunkWriterBase(
            std::move(config),
            std::move(options),
            std::move(schema),
            std::move(chunkWriter),
            std::move(blockCache))
        , DataToBlockFlush_(Config_->BlockSize)
    {
        // Only scan-optimized version for now.
        THashMap<TString, TDataBlockWriter*> groupBlockWriters;
        for (const auto& columnSchema : Schema_->Columns()) {
            if (columnSchema.Group() && groupBlockWriters.find(*columnSchema.Group()) == groupBlockWriters.end()) {
                auto blockWriter = std::make_unique<TDataBlockWriter>();
                groupBlockWriters[*columnSchema.Group()] = blockWriter.get();
                BlockWriters_.emplace_back(std::move(blockWriter));
            }
        }

        auto getBlockWriter = [&] (const NTableClient::TColumnSchema& columnSchema) -> TDataBlockWriter* {
            if (columnSchema.Group()) {
                return groupBlockWriters[*columnSchema.Group()];
            } else {
                BlockWriters_.emplace_back(std::make_unique<TDataBlockWriter>());
                return BlockWriters_.back().get();
            }
        };

        // Key columns.
        for (int keyColumnIndex = 0; keyColumnIndex < Schema_->GetKeyColumnCount(); ++keyColumnIndex) {
            const auto& columnSchema = Schema_->Columns()[keyColumnIndex];
            ValueColumnWriters_.emplace_back(CreateUnversionedColumnWriter(
                keyColumnIndex,
                columnSchema,
                getBlockWriter(columnSchema)));
        }

        // Non-key columns.
        for (
            int valueColumnIndex = Schema_->GetKeyColumnCount();
            valueColumnIndex < std::ssize(Schema_->Columns());
            ++valueColumnIndex)
        {
            const auto& columnSchema = Schema_->Columns()[valueColumnIndex];
            ValueColumnWriters_.emplace_back(CreateVersionedColumnWriter(
                valueColumnIndex,
                columnSchema,
                getBlockWriter(columnSchema)));
        }

        auto blockWriter = std::make_unique<TDataBlockWriter>();
        TimestampWriter_ = CreateTimestampWriter(blockWriter.get());
        BlockWriters_.emplace_back(std::move(blockWriter));

        YT_VERIFY(BlockWriters_.size() > 1);
    }

    virtual i64 GetCompressedDataSize() const override
    {
        i64 result = EncodingChunkWriter_->GetDataStatistics().compressed_data_size();
        for (const auto& blockWriter : BlockWriters_) {
            result += blockWriter->GetCurrentSize();
        }
        return result;
    }

    virtual i64 GetMetaSize() const override
    {
        i64 metaSize = 0;
        for (const auto& valueColumnWriter : ValueColumnWriters_) {
            metaSize += valueColumnWriter->GetMetaSize();
        }
        metaSize += TimestampWriter_->GetMetaSize();

        return metaSize + TVersionedChunkWriterBase::GetMetaSize();
    }

private:
    std::vector<std::unique_ptr<TDataBlockWriter>> BlockWriters_;
    std::vector<std::unique_ptr<IValueColumnWriter>> ValueColumnWriters_;
    std::unique_ptr<ITimestampWriter> TimestampWriter_;

    i64 DataToBlockFlush_;

    virtual void DoWriteRows(TRange<TVersionedRow> rows) override
    {
        int startRowIndex = 0;
        while (startRowIndex < std::ssize(rows)) {
            i64 weight = 0;
            int rowIndex = startRowIndex;
            for (; rowIndex < std::ssize(rows) && weight < DataToBlockFlush_; ++rowIndex) {
                auto row = rows[rowIndex];
                auto rowWeight = NTableClient::GetDataWeight(row);
                if (rowIndex == 0) {
                    ValidateRow(row, rowWeight, LastKey_.Begin(), LastKey_.End());
                } else {
                    ValidateRow(
                        row,
                        rowWeight,
                        rows[rowIndex - 1].BeginKeys(),
                        rows[rowIndex - 1].EndKeys());
                }

                UpdateColumnarStatistics(ColumnarStatisticsExt_, row);

                weight += rowWeight;
            }

            auto range = MakeRange(rows.Begin() + startRowIndex, rows.Begin() + rowIndex);
            for (const auto& columnWriter : ValueColumnWriters_) {
                columnWriter->WriteVersionedValues(range);
            }
            TimestampWriter_->WriteTimestamps(range);

            RowCount_ += range.Size();
            DataWeight_ += weight;

            startRowIndex = rowIndex;

            TryFlushBlock(rows[rowIndex - 1]);
        }

        for (auto row : rows) {
            EmitSampleRandomly(row);
        }
    }

    static void ValidateRow(
        TVersionedRow row,
        i64 dataWeight,
        const TUnversionedValue* beginPrevKey,
        const TUnversionedValue* endPrevKey)
    {
        ValidateRowsOrder(row, beginPrevKey, endPrevKey);
        ValidateRowDataWeight(row, dataWeight);
    }

    void TryFlushBlock(TVersionedRow lastRow)
    {
        while (true) {
            i64 totalSize = 0;
            i64 maxWriterSize = -1;
            int maxWriterIndex = -1;

            for (int i = 0; i < std::ssize(BlockWriters_); ++i) {
                auto size = BlockWriters_[i]->GetCurrentSize();
                totalSize += size;
                if (size > maxWriterSize) {
                    maxWriterIndex = i;
                    maxWriterSize = size;
                }
            }

            YT_VERIFY(maxWriterIndex >= 0);

            if (totalSize > Config_->MaxBufferSize || maxWriterSize > Config_->BlockSize) {
                FinishBlock(maxWriterIndex, lastRow.BeginKeys(), lastRow.EndKeys());
            } else {
                DataToBlockFlush_ = std::min(Config_->MaxBufferSize - totalSize, Config_->BlockSize - maxWriterSize);
                DataToBlockFlush_ = std::max(MinRowRangeDataWeight, DataToBlockFlush_);
                break;
            }
        }
    }

    void FinishBlock(int blockWriterIndex, const TUnversionedValue* beginKey, const TUnversionedValue* endKey)
    {
        auto block = BlockWriters_[blockWriterIndex]->DumpBlock(BlockMetaExt_.blocks_size(), RowCount_);
        YT_VERIFY(block.Meta.uncompressed_size() > 0);

        block.Meta.set_block_index(BlockMetaExt_.blocks_size());
        ToProto(block.Meta.mutable_last_key(), beginKey, endKey);

        BlockMetaExtSize_ += block.Meta.ByteSizeLong();

        BlockMetaExt_.add_blocks()->Swap(&block.Meta);
        EncodingChunkWriter_->WriteBlock(std::move(block.Data));
    }

    virtual void PrepareChunkMeta() override
    {
        TVersionedChunkWriterBase::PrepareChunkMeta();

        auto& miscExt = EncodingChunkWriter_->MiscExt();
        miscExt.set_min_timestamp(TimestampWriter_->GetMinTimestamp());
        miscExt.set_max_timestamp(TimestampWriter_->GetMaxTimestamp());

        auto meta = EncodingChunkWriter_->GetMeta();
        NProto::TColumnMetaExt columnMetaExt;
        for (const auto& valueColumnWriter : ValueColumnWriters_) {
            *columnMetaExt.add_columns() = valueColumnWriter->ColumnMeta();
        }
        *columnMetaExt.add_columns() = TimestampWriter_->ColumnMeta();
        SetProtoExtension(meta->mutable_extensions(), columnMetaExt);
    }

    virtual void DoClose() override
    {
        for (int i = 0; i < std::ssize(BlockWriters_); ++i) {
            if (BlockWriters_[i]->GetCurrentSize() > 0) {
                FinishBlock(i, LastKey_.Begin(), LastKey_.End());
            }
        }

        PrepareChunkMeta();

        EncodingChunkWriter_->Close();
    }

    virtual EChunkFormat GetChunkFormat() const override
    {
        return EChunkFormat::TableVersionedColumnar;
    }
};

////////////////////////////////////////////////////////////////////////////////

IVersionedChunkWriterPtr CreateVersionedChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    TTableSchemaPtr schema,
    IChunkWriterPtr chunkWriter,
    IBlockCachePtr blockCache)
{
    if (blockCache->GetSupportedBlockTypes() != EBlockType::None) {
        // It is hard to support both reordering and uncompressed block caching
        // since get cached significantly before we know the final permutation.
        // Supporting reordering for compressed block cache is not hard
        // to implement, but is not done for now.
        config->EnableBlockReordering = false;
    }

    if (options->OptimizeFor == EOptimizeFor::Scan) {
        return New<TColumnarVersionedChunkWriter>(
            std::move(config),
            std::move(options),
            std::move(schema),
            std::move(chunkWriter),
            std::move(blockCache));
    } else {
        return New<TSimpleVersionedChunkWriter>(
            std::move(config),
            std::move(options),
            std::move(schema),
            std::move(chunkWriter),
            std::move(blockCache));
    }
}

////////////////////////////////////////////////////////////////////////////////

IVersionedMultiChunkWriterPtr CreateVersionedMultiChunkWriter(
    std::function<IVersionedChunkWriterPtr(IChunkWriterPtr)> chunkWriterFactory,
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    NNative::IClientPtr client,
    TCellTag cellTag,
    TTransactionId transactionId,
    TChunkListId parentChunkListId,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
{
    using TVersionedMultiChunkWriter = TMultiChunkWriterBase<
        IVersionedMultiChunkWriter,
        IVersionedChunkWriter,
        TRange<TVersionedRow>
    >;

    auto writer = New<TVersionedMultiChunkWriter>(
        std::move(config),
        std::move(options),
        std::move(client),
        cellTag,
        transactionId,
        parentChunkListId,
        std::move(chunkWriterFactory),
        /* trafficMeter */ nullptr,
        std::move(throttler),
        std::move(blockCache));
    writer->Init();
    return writer;
}

IVersionedMultiChunkWriterPtr CreateVersionedMultiChunkWriter(
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    TTableSchemaPtr schema,
    NNative::IClientPtr client,
    TCellTag cellTag,
    TTransactionId transactionId,
    TChunkListId parentChunkListId,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
{
    auto chunkWriterFactory = [=] (IChunkWriterPtr underlyingWriter) {
        return CreateVersionedChunkWriter(
            config,
            options,
            schema,
            std::move(underlyingWriter),
            blockCache);
    };

    return CreateVersionedMultiChunkWriter(
        std::move(chunkWriterFactory),
        std::move(config),
        std::move(options),
        std::move(client),
        cellTag,
        transactionId,
        parentChunkListId,
        std::move(throttler),
        std::move(blockCache));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
