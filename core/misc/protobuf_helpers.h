#pragma once

#include "guid.h"
#include "mpl.h"
#include "nullable.h"
#include "object_pool.h"
#include "range.h"
#include "ref.h"
#include "serialize.h"
#include "small_vector.h"

#include <yt/core/compression/public.h>

#include <yt/core/misc/guid.pb.h>
#include <yt/core/misc/protobuf_helpers.pb.h>

#include <contrib/libs/protobuf/message.h>
#include <contrib/libs/protobuf/repeated_field.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

#define DEFINE_TRIVIAL_PROTO_CONVERSIONS(type)                   \
    inline void ToProto(type* serialized, type original)         \
    {                                                            \
        *serialized = original;                                  \
    }                                                            \
                                                                 \
    inline void FromProto(type* original, type serialized)       \
    {                                                            \
        *original = serialized;                                  \
    }

DEFINE_TRIVIAL_PROTO_CONVERSIONS(Stroka)
DEFINE_TRIVIAL_PROTO_CONVERSIONS(i8)
DEFINE_TRIVIAL_PROTO_CONVERSIONS(ui8)
DEFINE_TRIVIAL_PROTO_CONVERSIONS(i16)
DEFINE_TRIVIAL_PROTO_CONVERSIONS(ui16)
DEFINE_TRIVIAL_PROTO_CONVERSIONS(i32)
DEFINE_TRIVIAL_PROTO_CONVERSIONS(ui32)
DEFINE_TRIVIAL_PROTO_CONVERSIONS(i64)
DEFINE_TRIVIAL_PROTO_CONVERSIONS(ui64)
DEFINE_TRIVIAL_PROTO_CONVERSIONS(bool)

#undef DEFINE_TRIVIAL_PROTO_CONVERSIONS

////////////////////////////////////////////////////////////////////////////////

inline void ToProto(::google::protobuf::int64* serialized, TDuration original)
{
    *serialized = original.MicroSeconds();
}

inline ::google::protobuf::int64 ToProto(TDuration original)
{
    return original.MicroSeconds();
}

inline void FromProto(TDuration* original, ::google::protobuf::int64 serialized)
{
    *original = TDuration::MicroSeconds(serialized);
}

////////////////////////////////////////////////////////////////////////////////

inline void ToProto(::google::protobuf::int64* serialized, TInstant original)
{
    *serialized = original.MicroSeconds();
}

inline ::google::protobuf::int64 ToProto(TInstant original)
{
    return original.MicroSeconds();
}

