#include "helpers.h"
#include "schema.h"
#include "name_table.h"

#include <yt/client/table_client/proto/chunk_meta.pb.h>

#include <yt/core/ytree/convert.h>

#include <yt/core/net/address.h>

#include <yt/core/yson/parser.h>
#include <yt/core/yson/protobuf_interop.h>
#include <yt/core/yson/token_writer.h>

#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

namespace NYT::NTableClient {

using namespace NYTree;
using namespace NYson;
using namespace NNet;

using namespace google::protobuf;
using namespace google::protobuf::io;

////////////////////////////////////////////////////////////////////////////////

namespace {

void YTreeNodeToUnversionedValue(TUnversionedOwningRowBuilder* builder, const INodePtr& value, int id, bool aggregate)
{
    switch (value->GetType()) {
        case ENodeType::Entity:
            builder->AddValue(MakeUnversionedSentinelValue(EValueType::Null, id, aggregate));
            break;
        case ENodeType::Int64:
            builder->AddValue(MakeUnversionedInt64Value(value->GetValue<i64>(), id, aggregate));
            break;
        case ENodeType::Uint64:
            builder->AddValue(MakeUnversionedUint64Value(value->GetValue<ui64>(), id, aggregate));
            break;
        case ENodeType::Double:
            builder->AddValue(MakeUnversionedDoubleValue(value->GetValue<double>(), id, aggregate));
            break;
        case ENodeType::String:
            builder->AddValue(MakeUnversionedStringValue(value->GetValue<TString>(), id, aggregate));
            break;
        default:
            builder->AddValue(MakeUnversionedAnyValue(ConvertToYsonString(value).GetData(), id, aggregate));
            break;
    }
}

} // namespace

TUnversionedOwningRow YsonToSchemafulRow(
    const TString& yson,
    const TTableSchema& tableSchema,
    bool treatMissingAsNull)
{
    auto nameTable = TNameTable::FromSchema(tableSchema);

    auto rowParts = ConvertTo<THashMap<TString, INodePtr>>(
        TYsonString(yson, EYsonType::MapFragment));

    TUnversionedOwningRowBuilder rowBuilder;
    auto addValue = [&] (int id, INodePtr value) {
        if (value->GetType() == ENodeType::Entity) {
            rowBuilder.AddValue(MakeUnversionedSentinelValue(
                value->Attributes().Get<EValueType>("type", EValueType::Null), id));
            return;
        }

        switch (tableSchema.Columns()[id].GetPhysicalType()) {
            case EValueType::Boolean:
                rowBuilder.AddValue(MakeUnversionedBooleanValue(value->GetValue<bool>(), id));
                break;
            case EValueType::Int64:
                rowBuilder.AddValue(MakeUnversionedInt64Value(value->GetValue<i64>(), id));
                break;
            case EValueType::Uint64:
                rowBuilder.AddValue(MakeUnversionedUint64Value(value->GetValue<ui64>(), id));
                break;
            case EValueType::Double:
                rowBuilder.AddValue(MakeUnversionedDoubleValue(value->GetValue<double>(), id));
                break;
            case EValueType::String:
                rowBuilder.AddValue(MakeUnversionedStringValue(value->GetValue<TString>(), id));
                break;
            case EValueType::Any:
                rowBuilder.AddValue(MakeUnversionedAnyValue(ConvertToYsonString(value).GetData(), id));
                break;
            case EValueType::Null:
            case EValueType::Composite:

            case EValueType::Min:
            case EValueType::Max:
            case EValueType::TheBottom:
                YT_ABORT();
        }
    };

    const auto& keyColumns = tableSchema.GetKeyColumns();

    // Key
    for (int id = 0; id < static_cast<int>(keyColumns.size()); ++id) {
        auto it = rowParts.find(nameTable->GetName(id));
        if (it == rowParts.end()) {
            rowBuilder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id));
        } else {
            addValue(id, it->second);
        }
    }

    // Fixed values
    for (int id = static_cast<int>(keyColumns.size()); id < static_cast<int>(tableSchema.Columns().size()); ++id) {
        auto it = rowParts.find(nameTable->GetName(id));
        if (it != rowParts.end()) {
            addValue(id, it->second);
        } else if (treatMissingAsNull) {
            rowBuilder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id));
        }
    }

    // Variable values
    for (const auto& pair : rowParts) {
        int id = nameTable->GetIdOrRegisterName(pair.first);
        if (id >= tableSchema.Columns().size()) {
            YTreeNodeToUnversionedValue(&rowBuilder, pair.second, id, false);
        }
    }

    return rowBuilder.FinishRow();
}

