#include "input_chunk_slice.h"
#include "private.h"
#include "chunk_meta_extensions.h"

#include <yt/client/table_client/row_buffer.h>
#include <yt/client/table_client/serialize.h>

#include <yt/library/erasure/codec.h>
#include <yt/core/misc/numeric_helpers.h>
#include <yt/core/ytree/fluent.h>

#include <yt/core/misc/numeric_helpers.h>

#include <cmath>

namespace NYT::NChunkClient {

using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TLegacyInputSliceLimit::TLegacyInputSliceLimit(const TReadLimit& other)
{
    YT_VERIFY(!other.HasChunkIndex());
    YT_VERIFY(!other.HasOffset());
    if (other.HasRowIndex()) {
        RowIndex = other.GetRowIndex();
    }
    if (other.HasKey()) {
        Key = other.GetKey();
    }
}

TLegacyInputSliceLimit::TLegacyInputSliceLimit(
    const NProto::TReadLimit& other,
    const TRowBufferPtr& rowBuffer,
    TRange<TLegacyKey> keySet)
{
    YT_VERIFY(!other.has_chunk_index());
    YT_VERIFY(!other.has_offset());
    if (other.has_row_index()) {
        RowIndex = other.row_index();
    }
    if (other.has_legacy_key()) {
        NTableClient::FromProto(&Key, other.legacy_key(), rowBuffer);
    }
    if (other.has_key_index()) {
        Key = rowBuffer->Capture(keySet[other.key_index()]);
    }
}

void TLegacyInputSliceLimit::MergeLowerRowIndex(i64 rowIndex)
{
    if (!RowIndex || *RowIndex < rowIndex) {
        RowIndex = rowIndex;
    }
}

void TLegacyInputSliceLimit::MergeUpperRowIndex(i64 rowIndex)
{
    if (!RowIndex || *RowIndex > rowIndex) {
        RowIndex = rowIndex;
    }
}

void TLegacyInputSliceLimit::MergeLowerKey(NTableClient::TLegacyKey key)
{
    if (!Key || Key < key) {
        Key = key;
    }
}

void TLegacyInputSliceLimit::MergeUpperKey(NTableClient::TLegacyKey key)
{
    if (!Key || Key > key) {
        Key = key;
    }
}

void TLegacyInputSliceLimit::MergeLowerLimit(const TLegacyInputSliceLimit& limit)
{
    if (limit.RowIndex) {
        MergeLowerRowIndex(*limit.RowIndex);
    }
    if (limit.Key) {
        MergeLowerKey(limit.Key);
    }
}

void TLegacyInputSliceLimit::MergeUpperLimit(const TLegacyInputSliceLimit& limit)
{
    if (limit.RowIndex) {
        MergeUpperRowIndex(*limit.RowIndex);
    }
    if (limit.Key) {
        MergeUpperKey(limit.Key);
    }
}

void TLegacyInputSliceLimit::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, RowIndex);
    Persist(context, Key);
}

TString ToString(const TLegacyInputSliceLimit& limit)
{
    return Format("RowIndex: %v, Key: %v", limit.RowIndex, limit.Key);
}

void FormatValue(TStringBuilderBase* builder, const TLegacyInputSliceLimit& limit, TStringBuf /*format*/)
{
    builder->AppendFormat("{RowIndex: %v, Key: %v}",
        limit.RowIndex,
        limit.Key);
}

bool IsTrivial(const TLegacyInputSliceLimit& limit)
{
    return !limit.RowIndex && !limit.Key;
}

void ToProto(NProto::TReadLimit* protoLimit, const TLegacyInputSliceLimit& limit)
{
    if (limit.RowIndex) {
        protoLimit->set_row_index(*limit.RowIndex);
    } else {
        protoLimit->clear_row_index();
    }

    if (limit.Key) {
        ToProto(protoLimit->mutable_legacy_key(), limit.Key);
    } else {
        protoLimit->clear_legacy_key();
    }
}

////////////////////////////////////////////////////////////////////////////////

