#pragma once

#include "row_base.h"

#include <core/misc/error.h>
#include <core/misc/nullable.h>
#include <core/misc/property.h>
#include <core/misc/serialize.h>

#include <core/ytree/public.h>

#include <core/yson/public.h>

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

struct TColumnSchema
{
    TColumnSchema();
    TColumnSchema(
        const Stroka& name,
        EValueType type,
        const TNullable<Stroka>& lock = Null,
        const TNullable<Stroka>& expression = Null);

    TColumnSchema(const TColumnSchema&) = default;
    TColumnSchema(TColumnSchema&&) = default;

    TColumnSchema& operator=(const TColumnSchema&) = default;
    TColumnSchema& operator=(TColumnSchema&&) = default;

    Stroka Name;
    EValueType Type;
    TNullable<Stroka> Lock;
    TNullable<Stroka> Expression;
};

void Serialize(const TColumnSchema& schema, NYson::IYsonConsumer* consumer);
void Deserialize(TColumnSchema& schema, NYTree::INodePtr node);

void ToProto(NProto::TColumnSchema* protoSchema, const TColumnSchema& schema);
void FromProto(TColumnSchema* schema, const NProto::TColumnSchema& protoSchema);

////////////////////////////////////////////////////////////////////////////////

class TTableSchema
{
public:
    DEFINE_BYREF_RW_PROPERTY(std::vector<TColumnSchema>, Columns);

public:
    TColumnSchema* FindColumn(const TStringBuf& name);
    const TColumnSchema* FindColumn(const TStringBuf& name) const;

    TColumnSchema& GetColumnOrThrow(const TStringBuf& name);
    const TColumnSchema& GetColumnOrThrow(const TStringBuf& name) const;

    int GetColumnIndex(const TColumnSchema& column) const;
    int GetColumnIndexOrThrow(const TStringBuf& name) const;

    TTableSchema Filter(const TColumnFilter& columnFilter) const;
    TTableSchema TrimNonkeyColumns(const TKeyColumns& keyColumns) const;

    bool HasComputedColumns(int keySize) const;

    void Save(TStreamSaveContext& context) const;
    void Load(TStreamLoadContext& context);
};

void Serialize(const TTableSchema& schema, NYson::IYsonConsumer* consumer);
void Deserialize(TTableSchema& schema, NYTree::INodePtr node);

void ToProto(NProto::TTableSchemaExt* protoSchema, const TTableSchema& schema);
void FromProto(TTableSchema* schema, const NProto::TTableSchemaExt& protoSchema);

bool operator == (const TColumnSchema& lhs, const TColumnSchema& rhs);
bool operator != (const TColumnSchema& lhs, const TColumnSchema& rhs);

bool operator == (const TTableSchema& lhs, const TTableSchema& rhs);
bool operator != (const TTableSchema& lhs, const TTableSchema& rhs);

////////////////////////////////////////////////////////////////////////////////

void ValidateKeyColumns(const TKeyColumns& keyColumns);
void ValidateTableSchema(const TTableSchema& schema);
void ValidateTableSchemaAndKeyColumns(const TTableSchema& schema, const TKeyColumns& keyColumns);

////////////////////////////////////////////////////////////////////////////////

// NB: Need to place this into NProto for ADL to work properly since TKeyColumns is std::vector.
namespace NProto {

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TKeyColumnsExt* protoKeyColumns, const TKeyColumns& keyColumns);
void FromProto(TKeyColumns* keyColumns, const NProto::TKeyColumnsExt& protoKeyColumns);

////////////////////////////////////////////////////////////////////////////////

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