TUnversionedOwningRow YsonToSchemalessRow(const TString& valueYson)
{
    TUnversionedOwningRowBuilder builder;

    auto values = ConvertTo<std::vector<INodePtr>>(TYsonString(valueYson, EYsonType::ListFragment));
    for (const auto& value : values) {
        int id = value->Attributes().Get<int>("id");
        bool aggregate = value->Attributes().Get<bool>("aggregate", false);
        YTreeNodeToUnversionedValue(&builder, value, id, aggregate);
    }

    return builder.FinishRow();
}

void foo()
{
    MakeUnversionedOwningRow(AsStringBuf("hello"));
}

TVersionedRow YsonToVersionedRow(
    const TRowBufferPtr& rowBuffer,
    const TString& keyYson,
    const TString& valueYson,
    const std::vector<TTimestamp>& deleteTimestamps,
    const std::vector<TTimestamp>& extraWriteTimestamps)
{
    TVersionedRowBuilder builder(rowBuffer);

    auto keys = ConvertTo<std::vector<INodePtr>>(TYsonString(keyYson, EYsonType::ListFragment));

    for (auto key : keys) {
        int id = key->Attributes().Get<int>("id");
        switch (key->GetType()) {
            case ENodeType::Int64:
                builder.AddKey(MakeUnversionedInt64Value(key->GetValue<i64>(), id));
                break;
            case ENodeType::Uint64:
                builder.AddKey(MakeUnversionedUint64Value(key->GetValue<ui64>(), id));
                break;
            case ENodeType::Double:
                builder.AddKey(MakeUnversionedDoubleValue(key->GetValue<double>(), id));
                break;
            case ENodeType::String:
                builder.AddKey(MakeUnversionedStringValue(key->GetValue<TString>(), id));
                break;
            default:
                YT_ABORT();
                break;
        }
    }

    auto values = ConvertTo<std::vector<INodePtr>>(TYsonString(valueYson, EYsonType::ListFragment));
    for (auto value : values) {
        int id = value->Attributes().Get<int>("id");
        auto timestamp = value->Attributes().Get<TTimestamp>("ts");
        bool aggregate = value->Attributes().Get<bool>("aggregate", false);
        switch (value->GetType()) {
            case ENodeType::Entity:
                builder.AddValue(MakeVersionedSentinelValue(EValueType::Null, timestamp, id, aggregate));
                break;
            case ENodeType::Int64:
                builder.AddValue(MakeVersionedInt64Value(value->GetValue<i64>(), timestamp, id, aggregate));
                break;
            case ENodeType::Uint64:
                builder.AddValue(MakeVersionedUint64Value(value->GetValue<ui64>(), timestamp, id, aggregate));
                break;
            case ENodeType::Double:
                builder.AddValue(MakeVersionedDoubleValue(value->GetValue<double>(), timestamp, id, aggregate));
                break;
            case ENodeType::String:
                builder.AddValue(MakeVersionedStringValue(value->GetValue<TString>(), timestamp, id, aggregate));
                break;
            default:
                builder.AddValue(MakeVersionedAnyValue(ConvertToYsonString(value).GetData(), timestamp, id, aggregate));
                break;
        }
    }

    for (auto timestamp : deleteTimestamps) {
        builder.AddDeleteTimestamp(timestamp);
    }

    for (auto timestamp : extraWriteTimestamps) {
        builder.AddWriteTimestamp(timestamp);
    }

    return builder.FinishRow();
}

TUnversionedOwningRow YsonToKey(const TString& yson)
{
    TUnversionedOwningRowBuilder keyBuilder;
    auto keyParts = ConvertTo<std::vector<INodePtr>>(
        TYsonString(yson, EYsonType::ListFragment));

    for (int id = 0; id < keyParts.size(); ++id) {
        const auto& keyPart = keyParts[id];
        switch (keyPart->GetType()) {
            case ENodeType::Int64:
                keyBuilder.AddValue(MakeUnversionedInt64Value(
                    keyPart->GetValue<i64>(),
                    id));
                break;
            case ENodeType::Uint64:
                keyBuilder.AddValue(MakeUnversionedUint64Value(
                    keyPart->GetValue<ui64>(),
                    id));
                break;
            case ENodeType::Double:
                keyBuilder.AddValue(MakeUnversionedDoubleValue(
                    keyPart->GetValue<double>(),
                    id));
                break;
            case ENodeType::String:
                keyBuilder.AddValue(MakeUnversionedStringValue(
                    keyPart->GetValue<TString>(),
                    id));
                break;
            case ENodeType::Entity:
                keyBuilder.AddValue(MakeUnversionedSentinelValue(
                    keyPart->Attributes().Get<EValueType>("type", EValueType::Null),
                    id));
                break;
            default:
                keyBuilder.AddValue(MakeUnversionedAnyValue(
                    ConvertToYsonString(keyPart).GetData(),
                    id));
                break;
        }
    }

    return keyBuilder.FinishRow();
}

