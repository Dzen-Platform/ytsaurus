#include "schema.h"
#include "unversioned_row.h"

#include <yt/core/ytree/serialize.h>
#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/fluent.h>

#include <yt/core/misc/protobuf_helpers.h>

#include <yt/ytlib/table_client/chunk_meta.pb.h>

#include <yt/ytlib/chunk_client/schema.h>

// TODO(sandello,lukyan): Refine these dependencies.
#include <yt/ytlib/query_client/query_preparer.h>
#include <yt/ytlib/query_client/functions.h>

namespace NYT {
namespace NTableClient {

using namespace NYTree;
using namespace NYson;
using namespace NQueryClient;
using namespace NChunkClient;

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

TColumnSchema::TColumnSchema()
    : Type(EValueType::Null)
{ }

TColumnSchema::TColumnSchema(
    const Stroka& name,
    EValueType type)
    : Name(name)
    , Type(type)
{ }

TColumnSchema& TColumnSchema::SetSortOrder(const TNullable<ESortOrder>& value)
{
    SortOrder = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetLock(const TNullable<Stroka>& value)
{
    Lock = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetGroup(const TNullable<Stroka>& value)
{
    Group = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetExpression(const TNullable<Stroka>& value)
{
    Expression = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetAggregate(const TNullable<Stroka>& value)
{
    Aggregate = value;
    return *this;
}

struct TSerializableColumnSchema
    : public TYsonSerializableLite
    , public TColumnSchema
{
    TSerializableColumnSchema()
    {
        RegisterParameter("name", Name)
            .NonEmpty();
        RegisterParameter("type", Type);
        RegisterParameter("lock", Lock)
            .Default();
        RegisterParameter("expression", Expression)
            .Default();
        RegisterParameter("aggregate", Aggregate)
            .Default();
        RegisterParameter("sort_order", SortOrder)
            .Default();
        RegisterParameter("group", Group)
            .Default();

        RegisterValidator([&] () {
            // Name
            if (Name.empty()) {
                THROW_ERROR_EXCEPTION("Column name cannot be empty");
            }

            try {
               // Type
                ValidateSchemaValueType(Type);

                // Lock
                if (Lock && Lock->empty()) {
                    THROW_ERROR_EXCEPTION("Lock name cannot be empty");
                }

                // Group
                if (Group && Group->empty()) {
                    THROW_ERROR_EXCEPTION("Group name cannot be empty");
                }
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION("Error validating column %Qv in table schema",
                    Name)
                    << ex;
            }
        });
    }
};

void Serialize(const TColumnSchema& schema, IYsonConsumer* consumer)
{
    TSerializableColumnSchema wrapper;
    static_cast<TColumnSchema&>(wrapper) = schema;
    Serialize(static_cast<const TYsonSerializableLite&>(wrapper), consumer);
}

void Deserialize(TColumnSchema& schema, INodePtr node)
{
    TSerializableColumnSchema wrapper;
    Deserialize(static_cast<TYsonSerializableLite&>(wrapper), node);
    schema = static_cast<TColumnSchema&>(wrapper);
}

void ToProto(NProto::TColumnSchema* protoSchema, const TColumnSchema& schema)
{
    protoSchema->set_name(schema.Name);
    protoSchema->set_type(static_cast<int>(schema.Type));
    if (schema.Lock) {
        protoSchema->set_lock(*schema.Lock);
    }
    if (schema.Expression) {
        protoSchema->set_expression(*schema.Expression);
    }
    if (schema.Aggregate) {
        protoSchema->set_aggregate(*schema.Aggregate);
    }
    if (schema.SortOrder) {
        protoSchema->set_sort_order(static_cast<int>(*schema.SortOrder));
    }
    if (schema.Group) {
        protoSchema->set_group(*schema.Group);
    }
}

void FromProto(TColumnSchema* schema, const NProto::TColumnSchema& protoSchema)
{
    schema->Name = protoSchema.name();
    schema->Type = EValueType(protoSchema.type());
    schema->Lock = protoSchema.has_lock() ? MakeNullable(protoSchema.lock()) : Null;
    schema->Expression = protoSchema.has_expression() ? MakeNullable(protoSchema.expression()) : Null;
    schema->Aggregate = protoSchema.has_aggregate() ? MakeNullable(protoSchema.aggregate()) : Null;
    schema->SortOrder = protoSchema.has_sort_order() ? MakeNullable(ESortOrder(protoSchema.sort_order())) : Null;
    schema->Group = protoSchema.has_group() ? MakeNullable(protoSchema.group()) : Null;
}

////////////////////////////////////////////////////////////////////////////////

TTableSchema::TTableSchema()
    : Strict_(false)
    , UniqueKeys_(false)
{ }

TTableSchema::TTableSchema(
    std::vector<TColumnSchema> columns,
    bool strict,
    bool uniqueKeys)
    : Columns_(std::move(columns))
    , Strict_(strict)
    , UniqueKeys_(uniqueKeys)
{
    for (const auto& column : Columns_) {
        if (column.SortOrder) {
            ++KeyColumnCount_;
        }
    }
}

const TColumnSchema* TTableSchema::FindColumn(const TStringBuf& name) const
{
    for (auto& column : Columns_) {
        if (column.Name == name) {
            return &column;
        }
    }
    return nullptr;
}

const TColumnSchema& TTableSchema::GetColumn(const TStringBuf& name) const
{
    auto* column = FindColumn(name);
    YCHECK(column);
    return *column;
}

const TColumnSchema& TTableSchema::GetColumnOrThrow(const TStringBuf& name) const
{
    auto* column = FindColumn(name);
    if (!column) {
        THROW_ERROR_EXCEPTION("Missing schema column %Qv", name);
    }
    return *column;
}

int TTableSchema::GetColumnIndex(const TColumnSchema& column) const
{
    return &column - Columns().data();
}

int TTableSchema::GetColumnIndex(const TStringBuf& name) const
{
    return GetColumnIndex(GetColumn(name));
}

int TTableSchema::GetColumnIndexOrThrow(const TStringBuf& name) const
{
    return GetColumnIndex(GetColumnOrThrow(name));
}

TTableSchema TTableSchema::Filter(const TColumnFilter& columnFilter) const
{
    if (columnFilter.All) {
        return *this;
    }

    std::vector<TColumnSchema> columns;
    for (int id : columnFilter.Indexes) {
        if (id < 0 || id >= Columns_.size()) {
            THROW_ERROR_EXCEPTION("Invalid column id in filter: excepted in range [0, %v], got %v",
                Columns_.size() - 1,
                id);
        }
        columns.push_back(Columns_[id]);
    }

    // Validate that key columns go first.
    for (int index = 1; index < static_cast<int>(columns.size()); ++index) {
        if (columns[index].SortOrder && !columns[index - 1].SortOrder) {
            THROW_ERROR_EXCEPTION("Column filter contains key column %Qv after non-key column %Qv",
                columns[index].Name,
                columns[index - 1].Name);
        }
    }

    return TTableSchema(std::move(columns));
}

void TTableSchema::AppendColumn(const TColumnSchema& column)
{
    Columns_.push_back(column);
    if (column.SortOrder) {
        ++KeyColumnCount_;
    }
    // XXX(babenko): this line below was commented out since we must allow
    // system columns in query schema
    // ValidateTableSchema(temp);
}

bool TTableSchema::HasComputedColumns() const
{
    for (const auto& column : Columns()) {
        if (column.Expression) {
            return true;
        }
    }
    return false;
}

bool TTableSchema::IsSorted() const
{
    return KeyColumnCount_ > 0;
}

TKeyColumns TTableSchema::GetKeyColumns() const
{
    TKeyColumns keyColumns;
    for (const auto& column : Columns()) {
        if (column.SortOrder) {
            keyColumns.push_back(column.Name);
        }
    }
    return keyColumns;
}

int TTableSchema::GetKeyColumnCount() const
{
    return KeyColumnCount_;
}

TTableSchema TTableSchema::FromKeyColumns(const TKeyColumns& keyColumns)
{
    TTableSchema schema;
    for (const auto& columnName : keyColumns) {
        schema.Columns_.push_back(
            TColumnSchema(columnName, EValueType::Any)
                .SetSortOrder(ESortOrder::Ascending));
    }
    schema.KeyColumnCount_ = keyColumns.size();
    ValidateTableSchema(schema);
    return schema;
}

TTableSchema TTableSchema::ToQuery() const
{
    if (IsSorted()) {
        return *this;
    } else {
        std::vector<TColumnSchema> columns {
            TColumnSchema(TabletIndexColumnName, EValueType::Int64)
                .SetSortOrder(ESortOrder::Ascending),
            TColumnSchema(RowIndexColumnName, EValueType::Int64)
                .SetSortOrder(ESortOrder::Ascending)
        };
        columns.insert(columns.end(), Columns_.begin(), Columns_.end());
        return TTableSchema(std::move(columns));
    }
}

TTableSchema TTableSchema::ToWrite() const
{
    std::vector<TColumnSchema> columns;
    if (IsSorted()) {
        for (const auto& column : Columns_) {
            if (!column.Expression) {
                columns.push_back(column);
            }
        }
    } else {
        columns.push_back(TColumnSchema(TabletIndexColumnName, EValueType::Int64)
            .SetSortOrder(ESortOrder::Ascending));
        for (const auto& column : Columns_) {
            if (column.Name != TimestampColumnName) {
                columns.push_back(column);
            }
        }
    }
    return TTableSchema(std::move(columns));
}

TTableSchema TTableSchema::ToLookup() const
{
    std::vector<TColumnSchema> columns;
    for (const auto& column : Columns_) {
        if (column.SortOrder && !column.Expression) {
            columns.push_back(column);
        }
    }
    return TTableSchema(std::move(columns));
}

TTableSchema TTableSchema::ToDelete() const
{
    return ToLookup();
}

TTableSchema TTableSchema::ToKeys() const
{
    return TTableSchema(std::vector<TColumnSchema>(Columns_.begin(), Columns_.begin() + KeyColumnCount_));
}

TTableSchema TTableSchema::ToValues() const
{
    std::vector<TColumnSchema> columns(Columns_.begin() + KeyColumnCount_, Columns_.end());
    return TTableSchema(std::move(columns));
}

TTableSchema TTableSchema::ToUniqueKeys() const
{
    return TTableSchema(Columns_, Strict_, true);
}

TTableSchema TTableSchema::ToStrippedColumnAttributes() const
{
    std::vector<TColumnSchema> strippedColumns;
    for (auto& column : Columns_) {
        strippedColumns.emplace_back(column.Name, column.Type);
    }
    return TTableSchema(strippedColumns, Strict_, UniqueKeys_);
}

TTableSchema TTableSchema::ToCanonical() const
{
    auto columns = Columns();
    std::sort(
        columns.begin() + KeyColumnCount_,
        columns.end(),
        [] (const TColumnSchema& lhs, const TColumnSchema& rhs) {
            return lhs.Name < rhs.Name;
        });
    return TTableSchema(columns, Strict_, UniqueKeys_);
}

TTableSchema TTableSchema::ToSorted(const TKeyColumns& keyColumns) const
{
    auto columns = Columns();
    for (int index = 0; index < keyColumns.size(); ++index) {
        auto it = std::find_if(
            columns.begin() + index,
            columns.end(),
            [&] (const TColumnSchema& column) {
                return column.Name == keyColumns[index];
            });

        if (it == columns.end()) {
            THROW_ERROR_EXCEPTION("Column %Qv is not found in schema", keyColumns[index])
                << TErrorAttribute("schema", *this)
                << TErrorAttribute("key_columns", keyColumns);
        }

        std::swap(columns[index], *it);
        columns[index].SetSortOrder(ESortOrder::Ascending);
    }

    for (auto it = columns.begin() + keyColumns.size(); it != columns.end(); ++it) {
        it->SetSortOrder(Null);
    }

    return TTableSchema(columns, Strict_, UniqueKeys_);
}

void TTableSchema::Save(TStreamSaveContext& context) const
{
    using NYT::Save;
    Save(context, ToProto<NTableClient::NProto::TTableSchemaExt>(*this));
}

void TTableSchema::Load(TStreamLoadContext& context)
{
    using NYT::Load;
    auto protoSchema = NYT::Load<NTableClient::NProto::TTableSchemaExt>(context);
    *this = FromProto<TTableSchema>(protoSchema);
}

////////////////////////////////////////////////////////////////////////////////

Stroka ToString(const TTableSchema& schema)
{
    return ConvertToYsonString(schema, EYsonFormat::Text).Data();
}

void Serialize(const TTableSchema& schema, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginAttributes()
            .Item("strict").Value(schema.GetStrict())
            .Item("unique_keys").Value(schema.GetUniqueKeys())
        .EndAttributes()
        .Value(schema.Columns());
}

void Deserialize(TTableSchema& schema, INodePtr node)
{
    schema = TTableSchema(
        ConvertTo<std::vector<TColumnSchema>>(node),
        node->Attributes().Get<bool>("strict", true),
        node->Attributes().Get<bool>("unique_keys", false));
}

void ToProto(NProto::TTableSchemaExt* protoSchema, const TTableSchema& schema)
{
    ToProto(protoSchema->mutable_columns(), schema.Columns());
    protoSchema->set_strict(schema.GetStrict());
    protoSchema->set_unique_keys(schema.GetUniqueKeys());
}

void FromProto(TTableSchema* schema, const NProto::TTableSchemaExt& protoSchema)
{
    *schema = TTableSchema(
        FromProto<std::vector<TColumnSchema>>(protoSchema.columns()),
        protoSchema.strict(),
        protoSchema.unique_keys());
}

void FromProto(
    TTableSchema* schema,
    const NProto::TTableSchemaExt& protoSchema,
    const NProto::TKeyColumnsExt& protoKeyColumns)
{
    auto columns = FromProto<std::vector<TColumnSchema>>(protoSchema.columns());
    for (int columnIndex = 0; columnIndex < protoKeyColumns.names_size(); ++columnIndex) {
        auto& columnSchema = columns[columnIndex];
        YCHECK(columnSchema.Name == protoKeyColumns.names(columnIndex));
        columnSchema.SortOrder = ESortOrder::Ascending;
    }
    for (int columnIndex = protoKeyColumns.names_size(); columnIndex < columns.size(); ++columnIndex) {
        auto& columnSchema = columns[columnIndex];
        YCHECK(!columnSchema.SortOrder);
    }
    *schema = TTableSchema(
        std::move(columns),
        protoSchema.strict(),
        protoSchema.unique_keys());
}

////////////////////////////////////////////////////////////////////////////////

bool operator==(const TColumnSchema& lhs, const TColumnSchema& rhs)
{
    return lhs.Name == rhs.Name
           && lhs.Type == rhs.Type
           && lhs.SortOrder == rhs.SortOrder
           && lhs.Aggregate == rhs.Aggregate
           && lhs.Expression == rhs.Expression;
}

bool operator!=(const TColumnSchema& lhs, const TColumnSchema& rhs)
{
    return !(lhs == rhs);
}

bool operator==(const TTableSchema& lhs, const TTableSchema& rhs)
{
    return lhs.Columns() == rhs.Columns() &&
        lhs.GetStrict() == rhs.GetStrict() &&
        lhs.GetUniqueKeys() == rhs.GetUniqueKeys();
}

bool operator!=(const TTableSchema& lhs, const TTableSchema& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

void ValidateKeyColumns(const TKeyColumns& keyColumns)
{
    ValidateKeyColumnCount(keyColumns.size());

    yhash_set<Stroka> names;
    for (const auto& name : keyColumns) {
        if (!names.insert(name).second) {
            THROW_ERROR_EXCEPTION("Duplicate key column name %Qv",
                name);
        }
    }
}

void ValidateKeyColumnsUpdate(const TKeyColumns& oldKeyColumns, const TKeyColumns& newKeyColumns)
{
    ValidateKeyColumns(newKeyColumns);

    for (int index = 0; index < std::max(oldKeyColumns.size(), newKeyColumns.size()); ++index) {
        if (index >= newKeyColumns.size()) {
            THROW_ERROR_EXCEPTION("Missing original key column %Qv",
                oldKeyColumns[index]);
        } else if (index >= oldKeyColumns.size()) {
            // This is fine; new key column is added
        } else {
            if (oldKeyColumns[index] != newKeyColumns[index]) {
                THROW_ERROR_EXCEPTION("Key column mismatch in position %v: expected %Qv, got %Qv",
                    index,
                    oldKeyColumns[index],
                    newKeyColumns[index]);
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

void ValidateColumnSchema(const TColumnSchema& columnSchema)
{
    try {
        if (columnSchema.Name.empty()) {
            THROW_ERROR_EXCEPTION("Column name cannot be empty");
        }

        if (columnSchema.Name.has_prefix(SystemColumnNamePrefix)) {
            THROW_ERROR_EXCEPTION("Column name cannot start with prefix %Qv",
                SystemColumnNamePrefix);
        }

        if (columnSchema.Name.size() > MaxColumnNameLength) {
            THROW_ERROR_EXCEPTION("Column name is longer than maximum allowed: %v > %v",
                columnSchema.Name.size(),
                MaxColumnNameLength);
        }

        if (columnSchema.Lock) {
            if (columnSchema.Lock->empty()) {
                THROW_ERROR_EXCEPTION("Column lock name cannot be empty");
            }
            if (columnSchema.Lock->size() > MaxColumnLockLength) {
                THROW_ERROR_EXCEPTION("Column lock name is longer than maximum allowed: %v > %v",
                    columnSchema.Lock->size(),
                    MaxColumnLockLength);
            }
            if (columnSchema.SortOrder) {
                THROW_ERROR_EXCEPTION("Column lock cannot be set on a key column");
            }
        }

        if (columnSchema.Group) {
            if (columnSchema.Group->empty()) {
                THROW_ERROR_EXCEPTION("Column group should either be unset or be non-empty");
            }
            if (columnSchema.Group->size() > MaxColumnGroupLength) {
                THROW_ERROR_EXCEPTION("Column group name is longer than maximum allowed: %v > %v",
                    columnSchema.Group->size(),
                    MaxColumnGroupLength);
            }
        }

        ValidateSchemaValueType(columnSchema.Type);

        if (columnSchema.Expression && !columnSchema.SortOrder) {
            THROW_ERROR_EXCEPTION("Non-key column cannot be computed");
        }

        if (columnSchema.Aggregate && columnSchema.SortOrder) {
            THROW_ERROR_EXCEPTION("Key column cannot be aggregated");
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error validating schema of a column %Qv",
            columnSchema.Name)
                << ex;
    }
}

//! Validates the column schema update.
/*!
 *  \pre{oldColumn and newColumn should have the same name.}
 *
 *  Validates that:
 *  - Column type remains the same.
 *  - Column expression remains the same.
 *  - Column aggregate method either was introduced or remains the same.
 */
void ValidateColumnSchemaUpdate(const TColumnSchema& oldColumn, const TColumnSchema& newColumn)
{
    YCHECK(oldColumn.Name == newColumn.Name);
    if (newColumn.Type != oldColumn.Type) {
        THROW_ERROR_EXCEPTION("Type mismatch for column %Qv: old %Qlv, new %Qlv",
            oldColumn.Name,
            oldColumn.Type,
            newColumn.Type);
    }

    if (newColumn.SortOrder != oldColumn.SortOrder) {
        THROW_ERROR_EXCEPTION("Sort order mismatch for column %Qv: old %Qlv, new %Qlv",
            oldColumn.Name,
            oldColumn.SortOrder,
            newColumn.SortOrder);
    }

    if (newColumn.Expression != oldColumn.Expression) {
        THROW_ERROR_EXCEPTION("Expression mismatch for column %Qv: old %Qv, new %Qv",
            oldColumn.Name,
            oldColumn.Expression,
            newColumn.Expression);
    }

    if (oldColumn.Aggregate && oldColumn.Aggregate != newColumn.Aggregate) {
        THROW_ERROR_EXCEPTION("Aggregate mode mismatch for column %Qv: old %Qv, new %Qv",
            oldColumn.Name,
            oldColumn.Aggregate,
            newColumn.Aggregate);
    }

    if (oldColumn.SortOrder && oldColumn.Lock != newColumn.Lock) {
        THROW_ERROR_EXCEPTION("Lock mismatch for key column %Qv: old %Qv, new %Qv",
            oldColumn.Name,
            oldColumn.Lock,
            newColumn.Lock);
    }
}

////////////////////////////////////////////////////////////////////////////////

void ValidateDynamicTableConstraints(const TTableSchema& schema)
{
    if (!schema.GetStrict()) {
        THROW_ERROR_EXCEPTION("\"strict\" cannot be \"false\" for a dynamic table");
    }

    if (schema.IsSorted() && !schema.GetUniqueKeys()) {
        THROW_ERROR_EXCEPTION("\"unique_keys\" cannot be \"false\" for a sorted dynamic table");
    }

    if (schema.GetKeyColumnCount() == schema.Columns().size()) {
       THROW_ERROR_EXCEPTION("There must be at least one non-key column");
    }

    for (const auto& column : schema.Columns()) {
        if (column.SortOrder && column.Type == EValueType::Any) {
            THROW_ERROR_EXCEPTION("Invalid dynamic table key column type: %Qv",
                column.Type);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

//! Validates that all columns from the old schema are present in the new schema.
void ValidateColumnsNotRemoved(const TTableSchema& oldSchema, const TTableSchema& newSchema)
{
    YCHECK(newSchema.GetStrict());
    for (int oldColumnIndex = 0; oldColumnIndex < oldSchema.Columns().size(); ++oldColumnIndex) {
        const auto& oldColumn = oldSchema.Columns()[oldColumnIndex];
        if (!newSchema.FindColumn(oldColumn.Name)) {
            THROW_ERROR_EXCEPTION("Cannot remove column %Qv from a strict schema",
                oldColumn.Name);
        }
    }
}

//! Validates that all columns from the new schema are present in the old schema.
void ValidateColumnsNotInserted(const TTableSchema& oldSchema, const TTableSchema& newSchema)
{
    YCHECK(!oldSchema.GetStrict());
    for (int newColumnIndex = 0; newColumnIndex < newSchema.Columns().size(); ++newColumnIndex) {
        const auto& newColumn = newSchema.Columns()[newColumnIndex];
        if (!oldSchema.FindColumn(newColumn.Name)) {
            THROW_ERROR_EXCEPTION("Cannot insert a new column %Qv into non-strict schema",
                newColumn.Name);
        }
    }
}

//! Validates that for each column present in both #oldSchema and #newSchema, its declarations match each other.
//! Also validates that key columns positions are not changed.
void ValidateColumnsMatch(const TTableSchema& oldSchema, const TTableSchema& newSchema)
{
    for (int oldColumnIndex = 0; oldColumnIndex < oldSchema.Columns().size(); ++oldColumnIndex) {
        const auto& oldColumn = oldSchema.Columns()[oldColumnIndex];
        const auto* newColumnPtr = newSchema.FindColumn(oldColumn.Name);
        if (!newColumnPtr) {
            // We consider only columns present both in oldSchema and newSchema.
            continue;
        }
        const auto& newColumn = *newColumnPtr;
        ValidateColumnSchemaUpdate(oldColumn, newColumn);
        int newColumnIndex = newSchema.GetColumnIndex(newColumn);
        if (oldColumnIndex < oldSchema.GetKeyColumnCount()) {
            if (oldColumnIndex != newColumnIndex) {
                THROW_ERROR_EXCEPTION("Cannot change position of a key column %Qv: old %v, new %v",
                    oldColumn.Name,
                    oldColumnIndex,
                    newColumnIndex);
            }
        }
    }
}

//! Validates that there are no duplicates among the column names.
void ValidateColumnUniqueness(const TTableSchema& schema)
{
    yhash_set<Stroka> columnNames;
    for (const auto& column : schema.Columns()) {
        if (!columnNames.insert(column.Name).second) {
            THROW_ERROR_EXCEPTION("Duplicate column name %Qv in table schema",
                column.Name);
        }
    }
}

//! Validates that number of locks doesn't exceed #MaxColumnLockCount.
void ValidateLocks(const TTableSchema& schema)
{
    yhash_set<Stroka> lockNames;
    YCHECK(lockNames.insert(PrimaryLockName).second);
    for (const auto& column : schema.Columns()) {
        if (column.Lock) {
            lockNames.insert(*column.Lock);
        }
    }

    if (lockNames.size() > MaxColumnLockCount) {
        THROW_ERROR_EXCEPTION("Too many column locks in table schema: actual %v, limit %v",
            lockNames.size(),
            MaxColumnLockCount);
    }
}

//! Validates that key columns form a prefix of a table schema.
void ValidateKeyColumnsFormPrefix(const TTableSchema& schema)
{
    for (int index = 0; index < schema.GetKeyColumnCount(); ++index) {
        if (!schema.Columns()[index].SortOrder) {
            THROW_ERROR_EXCEPTION("Key columns must form a prefix of schema");
        }
    }
    // The fact that first GetKeyColumnCount() columns have SortOrder automatically
    // implies that the rest of columns don't have SortOrder, so we don't need to check it.
}

//! Validates computed columns.
/*!
 *  Validates that:
 *  - Computed column has to be key column.
 *  - Type of a computed column matches the type of its expression.
 *  - All referenced columns appear in schema, are key columns and are not computed.
 */
void ValidateComputedColumns(const TTableSchema& schema)
{
    // TODO(max42): Passing *this before the object is finally constructed
    // doesn't look like a good idea (although it works :) ). Get rid of this.

    for (int index = 0; index < schema.Columns().size(); ++index) {
        const auto& columnSchema = schema.Columns()[index];
        if (columnSchema.Expression) {
            if (index >= schema.GetKeyColumnCount()) {
                THROW_ERROR_EXCEPTION("Non-key column %Qv can't be computed", columnSchema.Name);
            }
            yhash_set<Stroka> references;
            auto expr = PrepareExpression(columnSchema.Expression.Get(), schema, BuiltinTypeInferrersMap, &references);
            if (expr->Type != columnSchema.Type) {
                THROW_ERROR_EXCEPTION(
                    "Computed column %Qv type mismatch: declared type is %Qlv but expression type is %Qlv",
                    columnSchema.Name,
                    columnSchema.Type,
                    expr->Type);
            }

            for (const auto& ref : references) {
                const auto& refColumn = schema.GetColumnOrThrow(ref);
                if (!refColumn.SortOrder) {
                    THROW_ERROR_EXCEPTION("Computed column %Qv depends on a non-key column %Qv",
                        columnSchema.Name,
                        ref);
                }
                if (refColumn.Expression) {
                    THROW_ERROR_EXCEPTION("Computed column %Qv depends on a computed column %Qv",
                        columnSchema.Name,
                        ref);
                }
            }
        }
    }
}

//! Validates aggregated columns.
/*!
 *  Validates that:
 *  - Aggregated columns are non-key.
 *  - Aggregate function appears in a list of pre-defined aggregate functions.
 *  - Type of an aggregated column matches the type of an aggregate function.
 */
void ValidateAggregatedColumns(const TTableSchema& schema)
{
    for (int index = 0; index < schema.Columns().size(); ++index) {
        const auto& columnSchema = schema.Columns()[index];
        if (columnSchema.Aggregate) {
            if (index < schema.GetKeyColumnCount()) {
                THROW_ERROR_EXCEPTION("Key column %Qv can't be aggregated", columnSchema.Name);
            }

            const auto& name = *columnSchema.Aggregate;
            if (auto descriptor = BuiltinTypeInferrersMap->GetFunction(name)->As<TAggregateTypeInferrer>()) {
                const auto& stateType = descriptor->InferStateType(columnSchema.Type, name, name);
                if (stateType != columnSchema.Type) {
                    THROW_ERROR_EXCEPTION("Aggregate function %Qv state type %Qv differs from column %Qv type %Qv",
                        columnSchema.Aggregate.Get(),
                        columnSchema.Type,
                        columnSchema.Name,
                        stateType);
                }
            } else {
                THROW_ERROR_EXCEPTION("Unknown aggregate function %Qv at column %Qv",
                    columnSchema.Aggregate.Get(),
                    columnSchema.Name);
            }
        }
    }
}

//! Validates |$timestamp| column, if any.
/*!
 *  Validate that:
 *  - |$timestamp| column cannot be a part of key.
 *  - |$timestamp| column can only be present in unsorted tables.
 *  - |$timestamp| column has type |uint64|.
 */
void ValidateTimestampColumn(const TTableSchema& schema)
{
    auto* column = schema.FindColumn(TimestampColumnName);
    if (!column) {
        return;
    }

    if (column->SortOrder) {
        THROW_ERROR_EXCEPTION("%Qv column cannot be a part of key",
            TimestampColumnName);
    }

    if (column->Type != EValueType::Uint64) {
        THROW_ERROR_EXCEPTION("%Qv column must have %Qlv type",
            TimestampColumnName,
            EValueType::Uint64);
    }

    if (schema.IsSorted()) {
        THROW_ERROR_EXCEPTION("%Qv column cannot appear in a sorted table",
            TimestampColumnName);
    }
}

// Validate schema attributes.
void ValidateSchemaAttributes(const TTableSchema& schema)
{
    if (schema.GetUniqueKeys() && schema.GetKeyColumnCount() == 0) {
        THROW_ERROR_EXCEPTION("\"unique_keys\" can only be true if key columns are present");
    }
}

void ValidateTableSchema(const TTableSchema& schema)
{
    for (const auto& column : schema.Columns()) {
        ValidateColumnSchema(column);
    }
    ValidateColumnUniqueness(schema);
    ValidateLocks(schema);
    ValidateKeyColumnsFormPrefix(schema);
    ValidateComputedColumns(schema);
    ValidateAggregatedColumns(schema);
    ValidateTimestampColumn(schema);
    ValidateSchemaAttributes(schema);
}

//! TODO(max42): document this functions somewhere (see also https://st.yandex-team.ru/YT-1433).
void ValidateTableSchemaUpdate(
    const TTableSchema& oldSchema,
    const TTableSchema& newSchema,
    bool isTableDynamic,
    bool isTableEmpty)
{
    ValidateTableSchema(newSchema);

    if (isTableDynamic) {
        ValidateDynamicTableConstraints(newSchema);
    }

    if (isTableEmpty) {
        // Any valid schema is allowed to be set for an empty table.
        return;
    }

    if (oldSchema.GetKeyColumnCount() > 0 && newSchema.GetKeyColumnCount() == 0) {
        THROW_ERROR_EXCEPTION("Cannot change schema from sorted to unsorted");
    }
    if (oldSchema.GetKeyColumnCount() == 0 && newSchema.GetKeyColumnCount() > 0) {
        THROW_ERROR_EXCEPTION("Cannot change schema from unsorted to sorted");
    }
    if (!oldSchema.GetStrict() && newSchema.GetStrict()) {
        THROW_ERROR_EXCEPTION("Changing \"strict\" from \"false\" to \"true\" is not allowed");
    }
    if (!oldSchema.GetUniqueKeys() && newSchema.GetUniqueKeys()) {
        THROW_ERROR_EXCEPTION("Changing \"unique_keys\" from \"false\" to \"true\" is not allowed");
    }

    if (oldSchema.GetStrict() && !newSchema.GetStrict()) {
        if (oldSchema.Columns() != newSchema.Columns()) {
            THROW_ERROR_EXCEPTION("Changing columns is not allowed while changing \"strict\" from \"true\" to \"false\"");
        }
        return;
    }

    if (oldSchema.GetStrict()) {
        ValidateColumnsNotRemoved(oldSchema, newSchema);
    } else {
        ValidateColumnsNotInserted(oldSchema, newSchema);
    }
    ValidateColumnsMatch(oldSchema, newSchema);

    // We allow adding computed columns only on creation of the table.
    if (!oldSchema.Columns().empty() || !isTableEmpty) {
        for (const auto& newColumn : newSchema.Columns()) {
            if (!oldSchema.FindColumn(newColumn.Name)) {
                if (newColumn.Expression) {
                    THROW_ERROR_EXCEPTION("Cannot introduce a new computed column %Qv after creation",
                        newColumn.Name);
                }
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

void ValidatePivotKey(const TOwningKey& pivotKey, const TTableSchema& schema)
{
    if (pivotKey.GetCount() > schema.GetKeyColumnCount()) {
        THROW_ERROR_EXCEPTION("Pivot key must form a prefix of key");
    }

    for (int index = 0; index < pivotKey.GetCount(); ++index) {
        if (pivotKey[index].Type != schema.Columns()[index].Type) {
            THROW_ERROR_EXCEPTION(
                "Mismatched type of column %Qv in pivot key: expected %Qlv, found %Qlv",
                schema.Columns()[index].Name,
                schema.Columns()[index].Type,
                pivotKey[index].Type);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

EValueType GetCommonValueType(EValueType lhs, EValueType rhs) {
    if (lhs == EValueType::Null) {
        return rhs;
    } else if (rhs == EValueType::Null) {
        return lhs;
    } else if (lhs == rhs) {
        return lhs;
    } else {
        return EValueType::Any;
    }
}

TTableSchema InferInputSchema(const std::vector<TTableSchema>& schemas, bool discardKeyColumns)
{
    YCHECK(!schemas.empty());

    int commonKeyColumnPrefix = 0;
    if (!discardKeyColumns) {
        while (true) {
            if (commonKeyColumnPrefix >= schemas.front().GetKeyColumnCount()) {
                break;
            }
            const auto& keyColumnName = schemas.front().Columns()[commonKeyColumnPrefix].Name;
            bool mismatch = false;
            for (const auto& schema : schemas) {
                if (commonKeyColumnPrefix >= schema.GetKeyColumnCount() ||
                    schema.Columns()[commonKeyColumnPrefix].Name != keyColumnName)
                {
                    mismatch = true;
                    break;
                }
            }
            if (mismatch) {
                break;
            }
            ++commonKeyColumnPrefix;
        }
    }

    yhash_map<Stroka, TColumnSchema> nameToColumnSchema;
    std::vector<Stroka> columnNames;

    for (const auto& schema : schemas) {
        for (int columnIndex = 0; columnIndex < schema.Columns().size(); ++columnIndex) {
            auto column = schema.Columns()[columnIndex];
            if (columnIndex >= commonKeyColumnPrefix) {
                column = column.SetSortOrder(Null);
            }
            column = column
                .SetExpression(Null)
                .SetLock(Null);

            auto it = nameToColumnSchema.find(column.Name);
            if (it == nameToColumnSchema.end()) {
                nameToColumnSchema[column.Name] = column;
                columnNames.push_back(column.Name);
            } else {
                auto commonType = GetCommonValueType(it->second.Type, column.Type);
                column.Type = it->second.Type = commonType;
                if (it->second != column) {
                    THROW_ERROR_EXCEPTION(
                        "Conflict while merging schemas, column %Qs has two conflicting declarations",
                        column.Name)
                        << TErrorAttribute("first_column_schema", it->second)
                        << TErrorAttribute("second_column_schema", column);
                }
            }
        }
    }

    std::vector<TColumnSchema> columns;
    for (auto columnName : columnNames) {
        columns.push_back(nameToColumnSchema[columnName]);
    }

    bool strict = true;
    for (const auto& schema : schemas) {
        if (!schema.GetStrict()) {
            strict = false;
        }
    }

    return TTableSchema(std::move(columns), strict);
}

//! Validates that read schema is consistent with existing table schema.
/*!
 *  Validates that:
 *  - If a column is present in both schemas, column types should be the same.
 *  - Either one of two possibilties holds:
 *    - #tableSchema.GetKeyColumns() is a prefix of #readSchema.GetKeyColumns()
 *    - #readSchema.GetKeyColumns() is a proper subset of #tableSchema.GetKeyColumns() and
 *      no extra key column in #readSchema appears in #tableSchema as a non-key column.
 */
void ValidateReadSchema(const TTableSchema& readSchema, const TTableSchema& tableSchema)
{
    for (int readColumnIndex = 0;
         readColumnIndex < static_cast<int>(tableSchema.Columns().size());
         ++readColumnIndex) {
        const auto& readColumn = readSchema.Columns()[readColumnIndex];
        const auto* tableColumnPtr = tableSchema.FindColumn(readColumn.Name);
        if (!tableColumnPtr) {
            continue;
        }

        // Validate column type consistency in two schemas.
        const auto& tableColumn = *tableColumnPtr;
        if (readColumn.Type != EValueType::Any &&
            tableColumn.Type != EValueType::Any &&
            readColumn.Type != tableColumn.Type)
        {
            THROW_ERROR_EXCEPTION(
                "Mismatched type of column %Qv in read schema: expected %Qlv, found %Qlv",
                readColumn.Name,
                tableColumn.Type,
                readColumn.Type);
        }

        // Validate that order of key columns intersection hasn't been changed.
        int tableColumnIndex = tableSchema.GetColumnIndex(tableColumn);
        if (readColumnIndex < readSchema.GetKeyColumnCount() &&
            tableColumnIndex < readSchema.GetKeyColumnCount() &&
            readColumnIndex != tableColumnIndex)
        {
            THROW_ERROR_EXCEPTION(
                "Key column %Qv position mismatch: its position is %v in table schema and %v in read schema",
                readColumn.Name,
                tableColumnIndex,
                readColumnIndex);
        }

        // Validate that a non-key column in tableSchema can't become a key column in readSchema.
        if (readColumnIndex < readSchema.GetKeyColumnCount() &&
            tableColumnIndex >= tableSchema.GetKeyColumnCount())
        {
            THROW_ERROR_EXCEPTION(
                "Column %Qv is declared as non-key in table schema and as a key in read schema",
                readColumn.Name);
        }
    }

    if (readSchema.GetKeyColumnCount() > tableSchema.GetKeyColumnCount() && !tableSchema.GetStrict()) {
        THROW_ERROR_EXCEPTION(
            "Table schema is not strict but read schema contains key column %Qv not present in table schema",
            readSchema.Columns()[tableSchema.GetKeyColumnCount()].Name);
    }
}


TError ValidateTableSchemaCompatibility(
    const TTableSchema& inputSchema,
    const TTableSchema& outputSchema,
    bool ignoreSortOrder)
{
    auto addAttributes = [&] (TError error) {
        return error
            << TErrorAttribute("input_table_schema", inputSchema)
            << TErrorAttribute("output_table_schema", outputSchema);
    };

    // If output schema is strict, check that input columns are subset of output columns.
    if (outputSchema.GetStrict()) {
        if (!inputSchema.GetStrict()) {
            return addAttributes(TError("Input schema is not strict"));
        }

        for (const auto& inputColumn : inputSchema.Columns()) {
            if (!outputSchema.FindColumn(inputColumn.Name)) {
                return addAttributes(TError("Unexpected column %Qv in input schema",
                    inputColumn.Name));
            }
        }
    }

    // Check that column types are the same.
    for (const auto& outputColumn : outputSchema.Columns()) {
        if (auto inputColumn = inputSchema.FindColumn(outputColumn.Name)) {
            if (inputColumn->Type != outputColumn.Type && outputColumn.Type != EValueType::Any) {
                return addAttributes(TError("Column %Qv input type is incompatible with the output type",
                    inputColumn->Name));
            }
        }
    }

    if (ignoreSortOrder) {
        return TError();
    }

    // Check that output key columns form a proper prefix of input key columns.
    int cmp = outputSchema.GetKeyColumnCount() - inputSchema.GetKeyColumnCount();
    if (cmp > 0) {
        return addAttributes(TError("Output key columns are wider than input key columns"));
    }

    if (outputSchema.GetUniqueKeys()) {
        if (!inputSchema.GetUniqueKeys()) {
            return addAttributes(TError("Input schema \"unique_keys\" attribute is false"));
        }
        if (cmp != 0) {
            return addAttributes(TError("Input key columns are wider than output key columns"));
        }
    }

    auto inputKeyColumns = inputSchema.GetKeyColumns();
    auto outputKeyColumns = outputSchema.GetKeyColumns();

    for (int index = 0; index < outputKeyColumns.size(); ++index) {
        if (inputKeyColumns[index] != outputKeyColumns[index]) {
            return addAttributes(TError("Input sorting order is incompatible with the output"));
        }
    }

    return TError();
}

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

using NYT::ToProto;
using NYT::FromProto;

void ToProto(TKeyColumnsExt* protoKeyColumns, const TKeyColumns& keyColumns)
{
    ToProto(protoKeyColumns->mutable_names(), keyColumns);
}

void FromProto(TKeyColumns* keyColumns, const TKeyColumnsExt& protoKeyColumns)
{
    *keyColumns = FromProto<TKeyColumns>(protoKeyColumns.names());
}

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
