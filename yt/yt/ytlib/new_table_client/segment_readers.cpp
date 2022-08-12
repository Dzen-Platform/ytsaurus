#include "segment_readers.h"

#define UNROLL_LOOPS

namespace NYT::NNewTableClient {

struct TWriteIdsTag { };
struct TRowToValueTag { };
struct TRowIndexTag { };

constexpr int UnpackSizeFactor = 2;

////////////////////////////////////////////////////////////////////////////////

void TTmpBuffers::Swap(TTmpBuffers& other)
{
    Values.swap(other.Values);
    Ids.swap(other.Ids);
    Offsets.swap(other.Offsets);
    DataSpans.swap(other.DataSpans);
}

TInitContext::TInitContext(ui32 rowOffset, TMutableRange<TReadSpan> spans, TTmpBuffers* tmpBuffers)
    : RowOffset(rowOffset)
    , Spans(spans)
    , TmpBuffersSource(tmpBuffers)
{
    TmpBuffersSource->Swap(*this);
}

TInitContext::~TInitContext()
{
    TmpBuffersSource->Swap(*this);

    // Those containers must be swapped with TTmpBuffers reduce memory reallocations.
    YT_VERIFY(Values.empty());
    YT_VERIFY(Ids.empty());
    YT_VERIFY(Offsets.empty());
    YT_VERIFY(DataSpans.empty());
}

////////////////////////////////////////////////////////////////////////////////

void CheckBatchSize(TRange<TReadSpan> spans, ui32 expectedBatchSize)
{
    Y_UNUSED(spans);
    Y_UNUSED(expectedBatchSize);
#ifndef NDEBUG
    ui32 batchSize = 0;
    for (auto [lower, upper] : spans) {
        batchSize += upper - lower;
    }

    YT_VERIFY(expectedBatchSize == batchSize);
#endif
}

////////////////////////////////////////////////////////////////////////////////

void DiffsToOffsets(TMutableRange<ui32> values, ui32 expectedPerItem, ui32 startOffset = 0)
{
    auto pivot = startOffset;
    for (auto& value : values) {
        pivot += expectedPerItem;
        value = pivot + ZigZagDecode32(value);
    }
}

#ifdef FULL_UNPACK
void TScanTimestampExtractor::InitSegment(const TTimestampMeta* meta, const char* data, TTmpBuffers* tmpBuffers)
{
    RowOffset_ = meta->ChunkRowCount - meta->RowCount;
    SegmentRowLimit_ = meta->ChunkRowCount;

    auto& timestampsDict = tmpBuffers->Values;
    auto& ids = tmpBuffers->Ids;

    TCompressedVectorView view(reinterpret_cast<const ui64*>(data));
    UnpackBitVector(view, &timestampsDict);

    auto writeTimestampIdsView = ++view;
    auto deleteTimestampIdsView = ++view;
    auto writeTimestampOffsetsView = ++view;
    auto deleteTimestampOffsetsView = ++view;

    YT_VERIFY(writeTimestampOffsetsView.GetSize() == meta->RowCount);
    YT_VERIFY(deleteTimestampOffsetsView.GetSize() == meta->RowCount);

    {
        auto* writeTimestampOffsets = WriteTimestampOffsets_.Resize(writeTimestampOffsetsView.GetSize() + 1);

        writeTimestampOffsets[0] = 0;
        writeTimestampOffsetsView.UnpackTo(writeTimestampOffsets + 1);

        auto expectedCount = meta->ExpectedWritesPerRow;
        DiffsToOffsets(MakeMutableRange(writeTimestampOffsets + 1, meta->RowCount), expectedCount);

#ifndef NDEBUG
        for (size_t index = 0; index < writeTimestampOffsetsView.GetSize(); ++index) {
            auto expected = GetOffset(writeTimestampOffsetsView, expectedCount, index);
            YT_VERIFY(writeTimestampOffsets[index] == expected);
        }
#endif
    }

    WriteTimestamps_.Resize(writeTimestampIdsView.GetSize());
    {
        UnpackBitVector(writeTimestampIdsView, &ids);
        for (size_t index = 0; index < ids.size(); ++index) {
            WriteTimestamps_[index] = meta->BaseTimestamp + timestampsDict[ids[index]];
        }
    }

    {
        auto* deleteTimestampOffsets = DeleteTimestampOffsets_.Resize(deleteTimestampOffsetsView.GetSize() + 1);

        deleteTimestampOffsets[0] = 0;
        deleteTimestampOffsetsView.UnpackTo(deleteTimestampOffsets + 1);

        auto expectedCount = meta->ExpectedDeletesPerRow;
        DiffsToOffsets(MakeMutableRange(deleteTimestampOffsets + 1, meta->RowCount), expectedCount);

#ifndef NDEBUG
        for (size_t index = 0; index < deleteTimestampOffsetsView.GetSize(); ++index) {
            auto expected = GetOffset(deleteTimestampOffsetsView, expectedCount, index);
            YT_VERIFY(deleteTimestampOffsets[index] == expected);
        }
#endif
    }

    DeleteTimestamps_.Resize(deleteTimestampIdsView.GetSize());
    {
        UnpackBitVector(deleteTimestampIdsView, &ids);
        for (size_t index = 0; index < ids.size(); ++index) {
            DeleteTimestamps_[index] = meta->BaseTimestamp + timestampsDict[ids[index]];
        }
    }
}
#endif

class TSpansSlice
{
public:
    TSpansSlice(const TSpansSlice&) = delete;
    TSpansSlice(TSpansSlice&&) = delete;

    TSpansSlice(
        TReadSpan* const spanItStart,
        TReadSpan* const spanItEnd,
        const ui32 batchSize,
        const ui32 savedUpperBound)
        : SpanItStart_(spanItStart)
        , SpanItEnd_(spanItEnd)
        , BatchSize_(batchSize)
        , SavedUpperBound_(savedUpperBound)
    { }

    ~TSpansSlice()
    {
        if (SavedUpperBound_ > 0) {
            SpanItEnd_->Upper = SavedUpperBound_;
        }
    }

    ui32 GetBatchSize() const
    {
        return BatchSize_;
    }

    TRange<TReadSpan> GetSpans() const
    {
        return {SpanItStart_, SpanItEnd_ + (SavedUpperBound_ > 0)};
    }