inline void FromProto(TInstant* original, ::google::protobuf::int64 serialized)
{
    *original = TInstant::MicroSeconds(serialized);
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
typename std::enable_if<NMpl::TIsConvertible<T*, ::google::protobuf::MessageLite*>::Value, void>::type ToProto(
    T* serialized,
    const T& original)
{
    *serialized = original;
}

template <class T>
typename std::enable_if<NMpl::TIsConvertible<T*, ::google::protobuf::MessageLite*>::Value, void>::type FromProto(
    T* original,
    const T& serialized)
{
    *original = serialized;
}

template <class T>
typename std::enable_if<TEnumTraits<T>::IsEnum, void>::type ToProto(
    int* serialized,
    T original)
{
    *serialized = static_cast<int>(original);
}

template <class T>
typename std::enable_if<TEnumTraits<T>::IsEnum, void>::type FromProto(
    T* original,
    int serialized)
{
    *original = T(serialized);
}

////////////////////////////////////////////////////////////////////////////////

template <class TSerializedArray, class TOriginalArray>
void ToProtoArrayImpl(
    TSerializedArray* serializedArray,
    const TOriginalArray& originalArray)
{
    serializedArray->Clear();
    serializedArray->Reserve(serializedArray->size());
    for (const auto& item : originalArray) {
        ToProto(serializedArray->Add(), item);
    }
}

template <class TSerialized, class TOriginal>
void ToProto(
    ::google::protobuf::RepeatedPtrField<TSerialized>* serializedArray,
    const std::vector<TOriginal>& originalArray)
{
    ToProtoArrayImpl(serializedArray, originalArray);
}

template <class TSerialized, class TOriginal>
void ToProto(
    ::google::protobuf::RepeatedField<TSerialized>* serializedArray,
    const std::vector<TOriginal>& originalArray)
{
    ToProtoArrayImpl(serializedArray, originalArray);
}

template <class TSerialized, class TOriginal>
void ToProto(
    ::google::protobuf::RepeatedPtrField<TSerialized>* serializedArray,
    const SmallVectorImpl<TOriginal>& originalArray)
{
    ToProtoArrayImpl(serializedArray, originalArray);
}

template <class TSerialized, class TOriginal>
void ToProto(
    ::google::protobuf::RepeatedField<TSerialized>* serializedArray,
    const SmallVectorImpl<TOriginal>& originalArray)
{
    ToProtoArrayImpl(serializedArray, originalArray);
}

template <class TSerialized, class TOriginal>
void ToProto(
    ::google::protobuf::RepeatedPtrField<TSerialized>* serializedArray,
    const TRange<TOriginal>& originalArray)
{
    ToProtoArrayImpl(serializedArray, originalArray);
}

template <class TSerialized, class TOriginal>
void ToProto(
    ::google::protobuf::RepeatedField<TSerialized>* serializedArray,
    const TRange<TOriginal>& originalArray)
{
    ToProtoArrayImpl(serializedArray, originalArray);
}

template <class TOriginalArray, class TSerializedArray>
void FromProtoArrayImpl(
    TOriginalArray* originalArray,
    const TSerializedArray& serializedArray)
{
    originalArray->clear();
    originalArray->resize(serializedArray.size());
    for (int i = 0; i < serializedArray.size(); ++i) {
        FromProto(&(*originalArray)[i], serializedArray.Get(i));
    }
}

template <class TOriginalArray, class TSerialized>
void FromProto(
    TOriginalArray* originalArray,
    const ::google::protobuf::RepeatedPtrField<TSerialized>& serializedArray)
{
    FromProtoArrayImpl(originalArray, serializedArray);
}

template <class TOriginalArray, class TSerialized>
void FromProto(
    TOriginalArray* originalArray,
    const ::google::protobuf::RepeatedField<TSerialized>& serializedArray)
{
    FromProtoArrayImpl(originalArray, serializedArray);
}

////////////////////////////////////////////////////////////////////////////////

template <class TSerialized, class TOriginal, class... TArgs>
TSerialized ToProto(const TOriginal& original, TArgs&&... args)
{
    TSerialized serialized;
    ToProto(&serialized, original, std::forward<TArgs>(args)...);
    return serialized;
}

template <class TOriginal, class TSerialized, class... TArgs>
TOriginal FromProto(const TSerialized& serialized, TArgs&&... args)
{
    TOriginal original;
    FromProto(&original, serialized, std::forward<TArgs>(args)...);
    return original;
}

////////////////////////////////////////////////////////////////////////////////

//! Serializes a protobuf message.
//! Returns |true| iff everything went well.
bool TrySerializeToProto(
    const google::protobuf::MessageLite& message,
    TSharedMutableRef* data,
    bool partial = true);

//! Serializes a protobuf message.
//! Fails on error.
TSharedRef SerializeToProto(
    const google::protobuf::MessageLite& message,
    bool partial = true);

//! Deserializes a chunk of memory into a protobuf message.
//! Returns |true| iff everything went well.
bool TryDeserializeFromProto(
    google::protobuf::MessageLite* message,
    const TRef& data);

//! Deserializes a chunk of memory into a protobuf message.
//! Fails on error.
void DeserializeFromProto(
    google::protobuf::MessageLite* message,
    const TRef& data);

//! Serializes a given protobuf message and wraps it with envelope.
//! Optionally compresses the serialized message.
//! Returns |true| iff everything went well.
bool TrySerializeToProtoWithEnvelope(
    const google::protobuf::MessageLite& message,
    TSharedMutableRef* data,
    NCompression::ECodec codecId = NCompression::ECodec::None,
    bool partial = true);

//! Serializes a given protobuf message and wraps it with envelope.
//! Optionally compresses the serialized message.
//! Fails on error.
TSharedRef SerializeToProtoWithEnvelope(
    const google::protobuf::MessageLite& message,
    NCompression::ECodec codecId = NCompression::ECodec::None,
    bool partial = true);

//! Unwraps a chunk of memory obtained from #TrySerializeToProtoWithEnvelope
//! and deserializes it into a protobuf message.
//! Returns |true| iff everything went well.
bool TryDeserializeFromProtoWithEnvelope(
    google::protobuf::MessageLite* message,
    const TRef& data);

//! Unwraps a chunk of memory obtained from #TrySerializeToProtoWithEnvelope
//! and deserializes it into a protobuf message.
//! Fails on error.
void DeserializeFromProtoWithEnvelope(
    google::protobuf::MessageLite* message,
    const TRef& data);

////////////////////////////////////////////////////////////////////////////////

struct TBinaryProtoSerializer
{
    //! Serializes a given protobuf message into a given stream.
    //! Throws an exception in case of error.
    static void Save(TStreamSaveContext& context, const ::google::protobuf::Message& message);

    //! Reads from a given stream protobuf message.
    //! Throws an exception in case of error.
    static void Load(TStreamLoadContext& context, ::google::protobuf::Message& message);
};

template <class T, class C>
struct TSerializerTraits<
    T,
    C,
    typename NMpl::TEnableIf<NMpl::TIsConvertible<T&, ::google::protobuf::Message&>>::TType>
{
    typedef TBinaryProtoSerializer TSerializer;
};

////////////////////////////////////////////////////////////////////////////////

/*
 *  YT Extension Set is a collection of |(tag, data)| pairs.
 *
 *  Here |tag| is a unique integer identifier and |data| is a protobuf-serialized
 *  embedded message.
 *
 *  In contrast to native Protobuf Extensions, ours are deserialized on-demand.
 */

//! Used to obtain an integer tag for a given type.
/*!
 *  Specialized versions of this traits are generated with |DECLARE_PROTO_EXTENSION|.
 */
template <class T>
struct TProtoExtensionTag;

#define DECLARE_PROTO_EXTENSION(type, tag) \
    template <> \
    struct TProtoExtensionTag<type> \
        : NMpl::TIntegralConstant<i32, tag> \
    { };

//! Finds and deserializes an extension of the given type. Fails if no matching
//! extension is found.
template <class T>
T GetProtoExtension(const NProto::TExtensionSet& extensions);

// Returns |true| iff an extension of a given type is present.
template <class T>
bool HasProtoExtension(const NProto::TExtensionSet& extensions);

//! Finds and deserializes an extension of the given type. Returns |Null| if no matching
//! extension is found.
template <class T>
TNullable<T> FindProtoExtension(const NProto::TExtensionSet& extensions);

//! Serializes and stores an extension.
//! Overwrites any extension with the same tag (if exists).
template <class T>
void SetProtoExtension(NProto::TExtensionSet* extensions, const T& value);

//! Tries to remove the extension.
//! Returns |true| iff the proper extension is found.
template <class T>
bool RemoveProtoExtension(NProto::TExtensionSet* extensions);

void FilterProtoExtensions(
    NProto::TExtensionSet* target,
    const NProto::TExtensionSet& source,
    const yhash_set<int>& tags);

////////////////////////////////////////////////////////////////////////////////

//! Wrapper that makes proto message refcounted.
template <class TProto>
class TRefCountedProto
    : public TIntrinsicRefCounted
    , public TProto
{
public:
    TRefCountedProto() = default;

    TRefCountedProto(const TRefCountedProto<TProto>& other)
    {
        TProto::CopyFrom(other);
    }

    TRefCountedProto(TRefCountedProto<TProto>&& other)
    {
        TProto::Swap(&other);
    }

    explicit TRefCountedProto(const TProto& other)
    {
        TProto::CopyFrom(other);
    }

    explicit TRefCountedProto(TProto&& other)
    {
        TProto::Swap(&other);
    }

    template <class T>
    friend size_t SpaceUsed(const TIntrusivePtr<TRefCountedProto<T>>& p);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define PROTOBUF_HELPERS_INL_H_
#include "protobuf_helpers-inl.h"
#undef PROTOBUF_HELPERS_INL_H_

