#include "unversioned_row.h"
#include "unversioned_value.h"
#include "serialize.h"

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/core/misc/farm_hash.h>
#include <yt/core/misc/hash.h>
#include <yt/core/misc/string.h>
#include <yt/core/misc/varint.h>

#include <yt/core/yson/consumer.h>

#include <yt/core/ytree/helpers.h>
#include <yt/core/ytree/node.h>
#include <yt/core/ytree/convert.h>

#include <util/generic/ymath.h>

#include <util/stream/str.h>

#include <cmath>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

static const Stroka SerializedNullRow("");
struct TOwningRowTag { };

////////////////////////////////////////////////////////////////////////////////

size_t GetByteSize(const TUnversionedValue& value)
{
    int result = MaxVarUint32Size * 2; // id and type

    switch (value.Type) {
        case EValueType::Null:
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            break;

        case EValueType::Int64:
        case EValueType::Uint64:
            result += MaxVarInt64Size;
            break;

        case EValueType::Double:
            result += sizeof(double);
            break;

        case EValueType::Boolean:
            result += 1;
            break;

        case EValueType::String:
        case EValueType::Any:
            result += MaxVarUint32Size + value.Length;
            break;

        default:
            YUNREACHABLE();
    }

    return result;
}

size_t GetDataWeight(const TUnversionedValue& value)
{
    switch (value.Type) {
        case EValueType::Null:
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            return 0;

        case EValueType::Int64:
            return sizeof(i64);

        case EValueType::Uint64:
            return sizeof(ui64);

        case EValueType::Double:
            return sizeof(double);

        case EValueType::Boolean:
            return 1;

        case EValueType::String:
        case EValueType::Any:
            return value.Length;

        default:
            YUNREACHABLE();
    }
}

size_t WriteValue(char* output, const TUnversionedValue& value)
{
    char* current = output;

    current += WriteVarUint32(current, value.Id);
    current += WriteVarUint32(current, static_cast<ui16>(value.Type));

    switch (value.Type) {
        case EValueType::Null:
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            break;

        case EValueType::Int64:
            current += WriteVarInt64(current, value.Data.Int64);
            break;

        case EValueType::Uint64:
            current += WriteVarUint64(current, value.Data.Uint64);
            break;

        case EValueType::Double:
            ::memcpy(current, &value.Data.Double, sizeof (double));
            current += sizeof (double);
            break;

        case EValueType::Boolean:
            *current++ = value.Data.Boolean ? '\x01' : '\x00';
            break;

        case EValueType::String:
        case EValueType::Any:
            current += WriteVarUint32(current, value.Length);
            ::memcpy(current, value.Data.String, value.Length);
            current += value.Length;
            break;

        default:
            YUNREACHABLE();
    }

    return current - output;
}

size_t ReadValue(const char* input, TUnversionedValue* value)
{
    const char* current = input;

    ui32 id;
    current += ReadVarUint32(current, &id);

    ui32 typeValue;
    current += ReadVarUint32(current, &typeValue);
    auto type = static_cast<EValueType>(typeValue);

    switch (type) {
        case EValueType::Null:
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            *value = MakeUnversionedSentinelValue(type, id);
            break;

        case EValueType::Int64: {
            i64 data;
            current += ReadVarInt64(current, &data);
            *value = MakeUnversionedInt64Value(data, id);
            break;
        }

        case EValueType::Uint64: {
            ui64 data;
            current += ReadVarUint64(current, &data);
            *value = MakeUnversionedUint64Value(data, id);
            break;
        }

        case EValueType::Double: {
            double data;
            ::memcpy(&data, current, sizeof (double));
            current += sizeof (double);
            *value = MakeUnversionedDoubleValue(data, id);
            break;
        }

        case EValueType::Boolean: {
            bool data = (*current) == 1;
            current += 1;
            *value = MakeUnversionedBooleanValue(data, id);
            break;
        }

        case EValueType::String:
        case EValueType::Any: {
            ui32 length;
            current += ReadVarUint32(current, &length);
            TStringBuf data(current, current + length);
            current += length;

            *value = type == EValueType::String 
                ? MakeUnversionedStringValue(data, id)
                : MakeUnversionedAnyValue(data, id);
            break;
        }

        default:
            YUNREACHABLE();
    }

    return current - input;
}

void Save(TStreamSaveContext& context, const TUnversionedValue& value)
{
    auto* output = context.GetOutput();
    if (IsStringLikeType(value.Type)) {
        output->Write(&value, sizeof (ui16) + sizeof (ui16) + sizeof (ui32)); // Id, Type, Length
        if (value.Length != 0) {
            output->Write(value.Data.String, value.Length);
        }
    } else {
        output->Write(&value, sizeof (TUnversionedValue));
    }
}

void Load(TStreamLoadContext& context, TUnversionedValue& value, TChunkedMemoryPool* pool)
{
    auto* input = context.GetInput();
    const size_t fixedSize = sizeof (ui16) + sizeof (ui16) + sizeof (ui32); // Id, Type, Length
    YCHECK(input->Load(&value, fixedSize) == fixedSize);
    if (IsStringLikeType(value.Type)) {
        if (value.Length != 0) {
            value.Data.String = pool->AllocateUnaligned(value.Length);
            YCHECK(input->Load(const_cast<char*>(value.Data.String), value.Length) == value.Length);
        } else {
            value.Data.String = nullptr;
        }
    } else {
        YCHECK(input->Load(&value.Data, sizeof (value.Data)) == sizeof (value.Data));
    }
}

size_t GetYsonSize(const TUnversionedValue& value)
{
    switch (value.Type) {
        case EValueType::Any:
            return value.Length;

        case EValueType::Null:
            // Marker type.
            return 1;

        case EValueType::Int64:
        case EValueType::Uint64:
            // Type marker + size;
            return 1 + MaxVarInt64Size;

        case EValueType::Double:
            // Type marker + sizeof double.
            return 1 + 8;

        case EValueType::String:
            // Type marker + length + string bytes.
            return 1 + MaxVarInt32Size + value.Length;

        case EValueType::Boolean:
            // Type marker + value.
            return 1 + 1;

        default:
            YUNREACHABLE();
    }
}