    ui32 GetSize() const
    {
        return SpanItEnd_ - SpanItStart_ + (SavedUpperBound_ > 0);
    }

private:
    TReadSpan* const SpanItStart_;
    TReadSpan* const SpanItEnd_;
    const ui32 BatchSize_;
    const ui32 SavedUpperBound_;
};

TSpansSlice GetBatchSlice(TMutableRange<TReadSpan> spans, ui32 rowLimit)
{
    auto spanIt = spans.begin();
    ui32 batchSize = 0;
    ui32 savedUpperBound = 0;
    while (spanIt != spans.end()) {
        auto [lower, upper] = *spanIt;

        if (upper <= rowLimit) {
            batchSize += upper - lower;
            ++spanIt;
            continue;
        } else if (lower < rowLimit) {
            batchSize += rowLimit - lower;
            savedUpperBound = spanIt->Upper;
            spanIt->Upper = rowLimit;
        }
        break;
    }

    return TSpansSlice(spans.begin(), spanIt, batchSize, savedUpperBound);
}

template <class T, class TDict>
void DoInitDictValues(
    T* output,
    T baseValue,
    TDict&& dict,
    TCompressedVectorView ids,
    TRange<TReadSpan> offsetSpans)
{
    for (auto [lower, upper] : offsetSpans) {
        ids.UnpackTo(output, lower, upper);
        auto outputEnd = output + upper - lower;
        while (output != outputEnd) {
            *output = baseValue + dict[*output];
            ++output;
        }
    }
}

template <class T>
void InitDictValues(
    TMutableRange<T> output,
    T baseValue,
    TCompressedVectorView dictView,
    std::vector<T>* dict,
    TCompressedVectorView ids,
    TRange<TReadSpan> offsetSpans,
    size_t /*segmentSize*/)
{
    auto batchSize = output.size();

    if (dict->empty() && batchSize * UnpackSizeFactor > dictView.GetSize()) {
        UnpackBitVector(dictView, dict);
    }

    if (!dict->empty()) {
        DoInitDictValues(output.Begin(), baseValue, *dict, ids, offsetSpans);
    } else {
        DoInitDictValues(output.Begin(), baseValue, dictView, ids, offsetSpans);
    }
}

ui32 DoInitTimestampOffsets(
    ui32 segmentRowOffset,
    ui32 expectedPerRow,
    TCompressedVectorView perRowDiffsView,
    ui32* output,
    TReadSpan* offsetsSpans,
    TRange<TReadSpan> spans)
{
    // First offset is zero.
    *output++ = 0;

    ui32 offset = 0;
    for (auto [lower, upper] : spans) {
        auto segmentLower = lower - segmentRowOffset;
        auto segmentUpper = upper - segmentRowOffset;

        auto startSegmentOffset = GetOffset(perRowDiffsView, expectedPerRow, segmentLower);

        perRowDiffsView.UnpackTo(output, segmentLower, segmentUpper);

        auto count = segmentUpper - segmentLower;
        DiffsToOffsets(
            MakeMutableRange(output, count),
            expectedPerRow,
            offset + expectedPerRow * segmentLower - startSegmentOffset);

#ifndef NDEBUG
        for (ui32 index = 0; index < count; ++index) {
            auto expected = GetOffset(perRowDiffsView, expectedPerRow, segmentLower + index + 1) -
                startSegmentOffset + offset;
            YT_VERIFY(output[index] == expected);
        }
#endif

        output += count;

        auto nextOffset = output[-1];

        auto endSegmentOffset = startSegmentOffset + nextOffset - offset;
        auto endSegmentOffsetExpected = GetOffset(perRowDiffsView, expectedPerRow, segmentUpper);
        YT_VERIFY(endSegmentOffset == endSegmentOffsetExpected);

        *offsetsSpans++ = {startSegmentOffset, endSegmentOffset};

        offset = nextOffset;
    }

    return offset;
}

void TScanTimestampExtractor::InitSegment(const TTimestampMeta* meta, const char* data, TInitContext* initContext)
{
    RowOffset_ = initContext->RowOffset;

    auto& timestampsDict = initContext->Values;
    timestampsDict.clear();

    TCompressedVectorView view(reinterpret_cast<const ui64*>(data));

    auto timestampsDictView = view;

    auto writeTimestampIdsView = ++view;
    auto deleteTimestampIdsView = ++view;

    auto writeTimestampPerRowDiffsView = ++view;
    auto deleteTimestampPerRowDiffsView = ++view;

    YT_VERIFY(writeTimestampPerRowDiffsView.GetSize() == meta->RowCount);
    YT_VERIFY(writeTimestampPerRowDiffsView.GetSize() == deleteTimestampPerRowDiffsView.GetSize());

    auto slice = GetBatchSlice(initContext->Spans, meta->ChunkRowCount);
    auto batchSize = slice.GetBatchSize();
    // Segment can be initialized multiple times if block bound (of other columns) crosses segment.
    YT_VERIFY(!slice.GetSpans().empty());
    SegmentRowLimit_ = slice.GetSpans().end()[-1].Upper;

    initContext->DataSpans.resize(slice.GetSize());

    WriteTimestampOffsets_.Resize(batchSize + 1);

    // Unpack offsets according to spans.
    // Build offset spans from initial spans to unpack data pointed by offsets.
    auto writeTimestampCount = DoInitTimestampOffsets(
        meta->ChunkRowCount - meta->RowCount,
        meta->ExpectedWritesPerRow,
        writeTimestampPerRowDiffsView,
        WriteTimestampOffsets_.GetData(),
        initContext->DataSpans.data(),
        slice.GetSpans());

    WriteTimestamps_.Resize(writeTimestampCount);
    auto writeTimestamps = MakeMutableRange(WriteTimestamps_.GetData(), writeTimestampCount);

    InitDictValues(
        writeTimestamps,
        meta->BaseTimestamp,
        timestampsDictView,
        &timestampsDict,
        writeTimestampIdsView,
        initContext->DataSpans,
        meta->RowCount);

    DeleteTimestampOffsets_.Resize(batchSize + 1);

    auto deleteTimestampCount = DoInitTimestampOffsets(
        meta->ChunkRowCount - meta->RowCount,
        meta->ExpectedDeletesPerRow,
        deleteTimestampPerRowDiffsView,
        DeleteTimestampOffsets_.GetData(),
        initContext->DataSpans.data(),
        slice.GetSpans());

    DeleteTimestamps_.Resize(deleteTimestampCount);
    auto deleteTimestamps = MakeMutableRange(DeleteTimestamps_.GetData(), deleteTimestampCount);

    InitDictValues(
        deleteTimestamps,
        meta->BaseTimestamp,
        timestampsDictView,
        &timestampsDict,
        deleteTimestampIdsView,
        initContext->DataSpans,
        meta->RowCount);
}

template <class T>
const ui64* TScanIntegerExtractor<T>::GetEndPtr(const TMetaBase* meta, const ui64* ptr)
{
    if (IsDirect(meta->Type)) {
        TCompressedVectorView valuesView(ptr);
        ptr += valuesView.GetSizeInWords();
        ptr += GetBitmapSize(valuesView.GetSize());
    } else {
        TCompressedVectorView valuesView(ptr);
        ptr += valuesView.GetSizeInWords();
        TCompressedVectorView idsView(ptr);
        ptr += idsView.GetSizeInWords();
    }

    return ptr;
}

#ifdef FULL_UNPACK
template <class T>
const ui64* TScanIntegerExtractor<T>::InitData(const TMetaBase* meta, const ui64* ptr, TTmpBuffers* tmpBuffers)
{
    auto& values = tmpBuffers->Values;
    auto& ids = tmpBuffers->Ids;

    bool direct = IsDirect(meta->Type);

    const auto* integerMeta = static_cast<const TIntegerMeta*>(meta);
    auto baseValue = integerMeta->BaseValue;

    if (direct) {
        TCompressedVectorView valuesView(ptr);
        ptr += valuesView.GetSizeInWords();

        size_t itemCount = valuesView.GetSize();

        NullBits_ = TBitmap(ptr);
        ptr += GetBitmapSize(itemCount);

        auto [items] = AllocateCombined<T>(&Items_, itemCount);

        valuesView.UnpackTo(items);

#ifdef UNROLL_LOOPS
        auto tailCount = itemCount % 8;
        auto itemsEnd = items + itemCount - tailCount;

#define ITERATION \
        *items = ConvertInt<T>(baseValue + *items); \
        ++items;

        while (items < itemsEnd) {
            for (int index = 0; index < 8; ++index) {
                ITERATION
            }
        }

        for (int index = 0; index < static_cast<int>(tailCount); ++index) {
            ITERATION
        }
#undef ITERATION

#else
        for (size_t index = 0; index < itemCount; ++index) {
            items[index] = ConvertInt<T>(baseValue + items[index]);
        }
#endif

    } else {
        TCompressedVectorView valuesView(ptr);
        ptr += valuesView.GetSizeInWords();

        TCompressedVectorView idsView(ptr);
        ptr += idsView.GetSizeInWords();
        auto itemCount = idsView.GetSize();

        auto [items, nullBits] = AllocateCombined<T, TBit>(&Items_, itemCount, itemCount);

        NullBits_ = nullBits;

        values.resize(1 + valuesView.GetSize());
        // Zero id denotes null value and allows to eliminate extra branch.
        values[0] = 0;
        valuesView.UnpackTo(values.data() + 1);

        UnpackBitVector(idsView, &ids);

#ifdef UNROLL_LOOPS
        auto tailCount = itemCount % 8;
        auto itemsEnd = items + itemCount - tailCount;

        ui8* nullBitsData = nullBits.GetData();
        auto idsPtr = ids.data();

#define ITERATION(count) { \
            ui8 word = 0; \
            for (int index = 0; index < (count); ++index) { \
                auto id = *idsPtr++; \
                word |= ui8(!id) << index; \
                *items++ = ConvertInt<T>(baseValue + values[id]); \
            } \
            *nullBitsData++ = word; \
        }

        while (items < itemsEnd) {
            ITERATION(8)
        }

        ITERATION(static_cast<int>(tailCount))

#undef ITERATION

#else
        for (size_t index = 0; index < itemCount; ++index) {
            auto id = ids[index];
            nullBits.Set(index, id == 0);
            items[index] = ConvertInt<T>(baseValue + values[id]);
        }
#endif

    }

