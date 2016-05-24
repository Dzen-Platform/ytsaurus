#ifndef ATTRIBUTE_HELPERS_INL_H_
#error "Direct inclusion of this file is not allowed, include attribute_helpers.h"
#endif

#include "attribute_consumer.h"
#include "convert.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

template <class T>
T IAttributeDictionary::Get(const Stroka& key) const
{
    auto yson = GetYson(key);
    return ConvertTo<T>(yson);
}

template <class T>
T IAttributeDictionary::Get(const Stroka& key, const T& defaultValue) const
{
    return Find<T>(key).Get(defaultValue);
}

template <class T>
typename TNullableTraits<T>::TNullableType IAttributeDictionary::Find(const Stroka& key) const
{
    auto yson = FindYson(key);
    if (!yson) {
        return typename TNullableTraits<T>::TNullableType();
    }
    return ConvertTo<T>(*yson);
}

template <class T>
void IAttributeDictionary::Set(const Stroka& key, const T& value)
{
    auto yson = ConvertToYsonString(value, NYson::EYsonFormat::Binary);
    SetYson(key, yson);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
