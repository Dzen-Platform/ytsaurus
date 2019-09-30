#include "validate_logical_type.h"
#include "logical_type.h"

#include <yt/core/yson/pull_parser.h>

#include <util/stream/mem.h>
#include <util/generic/adaptor.h>

namespace NYT::NTableClient {

using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

namespace {

////////////////////////////////////////////////////////////////////////////////

template <EValueType physicalType>
Y_FORCE_INLINE constexpr EYsonItemType ExpectedYsonItemType()
{
    if constexpr (physicalType == EValueType::Boolean) {
        return EYsonItemType::BooleanValue;
    } else if constexpr (physicalType == EValueType::Int64) {
        return EYsonItemType::Int64Value;
    } else if constexpr (physicalType == EValueType::Uint64) {
        return EYsonItemType::Uint64Value;
    } else if constexpr (physicalType == EValueType::Double) {
        return EYsonItemType::DoubleValue;
    } else if constexpr (physicalType == EValueType::String) {
        return EYsonItemType::StringValue;
    } else if constexpr (physicalType == EValueType::Null) {
        return EYsonItemType::EntityValue;
    } else {
        static_assert(physicalType == EValueType::Boolean, "Unexpected value type");
    }
}

////////////////////////////////////////////////////////////////////////////////

class TComplexLogicalTypeValidatorImpl
{
    class TFieldId;

public:
    TComplexLogicalTypeValidatorImpl(TYsonPullParser* parser, TComplexTypeFieldDescriptor descriptor)
        : Cursor_(parser)
        , RootDescriptor_(descriptor)
    { }

    void Validate()
    {
        return ValidateLogicalType(RootDescriptor_.GetType(), TFieldId());
    }

private:
    void ValidateLogicalType(const TLogicalTypePtr& type, const TFieldId& fieldId)
    {
        switch (type->GetMetatype()) {
            case ELogicalMetatype::Simple:
                ValidateSimpleType(type->UncheckedAsSimpleTypeRef().GetElement(), fieldId);
                return;
            case ELogicalMetatype::Optional:
                ValidateOptionalType(type->UncheckedAsOptionalTypeRef(), fieldId);
                return;
            case ELogicalMetatype::List:
                ValidateListType(type->UncheckedAsListTypeRef(), fieldId);
                return;
            case ELogicalMetatype::Struct:
                ValidateStructType(type->UncheckedAsStructTypeRef(), fieldId);
                return;
            case ELogicalMetatype::Tuple:
                ValidateTupleType(type->UncheckedAsTupleTypeRef(), fieldId);
                return;
            case ELogicalMetatype::VariantStruct:
                ValidateVariantStructType(type->UncheckedAsVariantStructTypeRef(), fieldId);
                return;
            case ELogicalMetatype::VariantTuple:
                ValidateVariantTupleType(type->UncheckedAsVariantTupleTypeRef(), fieldId);
                return;
            case ELogicalMetatype::Dict:
                ValidateDictType(type->UncheckedAsDictTypeRef(), fieldId);
                return;
            case ELogicalMetatype::Tagged:
                ValidateTaggedType(type->UncheckedAsTaggedTypeRef(), fieldId);
                return;
        }
        YT_ABORT();
    }

    void ThrowUnexpectedYsonToken(EYsonItemType type, const TFieldId& fieldId)
    {
        THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
            "Cannot parse %Qv; expected: %Qv found: %Qv",
            GetDescription(fieldId),
            type,
            Cursor_.GetCurrent().GetType());
    }

    Y_FORCE_INLINE void ValidateYsonTokenType(EYsonItemType type, const TFieldId& fieldId)
    {
        if (Cursor_.GetCurrent().GetType() != type) {
            ThrowUnexpectedYsonToken(type, fieldId);
        }
    }

    template <ESimpleLogicalValueType type>
    void ValidateSimpleType(const TFieldId& fieldId)
    {
        if constexpr (type == ESimpleLogicalValueType::Any) {
            switch (Cursor_.GetCurrent().GetType()) {
                case EYsonItemType::EntityValue:
                    THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                        "Cannot parse %Qv; unexpected entity value",
                        GetDescription(fieldId));
                case EYsonItemType::Int64Value:
                case EYsonItemType::BooleanValue:
                case EYsonItemType::Uint64Value:
                case EYsonItemType::DoubleValue:
                case EYsonItemType::StringValue:
                    Cursor_.Next();
                    return;
                case EYsonItemType::BeginAttributes:
                    THROW_ERROR_EXCEPTION(
                        EErrorCode::SchemaViolation,
                        "Cannot parse %Qv; unexpected top level attributes",
                        GetDescription(fieldId));

                case EYsonItemType::BeginList:
                case EYsonItemType::BeginMap: {
                    Cursor_.SkipComplexValue();
                    return;
                }
                default:
                    YT_ABORT();
            }
        } else {
            static_assert(type != ESimpleLogicalValueType::Any);
            constexpr auto expectedYsonEventType = ExpectedYsonItemType<GetPhysicalType(type)>();
            if (Cursor_.GetCurrent().GetType() != expectedYsonEventType) {
                THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                    "Cannot parse %Qv; expected: %Qv found: %Qv",
                    GetDescription(fieldId),
                    expectedYsonEventType,
                    Cursor_.GetCurrent().GetType());
            }