size_t WriteYson(char* buffer, const TUnversionedValue& unversionedValue)
{
    // TODO(psushin): get rid of output stream.
    TMemoryOutput output(buffer, GetYsonSize(unversionedValue));
    TYsonWriter writer(&output, EYsonFormat::Binary);
    switch (unversionedValue.Type) {
        case EValueType::Int64:
            writer.OnInt64Scalar(unversionedValue.Data.Int64);
            break;
        case EValueType::Uint64:
            writer.OnUint64Scalar(unversionedValue.Data.Uint64);
            break;

        case EValueType::Double:
            writer.OnDoubleScalar(unversionedValue.Data.Double);
            break;

        case EValueType::String:
            writer.OnStringScalar(TStringBuf(unversionedValue.Data.String, unversionedValue.Length));
            break;

        case EValueType::Boolean:
            writer.OnBooleanScalar(unversionedValue.Data.Boolean);
            break;

        case EValueType::Null:
            writer.OnEntity();
            break;

        default:
            YUNREACHABLE();
    }

    return output.Buf() - buffer;

}

Stroka ToString(const TUnversionedValue& value)
{
    switch (value.Type) {
        case EValueType::Null:
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            return Format("<%v>", value.Type);

        case EValueType::Int64:
            return Format("%v", value.Data.Int64);

        case EValueType::Uint64:
            return Format("%vu", value.Data.Uint64);

        case EValueType::Double:
            return Format("%v", value.Data.Double);

        case EValueType::Boolean:
            return Format("%v", value.Data.Boolean);

        case EValueType::String:
            return Stroka(value.Data.String, value.Length).Quote();

        case EValueType::Any:
            return ConvertToYsonString(
                    TYsonString(Stroka(value.Data.String, value.Length)),
                    EYsonFormat::Text)
                .Data();

        default:
            YUNREACHABLE();
    }
}

int CompareRowValues(const TUnversionedValue& lhs, const TUnversionedValue& rhs)
{
    if (lhs.Type == EValueType::Any || rhs.Type == EValueType::Any) {
        if (lhs.Type != EValueType::Min &&
            lhs.Type != EValueType::Max &&
            rhs.Type != EValueType::Min &&
            rhs.Type != EValueType::Max)
        {
            // Never compare composite values with non-sentinels.
            THROW_ERROR_EXCEPTION(
                EErrorCode::IncomparableType,
                "Cannot compare values of types %Qlv and %Qlv; only scalar types are allowed for key columns",
                lhs.Type,
                rhs.Type)
                << TErrorAttribute("lhs_value", lhs)
                << TErrorAttribute("rhs_value", rhs);
        }
    }

    if (Y_UNLIKELY(lhs.Type != rhs.Type)) {
        return static_cast<int>(lhs.Type) - static_cast<int>(rhs.Type);
    }

    switch (lhs.Type) {
        case EValueType::Int64: {
            auto lhsValue = lhs.Data.Int64;
            auto rhsValue = rhs.Data.Int64;
            if (lhsValue < rhsValue) {
                return -1;
            } else if (lhsValue > rhsValue) {
                return +1;
            } else {
                return 0;
            }
        }

        case EValueType::Uint64: {
            auto lhsValue = lhs.Data.Uint64;
            auto rhsValue = rhs.Data.Uint64;
            if (lhsValue < rhsValue) {
                return -1;
            } else if (lhsValue > rhsValue) {
                return +1;
            } else {
                return 0;
            }
        }

        case EValueType::Double: {
            double lhsValue = lhs.Data.Double;
            double rhsValue = rhs.Data.Double;
            if (lhsValue < rhsValue) {
                return -1;
            } else if (lhsValue > rhsValue) {
                return +1;
            } else {
                return 0;
            }
        }

        case EValueType::Boolean: {
            bool lhsValue = lhs.Data.Boolean;
            bool rhsValue = rhs.Data.Boolean;
            if (lhsValue < rhsValue) {
                return -1;
            } else if (lhsValue > rhsValue) {
                return +1;
            } else {
                return 0;
            }
        }

        case EValueType::String: {
            size_t lhsLength = lhs.Length;
            size_t rhsLength = rhs.Length;
            size_t minLength = std::min(lhsLength, rhsLength);
            int result = ::memcmp(lhs.Data.String, rhs.Data.String, minLength);
            if (result == 0) {
                if (lhsLength < rhsLength) {
                    return -1;
                } else if (lhsLength > rhsLength) {
                    return +1;
                } else {
                    return 0;
                }
            } else {
                return result;
            }
        }

        // NB: All sentinel types are equal.
        case EValueType::Null:
        case EValueType::Min:
        case EValueType::Max:
            return 0;

        case EValueType::Any:
        default:
            YUNREACHABLE();
    }
}

bool operator == (const TUnversionedValue& lhs, const TUnversionedValue& rhs)
{
    return CompareRowValues(lhs, rhs) == 0;
}

bool operator != (const TUnversionedValue& lhs, const TUnversionedValue& rhs)
{
    return CompareRowValues(lhs, rhs) != 0;
}

bool operator <= (const TUnversionedValue& lhs, const TUnversionedValue& rhs)
{
    return CompareRowValues(lhs, rhs) <= 0;
}

bool operator < (const TUnversionedValue& lhs, const TUnversionedValue& rhs)
{
    return CompareRowValues(lhs, rhs) < 0;
}

bool operator >= (const TUnversionedValue& lhs, const TUnversionedValue& rhs)
{
    return CompareRowValues(lhs, rhs) >= 0;
}

bool operator > (const TUnversionedValue& lhs, const TUnversionedValue& rhs)
{
    return CompareRowValues(lhs, rhs) > 0;
}

////////////////////////////////////////////////////////////////////////////////

int CompareRows(
    const TUnversionedValue* lhsBegin,
    const TUnversionedValue* lhsEnd,
    const TUnversionedValue* rhsBegin,
    const TUnversionedValue* rhsEnd)
{
    auto* lhsCurrent = lhsBegin;
    auto* rhsCurrent = rhsBegin;
    while (lhsCurrent != lhsEnd && rhsCurrent != rhsEnd) {
        int result = CompareRowValues(*lhsCurrent++, *rhsCurrent++);
        if (result != 0) {
            return result;
        }
    }
    return static_cast<int>(lhsEnd - lhsBegin) - static_cast<int>(rhsEnd - rhsBegin);
}