TString KeyToYson(TUnversionedRow row)
{
    return ConvertToYsonString(row, EYsonFormat::Text).GetData();
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, std::nullopt_t, const TRowBufferPtr& /*rowBuffer*/, int id)
{
    *unversionedValue = MakeUnversionedSentinelValue(EValueType::Null, id);
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, TGuid value, const TRowBufferPtr& rowBuffer, int id)
{
    auto strValue = ToString(value);
    *unversionedValue = value
        ? rowBuffer->Capture(MakeUnversionedStringValue(strValue, id))
        : MakeUnversionedSentinelValue(EValueType::Null);
}

void FromUnversionedValue(TGuid* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = TGuid();
        return;
    }
    if (unversionedValue.Type != EValueType::String) {
        THROW_ERROR_EXCEPTION("Cannot parse object id value from %Qlv",
            unversionedValue.Type);
    }
    *value = TGuid::FromString(TStringBuf(unversionedValue.Data.String, unversionedValue.Length));
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, const TString& value, const TRowBufferPtr& rowBuffer, int id)
{
    ToUnversionedValue(unversionedValue, static_cast<TStringBuf>(value), rowBuffer, id);
}

void FromUnversionedValue(TString* value, TUnversionedValue unversionedValue)
{
    TStringBuf uncapturedValue;
    FromUnversionedValue(&uncapturedValue, unversionedValue);
    *value = TString(uncapturedValue);
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, TStringBuf value, const TRowBufferPtr& rowBuffer, int id)
{
    *unversionedValue = rowBuffer->Capture(MakeUnversionedStringValue(value, id));
}

void FromUnversionedValue(TStringBuf* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = TStringBuf{};
        return;
    }
    if (unversionedValue.Type != EValueType::String) {
        THROW_ERROR_EXCEPTION("Cannot parse string value from %Qlv",
            unversionedValue.Type);
    }
    *value = TStringBuf(unversionedValue.Data.String, unversionedValue.Length);
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, const char* value, const TRowBufferPtr& rowBuffer, int id)
{
    ToUnversionedValue(unversionedValue, TStringBuf(value), rowBuffer, id);
}

void FromUnversionedValue(const char** value, TUnversionedValue unversionedValue)
{
    *value = FromUnversionedValue<TStringBuf>(unversionedValue).data();
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, bool value, const TRowBufferPtr& rowBuffer, int id)
{
    *unversionedValue = rowBuffer->Capture(MakeUnversionedBooleanValue(value, id));
}

void FromUnversionedValue(bool* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = false;
        return;
    }
    if (unversionedValue.Type != EValueType::Boolean) {
        THROW_ERROR_EXCEPTION("Cannot parse \"boolean\" value from %Qlv",
            unversionedValue.Type);
    }
    *value = unversionedValue.Data.Boolean;
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, const TYsonString& value, const TRowBufferPtr& rowBuffer, int id)
{
    YT_ASSERT(value.GetType() == EYsonType::Node);
    *unversionedValue = rowBuffer->Capture(MakeUnversionedAnyValue(value.GetData(), id));
}

void FromUnversionedValue(TYsonString* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse YSON string value from %Qlv",
            unversionedValue.Type);
    }
    *value = TYsonString(TString(unversionedValue.Data.String, unversionedValue.Length));
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, const NYson::TYsonStringBuf& value, const TRowBufferPtr& rowBuffer, int id)
{
    YT_ASSERT(value.GetType() == EYsonType::Node);
    *unversionedValue = rowBuffer->Capture(MakeUnversionedAnyValue(value.GetData(), id));
}

void FromUnversionedValue(NYson::TYsonStringBuf* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse YSON string value from %Qlv",
            unversionedValue.Type);
    }
    *value = TYsonStringBuf(TStringBuf(unversionedValue.Data.String, unversionedValue.Length));
}

////////////////////////////////////////////////////////////////////////////////

#define XX(cppType, codeType, humanReadableType) \
    void ToUnversionedValue(TUnversionedValue* unversionedValue, cppType value, const TRowBufferPtr& /*rowBuffer*/, int id) \
    { \
        *unversionedValue = MakeUnversioned ## codeType ## Value(value, id); \
    } \
    \
    void FromUnversionedValue(cppType* value, TUnversionedValue unversionedValue) \
    { \
        switch (unversionedValue.Type) { \
            case EValueType::Int64: \
                *value = CheckedIntegralCast<cppType>(unversionedValue.Data.Int64); \
                break; \
            case EValueType::Uint64: \
                *value = CheckedIntegralCast<cppType>(unversionedValue.Data.Uint64); \
                break; \
            default: \
                THROW_ERROR_EXCEPTION("Cannot parse \"" #humanReadableType "\" value from %Qlv", \
                    unversionedValue.Type); \
        } \
    }