    return ptr;
}
#endif

template <class T, class TFunctor>
void UnpackDict(
    T* idsBuffer,
    TMutableBitmap nullBits,
    TCompressedVectorView idsView,
    TRange<TReadSpan> spans,
    TFunctor&& functor)
{
    ui32 offset = 0;
    for (auto [lower, upper] : spans) {
        idsView.UnpackTo(idsBuffer + offset, lower, upper);

        auto count = upper - lower;
        const auto offsetEnd = offset + count;

#ifdef UNROLL_LOOPS
        const auto alignedStart = AlignUp<ui32>(offset, 8);
        const auto alignedEnd = AlignDown<ui32>(offsetEnd, 8);

        if (alignedStart < alignedEnd) {
            while (offset != alignedStart) {
                auto id = idsBuffer[offset];
                nullBits.Set(offset, id == 0);
                functor(offset, id);
                ++offset;
            }

#define ITERATION(count) { \
                ui8 word = 0; \
                for (int index = 0; index < (count); ++index) { \
                    auto id = idsBuffer[offset]; \
                    word |= ui8(!id) << index; \
                    functor(offset, id); \
                    ++offset; \
                } \
                *nullBitsData++ = word; \
            }

            ui8* nullBitsData = nullBits.GetData() + offset / 8;

            do {
                ITERATION(8)
            } while (offset < alignedEnd);

            auto tailCount = static_cast<int>(offsetEnd - offset);
            if (tailCount) {
                ITERATION(tailCount);
            }
        } else {
            while (offset != offsetEnd) {
                auto id = idsBuffer[offset];
                nullBits.Set(offset, id == 0);
                functor(offset, id);
                ++offset;
            }
        }

#undef ITERATION

#else
        while (offset != offsetEnd) {
            auto id = idsBuffer[offset];
            nullBits.Set(offset, id == 0);
            functor(offset, id);
            ++offset;
        }
#endif

    }
}

