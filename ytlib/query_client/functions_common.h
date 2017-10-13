#pragma once

#include "public.h"

#include <yt/ytlib/table_client/row_base.h>
#include <yt/core/misc/variant.h>

namespace NYT {
namespace NQueryClient {

using NTableClient::EValueType;

////////////////////////////////////////////////////////////////////////////////

using TTypeArgument = int;
using TUnionType = std::vector<EValueType>;
using TType = TVariant<EValueType, TTypeArgument, TUnionType>;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ECallingConvention,
    (Simple)
    (UnversionedValue)
);

DEFINE_ENUM(ETypeCategory,
    ((TypeArgument) (TType::TagOf<TTypeArgument>()))
    ((UnionType)    (TType::TagOf<TUnionType>()))
    ((ConcreteType) (TType::TagOf<EValueType>()))
);

////////////////////////////////////////////////////////////////////////////////

class TTypeSet
{
public:
    explicit TTypeSet(ui64 value = 0)
        : Value_(value)
    { }

    explicit TTypeSet(std::initializer_list<EValueType> values)
        : Value_(0)
    {
        Assign(values.begin(), values.end());
    }

    template <class TIterator>
    TTypeSet(TIterator begin, TIterator end)
        : Value_(0)
    {
        Assign(begin, end);
    }

    template <class TIterator>
    void Assign(TIterator begin, TIterator end)
    {
        Value_ = 0;
        for (; begin != end; ++begin) {
            Set(*begin);
        }
    }

    void Set(EValueType type)
    {
        Value_ |= 1 << ui8(type);
    }

    bool Get(EValueType type) const
    {
        return Value_ & (1 << ui8(type));
    }

    EValueType GetFront() const;

    bool IsEmpty() const
    {
        return Value_ == 0;
    }

    size_t GetSize() const;

    template <class TFunctor>
    void ForEach(TFunctor functor) const
    {
        ui64 mask = 1;
        for (size_t index = 0; index < 8 * sizeof(ui64); ++index, mask <<= 1) {
            if (Value_ & mask) {
                functor(EValueType(index));
            }
        }
    }

    friend TTypeSet operator | (const TTypeSet& lhs, const TTypeSet& rhs);
    friend TTypeSet operator & (const TTypeSet& lhs, const TTypeSet& rhs);

private:
    ui64 Value_ = 0;

};

TString ToString(const TTypeSet& typeSet);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
