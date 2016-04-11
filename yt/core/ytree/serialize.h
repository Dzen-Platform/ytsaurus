#pragma once

#include "public.h"
#include "ephemeral_node_factory.h"

#include <yt/core/yson/producer.h>

#include <yt/core/misc/guid.h>
#include <yt/core/misc/mpl.h>
#include <yt/core/misc/nullable.h>
#include <yt/core/misc/small_vector.h>

#include <yt/core/yson/writer.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

template <class T>
NYson::EYsonType GetYsonType(const T&);
NYson::EYsonType GetYsonType(const NYson::TYsonString& yson);
NYson::EYsonType GetYsonType(const NYson::TYsonInput& input);
NYson::EYsonType GetYsonType(const NYson::TYsonProducer& producer);

////////////////////////////////////////////////////////////////////////////////

template <class T>
void WriteYson(
    TOutputStream* output,
    const T& value,
    NYson::EYsonType type,
    NYson::EYsonFormat format = NYson::EYsonFormat::Binary,
    int indent = 4);

template <class T>
void WriteYson(
    TOutputStream* output,
    const T& value,
    NYson::EYsonFormat format = NYson::EYsonFormat::Binary);

template <class T>
void WriteYson(
    const NYson::TYsonOutput& output,
    const T& value,
    NYson::EYsonFormat format = NYson::EYsonFormat::Binary);

////////////////////////////////////////////////////////////////////////////////

template <class T>
void Serialize(T* value, NYson::IYsonConsumer* consumer);

template <class T>
void Serialize(const TIntrusivePtr<T>& value, NYson::IYsonConsumer* consumer);

// integers
void Serialize(signed char value, NYson::IYsonConsumer* consumer);
void Serialize(unsigned char value, NYson::IYsonConsumer* consumer);
void Serialize(short value, NYson::IYsonConsumer* consumer);
void Serialize(unsigned short value, NYson::IYsonConsumer* consumer);
void Serialize(int value, NYson::IYsonConsumer* consumer);
void Serialize(unsigned value, NYson::IYsonConsumer* consumer);
void Serialize(long value, NYson::IYsonConsumer* consumer);
void Serialize(unsigned long value, NYson::IYsonConsumer* consumer);
void Serialize(long long value, NYson::IYsonConsumer* consumer);
void Serialize(unsigned long long value, NYson::IYsonConsumer* consumer);

// double
void Serialize(double value, NYson::IYsonConsumer* consumer);

// Stroka
void Serialize(const Stroka& value, NYson::IYsonConsumer* consumer);

// TStringBuf
void Serialize(const TStringBuf& value, NYson::IYsonConsumer* consumer);

// const char*
void Serialize(const char* value, NYson::IYsonConsumer* consumer);

// bool
void Serialize(bool value, NYson::IYsonConsumer* consumer);

// char
void Serialize(char value, NYson::IYsonConsumer* consumer);

// TDuration
void Serialize(TDuration value, NYson::IYsonConsumer* consumer);

// TInstant
void Serialize(TInstant value, NYson::IYsonConsumer* consumer);

// TGuid
void Serialize(const TGuid& value, NYson::IYsonConsumer* consumer);

// TInputStream
void Serialize(TInputStream& input, NYson::IYsonConsumer* consumer);

// Enums
template <class T>
typename std::enable_if<TEnumTraits<T>::IsEnum, void>::type Serialize(
    T value,
    NYson::IYsonConsumer* consumer);

// TNullable
template <class T>
void Serialize(const TNullable<T>& value, NYson::IYsonConsumer* consumer);

// std::vector
template <class T>
void Serialize(const std::vector<T>& value, NYson::IYsonConsumer* consumer);

// SmallVector
template <class T, unsigned N>
void Serialize(const SmallVector<T, N>& value, NYson::IYsonConsumer* consumer);

// std::set
template <class T>
void Serialize(const std::set<T>& value, NYson::IYsonConsumer* consumer);

// yhash_set
template <class T>
void Serialize(const yhash_set<T>& value, NYson::IYsonConsumer* consumer);

// std::map
template <class K, class V>
void Serialize(const std::map<K, V>& value, NYson::IYsonConsumer* consumer);

// yhash_map
template <class K, class V>
void Serialize(const yhash_map<K, V>& value, NYson::IYsonConsumer* consumer);

// TErrorOr
template <class T>
void Serialize(const TErrorOr<T>& error, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

template <class T>
void Deserialize(TIntrusivePtr<T>& value, INodePtr node);

template <class T>
void Deserialize(std::unique_ptr<T>& value, INodePtr node);

// integers
void Deserialize(signed char& value, INodePtr node);
void Deserialize(unsigned char& value, INodePtr node);
void Deserialize(short& value, INodePtr node);
void Deserialize(unsigned short& value, INodePtr node);
void Deserialize(int& value, INodePtr node);
void Deserialize(unsigned& value, INodePtr node);
void Deserialize(long& value, INodePtr node);
void Deserialize(unsigned long& value, INodePtr node);
void Deserialize(long long& value, INodePtr node);
void Deserialize(unsigned long long& value, INodePtr node);

// double
void Deserialize(double& value, INodePtr node);

// Stroka
void Deserialize(Stroka& value, INodePtr node);

// bool
void Deserialize(bool& value, INodePtr node);

// char
void Deserialize(char& value, INodePtr node);

// TDuration
void Deserialize(TDuration& value, INodePtr node);

// TInstant
void Deserialize(TInstant& value, INodePtr node);

// TGuid
void Deserialize(TGuid& value, INodePtr node);

// Enums
template <class T>
typename std::enable_if<TEnumTraits<T>::IsEnum, void>::type Deserialize(
    T& value,
    INodePtr node);

// TNullable
template <class T>
void Deserialize(TNullable<T>& value, INodePtr node);

// std::vector
template <class T>
void Deserialize(std::vector<T>& value, INodePtr node);

// SmallVector
template <class T, unsigned N>
void Deserialize(SmallVector<T, N>& value, INodePtr node);

// std::set
template <class T>
void Deserialize(std::set<T>& value, INodePtr node);

// yhash_set
template <class T>
void Deserialize(yhash_set<T>& value, INodePtr node);

// std::map
template <class K, class V>
void Deserialize(std::map<K, V>& value, INodePtr node);

// yhash_map
template <class K, class V>
void Deserialize(yhash_map<K, V>& value, INodePtr node);

// TErrorOr
template <class T>
void Deserialize(TErrorOr<T>& error, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

#define SERIALIZE_INL_H_
#include "serialize-inl.h"
#undef SERIALIZE_INL_H_
