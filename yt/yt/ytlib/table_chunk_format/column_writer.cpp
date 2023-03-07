#include "column_writer.h"

#include "boolean_column_writer.h"
#include "double_column_writer.h"
#include "integer_column_writer.h"
#include "null_column_writer.h"
#include "string_column_writer.h"

#include <yt/client/table_client/schema.h>

namespace NYT::NTableChunkFormat {

using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IValueColumnWriter> CreateUnversionedColumnWriter(
    const TColumnSchema& columnSchema,
    int columnIndex,
    TDataBlockWriter* blockWriter)
{
    auto simplifiedLogicalType = columnSchema.SimplifiedLogicalType();
    if (!simplifiedLogicalType) {
        return CreateUnversionedComplexColumnWriter(columnIndex, blockWriter);
    }

    switch (GetPhysicalType(*simplifiedLogicalType)) {
        case EValueType::Int64:
            return CreateUnversionedInt64ColumnWriter(columnIndex, blockWriter);

        case EValueType::Uint64:
            return CreateUnversionedUint64ColumnWriter(columnIndex, blockWriter);

        case EValueType::Double:
            return CreateUnversionedDoubleColumnWriter(columnIndex, blockWriter);

        case EValueType::String:
            return CreateUnversionedStringColumnWriter(columnIndex, blockWriter);

        case EValueType::Boolean:
            return CreateUnversionedBooleanColumnWriter(columnIndex, blockWriter);

        case EValueType::Any:
            return CreateUnversionedAnyColumnWriter(columnIndex, blockWriter);

        case EValueType::Null:
            return CreateUnversionedNullColumnWriter(blockWriter);

        case EValueType::Composite:
        case EValueType::Min:
        case EValueType::TheBottom:
        case EValueType::Max:
            break;
    }
    ThrowUnexpectedValueType(columnSchema.GetPhysicalType());
}

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IValueColumnWriter> CreateVersionedColumnWriter(
    const TColumnSchema& columnSchema,
    int id,
    TDataBlockWriter* blockWriter)
{
    switch (columnSchema.GetPhysicalType()) {
        case EValueType::Int64:
            return CreateVersionedInt64ColumnWriter(
                id,
                static_cast<bool>(columnSchema.Aggregate()),
                blockWriter);

        case EValueType::Uint64:
            return CreateVersionedUint64ColumnWriter(
                id,
                static_cast<bool>(columnSchema.Aggregate()),
                blockWriter);

        case EValueType::Double:
            return CreateVersionedDoubleColumnWriter(
                id,
                static_cast<bool>(columnSchema.Aggregate()),
                blockWriter);

        case EValueType::Boolean:
            return CreateVersionedBooleanColumnWriter(
                id,
                static_cast<bool>(columnSchema.Aggregate()),
                blockWriter);

        case EValueType::Any:
            return CreateVersionedAnyColumnWriter(
                id,
                static_cast<bool>(columnSchema.Aggregate()),
                blockWriter);

        case EValueType::String:
            return CreateVersionedStringColumnWriter(
                id,
                static_cast<bool>(columnSchema.Aggregate()),
                blockWriter);

        case EValueType::Null:
        case EValueType::Composite:
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            break;
    }
    ThrowUnexpectedValueType(columnSchema.GetPhysicalType());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableChunkFormat
