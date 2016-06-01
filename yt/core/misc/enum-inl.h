#ifndef ENUM_INL_H_
#error "Direct inclusion of this file is not allowed, include enum.h"
#endif

#include <yt/core/misc/mpl.h>

#include <util/string/printf.h>

#include <stdexcept>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

#define ENUM__CLASS(name, underlyingType, seq) \
    enum class name : underlyingType \
    { \
        PP_FOR_EACH(ENUM__DOMAIN_ITEM, seq) \
    };

#define ENUM__DOMAIN_ITEM(item) \
    PP_IF( \
        PP_IS_SEQUENCE(item), \
        ENUM__DOMAIN_ITEM_SEQ, \
        ENUM__DOMAIN_ITEM_ATOMIC \
    )(item)()

#define ENUM__DOMAIN_ITEM_ATOMIC(item) \
    item PP_COMMA

#define ENUM__DOMAIN_ITEM_SEQ(seq) \
    PP_ELEMENT(seq, 0) = PP_ELEMENT(seq, 1) PP_COMMA

////////////////////////////////////////////////////////////////////////////////

#define ENUM__BEGIN_TRAITS(name, underlyingType, isBit, seq) \
    struct TEnumTraitsImpl_##name \
    { \
        using TType = name; \
        using TUnderlying = underlyingType; \
        static constexpr bool IsBitEnum = isBit; \
        \
        static const TStringBuf& GetTypeName() \
        { \
            static const TStringBuf typeName = STRINGBUF(PP_STRINGIZE(name)); \
            return typeName; \
        } \
        \
        static const TStringBuf* FindLiteralByValue(TType value) \
        { \
            PP_FOR_EACH(ENUM__LITERAL_BY_VALUE_ITEM, seq) \
            return nullptr; \
        } \
        \
        static bool FindValueByLiteral(const TStringBuf& literal, TType* result) \
        { \
            PP_FOR_EACH(ENUM__VALUE_BY_LITERAL_ITEM, seq); \
            return false; \
        } \
        \
        static constexpr int GetDomainSize() \
        { \
            return PP_COUNT(seq); \
        } \
        \
        static const std::vector<Stroka>& GetDomainNames() \
        { \
            static const Stroka values[] = { \
                PP_FOR_EACH(ENUM__GET_DOMAIN_NAMES_ITEM, seq) \
            }; \
            static const std::vector<Stroka> result(values, values + GetDomainSize()); \
            return result; \
        } \
        \
        static const std::vector<TType>& GetDomainValues() \
        { \
            static const TType values[] = { \
                PP_FOR_EACH(ENUM__GET_DOMAIN_VALUES_ITEM, seq) \
            }; \
            static std::vector<TType> result(values, values + GetDomainSize()); \
            return result; \
        } \
        \
        static TType FromString(const TStringBuf& str) \
        { \
            TType value; \
            if (!FindValueByLiteral(str, &value)) { \
                throw std::runtime_error(~Sprintf("Error parsing %s value %s", \
                    PP_STRINGIZE(name), \
                    ~Stroka(str).Quote())); \
            } \
            return value; \
        }

#define ENUM__LITERAL_BY_VALUE_ITEM(item) \
    PP_IF( \
        PP_IS_SEQUENCE(item), \
        ENUM__LITERAL_BY_VALUE_ITEM_SEQ, \
        ENUM__LITERAL_BY_VALUE_ITEM_ATOMIC \
    )(item)

#define ENUM__LITERAL_BY_VALUE_ITEM_SEQ(seq) \
    ENUM__LITERAL_BY_VALUE_ITEM_ATOMIC(PP_ELEMENT(seq, 0))

#define ENUM__LITERAL_BY_VALUE_ITEM_ATOMIC(item) \
    if (static_cast<TUnderlying>(value) == static_cast<TUnderlying>(TType::item)) { \
        static const TStringBuf literal = STRINGBUF(PP_STRINGIZE(item)); \
        return &literal; \
    }

#define ENUM__VALUE_BY_LITERAL_ITEM(item) \
    PP_IF( \
        PP_IS_SEQUENCE(item), \
        ENUM__VALUE_BY_LITERAL_ITEM_SEQ, \
        ENUM__VALUE_BY_LITERAL_ITEM_ATOMIC \
    )(item)

#define ENUM__VALUE_BY_LITERAL_ITEM_SEQ(seq) \
    ENUM__VALUE_BY_LITERAL_ITEM_ATOMIC(PP_ELEMENT(seq, 0))

#define ENUM__VALUE_BY_LITERAL_ITEM_ATOMIC(item) \
    if (literal == PP_STRINGIZE(item)) { \
        *result = TType::item; \
        return true; \
    }

#define ENUM__GET_DOMAIN_VALUES_ITEM(item) \
    PP_IF( \
        PP_IS_SEQUENCE(item), \
        ENUM__GET_DOMAIN_VALUES_ITEM_SEQ, \
        ENUM__GET_DOMAIN_VALUES_ITEM_ATOMIC \
    )(item)

#define ENUM__GET_DOMAIN_VALUES_ITEM_SEQ(seq) \
    ENUM__GET_DOMAIN_VALUES_ITEM_ATOMIC(PP_ELEMENT(seq, 0))

#define ENUM__GET_DOMAIN_VALUES_ITEM_ATOMIC(item) \
    TType::item,

#define ENUM__GET_DOMAIN_NAMES_ITEM(item) \
    PP_IF( \
        PP_IS_SEQUENCE(item), \
        ENUM__GET_DOMAIN_NAMES_ITEM_SEQ, \
        ENUM__GET_DOMAIN_NAMES_ITEM_ATOMIC \
    )(item)

#define ENUM__GET_DOMAIN_NAMES_ITEM_SEQ(seq) \
    ENUM__GET_DOMAIN_NAMES_ITEM_ATOMIC(PP_ELEMENT(seq, 0))

#define ENUM__GET_DOMAIN_NAMES_ITEM_ATOMIC(item) \
    Stroka(PP_STRINGIZE(item)),

#define ENUM__DECOMPOSE(name, seq) \
    static std::vector<TType> Decompose(TType value) \
    { \
        std::vector<TType> result; \
        PP_FOR_EACH(ENUM__DECOMPOSE_ITEM, seq) \
        return result; \
    }

#define ENUM__DECOMPOSE_ITEM(item) \
    ENUM__DECOMPOSE_ITEM_SEQ(PP_ELEMENT(item, 0))

#define ENUM__DECOMPOSE_ITEM_SEQ(item) \
    if (static_cast<TUnderlying>(value) & static_cast<TUnderlying>(TType::item)) { \
        result.push_back(TType::item); \
    }

#define ENUM__MINMAX(name, seq) \
    ENUM__MINMAX_IMPL(name, seq, Min) \
    ENUM__MINMAX_IMPL(name, seq, Max)

#define ENUM__MINMAX_IMPL(name, seq, ext) \
    static constexpr TType Get##ext##Value() \
    { \
        return TType(NMpl::ext( \
            PP_FOR_EACH(ENUM__MINMAX_ITEM, seq) \
            ENUM__MINMAX_ITEM_CORE(PP_HEAD(seq)) \
        )); \
    }

#define ENUM__MINMAX_ITEM(item) \
    ENUM__MINMAX_ITEM_CORE(item),

#define ENUM__MINMAX_ITEM_CORE(item) \
    PP_IF( \
        PP_IS_SEQUENCE(item), \
        ENUM__MINMAX_ITEM_CORE_SEQ, \
        ENUM__MINMAX_ITEM_CORE_ATOMIC \
    )(item)

#define ENUM__MINMAX_ITEM_CORE_SEQ(seq) \
    ENUM__MINMAX_ITEM_CORE_ATOMIC(PP_ELEMENT(seq, 0))

#define ENUM__MINMAX_ITEM_CORE_ATOMIC(item) \
    static_cast<TUnderlying>(TType::item)

#define ENUM__END_TRAITS(name) \
    }; \
    \
    inline TEnumTraitsImpl_##name GetEnumTraitsImpl(name) \
    { \
        return TEnumTraitsImpl_##name(); \
    } \
    using ::ToString; \
    inline Stroka ToString(name value) \
    { \
        return ::NYT::TEnumTraits<name>::ToString(value); \
    }