template <class T>
const ui64* TScanIntegerExtractor<T>::InitData(
    const TMetaBase* meta,
    const ui64* ptr,
    TRange<TReadSpan> spans,
    ui32 batchSize,
    TTmpBuffers* /*tmpBuffers*/)
{
    CheckBatchSize(spans, batchSize);

    bool direct = IsDirect(meta->Type);

    const auto* integerMeta = static_cast<const TIntegerMeta*>(meta);
    auto baseValue = integerMeta->BaseValue;

    if (direct) {
        TCompressedVectorView valuesView(ptr);
        ptr += valuesView.GetSizeInWords();

        TBitmap nullBitsView(ptr);
        ptr += GetBitmapSize(valuesView.GetSize());

        auto [items, nullBits] = AllocateCombined<T, TBit>(&Items_, batchSize, batchSize);
        NullBits_ = nullBits;

        ui32 offset = 0;
        for (auto [lower, upper] : spans) {
            valuesView.UnpackTo(items + offset, lower, upper);

            for (size_t index = 0; index < upper - lower; ++index) {
                items[offset + index] = ConvertInt<T>(baseValue + items[offset + index]);
            }

            CopyBitmap(
                nullBits.GetData(),
                offset,
                nullBitsView.GetData(),
                lower,
                upper - lower);

            offset += upper - lower;
        }
    } else {
        TCompressedVectorView valuesView(ptr);
        ptr += valuesView.GetSizeInWords();

        TCompressedVectorView idsView(ptr);
        ptr += idsView.GetSizeInWords();

        auto [items, nullBits] = AllocateCombined<T, TBit>(&Items_, batchSize, batchSize);
        NullBits_ = nullBits;

        // Even if segment is read completely it can be initialized multiple times.
        if (SegmentChunkRowCount_ != meta->ChunkRowCount) {
            ValuesDict_.clear();
            SegmentChunkRowCount_ = meta->ChunkRowCount;
        }

        if (ValuesDict_.empty() && batchSize * UnpackSizeFactor > valuesView.GetSize()) {
            ValuesDict_.resize(valuesView.GetSize() + 1);
            // Zero id denotes null value and allows to eliminate extra branch.
            ValuesDict_[0] = 0;
            valuesView.UnpackTo(ValuesDict_.data() + 1);
        }

        if (!ValuesDict_.empty()) {
            const auto* valuesDict = ValuesDict_.data();

            UnpackDict(items, nullBits, idsView, spans, [&, items = items] (ui32 index, ui32 id) {
                items[index] = ConvertInt<T>(baseValue + valuesDict[id]);
            });
        } else {
            UnpackDict(items, nullBits, idsView, spans, [&, items = items] (ui32 index, ui32 id) {
                if (id > 0) {
                    items[index] = ConvertInt<T>(baseValue + valuesView[id - 1]);
                }
            });

        }
    }

    return ptr;
}

template <class T>
void TScanIntegerExtractor<T>::InitNullData()
{
    auto [items, nullBits] = AllocateCombined<T, TBit>(&Items_, 1, 1);
    NullBits_ = nullBits;

    items[0] = 0;
    nullBits.Set(0, true);
}

template
class TScanIntegerExtractor<i64>;

template
class TScanIntegerExtractor<ui64>;

const ui64* TScanDataExtractor<EValueType::Double>::GetEndPtr(const TMetaBase* /*meta*/, const ui64* ptr)
{
    ui64 count = *ptr++;
    ptr += count;
    ptr += GetBitmapSize(count);
    return ptr;
}

#ifdef FULL_UNPACK
const ui64* TScanDataExtractor<EValueType::Double>::InitData(
    const TMetaBase* /*meta*/,
    const ui64* ptr,
    TTmpBuffers* /*tmpBuffers*/)
{
    // No dictionary mode for double.
    ui64 count = *ptr++;
    Items_ = reinterpret_cast<const double*>(ptr);
    ptr += count;

    NullBits_ = TBitmap(ptr);
    ptr += GetBitmapSize(count);

    return ptr;
}
#endif

const ui64* TScanDataExtractor<EValueType::Double>::InitData(
    const TMetaBase* /*meta*/,
    const ui64* ptr,
    TRange<TReadSpan> spans,
    ui32 batchSize,
    TTmpBuffers* /*tmpBuffers*/)
{
    CheckBatchSize(spans, batchSize);

    // No dictionary mode for double.
    ui64 count = *ptr++;
    auto* itemsData  = reinterpret_cast<const double*>(ptr);
    ptr += count;

    TBitmap nullBitsView(ptr);
    ptr += GetBitmapSize(count);

    auto [items, nullBits] = AllocateCombined<double, TBit>(&Holder_, batchSize, batchSize);
    Items_ = items;
    NullBits_ = nullBits;

    int offset = 0;
    for (auto [lower, upper] : spans) {
        for (ui32 index = 0; index < upper - lower; ++index) {
            items[offset + index] = itemsData[lower + index];
        }

        CopyBitmap(
            nullBits.GetData(),
            offset,
            nullBitsView.GetData(),
            lower,
            upper - lower);


        offset += upper - lower;
    }

    return ptr;
}

void TScanDataExtractor<EValueType::Double>::InitNullData()
{
    auto [items, nullBits] = AllocateCombined<double, TBit>(&Holder_, 1, 1);

    items[0] = 0;
    nullBits.Set(0, true);

    Items_ = items;
    NullBits_ = nullBits;
}

ui64 TScanDataExtractor<EValueType::Boolean>::NullBooleanSegmentData;

const ui64* TScanDataExtractor<EValueType::Boolean>::GetEndPtr(const TMetaBase* /*meta*/, const ui64* ptr)
{
    ui64 count = *ptr++;
    ptr += GetBitmapSize(count);
    ptr += GetBitmapSize(count);

    return ptr;
}

#ifdef FULL_UNPACK
const ui64* TScanDataExtractor<EValueType::Boolean>::InitData(
    const TMetaBase* /*meta*/,
    const ui64* ptr,
    TTmpBuffers* /*tmpBuffers*/)
{
    ui64 count = *ptr++;
    Items_ = TBitmap(ptr);
    ptr += GetBitmapSize(count);

    NullBits_ = TBitmap(ptr);
    ptr += GetBitmapSize(count);

    return ptr;
}
#endif

const ui64* TScanDataExtractor<EValueType::Boolean>::InitData(
    const TMetaBase* /*meta*/,
    const ui64* ptr,
    TRange<TReadSpan> spans,
    ui32 batchSize,
    TTmpBuffers* /*tmpBuffers*/)
{
    CheckBatchSize(spans, batchSize);

    ui64 count = *ptr++;
    TBitmap itemsData(ptr);
    ptr += GetBitmapSize(count);

    TBitmap nullBitsView(ptr);
    ptr += GetBitmapSize(count);

    auto [items, nullBits] = AllocateCombined<TBit, TBit>(&Holder_, batchSize, batchSize);

    Items_ = items;
    NullBits_ = nullBits;

    int offset = 0;
    for (auto [lower, upper] : spans) {
        CopyBitmap(
            items.GetData(),
            offset,
            itemsData.GetData(),
            lower,
            upper - lower);

        CopyBitmap(
            nullBits.GetData(),
            offset,
            nullBitsView.GetData(),
            lower,
            upper - lower);

        offset += upper - lower;
    }

    return ptr;
}

