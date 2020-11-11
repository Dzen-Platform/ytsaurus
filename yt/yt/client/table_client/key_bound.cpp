#include "key_bound.h"

#include "row_buffer.h"
#include "serialize.h"

namespace NYT::NTableClient {

using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

//! Used only for YT_LOG_FATAL below.
static const TLogger Logger("TableClientKey");

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

template <class TRow, class TKeyBound>
TKeyBound TKeyBoundImpl<TRow, TKeyBound>::FromRow(const TRow& row, bool isInclusive, bool isUpper)
{
    ValidateValueTypes(row);
    TKeyBound result;
    result.Prefix = row;
    result.IsInclusive = isInclusive;
    result.IsUpper = isUpper;
    return result;
}

template <class TRow, class TKeyBound>
TKeyBound TKeyBoundImpl<TRow, TKeyBound>::FromRow(TRow&& row, bool isInclusive, bool isUpper)
{
    ValidateValueTypes(row);
    TKeyBound result;
    result.Prefix = row;
    result.IsInclusive = isInclusive;
    result.IsUpper = isUpper;
    return result;
}

template <class TRow, class TKeyBound>
TKeyBound TKeyBoundImpl<TRow, TKeyBound>::FromRowUnchecked(const TRow& row, bool isInclusive, bool isUpper)
{
#ifndef NDEBUG
    try {
        ValidateValueTypes(row);
    } catch (const std::exception& ex) {
        YT_LOG_FATAL(ex, "Unexpected exception while building key bound from row");
    }
#endif

    TKeyBound result;
    result.Prefix = row;
    result.IsInclusive = isInclusive;
    result.IsUpper = isUpper;
    return result;
}

template <class TRow, class TKeyBound>
TKeyBound TKeyBoundImpl<TRow, TKeyBound>::FromRowUnchecked(TRow&& row, bool isInclusive, bool isUpper)
{
#ifndef NDEBUG
    try {
        ValidateValueTypes(row);
    } catch (const std::exception& ex) {
        YT_LOG_FATAL(ex, "Unexpected exception while building key bound from row");
    }
#endif

    TKeyBound result;
    result.Prefix = row;
    result.IsInclusive = isInclusive;
    result.IsUpper = isUpper;
    return result;
}

template <class TRow, class TKeyBound>
TKeyBound TKeyBoundImpl<TRow, TKeyBound>::MakeUniversal(bool isUpper)
{
    return TKeyBoundImpl<TRow, TKeyBound>::FromRow(EmptyKey(), /* isInclusive */ true, isUpper);
}

template <class TRow, class TKeyBound>
void TKeyBoundImpl<TRow, TKeyBound>::ValidateValueTypes(const TRow& row)
{
    for (const auto& value : row) {
        ValidateDataValueType(value.Type);
    }
}

template <class TRow, class TKeyBound>
void TKeyBoundImpl<TRow, TKeyBound>::FormatValue(TStringBuilderBase* builder) const
{
    builder->AppendChar(IsUpper ? '<' : '>');
    if (IsInclusive) {
        builder->AppendChar('=');
    }
    builder->AppendFormat("%v", Prefix);
}

template <class TRow, class TKeyBound>
bool TKeyBoundImpl<TRow, TKeyBound>::IsUniversal() const
{
    return IsInclusive && Prefix.GetCount() == 0;
}

template <class TRow, class TKeyBound>
TKeyBound TKeyBoundImpl<TRow, TKeyBound>::Invert() const
{
    return TKeyBound::FromRowUnchecked(Prefix, !IsInclusive, !IsUpper);
}

template <class TRow, class TKeyBound>
TKeyBound TKeyBoundImpl<TRow, TKeyBound>::ToggleInclusiveness() const
{
    return TKeyBound::FromRowUnchecked(Prefix, !IsInclusive, IsUpper);
}

template <class TRow, class TKeyBound>
TKeyBound TKeyBoundImpl<TRow, TKeyBound>::UpperCounterpart() const
{
    return IsUpper ? *static_cast<const TKeyBound*>(this) : Invert();
}

template <class TRow, class TKeyBound>
TKeyBound TKeyBoundImpl<TRow, TKeyBound>::LowerCounterpart() const
{
    return IsUpper ? Invert() : *static_cast<const TKeyBound*>(this);
}

template <class TRow, class TKeyBound>
void TKeyBoundImpl<TRow, TKeyBound>::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Prefix);
    Persist(context, IsInclusive);
    Persist(context, IsUpper);
}

////////////////////////////////////////////////////////////////////////////////

template class TKeyBoundImpl<TUnversionedRow, TKeyBound>;
template class TKeyBoundImpl<TUnversionedOwningRow, TOwningKeyBound>;

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

TOwningKeyBound::operator TKeyBound() const
{
    TKeyBound result;
    result.Prefix = Prefix;
    result.IsInclusive = IsInclusive;
    result.IsUpper = IsUpper;
    return result;
}

void FormatValue(TStringBuilderBase* builder, const TOwningKeyBound& keyBound, TStringBuf /*format*/)
{
    return keyBound.FormatValue(builder);
}

TString ToString(const TOwningKeyBound& keyBound)
{
    return ToStringViaBuilder(keyBound);
}

////////////////////////////////////////////////////////////////////////////////

void FormatValue(TStringBuilderBase* builder, const TKeyBound& keyBound, TStringBuf /*format*/)
{
    return keyBound.FormatValue(builder);
}

TString ToString(const TKeyBound& keyBound)
{
    return ToStringViaBuilder(keyBound);
}

////////////////////////////////////////////////////////////////////////////////