XX(i64,  Int64,  int64)
XX(ui64, Uint64, uint64)
XX(i32,  Int64,  int32)
XX(ui32, Uint64, uint32)
XX(i16,  Int64,  int32)
XX(ui16, Uint64, uint16)
XX(i8,   Int64,  int8)
XX(ui8,  Uint64, uint8)

#undef XX

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, double value, const TRowBufferPtr& /*rowBuffer*/, int id)
{
    *unversionedValue = MakeUnversionedDoubleValue(value, id);
}

void FromUnversionedValue(double* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type != EValueType::Double) {
        THROW_ERROR_EXCEPTION("Cannot parse \"double\" value from %Qlv",
            unversionedValue.Type);
    }
    *value = unversionedValue.Data.Double;
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, TInstant value, const TRowBufferPtr& /*rowBuffer*/, int id)
{
    *unversionedValue = MakeUnversionedUint64Value(value.MicroSeconds(), id);
}

void FromUnversionedValue(TInstant* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type != EValueType::Uint64) {
        THROW_ERROR_EXCEPTION("Cannot parse instant from %Qlv",
            unversionedValue.Type);
    }
    *value = TInstant::MicroSeconds(unversionedValue.Data.Uint64);
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, TDuration value, const TRowBufferPtr& /*rowBuffer*/, int id)
{
    *unversionedValue = MakeUnversionedUint64Value(value.MicroSeconds(), id);
}

void FromUnversionedValue(TDuration* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type != EValueType::Uint64) {
        THROW_ERROR_EXCEPTION("Cannot parse duration from %Qlv",
            unversionedValue.Type);
    }
    *value = TDuration::MicroSeconds(unversionedValue.Data.Uint64);
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, const IMapNodePtr& value, const TRowBufferPtr& rowBuffer, int id)
{
    *unversionedValue = rowBuffer->Capture(MakeUnversionedAnyValue(ConvertToYsonString(value).GetData(), id));
}

void FromUnversionedValue(IMapNodePtr* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = nullptr;
    }
    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse YSON map from %Qlv",
            unversionedValue.Type);
    }
    *value = ConvertTo<IMapNodePtr>(TYsonString(unversionedValue.Data.String, unversionedValue.Length));
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(TUnversionedValue* unversionedValue, const TIP6Address& value, const TRowBufferPtr& rowBuffer, int id)
{
    ToUnversionedValue(unversionedValue, ToString(value), rowBuffer, id);
}

void FromUnversionedValue(TIP6Address* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = TIP6Address();
    }
    auto strValue = FromUnversionedValue<TString>(unversionedValue);
    *value = TIP6Address::FromString(strValue);
}

////////////////////////////////////////////////////////////////////////////////

void ProtobufToUnversionedValueImpl(
    TUnversionedValue* unversionedValue,
    const Message& value,
    const TProtobufMessageType* type,
    const TRowBufferPtr& rowBuffer,
    int id)
{
    auto byteSize = value.ByteSize();
    auto* pool = rowBuffer->GetPool();
    auto* wireBuffer = pool->AllocateUnaligned(byteSize);
    YT_VERIFY(value.SerializePartialToArray(wireBuffer, byteSize));
    ArrayInputStream inputStream(wireBuffer, byteSize);
    TString ysonBytes;
    TStringOutput outputStream(ysonBytes);
    TYsonWriter ysonWriter(&outputStream);
    ParseProtobuf(&ysonWriter, &inputStream, type);
    *unversionedValue = rowBuffer->Capture(MakeUnversionedAnyValue(ysonBytes, id));
}

////////////////////////////////////////////////////////////////////////////////

void UnversionedValueToProtobufImpl(
    Message* value,
    const TProtobufMessageType* type,
    TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        value->Clear();
        return;
    }
    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse a protobuf message from %Qlv",
            unversionedValue.Type);
    }
    TString wireBytes;
    StringOutputStream outputStream(&wireBytes);
    TProtobufWriterOptions options;
    options.UnknownYsonFieldsMode = EUnknownYsonFieldsMode::Keep;
    auto protobufWriter = CreateProtobufWriter(&outputStream, type, options);
    ParseYsonStringBuffer(
        TStringBuf(unversionedValue.Data.String, unversionedValue.Length),
        EYsonType::Node,
        protobufWriter.get());
    if (!value->ParseFromArray(wireBytes.data(), wireBytes.size())) {
        THROW_ERROR_EXCEPTION("Error parsing %v from wire bytes",
            value->GetTypeName());
    }
}