void TScanDataExtractor<EValueType::Boolean>::InitNullData()
{
    TMutableBitmap bitmap(&NullBooleanSegmentData);
    bitmap.Set(0, true);

    Items_ = bitmap;
    NullBits_ = bitmap;
}

#ifdef FULL_UNPACK
void TScanBlobExtractor::InitData(const TMetaBase* meta, const ui64* ptr, TTmpBuffers* tmpBuffers)
{
    const bool direct = IsDirect(meta->Type);
    const auto expectedLength = static_cast<const TBlobMeta*>(meta)->ExpectedLength;

    auto& offsets = tmpBuffers->Offsets;

    if (direct) {
        ptr += UnpackBitVector(ptr, &offsets);
        auto valueCount = offsets.size();

        auto [items] = AllocateCombined<TItem>(&Items_, valueCount);

        ui32 begin = 0;
        for (size_t index = 0; index < valueCount; ++index) {
            ui32 end = GetOffsetNonZero(offsets, expectedLength, index + 1);
            items[index] = {begin, end};
            begin = end;
        }

        NullBits_ = TBitmap(ptr);
        ptr += GetBitmapSize(valueCount);

    } else {
        auto& ids = tmpBuffers->Ids;

        ptr += UnpackBitVector(ptr, &ids);
        auto valueCount = ids.size();
        ptr += UnpackBitVector(ptr, &offsets);

        auto [items, nullBits] = AllocateCombined<TItem, TBit>(&Items_, valueCount, valueCount);

        for (size_t index = 0; index < valueCount; ++index) {
            auto id = ids[index];
            nullBits.Set(index, id == 0);

            if (id > 0) {
                items[index] = {
                    GetOffset(offsets, expectedLength, id - 1),
                    GetOffsetNonZero(offsets, expectedLength, id)};
            }
        }

        NullBits_ = nullBits;
    }

    Data_ = reinterpret_cast<const char*>(ptr);
}
#endif

void TScanBlobExtractor::InitData(
    const TMetaBase* meta,
    const ui64* ptr,
    TRange<TReadSpan> spans,
    ui32 batchSize,
    TTmpBuffers* tmpBuffers)
{
    const bool direct = IsDirect(meta->Type);
    const auto expectedLength = static_cast<const TBlobMeta*>(meta)->ExpectedLength;

    CheckBatchSize(spans, batchSize);

    auto [items, nullBits] = AllocateCombined<TItem, TBit>(&Items_, batchSize, batchSize);

    if (direct) {
        TCompressedVectorView offsetsView(ptr);
        ptr += offsetsView.GetSizeInWords();
        TBitmap nullBitsView(ptr);
        ptr += GetBitmapSize(offsetsView.GetSize());

        int offset = 0;
        for (auto [lower, upper] : spans) {
            ui32 begin = GetOffset(offsetsView, expectedLength, lower);
            for (ui32 index = 0; index < upper - lower; ++index) {
                ui32 end = GetOffsetNonZero(offsetsView, expectedLength, lower + index + 1);
                items[offset + index] = {begin, end};
                begin = end;

                nullBits.Set(offset + index, nullBitsView[lower + index]);
            }

            offset += upper - lower;
        }
    } else {
        TCompressedVectorView idsView(ptr);
        ptr += idsView.GetSizeInWords();
        TCompressedVectorView offsetsView(ptr);
        ptr += offsetsView.GetSizeInWords();

        auto& ids = tmpBuffers->Ids;
        ids.resize(batchSize);

        // Even if segment is read completely it can be initialized multiple times.
        if (SegmentChunkRowCount_ != meta->ChunkRowCount) {
            OffsetsDict_.clear();
            SegmentChunkRowCount_ = meta->ChunkRowCount;
        }

        if (OffsetsDict_.empty() && batchSize * UnpackSizeFactor > offsetsView.GetSize()) {
            OffsetsDict_.resize(offsetsView.GetSize() + 1);
            // Zero id denotes null value and allows to eliminate extra branch.
            OffsetsDict_[0] = 0;
            offsetsView.UnpackTo(OffsetsDict_.data() + 1);
        }

        if (!OffsetsDict_.empty()) {
            const auto* offsets = OffsetsDict_.data() + 1;

            UnpackDict(ids.data(), nullBits, idsView, spans, [&, items = items] (ui32 index, ui32 id) {
                // FIXME(lukyan): Cannot remove extra branch because of ui32 index.
                if (id > 0) {
                    items[index] = {
                        GetOffset(offsets, expectedLength, id - 1),
                        GetOffsetNonZero(offsets, expectedLength, id)};
                }
            });
        } else {
            UnpackDict(ids.data(), nullBits, idsView, spans, [&, items = items] (ui32 index, ui32 id) {
                if (id > 0) {
                    items[index] = {
                        GetOffset(offsetsView, expectedLength, id - 1),
                        GetOffsetNonZero(offsetsView, expectedLength, id)};
                }
            });
        }
    }

    NullBits_ = nullBits;
    Data_ = reinterpret_cast<const char*>(ptr);
}

void TScanBlobExtractor::InitNullData()
{
    auto [items, nullBits] = AllocateCombined<TItem, TBit>(&Items_, 1, 1);

    items[0] = TItem{0, 0};
    nullBits.Set(0, true);

    NullBits_ = nullBits;
    Data_ = nullptr;
}