int CompareRows(TUnversionedRow lhs, TUnversionedRow rhs, int prefixLength)
{
    if (!lhs && !rhs) {
        return 0;
    }

    if (lhs && !rhs) {
        return +1;
    }

    if (!lhs && rhs) {
        return -1;
    }

    return CompareRows(
        lhs.Begin(),
        lhs.Begin() + std::min(lhs.GetCount(), prefixLength),
        rhs.Begin(),
        rhs.Begin() + std::min(rhs.GetCount(), prefixLength));
}

bool operator == (TUnversionedRow lhs, TUnversionedRow rhs)
{
    return CompareRows(lhs, rhs) == 0;
}

bool operator != (TUnversionedRow lhs, TUnversionedRow rhs)
{
    return CompareRows(lhs, rhs) != 0;
}

bool operator <= (TUnversionedRow lhs, TUnversionedRow rhs)
{
    return CompareRows(lhs, rhs) <= 0;
}

bool operator < (TUnversionedRow lhs, TUnversionedRow rhs)
{
    return CompareRows(lhs, rhs) < 0;
}

bool operator >= (TUnversionedRow lhs, TUnversionedRow rhs)
{
    return CompareRows(lhs, rhs) >= 0;
}

bool operator > (TUnversionedRow lhs, TUnversionedRow rhs)
{
    return CompareRows(lhs, rhs) > 0;
}

////////////////////////////////////////////////////////////////////////////////

bool operator == (TUnversionedRow lhs, const TUnversionedOwningRow& rhs)
{
    return CompareRows(lhs, rhs) == 0;
}

bool operator != (TUnversionedRow lhs, const TUnversionedOwningRow& rhs)
{
    return CompareRows(lhs, rhs) != 0;
}

bool operator <= (TUnversionedRow lhs, const TUnversionedOwningRow& rhs)
{
    return CompareRows(lhs, rhs) <= 0;
}

bool operator < (TUnversionedRow lhs, const TUnversionedOwningRow& rhs)
{
    return CompareRows(lhs, rhs) < 0;
}

bool operator >= (TUnversionedRow lhs, const TUnversionedOwningRow& rhs)
{
    return CompareRows(lhs, rhs) >= 0;
}

bool operator > (TUnversionedRow lhs, const TUnversionedOwningRow& rhs)
{
    return CompareRows(lhs, rhs) > 0;
}

////////////////////////////////////////////////////////////////////////////////

ui64 GetHash(TUnversionedRow row, int keyColumnCount)
{
    // NB: hash function may change in future. Use fingerprints for persistent hashing.
    return GetFarmFingerprint(row, keyColumnCount);
}

TFingerprint GetFarmFingerprint(TUnversionedRow row, int keyColumnCount)
{
    int partCount = std::min(row.GetCount(), keyColumnCount);
    const auto* begin = row.Begin();
    return GetFarmFingerprint(begin, begin + partCount);
}

size_t GetUnversionedRowByteSize(int valueCount)
{
    return sizeof(TUnversionedRowHeader) + sizeof(TUnversionedValue) * valueCount;
}

size_t GetDataWeight(TUnversionedRow row)
{
    return std::accumulate(
        row.Begin(),
        row.End(),
        0ULL,
        [] (size_t x, const TUnversionedValue& value) {
            return GetDataWeight(value) + x;
        });
}

////////////////////////////////////////////////////////////////////////////////

TMutableUnversionedRow TMutableUnversionedRow::Allocate(TChunkedMemoryPool* pool, int valueCount)
{
    size_t byteSize = GetUnversionedRowByteSize(valueCount);
    auto* header = reinterpret_cast<TUnversionedRowHeader*>(pool->AllocateAligned(byteSize));
    header->Count = valueCount;
    header->Capacity = valueCount;
    return TMutableUnversionedRow(header);
}

////////////////////////////////////////////////////////////////////////////////

