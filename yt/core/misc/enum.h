#pragma once

#include "preprocessor.h"

#include <util/generic/strbuf.h>

#include <util/string/cast.h>

#include <stdexcept>
#include <type_traits>
#include <vector>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

/*
 * Smart enumerations augment C++ enum classes with a bunch of reflection
 * capabilities accessible via TEnumTraits class specialization.
 *
 * Please refer to the unit test for an actual example of usage
 * (unittests/enum_ut.cpp).
 */

template <
    class T,
    bool = std::is_enum<T>::value && !std::is_convertible<T, int>::value
>
struct TEnumTraits
{
    static constexpr bool IsEnum = false;
    static constexpr bool IsBitEnum = false;
};

template <class T>
struct TEnumTraits<T, true>
{
    using TImpl = decltype(GetEnumTraitsImpl(T()));
    using TType = T;
    using TUnderlying = typename TImpl::TUnderlying;

    static constexpr bool IsEnum = true;
    static constexpr bool IsBitEnum = TImpl::IsBitEnum;

    static TStringBuf GetTypeName();

    static const TStringBuf* FindLiteralByValue(TType value);
    static bool FindValueByLiteral(TStringBuf literal, TType* result);

    static constexpr int GetDomainSize();

    static const std::vector<TString>& GetDomainNames();
    static const std::vector<TType>& GetDomainValues();

    static TType FromString(TStringBuf str);
    static TString ToString(TType value);

    // For non-bit enums only.
    static constexpr TType GetMinValue();
    static constexpr TType GetMaxValue();

    // For bit enums only.
    static std::vector<TType> Decompose(TType value);

    // LLVM SmallDenseMap interop.
    // This should only be used for enums whose underlying type has big enough range
    // (see getEmptyKey and getTombstoneKey functions).
    struct TDenseMapInfo
    {
        static inline TType getEmptyKey()
        {
            return static_cast<TType>(-1);
        }

        static inline TType getTombstoneKey()
        {
            return static_cast<TType>(-2);
        }

        static unsigned getHashValue(const TType& key)
        {
            return static_cast<unsigned>(key) * 37U;
        }

        static bool isEqual(const TType& lhs, const TType& rhs)
        {
            return lhs == rhs;
        }
    };

};

////////////////////////////////////////////////////////////////////////////////

//! Defines a smart enumeration with a specific underlying type.
/*!
 * \param name Enumeration name.
 * \param seq Enumeration domain encoded as a <em>sequence</em>.
 * \param underlyingType Underlying type.
 */
#define DEFINE_ENUM_WITH_UNDERLYING_TYPE(name, underlyingType, seq) \
    ENUM__CLASS(name, underlyingType, seq) \
    ENUM__BEGIN_TRAITS(name, underlyingType, false, seq) \
    ENUM__MINMAX(name, seq) \
    ENUM__END_TRAITS(name)

//! Defines a smart enumeration with the default |int| underlying type.
#define DEFINE_ENUM(name, seq) \
    DEFINE_ENUM_WITH_UNDERLYING_TYPE(name, int, seq)

//! Defines a smart enumeration with a specific underlying type.
/*!
 * \param name Enumeration name.
 * \param seq Enumeration domain encoded as a <em>sequence</em>.
 * \param underlyingType Underlying type.
 */
#define DEFINE_BIT_ENUM_WITH_UNDERLYING_TYPE(name, underlyingType, seq) \
    ENUM__CLASS(name, underlyingType, seq) \
    ENUM__BEGIN_TRAITS(name, underlyingType, true, seq) \
    ENUM__DECOMPOSE(name, seq) \
    ENUM__END_TRAITS(name) \
    ENUM__BITWISE_OPS(name)

//! Defines a smart enumeration with the default |unsigned| underlying type.
/*!
 * \param name Enumeration name.
 * \param seq Enumeration domain encoded as a <em>sequence</em>.
 */
#define DEFINE_BIT_ENUM(name, seq) \
    DEFINE_BIT_ENUM_WITH_UNDERLYING_TYPE(name, unsigned, seq)

////////////////////////////////////////////////////////////////////////////////

//! A statically sized vector with elements of type |T| indexed by
//! the items of enumeration type |E|.
/*!
 *  Items are value-initialized on construction.
 */
template <
    class T,
    class E,
    E Min = TEnumTraits<E>::GetMinValue(),
    E Max = TEnumTraits<E>::GetMaxValue()
>
class TEnumIndexedVector
{
public:
    TEnumIndexedVector();
    TEnumIndexedVector(std::initializer_list<T> elements);

    T& operator[] (E index);
    const T& operator[] (E index) const;

    // STL interop.
    T* begin();
    const T* begin() const;
    T* end();
    const T* end() const;

    static bool IsDomainValue(E value);

private:
    using TUnderlying = typename TEnumTraits<E>::TUnderlying;
    static constexpr int N = static_cast<TUnderlying>(Max) - static_cast<TUnderlying>(Min) + 1;
    // TODO(babenko): change this to std::array after migrating to GCC 4.9
    // Cf. https://gcc.gnu.org/bugzilla/show_bug.cgi?id=59659
    std::vector<T> Items_;
};

////////////////////////////////////////////////////////////////////////////////

//! Returns |true| iff the enumeration value is not bitwise zero.
template <class E>
typename std::enable_if<NYT::TEnumTraits<E>::IsBitEnum, bool>::type
Any(E value);

//! Returns |true| iff the enumeration value is bitwise zero.
template <class E>
typename std::enable_if<NYT::TEnumTraits<E>::IsBitEnum, bool>::type
None(E value);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define ENUM_INL_H_
#include "enum-inl.h"
#undef ENUM_INL_H_