#ifdef FULL_UNPACK
const ui64* TScanKeyIndexExtractor::InitIndex(const TMetaBase* meta, const ui64* ptr, bool dense)
{
    SegmentRowLimit_ = meta->ChunkRowCount;
    ui32 rowOffset = meta->ChunkRowCount - meta->RowCount;

    if (dense) {
        Count_ = meta->RowCount;
        auto rowIndexData = RowIndexes_.Resize(Count_ + 1, GetRefCountedTypeCookie<TRowIndexTag>());
        auto rowIndexDataEnd = rowIndexData + Count_;

#define ITERATION *rowIndexData++ = rowOffset++;
        while (rowIndexData + 4 < rowIndexDataEnd) {
            ITERATION
            ITERATION
            ITERATION
            ITERATION
        }

        while (rowIndexData < rowIndexDataEnd) {
            ITERATION
        }
#undef ITERATION
    } else {
        TCompressedVectorView rowIndexView(ptr);
        ptr += rowIndexView.GetSizeInWords();

        Count_ = rowIndexView.GetSize();
        auto rowIndexData = RowIndexes_.Resize(Count_ + 1, GetRefCountedTypeCookie<TRowIndexTag>());
        auto rowIndexDataEnd = rowIndexData + Count_;

        rowIndexView.UnpackTo(rowIndexData);

#define ITERATION *rowIndexData++ += rowOffset;
        while (rowIndexData + 4 < rowIndexDataEnd) {
            ITERATION
            ITERATION
            ITERATION
            ITERATION
        }

        while (rowIndexData < rowIndexDataEnd) {
            ITERATION
        }
#undef ITERATION
    }

    RowIndexes_[Count_] = meta->ChunkRowCount;

    return ptr;
}
#endif

const ui64* TScanKeyIndexExtractor::InitIndex(
    const TMetaBase* meta,
    const ui64* ptr,
    bool dense,
    TInitContext* initContext)
{
    ui32 segmentRowOffset = meta->ChunkRowCount - meta->RowCount;

    auto rowOffset = initContext->RowOffset;

    auto slice = GetBatchSlice(initContext->Spans, meta->ChunkRowCount);
    ui32 batchSize = slice.GetBatchSize();
    // Segment can be initialized multiple times if block bound (of other columns) crosses segment.
    YT_VERIFY(!slice.GetSpans().empty());
    SegmentRowLimit_ = slice.GetSpans().end()[-1].Upper;

    initContext->DataSpans.resize(slice.GetSize());
    auto* offsetsSpans = initContext->DataSpans.data();

    if (dense) {
        auto rowIndexes = RowIndexes_.Resize(batchSize + 1, GetRefCountedTypeCookie<TRowIndexTag>());
        auto rowIndexesEnd = rowIndexes + batchSize;

#define ITERATION *rowIndexes++ = rowOffset++;
        while (rowIndexes + 4 < rowIndexesEnd) {
            ITERATION
            ITERATION
            ITERATION
            ITERATION
        }

        while (rowIndexes < rowIndexesEnd) {
            ITERATION
        }
#undef ITERATION

        *rowIndexes = rowOffset;
        Count_ = rowIndexes - RowIndexes_.GetData();
        YT_ASSERT(Count_ == batchSize);

        for (auto [lower, upper] : slice.GetSpans()) {
            auto segmentLower = lower - segmentRowOffset;
            auto segmentUpper = upper - segmentRowOffset;
            *offsetsSpans++ = {segmentLower, segmentUpper};
        }
    } else {
        TCompressedVectorView rowIndexesView(ptr);
        ptr += rowIndexesView.GetSizeInWords();

        ui32 segmentItemCount = rowIndexesView.GetSize();
        auto bufferSize = std::min(segmentItemCount, batchSize) + 1;

        auto* rowIndexes = RowIndexes_.Resize(bufferSize, GetRefCountedTypeCookie<TRowIndexTag>());
        auto* rowIndexesBufferEnd = rowIndexes + bufferSize;

        // Source spans can be clashed if they are in one RLE range.
        // So offsetsSpans.size() will be less than or equal to slice.GetSpans().size().
        ui32 lastSegmentRowIndex = 0;

        // First item is always zero.
        YT_VERIFY(rowIndexesView[0] == 0);
        YT_VERIFY(segmentItemCount > 0);
        ui32 valueOffset = 1;

        for (auto [lower, upper] : slice.GetSpans()) {
            auto segmentLower = lower - segmentRowOffset;
            auto segmentUpper = upper - segmentRowOffset;


            if (segmentUpper <= lastSegmentRowIndex) {
                rowOffset += upper - lower;
                continue;
            } else if (segmentLower < lastSegmentRowIndex) {
                rowOffset += lastSegmentRowIndex - segmentLower;
                segmentLower = lastSegmentRowIndex;
            }

            valueOffset = ExponentialSearch(valueOffset, segmentItemCount, [&] (ui32 valueOffset) {
                return rowIndexesView[valueOffset] <= segmentLower;
            });

            if (offsetsSpans > initContext->DataSpans.data()) {
                YT_ASSERT(offsetsSpans[-1].Upper <= valueOffset - 1);
            }

            ui32 valueOffsetEnd = ExponentialSearch(valueOffset, segmentItemCount, [&] (ui32 valueOffset) {
                return rowIndexesView[valueOffset] < segmentUpper;
            });

            if (valueOffsetEnd != segmentItemCount) {
                lastSegmentRowIndex = rowIndexesView[valueOffsetEnd];
            } else {
                lastSegmentRowIndex = meta->RowCount;
            }

            YT_ASSERT(rowIndexes < rowIndexesBufferEnd);
            *rowIndexes++ = rowOffset;

            rowIndexesView.UnpackTo(rowIndexes, valueOffset, valueOffsetEnd);

            auto* rowIndexesEnd = rowIndexes + valueOffsetEnd - valueOffset;
            while (rowIndexes != rowIndexesEnd) {
                YT_ASSERT(rowIndexes < rowIndexesBufferEnd);
                YT_ASSERT(*rowIndexes + rowOffset >= segmentLower);

                *rowIndexes++ += rowOffset - segmentLower;
            }

            *offsetsSpans++ = {valueOffset - 1, valueOffsetEnd};

            rowOffset += segmentUpper - segmentLower;
            valueOffset = valueOffsetEnd;
        }

        initContext->DataSpans.resize(offsetsSpans - initContext->DataSpans.data());

        YT_VERIFY(rowIndexes > RowIndexes_.GetData());
        YT_VERIFY(rowIndexes[-1] < rowOffset);
        YT_VERIFY(rowIndexes < rowIndexesBufferEnd);
        *rowIndexes = rowOffset;
        Count_ = rowIndexes - RowIndexes_.GetData();

#ifndef NDEBUG
        ui32 dataBatchSize = 0;
        for (auto [lower, upper] : initContext->DataSpans) {
            dataBatchSize += upper - lower;
        }
        YT_VERIFY(dataBatchSize == Count_);
#endif
    };

    return ptr;
}

