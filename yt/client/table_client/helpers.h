#pragma once

#include "versioned_row.h"
#include "unversioned_row.h"

#include <yt/core/actions/future.h>

#include <yt/core/net/public.h>

#include <yt/core/ytree/public.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

// Mostly used in unittests and for debugging purposes.
// Quite inefficient.
TUnversionedOwningRow YsonToSchemafulRow(
    const TString& yson,
    const TTableSchema& tableSchema,
    bool treatMissingAsNull);
TUnversionedOwningRow YsonToSchemalessRow(
    const TString& yson);
TVersionedRow YsonToVersionedRow(
    const TRowBufferPtr& rowBuffer,
    const TString& keyYson,
    const TString& valueYson,
    const std::vector<TTimestamp>& deleteTimestamps = std::vector<TTimestamp>(),
    const std::vector<TTimestamp>& extraWriteTimestamps = std::vector<TTimestamp>());
TUnversionedOwningRow YsonToKey(const TString& yson);
TString KeyToYson(TUnversionedRow row);

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TIsScalarPersistentType
{
    static constexpr bool Value =
        std::is_same<T, TGuid>::value ||
        std::is_same<T, TString>::value ||
        std::is_same<T, i64>::value ||
        std::is_same<T, ui64>::value ||
        std::is_same<T, TInstant>::value;
};

void ToUnversionedValue(TUnversionedValue* unversionedValue, TGuid value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(TGuid* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, const TString& value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(TString* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, TStringBuf value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(TStringBuf* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, bool value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(bool* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, const NYson::TYsonString& value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(NYson::TYsonString* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, i64 value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(i64* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, ui64 value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(ui64* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, i32 value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(i32* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, ui32 value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(ui32* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, i16 value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(i16* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, ui16 value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(ui16* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, i8 value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(i8* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, ui8 value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(ui8* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, double value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(double* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, TInstant value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(TInstant* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, const NYTree::IMapNodePtr& value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(NYTree::IMapNodePtr* value, TUnversionedValue unversionedValue);

void ToUnversionedValue(TUnversionedValue* unversionedValue, const NNet::TIP6Address& value, const TRowBufferPtr& rowBuffer, int id = 0);
void FromUnversionedValue(NNet::TIP6Address* value, TUnversionedValue unversionedValue);

template <class T>
void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    T value,
    const TRowBufferPtr& rowBuffer,
    int id = 0,
    typename std::enable_if<TEnumTraits<T>::IsEnum, void>::type* = nullptr);
template <class T>
void FromUnversionedValue(
    T* value,
    TUnversionedValue unversionedValue,
    typename std::enable_if<TEnumTraits<T>::IsEnum, void>::type* = nullptr);

template <class T>
TUnversionedValue ToUnversionedValue(
    T&& value,
    const TRowBufferPtr& rowBuffer,
    int id = 0);
template <class T>
T FromUnversionedValue(TUnversionedValue unversionedValue);

template <class T>
void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const T& value,
    const TRowBufferPtr& rowBuffer,
    int id = 0,
    typename std::enable_if<std::is_convertible<T*, ::google::protobuf::Message*>::value, void>::type* = nullptr);
template <class T>
void FromUnversionedValue(
    T* value,
    TUnversionedValue unversionedValue,
    typename std::enable_if<std::is_convertible<T*, ::google::protobuf::Message*>::value, void>::type* = nullptr);

template <class T>
void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const std::optional<T>& value,
    const TRowBufferPtr& rowBuffer,
    int id = 0);
template <class T>
void FromUnversionedValue(
    std::optional<T>* value,
    TUnversionedValue unversionedValue);

template <class T>
void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const std::vector<T>& values,
    const TRowBufferPtr& rowBuffer,
    int id = 0);
template <class T>
void FromUnversionedValue(
    std::vector<T>* values,
    TUnversionedValue unversionedValue,
    typename std::enable_if<std::is_convertible<T*, ::google::protobuf::Message*>::value, void>::type* = nullptr);
template <class T>
void FromUnversionedValue(
    std::vector<T>* values,
    TUnversionedValue unversionedValue,
    typename std::enable_if<TIsScalarPersistentType<T>::Value, void>::type* = nullptr);

template <class TKey, class TValue>
void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const THashMap<TKey, TValue>& map,
    const TRowBufferPtr& rowBuffer,
    int id = 0);
template <class TKey, class TValue>
void FromUnversionedValue(
    THashMap<TKey, TValue>* map,
    TUnversionedValue unversionedValue,
    typename std::enable_if<std::is_convertible<TValue*, ::google::protobuf::Message*>::value, void>::type* = nullptr);

template <class... Ts>
auto ToUnversionedValues(
    const TRowBufferPtr& rowBuffer,
    Ts&&... values)
-> std::array<TUnversionedValue, sizeof...(Ts)>;

template <class... Ts>
void FromUnversionedRow(
    TUnversionedRow row,
    Ts*... values);

////////////////////////////////////////////////////////////////////////////////

//! Constructs an owning row from arbitrarily-typed values.
//! Values get sequential ids 0..N-1.
template <class... Ts>
TUnversionedOwningRow MakeUnversionedOwningRow(Ts&&... values);

////////////////////////////////////////////////////////////////////////////////

void UnversionedValueToYson(TUnversionedValue unversionedValue, NYson::IYsonConsumer* consumer);
void UnversionedValueToYson(TUnversionedValue unversionedValue, NYson::TCheckedInDebugYsonTokenWriter* tokenWriter);
NYson::TYsonString UnversionedValueToYson(TUnversionedValue unversionedValue, bool enableRaw = false);

////////////////////////////////////////////////////////////////////////////////

template <class TReader, class TRow>
TFuture<void> AsyncReadRows(const TIntrusivePtr<TReader>& reader, std::vector<TRow>* rows);

////////////////////////////////////////////////////////////////////////////////

void ToAny(TRowBuffer* context, TUnversionedValue* result, TUnversionedValue* value);

////////////////////////////////////////////////////////////////////////////////

void PrintTo(const TOwningKey& key, ::std::ostream* os);
void PrintTo(const TUnversionedValue& value, ::std::ostream* os);
void PrintTo(const TUnversionedRow& value, ::std::ostream* os);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient

#define HELPERS_INL_H_
#include "helpers-inl.h"
#undef HELPERS_INL_H_