////////////////////////////////////////////////////////////////////////////////

template <class T>
auto TEnumTraits<T, true>::Decompose(TType value) -> std::vector<TType>
{
    return TImpl::Decompose(value);
}

template <class T>
auto TEnumTraits<T, true>::FromString(const TStringBuf& str) -> TType
{
    return TImpl::FromString(str);
}

template <class T>
Stroka TEnumTraits<T, true>::ToString(TType value)
{
    Stroka result;
    const auto* literal = FindLiteralByValue(value);
    if (literal) {
        result = *literal;
    } else {
        result = GetTypeName();
        result += "(";
        result += ::ToString(static_cast<TUnderlying>(value));
        result += ")";
    }
    return result;
}

template <class T>
auto TEnumTraits<T, true>::GetDomainValues() -> const std::vector<TType>&
{
    return TImpl::GetDomainValues();
}

template <class T>
auto TEnumTraits<T, true>::GetDomainNames() -> const std::vector<Stroka>&
{
    return TImpl::GetDomainNames();
}

template <class T>
constexpr auto TEnumTraits<T, true>::GetMaxValue() -> TType
{
    return TImpl::GetMaxValue();
}

template <class T>
constexpr auto TEnumTraits<T, true>::GetMinValue() -> TType
{
    return TImpl::GetMinValue();
}

template <class T>
constexpr int TEnumTraits<T, true>::GetDomainSize()
{
    return TImpl::GetDomainSize();
}

template <class T>
bool TEnumTraits<T, true>::FindValueByLiteral(const TStringBuf& literal, TType* result)
{
    return TImpl::FindValueByLiteral(literal, result);
}