void TScanKeyIndexExtractor::InitNullIndex()
{
    Count_ = 1;
    RowIndexes_.Resize(2);
    RowIndexes_[0] = 0;
    RowIndexes_[1] = std::numeric_limits<ui32>::max();
    SegmentRowLimit_ = std::numeric_limits<ui32>::max();
}

#ifdef FULL_UNPACK
const ui64* TScanVersionExtractor<true>::InitVersion(const ui64* ptr)
{
    TCompressedVectorView writeTimestampIdsView(ptr);
    ptr += writeTimestampIdsView.GetSizeInWords();

    auto count = writeTimestampIdsView.GetSize();
    WriteTimestampIds_.Resize(count, GetRefCountedTypeCookie<TWriteIdsTag>());
    writeTimestampIdsView.UnpackTo(WriteTimestampIds_.GetData());

    AggregateBits_ = TBitmap(ptr);
    ptr += GetBitmapSize(count);

    return ptr;
}
#endif

const ui64* TScanVersionExtractor<true>::InitVersion(const ui64* ptr, TRange<TReadSpan> spans, ui32 batchSize)
{
    CheckBatchSize(spans, batchSize);

    TCompressedVectorView writeTimestampIdsView(ptr);
    ptr += writeTimestampIdsView.GetSizeInWords();
    TBitmap aggregateBitsView(ptr);
    ptr += GetBitmapSize(writeTimestampIdsView.GetSize());

    auto [writeTimestampIds, aggregateBits] = AllocateCombined<ui32, TBit>(
        &WriteTimestampIds_, batchSize, batchSize);

    ui32 offset = 0;
    for (auto [lower, upper] : spans) {
        writeTimestampIdsView.UnpackTo(writeTimestampIds, lower, upper);
        writeTimestampIds += upper - lower;

        CopyBitmap(
            aggregateBits.GetData(),
            offset,
            aggregateBitsView.GetData(),
            lower,
            upper - lower);

        offset += upper - lower;
    }

    AggregateBits_ = aggregateBits;

    return ptr;
}

#ifdef FULL_UNPACK
const ui64* TScanVersionExtractor<false>::InitVersion(const ui64* ptr)
{
    TCompressedVectorView writeTimestampIdsView(ptr);
    ptr += writeTimestampIdsView.GetSizeInWords();

    auto count = writeTimestampIdsView.GetSize();
    WriteTimestampIds_.Resize(count, GetRefCountedTypeCookie<TWriteIdsTag>());
    writeTimestampIdsView.UnpackTo(WriteTimestampIds_.GetData());

    return ptr;
}
#endif

const ui64* TScanVersionExtractor<false>::InitVersion(const ui64* ptr, TRange<TReadSpan> spans, ui32 batchSize)
{
    CheckBatchSize(spans, batchSize);

    TCompressedVectorView writeTimestampIdsView(ptr);
    ptr += writeTimestampIdsView.GetSizeInWords();

    auto writeTimestampIds = WriteTimestampIds_.Resize(batchSize, GetRefCountedTypeCookie<TWriteIdsTag>());
    for (auto [lower, upper] : spans) {
        writeTimestampIdsView.UnpackTo(writeTimestampIds, lower, upper);
        writeTimestampIds += upper - lower;
    }

    return ptr;
}

#ifdef FULL_UNPACK
const ui64* TScanMultiValueIndexExtractor::InitIndex(
    const TMetaBase* meta,
    const TDenseMeta* denseMeta,
    const ui64* ptr,
    TTmpBuffers* tmpBuffers)
{
    SegmentRowLimit_ = meta->ChunkRowCount;

    ui32 rowOffset = meta->ChunkRowCount - meta->RowCount;

    auto& offsets = tmpBuffers->Offsets;
    ptr += UnpackBitVector(ptr, &offsets);

    if (denseMeta) {
        ui32 expectedPerRow = denseMeta->ExpectedPerRow;

        auto perRowDiff = offsets.data();
        ui32 rowCount = offsets.size();
        ui32 valueCount = GetOffsetNonZero(perRowDiff, expectedPerRow, rowCount);

        auto rowToValue = RowToValue_.Resize(valueCount + 1, GetRefCountedTypeCookie<TRowToValueTag>());

        ui32 rowIndex = 0;
        ui32 valueOffset = 0;
#define ITERATION { \
            ui32 nextOffset = GetOffsetNonZero(perRowDiff, expectedPerRow, rowIndex + 1); \
            if (nextOffset - valueOffset) { \
                *rowToValue++ = {rowOffset + rowIndex, valueOffset}; \
            }   \
            valueOffset = nextOffset; \
            ++rowIndex; \
        }

#ifdef UNROLL_LOOPS
        while (rowIndex + 4 < rowCount) {
            ITERATION
            ITERATION
            ITERATION
            ITERATION
        }
#endif
        while (rowIndex < rowCount) {
            ITERATION
        }

#undef ITERATION

        YT_VERIFY(meta->ChunkRowCount == rowOffset + rowCount);
        YT_VERIFY(valueOffset == valueCount);
        // Extra ValueIndex is used in ReadRows.
        *rowToValue = {SegmentRowLimit_, valueCount};

        IndexCount_ = rowToValue - RowToValue_.GetData();
    } else {
        auto rowIndexes = offsets.data();
        ui32 count = offsets.size();

        auto rowToValue = RowToValue_.Resize(count + 1, GetRefCountedTypeCookie<TRowToValueTag>());

        // Init with sentinel row index.
        auto rowIndex = SegmentRowLimit_;
        for (ui32 valueOffset = 0; valueOffset < count; ++valueOffset) {
            if (rowIndexes[valueOffset] != rowIndex) {
                rowIndex = rowIndexes[valueOffset];
                *rowToValue++ = {rowOffset + rowIndex, valueOffset};
            }
        }

        // Extra ValueIndex is used in ReadRows.
        *rowToValue = {SegmentRowLimit_, count};

        IndexCount_ = rowToValue - RowToValue_.GetData();
    }

    return ptr;
}
#endif

