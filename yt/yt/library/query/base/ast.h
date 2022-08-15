#pragma once

#include "public.h"
#include "query_common.h"

#include <yt/yt/library/query/misc/objects_holder.h>
#include <library/cpp/yt/misc/hash.h>

#include <variant>

namespace NYT::NQueryClient::NAst {

////////////////////////////////////////////////////////////////////////////////

#define XX(name) \
struct name; \
using name ## Ptr = name*;

XX(TExpression)
XX(TReferenceExpression)
XX(TAliasExpression)
XX(TLiteralExpression)
XX(TFunctionExpression)
XX(TUnaryOpExpression)
XX(TBinaryOpExpression)
XX(TInExpression)
XX(TBetweenExpression)
XX(TTransformExpression)

#undef XX


using TIdentifierList = std::vector<TReferenceExpressionPtr>;
using TExpressionList = std::vector<TExpressionPtr>;
using TNullableExpressionList = std::optional<TExpressionList>;
using TNullableIdentifierList = std::optional<TIdentifierList>;
using TOrderExpressionList = std::vector<std::pair<TExpressionList, bool>>;

////////////////////////////////////////////////////////////////////////////////

struct TNullLiteralValue
{ };

using TLiteralValue = std::variant<
    TNullLiteralValue,
    i64,
    ui64,
    double,
    bool,
    TString
>;

using TLiteralValueList = std::vector<TLiteralValue>;
using TLiteralValueTuple = std::vector<TLiteralValue>;
using TLiteralValueTupleList = std::vector<TLiteralValueTuple>;
using TLiteralValueRangeList = std::vector<std::pair<TLiteralValueTuple, TLiteralValueTuple>>;

////////////////////////////////////////////////////////////////////////////////

struct TReference
{
    TReference() = default;

    TReference(const TString& columnName, const std::optional<TString>& tableName = std::nullopt)
        : ColumnName(columnName)
        , TableName(tableName)
    { }

    TString ColumnName;
    std::optional<TString> TableName;

    operator size_t() const;
};

bool operator == (const TReference& lhs, const TReference& rhs);
bool operator != (const TReference& lhs, const TReference& rhs);

////////////////////////////////////////////////////////////////////////////////

struct TExpression
{
    explicit TExpression(const TSourceLocation& sourceLocation)
        : SourceLocation(sourceLocation)
    { }

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

    TStringBuf GetSource(TStringBuf source) const;

    TSourceLocation SourceLocation;

    virtual ~TExpression()
    { }
};

//using TExpressionPtr = TExpression*;

template <class T, class... TArgs>
TExpressionList MakeExpression(TObjectsHolder* holder, TArgs&& ... args)
{
    return TExpressionList(1, holder->Register(new T(std::forward<TArgs>(args)...)));
}

bool operator == (const TExpression& lhs, const TExpression& rhs);
bool operator != (const TExpression& lhs, const TExpression& rhs);

////////////////////////////////////////////////////////////////////////////////

struct TLiteralExpression
    : public TExpression
{
    TLiteralExpression(
        const TSourceLocation& sourceLocation,
        TLiteralValue value)
        : TExpression(sourceLocation)
        , Value(std::move(value))
    { }

    TLiteralValue Value;
};

////////////////////////////////////////////////////////////////////////////////

struct TReferenceExpression
    : public TExpression
{
    TReferenceExpression(
        const TSourceLocation& sourceLocation,
        const TString& columnName)
        : TExpression(sourceLocation)
        , Reference(columnName)
    { }

    TReferenceExpression(
        const TSourceLocation& sourceLocation,
        const TString& columnName,
        const TString& tableName)
        : TExpression(sourceLocation)
        , Reference(columnName, tableName)
    { }

    TReferenceExpression(
        const TSourceLocation& sourceLocation,
        const TReference& reference)
        : TExpression(sourceLocation)
        , Reference(reference)
    { }

    TReference Reference;
};

////////////////////////////////////////////////////////////////////////////////

struct TAliasExpression
    : public TExpression
{
    TAliasExpression(
        const TSourceLocation& sourceLocation,
        const TExpressionPtr& expression,
        TStringBuf name)
        : TExpression(sourceLocation)
        , Expression(expression)
        , Name(TString(name))
    { }

    TExpressionPtr Expression;
    TString Name;
};

////////////////////////////////////////////////////////////////////////////////

struct TFunctionExpression
    : public TExpression
{
    TFunctionExpression(
        const TSourceLocation& sourceLocation,
        TStringBuf functionName,
        TExpressionList arguments)
        : TExpression(sourceLocation)
        , FunctionName(functionName)
        , Arguments(std::move(arguments))
    { }

    TString FunctionName;
    TExpressionList Arguments;
};

////////////////////////////////////////////////////////////////////////////////

struct TUnaryOpExpression
    : public TExpression
{
    TUnaryOpExpression(
        const TSourceLocation& sourceLocation,
        EUnaryOp opcode,
        TExpressionList operand)
        : TExpression(sourceLocation)
        , Opcode(opcode)
        , Operand(std::move(operand))
    { }

    EUnaryOp Opcode;
    TExpressionList Operand;
};

////////////////////////////////////////////////////////////////////////////////

struct TBinaryOpExpression
    : public TExpression
{
    TBinaryOpExpression(
        const TSourceLocation& sourceLocation,
        EBinaryOp opcode,
        TExpressionList lhs,
        TExpressionList rhs)
        : TExpression(sourceLocation)
        , Opcode(opcode)
        , Lhs(std::move(lhs))
        , Rhs(std::move(rhs))
    { }

    EBinaryOp Opcode;
    TExpressionList Lhs;
    TExpressionList Rhs;
};

////////////////////////////////////////////////////////////////////////////////

struct TInExpression
    : public TExpression
{
    TInExpression(
        const TSourceLocation& sourceLocation,
        TExpressionList expression,
        const TLiteralValueTupleList& values)
        : TExpression(sourceLocation)
        , Expr(std::move(expression))
        , Values(values)
    { }

    TExpressionList Expr;
    TLiteralValueTupleList Values;
};

////////////////////////////////////////////////////////////////////////////////

struct TBetweenExpression
    : public TExpression
{
    TBetweenExpression(
        const TSourceLocation& sourceLocation,
        TExpressionList expression,
        const TLiteralValueRangeList& values)
        : TExpression(sourceLocation)
        , Expr(std::move(expression))
        , Values(values)
    { }

    TExpressionList Expr;
    TLiteralValueRangeList Values;
};

////////////////////////////////////////////////////////////////////////////////

struct TTransformExpression
    : public TExpression
{
    TTransformExpression(
        const TSourceLocation& sourceLocation,
        TExpressionList expression,
        const TLiteralValueTupleList& from,
        const TLiteralValueTupleList& to,
        TNullableExpressionList defaultExpr)
        : TExpression(sourceLocation)
        , Expr(std::move(expression))
        , From(from)
        , To(to)
        , DefaultExpr(std::move(defaultExpr))
    { }

    TExpressionList Expr;
    TLiteralValueTupleList From;
    TLiteralValueTupleList To;
    TNullableExpressionList DefaultExpr;
};

////////////////////////////////////////////////////////////////////////////////
struct TTableDescriptor
{
    TTableDescriptor() = default;