namespace {

class TYsonAnyValidator
    : public IYsonConsumer
{
public:
    virtual void OnStringScalar(const TStringBuf& value) override
    { }

    virtual void OnInt64Scalar(i64 value) override
    { }

    virtual void OnUint64Scalar(ui64 value) override
    { }

    virtual void OnDoubleScalar(double value) override
    { }

    virtual void OnBooleanScalar(bool value) override
    { }

    virtual void OnEntity() override
    { }

    virtual void OnBeginList() override
    {
        ++Depth_;
    }

    virtual void OnListItem() override
    { }

    virtual void OnEndList() override
    {
        --Depth_;
    }

    virtual void OnBeginMap() override
    {
        ++Depth_;
    }

    virtual void OnKeyedItem(const TStringBuf& key) override
    { }

    virtual void OnEndMap() override
    {
        --Depth_;
    }

    virtual void OnBeginAttributes() override
    {
        if (Depth_ == 0) {
            THROW_ERROR_EXCEPTION("Table values cannot have top-level attributes");
        }
    }

    virtual void OnEndAttributes() override
    { }

    virtual void OnRaw(const TStringBuf& yson, EYsonType type) override
    { }

private:
    int Depth_ = 0;
};

void ValidateAnyValue(TStringBuf yson)
{
    TYsonAnyValidator validator;
    ParseYsonStringBuffer(yson, EYsonType::Node, &validator);
}

void ValidateDynamicValue(const TUnversionedValue& value)
{
    switch (value.Type) {
        case EValueType::String:
            if (value.Length > MaxStringValueLength) {
                THROW_ERROR_EXCEPTION("Value is too long: length %v, limit %v",
                    value.Length,
                    MaxStringValueLength);
            }
            break;

        case EValueType::Any:
            if (value.Length > MaxAnyValueLength) {
                THROW_ERROR_EXCEPTION("Value is too long: length %v, limit %v",
                    value.Length,
                    MaxAnyValueLength);
            }
            ValidateAnyValue(TStringBuf(value.Data.String, value.Length));
            break;

        case EValueType::Double:
            if (std::isnan(value.Data.Double)) {
                THROW_ERROR_EXCEPTION("Value of type \"double\" is not a number");
            }
            break;

        default:
            break;
    }
}

int ApplyIdMapping(
    const TUnversionedValue& value,
    const TTableSchema& schema,
    const TNameTableToSchemaIdMapping* idMappingPtr)
{
    auto valueId = value.Id;
    if (idMappingPtr) {
        const auto& idMapping = *idMappingPtr;
        if (valueId >= idMapping.size()) {
            THROW_ERROR_EXCEPTION("Invalid column id: actual %v, expected in range [0,%v]",
                valueId,
                idMapping.size() - 1);
        }
        return idMapping[valueId];
    } else {
        return valueId;
    }
}

void ValidateKeyPart(
    TUnversionedRow row,
    const TTableSchema& schema)
{
    ValidateKeyColumnCount(schema.GetKeyColumnCount());

    if (row.GetCount() < schema.GetKeyColumnCount()) {
        THROW_ERROR_EXCEPTION("Too few values in row: actual %v, expected >= %v",
            row.GetCount(),
            schema.GetKeyColumnCount());
    }

    for (int index = 0; index < schema.GetKeyColumnCount(); ++index) {
        const auto& value = row[index];
        ValidateKeyValue(value);
        int mappedId = ApplyIdMapping(value, schema, nullptr);
        if (mappedId < 0) {
            continue;
        }
        ValidateValueType(value, schema, mappedId);
        if (mappedId != index) {
            THROW_ERROR_EXCEPTION("Invalid column: actual %Qv, expected %Qv",
                schema.Columns()[mappedId].Name,
                schema.Columns()[index].Name);
        }
    }
}

void ValidateDataRow(
    TUnversionedRow row,
    const TNameTableToSchemaIdMapping* idMappingPtr,
    const TTableSchema& schema)
{
    ValidateRowValueCount(row.GetCount());
    ValidateKeyPart(row, schema);

    for (int index = schema.GetKeyColumnCount(); index < row.GetCount(); ++index) {
        const auto& value = row[index];
        ValidateDataValue(value);
        int mappedId = ApplyIdMapping(value, schema, idMappingPtr);
        if (mappedId < 0) {
            continue;
        }
        ValidateValueType(value, schema, mappedId);
    }
}

void ValidateKey(
    TKey key,
    const TTableSchema& schema)
{
    if (!key) {
        THROW_ERROR_EXCEPTION("Key cannot be null");
    }

    if (key.GetCount() != schema.GetKeyColumnCount()) {
        THROW_ERROR_EXCEPTION("Invalid number of key components: expected %v, actual %v",
            schema.GetKeyColumnCount(),
            key.GetCount());
    }

    ValidateKeyPart(key, schema);
}

void ValidateClientRow(
    TUnversionedRow row,
    const TTableSchema& schema,
    const TNameTableToSchemaIdMapping& idMapping,
    const TNameTablePtr& nameTable,
    bool isKey)
{
    ValidateRowValueCount(row.GetCount());
    ValidateKeyColumnCount(schema.GetKeyColumnCount());

    bool keyColumnSeen[MaxKeyColumnCount] {};

    for (const auto& value : row) {
        int mappedId = ApplyIdMapping(value, schema, &idMapping);

        if (mappedId < 0 || mappedId > schema.Columns().size()) {
            int size = nameTable->GetSize();
            if (value.Id < 0 || value.Id >= size) {
                THROW_ERROR_EXCEPTION("Expected value id in range [0:%v] but got %v",
                    size - 1,
                    value.Id);
            }

            THROW_ERROR_EXCEPTION("Unexpected column %Qv", nameTable->GetName(value.Id));
        }

        const auto& column = schema.Columns()[mappedId];
        ValidateValueType(value, schema, mappedId);

        if (column.Expression) {
            THROW_ERROR_EXCEPTION(
                "Column %Qv is computed automatically and should not be provided by user",
                column.Name);
        }

        if (mappedId < schema.GetKeyColumnCount()) {
            if (keyColumnSeen[mappedId]) {
                THROW_ERROR_EXCEPTION("Duplicate key column %Qv",
                    column.Name);
            }

            keyColumnSeen[mappedId] = true;
            ValidateKeyValue(value);
        } else if (isKey) {
            THROW_ERROR_EXCEPTION("Non-key column %Qv in a key",
                column.Name);
        } else {
            ValidateDataValue(value);
        }
    }

    for (int index = 0; index < schema.GetKeyColumnCount(); ++index) {
        if (!keyColumnSeen[index] && !schema.Columns()[index].Expression) {
            THROW_ERROR_EXCEPTION("Missing key column %Qv",
                schema.Columns()[index].Name);
        }
    }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

Stroka SerializeToString(TUnversionedRow row)
{
    return row
        ? SerializeToString(row.Begin(), row.End())
        : SerializedNullRow;
}

Stroka SerializeToString(const TUnversionedValue* begin, const TUnversionedValue* end)
{
    int size = 2 * MaxVarUint32Size; // header size
    for (auto* it = begin; it != end; ++it) {
        size += GetByteSize(*it);
    }

    Stroka buffer;
    buffer.resize(size);

    char* current = const_cast<char*>(buffer.data());
    current += WriteVarUint32(current, 0); // format version
    current += WriteVarUint32(current, static_cast<ui32>(std::distance(begin, end)));

    for (auto* it = begin; it != end; ++it) {
        current += WriteValue(current, *it);
    }

    buffer.resize(current - buffer.data());

    return buffer;
}

TUnversionedOwningRow DeserializeFromString(const Stroka& data)
{
    if (data == SerializedNullRow) {
        return TUnversionedOwningRow();
    }

    const char* current = data.data();

    ui32 version;
    current += ReadVarUint32(current, &version);
    YCHECK(version == 0);

    ui32 valueCount;
    current += ReadVarUint32(current, &valueCount);

    size_t fixedSize = GetUnversionedRowByteSize(valueCount);
    auto rowData = TSharedMutableRef::Allocate<TOwningRowTag>(fixedSize, false);
    auto* header = reinterpret_cast<TUnversionedRowHeader*>(rowData.Begin());

    header->Count = static_cast<i32>(valueCount);

    auto* values = reinterpret_cast<TUnversionedValue*>(header + 1);
    for (int index = 0; index < valueCount; ++index) {
        auto* value = values + index;
        current += ReadValue(current, value);
    }

    return TUnversionedOwningRow(std::move(rowData), data);
}

TUnversionedRow DeserializeFromString(const Stroka& data, const TRowBufferPtr& rowBuffer)
{
    if (data == SerializedNullRow) {
        return TUnversionedRow();
    }

    const char* current = data.data();

    ui32 version;
    current += ReadVarUint32(current, &version);
    YCHECK(version == 0);

    ui32 valueCount;
    current += ReadVarUint32(current, &valueCount);

    auto row = rowBuffer->Allocate(valueCount);

    auto* values = row.begin();
    for (int index = 0; index < valueCount; ++index) {
        auto* value = values + index;
        current += ReadValue(current, value);
        rowBuffer->Capture(value);
    }

    return row;
}

////////////////////////////////////////////////////////////////////////////////

void TUnversionedRow::Save(TSaveContext& context) const
{
    NYT::Save(context, SerializeToString(*this));
}

void TUnversionedRow::Load(TLoadContext& context)
{
    *this = DeserializeFromString(NYT::Load<Stroka>(context), context.GetRowBuffer());
}

void ValidateValueType(
    const TUnversionedValue& value,
    const TTableSchema& schema,
    int schemaId)
{
    if (value.Type != EValueType::Null && value.Type != schema.Columns()[schemaId].Type) {
        THROW_ERROR_EXCEPTION("Invalid type of column %Qv: expected %Qlv or %Qlv but got %Qlv",
            schema.Columns()[schemaId].Name,
            schema.Columns()[schemaId].Type,
            EValueType::Null,
            value.Type);
    }
}

void ValidateStaticValue(const TUnversionedValue& value)
{
    ValidateDataValueType(value.Type);
    switch (value.Type) {
        case EValueType::String:
        case EValueType::Any:
            if (value.Length > MaxRowWeightLimit) {
                THROW_ERROR_EXCEPTION("Value is too long: length %v, limit %v",
                    value.Length,
                    MaxRowWeightLimit);
            }
            break;

        case EValueType::Double:
            if (std::isnan(value.Data.Double)) {
                THROW_ERROR_EXCEPTION("Value of type \"double\" is not a number");
            }
            break;

        default:
            break;
    }
}

void ValidateDataValue(const TUnversionedValue& value)
{
    ValidateDataValueType(value.Type);
    ValidateDynamicValue(value);
}

void ValidateKeyValue(const TUnversionedValue& value)
{
    ValidateKeyValueType(value.Type);
    ValidateDynamicValue(value);
}

void ValidateRowValueCount(int count)
{
    if (count < 0) {
        THROW_ERROR_EXCEPTION("Negative number of values in row");
    }
    if (count > MaxValuesPerRow) {
        THROW_ERROR_EXCEPTION("Too many values in row: actual %v, limit %v",
            count,
            MaxValuesPerRow);
    }
}

void ValidateKeyColumnCount(int count)
{
    if (count < 0) {
        THROW_ERROR_EXCEPTION("Negative number of key columns");
    }
    if (count > MaxKeyColumnCount) {
        THROW_ERROR_EXCEPTION("Too many columns in key: actual %v, limit %v",
            count,
            MaxKeyColumnCount);
    }
}

void ValidateRowCount(int count)
{
    if (count < 0) {
        THROW_ERROR_EXCEPTION("Negative number of rows in rowset");
    }
    if (count > MaxRowsPerRowset) {
        THROW_ERROR_EXCEPTION("Too many rows in rowset: actual %v, limit %v",
            count,
            MaxRowsPerRowset);
    }
}

void ValidateClientDataRow(
    TUnversionedRow row,
    const TTableSchema& schema,
    const TNameTableToSchemaIdMapping& idMapping,
    const TNameTablePtr& nameTable)
{
    ValidateClientRow(row, schema, idMapping, nameTable, false);
}

void ValidateServerDataRow(
    TUnversionedRow row,
    const TTableSchema& schema)
{
    ValidateDataRow(row, nullptr, schema);
}

void ValidateClientKey(TKey key)
{
    for (const auto& value : key) {
        ValidateKeyValue(value);
    }
}

void ValidateClientKey(
    TKey key,
    const TTableSchema& schema,
    const TNameTableToSchemaIdMapping& idMapping,
    const TNameTablePtr& nameTable)
{
    ValidateClientRow(key, schema, idMapping, nameTable, true);
}

void ValidateServerKey(
    TKey key,
    const TTableSchema& schema)
{
    ValidateKey(key, schema);
}

void ValidateReadTimestamp(TTimestamp timestamp)
{
    if (timestamp != SyncLastCommittedTimestamp &&
        timestamp != AsyncLastCommittedTimestamp &&
        (timestamp < MinTimestamp || timestamp > MaxTimestamp))
    {
        THROW_ERROR_EXCEPTION("Invalid timestamp %v", timestamp);
    }
}

////////////////////////////////////////////////////////////////////////////////

TOwningKey GetKeySuccessorImpl(TKey key, int prefixLength, EValueType sentinelType)
{
    auto length = std::min(prefixLength, key.GetCount());
    TUnversionedOwningRowBuilder builder(length + 1);
    for (int index = 0; index < length; ++index) {
        builder.AddValue(key[index]);
    }
    builder.AddValue(MakeUnversionedSentinelValue(sentinelType));
    return builder.FinishRow();
}

TKey GetKeySuccessorImpl(TKey key, int prefixLength, EValueType sentinelType, const TRowBufferPtr& rowBuffer)
{
    auto length = std::min(prefixLength, key.GetCount());
    auto result = rowBuffer->Allocate(length + 1);
    for (int index = 0; index < length; ++index) {
        result[index] = rowBuffer->Capture(key[index]);
    }
    result[length] = MakeUnversionedSentinelValue(sentinelType);
    return result;
}

TOwningKey GetKeySuccessor(TKey key)
{
    return GetKeySuccessorImpl(
        key,
        key.GetCount(),
        EValueType::Min);
}

TKey GetKeySuccessor(TKey key, const TRowBufferPtr& rowBuffer)
{
    return GetKeySuccessorImpl(
        key,
        key.GetCount(),
        EValueType::Min,
        rowBuffer);
}

TOwningKey GetKeyPrefixSuccessor(TKey key, int prefixLength)
{
    return GetKeySuccessorImpl(
        key,
        prefixLength,
        EValueType::Max);
}

TKey GetKeyPrefixSuccessor(TKey key, int prefixLength, const TRowBufferPtr& rowBuffer)
{
    return GetKeySuccessorImpl(
        key,
        prefixLength,
        EValueType::Max,
        rowBuffer);
}

TOwningKey GetKeyPrefix(TKey key, int prefixLength)
{
    return TOwningKey(
        key.Begin(),
        key.Begin() + std::min(key.GetCount(), prefixLength));
}

TKey GetKeyPrefix(TKey key, int prefixLength, const TRowBufferPtr& rowBuffer)
{
    return rowBuffer->Capture(
        key.Begin(),
        std::min(key.GetCount(), prefixLength));
}

////////////////////////////////////////////////////////////////////////////////

static TOwningKey MakeSentinelKey(EValueType type)
{
    TUnversionedOwningRowBuilder builder;
    builder.AddValue(MakeUnversionedSentinelValue(type));
    return builder.FinishRow();
}

static const TOwningKey CachedMinKey = MakeSentinelKey(EValueType::Min);
static const TOwningKey CachedMaxKey = MakeSentinelKey(EValueType::Max);

const TOwningKey MinKey()
{
    return CachedMinKey;
}

const TOwningKey MaxKey()
{
    return CachedMaxKey;
}

static TOwningKey MakeEmptyKey()
{
    TUnversionedOwningRowBuilder builder;
    return builder.FinishRow();
}

static const TOwningKey CachedEmptyKey = MakeEmptyKey();

const TOwningKey EmptyKey()
{
    return CachedEmptyKey;
}

const TOwningKey& ChooseMinKey(const TOwningKey& a, const TOwningKey& b)
{
    int result = CompareRows(a, b);
    return result <= 0 ? a : b;
}

const TOwningKey& ChooseMaxKey(const TOwningKey& a, const TOwningKey& b)
{
    int result = CompareRows(a, b);
    return result >= 0 ? a : b;
}

void ToProto(TProtoStringType* protoRow, TUnversionedRow row)
{
    *protoRow = SerializeToString(row);
}

void ToProto(TProtoStringType* protoRow, const TUnversionedOwningRow& row)
{
    ToProto(protoRow, row.Get());
}

void ToProto(TProtoStringType* protoRow, const TUnversionedValue* begin, const TUnversionedValue* end)
{
    *protoRow = SerializeToString(begin, end);
}

void FromProto(TUnversionedOwningRow* row, const TProtoStringType& protoRow)
{
    *row = DeserializeFromString(protoRow);
}

void FromProto(TUnversionedRow* row, const TProtoStringType& protoRow, const TRowBufferPtr& rowBuffer)
{
    if (protoRow == SerializedNullRow) {
        *row = TUnversionedRow();
    }

    const char* current = protoRow.data();

    ui32 version;
    current += ReadVarUint32(current, &version);
    YCHECK(version == 0);

    ui32 valueCount;
    current += ReadVarUint32(current, &valueCount);

    auto mutableRow = rowBuffer->Allocate(valueCount);
    *row = mutableRow;

    auto* values = mutableRow.Begin();
    for (auto* value = values; value < values + valueCount; ++value) {
        current += ReadValue(current, value);
        rowBuffer->Capture(value);
    }
}

Stroka ToString(TUnversionedRow row)
{
    return row
        ? "[" + JoinToString(row.Begin(), row.End()) + "]"
        : "<Null>";
}

Stroka ToString(TMutableUnversionedRow row)
{
    return ToString(TUnversionedRow(row));
}

Stroka ToString(const TUnversionedOwningRow& row)
{
    return ToString(row.Get());
}

TSharedRange<TUnversionedRow> CaptureRows(
    const TRange<TUnversionedRow>& rows,
    TRefCountedTypeCookie tagCookie)
{
    size_t bufferSize = 0;
    bufferSize += sizeof (TUnversionedRow) * rows.Size();
    for (auto row : rows) {
        bufferSize += GetUnversionedRowByteSize(row.GetCount());
        for (const auto& value : row) {
            if (IsStringLikeType(value.Type)) {
                bufferSize += value.Length;
            }
        }
    }
    auto buffer = TSharedMutableRef::Allocate(bufferSize, false, tagCookie);

    char* alignedPtr = buffer.Begin();
    auto allocateAligned = [&] (size_t size) {
        auto* result = alignedPtr;
        alignedPtr += size;
        return result;
    };

    char* unalignedPtr = buffer.End();
    auto allocateUnaligned = [&] (size_t size) {
        unalignedPtr -= size;
        return unalignedPtr;
    };

    auto* capturedRows = reinterpret_cast<TUnversionedRow*>(allocateAligned(sizeof (TUnversionedRow) * rows.Size()));
    for (size_t index = 0; index < rows.Size(); ++index) {
        auto row = rows[index];
        int valueCount = row.GetCount();
        auto* capturedHeader = reinterpret_cast<TUnversionedRowHeader*>(allocateAligned(GetUnversionedRowByteSize(valueCount)));
        capturedHeader->Capacity = valueCount;
        capturedHeader->Count = valueCount;
        auto capturedRow = TMutableUnversionedRow(capturedHeader);
        capturedRows[index] = capturedRow;
        ::memcpy(capturedRow.Begin(), row.Begin(), sizeof (TUnversionedValue) * row.GetCount());
        for (auto& capturedValue : capturedRow) {
            if (IsStringLikeType(capturedValue.Type)) {
                auto* capturedString = allocateUnaligned(capturedValue.Length);
                ::memcpy(capturedString, capturedValue.Data.String, capturedValue.Length);
                capturedValue.Data.String = capturedString;
            }
        }
    }

    YCHECK(alignedPtr == unalignedPtr);

    return MakeSharedRange(MakeRange(capturedRows, rows.Size()), std::move(buffer));
}

void FromProto(TUnversionedOwningRow* row, const NChunkClient::NProto::TKey& protoKey)
{
    TUnversionedOwningRowBuilder rowBuilder(protoKey.parts_size());
    for (int id = 0; id < protoKey.parts_size(); ++id) {
        auto& keyPart = protoKey.parts(id);
        switch (ELegacyKeyPartType(keyPart.type())) {
            case ELegacyKeyPartType::Null:
                rowBuilder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id));
                break;

            case ELegacyKeyPartType::MinSentinel:
                rowBuilder.AddValue(MakeUnversionedSentinelValue(EValueType::Min, id));
                break;

            case ELegacyKeyPartType::MaxSentinel:
                rowBuilder.AddValue(MakeUnversionedSentinelValue(EValueType::Max, id));
                break;

            case ELegacyKeyPartType::Int64:
                rowBuilder.AddValue(MakeUnversionedInt64Value(keyPart.int64_value(), id));
                break;

            case ELegacyKeyPartType::Double:
                rowBuilder.AddValue(MakeUnversionedDoubleValue(keyPart.double_value(), id));
                break;

            case ELegacyKeyPartType::String:
                rowBuilder.AddValue(MakeUnversionedStringValue(keyPart.str_value(), id));
                break;

            case ELegacyKeyPartType::Composite:
                rowBuilder.AddValue(MakeUnversionedAnyValue(TStringBuf(), id));
                break;

            default:
                YUNREACHABLE();
        }
    }