// Init only pro spans of row indexes.
const ui64* TScanMultiValueIndexExtractor::InitIndex(
    const TMetaBase* meta,
    const TDenseMeta* denseMeta,
    const ui64* ptr,
    TInitContext* initContext)
{
    ui32 segmentRowOffset = meta->ChunkRowCount - meta->RowCount;

    auto rowOffset = initContext->RowOffset;

    auto slice = GetBatchSlice(initContext->Spans, meta->ChunkRowCount);
    ui32 batchSize = slice.GetBatchSize();

    // Segment can be initialized multiple times if block bound (of other columns) crosses segment.
    YT_VERIFY(!slice.GetSpans().empty());
    SegmentRowLimit_ = slice.GetSpans().end()[-1].Upper;

    initContext->DataSpans.resize(slice.GetSize());
    auto* offsetsSpans = initContext->DataSpans.data();

    if (denseMeta) {
        // For dense unpack only span ranges.
        TCompressedVectorView perRowDiffsView(ptr);
        ptr += perRowDiffsView.GetSizeInWords();

        auto rowToValue = RowToValue_.Resize(batchSize + 1, GetRefCountedTypeCookie<TRowToValueTag>());

        ui32 expectedPerRow = denseMeta->ExpectedPerRow;

        ui32 valueOffset = 0;

        // Upper part of rowToValue buffer is used for perRowDiffs.
        auto perRowDiffs = reinterpret_cast<ui32*>(rowToValue + batchSize + 1) - (batchSize + 1);

        for (auto [lower, upper] : slice.GetSpans()) {
            auto segmentLower = lower - segmentRowOffset;
            auto segmentUpper = upper - segmentRowOffset;

            auto startSegmentOffset = GetOffset(perRowDiffsView, expectedPerRow, segmentLower);
            perRowDiffsView.UnpackTo(perRowDiffs, segmentLower, segmentUpper);

            ui32 count = segmentUpper - segmentLower;

            ui32 startValueOffset = valueOffset;
            ui32 pivot = valueOffset + expectedPerRow * segmentLower - startSegmentOffset;

#ifndef NDEBUG
#define CHECK_EXPECTED_OFFSET \
                auto expectedNextOffset = GetOffset( \
                    perRowDiffsView, \
                    expectedPerRow, \
                    segmentLower + position + 1) - startSegmentOffset + startValueOffset; \
                YT_VERIFY(nextValueOffset == expectedNextOffset);
#else
#define CHECK_EXPECTED_OFFSET
#endif

#define ITERATION { \
                pivot += expectedPerRow; \
                auto nextValueOffset = pivot + ZigZagDecode32(perRowDiffs[position]); \
                CHECK_EXPECTED_OFFSET \
                YT_ASSERT(nextValueOffset >= valueOffset); \
                if (nextValueOffset > valueOffset) { \
                    YT_VERIFY(reinterpret_cast<ui32*>(rowToValue) < perRowDiffs + position); \
                    *rowToValue++ = {rowOffset + position, valueOffset}; \
                } \
                valueOffset = nextValueOffset; \
                ++position; \
            }

            ui32 position = 0;
#ifdef UNROLL_LOOPS
            while (position + 4 < count) {
                ITERATION
                ITERATION
                ITERATION
                ITERATION
            }
#endif

            while (position < count) {
                ITERATION
            }

#undef ITERATION
#undef CHECK_EXPECTED_OFFSET

            rowOffset += count;
            perRowDiffs += count;

            auto endSegmentOffset = startSegmentOffset + valueOffset - startValueOffset;
            auto endSegmentOffset0 = GetOffset(perRowDiffsView, expectedPerRow, segmentUpper);
            YT_VERIFY(endSegmentOffset == endSegmentOffset0);

            // TODO(lukyan): Skip empty data spans (if startSegmentOffset == endSegmentOffset)?
            *offsetsSpans++ = {startSegmentOffset, endSegmentOffset};

            YT_VERIFY(valueOffset - startValueOffset == endSegmentOffset - startSegmentOffset);
        }

        IndexCount_ = rowToValue - RowToValue_.GetData();

        // Extra ValueIndex is used in ReadRows.
        *rowToValue = {rowOffset, valueOffset};
    } else {
        TCompressedVectorView rowIndexesView(ptr);
        ptr += rowIndexesView.GetSizeInWords();

        auto rowToValue = RowToValue_.Resize(batchSize + 1, GetRefCountedTypeCookie<TRowToValueTag>());

        ui32 count = rowIndexesView.GetSize();

        // Value offset in segment.
        ui32 valueOffset = 0;
        ui32 valueCount = 0;

        for (auto [lower, upper] : slice.GetSpans()) {
            auto segmentLower = lower - segmentRowOffset;
            auto segmentUpper = upper - segmentRowOffset;

            valueOffset = ExponentialSearch(valueOffset, count, [&] (ui32 valueOffset) {
                return rowIndexesView[valueOffset] < segmentLower;
            });

            ui32 valueOffsetEnd = ExponentialSearch(valueOffset, count, [&] (ui32 valueOffset) {
                return rowIndexesView[valueOffset] < segmentUpper;
            });

            // Size of rowIndexesView may be much greater than batchSize if there are few rows but many values in row.
            // We unpack values to initContext->Offsets buffer.
            auto& rowIndexes = initContext->Offsets;
            rowIndexes.resize(valueOffsetEnd - valueOffset);
            rowIndexesView.UnpackTo(rowIndexes.data(), valueOffset, valueOffsetEnd);

            // Init with sentinel row index.
            ui32 rowIndex = -1;

#define ITERATION \
            if (rowIndexes[position] != rowIndex) { \
                rowIndex = rowIndexes[position]; \
                *rowToValue++ = {rowIndex - segmentLower + rowOffset, position + valueCount}; \
            } \
            ++position;

            ui32 position = 0;
            ui32 count = valueOffsetEnd - valueOffset;

#ifdef UNROLL_LOOPS
            while (position + 4 < count) {
                ITERATION
                ITERATION
                ITERATION
                ITERATION
            }
#endif
            while (position < count) {
                ITERATION
            }

#undef ITERATION

            rowOffset += segmentUpper - segmentLower;

            // TODO(lukyan): Skip empty data spans (if valueOffset == valueOffsetEnd)?
            *offsetsSpans++ = {valueOffset, valueOffsetEnd};
            valueCount += valueOffsetEnd - valueOffset;

            valueOffset = valueOffsetEnd;
        }

        IndexCount_ = rowToValue - RowToValue_.GetData();

        // Extra ValueIndex is used in ReadRows.
        *rowToValue = {rowOffset, valueCount};
    }

    return ptr;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNewTableClient
