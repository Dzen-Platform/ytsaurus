#pragma once

#include "public.h"
#include "ast.h"
#include "plan_fragment_common.h"

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/schema.h>
#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/core/misc/guid.h>
#include <yt/core/misc/property.h>
#include <yt/core/misc/range.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EExpressionKind,
    (Literal)
    (Reference)
    (Function)
    (UnaryOp)
    (BinaryOp)
    (InOp)
);

DEFINE_ENUM(EOperatorKind,
    (Scan)
    (Filter)
    (Group)
    (Project)
);

struct TExpression
    : public TIntrinsicRefCounted
{
    TExpression(EValueType type)
        : Type(type)
    { }

    const EValueType Type;

    template <class TDerived>
    const TDerived* As() const
    {
        return dynamic_cast<const TDerived*>(this);
    }

    template <class TDerived>
    TDerived* As()
    {
        return dynamic_cast<TDerived*>(this);
    }
};

DEFINE_REFCOUNTED_TYPE(TExpression)

struct TLiteralExpression
    : public TExpression
{
    TLiteralExpression(EValueType type)
        : TExpression(type)
    { }

    TLiteralExpression(EValueType type, TOwningValue value)
        : TExpression(type)
        , Value(value)
    { }

    TOwningValue Value;
};

struct TReferenceExpression
    : public TExpression
{
    TReferenceExpression(EValueType type)
        : TExpression(type)
    { }

    TReferenceExpression(EValueType type, TStringBuf columnName)
        : TExpression(type)
        , ColumnName(columnName)
    { }

    Stroka ColumnName;
};

struct TFunctionExpression
    : public TExpression
{
    TFunctionExpression(EValueType type)
        : TExpression(type)
    { }

    TFunctionExpression(
        EValueType type,
        const Stroka& functionName,
        const std::vector<TConstExpressionPtr>& arguments)
        : TExpression(type)
        , FunctionName(functionName)
        , Arguments(arguments)
    { }

    Stroka FunctionName;
    std::vector<TConstExpressionPtr> Arguments;
};

DEFINE_REFCOUNTED_TYPE(TFunctionExpression)

struct TUnaryOpExpression
    : public TExpression
{
    TUnaryOpExpression(EValueType type)
        : TExpression(type)
    { }

    TUnaryOpExpression(
        EValueType type,
        EUnaryOp opcode,
        TConstExpressionPtr operand)
        : TExpression(type)
        , Opcode(opcode)
        , Operand(operand)
    { }

    EUnaryOp Opcode;
    TConstExpressionPtr Operand;
};

struct TBinaryOpExpression
    : public TExpression
{
    TBinaryOpExpression(EValueType type)
        : TExpression(type)
    { }

    TBinaryOpExpression(
        EValueType type,
        EBinaryOp opcode,
        TConstExpressionPtr lhs,
        TConstExpressionPtr rhs)
        : TExpression(type)
        , Opcode(opcode)
        , Lhs(lhs)
        , Rhs(rhs)
    { }

    EBinaryOp Opcode;
    TConstExpressionPtr Lhs;
    TConstExpressionPtr Rhs;
};

struct TInOpExpression
    : public TExpression
{
    TInOpExpression(EValueType type)
        : TExpression(type)
    { }

    TInOpExpression(
        std::vector<TConstExpressionPtr> arguments,
        TSharedRange<TRow> values)
        : TExpression(EValueType::Boolean)
        , Arguments(std::move(arguments))
        , Values(std::move(values))
    { }

    std::vector<TConstExpressionPtr> Arguments;
    TSharedRange<TRow> Values;
};

EValueType InferBinaryExprType(
    EBinaryOp opCode,
    EValueType lhsType,
    EValueType rhsType,
    const TStringBuf& source,
    const TStringBuf& lhsSource,
    const TStringBuf& rhsSource);

////////////////////////////////////////////////////////////////////////////////

struct TNamedItem
{
    TNamedItem()
    { }

    TNamedItem(
        TConstExpressionPtr expression,
        const Stroka& name)
        : Expression(expression)
        , Name(name)
    { }

    TConstExpressionPtr Expression;
    Stroka Name;

    TColumnSchema GetColumnSchema() const
    {
        return TColumnSchema(Name, Expression->Type);
    }
};

typedef std::vector<TNamedItem> TNamedItemList;

struct TAggregateItem
    : public TNamedItem
{
    TAggregateItem()
    { }

    TAggregateItem(
        TConstExpressionPtr expression,
        const Stroka& aggregateFunction,
        const Stroka& name,
        EValueType stateType,
        EValueType resultType)
        : TNamedItem(expression, name)
        , AggregateFunction(aggregateFunction)
        , StateType(stateType)
        , ResultType(resultType)
    { }

    Stroka AggregateFunction;
    EValueType StateType;
    EValueType ResultType;
};

typedef std::vector<TAggregateItem> TAggregateItemList;

////////////////////////////////////////////////////////////////////////////////

struct TJoinClause
    : public TIntrinsicRefCounted
{
    TTableSchema ForeignTableSchema;
    i64 ForeignKeyColumnsCount;

    TTableSchema RenamedTableSchema;

    std::vector<std::pair<TConstExpressionPtr, TConstExpressionPtr>> Equations;

    bool IsLeft = false;

    TGuid ForeignDataId;

    // TODO: Use ITableSchemaInterface
    TTableSchema JoinedTableSchema;

    TTableSchema GetTableSchema() const
    {
        return JoinedTableSchema;
    }
};

DEFINE_REFCOUNTED_TYPE(TJoinClause)