    *row = rowBuilder.FinishRow();
}

void Serialize(const TUnversionedValue& value, IYsonConsumer* consumer)
{
    auto type = value.Type;
    switch (type) {
        case EValueType::Int64:
            consumer->OnInt64Scalar(value.Data.Int64);
            break;

        case EValueType::Uint64:
            consumer->OnUint64Scalar(value.Data.Uint64);
            break;

        case EValueType::Double:
            consumer->OnDoubleScalar(value.Data.Double);
            break;

        case EValueType::Boolean:
            consumer->OnBooleanScalar(value.Data.Boolean);
            break;

        case EValueType::String:
            consumer->OnStringScalar(TStringBuf(value.Data.String, value.Length));
            break;

        case EValueType::Any:
            ParseYsonStringBuffer(TStringBuf(value.Data.String, value.Length), EYsonType::Node, consumer);
            break;

        case EValueType::Null:
            consumer->OnEntity();
            break;

        default:
            consumer->OnBeginAttributes();
            consumer->OnKeyedItem("type");
            consumer->OnStringScalar(FormatEnum(type));
            consumer->OnEndAttributes();
            consumer->OnEntity();
            break;
    }
}

void Serialize(TKey key, IYsonConsumer* consumer)
{
    consumer->OnBeginList();
    for (const auto& value : key) {
        consumer->OnListItem();
        Serialize(value, consumer);
    }
    consumer->OnEndList();
}