TInputSliceLimit::TInputSliceLimit(
    const NProto::TReadLimit& other,
    const TRowBufferPtr& rowBuffer,
    TRange<TLegacyKey> keySet,
    int keyLength,
    bool isUpper)
{
    YT_VERIFY(!other.has_chunk_index());
    YT_VERIFY(!other.has_offset());
    if (other.has_row_index()) {
        RowIndex = other.row_index();
    }
    TUnversionedRow row;

    if (other.has_key_bound_prefix()) {
        NTableClient::FromProto(&KeyBound.Prefix, other.key_bound_prefix(), rowBuffer);
        KeyBound.IsInclusive = other.key_bound_is_inclusive();
        KeyBound.IsUpper = isUpper;
    } else {
        // Build from legacy-serialized read limit.
        if (other.has_legacy_key()) {
            NTableClient::FromProto(&row, other.legacy_key(), rowBuffer);
        }
        if (other.has_key_index()) {
            row = rowBuffer->Capture(keySet[other.key_index()]);
        }
        if (row) {
            KeyBound = KeyBoundFromLegacyRow(row, isUpper, keyLength, rowBuffer);
        } else {
            KeyBound = TKeyBound::MakeUniversal(isUpper);
        }
    }
}

void TInputSliceLimit::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, RowIndex);
    Persist(context, KeyBound);
}

TString ToString(const TInputSliceLimit& limit)
{
    return Format("RowIndex: %v, KeyBound: %v", limit.RowIndex, limit.KeyBound);
}

void FormatValue(TStringBuilderBase* builder, const TInputSliceLimit& limit, TStringBuf /*format*/)
{
    builder->AppendFormat("{RowIndex: %v, KeyBound: %v}",
        limit.RowIndex,
        limit.KeyBound);
}

bool IsTrivial(const TInputSliceLimit& limit)
{
    return !limit.RowIndex && limit.KeyBound.IsUniversal();
}

void ToProto(NProto::TReadLimit* protoLimit, const TInputSliceLimit& limit)
{
    if (limit.RowIndex) {
        protoLimit->set_row_index(*limit.RowIndex);
    } else {
        protoLimit->clear_row_index();
    }

    protoLimit->set_key_bound_is_inclusive(limit.KeyBound.IsInclusive);

    if (limit.KeyBound.IsUniversal()) {
        protoLimit->clear_legacy_key();
        protoLimit->clear_key_bound_prefix();
    } else {
        ToProto(protoLimit->mutable_legacy_key(), KeyBoundToLegacyRow(limit.KeyBound));
        ToProto(protoLimit->mutable_key_bound_prefix(), limit.KeyBound.Prefix);
    }
}

