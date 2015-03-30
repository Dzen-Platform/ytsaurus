#pragma once

#include "public.h"

#include "plan_fragment_common.h"

#include <ytlib/new_table_client/unversioned_row.h>
#include <ytlib/new_table_client/row_buffer.h>

#include <core/misc/array_ref.h>
#include <core/misc/variant.h>

namespace NYT {
namespace NQueryClient {
namespace NAst {

////////////////////////////////////////////////////////////////////////////////

typedef std::vector<TValue> TValueList;
typedef std::vector<std::vector<TValue>> TValueTupleList;

struct TExpression
    : public TIntrinsicRefCounted
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

    TStringBuf GetSource(const TStringBuf& source) const;

    TSourceLocation SourceLocation;

};

DECLARE_REFCOUNTED_STRUCT(TExpression)
DEFINE_REFCOUNTED_TYPE(TExpression)

typedef std::vector<TExpressionPtr> TExpressionList;

struct TLiteralExpression
    : public TExpression
{
    TLiteralExpression(
        const TSourceLocation& sourceLocation,
        TValue value)
        : TExpression(sourceLocation)
        , Value(value)
    { }

    TValue Value;

};

struct TReferenceExpression
    : public TExpression
{
    TReferenceExpression(
        const TSourceLocation& sourceLocation,
        TStringBuf columnName)
        : TExpression(sourceLocation)
        , ColumnName(columnName)
    { }

    Stroka ColumnName;

};

struct TCommaExpression
    : public TExpression
{
    TCommaExpression(
        const TSourceLocation& sourceLocation,
        const TExpressionPtr& lhs,
        const TExpressionPtr& rhs)
        : TExpression(sourceLocation)
        , Lhs(lhs)
        , Rhs(rhs)
    { }

    TExpressionPtr Lhs;
    TExpressionPtr Rhs;
};

struct TFunctionExpression
    : public TExpression
{
    TFunctionExpression(
        const TSourceLocation& sourceLocation,
        const TStringBuf& functionName,
        const TExpressionPtr& arguments)
        : TExpression(sourceLocation)
        , FunctionName(functionName)
        , Arguments(arguments)
    { }

    Stroka FunctionName;
    TExpressionPtr Arguments;

};

struct TUnaryOpExpression
    : public TExpression
{
    TUnaryOpExpression(
        const TSourceLocation& sourceLocation,
        EUnaryOp opcode,
        const TExpressionPtr& operand)
        : TExpression(sourceLocation)
        , Opcode(opcode)
        , Operand(operand)
    { }

    EUnaryOp Opcode;
    TExpressionPtr Operand;

};

struct TBinaryOpExpression
    : public TExpression
{
    TBinaryOpExpression(
        const TSourceLocation& sourceLocation,
        EBinaryOp opcode,
        const TExpressionPtr& lhs,
        const TExpressionPtr& rhs)
        : TExpression(sourceLocation)
        , Opcode(opcode)
        , Lhs(lhs)
        , Rhs(rhs)
    { }

    EBinaryOp Opcode;
    TExpressionPtr Lhs;
    TExpressionPtr Rhs;

};

struct TInExpression
    : public TExpression
{
    TInExpression(
        const TSourceLocation& sourceLocation,
        const TExpressionPtr& expression,
        const TValueTupleList& values)
        : TExpression(sourceLocation)
        , Expr(expression)
        , Values(values)
    { }

    TExpressionPtr Expr;
    TValueTupleList Values;

};

Stroka InferName(const TExpression* expr);

////////////////////////////////////////////////////////////////////////////////

typedef std::pair<TExpressionPtr, Stroka> TNamedExpression;
typedef std::vector<TNamedExpression> TNamedExpressionList;
typedef TNullable<TNamedExpressionList> TNullableNamedExprs;
typedef std::vector<Stroka> TIdentifierList;

typedef TNullable<TIdentifierList> TNullableIdentifiers;

struct TSource
    : public TIntrinsicRefCounted
{
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

DECLARE_REFCOUNTED_STRUCT(TSource)
DEFINE_REFCOUNTED_TYPE(TSource)

struct TSimpleSource
    : public TSource
{
    explicit TSimpleSource(const Stroka& path)
        : Path(path)
    { }

    Stroka Path;

};

struct TJoinSource
    : public TSource
{
    TJoinSource(
        const Stroka& leftPath,
        const Stroka& rightPath,
        const TIdentifierList& fields)
        : LeftPath(leftPath)
        , RightPath(rightPath)
        , Fields(fields)
    { }

    Stroka LeftPath;
    Stroka RightPath;
    TIdentifierList Fields;

};

struct TQuery
{
    TSourcePtr Source;
    TNullableNamedExprs SelectExprs;
    TExpressionPtr WherePredicate;
    TNullableNamedExprs GroupExprs;
    TNullableIdentifiers OrderFields;
    i64 Limit = 0;
};

typedef TVariant<TQuery, TNamedExpression> TAstHead;

////////////////////////////////////////////////////////////////////////////////

} // namespace NAst
} // namespace NQueryClient
} // namespace NYT