            if constexpr (expectedYsonEventType == EYsonItemType::EntityValue) {
                // nothing to check
            } else if constexpr (expectedYsonEventType == EYsonItemType::BooleanValue) {
                NTableClient::ValidateSimpleLogicalType<type>(Cursor_.GetCurrent().UncheckedAsBoolean());
            } else if constexpr (expectedYsonEventType == EYsonItemType::Int64Value) {
                NTableClient::ValidateSimpleLogicalType<type>(Cursor_.GetCurrent().UncheckedAsInt64());
            } else if constexpr (expectedYsonEventType == EYsonItemType::Uint64Value) {
                NTableClient::ValidateSimpleLogicalType<type>(Cursor_.GetCurrent().UncheckedAsUint64());
            } else if constexpr (expectedYsonEventType == EYsonItemType::DoubleValue) {
                NTableClient::ValidateSimpleLogicalType<type>(Cursor_.GetCurrent().UncheckedAsDouble());
            } else if constexpr (expectedYsonEventType == EYsonItemType::StringValue) {
                NTableClient::ValidateSimpleLogicalType<type>(Cursor_.GetCurrent().UncheckedAsString());
            } else {
                static_assert(expectedYsonEventType == EYsonItemType::EntityValue, "unexpected EYsonItemType");
            }
            Cursor_.Next();
        }
    }

    Y_FORCE_INLINE void ValidateSimpleType(ESimpleLogicalValueType type, const TFieldId& fieldId)
    {
        switch (type) {
#define CASE(x) \
            case x: \
                ValidateSimpleType<x>(fieldId); \
                return;
            CASE(ESimpleLogicalValueType::Null)
            CASE(ESimpleLogicalValueType::Int64)
            CASE(ESimpleLogicalValueType::Uint64)
            CASE(ESimpleLogicalValueType::Double)
            CASE(ESimpleLogicalValueType::Boolean)
            CASE(ESimpleLogicalValueType::String)
            CASE(ESimpleLogicalValueType::Any)
            CASE(ESimpleLogicalValueType::Int8)
            CASE(ESimpleLogicalValueType::Uint8)
            CASE(ESimpleLogicalValueType::Int16)
            CASE(ESimpleLogicalValueType::Uint16)
            CASE(ESimpleLogicalValueType::Int32)
            CASE(ESimpleLogicalValueType::Uint32)
            CASE(ESimpleLogicalValueType::Utf8)
            CASE(ESimpleLogicalValueType::Date)
            CASE(ESimpleLogicalValueType::Datetime)
            CASE(ESimpleLogicalValueType::Timestamp)
            CASE(ESimpleLogicalValueType::Interval)
#undef CASE
        }
        YT_ABORT();
    }

    void ValidateOptionalType(const TOptionalLogicalType& type, const TFieldId& fieldId)
    {
        if (Cursor_.GetCurrent().GetType() == EYsonItemType::EntityValue) {
            Cursor_.Next();
            return;
        }

        if (!type.IsElementNullable()) {
            ValidateLogicalType(type.GetElement(), fieldId.OptionalElement());
            return;
        }

        if (Cursor_.GetCurrent().GetType() != EYsonItemType::BeginList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::BeginList,
                Cursor_.GetCurrent().GetType());
        }
        Cursor_.Next();
        if (Cursor_.GetCurrent().GetType() == EYsonItemType::EndList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; empty yson",
                GetDescription(fieldId),
                Cursor_.GetCurrent().GetType());
        }
        ValidateLogicalType(type.GetElement(), fieldId.OptionalElement());
        if (Cursor_.GetCurrent().GetType() != EYsonItemType::EndList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::EndList,
                Cursor_.GetCurrent().GetType());
        }
        Cursor_.Next();
    }

    void ValidateListType(const TListLogicalType& type, const TFieldId& fieldId)
    {
        if (Cursor_.GetCurrent().GetType() != EYsonItemType::BeginList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::BeginList,
                Cursor_.GetCurrent().GetType());
        }
        Cursor_.Next();
        const auto& elementType = type.GetElement();
        auto elementFieldId = fieldId.ListElement();
        while (Cursor_.GetCurrent().GetType() != EYsonItemType::EndList) {
            ValidateLogicalType(elementType, elementFieldId);
        }
        Cursor_.Next();
    }

    void ValidateStructType(const TStructLogicalType& type, const TFieldId& fieldId)
    {
        if (Cursor_.GetCurrent().GetType() != EYsonItemType::BeginList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::BeginList,
                Cursor_.GetCurrent().GetType());
        }
        Cursor_.Next();
        const auto& fields = type.GetFields();
        for (size_t i = 0; i < fields.size(); ++i) {
            if (Cursor_.GetCurrent().GetType() == EYsonItemType::EndList) {
                do {
                    const auto& field = fields[i];
                    if (field.Type->GetMetatype() != ELogicalMetatype::Optional) {
                        THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                            "Cannot parse %Qv; struct ended before required field %Qv is set",
                            GetDescription(fieldId),
                            field.Name);
                    }
                    ++i;
                } while (i < fields.size());
                break;
            }
            const auto& field = fields[i];
            ValidateLogicalType(field.Type, fieldId.StructField(i));
        }
        if (Cursor_.GetCurrent().GetType() != EYsonItemType::EndList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::EndList,
                Cursor_.GetCurrent().GetType());
        }
        Cursor_.Next();
    }

    void ValidateTupleType(const TTupleLogicalType& type, const TFieldId& fieldId)
    {
        if (Cursor_.GetCurrent().GetType() != EYsonItemType::BeginList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::BeginList,
                Cursor_.GetCurrent().GetType());
        }
        Cursor_.Next();
        const auto& elements = type.GetElements();
        for (size_t i = 0; i < elements.size(); ++i) {
            if (Cursor_.GetCurrent().GetType() == EYsonItemType::EndList) {
                THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                    "Cannot parse %Qv; expected %Qv got %Qv",
                    GetDescription(fieldId),
                    GetDescription(fieldId.TupleElement(i)),
                    EYsonItemType::EndList);
            }
            ValidateLogicalType(elements[i], fieldId.TupleElement(i));
        }
        if (Cursor_.GetCurrent().GetType() != EYsonItemType::EndList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::EndList,
                Cursor_.GetCurrent().GetType());
        }
        Cursor_.Next();
    }

    template <typename T>
    Y_FORCE_INLINE void ValidateVariantTypeImpl(const T& type, const TFieldId& fieldId)
    {
        if (Cursor_.GetCurrent().GetType() != EYsonItemType::BeginList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::BeginList,
                Cursor_.GetCurrent().GetType());
        }
        Cursor_.Next();
        if (Cursor_.GetCurrent().GetType() != EYsonItemType::Int64Value) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::Int64Value,
                Cursor_.GetCurrent().GetType());
        }
        const auto alternativeIndex = Cursor_.GetCurrent().UncheckedAsInt64();
        Cursor_.Next();
        if constexpr (std::is_same_v<T, TVariantTupleLogicalType>) {
            const auto& elements = type.GetElements();
            if (alternativeIndex < 0) {
                THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                    "Cannot parse %Qv; variant alternative index %Qv is less than 0",
                    GetDescription(fieldId),
                    alternativeIndex);
            }
            if (alternativeIndex >= elements.size()) {
                THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                    "Cannot parse %Qv; variant alternative index %Qv exceeds number of variant elements %Qv",
                    GetDescription(fieldId),
                    alternativeIndex,
                    elements.size());
            }
            ValidateLogicalType(elements[alternativeIndex], fieldId.VariantTupleElement(alternativeIndex));
        } else {
            static_assert(std::is_same_v<T, TVariantStructLogicalType>);
            const auto& fields = type.GetFields();
            if (alternativeIndex < 0) {
                THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                    "Cannot parse %Qv; variant alternative index %Qv is less than 0",
                    GetDescription(fieldId),
                    alternativeIndex);
            }
            if (alternativeIndex >= fields.size()) {
                THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                    "Cannot parse %Qv; variant alternative index %Qv exceeds number of variant elements %Qv",
                    GetDescription(fieldId),
                    alternativeIndex,
                    fields.size());
            }
            ValidateLogicalType(fields[alternativeIndex].Type, fieldId.VariantStructField(alternativeIndex));
        }

        if (Cursor_.GetCurrent().GetType() != EYsonItemType::EndList) {
            THROW_ERROR_EXCEPTION(EErrorCode::SchemaViolation,
                "Cannot parse %Qv; expected: %Qv found: %Qv",
                GetDescription(fieldId),
                EYsonItemType::EndList,
                Cursor_.GetCurrent().GetType());
        }
        Cursor_.Next();
    }

    void ValidateVariantTupleType(const TVariantTupleLogicalType& type, const TFieldId& fieldId)
    {
        ValidateVariantTypeImpl(type, fieldId);
    }

    void ValidateVariantStructType(const TVariantStructLogicalType& type, const TFieldId& fieldId)
    {
        ValidateVariantTypeImpl(type, fieldId);
    }

    void ValidateDictType(const TDictLogicalType& type, const TFieldId& fieldId)
    {
        ValidateYsonTokenType(EYsonItemType::BeginList, fieldId);
        Cursor_.Next();
        while (Cursor_.GetCurrent().GetType() != EYsonItemType::EndList) {
            ValidateYsonTokenType(EYsonItemType::BeginList, fieldId);
            Cursor_.Next();

            ValidateLogicalType(type.GetKey(), fieldId.DictKey());
            ValidateLogicalType(type.GetValue(), fieldId.DictValue());

            ValidateYsonTokenType(EYsonItemType::EndList, fieldId);
            Cursor_.Next();
        }
        Cursor_.Next();
    }

    Y_FORCE_INLINE void ValidateTaggedType(const TTaggedLogicalType& type, const TFieldId& fieldId)
    {
        ValidateLogicalType(type.GetElement(), fieldId.TaggedElement());
    }

    TString GetDescription(const TFieldId& fieldId) const
    {
        return fieldId.GetDescriptor(RootDescriptor_).GetDescription();
    }