////////////////////////////////////////////////////////////////////////////////

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    TLegacyKey lowerKey,
    TLegacyKey upperKey)
    : InputChunk_(inputChunk)
    , DataWeight_(inputChunk->GetDataWeight())
    , RowCount_(inputChunk->GetRowCount())
{
    if (inputChunk->LowerLimit()) {
        LowerLimit_ = TLegacyInputSliceLimit(*inputChunk->LowerLimit());
    }
    if (lowerKey) {
        LowerLimit_.MergeLowerKey(lowerKey);
    }

    if (inputChunk->UpperLimit()) {
        UpperLimit_ = TLegacyInputSliceLimit(*inputChunk->UpperLimit());
    }
    if (upperKey) {
        UpperLimit_.MergeUpperKey(upperKey);
    }
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkSlice& inputSlice,
    TLegacyKey lowerKey,
    TLegacyKey upperKey)
    : InputChunk_(inputSlice.GetInputChunk())
    , LowerLimit_(inputSlice.LowerLimit())
    , UpperLimit_(inputSlice.UpperLimit())
    , PartIndex_(inputSlice.GetPartIndex())
    , SizeOverridden_(inputSlice.GetSizeOverridden())
    , DataWeight_(inputSlice.GetDataWeight())
    , RowCount_(inputSlice.GetRowCount())
{
    if (lowerKey) {
        LowerLimit_.MergeLowerKey(lowerKey);
    }
    if (upperKey) {
        UpperLimit_.MergeUpperKey(upperKey);
    }
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkSlice& chunkSlice,
    i64 lowerRowIndex,
    i64 upperRowIndex,
    i64 dataWeight)
    : InputChunk_(chunkSlice.GetInputChunk())
    , LowerLimit_(chunkSlice.LowerLimit())
    , UpperLimit_(chunkSlice.UpperLimit())
{
    LowerLimit_.RowIndex = lowerRowIndex;
    UpperLimit_.RowIndex = upperRowIndex;
    OverrideSize(upperRowIndex - lowerRowIndex, dataWeight);
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    int partIndex,
    i64 lowerRowIndex,
    i64 upperRowIndex,
    i64 dataWeight)
    : InputChunk_(inputChunk)
    , PartIndex_(partIndex)
{
    if (inputChunk->LowerLimit()) {
        LowerLimit_ = TLegacyInputSliceLimit(*inputChunk->LowerLimit());
    }
    LowerLimit_.MergeLowerRowIndex(lowerRowIndex);

    if (inputChunk->UpperLimit()) {
        UpperLimit_ = TLegacyInputSliceLimit(*inputChunk->UpperLimit());
    }
    UpperLimit_.MergeUpperRowIndex(upperRowIndex);

    OverrideSize(*UpperLimit_.RowIndex - *LowerLimit_.RowIndex, std::max<i64>(1, dataWeight * inputChunk->GetColumnSelectivityFactor()));
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    const TRowBufferPtr& rowBuffer,
    const NProto::TChunkSlice& protoChunkSlice,
    TRange<TLegacyKey> keySet)
    : TInputChunkSlice(inputChunk)
{
    LowerLimit_.MergeLowerLimit(TLegacyInputSliceLimit(protoChunkSlice.lower_limit(), rowBuffer, keySet));
    UpperLimit_.MergeUpperLimit(TLegacyInputSliceLimit(protoChunkSlice.upper_limit(), rowBuffer, keySet));
    PartIndex_ = DefaultPartIndex;

    if (protoChunkSlice.has_row_count_override() || protoChunkSlice.has_data_weight_override()) {
        YT_VERIFY((protoChunkSlice.has_row_count_override() && protoChunkSlice.has_data_weight_override()));
        OverrideSize(protoChunkSlice.row_count_override(), std::max<i64>(1, protoChunkSlice.data_weight_override() * inputChunk->GetColumnSelectivityFactor()));
    }
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    const TRowBufferPtr& rowBuffer,
    const NProto::TChunkSpec& protoChunkSpec)
    : TInputChunkSlice(inputChunk)
{
    static TRange<TLegacyKey> DummyKeys;
    LowerLimit_.MergeLowerLimit(TLegacyInputSliceLimit(protoChunkSpec.lower_limit(), rowBuffer, DummyKeys));
    UpperLimit_.MergeUpperLimit(TLegacyInputSliceLimit(protoChunkSpec.upper_limit(), rowBuffer, DummyKeys));
    PartIndex_ = DefaultPartIndex;

    if (protoChunkSpec.has_row_count_override() || protoChunkSpec.has_data_weight_override()) {
        YT_VERIFY((protoChunkSpec.has_row_count_override() && protoChunkSpec.has_data_weight_override()));
        OverrideSize(protoChunkSpec.row_count_override(), std::max<i64>(1, protoChunkSpec.data_weight_override() * inputChunk->GetColumnSelectivityFactor()));
    }
}

std::vector<TInputChunkSlicePtr> TInputChunkSlice::SliceEvenly(i64 sliceDataWeight, i64 sliceRowCount, TRowBufferPtr rowBuffer) const
{
    YT_VERIFY(sliceDataWeight > 0);
    YT_VERIFY(sliceRowCount > 0);

    i64 lowerRowIndex = LowerLimit_.RowIndex.value_or(0);
    i64 upperRowIndex = UpperLimit_.RowIndex.value_or(InputChunk_->GetRowCount());

    i64 rowCount = upperRowIndex - lowerRowIndex;

    i64 count = std::max(DivCeil(GetDataWeight(), sliceDataWeight), DivCeil(rowCount, sliceRowCount));
    count = std::max(std::min(count, rowCount), static_cast<i64>(1));

    std::vector<TInputChunkSlicePtr> result;
    for (i64 i = 0; i < count; ++i) {
        i64 sliceLowerRowIndex = lowerRowIndex + rowCount * i / count;
        i64 sliceUpperRowIndex = lowerRowIndex + rowCount * (i + 1) / count;
        if (sliceLowerRowIndex < sliceUpperRowIndex) {
            result.push_back(New<TInputChunkSlice>(
                *this,
                sliceLowerRowIndex,
                sliceUpperRowIndex,
                DivCeil(GetDataWeight(), count)));
        }
    }
    if (rowBuffer) {
        result.front()->LowerLimit().Key = rowBuffer->Capture(LowerLimit_.Key);
        result.back()->UpperLimit().Key = rowBuffer->Capture(UpperLimit_.Key);
    }
    return result;
}