////////////////////////////////////////////////////////////////////////////////

void ListToUnversionedValueImpl(
    TUnversionedValue* unversionedValue,
    const std::function<bool(TUnversionedValue*)> producer,
    const TRowBufferPtr& rowBuffer,
    int id)
{
    TString ysonBytes;
    TStringOutput outputStream(ysonBytes);
    NYT::NYson::TYsonWriter writer(&outputStream);
    writer.OnBeginList();

    TUnversionedValue itemValue;
    while (true) {
        writer.OnListItem();
        if (!producer(&itemValue)) {
            break;
        }
        UnversionedValueToYson(itemValue, &writer);
    }
    writer.OnEndList();

    *unversionedValue = rowBuffer->Capture(MakeUnversionedAnyValue(ysonBytes, id));
}

void UnversionedValueToListImpl(
    std::function<google::protobuf::Message*()> appender,
    const TProtobufMessageType* type,
    TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        return;
    }

    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse vector from %Qlv",
            unversionedValue.Type);
    }

    class TConsumer
        : public IYsonConsumer
    {
    public:
        TConsumer(
            std::function<google::protobuf::Message*()> appender,
            const TProtobufMessageType* type)
            : Appender_(std::move(appender))
              , Type_(type)
              , OutputStream_(&WireBytes_)
        { }

        virtual void OnStringScalar(TStringBuf value) override
        {
            GetUnderlying()->OnStringScalar(value);
        }

        virtual void OnInt64Scalar(i64 value) override
        {
            GetUnderlying()->OnInt64Scalar(value);
        }

        virtual void OnUint64Scalar(ui64 value) override
        {
            GetUnderlying()->OnUint64Scalar(value);
        }

        virtual void OnDoubleScalar(double value) override
        {
            GetUnderlying()->OnDoubleScalar(value);
        }

        virtual void OnBooleanScalar(bool value) override
        {
            GetUnderlying()->OnBooleanScalar(value);
        }

        virtual void OnEntity() override
        {
            GetUnderlying()->OnEntity();
        }

        virtual void OnBeginList() override
        {
            if (Depth_ > 0) {
                GetUnderlying()->OnBeginList();
            }
            ++Depth_;
        }

        virtual void OnListItem() override
        {
            if (Depth_ == 1) {
                NextElement();
            } else {
                GetUnderlying()->OnListItem();
            }
        }

        virtual void OnEndList() override
        {
            --Depth_;
            if (Depth_ == 0) {
                FlushElement();
            } else {
                GetUnderlying()->OnEndList();
            }
        }

        virtual void OnBeginMap() override
        {
            ++Depth_;
            GetUnderlying()->OnBeginMap();
        }

        virtual void OnKeyedItem(TStringBuf key) override
        {
            GetUnderlying()->OnKeyedItem(key);
        }

        virtual void OnEndMap() override
        {
            --Depth_;
            GetUnderlying()->OnEndMap();
        }

        virtual void OnBeginAttributes() override
        {
            GetUnderlying()->OnBeginAttributes();
        }

        virtual void OnEndAttributes() override
        {
            GetUnderlying()->OnEndAttributes();
        }

        virtual void OnRaw(TStringBuf yson, EYsonType type) override
        {
            GetUnderlying()->OnRaw(yson, type);
        }

    private:
        const std::function<google::protobuf::Message*()> Appender_;
        const TProtobufMessageType* const Type_;

        std::unique_ptr<IYsonConsumer> Underlying_;
        int Depth_ = 0;

        TString WireBytes_;
        StringOutputStream OutputStream_;


        IYsonConsumer* GetUnderlying()
        {
            if (!Underlying_) {
                THROW_ERROR_EXCEPTION("YSON value must be a list without attributes");
            }
            return Underlying_.get();
        }

        void NextElement()
        {
            FlushElement();
            WireBytes_.clear();
            Underlying_ = CreateProtobufWriter(&OutputStream_, Type_);
        }

        void FlushElement()
        {
            if (!Underlying_) {
                return;
            }
            auto* value = Appender_();
            if (!value->ParseFromArray(WireBytes_.data(), WireBytes_.size())) {
                THROW_ERROR_EXCEPTION("Error parsing %v from wire bytes",
                    value->GetTypeName());
            }
            Underlying_.reset();
        }
    } consumer(std::move(appender), type);

    ParseYsonStringBuffer(
        TStringBuf(unversionedValue.Data.String, unversionedValue.Length),
        EYsonType::Node,
        &consumer);
}

