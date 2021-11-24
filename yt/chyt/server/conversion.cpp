#include "conversion.h"

#include "yt_ch_converter.h"
#include "ch_yt_converter.h"
#include "config.h"
#include "helpers.h"

#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/row_buffer.h>

#include <library/cpp/iterator/functools.h>

#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeNullable.h>

#include <Storages/ColumnsDescription.h>

namespace NYT::NClickHouseServer {

using namespace NTableClient;
using namespace NLogging;

static const TLogger Logger("Conversion");

////////////////////////////////////////////////////////////////////////////////

DB::DataTypePtr ToDataType(const TComplexTypeFieldDescriptor& descriptor, const TCompositeSettingsPtr& settings, bool enableReadOnlyConversions)
{
    TYTCHConverter converter(descriptor, settings, enableReadOnlyConversions);
    return converter.GetDataType();
}

DB::DataTypes ToDataTypes(const NTableClient::TTableSchema& schema, const TCompositeSettingsPtr& settings, bool enableReadOnlyConversions)
{
    DB::DataTypes result;
    result.reserve(schema.GetColumnCount());

    for (const auto& column : schema.Columns()) {
        TComplexTypeFieldDescriptor descriptor(column);
        result.emplace_back(ToDataType(std::move(descriptor), settings, enableReadOnlyConversions));
    }

    return result;
}

DB::NamesAndTypesList ToNamesAndTypesList(const NTableClient::TTableSchema& schema, const TCompositeSettingsPtr& settings)
{
    const auto& dataTypes = ToDataTypes(schema, settings);

    DB::NamesAndTypesList result;

    for (int index = 0; index < schema.GetColumnCount(); ++index) {
        result.emplace_back(schema.Columns()[index].Name(), dataTypes[index]);
    }

    return result;
}

DB::Block ToHeaderBlock(const TTableSchema& schema, const TCompositeSettingsPtr& settings)
{
    DB::Block headerBlock;

    auto namesAndTypesList = ToNamesAndTypesList(schema, settings);

    for (const auto& nameAndTypePair : namesAndTypesList) {
        auto column = nameAndTypePair.type->createColumn();
        headerBlock.insert({ std::move(column), nameAndTypePair.type, nameAndTypePair.name });
    }

    return headerBlock;
}

////////////////////////////////////////////////////////////////////////////////

EValueType ToValueType(DB::Field::Types::Which which)
{
    switch (which) {
        case DB::Field::Types::Which::Null:
            return EValueType::Null;
        case DB::Field::Types::Which::Int64:
            return EValueType::Int64;
        case DB::Field::Types::Which::UInt64:
            return EValueType::Uint64;
        case DB::Field::Types::Which::Float64:
            return EValueType::Double;
        case DB::Field::Types::Which::String:
            return EValueType::String;
        default:
            THROW_ERROR_EXCEPTION(
                "ClickHouse physical type %Qv is not supported",
                DB::Field::Types::toString(which));
    }
}

////////////////////////////////////////////////////////////////////////////////

TLogicalTypePtr ToLogicalType(const DB::DataTypePtr& type, const TCompositeSettingsPtr& settings)
{
    TCHYTConverter converter(type, settings);
    return converter.GetLogicalType();
}

TTableSchema ToTableSchema(const DB::ColumnsDescription& columns, const TKeyColumns& keyColumns, const TCompositeSettingsPtr& settings)
{
    std::vector<TString> columnOrder;
    THashSet<TString> usedColumns;

    for (const auto& keyColumnName : keyColumns) {
        if (!columns.has(keyColumnName)) {
            THROW_ERROR_EXCEPTION("Column %Qv is specified as key column but is missing",
                keyColumnName);
        }
        columnOrder.emplace_back(keyColumnName);
        usedColumns.emplace(keyColumnName);
    }

    for (const auto& column : columns) {
        if (usedColumns.emplace(column.name).second) {
            columnOrder.emplace_back(column.name);
        }
    }

    std::vector<TColumnSchema> columnSchemas;
    columnSchemas.reserve(columnOrder.size());
    for (int index = 0; index < static_cast<int>(columnOrder.size()); ++index) {
        const auto& name = columnOrder[index];
        const auto& column = columns.get(name);
        const auto& type = ToLogicalType(column.type, settings);
        std::optional<ESortOrder> sortOrder;
        if (index < static_cast<int>(keyColumns.size())) {
            sortOrder = ESortOrder::Ascending;
        }
        columnSchemas.emplace_back(name, type, sortOrder);
    }

    return TTableSchema(columnSchemas);
}

////////////////////////////////////////////////////////////////////////////////

DB::Field ToField(
    const NTableClient::TUnversionedValue& value,
    const NTableClient::TLogicalTypePtr& type)
{
    auto settings = New<TCompositeSettings>();
    TYTCHConverter convertor(TComplexTypeFieldDescriptor(type), settings);

    convertor.ConsumeUnversionedValues(TRange(&value, 1));
    auto resultColumn = convertor.FlushColumn();

    YT_VERIFY(resultColumn->size() == 1);

return (*resultColumn)[0];
}

void ToUnversionedValue(const DB::Field& field, TUnversionedValue* value)
{
    value->Type = ToValueType(field.getType());
    switch (value->Type) {
        case EValueType::Int64:
        case EValueType::Uint64:
        case EValueType::Double: {
            memcpy(&value->Data, &field.reinterpret<ui64>(), sizeof(value->Data));
            break;
        }
        case EValueType::Boolean: {
            if (field.get<ui64>() > 1) {
                THROW_ERROR_EXCEPTION("Cannot convert value %v to boolean", field.get<ui64>());
            }
            memcpy(&value->Data, &field.get<ui64>(), sizeof(value->Data));
            break;
        }
        case EValueType::String: {
            const auto& str = field.get<std::string>();
            value->Data.String = str.data();
            value->Length = str.size();
            break;
        }
        default: {
            THROW_ERROR_EXCEPTION("Unexpected data type %Qlv", value->Type);
        }
    }
}

std::vector<DB::Field> UnversionedRowToFields(
    const NTableClient::TUnversionedRow& row,
    const NTableClient::TTableSchema& schema)
{
    YT_VERIFY(row.GetCount() <= schema.Columns().size());

    std::vector<DB::Field> fields;
    fields.reserve(row.GetCount());

    for (size_t index = 0; index < row.GetCount(); ++index) {
        fields.emplace_back(ToField(row[index], schema.Columns()[index].LogicalType()));
    }

    return fields;
}

////////////////////////////////////////////////////////////////////////////////

DB::Block ToBlock(
    const IUnversionedRowBatchPtr& batch,
    const TTableSchema& readSchema,
    const std::vector<int>& idToColumnIndex,
    const DB::Block& headerBlock,
    const TCompositeSettingsPtr& compositeSettings)
{
    // NB(max42): CHYT-256.
    // If chunk schema contains not all of the requested columns (which may happen
    // when a non-required column was introduced after chunk creation), we are not
    // going to receive some of the unversioned values with nulls. We still need
    // to provide them to CH, though, so we keep track of present columns for each
    // row we get and add nulls for all unpresent columns.
    std::vector<bool> presentColumnMask(readSchema.GetColumnCount());

    auto block = headerBlock.cloneEmpty();

    // Indexed by column indices.
    std::vector<TYTCHConverter> converters;
    converters.reserve(readSchema.GetColumnCount());

    for (int columnIndex = 0; columnIndex < readSchema.GetColumnCount(); ++columnIndex) {
        const auto& columnSchema = readSchema.Columns()[columnIndex];
        TComplexTypeFieldDescriptor descriptor(columnSchema);
        converters.emplace_back(descriptor, compositeSettings);
    }

    if (auto columnarBatch = batch->TryAsColumnar()) {
        auto batchColumns = columnarBatch->MaterializeColumns();
        for (const auto* ytColumn : batchColumns) {
            auto columnIndex = idToColumnIndex[ytColumn->Id];
            YT_VERIFY(columnIndex != -1);
            converters[columnIndex].ConsumeYtColumn(*ytColumn);
            presentColumnMask[columnIndex] = true;
        }
        for (int columnIndex = 0; columnIndex < static_cast<int>(readSchema.Columns().size()); ++columnIndex) {
            if (!presentColumnMask[columnIndex]) {
                YT_VERIFY(!readSchema.Columns()[columnIndex].Required());
                converters[columnIndex].ConsumeNulls(batch->GetRowCount());
            }
        }
    } else {
        auto rowBatch = batch->MaterializeRows();
        // We transpose rows by writing down contiguous range of values for each column.
        // This is done to reduce the number of converter virtual calls.
        std::vector<std::vector<TUnversionedValue>> columnIndexToUnversionedValues(readSchema.GetColumnCount());
        for (auto& unversionedValues : columnIndexToUnversionedValues) {
            unversionedValues.reserve(rowBatch.size());
        }

        auto nullValue = MakeUnversionedNullValue();

        for (auto row : rowBatch) {
            presentColumnMask.assign(readSchema.GetColumnCount(), false);
            for (int index = 0; index < static_cast<int>(row.GetCount()); ++index) {
                auto value = row[index];
                auto id = value.Id;
                int columnIndex = (id < idToColumnIndex.size()) ? idToColumnIndex[id] : -1;
                YT_VERIFY(columnIndex != -1);
                YT_VERIFY(!presentColumnMask[columnIndex]);
                presentColumnMask[columnIndex] = true;
                columnIndexToUnversionedValues[columnIndex].emplace_back(value);
            }
            for (int columnIndex = 0; columnIndex < readSchema.GetColumnCount(); ++columnIndex) {
                if (!presentColumnMask[columnIndex]) {
                    YT_VERIFY(readSchema.Columns()[columnIndex].LogicalType()->IsNullable());
                    // NB: converter does not care about value ids.
                    columnIndexToUnversionedValues[columnIndex].emplace_back(nullValue);
                }
            }
        }

        for (int columnIndex = 0; columnIndex < readSchema.GetColumnCount(); ++columnIndex) {
            const auto& unversionedValues = columnIndexToUnversionedValues[columnIndex];
            YT_VERIFY(unversionedValues.size() == rowBatch.size());
            auto& converter = converters[columnIndex];
            converter.ConsumeUnversionedValues(unversionedValues);
        }
    }

    for (const auto& [columnIndex, converter] : Enumerate(converters)) {
        auto column = converter.FlushColumn();
        YT_VERIFY(std::ssize(*column) == batch->GetRowCount());
        block.getByPosition(columnIndex).column = std::move(column);
    }

    return block;
}

TSharedRange<TUnversionedRow> ToRowRange(
    const DB::Block& block,
    const std::vector<DB::DataTypePtr>& dataTypes,
    const std::vector<int>& columnIndexToId,
    const TCompositeSettingsPtr& settings)
{
    int columnCount = columnIndexToId.size();
    i64 rowCount = block.rows();
    const auto& columns = block.getColumns();
    YT_VERIFY(std::ssize(columns) == columnCount);

    std::vector<TCHYTConverter> converters;
    converters.reserve(columnCount);
    for (int columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
        converters.emplace_back(dataTypes[columnIndex], settings);
    }

    TRowBufferPtr rowBuffer = New<TRowBuffer>();
    std::vector<TMutableUnversionedRow> mutableRows(rowCount);

    for (i64 rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        mutableRows[rowIndex] = rowBuffer->AllocateUnversioned(columnCount);
    }

    for (int columnIndex = 0; columnIndex < columnCount; ++columnIndex) {
        auto& converter = converters[columnIndex];
        auto valueRange = converter.ConvertColumnToUnversionedValues(columns[columnIndex]);
        int id = columnIndexToId[columnIndex];
        YT_VERIFY(std::ssize(valueRange) == rowCount);
        for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
            auto& value = mutableRows[rowIndex][columnIndex];
            value = valueRange[rowIndex];
            value.Id = id;
        }
    }