std::pair<TInputChunkSlicePtr, TInputChunkSlicePtr> TInputChunkSlice::SplitByRowIndex(i64 splitRow) const
{
    i64 lowerRowIndex = LowerLimit_.RowIndex.value_or(0);
    i64 upperRowIndex = UpperLimit_.RowIndex.value_or(InputChunk_->GetRowCount());

    i64 rowCount = upperRowIndex - lowerRowIndex;

    YT_VERIFY(splitRow > 0 && splitRow < rowCount);

    return std::make_pair(
        New<TInputChunkSlice>(
            *this,
            lowerRowIndex,
            lowerRowIndex + splitRow,
            GetDataWeight() / rowCount * splitRow),
        New<TInputChunkSlice>(
            *this,
            lowerRowIndex + splitRow,
            upperRowIndex,
            GetDataWeight() / rowCount * (rowCount - splitRow)));
}

i64 TInputChunkSlice::GetLocality(int replicaPartIndex) const
{
    i64 result = GetDataWeight();

    if (PartIndex_ == DefaultPartIndex) {
        // For erasure chunks without specified part index,
        // data size is assumed to be split evenly between data parts.
        auto codecId = InputChunk_->GetErasureCodec();
        if (codecId != NErasure::ECodec::None) {
            auto* codec = NErasure::GetCodec(codecId);
            int dataPartCount = codec->GetDataPartCount();
            result = (result + dataPartCount - 1) / dataPartCount;
        }
    } else if (PartIndex_ != replicaPartIndex) {
        result = 0;
    }

    return result;
}

int TInputChunkSlice::GetPartIndex() const
{
    return PartIndex_;
}

i64 TInputChunkSlice::GetMaxBlockSize() const
{
    return InputChunk_->GetMaxBlockSize();
}

bool TInputChunkSlice::GetSizeOverridden() const
{
    return SizeOverridden_;
}

i64 TInputChunkSlice::GetDataWeight() const
{
    return SizeOverridden_ ? DataWeight_ : InputChunk_->GetDataWeight();
}

i64 TInputChunkSlice::GetRowCount() const
{
    return SizeOverridden_ ? RowCount_ : InputChunk_->GetRowCount();
}

void TInputChunkSlice::OverrideSize(i64 rowCount, i64 dataWeight)
{
    RowCount_ = rowCount;
    DataWeight_ = dataWeight;
    SizeOverridden_ = true;
}

void TInputChunkSlice::ApplySamplingSelectivityFactor(double samplingSelectivityFactor)
{
    i64 rowCount = GetRowCount() * samplingSelectivityFactor;
    i64 dataWeight = GetDataWeight() * samplingSelectivityFactor;
    OverrideSize(rowCount, dataWeight);
}

void TInputChunkSlice::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, InputChunk_);
    Persist(context, LowerLimit_);
    Persist(context, UpperLimit_);
    Persist(context, PartIndex_);
    Persist(context, SizeOverridden_);
    Persist(context, RowCount_);
    Persist(context, DataWeight_);
}

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TInputChunkSlicePtr& slice)
{
    return Format("ChunkId: %v, LowerLimit: %v, UpperLimit: %v, RowCount: %v, DataWeight: %v, PartIndex: %v",
        slice->GetInputChunk()->ChunkId(),
        slice->LowerLimit(),
        slice->UpperLimit(),
        slice->GetRowCount(),
        slice->GetDataWeight(),
        slice->GetPartIndex());
}

////////////////////////////////////////////////////////////////////////////////

TInputChunkSlicePtr CreateInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    TLegacyKey lowerKey,
    TLegacyKey upperKey)
{
    return New<TInputChunkSlice>(inputChunk, lowerKey, upperKey);
}

TInputChunkSlicePtr CreateInputChunkSlice(
    const TInputChunkSlice& inputSlice,
    TLegacyKey lowerKey,
    TLegacyKey upperKey)
{
    return New<TInputChunkSlice>(inputSlice, lowerKey, upperKey);
}

TInputChunkSlicePtr CreateInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    const NTableClient::TRowBufferPtr& rowBuffer,
    const NProto::TChunkSpec& protoChunkSpec)
{
    return New<TInputChunkSlice>(inputChunk, rowBuffer, protoChunkSpec);
}

std::vector<TInputChunkSlicePtr> CreateErasureInputChunkSlices(
    const TInputChunkPtr& inputChunk,
    NErasure::ECodec codecId)
{
    std::vector<TInputChunkSlicePtr> slices;

    i64 dataSize = inputChunk->GetUncompressedDataSize();
    i64 rowCount = inputChunk->GetRowCount();

    auto* codec = NErasure::GetCodec(codecId);
    int dataPartCount = codec->GetDataPartCount();

    for (int partIndex = 0; partIndex < dataPartCount; ++partIndex) {
        i64 sliceLowerRowIndex = rowCount * partIndex / dataPartCount;
        i64 sliceUpperRowIndex = rowCount * (partIndex + 1) / dataPartCount;
        if (sliceLowerRowIndex < sliceUpperRowIndex) {
            auto chunkSlice = New<TInputChunkSlice>(
                inputChunk,
                partIndex,
                sliceLowerRowIndex,
                sliceUpperRowIndex,
                (dataSize + dataPartCount - 1) / dataPartCount);
            slices.emplace_back(std::move(chunkSlice));
        }
    }

    return slices;
}