bool operator ==(const TKeyBound& lhs, const TKeyBound& rhs)
{
    return
        lhs.Prefix == rhs.Prefix &&
        lhs.IsInclusive == rhs.IsInclusive &&
        lhs.IsUpper == rhs.IsUpper;
}

////////////////////////////////////////////////////////////////////////////////

// Common implementation for owning case and non-owning case over row buffer.
// Returns pair {prefixKeyLength, isInclusive} describing how to transform
// legacy key into key bound.
std::pair<int, bool> KeyBoundFromLegacyRowImpl(TUnversionedRow row, bool isUpper, int keyLength)
{
    // Flag indicating that row starts with #keyLength non-sentinel values followed by at least one arbitrary value.
    bool isLongRow = false;

    // If row contains at least one sentinel on first #keyLength positions, type of leftmost of them.
    std::optional<EValueType> leftmostSentinelType;

    // Length of the longest prefix of row which is free of sentinels. Prefix length is limited by #keyLength.
    int prefixLength = 0;
    for (int index = 0; index < row.GetCount() && index <= keyLength; ++index) {
        if (index == keyLength) {
            isLongRow = true;
            break;
        }
        if (row[index].Type != EValueType::Min && row[index].Type != EValueType::Max) {
            ++prefixLength;
        } else {
            leftmostSentinelType = row[index].Type;
            break;
        }
    }

    // When dealing with legacy rows, upper limit is always exclusive and lower limit is always inclusive.
    // We will call this kind of inclusiveness standard. This implies following cases for key bounds.
    //
    // (A) If row is long, upper limit will be inclusive and lower limit will be exclusive, i.e. inclusiveness is toggled.
    // (B) Otherwise, if row has exactly length of #keyLength and does not contain sentinels, inclusiveness is standard.
    //
    // Suppose none of (A) and (B) happened. We know that prefix is strictly shorter than #keyLength. If may or may not be
    // followed by a sentinel. Actually there is no difference if prefix is followed by Min or if it is not followed by sentinel.
    // To prove this fact, consider row R = prefix + [Min], length(R) < #keyLength and key K, length(K) == #keyLength.
    // It is easy to see that R is compared to K in exactly the same way as prefix is compared to K; this case
    // corresponds to a key bound with standard inclusiveness.
    //
    // Similar argument shows that if prefix is followed by Max, key bound inclusiveness should be toggled.
    //
    // So, we have only two more cases:
    //
    // (C) Otherwise, if prefix is followed by Min or no sentinel, inclusiveness is standard.
    // (D) Otherwise (prefix is followed by Max), inclusiveness is toggled.

    // Cases (A) and (D).
    bool toggleInclusiveness = isLongRow || leftmostSentinelType == EValueType::Max;

    bool isInclusive = (isUpper && toggleInclusiveness) || (!isUpper && !toggleInclusiveness);

    return {prefixLength, isInclusive};
}

TOwningKeyBound KeyBoundFromLegacyRow(TUnversionedRow row, bool isUpper, int keyLength)
{
    if (!row) {
        return TOwningKeyBound::MakeUniversal(isUpper);
    }

    auto [prefixLength, isInclusive] = KeyBoundFromLegacyRowImpl(row, isUpper, keyLength);
    return TOwningKeyBound::FromRow(
        TUnversionedOwningRow(row.Begin(), row.Begin() + prefixLength),
        isInclusive,
        isUpper);
}

TKeyBound KeyBoundFromLegacyRow(TUnversionedRow row, bool isUpper, int keyLength, const TRowBufferPtr& rowBuffer)
{
    if (!row) {
        return TKeyBound::MakeUniversal(isUpper);
    }

    auto [prefixLength, isInclusive] = KeyBoundFromLegacyRowImpl(row, isUpper, keyLength);
    return TKeyBound::FromRow(
        rowBuffer->Capture(row.Begin(), prefixLength),
        isInclusive,
        isUpper);
}

TUnversionedOwningRow KeyBoundToLegacyRow(TKeyBound keyBound)
{
    TUnversionedOwningRowBuilder builder;
    for (const auto& value : keyBound.Prefix) {
        builder.AddValue(value);
    }
    auto shouldAddMax = (keyBound.IsUpper && keyBound.IsInclusive) || (!keyBound.IsUpper && !keyBound.IsInclusive);
    if (shouldAddMax) {
        builder.AddValue(MakeUnversionedSentinelValue(EValueType::Max));
    }
    return builder.FinishRow();
}

TUnversionedRow KeyBoundToLegacyRow(TKeyBound keyBound, const TRowBufferPtr& rowBuffer)
{
    auto shouldAddMax = (keyBound.IsUpper && keyBound.IsInclusive) || (!keyBound.IsUpper && !keyBound.IsInclusive);

    auto row = rowBuffer->AllocateUnversioned(keyBound.Prefix.GetCount() + (shouldAddMax ? 1 : 0));
    memcpy(row.Begin(), keyBound.Prefix.Begin(), sizeof(TUnversionedValue) * keyBound.Prefix.GetCount());
    if (shouldAddMax) {
        row[keyBound.Prefix.GetCount()] = MakeUnversionedSentinelValue(EValueType::Max);
    }
    for (auto& value : row) {
        rowBuffer->Capture(&value);
    }

    return row;
}

TKeyBound ShortenKeyBound(TKeyBound keyBound, int length, const TRowBufferPtr& rowBuffer)
{
    if (keyBound.Prefix.GetCount() <= length) {
        // No need to change anything.
        return keyBound;
    }

    // If we do perform shortening, resulting key bound is going to be inclusive despite the original inclusiveness.

    return TKeyBound::FromRowUnchecked(
        rowBuffer->Capture(keyBound.Prefix.Begin(), length),
        /* isInclusive */ true,
        keyBound.IsUpper);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