template <class T>
const TStringBuf* TEnumTraits<T, true>::FindLiteralByValue(TType value)
{
    return TImpl::FindLiteralByValue(value);
}

template <class T>
const TStringBuf& TEnumTraits<T, true>::GetTypeName()
{
    return TImpl::GetTypeName();
}

////////////////////////////////////////////////////////////////////////////////

template <class T, class E, E Min, E Max>
TEnumIndexedVector<T, E, Min, Max>::TEnumIndexedVector()
    : Items_(N)
{ }

template <class T, class E, E Min, E Max>
TEnumIndexedVector<T, E, Min, Max>::TEnumIndexedVector(std::initializer_list<T> elements)
    : Items_(N)
{
    Y_ASSERT(std::distance(elements.begin(), elements.end()) <= N);
    int index = 0;
    for (const auto& element : elements) {
        Items_[index++] = element;
    }
}

template <class T, class E, E Min, E Max>
T& TEnumIndexedVector<T, E, Min, Max>::operator[] (E index)
{
    Y_ASSERT(index >= Min && index <= Max);
    return Items_[static_cast<TUnderlying>(index) - static_cast<TUnderlying>(Min)];
}

template <class T, class E, E Min, E Max>
const T& TEnumIndexedVector<T, E, Min, Max>::operator[] (E index) const
{
    return const_cast<TEnumIndexedVector&>(*this)[index];
}

template <class T, class E, E Min, E Max>
T* TEnumIndexedVector<T, E, Min, Max>::begin()
{
    return Items_.data();
}

template <class T, class E, E Min, E Max>
const T* TEnumIndexedVector<T, E, Min, Max>::begin() const
{
    return Items_.data();
}

template <class T, class E, E Min, E Max>
T* TEnumIndexedVector<T, E, Min, Max>::end()
{
    return begin() + N;
}

template <class T, class E, E Min, E Max>
const T* TEnumIndexedVector<T, E, Min, Max>::end() const
{
    return begin() + N;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

////////////////////////////////////////////////////////////////////////////////

#define ENUM__BINARY_BITWISE_OPERATOR(assignOp, op) \
template <class T, class = typename std::enable_if<NYT::TEnumTraits<T>::IsBitEnum>::type> \
constexpr T operator op (T lhs, T rhs) \
{ \
    using TUnderlying = typename NYT::TEnumTraits<T>::TUnderlying; \
    return T(static_cast<TUnderlying>(lhs) op static_cast<TUnderlying>(rhs)); \
} \
\
template <class T, class = typename std::enable_if<NYT::TEnumTraits<T>::IsBitEnum>::type> \
T& operator assignOp (T& lhs, T rhs) \
{ \
    using TUnderlying = typename NYT::TEnumTraits<T>::TUnderlying; \
    lhs = T(static_cast<TUnderlying>(lhs) op static_cast<TUnderlying>(rhs)); \
    return lhs; \
}

#define ENUM__UNARY_BITWISE_OPERATOR(op) \
template <class T, class = typename std::enable_if<NYT::TEnumTraits<T>::IsBitEnum>::type> \
constexpr T operator op (T value) \
{ \
    using TUnderlying = typename NYT::TEnumTraits<T>::TUnderlying; \
    return T(op static_cast<TUnderlying>(value)); \
}

#define ENUM__BIT_SHIFT_OPERATOR(assignOp, op) \
template <class T, class = typename std::enable_if<NYT::TEnumTraits<T>::IsBitEnum>::type> \
constexpr T operator op (T lhs, size_t rhs) \
{ \
    using TUnderlying = typename NYT::TEnumTraits<T>::TUnderlying; \
    return T(static_cast<TUnderlying>(lhs) op rhs); \
} \
\
template <class T, class = typename std::enable_if<NYT::TEnumTraits<T>::IsBitEnum>::type> \
T& operator assignOp (T& lhs, size_t rhs) \
{ \
    using TUnderlying = typename NYT::TEnumTraits<T>::TUnderlying; \
    lhs = T(static_cast<TUnderlying>(lhs) op rhs); \
    return lhs; \
}

ENUM__BINARY_BITWISE_OPERATOR(&=, &)
ENUM__BINARY_BITWISE_OPERATOR(|=, | )
ENUM__BINARY_BITWISE_OPERATOR(^=, ^)

ENUM__UNARY_BITWISE_OPERATOR(~) \

ENUM__BIT_SHIFT_OPERATOR(<<=, << )
ENUM__BIT_SHIFT_OPERATOR(>>=, >> )

////////////////////////////////////////////////////////////////////////////////

template <class E>
typename std::enable_if<NYT::TEnumTraits<E>::IsBitEnum, bool>::type
Any(E value)
{
    return static_cast<typename NYT::TEnumTraits<E>::TUnderlying>(value) != 0;
}

template <class E>
typename std::enable_if<NYT::TEnumTraits<E>::IsBitEnum, bool>::type
None(E value)
{
    return static_cast<typename NYT::TEnumTraits<E>::TUnderlying>(value) == 0;
}
