#include "schema.h"
#include "unversioned_row.h"

#include <yt/yt/core/yson/public.h>
#include <yt/yt/core/yson/pull_parser_deserialize.h>

#include <yt/yt/core/ytree/serialize.h>
#include <yt/yt/core/ytree/convert.h>
#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/yson_serializable.h>

#include <yt/yt/client/table_client/proto/chunk_meta.pb.h>
#include <yt/yt/client/table_client/proto/wire_protocol.pb.h>

#include <yt/yt/client/tablet_client/public.h>

namespace NYT::NTableClient {

using namespace NYTree;
using namespace NYson;
using namespace NChunkClient;
using namespace NTabletClient;

using NYT::ToProto;
using NYT::FromProto;

/////////////////////////////////////////////////////////////////////////////

TLockMask MaxMask(TLockMask lhs, TLockMask rhs)
{
    TLockMask result;
    for (int index = 0; index < TLockMask::MaxCount; ++index) {
        result.Set(index, std::max(lhs.Get(index), rhs.Get(index)));
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

TColumnSchema::TColumnSchema()
    : TColumnSchema(TString(), NullLogicalType, std::nullopt)
{ }

TColumnSchema::TColumnSchema(
    const TString& name,
    EValueType type,
    std::optional<ESortOrder> sortOrder)
    : TColumnSchema(name, MakeLogicalType(GetLogicalType(type), /*required*/ false), sortOrder)
{ }

TColumnSchema::TColumnSchema(
    const TString& name,
    ESimpleLogicalValueType type,
    std::optional<ESortOrder> sortOrder)
    : TColumnSchema(name, MakeLogicalType(type, /*required*/ false), sortOrder)
{ }

TColumnSchema::TColumnSchema(
    const TString& name,
    TLogicalTypePtr type,
    std::optional<ESortOrder> sortOrder)
    : Name_(name)
    , SortOrder_(sortOrder)
{
    SetLogicalType(std::move(type));
}

TColumnSchema& TColumnSchema::SetName(const TString& value)
{
    Name_ = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetSortOrder(const std::optional<ESortOrder>& value)
{
    SortOrder_ = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetLock(const std::optional<TString>& value)
{
    Lock_ = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetGroup(const std::optional<TString>& value)
{
    Group_ = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetExpression(const std::optional<TString>& value)
{
    Expression_ = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetAggregate(const std::optional<TString>& value)
{
    Aggregate_ = value;
    return *this;
}

TColumnSchema& TColumnSchema::SetLogicalType(TLogicalTypePtr type)
{
    LogicalType_ = std::move(type);
    IsOfV1Type_ = IsV1Type(LogicalType_);
    std::tie(V1Type_, Required_) = NTableClient::CastToV1Type(LogicalType_);
    return *this;
}

EValueType TColumnSchema::GetPhysicalType() const
{
    return NTableClient::GetPhysicalType(V1Type_);
}

i64 TColumnSchema::GetMemoryUsage() const
{
    return sizeof(TColumnSchema) +
        Name_.size() +
        (LogicalType_ ? LogicalType_->GetMemoryUsage() : 0) +
        (Lock_ ? Lock_->size() : 0) +
        (Expression_ ? Expression_->size() : 0) +
        (Aggregate_ ? Aggregate_->size() : 0) +
        (Group_ ? Group_->size() : 0);
}

bool TColumnSchema::IsOfV1Type() const
{
    return IsOfV1Type_;
}

bool TColumnSchema::IsOfV1Type(ESimpleLogicalValueType type) const
{
    return IsOfV1Type_ && V1Type_ == type;
}

ESimpleLogicalValueType TColumnSchema::CastToV1Type() const
{
    return V1Type_;
}

////////////////////////////////////////////////////////////////////////////////

static void RunColumnSchemaPostprocessor(
    TColumnSchema& schema,
    std::optional<ESimpleLogicalValueType> logicalTypeV1,
    std::optional<bool> requiredV1,
    const TLogicalTypePtr& logicalTypeV3)
{
    // Name
    if (schema.Name().empty()) {
        THROW_ERROR_EXCEPTION("Column name cannot be empty");
    }

    try {
        int setTypeVersion = 0;
        if (logicalTypeV3) {
            schema.SetLogicalType(logicalTypeV3);
            setTypeVersion = 3;
        }

        if (logicalTypeV1) {
            if (setTypeVersion == 0) {
                schema.SetLogicalType(MakeLogicalType(*logicalTypeV1, requiredV1.value_or(false)));
                setTypeVersion = 1;
            } else {
                if (*logicalTypeV1 != schema.CastToV1Type()) {
                    THROW_ERROR_EXCEPTION(
                        "\"type_v%v\" doesn't match \"type\"; \"type_v%v\": %Qv \"type\": %Qlv expected \"type\": %Qlv",
                        setTypeVersion,
                        setTypeVersion,
                        *schema.LogicalType(),
                        *logicalTypeV1,
                        schema.CastToV1Type());
                }
            }
        }

        if (requiredV1 && setTypeVersion > 1 && *requiredV1 != schema.Required()) {
            THROW_ERROR_EXCEPTION(
                "\"type_v%v\" doesn't match \"required\"; \"type_v%v\": %Qv \"required\": %Qlv",
                setTypeVersion,
                setTypeVersion,
                *schema.LogicalType(),
                *requiredV1);
        }

        if (setTypeVersion == 0) {
            THROW_ERROR_EXCEPTION("Column type is not specified");
        }

        if (*DetagLogicalType(schema.LogicalType()) == *SimpleLogicalType(ESimpleLogicalValueType::Any)) {
            THROW_ERROR_EXCEPTION("Column of type %Qlv cannot be \"required\"",
                ESimpleLogicalValueType::Any);
        }

        // Lock
        if (schema.Lock() && schema.Lock()->empty()) {
            THROW_ERROR_EXCEPTION("Lock name cannot be empty");
        }

        // Group
        if (schema.Group() && schema.Group()->empty()) {
            THROW_ERROR_EXCEPTION("Group name cannot be empty");
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error validating column %Qv in table schema",
            schema.Name())
            << ex;
    }
}

struct TSerializableColumnSchema
    : public TYsonSerializableLite
    , private TColumnSchema
{
    TSerializableColumnSchema()
    {
        RegisterParameter("name", Name_)
            .NonEmpty();
        RegisterParameter("type", LogicalTypeV1_)
            .Default(std::nullopt);
        RegisterParameter("required", RequiredV1_)
            .Default(std::nullopt);
        RegisterParameter("type_v3", LogicalTypeV3_)
            .Default();
        RegisterParameter("lock", Lock_)
            .Default();
        RegisterParameter("expression", Expression_)
            .Default();
        RegisterParameter("aggregate", Aggregate_)
            .Default();
        RegisterParameter("sort_order", SortOrder_)
            .Default();
        RegisterParameter("group", Group_)
            .Default();

        RegisterPostprocessor([&] () {
            RunColumnSchemaPostprocessor(
                *this,
                LogicalTypeV1_,
                RequiredV1_,
                LogicalTypeV3_ ? LogicalTypeV3_->LogicalType : nullptr
            );
        });
    }

public:
    void SetColumnSchema(const TColumnSchema& columnSchema)
    {
        static_cast<TColumnSchema&>(*this) = columnSchema;
        LogicalTypeV1_ = columnSchema.CastToV1Type();
        RequiredV1_ = columnSchema.Required();
        LogicalTypeV3_ = TTypeV3LogicalTypeWrapper{columnSchema.LogicalType()};
    }

    const TColumnSchema& GetColumnSchema() const
    {
        return *this;
    }

private:
    std::optional<ESimpleLogicalValueType> LogicalTypeV1_;
    std::optional<bool> RequiredV1_;

    std::optional<TTypeV3LogicalTypeWrapper> LogicalTypeV3_;
};

void FormatValue(TStringBuilderBase* builder, const TColumnSchema& schema, TStringBuf spec)
{
    builder->AppendChar('{');

    builder->AppendFormat("name=%Qv", schema.Name());

    if (const auto& logicalType = schema.LogicalType()) {
        builder->AppendFormat("; type=%v", *logicalType);
    }

    if (const auto& sortOrder = schema.SortOrder()) {
        builder->AppendFormat("; sort_order=%v", *sortOrder);
    }

    if (const auto& lock = schema.Lock()) {
        builder->AppendFormat("; lock=%v", *lock);
    }

    if (const auto& expression = schema.Expression()) {
        builder->AppendFormat("; expression=%Qv", *expression);
    }

    if (const auto& aggregate = schema.Aggregate()) {
        builder->AppendFormat("; aggregate=%v", *aggregate);
    }

    if (const auto& group = schema.Group()) {
        builder->AppendFormat("; group=%v", *group);
    }

    builder->AppendFormat("; physical_type=%v", CamelCaseToUnderscoreCase(ToString(schema.CastToV1Type())));

    builder->AppendFormat("; required=%v", schema.Required());

    builder->AppendChar('}');
}

void Serialize(const TColumnSchema& schema, IYsonConsumer* consumer)
{
    TSerializableColumnSchema wrapper;
    wrapper.SetColumnSchema(schema);
    Serialize(static_cast<const TYsonSerializableLite&>(wrapper), consumer);
}

void Deserialize(TColumnSchema& schema, INodePtr node)
{
    TSerializableColumnSchema wrapper;
    Deserialize(static_cast<TYsonSerializableLite&>(wrapper), node);
    schema = wrapper.GetColumnSchema();
}

void Deserialize(TColumnSchema& schema, NYson::TYsonPullParserCursor* cursor)
{
    std::optional<ESimpleLogicalValueType> logicalTypeV1;
    std::optional<bool> requiredV1;
    TLogicalTypePtr logicalTypeV3;

    cursor->ParseMap([&] (NYson::TYsonPullParserCursor* cursor) {
        EnsureYsonToken("column schema attribute key", *cursor, EYsonItemType::StringValue);
        auto key = (*cursor)->UncheckedAsString();
        if (key == TStringBuf("name")) {
            cursor->Next();
            schema.SetName(ExtractTo<TString>(cursor));
        } else if (key == TStringBuf("required")) {
            cursor->Next();
            requiredV1 = ExtractTo<bool>(cursor);
        } else if (key == TStringBuf("type")) {
            cursor->Next();
            logicalTypeV1 = ExtractTo<ESimpleLogicalValueType>(cursor);
        } else if (key == TStringBuf("type_v3")) {
            cursor->Next();
            DeserializeV3(logicalTypeV3, cursor);
        } else if (key == TStringBuf("lock")) {
            cursor->Next();
            schema.SetLock(ExtractTo<std::optional<TString>>(cursor));
        } else if (key == TStringBuf("expression")) {
            cursor->Next();
            schema.SetExpression(ExtractTo<std::optional<TString>>(cursor));
        } else if (key == TStringBuf("aggregate")) {
            cursor->Next();
            schema.SetAggregate(ExtractTo<std::optional<TString>>(cursor));
        } else if (key == TStringBuf("sort_order")) {
            cursor->Next();
            schema.SetSortOrder(ExtractTo<std::optional<ESortOrder>>(cursor));
        } else if (key == TStringBuf("group")) {
            cursor->Next();
            schema.SetGroup(ExtractTo<std::optional<TString>>(cursor));
        } else {
            cursor->Next();
            cursor->SkipComplexValue();
        }
    });

    RunColumnSchemaPostprocessor(schema, logicalTypeV1, requiredV1, logicalTypeV3);
}

void ToProto(NProto::TColumnSchema* protoSchema, const TColumnSchema& schema)
{
    protoSchema->set_name(schema.Name());
    protoSchema->set_type(static_cast<int>(schema.GetPhysicalType()));

    if (schema.IsOfV1Type()) {
        protoSchema->set_simple_logical_type(static_cast<int>(schema.CastToV1Type()));
    }
    if (schema.Required()) {
        protoSchema->set_required(true);
    }
    ToProto(protoSchema->mutable_logical_type(), schema.LogicalType());

    if (schema.Lock()) {
        protoSchema->set_lock(*schema.Lock());
    }
    if (schema.Expression()) {
        protoSchema->set_expression(*schema.Expression());
    }
    if (schema.Aggregate()) {
        protoSchema->set_aggregate(*schema.Aggregate());
    }
    if (schema.SortOrder()) {
        protoSchema->set_sort_order(static_cast<int>(*schema.SortOrder()));
    }
    if (schema.Group()) {
        protoSchema->set_group(*schema.Group());
    }
}

void FromProto(TColumnSchema* schema, const NProto::TColumnSchema& protoSchema)
{
    schema->SetName(protoSchema.name());

    if (protoSchema.has_logical_type()) {
        TLogicalTypePtr logicalType;
        FromProto(&logicalType, protoSchema.logical_type());
        schema->SetLogicalType(logicalType);
    } else if (protoSchema.has_simple_logical_type()) {
        schema->SetLogicalType(
            MakeLogicalType(
                CheckedEnumCast<ESimpleLogicalValueType>(protoSchema.simple_logical_type()),
                protoSchema.required()));
    } else {
        auto physicalType = CheckedEnumCast<EValueType>(protoSchema.type());
        schema->SetLogicalType(MakeLogicalType(GetLogicalType(physicalType), protoSchema.required()));
    }

    schema->SetLock(protoSchema.has_lock() ? std::make_optional(protoSchema.lock()) : std::nullopt);
    schema->SetExpression(protoSchema.has_expression() ? std::make_optional(protoSchema.expression()) : std::nullopt);
    schema->SetAggregate(protoSchema.has_aggregate() ? std::make_optional(protoSchema.aggregate()) : std::nullopt);
    schema->SetSortOrder(protoSchema.has_sort_order() ? std::make_optional(CheckedEnumCast<ESortOrder>(protoSchema.sort_order())) : std::nullopt);
    schema->SetGroup(protoSchema.has_group() ? std::make_optional(protoSchema.group()) : std::nullopt);
}

////////////////////////////////////////////////////////////////////////////////

TTableSchema::TTableSchema()
    : Strict_(false)
    , UniqueKeys_(false)
    , SchemaModification_(ETableSchemaModification::None)
{ }

TTableSchema::TTableSchema(
    std::vector<TColumnSchema> columns,
    bool strict,
    bool uniqueKeys,
    ETableSchemaModification schemaModification)
    : Columns_(std::move(columns))
    , Strict_(strict)
    , UniqueKeys_(uniqueKeys)
    , SchemaModification_(schemaModification)
{
    for (const auto& column : Columns_) {
        if (column.SortOrder()) {
            ++KeyColumnCount_;
        }
    }
}

const TColumnSchema* TTableSchema::FindColumn(TStringBuf name) const
{
    for (auto& column : Columns_) {
        if (column.Name() == name) {
            return &column;
        }
    }
    return nullptr;
}

const TColumnSchema& TTableSchema::GetColumn(TStringBuf name) const
{
    auto* column = FindColumn(name);
    YT_VERIFY(column);
    return *column;
}

const TColumnSchema& TTableSchema::GetColumnOrThrow(TStringBuf name) const
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

int TTableSchema::GetColumnIndex(TStringBuf name) const
{
    return GetColumnIndex(GetColumn(name));
}

int TTableSchema::GetColumnIndexOrThrow(TStringBuf name) const
{
    return GetColumnIndex(GetColumnOrThrow(name));
}

TTableSchemaPtr TTableSchema::Filter(const TColumnFilter& columnFilter, bool discardSortOrder) const
{
    int newKeyColumnCount = 0;
    std::vector<TColumnSchema> columns;

    if (columnFilter.IsUniversal()) {
        if (!discardSortOrder) {
            return New<TTableSchema>(*this);
        } else {
            columns = Columns_;
            for (auto& column : columns) {
                column.SetSortOrder(std::nullopt);
            }
        }
    } else {
        bool inKeyColumns = !discardSortOrder;
        for (int id : columnFilter.GetIndexes()) {
            if (id < 0 || id >= Columns_.size()) {
                THROW_ERROR_EXCEPTION("Invalid column during schema filtering: excepted in range [0, %v), got %v",
                    Columns_.size(),
                    id);
            }

            if (id != columns.size() || !Columns_[id].SortOrder()) {
                inKeyColumns = false;
            }

            columns.push_back(Columns_[id]);

            if (!inKeyColumns) {
                columns.back().SetSortOrder(std::nullopt);
            }

            if (columns.back().SortOrder()) {
                ++newKeyColumnCount;
            }
        }
    }

    return New<TTableSchema>(
        std::move(columns),
        Strict_,
        UniqueKeys_ && (newKeyColumnCount == GetKeyColumnCount()));
}

TTableSchemaPtr TTableSchema::Filter(const THashSet<TString>& columns, bool discardSortOrder) const
{
    TColumnFilter::TIndexes indexes;
    for (const auto& column : Columns()) {
        if (columns.find(column.Name()) != columns.end()) {
            indexes.push_back(GetColumnIndex(column));
        }
    }
    return Filter(TColumnFilter(std::move(indexes)), discardSortOrder);
}

TTableSchemaPtr TTableSchema::Filter(const std::optional<std::vector<TString>>& columns, bool discardSortOrder) const
{
    if (!columns) {
        return Filter(TColumnFilter(), discardSortOrder);
    }

    return Filter(THashSet<TString>(columns->begin(), columns->end()), discardSortOrder);
}

bool TTableSchema::HasComputedColumns() const
{
    for (const auto& column : Columns()) {
        if (column.Expression()) {
            return true;
        }
    }
    return false;
}

bool TTableSchema::IsSorted() const
{
    return KeyColumnCount_ > 0;
}

bool TTableSchema::IsUniqueKeys() const
{
    return UniqueKeys_;
}

TKeyColumns TTableSchema::GetKeyColumns() const
{
    TKeyColumns keyColumns;
    for (const auto& column : Columns()) {
        if (column.SortOrder()) {
            keyColumns.push_back(column.Name());
        }
    }
    return keyColumns;
}

int TTableSchema::GetColumnCount() const
{
    return static_cast<int>(Columns_.size());
}

std::vector<TString> TTableSchema::GetColumnNames() const
{
    std::vector<TString> result;
    result.reserve(Columns_.size());
    for (const auto& column : Columns_) {
        result.push_back(column.Name());
    }
    return result;
}

int TTableSchema::GetKeyColumnCount() const
{
    return KeyColumnCount_;
}

int TTableSchema::GetValueColumnCount() const
{
    return GetColumnCount() - GetKeyColumnCount();
}

TSortColumns TTableSchema::GetSortColumns() const
{
    TSortColumns sortColumns;
    sortColumns.reserve(GetKeyColumnCount());

    for (const auto& column : Columns()) {
        if (column.SortOrder()) {
            sortColumns.push_back(TColumnSortSchema{
                .Name = column.Name(),
                .SortOrder = *column.SortOrder()
            });
        }
    }

    return sortColumns;
}

TTableSchemaPtr TTableSchema::SetKeyColumnCount(int keyColumnCount) const
{
    auto schema = *this;

    for (int columnIndex = 0; columnIndex < schema.GetColumnCount(); ++columnIndex) {
        auto& column = schema.Columns_[columnIndex];
        if (columnIndex < keyColumnCount) {
            column.SetSortOrder(ESortOrder::Ascending);
        } else {
            column.SetSortOrder(std::nullopt);
        }
    }

    schema.KeyColumnCount_ = keyColumnCount;

    return New<TTableSchema>(std::move(schema));
}

TTableSchemaPtr TTableSchema::SetUniqueKeys(bool uniqueKeys) const
{
    auto schema = *this;
    schema.UniqueKeys_ = uniqueKeys;
    return New<TTableSchema>(std::move(schema));
}

TTableSchemaPtr TTableSchema::SetSchemaModification(ETableSchemaModification schemaModification) const
{
    auto schema = *this;
    schema.SchemaModification_ = schemaModification;
    return New<TTableSchema>(std::move(schema));
}

bool TTableSchema::HasNontrivialSchemaModification() const
{
    return GetSchemaModification() != ETableSchemaModification::None;
}

TTableSchemaPtr TTableSchema::FromKeyColumns(const TKeyColumns& keyColumns)
{
    TTableSchema schema;
    for (const auto& columnName : keyColumns) {
        schema.Columns_.push_back(
            TColumnSchema(columnName, ESimpleLogicalValueType::Any)
                .SetSortOrder(ESortOrder::Ascending));
    }
    schema.KeyColumnCount_ = keyColumns.size();
    ValidateTableSchema(schema);
    return New<TTableSchema>(std::move(schema));
}

TTableSchemaPtr TTableSchema::FromSortColumns(const TSortColumns& sortColumns)
{
    TTableSchema schema;
    for (const auto& sortColumn : sortColumns) {
        schema.Columns_.push_back(
            TColumnSchema(sortColumn.Name, ESimpleLogicalValueType::Any)
                .SetSortOrder(sortColumn.SortOrder));
    }
    schema.KeyColumnCount_ = sortColumns.size();
    ValidateTableSchema(schema);
    return New<TTableSchema>(std::move(schema));
}

TTableSchemaPtr TTableSchema::ToQuery() const
{
    if (IsSorted()) {
        return New<TTableSchema>(*this);
    } else {
        std::vector<TColumnSchema> columns {
            TColumnSchema(TabletIndexColumnName, ESimpleLogicalValueType::Int64)
                .SetSortOrder(ESortOrder::Ascending),
            TColumnSchema(RowIndexColumnName, ESimpleLogicalValueType::Int64)
                .SetSortOrder(ESortOrder::Ascending)
        };
        columns.insert(columns.end(), Columns_.begin(), Columns_.end());
        return New<TTableSchema>(std::move(columns));
    }
}

TTableSchemaPtr TTableSchema::ToWrite() const
{
    std::vector<TColumnSchema> columns;
    if (IsSorted()) {
        for (const auto& column : Columns_) {
            if (!column.Expression()) {
                columns.push_back(column);
            }
        }
    } else {
        columns.push_back(TColumnSchema(TabletIndexColumnName, ESimpleLogicalValueType::Int64)
            .SetSortOrder(ESortOrder::Ascending));
        for (const auto& column : Columns_) {
            if (column.Name() != TimestampColumnName) {
                columns.push_back(column);
            }
        }
    }
    return New<TTableSchema>(std::move(columns), Strict_, UniqueKeys_);
}

TTableSchemaPtr TTableSchema::WithTabletIndex() const
{
    if (IsSorted()) {
        return New<TTableSchema>(*this);
    } else {
        auto columns = Columns_;
        columns.push_back(TColumnSchema(TabletIndexColumnName, ESimpleLogicalValueType::Int64));

        return New<TTableSchema>(std::move(columns), Strict_, UniqueKeys_);
    }
}

TTableSchemaPtr TTableSchema::ToVersionedWrite() const
{
    return New<TTableSchema>(*this);
}

TTableSchemaPtr TTableSchema::ToLookup() const
{
    std::vector<TColumnSchema> columns;
    for (const auto& column : Columns_) {
        if (column.SortOrder() && !column.Expression()) {
            columns.push_back(column);
        }
    }
    return New<TTableSchema>(std::move(columns), Strict_, UniqueKeys_);
}

TTableSchemaPtr TTableSchema::ToDelete() const
{
    return ToLookup();
}

TTableSchemaPtr TTableSchema::ToKeys() const
{
    std::vector<TColumnSchema> columns(Columns_.begin(), Columns_.begin() + KeyColumnCount_);
    return New<TTableSchema>(std::move(columns), Strict_, UniqueKeys_);
}

TTableSchemaPtr TTableSchema::ToValues() const
{
    std::vector<TColumnSchema> columns(Columns_.begin() + KeyColumnCount_, Columns_.end());
    return New<TTableSchema>(std::move(columns), Strict_, false);
}

TTableSchemaPtr TTableSchema::ToUniqueKeys() const
{
    return New<TTableSchema>(Columns_, Strict_, /*uniqueKeys*/ true);
}

TTableSchemaPtr TTableSchema::ToStrippedColumnAttributes() const
{
    std::vector<TColumnSchema> strippedColumns;
    for (auto& column : Columns_) {
        strippedColumns.emplace_back(column.Name(), column.LogicalType());
    }
    return New<TTableSchema>(strippedColumns, Strict_, /*uniqueKeys*/ false);
}

TTableSchemaPtr TTableSchema::ToSortedStrippedColumnAttributes() const
{
    std::vector<TColumnSchema> strippedColumns;
    for (auto& column : Columns_) {
        strippedColumns.emplace_back(column.Name(), column.LogicalType(), column.SortOrder());
    }
    return New<TTableSchema>(strippedColumns, Strict_, UniqueKeys_);
}

TTableSchemaPtr TTableSchema::ToCanonical() const
{
    auto columns = Columns();
    std::sort(
        columns.begin() + KeyColumnCount_,
        columns.end(),
        [] (const TColumnSchema& lhs, const TColumnSchema& rhs) {
            return lhs.Name() < rhs.Name();
        });
    return New<TTableSchema>(columns, Strict_, UniqueKeys_);
}

TTableSchemaPtr TTableSchema::ToSorted(const TKeyColumns& keyColumns) const
{
    TSortColumns sortColumns;
    sortColumns.reserve(keyColumns.size());
    for (const auto& keyColumn : keyColumns) {
        sortColumns.push_back(TColumnSortSchema{
            .Name = keyColumn,
            .SortOrder = ESortOrder::Ascending
        });
    }

    return TTableSchema::ToSorted(sortColumns);
}

TTableSchemaPtr TTableSchema::ToSorted(const TSortColumns& sortColumns) const
{
    int oldKeyColumnCount = 0;
    auto columns = Columns();
    for (int index = 0; index < sortColumns.size(); ++index) {
        auto it = std::find_if(
            columns.begin() + index,
            columns.end(),
            [&] (const TColumnSchema& column) {
                return column.Name() == sortColumns[index].Name;
            });

        if (it == columns.end()) {
            if (Strict_) {
                THROW_ERROR_EXCEPTION(
                    EErrorCode::IncompatibleKeyColumns,
                    "Column %Qv is not found in strict schema", sortColumns[index].Name)
                    << TErrorAttribute("schema", *this)
                    << TErrorAttribute("sort_columns", sortColumns);
            } else {
                columns.push_back(TColumnSchema(sortColumns[index].Name, EValueType::Any));
                it = columns.end();
                --it;
            }
        }

        if (it->SortOrder()) {
            ++oldKeyColumnCount;
        }

        std::swap(columns[index], *it);
        columns[index].SetSortOrder(sortColumns[index].SortOrder);
    }

    auto uniqueKeys = UniqueKeys_ && oldKeyColumnCount == GetKeyColumnCount();

    for (auto it = columns.begin() + sortColumns.size(); it != columns.end(); ++it) {
        it->SetSortOrder(std::nullopt);
    }

    return New<TTableSchema>(columns, Strict_, uniqueKeys, GetSchemaModification());
}

TTableSchemaPtr TTableSchema::ToReplicationLog() const
{
    std::vector<TColumnSchema> columns;
    columns.push_back(TColumnSchema(TimestampColumnName, ESimpleLogicalValueType::Uint64));
    if (IsSorted()) {
        columns.push_back(TColumnSchema(TReplicationLogTable::ChangeTypeColumnName, ESimpleLogicalValueType::Int64));
        for (const auto& column : Columns_) {
            if (column.SortOrder()) {
                columns.push_back(
                    TColumnSchema(
                        TReplicationLogTable::KeyColumnNamePrefix + column.Name(),
                        column.LogicalType()));
            } else {
                columns.push_back(
                    TColumnSchema(
                        TReplicationLogTable::ValueColumnNamePrefix + column.Name(),
                        MakeOptionalIfNot(column.LogicalType())));
                columns.push_back(
                    TColumnSchema(TReplicationLogTable::FlagsColumnNamePrefix + column.Name(), ESimpleLogicalValueType::Uint64));
            }
        }
    } else {
        for (const auto& column : Columns_) {
            columns.push_back(
                TColumnSchema(
                    TReplicationLogTable::ValueColumnNamePrefix + column.Name(),
                    MakeOptionalIfNot(column.LogicalType())));
        }
        columns.push_back(TColumnSchema(TReplicationLogTable::ValueColumnNamePrefix + TabletIndexColumnName, ESimpleLogicalValueType::Int64));
    }
    return New<TTableSchema>(std::move(columns), /* strict */ true, /* uniqueKeys */ false);
}

TTableSchemaPtr TTableSchema::ToUnversionedUpdate(bool sorted) const
{
    YT_VERIFY(IsSorted());

    std::vector<TColumnSchema> columns;
    columns.reserve(GetKeyColumnCount() + 1 + GetValueColumnCount() * 2);

    // Keys.
    for (int columnIndex = 0; columnIndex < GetKeyColumnCount(); ++columnIndex) {
        auto column = Columns_[columnIndex];
        if (!sorted) {
            column.SetSortOrder(std::nullopt);
        }
        columns.push_back(column);
    }

    // Modification type.
    columns.emplace_back(
        TUnversionedUpdateSchema::ChangeTypeColumnName,
        MakeLogicalType(ESimpleLogicalValueType::Uint64, /*required*/ true));

    // Values.
    for (int columnIndex = GetKeyColumnCount(); columnIndex < GetColumnCount(); ++columnIndex) {
        const auto& column = Columns_[columnIndex];
        YT_VERIFY(!column.SortOrder());
        columns.emplace_back(
            TUnversionedUpdateSchema::ValueColumnNamePrefix + column.Name(),
            MakeOptionalIfNot(column.LogicalType()));
        columns.emplace_back(
            TUnversionedUpdateSchema::FlagsColumnNamePrefix + column.Name(),
            MakeLogicalType(ESimpleLogicalValueType::Uint64, /*required*/ false));
    }

    return New<TTableSchema>(std::move(columns), /*strict*/ true, /*uniqueKeys*/ sorted);
}

TTableSchemaPtr TTableSchema::ToModifiedSchema(ETableSchemaModification schemaModification) const
{
    if (HasNontrivialSchemaModification()) {
        THROW_ERROR_EXCEPTION("Cannot apply schema modification because schema is already modified")
            << TErrorAttribute("existing_modification", GetSchemaModification())
            << TErrorAttribute("requested_modification", schemaModification);
    }
    YT_VERIFY(GetSchemaModification() == ETableSchemaModification::None);

    switch (schemaModification) {
        case ETableSchemaModification::None:
            return New<TTableSchema>(*this);

        case ETableSchemaModification::UnversionedUpdate:
            return ToUnversionedUpdate(/*sorted*/ true)
                ->SetSchemaModification(schemaModification);

        case ETableSchemaModification::UnversionedUpdateUnsorted:
            return ToUnversionedUpdate(/*sorted*/ false)
                ->SetSchemaModification(schemaModification);

        default:
            YT_ABORT();
    }
}

TComparator TTableSchema::ToComparator() const
{
    std::vector<ESortOrder> sortOrders(KeyColumnCount_);
    for (int index = 0; index < KeyColumnCount_; ++index) {
        YT_VERIFY(Columns_[index].SortOrder());
        sortOrders[index] = *Columns_[index].SortOrder();
    }
    return TComparator(std::move(sortOrders));
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

i64 TTableSchema::GetMemoryUsage() const
{
    i64 usage = sizeof(TTableSchema);
    for (const auto& column : Columns_) {
        usage += column.GetMemoryUsage();
    }
    return usage;
}

////////////////////////////////////////////////////////////////////////////////

void FormatValue(TStringBuilderBase* builder, const TTableSchema& schema, TStringBuf /*spec*/)
{
    builder->AppendFormat("<strict=%v;unique_keys=%v", schema.GetStrict(), schema.GetUniqueKeys());
    if (schema.HasNontrivialSchemaModification()) {
        builder->AppendFormat(";schema_modification=%v", schema.GetSchemaModification());
    }
    builder->AppendChar('>');
    builder->AppendChar('[');
    bool first = true;
    for (const auto& column : schema.Columns()) {
        if (!first) {
            builder->AppendString(TStringBuf("; "));
        }
        builder->AppendFormat("%v", column);
        first = false;
    }
    builder->AppendChar(']');
}

TString ToString(const TTableSchema& schema)
{
    return ToStringViaBuilder(schema);
}

void FormatValue(TStringBuilderBase* builder, const TTableSchemaPtr& schema, TStringBuf spec)
{
    if (schema) {
        FormatValue(builder, *schema, spec);
    } else {
        builder->AppendString(TStringBuf("<null>"));
    }
}

TString ToString(const TTableSchemaPtr& schema)
{
    return ToStringViaBuilder(schema);
}

TString SerializeToWireProto(const TTableSchemaPtr& schema)
{
    NTableClient::NProto::TTableSchemaExt protoSchema;
    ToProto(&protoSchema, schema);
    return protoSchema.SerializeAsString();
}

void DeserializeFromWireProto(TTableSchemaPtr* schema, const TString& serializedProto)
{
    NTableClient::NProto::TTableSchemaExt protoSchema;
    if (!protoSchema.ParseFromString(serializedProto)) {
        THROW_ERROR_EXCEPTION("Failed to deserialize table schema from wire proto");
    }
    FromProto(schema, protoSchema);
}

void Serialize(const TTableSchema& schema, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginAttributes()
            .Item("strict").Value(schema.GetStrict())
            .Item("unique_keys").Value(schema.GetUniqueKeys())
            .DoIf(schema.HasNontrivialSchemaModification(), [&] (TFluentMap fluent) {
                fluent.Item("schema_modification").Value(schema.GetSchemaModification());
            })
        .EndAttributes()
        .Value(schema.Columns());
}

void Deserialize(TTableSchema& schema, INodePtr node)
{
    schema = TTableSchema(
        ConvertTo<std::vector<TColumnSchema>>(node),
        node->Attributes().Get<bool>("strict", true),
        node->Attributes().Get<bool>("unique_keys", false),
        node->Attributes().Get<ETableSchemaModification>(
            "schema_modification",
            ETableSchemaModification::None));
}

void Deserialize(TTableSchema& schema, NYson::TYsonPullParserCursor* cursor)
{
    bool strict = true;
    bool uniqueKeys = false;
    ETableSchemaModification modification = ETableSchemaModification::None;
    std::vector<TColumnSchema> columns;

    if ((*cursor)->GetType() == EYsonItemType::BeginAttributes) {
        cursor->ParseAttributes([&] (TYsonPullParserCursor* cursor) {
            EnsureYsonToken(TStringBuf("table schema attribute key"), *cursor, EYsonItemType::StringValue);
            auto key = (*cursor)->UncheckedAsString();
            if (key == TStringBuf("strict")) {
                cursor->Next();
                strict = ExtractTo<bool>(cursor);
            } else if (key == TStringBuf("unique_keys")) {
                cursor->Next();
                uniqueKeys = ExtractTo<bool>(cursor);
            } else if (key == TStringBuf("schema_modification")) {
                cursor->Next();
                modification = ExtractTo<ETableSchemaModification>(cursor);
            } else {
                cursor->Next();
                cursor->SkipComplexValue();
            }
        });
    }
    EnsureYsonToken(TStringBuf("table schema"), *cursor, EYsonItemType::BeginList);
    columns = ExtractTo<std::vector<TColumnSchema>>(cursor);
    schema = TTableSchema(std::move(columns), strict, uniqueKeys, modification);
}

void Serialize(const TTableSchemaPtr& schema, IYsonConsumer* consumer)
{
    Serialize(*schema, consumer);
}


void Deserialize(TTableSchemaPtr& schema, INodePtr node)
{
    TTableSchema actualSchema;
    Deserialize(actualSchema, node);
    schema = New<TTableSchema>(std::move(actualSchema));
}

void Deserialize(TTableSchemaPtr& schema, NYson::TYsonPullParserCursor* cursor)
{
    TTableSchema actualSchema;
    Deserialize(actualSchema, cursor);
    schema = New<TTableSchema>(std::move(actualSchema));
}

void ToProto(NProto::TTableSchemaExt* protoSchema, const TTableSchema& schema)
{
    ToProto(protoSchema->mutable_columns(), schema.Columns());
    protoSchema->set_strict(schema.GetStrict());
    protoSchema->set_unique_keys(schema.GetUniqueKeys());
    protoSchema->set_schema_modification(static_cast<int>(schema.GetSchemaModification()));
}

void FromProto(TTableSchema* schema, const NProto::TTableSchemaExt& protoSchema)
{
    *schema = TTableSchema(
        FromProto<std::vector<TColumnSchema>>(protoSchema.columns()),
        protoSchema.strict(),
        protoSchema.unique_keys(),
        CheckedEnumCast<ETableSchemaModification>(protoSchema.schema_modification()));
}

void FromProto(
    TTableSchema* schema,
    const NProto::TTableSchemaExt& protoSchema,
    const NProto::TKeyColumnsExt& protoKeyColumns)
{
    auto columns = FromProto<std::vector<TColumnSchema>>(protoSchema.columns());
    for (int columnIndex = 0; columnIndex < protoKeyColumns.names_size(); ++columnIndex) {
        auto& columnSchema = columns[columnIndex];
        YT_VERIFY(columnSchema.Name() == protoKeyColumns.names(columnIndex));
        // TODO(gritukan): YT-14155
        if (!columnSchema.SortOrder()) {
            columnSchema.SetSortOrder(ESortOrder::Ascending);
        }
    }
    for (int columnIndex = protoKeyColumns.names_size(); columnIndex < columns.size(); ++columnIndex) {
        auto& columnSchema = columns[columnIndex];
        YT_VERIFY(!columnSchema.SortOrder());
    }
    *schema = TTableSchema(
        std::move(columns),
        protoSchema.strict(),
        protoSchema.unique_keys());
}

void ToProto(NProto::TTableSchemaExt* protoSchema, const TTableSchemaPtr& schema)
{
    ToProto(protoSchema, *schema);
}

void FromProto(TTableSchemaPtr* schema, const NProto::TTableSchemaExt& protoSchema)
{
    *schema = New<TTableSchema>();
    FromProto(schema->Get(), protoSchema);
}

void FromProto(
    TTableSchemaPtr* schema,
    const NProto::TTableSchemaExt& protoSchema,
    const NProto::TKeyColumnsExt& keyColumnsExt)
{
    *schema = New<TTableSchema>();
    FromProto(schema->Get(), protoSchema, keyColumnsExt);
}

////////////////////////////////////////////////////////////////////////////////

bool operator==(const TColumnSchema& lhs, const TColumnSchema& rhs)
{
    return lhs.Name() == rhs.Name()
           && *lhs.LogicalType() == *rhs.LogicalType()
           && lhs.Required() == rhs.Required()
           && lhs.SortOrder() == rhs.SortOrder()
           && lhs.Lock() == rhs.Lock()
           && lhs.Expression() == rhs.Expression()
           && lhs.Aggregate() == rhs.Aggregate()
           && lhs.Group() == rhs.Group();
}

bool operator!=(const TColumnSchema& lhs, const TColumnSchema& rhs)
{
    return !(lhs == rhs);
}

bool operator==(const TTableSchema& lhs, const TTableSchema& rhs)
{
    return lhs.Columns() == rhs.Columns() &&
        lhs.GetStrict() == rhs.GetStrict() &&
        lhs.GetUniqueKeys() == rhs.GetUniqueKeys() &&
        lhs.GetSchemaModification() == rhs.GetSchemaModification();
}

// Compat code for https://st.yandex-team.ru/YT-10668 workaround.
bool IsEqualIgnoringRequiredness(const TTableSchema& lhs, const TTableSchema& rhs)
{
    auto dropRequiredness = [] (const TTableSchema& schema) {
        std::vector<TColumnSchema> resultColumns;
        for (auto column : schema.Columns()) {
            if (column.LogicalType()->GetMetatype() == ELogicalMetatype::Optional) {
                column.SetLogicalType(column.LogicalType()->AsOptionalTypeRef().GetElement());
            }
            resultColumns.emplace_back(column);
        }
        return TTableSchema(resultColumns, schema.GetStrict(), schema.GetUniqueKeys());
    };
    return dropRequiredness(lhs) == dropRequiredness(rhs);
}

bool operator!=(const TTableSchema& lhs, const TTableSchema& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

void ValidateKeyColumns(const TKeyColumns& keyColumns)
{
    ValidateKeyColumnCount(keyColumns.size());

    THashSet<TString> names;
    for (const auto& name : keyColumns) {
        if (!names.insert(name).second) {
            THROW_ERROR_EXCEPTION("Duplicate key column name %Qv",
                name);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

void ValidateSystemColumnSchema(
    const TColumnSchema& columnSchema,
    bool isTableSorted,
    bool allowUnversionedUpdateColumns)
{
    static const auto allowedSortedTablesSystemColumns = THashMap<TString, EValueType>{
    };

    static const auto allowedOrderedTablesSystemColumns = THashMap<TString, EValueType>{
        {TimestampColumnName, EValueType::Uint64}
    };

    auto validateType = [&] (EValueType expected) {
        auto actual = columnSchema.GetPhysicalType();
        if (actual != expected) {
            THROW_ERROR_EXCEPTION("Invalid type of column %Qv: expected %Qlv, got %Qlv",
                columnSchema.Name(),
                expected,
                actual);
        }
    };

    const auto& name = columnSchema.Name();

    const auto& allowedSystemColumns = isTableSorted
        ? allowedSortedTablesSystemColumns
        : allowedOrderedTablesSystemColumns;
    auto it = allowedSystemColumns.find(name);

    // Ordinary system column.
    if (it != allowedSystemColumns.end()) {
        validateType(it->second);
        return;
    }

    if (allowUnversionedUpdateColumns) {
        // Unversioned update schema system column.
        if (name == TUnversionedUpdateSchema::ChangeTypeColumnName) {
            validateType(EValueType::Uint64);
            return;
        } else if (name.StartsWith(TUnversionedUpdateSchema::FlagsColumnNamePrefix)) {
            validateType(EValueType::Uint64);
            return;
        } else if (name.StartsWith(TUnversionedUpdateSchema::ValueColumnNamePrefix)) {
            // Value can have any type.
            return;
        }
    }

    // Unexpected system column.
    THROW_ERROR_EXCEPTION("System column name %Qv is not allowed here",
        name);
}

void ValidateColumnSchema(
    const TColumnSchema& columnSchema,
    bool isTableSorted,
    bool isTableDynamic,
    bool allowUnversionedUpdateColumns)
{
    static const auto allowedAggregates = THashSet<TString>{
        "sum",
        "min",
        "max",
        "first"
    };

    const auto& name = columnSchema.Name();
    if (name.empty()) {
        THROW_ERROR_EXCEPTION("Column name cannot be empty");
    }

    try {
        if (name.StartsWith(SystemColumnNamePrefix)) {
            ValidateSystemColumnSchema(columnSchema, isTableSorted, allowUnversionedUpdateColumns);
        }

        if (name.size() > MaxColumnNameLength) {
            THROW_ERROR_EXCEPTION("Column name is longer than maximum allowed: %v > %v",
                columnSchema.Name().size(),
                MaxColumnNameLength);
        }

        {
            TComplexTypeFieldDescriptor descriptor(name, columnSchema.LogicalType());
            ValidateLogicalType(descriptor);
        }

        if (!IsComparable(columnSchema.LogicalType()) &&
            columnSchema.SortOrder() &&
            !columnSchema.IsOfV1Type(ESimpleLogicalValueType::Any))
        {
            THROW_ERROR_EXCEPTION("Key column cannot be of %Qv type",
                *columnSchema.LogicalType());
        }

        if (*DetagLogicalType(columnSchema.LogicalType()) == *SimpleLogicalType(ESimpleLogicalValueType::Any)) {
            THROW_ERROR_EXCEPTION("Column of type %Qlv cannot be required",
                ESimpleLogicalValueType::Any);
        }

        if (columnSchema.Lock()) {
            if (columnSchema.Lock()->empty()) {
                THROW_ERROR_EXCEPTION("Column lock name cannot be empty");
            }
            if (columnSchema.Lock()->size() > MaxColumnLockLength) {
                THROW_ERROR_EXCEPTION("Column lock name is longer than maximum allowed: %v > %v",
                    columnSchema.Lock()->size(),
                    MaxColumnLockLength);
            }
            if (columnSchema.SortOrder()) {
                THROW_ERROR_EXCEPTION("Column lock cannot be set on a key column");
            }
        }

        if (columnSchema.Group()) {
            if (columnSchema.Group()->empty()) {
                THROW_ERROR_EXCEPTION("Column group should either be unset or be non-empty");
            }
            if (columnSchema.Group()->size() > MaxColumnGroupLength) {
                THROW_ERROR_EXCEPTION("Column group name is longer than maximum allowed: %v > %v",
                    columnSchema.Group()->size(),
                    MaxColumnGroupLength);
            }
        }

        ValidateSchemaValueType(columnSchema.GetPhysicalType());

        if (columnSchema.Expression() && !columnSchema.SortOrder() && isTableDynamic) {
            THROW_ERROR_EXCEPTION("Non-key column cannot be computed");
        }

        if (columnSchema.Aggregate() && columnSchema.SortOrder()) {
            THROW_ERROR_EXCEPTION("Key column cannot be aggregated");
        }

        if (columnSchema.Aggregate() && allowedAggregates.find(*columnSchema.Aggregate()) == allowedAggregates.end()) {
            THROW_ERROR_EXCEPTION("Invalid aggregate function %Qv",
                *columnSchema.Aggregate());
        }

        if (columnSchema.Expression() && columnSchema.Required()) {
            THROW_ERROR_EXCEPTION("Computed column cannot be required");
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error validating schema of a column %Qv",
            name)
            << ex;
    }
}

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

    if (schema.GetKeyColumnCount() > MaxKeyColumnCountInDynamicTable) {
        THROW_ERROR_EXCEPTION("Key column count must be not greater than %v, actual: %v",
            MaxKeyColumnCountInDynamicTable,
            schema.GetKeyColumnCount());
    }

    for (const auto& column : schema.Columns()) {
        try {
            if (!column.IsOfV1Type()) {
                THROW_ERROR_EXCEPTION("Complex types are not allowed in dynamic tables yet");
            }
            if (column.SortOrder() && column.GetPhysicalType() == EValueType::Any) {
                THROW_ERROR_EXCEPTION("Dynamic table cannot have key column of type: %Qv",
                    *column.LogicalType());
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error validating column %Qv in dynamic table schema",
                column.Name())
                << ex;
        }
    }
}

//! Validates that there are no duplicates among the column names.
void ValidateColumnUniqueness(const TTableSchema& schema)
{
    THashSet<TString> columnNames;
    for (const auto& column : schema.Columns()) {
        if (!columnNames.insert(column.Name()).second) {
            THROW_ERROR_EXCEPTION("Duplicate column name %Qv in table schema",
                column.Name());
        }
    }
}

//! Validates that number of locks doesn't exceed #MaxColumnLockCount.
void ValidateLocks(const TTableSchema& schema)
{
    THashSet<TString> lockNames;
    YT_VERIFY(lockNames.insert(PrimaryLockName).second);
    for (const auto& column : schema.Columns()) {
        if (column.Lock()) {
            lockNames.insert(*column.Lock());
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
        if (!schema.Columns()[index].SortOrder()) {
            THROW_ERROR_EXCEPTION("Key columns must form a prefix of schema");
        }
    }
    // The fact that first GetKeyColumnCount() columns have SortOrder automatically
    // implies that the rest of columns don't have SortOrder, so we don't need to check it.
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

    if (column->SortOrder()) {
        THROW_ERROR_EXCEPTION("Column %Qv cannot be a part of key",
            TimestampColumnName);
    }

    if (!column->IsOfV1Type(ESimpleLogicalValueType::Uint64)) {
        THROW_ERROR_EXCEPTION("Column %Qv must have %Qlv type",
            TimestampColumnName,
            EValueType::Uint64);
    }

    if (schema.IsSorted()) {
        THROW_ERROR_EXCEPTION("Column %Qv cannot appear in a sorted table",
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

void ValidateTableSchema(const TTableSchema& schema, bool isTableDynamic, bool allowUnversionedUpdateColumns)
{
    int totalTypeComplexity = 0;
    for (const auto& column : schema.Columns()) {
        ValidateColumnSchema(
            column,
            schema.IsSorted(),
            isTableDynamic,
            allowUnversionedUpdateColumns);
        totalTypeComplexity += column.LogicalType()->GetTypeComplexity();
    }
    if (totalTypeComplexity >= MaxSchemaTotalTypeComplexity) {
        THROW_ERROR_EXCEPTION("Table schema is too complex, reduce number of columns or simplify their types");
    }
    ValidateColumnUniqueness(schema);
    ValidateLocks(schema);
    ValidateKeyColumnsFormPrefix(schema);
    ValidateTimestampColumn(schema);
    ValidateSchemaAttributes(schema);
    if (isTableDynamic) {
        ValidateDynamicTableConstraints(schema);
    }
}

void ValidateNoDescendingSortOrder(const TTableSchema& schema)
{
    for (const auto& column : schema.Columns()) {
        if (column.SortOrder() == ESortOrder::Descending) {
            THROW_ERROR_EXCEPTION(
                NTableClient::EErrorCode::InvalidSchemaValue,
                "Descending sort order is not available in this context yet")
                << TErrorAttribute("column_name", column.Name());
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

THashMap<TString, int> GetLocksMapping(
    const NTableClient::TTableSchema& schema,
    bool fullAtomicity,
    std::vector<int>* columnIndexToLockIndex,
    std::vector<TString>* lockIndexToName)
{
    if (columnIndexToLockIndex) {
        // Assign dummy lock indexes to key components.
        columnIndexToLockIndex->assign(schema.Columns().size(), -1);
    }

    if (lockIndexToName) {
        lockIndexToName->push_back(PrimaryLockName);
    }

    THashMap<TString, int> groupToIndex;
    if (fullAtomicity) {
        // Assign lock indexes to data components.
        for (int index = schema.GetKeyColumnCount(); index < schema.Columns().size(); ++index) {
            const auto& columnSchema = schema.Columns()[index];
            int lockIndex = PrimaryLockIndex;

            if (columnSchema.Lock()) {
                auto emplaced = groupToIndex.emplace(*columnSchema.Lock(), groupToIndex.size() + 1);
                if (emplaced.second && lockIndexToName) {
                    lockIndexToName->push_back(*columnSchema.Lock());
                }
                lockIndex = emplaced.first->second;
            }

            if (columnIndexToLockIndex) {
                (*columnIndexToLockIndex)[index] = lockIndex;
            }
        }
    } else if (columnIndexToLockIndex) {
        // No locking supported for non-atomic tablets, however we still need the primary
        // lock descriptor to maintain last commit timestamps.
        for (int index = schema.GetKeyColumnCount(); index < schema.Columns().size(); ++index) {
            (*columnIndexToLockIndex)[index] = PrimaryLockIndex;
        }
    }
    return groupToIndex;
}

TLockMask GetLockMask(
    const NTableClient::TTableSchema& schema,
    bool fullAtomicity,
    const std::vector<TString>& locks,
    ELockType lockType)
{
    THashMap<TString, int> groupToIndex = GetLocksMapping(
        schema,
        fullAtomicity);

    TLockMask lockMask;
    for (const auto& lock : locks) {
        auto it = groupToIndex.find(lock);
        if (it != groupToIndex.end()) {
            lockMask.Set(it->second, lockType);
        } else {
            THROW_ERROR_EXCEPTION("Lock group %Qv not found in schema", lock);
        }
    }
    return lockMask;
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

void ToProto(TColumnFilter* protoColumnFilter, const NTableClient::TColumnFilter& columnFilter)
{
    if (!columnFilter.IsUniversal()) {
        for (auto index : columnFilter.GetIndexes()) {
            protoColumnFilter->add_indexes(index);
        }
    }
}

void FromProto(NTableClient::TColumnFilter* columnFilter, const TColumnFilter& protoColumnFilter)
{
    *columnFilter = protoColumnFilter.indexes().empty()
        ? NTableClient::TColumnFilter()
        : NTableClient::TColumnFilter(FromProto<NTableClient::TColumnFilter::TIndexes>(protoColumnFilter.indexes()));
}

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

TKeyColumnTypes GetKeyColumnTypes(NTableClient::TTableSchemaPtr schema)
{
    TKeyColumnTypes keyColumnTypes;
    for (int i = 0; i < schema->GetKeyColumnCount(); ++i) {
        keyColumnTypes.push_back(schema->Columns()[i].GetPhysicalType());
    }
    return keyColumnTypes;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