    std::vector<TUnversionedRow> rows(rowCount);
    for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        rows[rowIndex] = mutableRows[rowIndex];
    }

    // Rows are backed up by row buffer, string data is backed up by converters (which
    // hold original columns if necessary).
    return MakeSharedRange(rows, std::move(converters), std::move(rowBuffer));
}

////////////////////////////////////////////////////////////////////////////////

/*
 * How does it work?
 *
 * If types are not Nullable, then conversion is trivial.
 * Just convert every TUnversiondValue to DB::Field.
 *
 * If a key is shorter than provided usedKeyColumnCount, the rest of the key is
 * filled with min (lower) or max (upper) possible value of the coresponding column.
 *
 * If a column has a Nullable (optional) type, then we have a problem, because
 * ClickHouse does not support Nullable columns in primary key.
 *
 * To overcome this limitation, we use the following trick:
 * Imagine that we have a sorted table and following key bounds:
 *
 * Bound-1: >= [#; 2]
 * Bound-2: <= [0; 1]
 *
 * [#; 0], [#; 2], [#; 4], [0; 1], [0; 3], [0, 5], [1, 0]
 * [-----------------------Sorted-----------------------]
 *         [-------------------Bound-1------------------]
 * [-----------Bound-2----------]
 *
 * We replace all Null (#) values with the minimum possible type value (in the mind).
 * After it, the table contains following values:
 *
 * [0; 0], [0; 2], [0; 4], [0; 1], [0; 3], [0, 5], [1, 0]
 * [-------Sorted-------]  [-----------Sorted-----------]
 * [---------------------Not-Sorted---------------------]
 *
 * The table is not sorted any more. Instead, we have two sorted segments.
 * But it's still possible to efficiently filter some chunks.
 *
 * Unfortunately, replacing all Null values with minimum breaks key bounds:
 *
 * Bound-1': >= [0; 2]
 * Bound-1': <= [0; 1]
 *
 * [0; 0],         [0; 2],         [0; 4]
 *         [0; 1],         [0; 3],         [0; 5], [1; 0]
 *                 [--------------Bound-1'--------------]
 * [--Bound-2'--]
 *
 * Now both Bound-1 and Bound-2 cover less values.
 *
 * It's because before replacing Bound-1 included all rows starting with 0
 * and Bound-2 included all rows starting with #.
 * Now some of these values are missing.
 *
 * We need to adjust key bounds a little bit, so they include all previous values:
 *
 * Adjusted-Bound-1: >= [0; std::numeric_limits<Uint64>::min()]
 * Adjusted-Bound-2: <= [0; std::numeric_limits<Uint64>::max()]
 *
 * [0; 0],         [0; 2],         [0; 4]
 *         [0; 1],         [0; 3],         [0; 5], [1; 0]
 * [------------------Adjusted-Bound-1------------------]
 * [--------------Adjusted-Bound-2--------------]
 *
 * Now key bounds do not contain Null values and cover all original rows.
 * They also may cover some 'extra' rows (range became wider). But it does not
 * affect correctness, it can only affect performance a little bit.
 *
 * We need to adjust lower key bound only when it contains Null,
 * and adjust upper key bound only when it contains minimum type value.
 *
 * One more optimization:
 * If the common prefix of lowerBound and upperBound contains Null or minumum type value,
 * then we do not need to adjust bounds. They already include all values from the range:
 *
 * Bound-1: >= [#; 1]
 * Bound-2: <= [#; 2]
 *
 * [#; 0], [#; 2], [#; 4], [0; 1], [0; 3], [0, 5], [1, 0]
 * [-----------------------Sorted-----------------------]
 *         [------------------Bound-1-------------------]
 * [---Bound-2--]
 *
 * After replacing:
 *
 * Bound-1': >= [0; 1]
 * Bound-2': <= [0; 2]
 *
 * [0; 0],         [0; 2],         [0; 4]
 *         [0; 1],         [0; 3],         [0; 5], [1; 0]
 *         [------------------Bound-1'------------------]
 * [------Bound-2'------]
 */