struct TGroupClause
    : public TIntrinsicRefCounted
{
    TNamedItemList GroupItems;
    TAggregateItemList AggregateItems;
    bool IsMerge;
    bool IsFinal;

    // TODO: Use ITableSchemaInterface
    TTableSchema GroupedTableSchema;

    void AddGroupItem(const TNamedItem& namedItem)
    {
        GroupItems.push_back(namedItem);
        GroupedTableSchema.Columns().emplace_back(namedItem.Name, namedItem.Expression->Type);
    }

    void AddGroupItem(TConstExpressionPtr expression, Stroka name)
    {
        AddGroupItem(TNamedItem(expression, name));
    }

    TTableSchema GetTableSchema() const
    {
        return GroupedTableSchema;
    }
};

DEFINE_REFCOUNTED_TYPE(TGroupClause)

typedef std::pair<TConstExpressionPtr, bool> TOrderItem;

struct TOrderClause
    : public TIntrinsicRefCounted
{
    std::vector<TOrderItem> OrderItems;
};

DEFINE_REFCOUNTED_TYPE(TOrderClause)

struct TProjectClause
    : public TIntrinsicRefCounted
{
    TNamedItemList Projections;

    // TODO: Use ITableSchemaInterface
    TTableSchema ProjectTableSchema;

    void AddProjection(const TNamedItem& namedItem)
    {
        Projections.push_back(namedItem);
        ProjectTableSchema.Columns().emplace_back(namedItem.Name, namedItem.Expression->Type);
    }

    void AddProjection(TConstExpressionPtr expression, Stroka name)
    {
        AddProjection(TNamedItem(expression, name));
    }

    TTableSchema GetTableSchema() const
    {
        return ProjectTableSchema;
    }
};

DEFINE_REFCOUNTED_TYPE(TProjectClause)

struct TQuery
    : public TIntrinsicRefCounted
{
    TQuery(
        i64 inputRowLimit,
        i64 outputRowLimit,
        const TGuid& id = TGuid::Create())
        : InputRowLimit(inputRowLimit)
        , OutputRowLimit(outputRowLimit)
        , Id(id)
    { }

    TQuery(const TQuery& other)
        : InputRowLimit(other.InputRowLimit)
        , OutputRowLimit(other.OutputRowLimit)
        , Id(TGuid::Create())
        , TableSchema(other.TableSchema)
        , KeyColumnsCount(other.KeyColumnsCount)
        , RenamedTableSchema(other.RenamedTableSchema)
        , JoinClauses(other.JoinClauses)
        , WhereClause(other.WhereClause)
        , GroupClause(other.GroupClause)
        , HavingClause(other.HavingClause)
        , ProjectClause(other.ProjectClause)
        , OrderClause(other.OrderClause)
        , Limit(other.Limit)
    { }

    i64 InputRowLimit;
    i64 OutputRowLimit;
    TGuid Id;

    // TODO: Move out TableSchema and KeyColumns 
    TTableSchema TableSchema;
    i64 KeyColumnsCount;

    TTableSchema RenamedTableSchema;
    std::vector<TConstJoinClausePtr> JoinClauses;
    TConstExpressionPtr WhereClause;
    TConstGroupClausePtr GroupClause;
    TConstExpressionPtr HavingClause;
    TConstProjectClausePtr ProjectClause;
    TConstOrderClausePtr OrderClause;

    i64 Limit = std::numeric_limits<i64>::max();

    bool IsOrdered() const
    {
        if (Limit < std::numeric_limits<i64>::max() ) {
            return !OrderClause && !GroupClause;
        } else {
            YCHECK(!OrderClause);
        }

        return false;
    }

    TTableSchema GetTableSchema() const
    {
        if (ProjectClause) {
            return ProjectClause->GetTableSchema();
        }

        if (GroupClause) {
            return GroupClause->GetTableSchema();
        }

        if (!JoinClauses.empty()) {
            return JoinClauses.back()->GetTableSchema();
        }

        return RenamedTableSchema;
    }
};

DEFINE_REFCOUNTED_TYPE(TQuery)

void ToProto(NProto::TQuery* proto, TConstQueryPtr original);
TQueryPtr FromProto(const NProto::TQuery& serialized);

struct TQueryOptions
{
    NTransactionClient::TTimestamp Timestamp = NTransactionClient::SyncLastCommittedTimestamp;
    bool VerboseLogging = false;
    int MaxSubqueries = std::numeric_limits<int>::max();
    ui64 RangeExpansionLimit = 0;
    bool EnableCodeCache = true;
};

struct TDataSource
{
    //! Either a chunk id or tablet id.
    NObjectClient::TObjectId Id;
    TRowRange Range;
};

typedef std::vector<TDataSource> TDataSources;

struct TPlanFragmentBase
    : public TIntrinsicRefCounted
{
    TTimestamp Timestamp;
    TRowBufferPtr KeyRangesRowBuffer = New<TRowBuffer>();

    TConstQueryPtr Query;
    TQueryOptions Options;

};

struct TDataSource2
{
    //! Either a chunk id or tablet id.
    NObjectClient::TObjectId Id;
    TSharedRange<TRowRange> Ranges;
};

void ToProto(NProto::TQueryOptions* proto, const TQueryOptions& options);
TQueryOptions FromProto(const NProto::TQueryOptions& serialized);

void ToProto(NProto::TDataSource2* proto, const TDataSource2& dataSource);
TDataSource2 FromProto(const NProto::TDataSource2& serialized);

Stroka InferName(TConstExpressionPtr expr, bool omitValues = false);
Stroka InferName(TConstQueryPtr query, bool omitValues = false);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
