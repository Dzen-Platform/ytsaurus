#include "stdafx.h"
#include "versioned_chunk_writer.h"

#include "chunk_meta_extensions.h"
#include "config.h"
#include "versioned_block_writer.h"
#include "versioned_writer.h"
#include "unversioned_row.h"

#include <ytlib/chunk_client/chunk_writer.h>
#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/encoding_chunk_writer.h>
#include <ytlib/chunk_client/encoding_writer.h>
#include <ytlib/chunk_client/multi_chunk_writer_base.h>

namespace NYT {
namespace NVersionedTableClient {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NRpc;
using namespace NTransactionClient;
using namespace NVersionedTableClient::NProto;

////////////////////////////////////////////////////////////////////////////////

class TVersionedChunkWriter
    : public IVersionedChunkWriter
{
public:
    TVersionedChunkWriter(
        TChunkWriterConfigPtr config,
        TChunkWriterOptionsPtr options,
        const TTableSchema& schema,
        const TKeyColumns& keyColumns,
        IChunkWriterPtr chunkWriter,
        IBlockCachePtr blockCache);

    virtual TFuture<void> Open() override;

    virtual bool Write(const std::vector<TVersionedRow>& rows) override;

    virtual TFuture<void> Close() override;

    virtual TFuture<void> GetReadyEvent() override;

    virtual i64 GetMetaSize() const override;
    virtual i64 GetDataSize() const override;

    virtual TChunkMeta GetMasterMeta() const override;
    virtual TChunkMeta GetSchedulerMeta() const override;

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;

private:
    TChunkWriterConfigPtr Config_;
    TTableSchema Schema_;
    TKeyColumns KeyColumns_;

    TEncodingChunkWriterPtr EncodingChunkWriter_;

    TOwningKey LastKey_;
    std::unique_ptr<TSimpleVersionedBlockWriter> BlockWriter_;

    TBlockMetaExt BlockMetaExt_;
    i64 BlockMetaExtSize_ = 0;

    TSamplesExt SamplesExt_;
    i64 SamplesExtSize_ = 0;
    double AverageSampleSize_ = 0.0;

    i64 DataWeight_ = 0;

    TBoundaryKeysExt BoundaryKeysExt_;

    i64 RowCount_ = 0;

    TTimestamp MinTimestamp_;
    TTimestamp MaxTimestamp_;

#if 0
    TBloomFilterBuilder KeyFilter_;
#endif

    void WriteRow(
        TVersionedRow row,
        const TUnversionedValue* beginPreviousKey,
        const TUnversionedValue* endPreviousKey);

    void EmitSample(TVersionedRow row);

    void FinishBlockIfLarge(TVersionedRow row);
    void FinishBlock(const TUnversionedValue* beginKey, const TUnversionedValue* endKey);

    void DoClose();
    void FillCommonMeta(TChunkMeta* meta) const;

    i64 GetUncompressedSize() const;

};

////////////////////////////////////////////////////////////////////////////////

TVersionedChunkWriter::TVersionedChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns,
    IChunkWriterPtr chunkWriter,
    IBlockCachePtr blockCache)
    : Config_(config)
    , Schema_(schema)
    , KeyColumns_(keyColumns)
    , EncodingChunkWriter_(New<TEncodingChunkWriter>(
        config,
        options,
        chunkWriter,
        blockCache))
    , LastKey_(static_cast<TUnversionedValue*>(nullptr), static_cast<TUnversionedValue*>(nullptr))
    , BlockWriter_(new TSimpleVersionedBlockWriter(Schema_, KeyColumns_))
    , MinTimestamp_(MaxTimestamp)
    , MaxTimestamp_(MinTimestamp)
#if 0
    , KeyFilter_(Config_->MaxKeyFilterSize, Config_->KeyFilterFalsePositiveRate)
#endif
{ }

TFuture<void> TVersionedChunkWriter::Open()
{
    try {
        ValidateTableSchemaAndKeyColumns(Schema_, KeyColumns_);
    } catch (const std::exception& ex) {
        return MakeFuture<void>(ex);
    }
    return VoidFuture;
}

bool TVersionedChunkWriter::Write(const std::vector<TVersionedRow>& rows)
{
    YCHECK(rows.size() > 0);

    if (RowCount_ == 0) {
        ToProto(
            BoundaryKeysExt_.mutable_min(),
            TOwningKey(rows.front().BeginKeys(), rows.front().EndKeys()));
        EmitSample(rows.front());
    }

    //FIXME: insert key into bloom filter.
    //KeyFilter_.Insert(GetFarmFingerprint(rows.front().BeginKeys(), rows.front().EndKeys()));
    WriteRow(rows.front(), LastKey_.Begin(), LastKey_.End());
    FinishBlockIfLarge(rows.front());

    for (int i = 1; i < rows.size(); ++i) {
        //KeyFilter_.Insert(GetFarmFingerprint(rows[i].BeginKeys(), rows[i].EndKeys()));
        WriteRow(rows[i], rows[i - 1].BeginKeys(), rows[i - 1].EndKeys());
        FinishBlockIfLarge(rows[i]);
    }

    LastKey_ = TOwningKey(rows.back().BeginKeys(), rows.back().EndKeys());
    return EncodingChunkWriter_->IsReady();
}

TFuture<void> TVersionedChunkWriter::Close()
{
    if (RowCount_ == 0) {
        // Empty chunk.
        return VoidFuture;
    }

    return BIND(&TVersionedChunkWriter::DoClose, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();
}

TFuture<void> TVersionedChunkWriter::GetReadyEvent()
{
    return EncodingChunkWriter_->GetReadyEvent();
}

i64 TVersionedChunkWriter::GetMetaSize() const
{
    // Other meta parts are negligible.
    return BlockMetaExtSize_ + SamplesExtSize_;
}

i64 TVersionedChunkWriter::GetDataSize() const
{
    return EncodingChunkWriter_->GetDataStatistics().compressed_data_size() +
        (BlockWriter_ ? BlockWriter_->GetBlockSize() : 0);
}

TChunkMeta TVersionedChunkWriter::GetMasterMeta() const
{
    TChunkMeta meta;
    FillCommonMeta(&meta);
    SetProtoExtension(meta.mutable_extensions(), EncodingChunkWriter_->MiscExt());
    return meta;
}

TChunkMeta TVersionedChunkWriter::GetSchedulerMeta() const
{
    return GetMasterMeta();
}

TDataStatistics TVersionedChunkWriter::GetDataStatistics() const
{
    return EncodingChunkWriter_->GetDataStatistics();
}

void TVersionedChunkWriter::WriteRow(
    TVersionedRow row,
    const TUnversionedValue* beginPreviousKey,
    const TUnversionedValue* endPreviousKey)
{
    double avgRowSize = EncodingChunkWriter_->GetCompressionRatio() * GetUncompressedSize() / RowCount_;
    double sampleProbability = Config_->SampleRate * avgRowSize / AverageSampleSize_;

    if (RandomNumber<double>() < sampleProbability) {
        EmitSample(row);
    }

    ++RowCount_;
    DataWeight_ += GetDataWeight(row);
    BlockWriter_->WriteRow(row, beginPreviousKey, endPreviousKey);
}

void TVersionedChunkWriter::EmitSample(TVersionedRow row)
{
    auto entry = SerializeToString(row.BeginKeys(), row.EndKeys());
    SamplesExt_.add_entries(entry);
    SamplesExtSize_ += entry.length();
    AverageSampleSize_ = static_cast<double>(SamplesExtSize_) / SamplesExt_.entries_size();
}

void TVersionedChunkWriter::FinishBlockIfLarge(TVersionedRow row)
{
    if (BlockWriter_->GetBlockSize() < Config_->BlockSize) {
        return;
    }

    FinishBlock(row.BeginKeys(), row.EndKeys());
    BlockWriter_.reset(new TSimpleVersionedBlockWriter(Schema_, KeyColumns_));
}

void TVersionedChunkWriter::FinishBlock(const TUnversionedValue* beginKey, const TUnversionedValue* endKey)
{
    auto block = BlockWriter_->FlushBlock();
    block.Meta.set_chunk_row_count(RowCount_);
    block.Meta.set_block_index(BlockMetaExt_.blocks_size());
    ToProto(block.Meta.mutable_last_key(), beginKey, endKey);

    BlockMetaExtSize_ += block.Meta.ByteSize();

    BlockMetaExt_.add_blocks()->Swap(&block.Meta);
    EncodingChunkWriter_->WriteBlock(std::move(block.Data));

    MaxTimestamp_ = std::max(MaxTimestamp_, BlockWriter_->GetMaxTimestamp());
    MinTimestamp_ = std::min(MinTimestamp_, BlockWriter_->GetMinTimestamp());
}

void TVersionedChunkWriter::DoClose()
{
    using NYT::ToProto;

    if (BlockWriter_->GetRowCount() > 0) {
        FinishBlock(LastKey_.Begin(), LastKey_.End());
    }

    ToProto(BoundaryKeysExt_.mutable_max(), LastKey_);

    auto& meta = EncodingChunkWriter_->Meta();
    FillCommonMeta(&meta);

    SetProtoExtension(meta.mutable_extensions(), ToProto<TTableSchemaExt>(Schema_));

    TKeyColumnsExt keyColumnsExt;
    for (const auto& name : KeyColumns_) {
        keyColumnsExt.add_names(name);
    }
    SetProtoExtension(meta.mutable_extensions(), keyColumnsExt);

    SetProtoExtension(meta.mutable_extensions(), BlockMetaExt_);
    SetProtoExtension(meta.mutable_extensions(), SamplesExt_);

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
    miscExt.set_min_timestamp(MinTimestamp_);
    miscExt.set_max_timestamp(MaxTimestamp_);

    EncodingChunkWriter_->Close();
}

void TVersionedChunkWriter::FillCommonMeta(TChunkMeta* meta) const
{
    meta->set_type(static_cast<int>(EChunkType::Table));
    meta->set_version(static_cast<int>(TSimpleVersionedBlockWriter::FormatVersion));

    SetProtoExtension(meta->mutable_extensions(), BoundaryKeysExt_);
}

i64 TVersionedChunkWriter::GetUncompressedSize() const 
{
    i64 size = EncodingChunkWriter_->GetDataStatistics().uncompressed_data_size();
    if (BlockWriter_) {
        size += BlockWriter_->GetBlockSize();
    }
    return size;
}

////////////////////////////////////////////////////////////////////////////////

IVersionedChunkWriterPtr CreateVersionedChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns,
    IChunkWriterPtr chunkWriter,
    IBlockCachePtr blockCache)
{
    return New<TVersionedChunkWriter>(
        config,
        options,
        schema,
        keyColumns,
        chunkWriter,
        blockCache);
}

////////////////////////////////////////////////////////////////////////////////

IVersionedMultiChunkWriterPtr CreateVersionedMultiChunkWriter(
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns,
    NRpc::IChannelPtr masterChannel,
    const NTransactionClient::TTransactionId& transactionId,
    const TChunkListId& parentChunkListId,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
{
    typedef TMultiChunkWriterBase<
        IVersionedMultiChunkWriter,
        IVersionedChunkWriter,
        const std::vector<TVersionedRow>&> TVersionedMultiChunkWriter;

    auto createChunkWriter = [=] (IChunkWriterPtr underlyingWriter) {
        return CreateVersionedChunkWriter(
            config,
            options,
            schema,
            keyColumns,
            underlyingWriter,
            blockCache);
    };

    return New<TVersionedMultiChunkWriter>(
        config,
        options,
        masterChannel,
        transactionId,
        parentChunkListId,
        createChunkWriter,
        throttler,
        blockCache);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
