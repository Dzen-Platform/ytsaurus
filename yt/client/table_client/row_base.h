#pragma once

#include "public.h"

#include <yt/core/misc/dense_map.h>
#include <yt/core/misc/optional.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM_WITH_UNDERLYING_TYPE(EValueType, ui8,
    ((Min)         (0x00))

    ((TheBottom)   (0x01))
    ((Null)        (0x02))

    ((Int64)       (0x03))
    ((Uint64)      (0x04))
    ((Double)      (0x05))
    ((Boolean)     (0x06))

    ((String)      (0x10))
    ((Any)         (0x11))

    ((Max)         (0xef))
);

static_assert(
    EValueType::Int64 < EValueType::Uint64 &&
    EValueType::Uint64 < EValueType::Double,
    "Incorrect type order.");

DEFINE_ENUM_WITH_UNDERLYING_TYPE(ESimpleLogicalValueType, ui32,
    ((Null)        (0x02))

    ((Int64)       (0x03))
    ((Uint64)      (0x04))
    ((Double)      (0x05))
    ((Boolean)     (0x06))

    ((String)      (0x10))
    ((Any)         (0x11))

    ((Int8)        (0x1000))
    ((Uint8)       (0x1001))

    ((Int16)       (0x1003))
    ((Uint16)      (0x1004))

    ((Int32)       (0x1005))
    ((Uint32)      (0x1006))

    ((Utf8)        (0x1007))

    ((Date)        (0x1008))
    ((Datetime)    (0x1009))
    ((Timestamp)   (0x100a))
    ((Interval)    (0x100b))

    ((Void)        (0x100c))
);

////////////////////////////////////////////////////////////////////////////////

inline constexpr EValueType GetPhysicalType(ESimpleLogicalValueType type)
{
    switch (type) {
        case ESimpleLogicalValueType::Null:
        case ESimpleLogicalValueType::Int64:
        case ESimpleLogicalValueType::Uint64:
        case ESimpleLogicalValueType::Double:
        case ESimpleLogicalValueType::Boolean:
        case ESimpleLogicalValueType::String:
        case ESimpleLogicalValueType::Any:
            return static_cast<EValueType>(type);

        case ESimpleLogicalValueType::Int8:
        case ESimpleLogicalValueType::Int16:
        case ESimpleLogicalValueType::Int32:
            return EValueType::Int64;

        case ESimpleLogicalValueType::Uint8:
        case ESimpleLogicalValueType::Uint16:
        case ESimpleLogicalValueType::Uint32:
            return EValueType::Uint64;

        case ESimpleLogicalValueType::Utf8:
            return EValueType::String;
        case ESimpleLogicalValueType::Date:
        case ESimpleLogicalValueType::Datetime:
        case ESimpleLogicalValueType::Timestamp:
            return EValueType::Uint64;
        case ESimpleLogicalValueType::Interval:
            return EValueType::Int64;

        case ESimpleLogicalValueType::Void:
            return EValueType::Null;
    }
    YT_ABORT();
}

inline constexpr ESimpleLogicalValueType GetLogicalType(EValueType type)
{
    switch (type) {
        case EValueType::Null:
        case EValueType::Int64:
        case EValueType::Uint64:
        case EValueType::Double:
        case EValueType::Boolean:
        case EValueType::String:
        case EValueType::Any:
            return static_cast<ESimpleLogicalValueType>(type);

        default:
            YT_ABORT();
    }
}

inline constexpr bool IsIntegralType(EValueType type)
{
    return type == EValueType::Int64 || type == EValueType::Uint64;
}

inline constexpr bool IsArithmeticType(EValueType type)
{
    return IsIntegralType(type) || type == EValueType::Double;
}

inline constexpr bool IsStringLikeType(EValueType type)
{
    return type == EValueType::String || type == EValueType::Any;
}

inline constexpr bool IsValueType(EValueType type)
{
    return
        type == EValueType::Int64 ||
        type == EValueType::Uint64 ||
        type == EValueType::Double ||
        type == EValueType::Boolean ||
        type == EValueType::String ||
        type == EValueType::Any;
}

inline constexpr bool IsSentinelType(EValueType type)
{
    return type == EValueType::Min || type == EValueType::Max;
}