void Serialize(const TOwningKey& key, IYsonConsumer* consumer)
{
    return Serialize(key.Get(), consumer);
}

void Deserialize(TOwningKey& key, INodePtr node)
{
    if (node->GetType() != ENodeType::List) {
        THROW_ERROR_EXCEPTION("Key cannot be parsed from %Qlv",
            node->GetType());
    }

    TUnversionedOwningRowBuilder builder;
    int id = 0;
    for (const auto& item : node->AsList()->GetChildren()) {
        try {
            switch (item->GetType()) {
                case ENodeType::Int64:
                    builder.AddValue(MakeUnversionedInt64Value(item->GetValue<i64>(), id));
                    break;

                case ENodeType::Uint64:
                    builder.AddValue(MakeUnversionedUint64Value(item->GetValue<ui64>(), id));
                    break;

                case ENodeType::Double:
                    builder.AddValue(MakeUnversionedDoubleValue(item->GetValue<double>(), id));
                    break;

                case ENodeType::Boolean:
                    builder.AddValue(MakeUnversionedBooleanValue(item->GetValue<bool>(), id));
                    break;

                case ENodeType::String:
                    builder.AddValue(MakeUnversionedStringValue(item->GetValue<Stroka>(), id));
                    break;

                case ENodeType::Entity: {
                    auto valueType = item->Attributes().Get<EValueType>("type", EValueType::Null);
                    if (valueType != EValueType::Null && !IsSentinelType(valueType)) {
                        THROW_ERROR_EXCEPTION("Entities can only represent %Qlv and sentinel values but "
                            "not values of type %Qlv",
                            EValueType::Null,
                            valueType);
                    }
                    builder.AddValue(MakeUnversionedSentinelValue(valueType, id));
                    break;
                }

                default:
                    THROW_ERROR_EXCEPTION("Key cannot contain %Qlv values",
                        item->GetType());
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error deserializing key component #%v", id)
                << ex;
        }
        ++id;
    }
    key = builder.FinishRow();
}

void TUnversionedOwningRow::Save(TStreamSaveContext& context) const
{
    NYT::Save(context, SerializeToString(Get()));
}

void TUnversionedOwningRow::Load(TStreamLoadContext& context)
{
    *this = DeserializeFromString(NYT::Load<Stroka>(context));
}

////////////////////////////////////////////////////////////////////////////////

TUnversionedRowBuilder::TUnversionedRowBuilder(int initialValueCapacity /*= 16*/)
{
    RowData_.resize(GetUnversionedRowByteSize(initialValueCapacity));
    Reset();
    GetHeader()->Capacity = initialValueCapacity;
}

int TUnversionedRowBuilder::AddValue(const TUnversionedValue& value)
{
    auto* header = GetHeader();
    if (header->Count == header->Capacity) {
        auto valueCapacity = 2 * std::max(1U, header->Capacity);
        RowData_.resize(GetUnversionedRowByteSize(valueCapacity));
        header = GetHeader();
        header->Capacity = valueCapacity;
    }

    *GetValue(header->Count) = value;
    return header->Count++;
}

TMutableUnversionedRow TUnversionedRowBuilder::GetRow()
{
    return TMutableUnversionedRow(GetHeader());
}

void TUnversionedRowBuilder::Reset()
{
    auto* header = GetHeader();
    header->Count = 0;
}

TUnversionedRowHeader* TUnversionedRowBuilder::GetHeader()
{
    return reinterpret_cast<TUnversionedRowHeader*>(RowData_.data());
}

TUnversionedValue* TUnversionedRowBuilder::GetValue(int index)
{
    return reinterpret_cast<TUnversionedValue*>(GetHeader() + 1) + index;
}

////////////////////////////////////////////////////////////////////////////////

TUnversionedOwningRowBuilder::TUnversionedOwningRowBuilder(int initialValueCapacity /*= 16*/)
    : InitialValueCapacity_(initialValueCapacity)
    , RowData_(TOwningRowTag())
{
    Reset();
}

int TUnversionedOwningRowBuilder::AddValue(const TUnversionedValue& value)
{
    auto* header = GetHeader();
    if (header->Count == header->Capacity) {
        auto valueCapacity = 2 * std::max(1U, header->Capacity);
        RowData_.Resize(GetUnversionedRowByteSize(valueCapacity));
        header = GetHeader();
        header->Capacity = valueCapacity;
    }

    auto* newValue = GetValue(header->Count);
    *newValue = value;

    if (value.Type == EValueType::String || value.Type == EValueType::Any) {
        if (StringData_.length() + value.Length > StringData_.capacity()) {
            char* oldStringData = const_cast<char*>(StringData_.begin());
            StringData_.reserve(std::max(
                StringData_.capacity() * 2,
                StringData_.length() + value.Length));
            char* newStringData = const_cast<char*>(StringData_.begin());
            for (int index = 0; index < header->Count; ++index) {
                auto* existingValue = GetValue(index);
                if (existingValue->Type == EValueType::String || existingValue->Type == EValueType::Any) {
                    existingValue->Data.String = newStringData + (existingValue->Data.String - oldStringData);
                }
            }
        }
        newValue->Data.String = const_cast<char*>(StringData_.end());
        StringData_.append(value.Data.String, value.Data.String + value.Length);
    }

    return header->Count++;
}

TUnversionedValue* TUnversionedOwningRowBuilder::BeginValues()
{
    return reinterpret_cast<TUnversionedValue*>(GetHeader() + 1);
}

TUnversionedValue* TUnversionedOwningRowBuilder::EndValues()
{
    return BeginValues() + GetHeader()->Count;
}

TUnversionedOwningRow TUnversionedOwningRowBuilder::FinishRow()
{
    auto row = TUnversionedOwningRow(
        TSharedMutableRef::FromBlob(std::move(RowData_)),
        std::move(StringData_));
    Reset();
    return row;
}

void TUnversionedOwningRowBuilder::Reset()
{
    RowData_.Resize(GetUnversionedRowByteSize(InitialValueCapacity_));

    auto* header = GetHeader();
    header->Count = 0;
    header->Capacity = InitialValueCapacity_;
}

TUnversionedRowHeader* TUnversionedOwningRowBuilder::GetHeader()
{
    return reinterpret_cast<TUnversionedRowHeader*>(RowData_.Begin());
}

TUnversionedValue* TUnversionedOwningRowBuilder::GetValue(int index)
{
    return reinterpret_cast<TUnversionedValue*>(GetHeader() + 1) + index;
}

////////////////////////////////////////////////////////////////////////////////

void TUnversionedOwningRow::Init(const TUnversionedValue* begin, const TUnversionedValue* end)
{
    int count = std::distance(begin, end);

    size_t fixedSize = GetUnversionedRowByteSize(count);
    RowData_ = TSharedMutableRef::Allocate<TOwningRowTag>(fixedSize, false);
    auto* header = GetHeader();

    header->Count = count;
    header->Capacity = count;
    ::memcpy(header + 1, begin, reinterpret_cast<const char*>(end) - reinterpret_cast<const char*>(begin));

    size_t variableSize = 0;
    for (auto it = begin; it != end; ++it) {
        const auto& otherValue = *it;
        if (otherValue.Type == EValueType::String || otherValue.Type == EValueType::Any) {
            variableSize += otherValue.Length;
        }
    }

    if (variableSize > 0) {
        StringData_.resize(variableSize);
        char* current = const_cast<char*>(StringData_.data());

        for (int index = 0; index < count; ++index) {
            const auto& otherValue = begin[index];
            auto& value = reinterpret_cast<TUnversionedValue*>(header + 1)[index];;
            if (otherValue.Type == EValueType::String || otherValue.Type == EValueType::Any) {
                ::memcpy(current, otherValue.Data.String, otherValue.Length);
                value.Data.String = current;
                current += otherValue.Length;
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

TOwningKey WidenKey(const TOwningKey& key, int keyColumnCount)
{
    YCHECK(keyColumnCount >= key.GetCount());

    if (key.GetCount() == keyColumnCount) {
        return key;
    }

    TUnversionedOwningRowBuilder builder;
    for (const auto* value = key.Begin(); value != key.End(); ++value) {
        builder.AddValue(*value);
    }

    for (int i = key.GetCount(); i < keyColumnCount; ++i) {
        builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null));
    }

    return builder.FinishRow();
}

////////////////////////////////////////////////////////////////////////////////

TUnversionedOwningRow BuildRow(
    const Stroka& yson,
    const TTableSchema& tableSchema,
    bool treatMissingAsNull /*= true*/)
{
    auto nameTable = TNameTable::FromSchema(tableSchema);

    auto rowParts = ConvertTo<yhash_map<Stroka, INodePtr>>(
        TYsonString(yson, EYsonType::MapFragment));

    TUnversionedOwningRowBuilder rowBuilder;
    auto addValue = [&] (int id, INodePtr value) {
        switch (value->GetType()) {
            case ENodeType::Int64:
                rowBuilder.AddValue(MakeUnversionedInt64Value(value->GetValue<i64>(), id));
                break;
            case ENodeType::Uint64:
                rowBuilder.AddValue(MakeUnversionedUint64Value(value->GetValue<ui64>(), id));
                break;
            case ENodeType::Double:
                rowBuilder.AddValue(MakeUnversionedDoubleValue(value->GetValue<double>(), id));
                break;
            case ENodeType::Boolean:
                rowBuilder.AddValue(MakeUnversionedBooleanValue(value->GetValue<bool>(), id));
                break;
            case ENodeType::String:
                rowBuilder.AddValue(MakeUnversionedStringValue(value->GetValue<Stroka>(), id));
                break;
            case ENodeType::Entity:
                rowBuilder.AddValue(MakeUnversionedSentinelValue(value->Attributes().Get<EValueType>("type"), id));
                break;
            default:
                rowBuilder.AddValue(MakeUnversionedAnyValue(ConvertToYsonString(value).Data(), id));
                break;
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
            addValue(id, pair.second);
        }
    }

    return rowBuilder.FinishRow();
}

TUnversionedOwningRow BuildKey(const Stroka& yson)
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
                    keyPart->GetValue<Stroka>(),
                    id));
                break;
            case ENodeType::Entity:
                keyBuilder.AddValue(MakeUnversionedSentinelValue(
                    keyPart->Attributes().Get<EValueType>("type"),
                    id));
                break;
            default:
                keyBuilder.AddValue(MakeUnversionedAnyValue(
                    ConvertToYsonString(keyPart).Data(),
                    id));
                break;
        }
    }

    return keyBuilder.FinishRow();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