void InferLimitsFromBoundaryKeys(const TInputChunkSlicePtr& chunkSlice, const TRowBufferPtr& rowBuffer, int keyColumnCount)
{
    if (const auto& boundaryKeys = chunkSlice->GetInputChunk()->BoundaryKeys()) {
        chunkSlice->LowerLimit().MergeLowerKey(GetStrictKey(boundaryKeys->MinKey, keyColumnCount, rowBuffer));
        chunkSlice->UpperLimit().MergeUpperKey(GetStrictKeySuccessor(boundaryKeys->MaxKey, keyColumnCount, rowBuffer));
    }
}

std::vector<TInputChunkSlicePtr> SliceChunkByRowIndexes(
    const TInputChunkPtr& inputChunk,
    i64 sliceDataWeight,
    i64 sliceRowCount)
{
    return CreateInputChunkSlice(inputChunk)->SliceEvenly(sliceDataWeight, sliceRowCount);
}

void ToProto(NProto::TChunkSpec* chunkSpec, const TInputChunkSlicePtr& inputSlice, EDataSourceType dataSourceType)
{
    // The chunk spec in the slice has arrived from master, so it can't possibly contain any extensions
    // except misc and boundary keys (in sorted merge or reduce). Jobs request boundary keys
    // from the nodes when needed, so we remove it here, to optimize traffic from the scheduler and
    // proto serialization time.

    ToProto(chunkSpec, inputSlice->GetInputChunk(), dataSourceType);

    if (!IsTrivial(inputSlice->LowerLimit())) {
        // NB(psushin): if lower limit key is less than min chunk key, we can eliminate it from job spec.
        // Moreover, it is important for GetJobInputPaths handle to work properly.
        bool pruneKeyLimit = dataSourceType == EDataSourceType::UnversionedTable
            && inputSlice->LowerLimit().Key
            && inputSlice->GetInputChunk()->BoundaryKeys()
            && inputSlice->LowerLimit().Key <= inputSlice->GetInputChunk()->BoundaryKeys()->MinKey;

        if (pruneKeyLimit && inputSlice->LowerLimit().RowIndex) {
            TLegacyInputSliceLimit inputSliceLimit;
            inputSliceLimit.RowIndex = inputSlice->LowerLimit().RowIndex;
            ToProto(chunkSpec->mutable_lower_limit(), inputSliceLimit);
        } else if (!pruneKeyLimit) {
            ToProto(chunkSpec->mutable_lower_limit(), inputSlice->LowerLimit());
        }
    }

    if (!IsTrivial(inputSlice->UpperLimit())) {
        // NB(psushin): if upper limit key is greater than max chunk key, we can eliminate it from job spec.
        // Moreover, it is important for GetJobInputPaths handle to work properly.
        bool pruneKeyLimit = dataSourceType == EDataSourceType::UnversionedTable
            && inputSlice->UpperLimit().Key
            && inputSlice->GetInputChunk()->BoundaryKeys()
            && inputSlice->UpperLimit().Key > inputSlice->GetInputChunk()->BoundaryKeys()->MaxKey;

        if (pruneKeyLimit && inputSlice->UpperLimit().RowIndex) {
            TLegacyInputSliceLimit inputSliceLimit;
            inputSliceLimit.RowIndex = inputSlice->UpperLimit().RowIndex;
            ToProto(chunkSpec->mutable_upper_limit(), inputSliceLimit);
        } else if (!pruneKeyLimit) {
            ToProto(chunkSpec->mutable_upper_limit(), inputSlice->UpperLimit());
        }
    }

    chunkSpec->set_data_weight_override(inputSlice->GetDataWeight());

    // NB(psushin): always setting row_count_override is important for GetJobInputPaths handle to work properly.
    chunkSpec->set_row_count_override(inputSlice->GetRowCount());
    if (inputSlice->GetInputChunk()->IsDynamicStore()) {
        ToProto(chunkSpec->mutable_tablet_id(), inputSlice->GetInputChunk()->TabletId());
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