void UnversionedValueToListImpl(
    std::function<void(TUnversionedValue)> appender,
    TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        return;
    }

    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse a vector from %Qlv",
            unversionedValue.Type);
    }

    class TConsumer
        : public TYsonConsumerBase
    {
    public:
        explicit TConsumer(std::function<void(TUnversionedValue)> appender)
            : Appender_(std::move(appender))
        { }

        virtual void OnStringScalar(TStringBuf value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedStringValue(value));
        }

        virtual void OnInt64Scalar(i64 value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedInt64Value(value));
        }

        virtual void OnUint64Scalar(ui64 value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedUint64Value(value));
        }

        virtual void OnDoubleScalar(double value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedDoubleValue(value));
        }

        virtual void OnBooleanScalar(bool value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedBooleanValue(value));
        }

        virtual void OnEntity() override
        {
            THROW_ERROR_EXCEPTION("YSON entities are not supported");
        }

        virtual void OnBeginList() override
        {
            EnsureNotInList();
            InList_ = true;
        }

        virtual void OnListItem() override
        { }

        virtual void OnEndList() override
        { }

        virtual void OnBeginMap() override
        {
            THROW_ERROR_EXCEPTION("YSON maps are not supported");
        }

        virtual void OnKeyedItem(TStringBuf /*key*/) override
        {
            YT_ABORT();
        }

        virtual void OnEndMap() override
        {
            YT_ABORT();
        }

        virtual void OnBeginAttributes() override
        {
            THROW_ERROR_EXCEPTION("YSON attributes are not supported");
        }

        virtual void OnEndAttributes() override
        {
            YT_ABORT();
        }

    private:
        const std::function<void(TUnversionedValue)> Appender_;

        bool InList_ = false;

        void EnsureInList()
        {
            if (!InList_) {
                THROW_ERROR_EXCEPTION("YSON list expected");
            }
        }

        void EnsureNotInList()
        {
            if (InList_) {
                THROW_ERROR_EXCEPTION("YSON list is unexpected");
            }
        }
    } consumer(std::move(appender));

    ParseYsonStringBuffer(
        TStringBuf(unversionedValue.Data.String, unversionedValue.Length),
        EYsonType::Node,
        &consumer);
}

////////////////////////////////////////////////////////////////////////////////

void MapToUnversionedValueImpl(
    TUnversionedValue* unversionedValue,
    const std::function<bool(TString*, TUnversionedValue*)> producer,
    const TRowBufferPtr& rowBuffer,
    int id)
{
    TString ysonBytes;
    TStringOutput outputStream(ysonBytes);
    NYT::NYson::TYsonWriter writer(&outputStream);
    writer.OnBeginMap();

    TString itemKey;
    TUnversionedValue itemValue;
    while (true) {
        if (!producer(&itemKey, &itemValue)) {
            break;
        }
        writer.OnKeyedItem(itemKey);
        UnversionedValueToYson(itemValue, &writer);
    }
    writer.OnEndMap();

    *unversionedValue = rowBuffer->Capture(MakeUnversionedAnyValue(ysonBytes, id));
}

