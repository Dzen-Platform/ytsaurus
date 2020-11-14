#pragma once

#include "unversioned_row.h"
#include "key.h"

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

//! This class represents a (contextually) schemaful key bound. It defines
//! an open or closed ray in a space of all possible keys.
//! This is a CRTP base for common boilerplate code for owning and non-owning versions.
template <class TRow, class TKeyBound>
class TKeyBoundImpl
{
public:
    TRow Prefix;
    bool IsInclusive = false;
    bool IsUpper = false;

    //! Construct from a given row and validate that row does not contain
    //! setntinels of types Min, Max and Bottom.
    static TKeyBound FromRow(const TRow& row, bool isInclusive, bool isUpper);

    //! Same as previous but for rvalue refs.
    static TKeyBound FromRow(TRow&& row, bool isInclusive, bool isUpper);

    //! Construct from a given row without checking presence of types Min, Max and Bottom.
    //! NB: in debug mode value type check is still performed, but results in YT_ABORT().
    static TKeyBound FromRowUnchecked(const TRow& row, bool isInclusive, bool isUpper);

    //! Same as previous but for rvalue refs.
    static TKeyBound FromRowUnchecked(TRow&& row, bool isInclusive, bool isUpper);

    //! Return a key bound that allows any key.
    static TKeyBound MakeUniversal(bool isUpper);

    //! Return a key bound that does not allow any key.
    static TKeyBound MakeEmpty(bool isUpper);

    void FormatValue(TStringBuilderBase* builder) const;

    //! Test if this key bound allows any key.
    bool IsUniversal() const;

    //! Return key bound which is complementary to current.
    TKeyBound Invert() const;

    //! Return key bound with same prefix and direction but toggled inclusiveness.
    TKeyBound ToggleInclusiveness() const;

    //! Return key bound that is upper among {*this, this->Invert()}.
    TKeyBound UpperCounterpart() const;
    //! Return key bound that is lower among {*this, this->Invert()}.
    TKeyBound LowerCounterpart() const;

    void Persist(const TPersistenceContext& context);

private:
    static void ValidateValueTypes(const TRow& row);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

class TKeyBound
    : public NDetail::TKeyBoundImpl<TUnversionedRow, TKeyBound>
{ };

void FormatValue(TStringBuilderBase* builder, const TKeyBound& keyBound, TStringBuf format);
TString ToString(const TKeyBound& keyBound);

////////////////////////////////////////////////////////////////////////////////

class TOwningKeyBound
    : public NDetail::TKeyBoundImpl<TUnversionedOwningRow, TOwningKeyBound>
{
public:
    operator TKeyBound() const;
};

void FormatValue(TStringBuilderBase* builder, const TOwningKeyBound& keyBound, TStringBuf format);
TString ToString(const TOwningKeyBound& keyBound);

////////////////////////////////////////////////////////////////////////////////

bool operator ==(const TKeyBound& lhs, const TKeyBound& rhs);

////////////////////////////////////////////////////////////////////////////////

// Interop functions.

//! Convert legacy key bound expressed as a row possibly containing Min/Max to owning key bound.
//! NB: key length is needed to properly distinguish if K + [min] is an inclusive K or exclusive K.
TOwningKeyBound KeyBoundFromLegacyRow(TUnversionedRow row, bool isUpper, int keyLength);

//! Same as previous, but non-owning variant over row buffer.
TKeyBound KeyBoundFromLegacyRow(TUnversionedRow row, bool isUpper, int keyLength, const TRowBufferPtr& rowBuffer);

//! Convert key bound to legacy key bound.
TUnversionedOwningRow KeyBoundToLegacyRow(TKeyBound keyBound);

//! Same as previous, but non-owning variant over row buffer.
TUnversionedRow KeyBoundToLegacyRow(TKeyBound keyBound, const TRowBufferPtr& rowBuffer);

//! Build the most accurate key bound of length #length corresponding to the ray containing
//! ray corresponding to #keyBound.
TKeyBound ShortenKeyBound(TKeyBound keyBound, int length, const TRowBufferPtr& rowBuffer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
