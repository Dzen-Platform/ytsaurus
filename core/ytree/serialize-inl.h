#pragma once
#ifndef SERIALIZE_INL_H_
#error "Direct inclusion of this file is not allowed, include serialize.h"
#endif

#include "node.h"
#include "yson_serializable.h"

#include <yt/core/misc/nullable.h>
#include <yt/core/misc/string.h>
#include <yt/core/misc/error.h>
#include <yt/core/misc/collection_helpers.h>

#include <yt/core/yson/stream.h>
#include <yt/core/yson/string.h>
#include <yt/core/yson/protobuf_interop.h>

#include <numeric>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

namespace {

template <class T>
void SerializeVector(const T& items, NYson::IYsonConsumer* consumer)
{
    consumer->OnBeginList();
    for (const auto& item : items) {
        consumer->OnListItem();
        Serialize(item, consumer);
    }
    consumer->OnEndList();
}

template <class T>
void SerializeSet(const T& items, NYson::IYsonConsumer* consumer)
{
    consumer->OnBeginList();
    for (auto it : GetSortedIterators(items)) {
        consumer->OnListItem();
        Serialize(*it, consumer);
    }
    consumer->OnEndList();
}

template <class T, bool IsEnum = TEnumTraits<T>::IsEnum>
struct TMapKeyHelper;

template <class T>
struct TMapKeyHelper<T, true>
{
    static void Serialize(const T& value, NYson::IYsonConsumer* consumer)
    {
        consumer->OnKeyedItem(FormatEnum(value));
    }

    static void Deserialize(T& value, const TString& key)
    {
        value = ParseEnum<T>(key);
    }
};

template <class T>
struct TMapKeyHelper<T, false>
{
    static void Serialize(const T& value, NYson::IYsonConsumer* consumer)
    {
        consumer->OnKeyedItem(ToString(value));
    }

    static void Deserialize(T& value, const TString& key)
    {
        value = FromString<T>(key);
    }
};

template <class T>
void SerializeMap(const T& items, NYson::IYsonConsumer* consumer)
{
    consumer->OnBeginMap();
    for (auto it : GetSortedIterators(items)) {
        TMapKeyHelper<typename T::key_type>::Serialize(it->first, consumer);
        Serialize(it->second, consumer);
    }
    consumer->OnEndMap();
}

template <class T>
void DeserializeVector(T& value, INodePtr node)
{
    auto listNode = node->AsList();
    auto size = listNode->GetChildCount();
    value.resize(size);
    for (int i = 0; i < size; ++i) {
        Deserialize(value[i], listNode->GetChild(i));
    }
}

template <class T>
void DeserializeSet(T& value, INodePtr node)
{
    auto listNode = node->AsList();
    auto size = listNode->GetChildCount();
    for (int i = 0; i < size; ++i) {
        typename T::value_type item;
        Deserialize(item, listNode->GetChild(i));
        value.insert(std::move(item));
    }
}

template <class T>
void DeserializeMap(T& value, INodePtr node)
{
    auto mapNode = node->AsMap();
    value.clear();
    for (const auto& pair : mapNode->GetChildren()) {
        typename T::key_type key;
        TMapKeyHelper<typename T::key_type>::Deserialize(key, pair.first);
        typename T::mapped_type item;
        Deserialize(item, pair.second);
        value.emplace(std::move(key), std::move(item));
    }
}

template <class T, bool IsSet = std::is_same<typename T::key_type, typename T::value_type>::value>
struct TAssociativeHelper;

template <class T>
struct TAssociativeHelper<T, true>
{
    static void Serialize(const T& value, NYson::IYsonConsumer* consumer)
    {
        SerializeSet(value, consumer);
    }

    static void Deserialize(T& value, INodePtr consumer)
    {
        DeserializeSet(value, consumer);
    }
};

template <class T>
struct TAssociativeHelper<T, false>
{
    static void Serialize(const T& value, NYson::IYsonConsumer* consumer)
    {
        SerializeMap(value, consumer);
    }

    static void Deserialize(T& value, INodePtr consumer)
    {
        DeserializeMap(value, consumer);
    }
};

template <class T>
void SerializeAssociative(const T& items, NYson::IYsonConsumer* consumer)
{
    TAssociativeHelper<T>::Serialize(items, consumer);
}

template <class T>
void DeserializeAssociative(T& value, INodePtr node)
{
    TAssociativeHelper<T>::Deserialize(value, node);
}

template <class T, size_t Size = std::tuple_size<T>::value>
struct TTupleHelper;

template <class T>
struct TTupleHelper<T, 0U>
{
    static void SerializeItem(const T&, NYson::IYsonConsumer*) {}
    static void DeserializeItem(T&, IListNodePtr) {}
};

template <class T, size_t Size>
struct TTupleHelper
{
    static void SerializeItem(const T& value, NYson::IYsonConsumer* consumer)
    {
        TTupleHelper<T, Size - 1U>::SerializeItem(value, consumer);
        consumer->OnListItem();
        Serialize(std::get<Size - 1U>(value), consumer);
    }