private:
    class TFieldId
    {
    public:
        // Root field id
        TFieldId() = default;

        TFieldId OptionalElement() const
        {
            return {this, 0};
        }

        TFieldId ListElement() const
        {
            return {this, 0};
        }

        TFieldId StructField(int i) const
        {
            return {this, i};
        }

        TFieldId TupleElement(int i) const
        {
            return {this, i};
        }

        TFieldId VariantStructField(int i) const
        {
            return {this, i};
        }

        TFieldId VariantTupleElement(int i) const
        {
            return {this, i};
        }

        TFieldId DictKey() const
        {
            return {this, 0};
        }

        TFieldId DictValue() const
        {
            return {this, 1};
        }

        TFieldId TaggedElement() const
        {
            return {this, 0};
        }

        TComplexTypeFieldDescriptor GetDescriptor(const TComplexTypeFieldDescriptor& root) const
        {
            std::vector<int> path;
            const auto* current = this;
            while (current->Parent_ != nullptr) {
                path.push_back(current->SiblingIndex_);
                current = current->Parent_;
            }

            auto descriptor = root;
            for (const auto& childIndex : Reversed(path)) {
                const auto& type = descriptor.GetType();
                switch (type->GetMetatype()) {
                    case ELogicalMetatype::Simple:
                        return descriptor;
                    case ELogicalMetatype::Optional:
                        descriptor = descriptor.OptionalElement();
                        continue;
                    case ELogicalMetatype::List:
                        descriptor = descriptor.ListElement();
                        continue;
                    case ELogicalMetatype::Struct:
                        descriptor = descriptor.StructField(childIndex);
                        continue;
                    case ELogicalMetatype::Tuple:
                        descriptor = descriptor.TupleElement(childIndex);
                        continue;
                    case ELogicalMetatype::VariantStruct:
                        descriptor = descriptor.VariantStructField(childIndex);
                        continue;
                    case ELogicalMetatype::VariantTuple:
                        descriptor = descriptor.VariantTupleElement(childIndex);
                        continue;
                    case ELogicalMetatype::Dict:
                        switch (childIndex) {
                            case 0:
                                descriptor = descriptor.DictKey();
                                continue;
                            case 1:
                                descriptor = descriptor.DictValue();
                                continue;
                        }
                        break;
                    case ELogicalMetatype::Tagged:
                        descriptor = descriptor.TaggedElement();
                        continue;
                }
                YT_ABORT();
            }
            return descriptor;
        }

    private:
        TFieldId(const TFieldId* parent, int siblingIndex)
            : Parent_(parent)
            , SiblingIndex_(siblingIndex)
        { }

    private:
        const TFieldId* Parent_ = nullptr;
        int SiblingIndex_ = 0;
    };

private:
    TYsonPullParserCursor Cursor_;
    const TComplexTypeFieldDescriptor RootDescriptor_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

void ValidateComplexLogicalType(TStringBuf ysonData, const TLogicalTypePtr& type)
{
    TMemoryInput in(ysonData);
    TYsonPullParser parser(&in, EYsonType::Node);
    TComplexLogicalTypeValidatorImpl validator(&parser, TComplexTypeFieldDescriptor(type));
    validator.Validate();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