void UnversionedValueToMapImpl(
    std::function<google::protobuf::Message*(TString)> appender,
    const TProtobufMessageType* type,
    TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        return;
    }

    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse map from %Qlv",
            unversionedValue.Type);
    }

    class TConsumer
        : public IYsonConsumer
    {
    public:
        TConsumer(
            std::function<google::protobuf::Message*(TString)> appender,
            const TProtobufMessageType* type)
            : Appender_(std::move(appender))
            , Type_(type)
            , OutputStream_(&WireBytes_)
        { }

        virtual void OnStringScalar(TStringBuf value) override
        {
            GetUnderlying()->OnStringScalar(value);
        }

        virtual void OnInt64Scalar(i64 value) override
        {
            GetUnderlying()->OnInt64Scalar(value);
        }

        virtual void OnUint64Scalar(ui64 value) override
        {
            GetUnderlying()->OnUint64Scalar(value);
        }

        virtual void OnDoubleScalar(double value) override
        {
            GetUnderlying()->OnDoubleScalar(value);
        }

        virtual void OnBooleanScalar(bool value) override
        {
            GetUnderlying()->OnBooleanScalar(value);
        }

        virtual void OnEntity() override
        {
            GetUnderlying()->OnEntity();
        }

        virtual void OnBeginList() override
        {
            ++Depth_;
            GetUnderlying()->OnBeginList();
        }

        virtual void OnListItem() override
        {
            GetUnderlying()->OnListItem();
        }

        virtual void OnEndList() override
        {
            --Depth_;
            GetUnderlying()->OnEndList();
        }

        virtual void OnBeginMap() override
        {
            if (Depth_ > 0) {
                GetUnderlying()->OnBeginMap();
            }
            ++Depth_;
        }

        virtual void OnKeyedItem(TStringBuf key) override
        {
            if (Depth_ == 1) {
                NextElement(key);
            } else {
                GetUnderlying()->OnKeyedItem(key);
            }
        }

        virtual void OnEndMap() override
        {
            --Depth_;
            if (Depth_ == 0) {
                FlushElement();
            } else {
                GetUnderlying()->OnEndMap();
            }
        }

        virtual void OnBeginAttributes() override
        {
            GetUnderlying()->OnBeginAttributes();
        }

        virtual void OnEndAttributes() override
        {
            GetUnderlying()->OnEndAttributes();
        }

        virtual void OnRaw(TStringBuf yson, EYsonType type) override
        {
            GetUnderlying()->OnRaw(yson, type);
        }

    private:
        const std::function<google::protobuf::Message*(TString)> Appender_;
        const TProtobufMessageType* const Type_;

        std::optional<TString> Key_;
        std::unique_ptr<IYsonConsumer> Underlying_;
        int Depth_ = 0;

        TString WireBytes_;
        StringOutputStream OutputStream_;


        IYsonConsumer* GetUnderlying()
        {
            if (!Underlying_) {
                THROW_ERROR_EXCEPTION("YSON value must be a list without attributes");
            }
            return Underlying_.get();
        }

        void NextElement(TStringBuf key)
        {
            FlushElement();
            WireBytes_.clear();
            Key_ = TString(key);
            Underlying_ = CreateProtobufWriter(&OutputStream_, Type_);
        }

        void FlushElement()
        {
            if (!Underlying_) {
                return;
            }
            auto* value = Appender_(*Key_);
            if (!value->ParseFromArray(WireBytes_.data(), WireBytes_.size())) {
                THROW_ERROR_EXCEPTION("Error parsing %v from wire bytes",
                    value->GetTypeName());
            }
            Underlying_.reset();
            Key_.reset();
        }
    } consumer(std::move(appender), type);

    ParseYsonStringBuffer(
        TStringBuf(unversionedValue.Data.String, unversionedValue.Length),
        EYsonType::Node,
        &consumer);
}

////////////////////////////////////////////////////////////////////////////////

void UnversionedValueToYson(TUnversionedValue unversionedValue, TCheckedInDebugYsonTokenWriter* tokenWriter)
{
    switch (unversionedValue.Type) {
        case EValueType::Int64:
            tokenWriter->WriteBinaryInt64(unversionedValue.Data.Int64);
            return;
        case EValueType::Uint64:
            tokenWriter->WriteBinaryUint64(unversionedValue.Data.Uint64);
            return;
        case EValueType::Double:
            tokenWriter->WriteBinaryDouble(unversionedValue.Data.Double);
            return;
        case EValueType::String:
            tokenWriter->WriteBinaryString(TStringBuf(unversionedValue.Data.String, unversionedValue.Length));
            return;
        case EValueType::Any:
        case EValueType::Composite:
            tokenWriter->WriteRawNodeUnchecked(TStringBuf(unversionedValue.Data.String, unversionedValue.Length));
            return;
        case EValueType::Boolean:
            tokenWriter->WriteBinaryBoolean(unversionedValue.Data.Boolean);
            return;
        case EValueType::Null:
            tokenWriter->WriteEntity();
            return;
        case EValueType::TheBottom:
        case EValueType::Min:
        case EValueType::Max:
            YT_ABORT();
    }
    ThrowUnexpectedValueType(unversionedValue.Type);
}

void UnversionedValueToYson(TUnversionedValue unversionedValue, IYsonConsumer* consumer)
{
    switch (unversionedValue.Type) {
        case EValueType::Int64:
            consumer->OnInt64Scalar(unversionedValue.Data.Int64);
            return;
        case EValueType::Uint64:
            consumer->OnUint64Scalar(unversionedValue.Data.Uint64);
            return;
        case EValueType::Double:
            consumer->OnDoubleScalar(unversionedValue.Data.Double);
            return;
        case EValueType::String:
            consumer->OnStringScalar(TStringBuf(unversionedValue.Data.String, unversionedValue.Length));
            return;
        case EValueType::Any:
        case EValueType::Composite:
            consumer->OnRaw(TStringBuf(unversionedValue.Data.String, unversionedValue.Length), EYsonType::Node);
            return;
        case EValueType::Boolean:
            consumer->OnBooleanScalar(unversionedValue.Data.Boolean);
            return;
        case EValueType::Null:
            consumer->OnEntity();
            return;
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            YT_ABORT();
    }
    ThrowUnexpectedValueType(unversionedValue.Type);
}