    static void DeserializeItem(T& value, IListNodePtr list)
    {
        TTupleHelper<T, Size - 1U>::DeserializeItem(value, list);
        if (list->GetChildCount() >= Size) {
            Deserialize(std::get<Size - 1U>(value), list->GetChild(Size - 1U));
        }
    }
};

template <class T>
void SerializeTuple(const T& items, NYson::IYsonConsumer* consumer)
{
    consumer->OnBeginList();
    TTupleHelper<T>::SerializeItem(items, consumer);
    consumer->OnEndList();
}

template <class T>
void DeserializeTuple(T& value, INodePtr node)
{
    TTupleHelper<T>::DeserializeItem(value, node->AsList());
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

template <class T>
NYson::EYsonType GetYsonType(const T&)
{
    return NYson::EYsonType::Node;
}

template <class T>
void WriteYson(
    IOutputStream* output,
    const T& value,
    NYson::EYsonType type,
    NYson::EYsonFormat format,
    int indent)
{
    NYson::TYsonWriter writer(output, format, type, false, false, indent);
    Serialize(value, &writer);
}

template <class T>
void WriteYson(
    IOutputStream* output,
    const T& value,
    NYson::EYsonFormat format)
{
    WriteYson(output, value, GetYsonType(value), format);
}

template <class T>
void WriteYson(
    const NYson::TYsonOutput& output,
    const T& value,
    NYson::EYsonFormat format)
{
    WriteYson(output.GetStream(), value, output.GetType(), format);
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
void Serialize(T* value, NYson::IYsonConsumer* consumer)
{
    Y_ASSERT(value);
    Serialize(*value, consumer);
}

template <class T>
void Serialize(const TIntrusivePtr<T>& value, NYson::IYsonConsumer* consumer)
{
    Serialize(value.Get(), consumer);
}

// Enums
template <class T>
void Serialize(
    T value,
    NYson::IYsonConsumer* consumer,
    typename std::enable_if<TEnumTraits<T>::IsEnum, void>::type*)
{
    consumer->OnStringScalar(FormatEnum(value));
}

// TNullable
template <class T>
void Serialize(const TNullable<T>& value, NYson::IYsonConsumer* consumer)
{
    if (!value) {
        consumer->OnEntity();
    } else {
        Serialize(*value, consumer);
    }
}

// std::vector
template <class T, class A>
void Serialize(const std::vector<T, A>& items, NYson::IYsonConsumer* consumer)
{
    SerializeVector(items, consumer);
}

// SmallVector
template <class T, unsigned N>
void Serialize(const SmallVector<T, N>& items, NYson::IYsonConsumer* consumer)
{
    SerializeVector(items, consumer);
}

// RepeatedPtrField
template <class T>
void Serialize(const NProtoBuf::RepeatedPtrField<T>& items, NYson::IYsonConsumer* consumer)
{
    SerializeVector(items, consumer);
}

// RepeatedField
template <class T>
void Serialize(const NProtoBuf::RepeatedField<T>& items, NYson::IYsonConsumer* consumer)
{
    SerializeVector(items, consumer);
}

// TErrorOr
template <class T>
void Serialize(const TErrorOr<T>& error, NYson::IYsonConsumer* consumer)
{
    const TError& justError = error;
    if (error.IsOK()) {
        std::function<void(NYson::IYsonConsumer*)> valueProducer = [&error] (NYson::IYsonConsumer* consumer) {
            Serialize(error.Value(), consumer);
        };
        Serialize(justError, consumer, &valueProducer);
    } else {
        Serialize(justError, consumer);
    }
}

template <class F, class S>
void Serialize(const std::pair<F, S>& value, NYson::IYsonConsumer* consumer)
{
    SerializeTuple(value, consumer);
}

template <class T, size_t N>
void Serialize(const std::array<T, N>& value, NYson::IYsonConsumer* consumer)
{
    SerializeTuple(value, consumer);
}

template <class... T>
void Serialize(const std::tuple<T...>& value, NYson::IYsonConsumer* consumer)
{
    SerializeTuple(value, consumer);
}

// For any associative container.
template <template<typename...> class C, class... T, class K>
void Serialize(const C<T...>& value, NYson::IYsonConsumer* consumer)
{
    SerializeAssociative(value, consumer);
}

template <class T, class E, E Min, E Max>
void Serialize(const TEnumIndexedVector<T, E, Min, Max>& value, NYson::IYsonConsumer* consumer)
{
    consumer->OnBeginMap();
    for (auto key : TEnumTraits<E>::GetDomainValues()) {
        if (!value.IsDomainValue(key)) {
            continue;
        }
        consumer->OnKeyedItem(FormatEnum(key));
        Serialize(value[key], consumer);
    }
    consumer->OnEndMap();
}

void SerializeProtobufMessage(
    const google::protobuf::Message& message,
    const NYson::TProtobufMessageType* type,
    NYson::IYsonConsumer* consumer);

template <class T>
void Serialize(
    const T& message,
    NYson::IYsonConsumer* consumer,
    typename std::enable_if<std::is_convertible<T*, ::google::protobuf::Message*>::value, void>::type*)
{
    SerializeProtobufMessage(message, NYson::ReflectProtobufMessageType<T>(), consumer);
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
void Deserialize(TIntrusivePtr<T>& value, INodePtr node)
{
    if (!value) {
        value = New<T>();
    }
    Deserialize(*value, node);
}

template <class T>
void Deserialize(std::unique_ptr<T>& value, INodePtr node)
{
    if (!value) {
        value.reset(new T());
    }
    Deserialize(*value, node);
}

// Enums
template <class T>
void Deserialize(
    T& value,
    INodePtr node,
    typename std::enable_if<TEnumTraits<T>::IsEnum, void>::type*)
{
    auto stringValue = node->AsString()->GetValue();
    value = ParseEnum<T>(stringValue);
}

// TNullable
template <class T>
void Deserialize(TNullable<T>& value, INodePtr node)
{
    if (node->GetType() == ENodeType::Entity) {
        value = Null;
    } else {
        if (!value) {
            value = T();
        }
        Deserialize(*value, node);
    }
}

// std::vector
template <class T, class A>
void Deserialize(std::vector<T, A>& value, INodePtr node)
{
    DeserializeVector(value, node);
}

// SmallVector
template <class T, unsigned N>
void Deserialize(SmallVector<T, N>& value, INodePtr node)
{
    DeserializeVector(value, node);
}

// TErrorOr
template <class T>
void Deserialize(TErrorOr<T>& error, NYTree::INodePtr node)
{
    TError& justError = error;
    Deserialize(justError, node);
    if (error.IsOK()) {
        auto mapNode = node->AsMap();
        auto valueNode = mapNode->FindChild("value");
        if (valueNode) {
            Deserialize(error.Value(), std::move(valueNode));
        }
    }
}

template <class F, class S>
void Deserialize(std::pair<F, S>& value, INodePtr node)
{
    DeserializeTuple(value, node);
}

template <class T, size_t N>
void Deserialize(std::array<T, N>& value, INodePtr node)
{
    DeserializeTuple(value, node);
}

template <class... T>
void Deserialize(std::tuple<T...>& value, INodePtr node)
{
    DeserializeTuple(value, node);
}

// For any associative container.
template <template<typename...> class C, class... T, class K>
void Deserialize(C<T...>& value, INodePtr node)
{
    DeserializeAssociative(value, node);
}

template <class T, class E, E Min, E Max>
void Deserialize(TEnumIndexedVector<T, E, Min, Max>& value, INodePtr node)
{
    for (auto key : TEnumTraits<E>::GetDomainValues()) {
        if (!value.IsDomainValue(key)) {
            continue;
        }
        value[key] = T();
    }

    auto mapNode = node->AsMap();
    for (const auto& pair : mapNode->GetChildren()) {
        E key;
        if (!TEnumTraits<E>::FindValueByLiteral(pair.first, &key)) {
            continue;
        }
        if (!value.IsDomainValue(key)) {
            continue;
        }
        Deserialize(value[key], pair.second);
    }
}

void DeserializeProtobufMessage(
    google::protobuf::Message& message,
    const NYson::TProtobufMessageType* type,
    const INodePtr& node);

template <class T>
void Deserialize(
    T& message,
    const INodePtr& node,
    typename std::enable_if<std::is_convertible<T*, google::protobuf::Message*>::value, void>::type*)
{
    DeserializeProtobufMessage(message, NYson::ReflectProtobufMessageType<T>(), node);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