    explicit TTableDescriptor(
        const NYPath::TYPath& path,
        const std::optional<TString>& alias = std::nullopt)
        : Path(path)
        , Alias(alias)
    { }

    NYPath::TYPath Path;
    std::optional<TString> Alias;
};

bool operator == (const TTableDescriptor& lhs, const TTableDescriptor& rhs);
bool operator != (const TTableDescriptor& lhs, const TTableDescriptor& rhs);

////////////////////////////////////////////////////////////////////////////////

struct TJoin
{
    TJoin(
        bool isLeft,
        const TTableDescriptor& table,
        const TIdentifierList& fields,
        const TNullableExpressionList& predicate)
        : IsLeft(isLeft)
        , Table(table)
        , Fields(fields)
        , Predicate(predicate)
    { }

    TJoin(
        bool isLeft,
        const TTableDescriptor& table,
        const TExpressionList& lhs,
        const TExpressionList& rhs,
        const TNullableExpressionList& predicate)
        : IsLeft(isLeft)
        , Table(table)
        , Lhs(lhs)
        , Rhs(rhs)
        , Predicate(predicate)
    { }

    bool IsLeft;
    TTableDescriptor Table;
    TIdentifierList Fields;

    TExpressionList Lhs;
    TExpressionList Rhs;

    TNullableExpressionList Predicate;
};

bool operator == (const TJoin& lhs, const TJoin& rhs);
bool operator != (const TJoin& lhs, const TJoin& rhs);

////////////////////////////////////////////////////////////////////////////////

struct TQuery
{
    TTableDescriptor Table;
    std::vector<TJoin> Joins;

    TNullableExpressionList SelectExprs;
    TNullableExpressionList WherePredicate;

    std::optional<std::pair<TExpressionList, ETotalsMode>> GroupExprs;
    TNullableExpressionList HavingPredicate;

    TOrderExpressionList OrderExpressions;

    std::optional<i64> Offset;
    std::optional<i64> Limit;
};

bool operator == (const TQuery& lhs, const TQuery& rhs);
bool operator != (const TQuery& lhs, const TQuery& rhs);

////////////////////////////////////////////////////////////////////////////////

using TAliasMap = THashMap<TString, TExpressionPtr>;

struct TAstHead
    : public TObjectsHolder
{
    static TAstHead MakeQuery()
    {
        TAstHead result;
        result.Ast.emplace<TQuery>();
        return result;
    }

    static TAstHead MakeExpression()
    {
        TAstHead result;
        result.Ast.emplace<TExpressionPtr>();
        return result;
    }

    std::variant<TQuery, TExpressionPtr> Ast;
    TAliasMap AliasMap;

};

////////////////////////////////////////////////////////////////////////////////

TStringBuf GetSource(TSourceLocation sourceLocation, TStringBuf source);

TString FormatId(TStringBuf id);
TString FormatLiteralValue(const TLiteralValue& value);
TString FormatReference(const TReference& ref);
TString FormatExpression(const TExpression& expr);
TString FormatExpression(const TExpressionList& exprs);
TString FormatJoin(const TJoin& join);
TString FormatQuery(const TQuery& query);
TString InferColumnName(const TExpression& expr);
TString InferColumnName(const TReference& ref);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient::NAst