////////////////////////////////////////////////////////////////////////////////

// This class contains ordered collection of indexes of columns of some table.
// Position in context of the class means position of some column index in ordered collection.
class TColumnFilter
{
public:
    using TIndexes = SmallVector<int, TypicalColumnCount>;

    TColumnFilter();
    TColumnFilter(const std::initializer_list<int>& indexes);
    explicit TColumnFilter(TIndexes&& indexes);
    explicit TColumnFilter(const std::vector<int>& indexes);
    explicit TColumnFilter(int schemaColumnCount);

    bool ContainsIndex(int columnIndex) const;
    int GetPosition(int columnIndex) const;
    std::optional<int> FindPosition(int columnIndex) const;
    const TIndexes& GetIndexes() const;
    bool IsUniversal() const;

private:
    bool IsUniversal_;
    TIndexes Indexes_;
};

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TColumnFilter& columnFilter);

////////////////////////////////////////////////////////////////////////////////

struct TTypeErasedRow
{
    const void* OpaqueHeader;

    explicit operator bool() const
    {
        return OpaqueHeader != nullptr;
    }
};

static_assert(std::is_pod<TTypeErasedRow>::value, "TTypeErasedRow must be POD.");

////////////////////////////////////////////////////////////////////////////////

//! Checks that #type is allowed to appear in data. Throws on failure.
void ValidateDataValueType(EValueType type);

//! Checks that #type is allowed to appear in keys. Throws on failure.
void ValidateKeyValueType(EValueType type);

//! Checks that #type is allowed to appear in schema. Throws on failure.
void ValidateSchemaValueType(EValueType type);

//! Checks that column filter contains indexes in range |[0, schemaColumnCount - 1]|.
void ValidateColumnFilter(const TColumnFilter& columnFilter, int schemaColumnCount);

////////////////////////////////////////////////////////////////////////////////

template <class TValue>
TValue MakeSentinelValue(EValueType type, int id = 0, bool aggregate = false)
{
    TValue result{};
    result.Id = id;
    result.Type = type;
    result.Aggregate = aggregate;
    return result;
}

template <class TValue>
TValue MakeNullValue(int id = 0, bool aggregate = false)
{
    TValue result{};
    result.Id = id;
    result.Type = EValueType::Null;
    result.Aggregate = aggregate;
    return result;
}

template <class TValue>
TValue MakeInt64Value(i64 value, int id = 0, bool aggregate = false)
{
    TValue result{};
    result.Id = id;
    result.Type = EValueType::Int64;
    result.Aggregate = aggregate;
    result.Data.Int64 = value;
    return result;
}

template <class TValue>
TValue MakeUint64Value(ui64 value, int id = 0, bool aggregate = false)
{
    TValue result{};
    result.Id = id;
    result.Type = EValueType::Uint64;
    result.Aggregate = aggregate;
    result.Data.Uint64 = value;
    return result;
}

template <class TValue>
TValue MakeDoubleValue(double value, int id = 0, bool aggregate = false)
{
    TValue result{};
    result.Id = id;
    result.Type = EValueType::Double;
    result.Aggregate = aggregate;
    result.Data.Double = value;
    return result;
}

template <class TValue>
TValue MakeBooleanValue(bool value, int id = 0, bool aggregate = false)
{
    TValue result{};
    result.Id = id;
    result.Type = EValueType::Boolean;
    result.Aggregate = aggregate;
    result.Data.Boolean = value;
    return result;
}

template <class TValue>
TValue MakeStringValue(TStringBuf value, int id = 0, bool aggregate = false)
{
    TValue result{};
    result.Id = id;
    result.Type = EValueType::String;
    result.Aggregate = aggregate;
    result.Length = value.length();
    result.Data.String = value.begin();
    return result;
}

template <class TValue>
TValue MakeAnyValue(TStringBuf value, int id = 0, bool aggregate = false)
{
    TValue result{};
    result.Id = id;
    result.Type = EValueType::Any;
    result.Aggregate = aggregate;
    result.Length = value.length();
    result.Data.String = value.begin();
    return result;
}

[[noreturn]] void ThrowUnexpectedValueType(EValueType valueType);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
