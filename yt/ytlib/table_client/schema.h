#pragma once

#include "row_base.h"

#include <yt/core/misc/error.h>
#include <yt/core/misc/nullable.h>
#include <yt/core/misc/property.h>

#include <yt/core/yson/public.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ESortOrder,
    (Ascending)
)

////////////////////////////////////////////////////////////////////////////////

struct TColumnSchema
{
    TColumnSchema();
    TColumnSchema(
        const Stroka& name,
        EValueType type);

    TColumnSchema(const TColumnSchema&) = default;
    TColumnSchema(TColumnSchema&&) = default;

    TColumnSchema& operator=(const TColumnSchema&) = default;
    TColumnSchema& operator=(TColumnSchema&&) = default;

    TColumnSchema& SetSortOrder(const TNullable<ESortOrder>& value);
    TColumnSchema& SetLock(const TNullable<Stroka>& value);
    TColumnSchema& SetExpression(const TNullable<Stroka>& value);
    TColumnSchema& SetAggregate(const TNullable<Stroka>& value);
    TColumnSchema& SetGroup(const TNullable<Stroka>& value);

    Stroka Name;
    EValueType Type;
    TNullable<ESortOrder> SortOrder;
    TNullable<Stroka> Lock;
    TNullable<Stroka> Expression;
    TNullable<Stroka> Aggregate;
    TNullable<Stroka> Group;
};

void Serialize(const TColumnSchema& schema, NYson::IYsonConsumer* consumer);
void Deserialize(TColumnSchema& schema, NYTree::INodePtr node);

void ToProto(NProto::TColumnSchema* protoSchema, const TColumnSchema& schema);
void FromProto(TColumnSchema* schema, const NProto::TColumnSchema& protoSchema);

////////////////////////////////////////////////////////////////////////////////

class TTableSchema
{
public:
    DEFINE_BYREF_RO_PROPERTY(std::vector<TColumnSchema>, Columns);
    DEFINE_BYVAL_RO_PROPERTY(bool, Strict);

    //! Constructs an empty non-strict schema.
    TTableSchema();

    //! Constructs a schema with given columns and strictness flag.
    //! No validation is performed.
    explicit TTableSchema(
        std::vector<TColumnSchema> columns,
        bool strict = true);

    const TColumnSchema* FindColumn(const TStringBuf& name) const;
    const TColumnSchema& GetColumn(const TStringBuf& name) const;
    const TColumnSchema& GetColumnOrThrow(const TStringBuf& name) const;

    int GetColumnIndex(const TColumnSchema& column) const;
    int GetColumnIndexOrThrow(const TStringBuf& name) const;

    TTableSchema Filter(const TColumnFilter& columnFilter) const;

    // TODO(babenko): this function is deprecated
    void AppendColumn(const TColumnSchema& column);

    bool HasComputedColumns() const;
    bool IsSorted() const;

    TKeyColumns GetKeyColumns() const;
    int GetKeyColumnCount() const;

    //! Constructs a non-strict schema from #keyColumns assigning all components EValueType::Any type.
    //! #keyColumns could be empty, in which case an empty non-strict schema is returned.
    //! The resulting schema is validated.
    static TTableSchema FromKeyColumns(const TKeyColumns& keyColumns);

    //! For sorted tables, return the current schema as-is.
    //! For ordered tables, prepends the current schema with |(tablet_index, row_index)| key columns.
    TTableSchema ToQuery() const;

    //! For sorted tables, return the current schema without computed columns.
    //! For ordered tables, prepends the current schema with |(tablet_index)| key column
    //! but without |$timestamp| column, if any.
    TTableSchema ToWrite() const;

    //! For sorted tables, returns the non-computed key columns.
    //! For ordered tables, returns an empty schema.
    TTableSchema ToLookup() const;

    //! For sorted tables, returns the non-computed key columns.
    //! For ordered tables, returns an empty schema.
    TTableSchema ToDelete() const;

    //! Returns just the key columns.
    TTableSchema ToKeys() const;

    //! Returns the non-key columns.
    TTableSchema ToValues() const;

    void Save(TStreamSaveContext& context) const;
    void Load(TStreamLoadContext& context);

private:
    int KeyColumnCount_ = 0;

};

Stroka ToString(const TTableSchema& schema);

void Serialize(const TTableSchema& schema, NYson::IYsonConsumer* consumer);
void Deserialize(TTableSchema& schema, NYTree::INodePtr node);

void ToProto(NProto::TTableSchemaExt* protoSchema, const TTableSchema& schema);
void FromProto(TTableSchema* schema, const NProto::TTableSchemaExt& protoSchema);
void FromProto(
    TTableSchema* schema,
    const NProto::TTableSchemaExt& protoSchema,
    const NProto::TKeyColumnsExt& keyColumnsExt);

////////////////////////////////////////////////////////////////////////////////

bool operator == (const TColumnSchema& lhs, const TColumnSchema& rhs);
bool operator != (const TColumnSchema& lhs, const TColumnSchema& rhs);

bool operator == (const TTableSchema& lhs, const TTableSchema& rhs);
bool operator != (const TTableSchema& lhs, const TTableSchema& rhs);

////////////////////////////////////////////////////////////////////////////////

void ValidateKeyColumns(const TKeyColumns& keyColumns);
void ValidateKeyColumnsUpdate(const TKeyColumns& oldKeyColumns, const TKeyColumns& newKeyColumns);

void ValidateColumnSchema(const TColumnSchema& columnSchema);
void ValidateColumnSchemaUpdate(const TColumnSchema& oldColumn, const TColumnSchema& newColumn);

void ValidateTableSchema(const TTableSchema& schema);
void ValidateTableSchemaUpdate(
    const TTableSchema& oldSchema,
    const TTableSchema& newSchema,
    bool isTableDynamic = false,
    bool isTableEmpty = false);

void ValidatePivotKey(const TOwningKey& pivotKey, const TTableSchema& schema);

void ValidateReadSchema(const TTableSchema& readSchema, const TTableSchema& tableSchema);

TTableSchema InferInputSchema(const std::vector<TTableSchema>& schemas, bool discardKeyColumns);

////////////////////////////////////////////////////////////////////////////////

// NB: Need to place this into NProto for ADL to work properly since TKeyColumns is std::vector.
namespace NProto {

void ToProto(NProto::TKeyColumnsExt* protoKeyColumns, const TKeyColumns& keyColumns);
void FromProto(TKeyColumns* keyColumns, const NProto::TKeyColumnsExt& protoKeyColumns);

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