TClickHouseKeys ToClickHouseKeys(
    const TKeyBound& lowerBound,
    const TKeyBound& upperBound,
    const NTableClient::TTableSchema& schema,
    const DB::DataTypes& dataTypes,
    int usedKeyColumnCount,
    bool tryMakeBoundsInclusive)
{
    YT_VERIFY(usedKeyColumnCount <= std::ssize(dataTypes));

    int commonPrefixSize = 0;
    if (lowerBound && upperBound) {
        while (commonPrefixSize < static_cast<int>(lowerBound.Prefix.GetCount())
            && commonPrefixSize < static_cast<int>(upperBound.Prefix.GetCount())
            && lowerBound.Prefix[commonPrefixSize] == upperBound.Prefix[commonPrefixSize])
        {
            ++commonPrefixSize;
        }
    }

    auto convertToClickHouseKey = [&] (const TKeyBound& ytBound) {
        std::vector<DB::FieldRef> chKey(usedKeyColumnCount);
        // See explanation above.
        bool adjusted = false;

        int ytBoundSize = ytBound.Prefix.GetCount();

        for (int index = 0; index < usedKeyColumnCount; ++index) {
            const auto& dataType = DB::removeNullable(dataTypes[index]);
            bool isNullable = dataTypes[index]->isNullable();

            if (index < ytBoundSize && !adjusted) {
                if (ytBound.Prefix[index].Type == EValueType::Null) {
                    chKey[index] = GetMinimumTypeValue(dataType);

                    adjusted = !ytBound.IsUpper && index >= commonPrefixSize;
                } else {
                    chKey[index] = ToField(ytBound.Prefix[index], schema.Columns()[index].LogicalType());

                    adjusted = ytBound.IsUpper
                        && index >= commonPrefixSize
                        && isNullable
                        && chKey[index] == GetMinimumTypeValue(dataType);
                }
            } else {
                if (ytBound.IsUpper) {
                    chKey[index] = GetMaximumTypeValue(dataType);
                } else {
                    chKey[index] = GetMinimumTypeValue(dataType);
                }
            }
        }

        bool isInclusive = ytBound.IsInclusive;
        // Adjusted key is always inclusive.
        isInclusive |= adjusted;
        // Truncated key also loses its exclusiveness.
        isInclusive |= (ytBoundSize > usedKeyColumnCount);

        if (!isInclusive && tryMakeBoundsInclusive && ytBoundSize > 0) {
            int index = ytBoundSize - 1;
            const auto& dataType = DB::removeNullable(dataTypes[index]);

            std::optional<DB::Field> value;
            if (ytBound.IsUpper) {
                value = TryDecrementFieldValue(chKey[index], dataType);
            } else {
                value = TryIncrementFieldValue(chKey[index], dataType);
            }
            if (value) {
                chKey[index] = *value;
            }
        }

        return chKey;
    };

    TClickHouseKeys result;

    if (lowerBound) {
        result.MinKey = convertToClickHouseKey(lowerBound);
    }
    if (upperBound) {
        result.MaxKey = convertToClickHouseKey(upperBound);
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