TYsonString UnversionedValueToYson(TUnversionedValue unversionedValue, bool enableRaw)
{
    TString data;
    data.reserve(GetYsonSize(unversionedValue));
    TStringOutput output(data);
    TYsonWriter writer(&output, EYsonFormat::Binary, EYsonType::Node, /* enableRaw */ enableRaw);
    UnversionedValueToYson(unversionedValue, &writer);
    return TYsonString(std::move(data));
}

////////////////////////////////////////////////////////////////////////////////

void ToAny(TRowBuffer* rowBuffer, TUnversionedValue* result, TUnversionedValue* value, EYsonFormat format)
{
    TStringStream stream;
    NYson::TYsonWriter writer(&stream, format);

    switch (value->Type) {
        case EValueType::Null: {
            result->Type = EValueType::Null;
            return;
        }
        case EValueType::Any:
        case EValueType::Composite: {
            if (format == EYsonFormat::Binary) {
                *result = *value;
                return;
            } else {
                writer.OnRaw(TStringBuf(value->Data.String, value->Length));
            }
            break;
        }
        case EValueType::String: {
            writer.OnStringScalar(TStringBuf(value->Data.String, value->Length));
            break;
        }
        case EValueType::Int64: {
            writer.OnInt64Scalar(value->Data.Int64);
            break;
        }
        case EValueType::Uint64: {
            writer.OnUint64Scalar(value->Data.Uint64);
            break;
        }
        case EValueType::Double: {
            writer.OnDoubleScalar(value->Data.Double);
            break;
        }
        case EValueType::Boolean: {
            writer.OnBooleanScalar(value->Data.Boolean);
            break;
        }
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            YT_ABORT();
    }

    writer.Flush();
    result->Type = EValueType::Any;
    result->Length = stream.Size();
    result->Data.String = stream.Data();
    *result = rowBuffer->Capture(*result);
}

////////////////////////////////////////////////////////////////////////////////

void PrintTo(const TOwningKey& key, ::std::ostream* os)
{
    *os << KeyToYson(key);
}

void PrintTo(const TUnversionedValue& value, ::std::ostream* os)
{
    *os << ToString(value);
}

void PrintTo(const TUnversionedRow& value, ::std::ostream* os)
{
    *os << ToString(value);
}

////////////////////////////////////////////////////////////////////////////////

struct TDefaultUnversionedRowsBuilderTag
{ };

TUnversionedRowsBuilder::TUnversionedRowsBuilder()
    : TUnversionedRowsBuilder(New<TRowBuffer>(TDefaultUnversionedRowsBuilderTag()))
{ }

TUnversionedRowsBuilder::TUnversionedRowsBuilder(TRowBufferPtr rowBuffer)
    : RowBuffer_(std::move(rowBuffer))
{ }

void TUnversionedRowsBuilder::ReserveRows(int rowCount)
{
    Rows_.reserve(rowCount);
}

void TUnversionedRowsBuilder::AddRow(TUnversionedRow row)
{
    Rows_.push_back(RowBuffer_->Capture(row));
}

void TUnversionedRowsBuilder::AddRow(TMutableUnversionedRow row)
{
    AddRow(TUnversionedRow(row));
}

void TUnversionedRowsBuilder::AddProtoRow(const TString& protoRow)
{
    auto& row = Rows_.emplace_back();
    FromProto(&row, protoRow, RowBuffer_);
}

TSharedRange<TUnversionedRow> TUnversionedRowsBuilder::Build()
{
    return MakeSharedRange(std::move(Rows_), std::move(RowBuffer_));
}

////////////////////////////////////////////////////////////////////////////////

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(NProto::TBlockMeta, /*last_key*/9, TUnversionedOwningRow)

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(NProto::TBoundaryKeysExt, /*min*/1, TUnversionedOwningRow)
REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(NProto::TBoundaryKeysExt, /*max*/2, TUnversionedOwningRow)

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(NProto::TSamplesExt, /*entries*/1, TUnversionedOwningRow)

namespace {

////////////////////////////////////////////////////////////////////////////////

//! These converters are used only for THeavyColumnStatisticsExt column_data_weights field proto<->yson conversion.
void FromBytes(std::vector<int>* vector, TStringBuf bytes)
{
    for (auto value : bytes) {
        vector->push_back(static_cast<int>(value));
    }
}

void ToBytes(TString* bytes, const std::vector<int>& vector)
{
    for (auto value : vector) {
        bytes->push_back(static_cast<unsigned char>(value));
    }
}

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(NProto::THeavyColumnStatisticsExt, /*column_data_weights*/5, TUnversionedOwningRow)

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
