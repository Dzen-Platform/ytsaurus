#include "query_preparer.h"
#include "private.h"
#include "callbacks.h"
#include "functions.h"
#include "helpers.h"
#include "lexer.h"
#include "query_helpers.h"

#include <yt/ytlib/query_client/parser.h>

#include <yt/client/chunk_client/proto/chunk_spec.pb.h>

#include <yt/core/ytree/yson_serializable.h>
#include <yt/core/ytree/convert.h>

#include <yt/core/misc/collection_helpers.h>
#include <yt/core/misc/finally.h>

#include <yt/core/concurrency/fiber.h>

#include <unordered_set>

namespace NYT::NQueryClient {

using namespace NConcurrency;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

static constexpr size_t MaxExpressionDepth = 50;

#ifdef _asan_enabled_
static const int MinimumStackFreeSpace = 128_KB;
#else
static const int MinimumStackFreeSpace = 16_KB;
#endif

struct TQueryPreparerBufferTag
{ };

////////////////////////////////////////////////////////////////////////////////

namespace {

void CheckStackDepth()
{
    auto* scheduler = TryGetCurrentScheduler();
    if (scheduler && !scheduler->GetCurrentFiber()->CheckFreeStackSpace(MinimumStackFreeSpace)) {
        THROW_ERROR_EXCEPTION("Expression depth causes stack overflow");
    }
}

void ExtractFunctionNames(
    const NAst::TNullableExpressionList& exprs,
    std::vector<TString>* functions);

void ExtractFunctionNames(
    const NAst::TExpressionPtr& expr,
    std::vector<TString>* functions)
{
    if (auto functionExpr = expr->As<NAst::TFunctionExpression>()) {
        functions->push_back(to_lower(functionExpr->FunctionName));
        ExtractFunctionNames(functionExpr->Arguments, functions);
    } else if (auto unaryExpr = expr->As<NAst::TUnaryOpExpression>()) {
        ExtractFunctionNames(unaryExpr->Operand, functions);
    } else if (auto binaryExpr = expr->As<NAst::TBinaryOpExpression>()) {
        ExtractFunctionNames(binaryExpr->Lhs, functions);
        ExtractFunctionNames(binaryExpr->Rhs, functions);
    } else if (auto inExpr = expr->As<NAst::TInExpression>()) {
        ExtractFunctionNames(inExpr->Expr, functions);
    } else if (auto betweenExpr = expr->As<NAst::TBetweenExpression>()) {
        ExtractFunctionNames(betweenExpr->Expr, functions);
    } else if (auto transformExpr = expr->As<NAst::TTransformExpression>()) {
        ExtractFunctionNames(transformExpr->Expr, functions);
        ExtractFunctionNames(transformExpr->DefaultExpr, functions);
    } else if (expr->As<NAst::TLiteralExpression>()) {
    } else if (expr->As<NAst::TReferenceExpression>()) {
    } else if (expr->As<NAst::TAliasExpression>()) {
    } else {
        YT_ABORT();
    }
}

void ExtractFunctionNames(
    const NAst::TNullableExpressionList& exprs,
    std::vector<TString>* functions)
{
    if (!exprs) {
        return;
    }

    CheckStackDepth();

    for (const auto& expr : *exprs) {
        ExtractFunctionNames(expr, functions);
    }
}

std::vector<TString> ExtractFunctionNames(
    const NAst::TQuery& query,
    const NAst::TAliasMap& aliasMap)
{
    std::vector<TString> functions;

    ExtractFunctionNames(query.WherePredicate, &functions);
    ExtractFunctionNames(query.HavingPredicate, &functions);
    ExtractFunctionNames(query.SelectExprs, &functions);

    if (query.GroupExprs) {
        for (const auto& expr : query.GroupExprs->first) {
            ExtractFunctionNames(expr, &functions);
        }
    }

    for (const auto& join : query.Joins) {
        ExtractFunctionNames(join.Lhs, &functions);
        ExtractFunctionNames(join.Rhs, &functions);
    }

    for (const auto& orderExpression : query.OrderExpressions) {
        for (const auto& expr : orderExpression.first) {
            ExtractFunctionNames(expr, &functions);
        }
    }

    for (const auto& aliasedExpression : aliasMap) {
        ExtractFunctionNames(aliasedExpression.second.Get(), &functions);
    }

    std::sort(functions.begin(), functions.end());
    functions.erase(
        std::unique(functions.begin(), functions.end()),
        functions.end());

    return functions;
}

////////////////////////////////////////////////////////////////////////////////

TValue CastValueWithCheck(TValue value, EValueType targetType)
{
    if (value.Type == targetType || value.Type == EValueType::Null) {
        return value;
    }

    if (value.Type == EValueType::Int64) {
        if (targetType == EValueType::Double) {
            auto int64Value = value.Data.Int64;
            if (i64(double(int64Value)) != int64Value) {
                THROW_ERROR_EXCEPTION("Failed to cast %v to double: inaccurate conversion", int64Value);
            }
            value.Data.Double = int64Value;
        } else {
            YT_VERIFY(targetType == EValueType::Uint64);
        }
    } else if (value.Type == EValueType::Uint64) {
        if (targetType == EValueType::Int64) {
            if (value.Data.Uint64 > std::numeric_limits<i64>::max()) {
                THROW_ERROR_EXCEPTION(
                    "Failed to cast %vu to int64: value is greater than maximum", value.Data.Uint64);
            }
        } else if (targetType == EValueType::Double) {
            auto uint64Value = value.Data.Uint64;
            if (ui64(double(uint64Value)) != uint64Value) {
                THROW_ERROR_EXCEPTION("Failed to cast %vu to double: inaccurate conversion", uint64Value);
            }
            value.Data.Double = uint64Value;
        } else {
            YT_ABORT();
        }
    } else if (value.Type == EValueType::Double) {
        auto doubleValue = value.Data.Double;
        if (targetType == EValueType::Uint64) {
            if (double(ui64(doubleValue)) != doubleValue) {
                THROW_ERROR_EXCEPTION("Failed to cast %v to uint64: inaccurate conversion", doubleValue);
            }
            value.Data.Uint64 = doubleValue;
        } else if (targetType == EValueType::Int64) {
            if (double(i64(doubleValue)) != doubleValue) {
                THROW_ERROR_EXCEPTION("Failed to cast %v to int64: inaccurate conversion", doubleValue);
            }
            value.Data.Int64 = doubleValue;
        } else {
            YT_ABORT();
        }
    } else {
        YT_ABORT();
    }

    value.Type = targetType;
    return value;
}

EValueType GetType(const NAst::TLiteralValue& literalValue)
{
    switch (literalValue.index()) {
        case VariantIndexV<NAst::TNullLiteralValue, NAst::TLiteralValue>:
            return EValueType::Null;
        case VariantIndexV<i64, NAst::TLiteralValue>:
            return EValueType::Int64;
        case VariantIndexV<ui64, NAst::TLiteralValue>:
            return EValueType::Uint64;
        case VariantIndexV<double, NAst::TLiteralValue>:
            return EValueType::Double;
        case VariantIndexV<bool, NAst::TLiteralValue>:
            return EValueType::Boolean;
        case VariantIndexV<TString, NAst::TLiteralValue>:
            return EValueType::String;
        default:
            YT_ABORT();
    }
}

TTypeSet GetTypes(const NAst::TLiteralValue& literalValue)
{
    switch (literalValue.index()) {
        case VariantIndexV<NAst::TNullLiteralValue, NAst::TLiteralValue>:
            return TTypeSet({
                EValueType::Null,
                EValueType::Int64,
                EValueType::Uint64,
                EValueType::Double,
                EValueType::Boolean,
                EValueType::String,
                EValueType::Any});
        case VariantIndexV<i64, NAst::TLiteralValue>:
            return TTypeSet({
                EValueType::Int64,
                EValueType::Uint64,
                EValueType::Double});
        case VariantIndexV<ui64, NAst::TLiteralValue>:
            return TTypeSet({
                EValueType::Uint64,
                EValueType::Double});
        case VariantIndexV<double, NAst::TLiteralValue>:
            return TTypeSet({
                EValueType::Double});
        case VariantIndexV<bool, NAst::TLiteralValue>:
            return TTypeSet({
                EValueType::Boolean});
        case VariantIndexV<TString, NAst::TLiteralValue>:
            return TTypeSet({
                EValueType::String});
        default:
            YT_ABORT();
    }
}

TValue GetValue(const NAst::TLiteralValue& literalValue)
{
    switch (literalValue.index()) {
        case VariantIndexV<NAst::TNullLiteralValue, NAst::TLiteralValue>:
            return MakeUnversionedSentinelValue(EValueType::Null);
        case VariantIndexV<i64, NAst::TLiteralValue>:
            return MakeUnversionedInt64Value(std::get<i64>(literalValue));
        case VariantIndexV<ui64, NAst::TLiteralValue>:
            return MakeUnversionedUint64Value(std::get<ui64>(literalValue));
        case VariantIndexV<double, NAst::TLiteralValue>:
            return MakeUnversionedDoubleValue(std::get<double>(literalValue));
        case VariantIndexV<bool, NAst::TLiteralValue>:
            return MakeUnversionedBooleanValue(std::get<bool>(literalValue));
        case VariantIndexV<TString, NAst::TLiteralValue>: {
            const auto& data = std::get<TString>(literalValue);
            return MakeUnversionedStringValue(TStringBuf(data.c_str(), data.length()));
        }

        default:
            YT_ABORT();
    }
}


void BuildRow(
    TUnversionedRowBuilder* rowBuilder,
    const NAst::TLiteralValueTuple& tuple,
    const std::vector<EValueType>& argTypes,
    TStringBuf source)
{
    for (int i = 0; i < tuple.size(); ++i) {
        auto valueType = GetType(tuple[i]);
        auto value = GetValue(tuple[i]);

        if (valueType == EValueType::Null) {
            value = MakeUnversionedSentinelValue(EValueType::Null);
        } else if (valueType != argTypes[i]) {
            if (IsArithmeticType(valueType) && IsArithmeticType(argTypes[i])) {
                value = CastValueWithCheck(value, argTypes[i]);
            } else {
                THROW_ERROR_EXCEPTION("Types mismatch in tuple")
                << TErrorAttribute("source", source)
                << TErrorAttribute("actual_type", valueType)
                << TErrorAttribute("expected_type", argTypes[i]);
            }
        }
        rowBuilder->AddValue(value);
    }
}

TSharedRange<TRow> LiteralTupleListToRows(
    const NAst::TLiteralValueTupleList& literalTuples,
    const std::vector<EValueType>& argTypes,
    TStringBuf source)
{
    auto rowBuffer = New<TRowBuffer>(TQueryPreparerBufferTag());
    TUnversionedRowBuilder rowBuilder;
    std::vector<TRow> rows;
    for (const auto& tuple : literalTuples) {
        if (tuple.size() != argTypes.size()) {
            THROW_ERROR_EXCEPTION("Arguments size mismatch in tuple")
                << TErrorAttribute("source", source);
        }

        BuildRow(&rowBuilder, tuple, argTypes, source);

        rows.push_back(rowBuffer->Capture(rowBuilder.GetRow()));
        rowBuilder.Reset();
    }

    std::sort(rows.begin(), rows.end());
    return MakeSharedRange(std::move(rows), std::move(rowBuffer));
}

TSharedRange<TRowRange> LiteralRangesListToRows(
    const NAst::TLiteralValueRangeList& literalRanges,
    const std::vector<EValueType>& argTypes,
    TStringBuf source)
{
    auto rowBuffer = New<TRowBuffer>(TQueryPreparerBufferTag());
    TUnversionedRowBuilder rowBuilder;
    std::vector<TRowRange> ranges;
    for (const auto& range : literalRanges) {
        if (range.first.size() > argTypes.size()) {
            THROW_ERROR_EXCEPTION("Arguments size mismatch in tuple")
                << TErrorAttribute("source", source);
        }

        if (range.second.size() > argTypes.size()) {
            THROW_ERROR_EXCEPTION("Arguments size mismatch in tuple")
                << TErrorAttribute("source", source);
        }

        BuildRow(&rowBuilder, range.first, argTypes, source);
        auto lower = rowBuffer->Capture(rowBuilder.GetRow());
        rowBuilder.Reset();

        BuildRow(&rowBuilder, range.second, argTypes, source);
        auto upper = rowBuffer->Capture(rowBuilder.GetRow());
        rowBuilder.Reset();

        if (CompareRows(lower, upper, std::min(lower.GetCount(), upper.GetCount())) > 0) {
            THROW_ERROR_EXCEPTION("Lower bound is greater than upper")
                << TErrorAttribute("lower", lower)
                << TErrorAttribute("upper", upper);
        }

        ranges.emplace_back(lower, upper);
    }

    std::sort(ranges.begin(), ranges.end());

    for (size_t index = 1; index < ranges.size(); ++index) {
        TRow previousUpper = ranges[index - 1].second;
        TRow currentLower = ranges[index].first;

        if (CompareRows(
            previousUpper,
            currentLower,
            std::min(previousUpper.GetCount(), currentLower.GetCount())) >= 0)
        {
            THROW_ERROR_EXCEPTION("Ranges are not disjoint")
                << TErrorAttribute("first", ranges[index - 1])
                << TErrorAttribute("second", ranges[index]);
        }
    }

    return MakeSharedRange(std::move(ranges), std::move(rowBuffer));
}

std::optional<TUnversionedValue> FoldConstants(
    EUnaryOp opcode,
    const TConstExpressionPtr& operand)
{
    if (auto literalExpr = operand->As<TLiteralExpression>()) {
        if (opcode == EUnaryOp::Plus) {
            return static_cast<TUnversionedValue>(literalExpr->Value);
        } else if (opcode == EUnaryOp::Minus) {
            TUnversionedValue value = literalExpr->Value;
            switch (value.Type) {
                case EValueType::Int64:
                    value.Data.Int64 = -value.Data.Int64;
                    break;
                case EValueType::Uint64:
                    value.Data.Uint64 = -value.Data.Uint64;
                    break;
                case EValueType::Double:
                    value.Data.Double = -value.Data.Double;
                    break;
                default:
                    YT_ABORT();
            }
            return value;
        } else if (opcode == EUnaryOp::BitNot) {
            TUnversionedValue value = literalExpr->Value;
            switch (value.Type) {
                case EValueType::Int64:
                    value.Data.Int64 = ~value.Data.Int64;
                    break;
                case EValueType::Uint64:
                    value.Data.Uint64 = ~value.Data.Uint64;
                    break;
                default:
                    YT_ABORT();
            }
            return value;
        }
    }
    return std::nullopt;
}

std::optional<TUnversionedValue> FoldConstants(
    EBinaryOp opcode,
    const TConstExpressionPtr& lhsExpr,
    const TConstExpressionPtr& rhsExpr)
{
    auto lhsLiteral = lhsExpr->As<TLiteralExpression>();
    auto rhsLiteral = rhsExpr->As<TLiteralExpression>();
    if (lhsLiteral && rhsLiteral) {
        auto lhs = static_cast<TUnversionedValue>(lhsLiteral->Value);
        auto rhs = static_cast<TUnversionedValue>(rhsLiteral->Value);

        auto checkType = [&] () {
            if (lhs.Type != rhs.Type) {
                if (IsArithmeticType(lhs.Type) && IsArithmeticType(rhs.Type)) {
                    auto targetType = std::max(lhs.Type, rhs.Type);
                    lhs = CastValueWithCheck(lhs, targetType);
                    rhs = CastValueWithCheck(rhs, targetType);
                } else {
                    ThrowTypeMismatchError(lhs.Type, rhs.Type, "", InferName(lhsExpr), InferName(rhsExpr));
                }
            }
        };

        auto checkTypeIfNotNull = [&] () {
            if (lhs.Type != EValueType::Null && rhs.Type != EValueType::Null) {
                checkType();
            }
        };

        #define CHECK_TYPE() \
            if (lhs.Type == EValueType::Null) { \
                return MakeUnversionedSentinelValue(EValueType::Null); \
            } \
            if (rhs.Type == EValueType::Null) { \
                return MakeUnversionedSentinelValue(EValueType::Null); \
            } \
            checkType();

        auto evaluateLogicalOp = [&] (bool parameter) {
            YT_VERIFY(lhs.Type == EValueType::Null || lhs.Type == EValueType::Boolean);
            YT_VERIFY(rhs.Type == EValueType::Null || rhs.Type == EValueType::Boolean);

            if (lhs.Type == EValueType::Null) {
                if (rhs.Type != EValueType::Null && rhs.Data.Boolean == parameter) {
                    return rhs;
                } else {
                    return lhs;
                }
            } else if (lhs.Data.Boolean == parameter) {
                return lhs;
            } else {
                return rhs;
            }
        };

        switch (opcode) {
            case EBinaryOp::Plus:
                CHECK_TYPE();
                switch (lhs.Type) {
                    case EValueType::Int64:
                        lhs.Data.Int64 += rhs.Data.Int64;
                        return lhs;
                    case EValueType::Uint64:
                        lhs.Data.Uint64 += rhs.Data.Uint64;
                        return lhs;
                    case EValueType::Double:
                        lhs.Data.Double += rhs.Data.Double;
                        return lhs;
                    default:
                        break;
                }
                break;
            case EBinaryOp::Minus:
                CHECK_TYPE();
                switch (lhs.Type) {
                    case EValueType::Int64:
                        lhs.Data.Int64 -= rhs.Data.Int64;
                        return lhs;
                    case EValueType::Uint64:
                        lhs.Data.Uint64 -= rhs.Data.Uint64;
                        return lhs;
                    case EValueType::Double:
                        lhs.Data.Double -= rhs.Data.Double;
                        return lhs;
                    default:
                        break;
                }
                break;
            case EBinaryOp::Multiply:
                CHECK_TYPE();
                switch (lhs.Type) {
                    case EValueType::Int64:
                        lhs.Data.Int64 *= rhs.Data.Int64;
                        return lhs;
                    case EValueType::Uint64:
                        lhs.Data.Uint64 *= rhs.Data.Uint64;
                        return lhs;
                    case EValueType::Double:
                        lhs.Data.Double *= rhs.Data.Double;
                        return lhs;
                    default:
                        break;
                }
                break;
            case EBinaryOp::Divide:
                CHECK_TYPE();
                switch (lhs.Type) {
                    case EValueType::Int64:
                        if (rhs.Data.Int64 == 0) {
                            THROW_ERROR_EXCEPTION("Division by zero");
                        }
                        lhs.Data.Int64 /= rhs.Data.Int64;
                        return lhs;
                    case EValueType::Uint64:
                        if (rhs.Data.Uint64 == 0) {
                            THROW_ERROR_EXCEPTION("Division by zero");
                        }
                        lhs.Data.Uint64 /= rhs.Data.Uint64;
                        return lhs;
                    case EValueType::Double:
                        lhs.Data.Double /= rhs.Data.Double;
                        return lhs;
                    default:
                        break;
                }
                break;
            case EBinaryOp::Modulo:
                CHECK_TYPE();
                switch (lhs.Type) {
                    case EValueType::Int64:
                        if (rhs.Data.Int64 == 0) {
                            THROW_ERROR_EXCEPTION("Division by zero");
                        }
                        lhs.Data.Int64 %= rhs.Data.Int64;
                        return lhs;
                    case EValueType::Uint64:
                        if (rhs.Data.Uint64 == 0) {
                            THROW_ERROR_EXCEPTION("Division by zero");
                        }
                        lhs.Data.Uint64 %= rhs.Data.Uint64;
                        return lhs;
                    default:
                        break;
                }
                break;
            case EBinaryOp::LeftShift:
                CHECK_TYPE();
                switch (lhs.Type) {
                    case EValueType::Int64:
                        lhs.Data.Int64 <<= rhs.Data.Int64;
                        return lhs;
                    case EValueType::Uint64:
                        lhs.Data.Uint64 <<= rhs.Data.Uint64;
                        return lhs;
                    default:
                        break;
                }
                break;
            case EBinaryOp::RightShift:
                CHECK_TYPE();
                switch (lhs.Type) {
                    case EValueType::Int64:
                        lhs.Data.Int64 >>= rhs.Data.Int64;
                        return lhs;
                    case EValueType::Uint64:
                        lhs.Data.Uint64 >>= rhs.Data.Uint64;
                        return lhs;
                    default:
                        break;
                }
                break;
            case EBinaryOp::BitOr:
                CHECK_TYPE();
                switch (lhs.Type) {
                    case EValueType::Uint64:
                        lhs.Data.Uint64 = lhs.Data.Uint64 | rhs.Data.Uint64;
                        return lhs;
                    case EValueType::Int64:
                        lhs.Data.Int64 = lhs.Data.Int64 | rhs.Data.Int64;
                        return lhs;
                    default:
                        break;
                }
                break;
            case EBinaryOp::BitAnd:
                CHECK_TYPE();
                switch (lhs.Type) {
                    case EValueType::Uint64:
                        lhs.Data.Uint64 = lhs.Data.Uint64 & rhs.Data.Uint64;
                        return lhs;
                    case EValueType::Int64:
                        lhs.Data.Int64 = lhs.Data.Int64 & rhs.Data.Int64;
                        return lhs;
                    default:
                        break;
                }
                break;
            case EBinaryOp::And:
                return evaluateLogicalOp(false);
                break;
            case EBinaryOp::Or:
                return evaluateLogicalOp(true);
                break;
            case EBinaryOp::Equal:
                checkTypeIfNotNull();
                return MakeUnversionedBooleanValue(CompareRowValues(lhs, rhs) == 0);
                break;
            case EBinaryOp::NotEqual:
                checkTypeIfNotNull();
                return MakeUnversionedBooleanValue(CompareRowValues(lhs, rhs) != 0);
                break;
            case EBinaryOp::Less:
                checkTypeIfNotNull();
                return MakeUnversionedBooleanValue(CompareRowValues(lhs, rhs) < 0);
                break;
            case EBinaryOp::Greater:
                checkTypeIfNotNull();
                return MakeUnversionedBooleanValue(CompareRowValues(lhs, rhs) > 0);
                break;
            case EBinaryOp::LessOrEqual:
                checkTypeIfNotNull();
                return MakeUnversionedBooleanValue(CompareRowValues(lhs, rhs) <= 0);
                break;
            case EBinaryOp::GreaterOrEqual:
                checkTypeIfNotNull();
                return MakeUnversionedBooleanValue(CompareRowValues(lhs, rhs) >= 0);
                break;
            default:
                break;
        }
    }
    return std::nullopt;
}

struct TNotExpressionPropagator
    : TRewriter<TNotExpressionPropagator>
{
    using TBase = TRewriter<TNotExpressionPropagator>;

    TConstExpressionPtr OnUnary(const TUnaryOpExpression* unaryExpr)
    {
        auto& operand = unaryExpr->Operand;
        if (unaryExpr->Opcode == EUnaryOp::Not) {
            if (auto operandUnaryOp = operand->As<TUnaryOpExpression>()) {
                if (operandUnaryOp->Opcode == EUnaryOp::Not) {
                    return Visit(operandUnaryOp->Operand);
                }
            } else if (auto operandBinaryOp = operand->As<TBinaryOpExpression>()) {
                if (operandBinaryOp->Opcode == EBinaryOp::And) {
                    return Visit(New<TBinaryOpExpression>(
                        EValueType::Boolean,
                        EBinaryOp::Or,
                        New<TUnaryOpExpression>(
                            operandBinaryOp->Lhs->Type,
                            EUnaryOp::Not,
                            operandBinaryOp->Lhs),
                        New<TUnaryOpExpression>(
                            operandBinaryOp->Rhs->Type,
                            EUnaryOp::Not,
                            operandBinaryOp->Rhs)));
                } else if (operandBinaryOp->Opcode == EBinaryOp::Or) {
                    return Visit(New<TBinaryOpExpression>(
                        EValueType::Boolean,
                        EBinaryOp::And,
                        New<TUnaryOpExpression>(
                            operandBinaryOp->Lhs->Type,
                            EUnaryOp::Not,
                            operandBinaryOp->Lhs),
                        New<TUnaryOpExpression>(
                            operandBinaryOp->Rhs->Type,
                            EUnaryOp::Not,
                            operandBinaryOp->Rhs)));
                } else if (IsRelationalBinaryOp(operandBinaryOp->Opcode)) {
                    return Visit(New<TBinaryOpExpression>(
                        operandBinaryOp->Type,
                        GetInversedBinaryOpcode(operandBinaryOp->Opcode),
                        operandBinaryOp->Lhs,
                        operandBinaryOp->Rhs));
                }
            } else if (auto literal = operand->As<TLiteralExpression>()) {
                TUnversionedValue value = literal->Value;
                value.Data.Boolean = !value.Data.Boolean;
                return New<TLiteralExpression>(
                    literal->Type,
                    value);
            }
        }

        return TBase::OnUnary(unaryExpr);
    }
};

struct TCastEliminator
    : TRewriter<TCastEliminator>
{
    using TBase = TRewriter<TCastEliminator>;

    TConstExpressionPtr OnFunction(const TFunctionExpression* functionExpr)
    {
        if (IsUserCastFunction(functionExpr->FunctionName)) {
            YT_VERIFY(functionExpr->Arguments.size() == 1);

            if (functionExpr->Type == functionExpr->Arguments[0]->Type) {
                return Visit(functionExpr->Arguments[0]);
            }
        }

        return TBase::OnFunction(functionExpr);
    }
};

struct TExpressionSimplifier
    : TRewriter<TExpressionSimplifier>
{
    using TBase = TRewriter<TExpressionSimplifier>;

    TConstExpressionPtr OnFunction(const TFunctionExpression* functionExpr)
    {
        if (functionExpr->FunctionName == "if") {
            if (auto functionCondition = functionExpr->Arguments[0]->As<TFunctionExpression>()) {
                auto reference1 = functionExpr->Arguments[2]->As<TReferenceExpression>();
                if (functionCondition->FunctionName == "is_null" && reference1) {
                    auto reference0 = functionCondition->Arguments[0]->As<TReferenceExpression>();
                    if (reference0 && reference1->ColumnName == reference0->ColumnName) {
                        return New<TFunctionExpression>(
                            functionExpr->Type,
                            "if_null",
                            std::vector<TConstExpressionPtr>{
                                functionCondition->Arguments[0],
                                functionExpr->Arguments[1]});

                    }
                }
            }
        }

        return TBase::OnFunction(functionExpr);
    }
};

struct TTypedExpressionBuilder;

typedef std::function<TConstExpressionPtr(EValueType)> TExpressionGenerator;

struct TUntypedExpression
{
    TTypeSet FeasibleTypes;
    TExpressionGenerator Generator;
    bool IsConstant;
};

DECLARE_REFCOUNTED_CLASS(ISchemaProxy)

class ISchemaProxy
    : public TIntrinsicRefCounted
{
public:
    virtual std::optional<TBaseColumn> GetColumnPtr(
        const NAst::TReference& reference) = 0;

    virtual TUntypedExpression GetAggregateColumnPtr(
        const TString& columnName,
        const TAggregateTypeInferrer* aggregateFunction,
        const NAst::TExpression* arguments,
        const TString& subexprName,
        const TTypedExpressionBuilder& builder) = 0;
};

DEFINE_REFCOUNTED_TYPE(ISchemaProxy)

bool Unify(TTypeSet* genericAssignments, const TTypeSet& types)
{
    auto intersection = *genericAssignments & types;

    if (intersection.IsEmpty()) {
        return false;
    } else {
        *genericAssignments = intersection;
        return true;
    }
}

EValueType GetFrontWithCheck(const TTypeSet& typeSet, TStringBuf source)
{
    auto result = typeSet.GetFront();
    if (result == EValueType::Null) {
        THROW_ERROR_EXCEPTION("Type inference failed")
            << TErrorAttribute("actual_type", EValueType::Null)
            << TErrorAttribute("source", source);
    }
    return result;
}

TTypeSet InferFunctionTypes(
    const TFunctionTypeInferrer* inferrer,
    const std::vector<TTypeSet>& effectiveTypes,
    std::vector<TTypeSet>* genericAssignments,
    TStringBuf functionName,
    TStringBuf source)
{
    std::vector<TTypeSet> typeConstraints;
    std::vector<size_t> formalArguments;
    std::optional<std::pair<size_t, bool>> repeatedType;
    size_t formalResultType = inferrer->GetNormalizedConstraints(
        &typeConstraints,
        &formalArguments,
        &repeatedType);

    *genericAssignments = typeConstraints;

    auto argIndex = 1;
    auto arg = effectiveTypes.begin();
    auto formalArg = formalArguments.begin();
    for (;
        formalArg != formalArguments.end() && arg != effectiveTypes.end();
        arg++, formalArg++, argIndex++)
    {
        auto& constraints = (*genericAssignments)[*formalArg];
        if (!Unify(&constraints, *arg)) {
            THROW_ERROR_EXCEPTION(
                "Wrong type for argument %v to function %Qv: expected %Qv, got %Qv",
                argIndex,
                functionName,
                constraints,
                *arg)
                << TErrorAttribute("expression", source);
        }
    }

    bool hasNoRepeatedArgument = !repeatedType.operator bool();

    if (formalArg != formalArguments.end() ||
        (arg != effectiveTypes.end() && hasNoRepeatedArgument))
    {
        THROW_ERROR_EXCEPTION(
            "Wrong number of arguments to function %Qv: expected %v, got %v",
            functionName,
            formalArguments.size(),
            effectiveTypes.size())
            << TErrorAttribute("expression", source);
    }

    for (; arg != effectiveTypes.end(); arg++) {
        size_t constraintIndex = repeatedType->first;
        if (repeatedType->second) {
            constraintIndex = genericAssignments->size();
            genericAssignments->push_back((*genericAssignments)[repeatedType->first]);
        }
        auto& constraints = (*genericAssignments)[constraintIndex];
        if (!Unify(&constraints, *arg)) {
            THROW_ERROR_EXCEPTION(
                "Wrong type for repeated argument to function %Qv: expected %Qv, got %Qv",
                functionName,
                constraints,
                *arg)
                << TErrorAttribute("expression", source);
        }
    }

    return (*genericAssignments)[formalResultType];
}

std::vector<EValueType> RefineFunctionTypes(
    const TFunctionTypeInferrer* inferrer,
    EValueType resultType,
    size_t argumentCount,
    std::vector<TTypeSet>* genericAssignments,
    TStringBuf source)
{
    std::vector<TTypeSet> typeConstraints;
    std::vector<size_t> formalArguments;
    std::optional<std::pair<size_t, bool>> repeatedType;
    size_t formalResultType = inferrer->GetNormalizedConstraints(
        &typeConstraints,
        &formalArguments,
        &repeatedType);

    (*genericAssignments)[formalResultType] = TTypeSet({resultType});

    std::vector<EValueType> genericAssignmentsMin;
    for (auto& constraint : *genericAssignments) {
        genericAssignmentsMin.push_back(GetFrontWithCheck(constraint, source));
    }

    std::vector<EValueType> effectiveTypes;
    auto argIndex = 0;
    auto formalArg = formalArguments.begin();
    for (;
        formalArg != formalArguments.end() && argIndex < argumentCount;
        ++formalArg, ++argIndex)
    {
        effectiveTypes.push_back(genericAssignmentsMin[*formalArg]);
    }

    for (; argIndex < argumentCount; ++argIndex) {
        size_t constraintIndex = repeatedType->first;
        if (repeatedType->second) {
            constraintIndex = genericAssignments->size() - (argumentCount - argIndex);
        }

        effectiveTypes.push_back(genericAssignmentsMin[constraintIndex]);
    }

    return effectiveTypes;
}

// 1. Init generic assignments with constraints
//    Intersect generic assignments with argument types and save them
//    Infer feasible result types
// 2. Apply result types and restrict generic assignments and argument types

struct TOperatorTyper
{
    TTypeSet Constraint;
    std::optional<EValueType> ResultType;
};

TEnumIndexedVector<TOperatorTyper, EBinaryOp> BuildBinaryOperatorTypers()
{
    TEnumIndexedVector<TOperatorTyper, EBinaryOp> result;

    for (auto op : {
        EBinaryOp::Plus,
        EBinaryOp::Minus,
        EBinaryOp::Multiply,
        EBinaryOp::Divide})
    {
        result[op] = {
            TTypeSet({EValueType::Int64, EValueType::Uint64, EValueType::Double}),
            std::nullopt
        };
    }

    for (auto op : {
        EBinaryOp::Modulo,
        EBinaryOp::LeftShift,
        EBinaryOp::RightShift,
        EBinaryOp::BitOr,
        EBinaryOp::BitAnd})
    {
        result[op] = {
            TTypeSet({EValueType::Int64, EValueType::Uint64}),
            std::nullopt
        };
    }

    for (auto op : {
        EBinaryOp::And,
        EBinaryOp::Or})
    {
        result[op] = {
            TTypeSet({EValueType::Boolean}),
            EValueType::Boolean
        };
    }

    for (auto op : {
        EBinaryOp::Equal,
        EBinaryOp::NotEqual,
        EBinaryOp::Less,
        EBinaryOp::Greater,
        EBinaryOp::LessOrEqual,
        EBinaryOp::GreaterOrEqual})
    {
        result[op] = {
            TTypeSet({
                EValueType::Int64,
                EValueType::Uint64,
                EValueType::Double,
                EValueType::Boolean,
                EValueType::String,
                EValueType::Any}),
            EValueType::Boolean
        };
    }

    return result;
}

const TEnumIndexedVector<TOperatorTyper, EBinaryOp>& GetBinaryOperatorTypers()
{
    static auto result = BuildBinaryOperatorTypers();
    return result;
}

TEnumIndexedVector<TOperatorTyper, EUnaryOp> BuildUnaryOperatorTypers()
{
    TEnumIndexedVector<TOperatorTyper, EUnaryOp> result;

    for (auto op : {
        EUnaryOp::Plus,
        EUnaryOp::Minus})
    {
        result[op] = {
            TTypeSet({EValueType::Int64, EValueType::Uint64, EValueType::Double}),
            std::nullopt
        };
    }

    result[EUnaryOp::BitNot] = {
        TTypeSet({EValueType::Int64, EValueType::Uint64}),
        std::nullopt
    };

    result[EUnaryOp::Not] = {
        TTypeSet({EValueType::Boolean}),
        std::nullopt
    };

    return result;
}

const TEnumIndexedVector<TOperatorTyper, EUnaryOp>& GetUnaryOperatorTypers()
{
    static auto result = BuildUnaryOperatorTypers();
    return result;
}

TTypeSet InferBinaryExprTypes(
    EBinaryOp opCode,
    const TTypeSet& lhsTypes,
    const TTypeSet& rhsTypes,
    TTypeSet* genericAssignments,
    TStringBuf lhsSource,
    TStringBuf rhsSource)
{
    if (IsRelationalBinaryOp(opCode) && (lhsTypes & rhsTypes).IsEmpty()) {
        return TTypeSet{EValueType::Boolean};
    }

    const auto& binaryOperators = GetBinaryOperatorTypers();

    *genericAssignments = binaryOperators[opCode].Constraint;

    if (!Unify(genericAssignments, lhsTypes)) {
        THROW_ERROR_EXCEPTION("Type mismatch in expression %Qv: expected %Qv, got %Qv",
            opCode,
            *genericAssignments,
            lhsTypes)
            << TErrorAttribute("lhs_source", lhsSource)
            << TErrorAttribute("rhs_source", rhsSource);
    }

    if (!Unify(genericAssignments, rhsTypes)) {
        THROW_ERROR_EXCEPTION("Type mismatch in expression %Qv: expected %Qv, got %Qv",
            opCode,
            *genericAssignments,
            rhsTypes)
            << TErrorAttribute("lhs_source", lhsSource)
            << TErrorAttribute("rhs_source", rhsSource);
    }

    TTypeSet resultTypes;
    if (binaryOperators[opCode].ResultType) {
        resultTypes = TTypeSet({*binaryOperators[opCode].ResultType});
    } else {
        resultTypes = *genericAssignments;
    }

    return resultTypes;
}

std::pair<EValueType, EValueType> RefineBinaryExprTypes(
    EBinaryOp opCode,
    EValueType resultType,
    const TTypeSet& lhsTypes,
    const TTypeSet& rhsTypes,
    TTypeSet* genericAssignments,
    TStringBuf lhsSource,
    TStringBuf rhsSource,
    TStringBuf source)
{
    if (IsRelationalBinaryOp(opCode) && (lhsTypes & rhsTypes).IsEmpty()) {
        // Empty intersection (Any, alpha) || (alpha, Any), where alpha = {bool, int, uint, double, string}
        if (lhsTypes.Get(EValueType::Any)) {
            return std::make_pair(EValueType::Any, GetFrontWithCheck(rhsTypes, rhsSource));
        }

        if (rhsTypes.Get(EValueType::Any)) {
            return std::make_pair(GetFrontWithCheck(lhsTypes, lhsSource), EValueType::Any);
        }

        THROW_ERROR_EXCEPTION("Type mismatch in expression")
            << TErrorAttribute("lhs_source", lhsSource)
            << TErrorAttribute("rhs_source", rhsSource);
    }

    const auto& binaryOperators = GetBinaryOperatorTypers();

    EValueType argType;
    if (binaryOperators[opCode].ResultType) {
        argType = GetFrontWithCheck(*genericAssignments, source);
    } else {
        YT_VERIFY(genericAssignments->Get(resultType));
        argType = resultType;
    }

    return std::make_pair(argType, argType);
}

TTypeSet InferUnaryExprTypes(
    EUnaryOp opCode,
    const TTypeSet& argTypes,
    TTypeSet* genericAssignments,
    TStringBuf opSource)
{
    const auto& unaryOperators = GetUnaryOperatorTypers();

    *genericAssignments = unaryOperators[opCode].Constraint;

    if (!Unify(genericAssignments, argTypes)) {
        THROW_ERROR_EXCEPTION("Type mismatch in expression %Qv: expected %Qv, got %Qv",
            opCode,
            *genericAssignments,
            argTypes)
            << TErrorAttribute("op_source", opSource);
    }

    TTypeSet resultTypes;
    if (unaryOperators[opCode].ResultType) {
        resultTypes = TTypeSet({*unaryOperators[opCode].ResultType});
    } else {
        resultTypes = *genericAssignments;
    }

    return resultTypes;
}

EValueType RefineUnaryExprTypes(
    EUnaryOp opCode,
    EValueType resultType,
    TTypeSet* genericAssignments,
    TStringBuf opSource)
{
    const auto& unaryOperators = GetUnaryOperatorTypers();

    EValueType argType;
    if (unaryOperators[opCode].ResultType) {
        argType = GetFrontWithCheck(*genericAssignments, opSource);
    } else {
        YT_VERIFY(genericAssignments->Get(resultType));
        argType = resultType;
    }

    return argType;
}

struct TTypedExpressionBuilder
{
    const TString& Source;
    const TConstTypeInferrerMapPtr& Functions;
    const NAst::TAliasMap& AliasMap;
    mutable std::set<TString> UsedAliases;
    mutable bool AfterGroupBy;
    mutable size_t Depth;

    TUntypedExpression DoBuildUntypedExpression(
        const NAst::TReference& reference,
        ISchemaProxyPtr schema) const;

    TUntypedExpression DoBuildUntypedFunctionExpression(
        const NAst::TFunctionExpression* functionExpr,
        ISchemaProxyPtr schema) const;

    TUntypedExpression DoBuildUntypedUnaryExpression(
        const NAst::TUnaryOpExpression* unaryExpr,
        ISchemaProxyPtr schema) const;

    TUntypedExpression MakeBinaryExpr(
        const NAst::TBinaryOpExpression* binaryExpr,
        EBinaryOp op,
        TUntypedExpression lhs,
        TUntypedExpression rhs,
        std::optional<size_t> offset) const;

    TUntypedExpression DoBuildUntypedBinaryExpression(
        const NAst::TBinaryOpExpression* binaryExpr,
        ISchemaProxyPtr schema) const;

    void InferArgumentTypes(
        std::vector<TConstExpressionPtr>* typedArguments,
        std::vector<EValueType>* argTypes,
        const NAst::TExpressionList& expressions,
        ISchemaProxyPtr schema,
        TStringBuf operatorName,
        TStringBuf source) const;

    TUntypedExpression DoBuildUntypedInExpression(
        const NAst::TInExpression* inExpr,
        ISchemaProxyPtr schema) const;

    TUntypedExpression DoBuildUntypedBetweenExpression(
        const NAst::TBetweenExpression* betweenExpr,
        ISchemaProxyPtr schema) const;

    TUntypedExpression DoBuildUntypedTransformExpression(
        const NAst::TTransformExpression* transformExpr,
        ISchemaProxyPtr schema) const;

    TUntypedExpression DoBuildUntypedExpression(
        const NAst::TExpression* expr,
        ISchemaProxyPtr schema) const
    {
        CheckStackDepth();

        ++Depth;
        auto depthGuard = Finally([&] {
            --Depth;
        });

        if (Depth > MaxExpressionDepth) {
            THROW_ERROR_EXCEPTION("Maximum expression depth exceeded")
                << TErrorAttribute("max_expression_depth", MaxExpressionDepth);
        }

        if (auto literalExpr = expr->As<NAst::TLiteralExpression>()) {
            const auto& literalValue = literalExpr->Value;

            auto resultTypes = GetTypes(literalValue);
            TExpressionGenerator generator = [literalValue] (EValueType type) {
                return New<TLiteralExpression>(
                    type,
                    CastValueWithCheck(GetValue(literalValue), type));
            };
            return TUntypedExpression{resultTypes, std::move(generator), true};
        } else if (auto aliasExpr = expr->As<NAst::TAliasExpression>()) {
            return DoBuildUntypedExpression(NAst::TReference(aliasExpr->Name), schema);
        } else if (auto referenceExpr = expr->As<NAst::TReferenceExpression>()) {
            return DoBuildUntypedExpression(referenceExpr->Reference, schema);
        } else if (auto functionExpr = expr->As<NAst::TFunctionExpression>()) {
            return DoBuildUntypedFunctionExpression(functionExpr, schema);
        } else if (auto unaryExpr = expr->As<NAst::TUnaryOpExpression>()) {
            return DoBuildUntypedUnaryExpression(unaryExpr, schema);
        } else if (auto binaryExpr = expr->As<NAst::TBinaryOpExpression>()) {
            return DoBuildUntypedBinaryExpression(binaryExpr, schema);
        } else if (auto inExpr = expr->As<NAst::TInExpression>()) {
            return DoBuildUntypedInExpression(inExpr, schema);
        } else if (auto betweenExpr = expr->As<NAst::TBetweenExpression>()) {
            return DoBuildUntypedBetweenExpression(betweenExpr, schema);
        } else if (auto transformExpr = expr->As<NAst::TTransformExpression>()) {
            return DoBuildUntypedTransformExpression(transformExpr, schema);
        }

        YT_ABORT();
    }

    TUntypedExpression BuildUntypedExpression(
        const NAst::TExpression* expr,
        ISchemaProxyPtr schema) const
    {
        return DoBuildUntypedExpression(expr, schema);
    }

    TConstExpressionPtr BuildTypedExpression(
        const NAst::TExpression* expr,
        ISchemaProxyPtr schema,
        TTypeSet feasibleTypes = TTypeSet({
            EValueType::Null,
            EValueType::Int64,
            EValueType::Uint64,
            EValueType::Double,
            EValueType::Boolean,
            EValueType::String,
            EValueType::Any})) const
    {
        auto expressionTyper = BuildUntypedExpression(expr, schema);
        YT_VERIFY(!expressionTyper.FeasibleTypes.IsEmpty());

        if (!Unify(&feasibleTypes, expressionTyper.FeasibleTypes)) {
            THROW_ERROR_EXCEPTION("Type mismatch in expression: expected %Qv, got %Qv",
                feasibleTypes,
                expressionTyper.FeasibleTypes)
                << TErrorAttribute("source", expr->GetSource(Source));
        }

        auto result = expressionTyper.Generator(
            GetFrontWithCheck(feasibleTypes, expr->GetSource(Source)));

        result = TCastEliminator().Visit(result);
        result = TExpressionSimplifier().Visit(result);
        result = TNotExpressionPropagator().Visit(result);
        return result;
    }

};

TUntypedExpression TTypedExpressionBuilder::DoBuildUntypedExpression(
    const NAst::TReference& reference,
    ISchemaProxyPtr schema) const
{
    if (AfterGroupBy) {
        if (auto column = schema->GetColumnPtr(reference)) {
            TTypeSet resultTypes({column->Type});
            TExpressionGenerator generator = [name = column->Name] (EValueType type) {
                return New<TReferenceExpression>(type, name);
            };
            return TUntypedExpression{resultTypes, std::move(generator), false};
        }
    }

    if (!reference.TableName) {
        const auto& columnName = reference.ColumnName;
        auto found = AliasMap.find(columnName);

        if (found != AliasMap.end()) {
            // try InferName(found, expand aliases = true)

            if (UsedAliases.insert(columnName).second) {
                auto aliasExpr = DoBuildUntypedExpression(
                    found->second.Get(),
                    schema);
                UsedAliases.erase(columnName);
                return aliasExpr;
            }
        }
    }

    if (!AfterGroupBy) {
        if (auto column = schema->GetColumnPtr(reference)) {
            TTypeSet resultTypes({column->Type});
            TExpressionGenerator generator = [name = column->Name] (EValueType type) {
                return New<TReferenceExpression>(type, name);
            };
            return TUntypedExpression{resultTypes, std::move(generator), false};
        }
    }

    THROW_ERROR_EXCEPTION("Undefined reference %Qv",
        NAst::InferColumnName(reference));
}

TUntypedExpression TTypedExpressionBuilder::DoBuildUntypedFunctionExpression(
    const NAst::TFunctionExpression* functionExpr,
    ISchemaProxyPtr schema) const
{
    auto functionName = functionExpr->FunctionName;
    functionName.to_lower();

    const auto& descriptor = Functions->GetFunction(functionName);

    if (const auto* aggregateFunction = descriptor->As<TAggregateTypeInferrer>()) {
        auto subexprName = InferColumnName(*functionExpr);

        try {
            if (functionExpr->Arguments.size() != 1) {
                THROW_ERROR_EXCEPTION(
                    "Aggregate function %Qv must have exactly one argument",
                    functionName);
            }

            auto aggregateColumn = schema->GetAggregateColumnPtr(
                functionName,
                aggregateFunction,
                functionExpr->Arguments.front().Get(),
                subexprName,
                *this);

            return aggregateColumn;
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error creating aggregate")
                << TErrorAttribute("source", functionExpr->GetSource(Source))
                << ex;
        }
    } else if (const auto* regularFunction = descriptor->As<TFunctionTypeInferrer>()) {
        std::vector<TTypeSet> argTypes;
        std::vector<TExpressionGenerator> operandTypers;
        for (const auto& argument : functionExpr->Arguments) {
            auto untypedArgument = DoBuildUntypedExpression(
                argument.Get(),
                schema);
            argTypes.push_back(untypedArgument.FeasibleTypes);
            operandTypers.push_back(untypedArgument.Generator);
        }

        std::vector<TTypeSet> genericAssignments;
        auto resultTypes = InferFunctionTypes(
            regularFunction,
            argTypes,
            &genericAssignments,
            functionName,
            functionExpr->GetSource(Source));

        TExpressionGenerator generator = [
            functionName,
            regularFunction,
            operandTypers,
            genericAssignments,
            source = functionExpr->GetSource(Source)]
        (EValueType type) mutable
        {
            auto effectiveTypes = RefineFunctionTypes(
                regularFunction,
                type,
                operandTypers.size(),
                &genericAssignments,
                source);

            std::vector<TConstExpressionPtr> typedOperands;
            for (size_t index = 0; index < effectiveTypes.size(); ++index) {
                typedOperands.push_back(operandTypers[index](effectiveTypes[index]));
            }

            return New<TFunctionExpression>(type, functionName, typedOperands);
        };

        return TUntypedExpression{resultTypes, std::move(generator), false};
    } else {
        YT_ABORT();
    }
}

TUntypedExpression TTypedExpressionBuilder::DoBuildUntypedUnaryExpression(
    const NAst::TUnaryOpExpression* unaryExpr,
    ISchemaProxyPtr schema) const
{
    if (unaryExpr->Operand.size() != 1) {
        THROW_ERROR_EXCEPTION(
            "Unary operator %Qv must have exactly one argument",
            unaryExpr->Opcode);
    }

    auto untypedOperand = DoBuildUntypedExpression(
        unaryExpr->Operand.front().Get(),
        schema);

    TTypeSet genericAssignments;
    auto resultTypes = InferUnaryExprTypes(
        unaryExpr->Opcode,
        untypedOperand.FeasibleTypes,
        &genericAssignments,
        unaryExpr->Operand.front()->GetSource(Source));

    if (untypedOperand.IsConstant) {
        auto value = untypedOperand.Generator(untypedOperand.FeasibleTypes.GetFront());
        if (auto foldedExpr = FoldConstants(unaryExpr->Opcode, value)) {
            TExpressionGenerator generator = [foldedExpr] (EValueType type) {
                return New<TLiteralExpression>(
                    type,
                    CastValueWithCheck(*foldedExpr, type));
            };
            return TUntypedExpression{resultTypes, std::move(generator), true};
        }
    }

    TExpressionGenerator generator = [
        op = unaryExpr->Opcode,
        untypedOperand,
        genericAssignments,
        opSource = unaryExpr->Operand.front()->GetSource(Source)
    ] (EValueType type) mutable {
        auto argType = RefineUnaryExprTypes(
            op,
            type,
            &genericAssignments,
            opSource);
        return New<TUnaryOpExpression>(type, op, untypedOperand.Generator(argType));
    };
    return TUntypedExpression{resultTypes, std::move(generator), false};
}

TUntypedExpression TTypedExpressionBuilder::MakeBinaryExpr(
    const NAst::TBinaryOpExpression* binaryExpr,
    EBinaryOp op,
    TUntypedExpression lhs,
    TUntypedExpression rhs,
    std::optional<size_t> offset) const
{
    TTypeSet genericAssignments;

    auto lhsSource = offset ? binaryExpr->Lhs[*offset]->GetSource(Source) : "";
    auto rhsSource = offset ? binaryExpr->Rhs[*offset]->GetSource(Source) : "";

    auto resultTypes = InferBinaryExprTypes(
        op,
        lhs.FeasibleTypes,
        rhs.FeasibleTypes,
        &genericAssignments,
        lhsSource,
        rhsSource);

    if (lhs.IsConstant && rhs.IsConstant) {
        auto lhsValue = lhs.Generator(lhs.FeasibleTypes.GetFront());
        auto rhsValue = rhs.Generator(rhs.FeasibleTypes.GetFront());
        if (auto foldedExpr = FoldConstants(op, lhsValue, rhsValue)) {
            TExpressionGenerator generator = [foldedExpr] (EValueType type) {
                return New<TLiteralExpression>(
                    type,
                    CastValueWithCheck(*foldedExpr, type));
            };
            return TUntypedExpression{resultTypes, std::move(generator), true};
        }
    }

    TExpressionGenerator generator = [
        op,
        lhs,
        rhs,
        genericAssignments,
        lhsSource,
        rhsSource,
        source = binaryExpr->GetSource(Source)
    ] (EValueType type) mutable {
        auto argTypes = RefineBinaryExprTypes(
            op,
            type,
            lhs.FeasibleTypes,
            rhs.FeasibleTypes,
            &genericAssignments,
            lhsSource,
            rhsSource,
            source);

        return New<TBinaryOpExpression>(
            type,
            op,
            lhs.Generator(argTypes.first),
            rhs.Generator(argTypes.second));
    };
    return TUntypedExpression{resultTypes, std::move(generator), false};
};

struct TGenerator
{
    const TTypedExpressionBuilder& Builder;
    const NAst::TBinaryOpExpression* BinaryExpr;
    ISchemaProxyPtr Schema;

    TUntypedExpression Do(size_t keySize, EBinaryOp op)
    {
        YT_VERIFY(keySize > 0);
        size_t offset = keySize - 1;

        auto untypedLhs = Builder.DoBuildUntypedExpression(
            BinaryExpr->Lhs[offset].Get(),
            Schema);
        auto untypedRhs = Builder.DoBuildUntypedExpression(
            BinaryExpr->Rhs[offset].Get(),
            Schema);

        auto result = Builder.MakeBinaryExpr(BinaryExpr, op, std::move(untypedLhs), std::move(untypedRhs), offset);

        while (offset > 0) {
            --offset;
            auto untypedLhs = Builder.DoBuildUntypedExpression(
                BinaryExpr->Lhs[offset].Get(),
                Schema);
            auto untypedRhs = Builder.DoBuildUntypedExpression(
                BinaryExpr->Rhs[offset].Get(),
                Schema);

            auto eq = Builder.MakeBinaryExpr(
                BinaryExpr,
                op == EBinaryOp::NotEqual ? EBinaryOp::Or : EBinaryOp::And,
                Builder.MakeBinaryExpr(
                    BinaryExpr,
                    op == EBinaryOp::NotEqual ? EBinaryOp::NotEqual : EBinaryOp::Equal,
                    untypedLhs,
                    untypedRhs,
                    offset),
                std::move(result),
                std::nullopt);

            if (op == EBinaryOp::Equal || op == EBinaryOp::NotEqual) {
                result = eq;
                continue;
            }

            EBinaryOp strongOp = op;
            if (op == EBinaryOp::LessOrEqual) {
                strongOp = EBinaryOp::Less;
            } else if (op == EBinaryOp::GreaterOrEqual)  {
                strongOp = EBinaryOp::Greater;
            }

            result = Builder.MakeBinaryExpr(
                BinaryExpr,
                EBinaryOp::Or,
                Builder.MakeBinaryExpr(
                    BinaryExpr,
                    strongOp,
                    std::move(untypedLhs),
                    std::move(untypedRhs),
                    offset),
                std::move(eq),
                std::nullopt);
        }

        return result;
    }
};

TUntypedExpression TTypedExpressionBuilder::DoBuildUntypedBinaryExpression(
    const NAst::TBinaryOpExpression* binaryExpr,
    ISchemaProxyPtr schema) const
{
    if (IsRelationalBinaryOp(binaryExpr->Opcode)) {
        if (binaryExpr->Lhs.size() != binaryExpr->Rhs.size()) {
            THROW_ERROR_EXCEPTION("Tuples of same size are expected but got %v vs %v",
                binaryExpr->Lhs.size(),
                binaryExpr->Rhs.size())
                << TErrorAttribute("source", binaryExpr->GetSource(Source));
        }

        int keySize = binaryExpr->Lhs.size();
        return TGenerator{*this, binaryExpr, schema}.Do(keySize, binaryExpr->Opcode);
    } else {
        if (binaryExpr->Lhs.size() != 1) {
            THROW_ERROR_EXCEPTION("Expecting scalar expression")
                << TErrorAttribute("source", FormatExpression(binaryExpr->Lhs));
        }

        if (binaryExpr->Rhs.size() != 1) {
            THROW_ERROR_EXCEPTION("Expecting scalar expression")
                << TErrorAttribute("source", FormatExpression(binaryExpr->Rhs));
        }

        auto untypedLhs = DoBuildUntypedExpression(
            binaryExpr->Lhs.front().Get(),
            schema);
        auto untypedRhs = DoBuildUntypedExpression(
            binaryExpr->Rhs.front().Get(),
            schema);

        return MakeBinaryExpr(binaryExpr, binaryExpr->Opcode, std::move(untypedLhs), std::move(untypedRhs), 0);
    }
}

void TTypedExpressionBuilder::InferArgumentTypes(
    std::vector<TConstExpressionPtr>* typedArguments,
    std::vector<EValueType>* argTypes,
    const NAst::TExpressionList& expressions,
    ISchemaProxyPtr schema,
    TStringBuf operatorName,
    TStringBuf source) const
{
    std::unordered_set<TString> columnNames;

    for (const auto& argument : expressions) {
        auto untypedArgument = DoBuildUntypedExpression(argument.Get(), schema);

        EValueType argType = GetFrontWithCheck(untypedArgument.FeasibleTypes, argument->GetSource(Source));
        auto typedArgument = untypedArgument.Generator(argType);

        typedArguments->push_back(typedArgument);
        argTypes->push_back(argType);
        if (auto reference = typedArgument->As<TReferenceExpression>()) {
            if (!columnNames.insert(reference->ColumnName).second) {
                THROW_ERROR_EXCEPTION("%v operator has multiple references to column %Qv",
                    operatorName,
                    reference->ColumnName)
                    << TErrorAttribute("source", source);
            }
        }
    }
}

TUntypedExpression TTypedExpressionBuilder::DoBuildUntypedInExpression(
    const NAst::TInExpression* inExpr,
    ISchemaProxyPtr schema) const
{
    std::vector<TConstExpressionPtr> typedArguments;
    std::vector<EValueType> argTypes;

    auto source = inExpr->GetSource(Source);

    InferArgumentTypes(
        &typedArguments,
        &argTypes,
        inExpr->Expr,
        schema,
        "IN",
        inExpr->GetSource(Source));

    auto capturedRows = LiteralTupleListToRows(inExpr->Values, argTypes, source);
    auto result = New<TInExpression>(std::move(typedArguments), std::move(capturedRows));

    TTypeSet resultTypes({EValueType::Boolean});
    TExpressionGenerator generator = [result] (EValueType type) mutable {
        return result;
    };
    return TUntypedExpression{resultTypes, std::move(generator), false};
}

TUntypedExpression TTypedExpressionBuilder::DoBuildUntypedBetweenExpression(
    const NAst::TBetweenExpression* betweenExpr,
    ISchemaProxyPtr schema) const
{
    std::vector<TConstExpressionPtr> typedArguments;
    std::vector<EValueType> argTypes;

    auto source = betweenExpr->GetSource(Source);

    InferArgumentTypes(
        &typedArguments,
        &argTypes,
        betweenExpr->Expr,
        schema,
        "BETWEEN",
        source);

    auto capturedRows = LiteralRangesListToRows(betweenExpr->Values, argTypes, source);
    auto result = New<TBetweenExpression>(std::move(typedArguments), std::move(capturedRows));

    TTypeSet resultTypes({EValueType::Boolean});
    TExpressionGenerator generator = [result] (EValueType type) mutable {
        return result;
    };
    return TUntypedExpression{resultTypes, std::move(generator), false};
}

TUntypedExpression TTypedExpressionBuilder::DoBuildUntypedTransformExpression(
    const NAst::TTransformExpression* transformExpr,
    ISchemaProxyPtr schema) const
{
    std::vector<TConstExpressionPtr> typedArguments;
    std::vector<EValueType> argTypes;

    auto source = transformExpr->GetSource(Source);

    InferArgumentTypes(
        &typedArguments,
        &argTypes,
        transformExpr->Expr,
        schema,
        "TRANSFORM",
        source);

    if (transformExpr->From.size() != transformExpr->To.size()) {
        THROW_ERROR_EXCEPTION("Size mismatch for source and result arrays in TRANSFORM operator")
            << TErrorAttribute("source", source);
    }

    TTypeSet resultTypes({
        EValueType::Null,
        EValueType::Int64,
        EValueType::Uint64,
        EValueType::Double,
        EValueType::Boolean,
        EValueType::String,
        EValueType::Any});

    for (const auto& tuple : transformExpr->To) {
        if (tuple.size() != 1) {
            THROW_ERROR_EXCEPTION("Expecting scalar expression")
                << TErrorAttribute("source", source);
        }

        auto valueTypes = GetTypes(tuple.front());

        if (!Unify(&resultTypes, valueTypes)) {
            THROW_ERROR_EXCEPTION("Types mismatch in tuple")
                << TErrorAttribute("source", source)
                << TErrorAttribute("actual_type", ToString(valueTypes))
                << TErrorAttribute("expected_type", ToString(resultTypes));
        }
    }

    const auto& defaultExpr = transformExpr->DefaultExpr;

    TConstExpressionPtr defaultTypedExpr;

    EValueType resultType;
    if (defaultExpr) {
        if (defaultExpr->size() != 1) {
            THROW_ERROR_EXCEPTION("Default expression must scalar")
                << TErrorAttribute("source", source);
        }

        auto untypedArgument = DoBuildUntypedExpression(defaultExpr->front().Get(), schema);

        if (!Unify(&resultTypes, untypedArgument.FeasibleTypes)) {
            THROW_ERROR_EXCEPTION("Type mismatch in default expression: expected %Qlv, got %Qlv",
                resultTypes,
                untypedArgument.FeasibleTypes)
                << TErrorAttribute("source", source);
        }

        resultType = GetFrontWithCheck(resultTypes, source);

        defaultTypedExpr = untypedArgument.Generator(resultType);
    } else {
        resultType = GetFrontWithCheck(resultTypes, source);
    }

    auto rowBuffer = New<TRowBuffer>(TQueryPreparerBufferTag());
    TUnversionedRowBuilder rowBuilder;
    std::vector<TRow> rows;

    for (size_t index = 0; index < transformExpr->From.size(); ++index) {
        const auto& sourceTuple = transformExpr->From[index];
        if (sourceTuple.size() != argTypes.size()) {
            THROW_ERROR_EXCEPTION("Arguments size mismatch in tuple")
                << TErrorAttribute("source", source);
        }
        for (int i = 0; i < sourceTuple.size(); ++i) {
            auto valueType = GetType(sourceTuple[i]);
            auto value = GetValue(sourceTuple[i]);

            if (valueType == EValueType::Null) {
                value = MakeUnversionedSentinelValue(EValueType::Null);
            } else if (valueType != argTypes[i]) {
                if (IsArithmeticType(valueType) && IsArithmeticType(argTypes[i])) {
                    value = CastValueWithCheck(value, argTypes[i]);
                } else {
                    THROW_ERROR_EXCEPTION("Types mismatch in tuple")
                    << TErrorAttribute("source", source)
                    << TErrorAttribute("actual_type", valueType)
                    << TErrorAttribute("expected_type", argTypes[i]);
                }
            }
            rowBuilder.AddValue(value);
        }

        const auto& resultTuple = transformExpr->To[index];

        YT_VERIFY(resultTuple.size() == 1);
        auto value = CastValueWithCheck(GetValue(resultTuple.front()), resultType);
        rowBuilder.AddValue(value);

        rows.push_back(rowBuffer->Capture(rowBuilder.GetRow()));
        rowBuilder.Reset();
    }

    std::sort(rows.begin(), rows.end(), [argCount = argTypes.size()] (TRow lhs, TRow rhs) {
        return CompareRows(lhs, rhs, argCount) < 0;
    });

    auto capturedRows =  MakeSharedRange(std::move(rows), std::move(rowBuffer));
    auto result = New<TTransformExpression>(
        resultType,
        std::move(typedArguments),
        std::move(capturedRows),
        std::move(defaultTypedExpr));

    TExpressionGenerator generator = [result] (EValueType type) mutable {
        return result;
    };
    return TUntypedExpression{TTypeSet({resultType}), std::move(generator), false};
}

DECLARE_REFCOUNTED_CLASS(TSchemaProxy)

class TSchemaProxy
    : public ISchemaProxy
{
public:
    TSchemaProxy() = default;

    virtual std::optional<TBaseColumn> GetColumnPtr(
        const NAst::TReference& reference) override
    {
        auto found = Lookup_.find(reference);
        if (found != Lookup_.end()) {
            return found->second;
        } else if (auto column = ProvideColumn(reference)) {
            YT_VERIFY(Lookup_.emplace(reference, *column).second);
            return column;
        } else {
            return std::nullopt;
        }
    }

    virtual TUntypedExpression GetAggregateColumnPtr(
        const TString& columnName,
        const TAggregateTypeInferrer* aggregateFunction,
        const NAst::TExpression* arguments,
        const TString& subexprName,
        const TTypedExpressionBuilder& builder) override
    {
        auto typer = ProvideAggregateColumn(
            columnName,
            aggregateFunction,
            arguments,
            subexprName,
            builder);

        TExpressionGenerator generator = [=] (EValueType type) {
            auto found = AggregateLookup_.find(std::make_pair(subexprName, type));
            if (found != AggregateLookup_.end()) {
                TBaseColumn columnInfo = found->second;
                return New<TReferenceExpression>(columnInfo.Type, columnInfo.Name);
            } else {
                TBaseColumn columnInfo = typer.second(type);
                YT_VERIFY(AggregateLookup_.emplace(std::make_pair(subexprName, type), columnInfo).second);
                return New<TReferenceExpression>(columnInfo.Type, columnInfo.Name);
            }
        };

        return TUntypedExpression{typer.first, std::move(generator), false};
    }

    virtual void Finish()
    { }

    const THashMap<NAst::TReference, TBaseColumn>& GetLookup() const
    {
        return Lookup_;
    }

private:
    THashMap<NAst::TReference, TBaseColumn> Lookup_;
    THashMap<std::pair<TString, EValueType>, TBaseColumn> AggregateLookup_;

protected:
    virtual std::optional<TBaseColumn> ProvideColumn(const NAst::TReference& /*reference*/)
    {
        return std::nullopt;
    }

    virtual std::pair<TTypeSet, std::function<TBaseColumn(EValueType)>> ProvideAggregateColumn(
        const TString& name,
        const TAggregateTypeInferrer* /*aggregateFunction*/,
        const NAst::TExpression* /*arguments*/,
        const TString& /*subexprName*/,
        const TTypedExpressionBuilder& /*builder*/)
    {
        THROW_ERROR_EXCEPTION("Misuse of aggregate function %Qv",
            name);
    }

};

DEFINE_REFCOUNTED_TYPE(TSchemaProxy)

class TScanSchemaProxy
    : public TSchemaProxy
{
public:
    TScanSchemaProxy(
        const TTableSchema& sourceTableSchema,
        const std::optional<TString>& tableName,
        std::vector<TColumnDescriptor>* mapping = nullptr)
        : Mapping_(mapping)
        , SourceTableSchema_(sourceTableSchema)
        , TableName_(tableName)
    { }

    virtual std::optional<TBaseColumn> ProvideColumn(const NAst::TReference& reference) override
    {
        if (reference.TableName != TableName_) {
            return std::nullopt;
        }

        auto column = SourceTableSchema_.FindColumn(reference.ColumnName);

        if (column) {
            auto formattedName = NAst::InferColumnName(reference);
            if (size_t collisionIndex = ColumnsCollisions_.emplace(reference.ColumnName, 0).first->second++) {
                formattedName = Format("%v#%v", formattedName, collisionIndex);
            }

            if (Mapping_) {
                Mapping_->push_back(TColumnDescriptor{
                    formattedName,
                    SourceTableSchema_.GetColumnIndex(*column)
                });
            }

            return TBaseColumn(formattedName, column->GetPhysicalType());
        } else {
            return std::nullopt;
        }
    }

    virtual void Finish() override
    {
        for (const auto& column : SourceTableSchema_.Columns()) {
            GetColumnPtr(NAst::TReference(column.Name(), TableName_));
        }
    }

private:
    std::vector<TColumnDescriptor>* Mapping_;
    THashMap<TString, size_t> ColumnsCollisions_;
    const TTableSchema SourceTableSchema_;
    const std::optional<TString> TableName_;

    DECLARE_NEW_FRIEND();
};

class TJoinSchemaProxy
    : public TSchemaProxy
{
public:
    TJoinSchemaProxy(
        std::vector<TString>* selfJoinedColumns,
        std::vector<TString>* foreignJoinedColumns,
        const THashSet<NAst::TReference>& sharedColumns,
        TSchemaProxyPtr self,
        TSchemaProxyPtr foreign)
        : SharedColumns_(sharedColumns)
        , Self_(self)
        , Foreign_(foreign)
        , SelfJoinedColumns_(selfJoinedColumns)
        , ForeignJoinedColumns_(foreignJoinedColumns)
    { }

    virtual std::optional<TBaseColumn> ProvideColumn(const NAst::TReference& reference) override
    {
        if (auto column = Self_->GetColumnPtr(reference)) {
            if (SharedColumns_.find(reference) == SharedColumns_.end() &&
                Foreign_->GetColumnPtr(reference))
            {
                THROW_ERROR_EXCEPTION("Column %Qv occurs both in main and joined tables",
                    NAst::InferColumnName(reference));
            }
            SelfJoinedColumns_->push_back(column->Name);
            return column;
        } else if (auto column = Foreign_->GetColumnPtr(reference)) {
            ForeignJoinedColumns_->push_back(column->Name);
            return column;
        } else {
            return std::nullopt;
        }
    }

    virtual void Finish() override
    {
        Self_->Finish();
        Foreign_->Finish();

        for (const auto& column : Self_->GetLookup()) {
            GetColumnPtr(column.first);
        }

        for (const auto& column : Foreign_->GetLookup()) {
            GetColumnPtr(column.first);
        }
    }

private:
    const THashSet<NAst::TReference> SharedColumns_;
    const TSchemaProxyPtr Self_;
    const TSchemaProxyPtr Foreign_;

    std::vector<TString>* const SelfJoinedColumns_;
    std::vector<TString>* const ForeignJoinedColumns_;

};

const std::optional<TBaseColumn> FindColumn(const TNamedItemList& schema, const TString& name)
{
    for (size_t index = 0; index < schema.size(); ++index) {
        if (schema[index].Name == name) {
            return TBaseColumn(name, schema[index].Expression->Type);
        }
    }
    return std::nullopt;
}

class TGroupSchemaProxy
    : public TSchemaProxy
{
public:
    TGroupSchemaProxy(
        const TNamedItemList* groupItems,
        TSchemaProxyPtr base,
        TAggregateItemList* aggregateItems)
        : GroupItems_(groupItems)
        , Base_(base)
        , AggregateItems_(aggregateItems)
    { }

    virtual std::optional<TBaseColumn> ProvideColumn(const NAst::TReference& reference) override
    {
        if (reference.TableName) {
            return std::nullopt;
        }

        return FindColumn(*GroupItems_, reference.ColumnName);
    }

    virtual std::pair<TTypeSet, std::function<TBaseColumn(EValueType)>> ProvideAggregateColumn(
        const TString& name,
        const TAggregateTypeInferrer* aggregateFunction,
        const NAst::TExpression* argument,
        const TString& subexprName,
        const TTypedExpressionBuilder& builder) override
    {
        YT_VERIFY(builder.AfterGroupBy);

        builder.AfterGroupBy = false;
        auto untypedOperand = builder.BuildUntypedExpression(
            argument,
            Base_);
        builder.AfterGroupBy = true;

        TTypeSet constraint;
        std::optional<EValueType> stateType;
        std::optional<EValueType> resultType;

        aggregateFunction->GetNormalizedConstraints(&constraint, &stateType, &resultType, name);

        TTypeSet resultTypes;
        TTypeSet genericAssignments = constraint;

        if (!Unify(&genericAssignments, untypedOperand.FeasibleTypes)) {
            THROW_ERROR_EXCEPTION("Type mismatch in function %Qv: expected %Qv, got %Qv",
                name,
                genericAssignments,
                untypedOperand.FeasibleTypes)
                << TErrorAttribute("source", subexprName);
        }

        if (resultType) {
            resultTypes = TTypeSet({*resultType});
        } else {
            resultTypes = genericAssignments;
        }

        return std::make_pair(resultTypes, [=] (EValueType type) {
            EValueType argType;
            if (resultType) {
                YT_VERIFY(!genericAssignments.IsEmpty());
                argType = GetFrontWithCheck(genericAssignments, argument->GetSource(builder.Source));
            } else {
                argType = type;
            }

            EValueType effectiveStateType;
            if (stateType) {
                effectiveStateType = *stateType;
            } else {
                effectiveStateType = argType;
            }

            auto typedOperand = untypedOperand.Generator(argType);

            typedOperand = TCastEliminator().Visit(typedOperand);
            typedOperand = TExpressionSimplifier().Visit(typedOperand);
            typedOperand = TNotExpressionPropagator().Visit(typedOperand);

            AggregateItems_->emplace_back(
                typedOperand,
                name,
                subexprName,
                effectiveStateType,
                type);

            return TBaseColumn(subexprName, type);
        });
    }

private:
    const TNamedItemList* GroupItems_;
    TSchemaProxyPtr Base_;
    TAggregateItemList* AggregateItems_;

};

TConstExpressionPtr BuildPredicate(
    const NAst::TExpressionList& expressionAst,
    const TSchemaProxyPtr& schemaProxy,
    const TTypedExpressionBuilder& builder,
    TStringBuf name)
{
    if (expressionAst.size() != 1) {
        THROW_ERROR_EXCEPTION("Expecting scalar expression")
            << TErrorAttribute("source", FormatExpression(expressionAst));
    }

    auto typedPredicate = builder.BuildTypedExpression(expressionAst.front().Get(), schemaProxy);

    auto actualType = typedPredicate->Type;
    EValueType expectedType(EValueType::Boolean);
    if (actualType != expectedType) {
        THROW_ERROR_EXCEPTION("%v is not a boolean expression", name)
            << TErrorAttribute("source", expressionAst.front().Get()->GetSource(builder.Source))
            << TErrorAttribute("actual_type", actualType)
            << TErrorAttribute("expected_type", expectedType);
    }

    return typedPredicate;
}

TConstGroupClausePtr BuildGroupClause(
    const NAst::TExpressionList& expressionsAst,
    ETotalsMode totalsMode,
    TSchemaProxyPtr& schemaProxy,
    const TTypedExpressionBuilder& builder)
{
    auto groupClause = New<TGroupClause>();
    groupClause->TotalsMode = totalsMode;

    TTypeSet groupItemTypes({
        EValueType::Boolean,
        EValueType::Int64,
        EValueType::Uint64,
        EValueType::Double,
        EValueType::String});

    for (const auto& expressionAst : expressionsAst) {
        auto typedExpr = builder.BuildTypedExpression(expressionAst.Get(), schemaProxy, groupItemTypes);

        groupClause->AddGroupItem(typedExpr, InferColumnName(*expressionAst));
    }

    schemaProxy = New<TGroupSchemaProxy>(
        &groupClause->GroupItems,
        std::move(schemaProxy),
        &groupClause->AggregateItems);

    return groupClause;
}

TConstProjectClausePtr BuildProjectClause(
    const NAst::TExpressionList& expressionsAst,
    TSchemaProxyPtr& schemaProxy,
    const TTypedExpressionBuilder& builder)
{
    auto projectClause = New<TProjectClause>();
    for (const auto& expressionAst : expressionsAst) {
        auto typedExpr = builder.BuildTypedExpression(expressionAst.Get(), schemaProxy);

        projectClause->AddProjection(typedExpr, InferColumnName(*expressionAst));
    }

    schemaProxy = New<TScanSchemaProxy>(projectClause->GetTableSchema(), std::nullopt);

    return projectClause;
}

void PrepareQuery(
    const TQueryPtr& query,
    const NAst::TQuery& ast,
    TSchemaProxyPtr& schemaProxy,
    const TTypedExpressionBuilder& builder)
{
    if (ast.WherePredicate) {
        query->WhereClause = BuildPredicate(
            *ast.WherePredicate,
            schemaProxy,
            builder,
            "WHERE-clause");
    }

    if (ast.GroupExprs) {
        query->GroupClause = BuildGroupClause(
            ast.GroupExprs->first,
            ast.GroupExprs->second,
            schemaProxy,
            builder);
        builder.AfterGroupBy = true;
    }

    if (ast.HavingPredicate) {
        if (!query->GroupClause) {
            THROW_ERROR_EXCEPTION("Expected GROUP BY before HAVING");
        }
        query->HavingClause = BuildPredicate(
            *ast.HavingPredicate,
            schemaProxy,
            builder,
            "HAVING-clause");
    }

    if (!ast.OrderExpressions.empty()) {
        auto orderClause = New<TOrderClause>();

        for (const auto& orderExpr : ast.OrderExpressions) {
            for (const auto& expressionAst : orderExpr.first) {
                auto typedExpr = builder.BuildTypedExpression(expressionAst.Get(), schemaProxy);

                orderClause->OrderItems.emplace_back(typedExpr, orderExpr.second);
            }
        }

        size_t keyPrefix = 0;
        while (keyPrefix < orderClause->OrderItems.size()) {
            const auto& item = orderClause->OrderItems[keyPrefix];

            if (item.second) {
                break;
            }

            const auto* referenceExpr = item.first->As<TReferenceExpression>();

            if (!referenceExpr) {
                break;
            }

            auto columnIndex = ColumnNameToKeyPartIndex(query->GetKeyColumns(), referenceExpr->ColumnName);

            if (keyPrefix != columnIndex) {
                break;
            }
            ++keyPrefix;
        }

        if (keyPrefix < orderClause->OrderItems.size()) {
            query->OrderClause = std::move(orderClause);

        }

        // Use ordered scan otherwise
    }

    if (ast.SelectExprs) {
        query->ProjectClause = BuildProjectClause(
            *ast.SelectExprs,
            schemaProxy,
            builder);
    }

    schemaProxy->Finish();
}

void ParseQueryString(
    NAst::TAstHead* astHead,
    const TString& source,
    NAst::TParser::token::yytokentype strayToken)
{
    NAst::TLexer lexer(source, strayToken);
    NAst::TParser parser(lexer, astHead, source);

    int result = parser.parse();

    if (result != 0) {
        THROW_ERROR_EXCEPTION("Parse failure")
            << TErrorAttribute("source", source);
    }
}

NAst::TParser::token::yytokentype GetStrayToken(EParseMode mode)
{
    switch (mode) {
        case EParseMode::Query:      return NAst::TParser::token::StrayWillParseQuery;
        case EParseMode::JobQuery:   return NAst::TParser::token::StrayWillParseJobQuery;
        case EParseMode::Expression: return NAst::TParser::token::StrayWillParseExpression;
        default:                     YT_ABORT();
    }
}

NAst::TAstHead MakeAstHead(EParseMode mode)
{
    switch (mode) {
        case EParseMode::Query:
        case EParseMode::JobQuery:   return NAst::TAstHead::MakeQuery();
        case EParseMode::Expression: return NAst::TAstHead::MakeExpression();
        default:                     YT_ABORT();
    }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

void DefaultFetchFunctions(const std::vector<TString>& names, const TTypeInferrerMapPtr& typeInferrers)
{
    MergeFrom(typeInferrers.Get(), *BuiltinTypeInferrersMap);
}

////////////////////////////////////////////////////////////////////////////////

TParsedSource::TParsedSource(const TString& source, const NAst::TAstHead& astHead)
    : Source(source)
    , AstHead(astHead)
{ }

std::unique_ptr<TParsedSource> ParseSource(const TString& source, EParseMode mode)
{
    auto parsedSource = std::make_unique<TParsedSource>(
        source,
        MakeAstHead(mode));
    ParseQueryString(
        &parsedSource->AstHead,
        source,
        GetStrayToken(mode));
    return parsedSource;
}

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<TPlanFragment> PreparePlanFragment(
    IPrepareCallbacks* callbacks,
    const TString& source,
    const TFunctionsFetcher& functionsFetcher,
    TTimestamp timestamp)
{
    return PreparePlanFragment(
        callbacks,
        *ParseSource(source, EParseMode::Query),
        functionsFetcher,
        timestamp);
}

std::unique_ptr<TPlanFragment> PreparePlanFragment(
    IPrepareCallbacks* callbacks,
    const TParsedSource& parsedSource,
    const TFunctionsFetcher& functionsFetcher,
    TTimestamp timestamp)
{
    auto query = New<TQuery>(TGuid::Create());

    auto Logger = MakeQueryLogger(query);

    const auto& ast = std::get<NAst::TQuery>(parsedSource.AstHead.Ast);
    const auto& aliasMap = parsedSource.AstHead.AliasMap;

    auto functionNames = ExtractFunctionNames(ast, aliasMap);

    auto functions = New<TTypeInferrerMap>();
    functionsFetcher(functionNames, functions);

    const auto& table = ast.Table;

    YT_LOG_DEBUG("Getting initial data splits (PrimaryPath: %v, ForeignPaths: %v)",
        table.Path,
        MakeFormattableView(ast.Joins, [] (TStringBuilderBase* builder, const auto& join) {
            FormatValue(builder, join.Table.Path, TStringBuf());
        }));

    std::vector<TFuture<TDataSplit>> asyncDataSplits;
    asyncDataSplits.push_back(callbacks->GetInitialSplit(table.Path, timestamp));
    for (const auto& join : ast.Joins) {
        asyncDataSplits.push_back(callbacks->GetInitialSplit(join.Table.Path, timestamp));
    }

    auto dataSplits = WaitFor(Combine(asyncDataSplits))
        .ValueOrThrow();

    YT_LOG_DEBUG("Initial data splits received");

    const auto& selfDataSplit = dataSplits[0];

    auto tableSchema = GetTableSchemaFromDataSplit(selfDataSplit);
    query->OriginalSchema = tableSchema;

    TSchemaProxyPtr schemaProxy = New<TScanSchemaProxy>(
        tableSchema,
        table.Alias,
        &query->SchemaMapping);

    TTypedExpressionBuilder builder{
        parsedSource.Source,
        functions,
        aliasMap,
        std::set<TString>(),
        false,
        0};

    size_t commonKeyPrefix = std::numeric_limits<size_t>::max();

    std::vector<TJoinClausePtr> joinClauses;
    for (size_t joinIndex = 0; joinIndex < ast.Joins.size(); ++joinIndex) {
        const auto& join = ast.Joins[joinIndex];
        const auto& foreignDataSplit = dataSplits[joinIndex + 1];

        auto foreignTableSchema = GetTableSchemaFromDataSplit(foreignDataSplit);
        auto foreignKeyColumnsCount = foreignTableSchema.GetKeyColumns().size();

        auto joinClause = New<TJoinClause>();
        joinClause->OriginalSchema = foreignTableSchema;
        joinClause->ForeignDataId = GetObjectIdFromDataSplit(foreignDataSplit);
        joinClause->IsLeft = join.IsLeft;

        auto foreignSourceProxy = New<TScanSchemaProxy>(
            foreignTableSchema,
            join.Table.Alias,
            &joinClause->SchemaMapping);

        std::vector<std::pair<TConstExpressionPtr, bool>> selfEquations;
        std::vector<TConstExpressionPtr> foreignEquations;
        THashSet<NAst::TReference> sharedColumns;
        // Merge columns.
        for (const auto& referenceExpr : join.Fields) {
            auto selfColumn = schemaProxy->GetColumnPtr(referenceExpr->Reference);
            auto foreignColumn = foreignSourceProxy->GetColumnPtr(referenceExpr->Reference);

            if (!selfColumn || !foreignColumn) {
                THROW_ERROR_EXCEPTION("Column %Qv not found",
                    NAst::InferColumnName(referenceExpr->Reference));
            }

            if (selfColumn->Type != foreignColumn->Type) {
                THROW_ERROR_EXCEPTION("Column %Qv type mismatch in join",
                    NAst::InferColumnName(referenceExpr->Reference))
                    << TErrorAttribute("self_type", selfColumn->Type)
                    << TErrorAttribute("foreign_type", foreignColumn->Type);
            }

            selfEquations.emplace_back(New<TReferenceExpression>(selfColumn->Type, selfColumn->Name), false);
            foreignEquations.push_back(New<TReferenceExpression>(foreignColumn->Type, foreignColumn->Name));

            // Add to mapping.
            sharedColumns.emplace(referenceExpr->Reference.ColumnName, referenceExpr->Reference.TableName);
        }

        for (const auto& argument : join.Lhs) {
            selfEquations.emplace_back(builder.BuildTypedExpression(argument.Get(), schemaProxy), false);
        }

        for (const auto& argument : join.Rhs) {
            foreignEquations.push_back(builder.BuildTypedExpression(argument.Get(), foreignSourceProxy));
        }

        if (selfEquations.size() != foreignEquations.size()) {
            THROW_ERROR_EXCEPTION("Tuples of same size are expected but got %v vs %v",
                selfEquations.size(),
                foreignEquations.size())
                << TErrorAttribute("lhs_source", FormatExpression(join.Lhs))
                << TErrorAttribute("rhs_source", FormatExpression(join.Rhs));
        }

        for (size_t index = 0; index < selfEquations.size(); ++index) {
            if (selfEquations[index].first->Type != foreignEquations[index]->Type) {
                THROW_ERROR_EXCEPTION("Types mismatch in join equation \"%v = %v\"",
                    InferName(selfEquations[index].first),
                    InferName(foreignEquations[index]))
                    << TErrorAttribute("self_type", selfEquations[index].first->Type)
                    << TErrorAttribute("foreign_type", foreignEquations[index]->Type);
            }
        }

        // If can use ranges, rearrange equations according to key columns and enrich with evaluated columns

        std::vector<std::pair<TConstExpressionPtr, bool>> keySelfEquations(foreignKeyColumnsCount);
        std::vector<TConstExpressionPtr> keyForeignEquations(foreignKeyColumnsCount);

        for (size_t equationIndex = 0; equationIndex < foreignEquations.size(); ++equationIndex) {
            const auto& expr = foreignEquations[equationIndex];

            if (const auto* referenceExpr = expr->As<TReferenceExpression>()) {
                auto index = ColumnNameToKeyPartIndex(joinClause->GetKeyColumns(), referenceExpr->ColumnName);

                if (index >= 0) {
                    keySelfEquations[index] = selfEquations[equationIndex];
                    keyForeignEquations[index] = foreignEquations[equationIndex];
                    continue;
                }
            }

            keySelfEquations.push_back(selfEquations[equationIndex]);
            keyForeignEquations.push_back(foreignEquations[equationIndex]);
        }

        size_t keyPrefix = 0;
        for (; keyPrefix < foreignKeyColumnsCount; ++keyPrefix) {
            if (keyForeignEquations[keyPrefix]) {
                YT_VERIFY(keySelfEquations[keyPrefix].first);

                if (const auto* referenceExpr = keySelfEquations[keyPrefix].first->As<TReferenceExpression>()) {
                    if (ColumnNameToKeyPartIndex(query->GetKeyColumns(), referenceExpr->ColumnName) != keyPrefix) {
                        commonKeyPrefix = std::min(commonKeyPrefix, keyPrefix);
                    }
                } else {
                    commonKeyPrefix = std::min(commonKeyPrefix, keyPrefix);
                }

                continue;
            }

            const auto& foreignColumnExpression = foreignTableSchema.Columns()[keyPrefix].Expression();

            if (!foreignColumnExpression) {
                break;
            }

            THashSet<TString> references;
            auto evaluatedColumnExpression = PrepareExpression(
                *foreignColumnExpression,
                foreignTableSchema,
                functions,
                &references);

            auto canEvaluate = true;
            for (const auto& reference : references) {
                int referenceIndex = foreignTableSchema.GetColumnIndexOrThrow(reference);
                if (!keySelfEquations[referenceIndex].first) {
                    YT_VERIFY(!keyForeignEquations[referenceIndex]);
                    canEvaluate = false;
                }
            }

            if (!canEvaluate) {
                break;
            }

            keySelfEquations[keyPrefix] = std::make_pair(evaluatedColumnExpression, true);

            auto reference = NAst::TReference(
                foreignTableSchema.Columns()[keyPrefix].Name(),
                join.Table.Alias);

            auto foreignColumn = foreignSourceProxy->GetColumnPtr(reference);

            keyForeignEquations[keyPrefix] = New<TReferenceExpression>(
                foreignColumn->Type,
                foreignColumn->Name);
        }

        commonKeyPrefix = std::min(commonKeyPrefix, keyPrefix);

        for (size_t index = 0; index < keyPrefix; ++index) {
            if (keySelfEquations[index].second) {
                const auto& evaluatedColumnExpression = keySelfEquations[index].first;

                if (const auto& selfColumnExpression = tableSchema.Columns()[index].Expression()) {
                    auto evaluatedSelfColumnExpression = PrepareExpression(
                        *selfColumnExpression,
                        tableSchema,
                        functions);

                    if (!Compare(
                        evaluatedColumnExpression,
                        foreignTableSchema,
                        evaluatedSelfColumnExpression,
                        tableSchema,
                        commonKeyPrefix))
                    {
                        commonKeyPrefix = std::min(commonKeyPrefix, index);
                    }
                } else {
                    commonKeyPrefix = std::min(commonKeyPrefix, index);
                }
            }
        }

        YT_VERIFY(keyForeignEquations.size() == keySelfEquations.size());

        size_t lastEmptyIndex = keyPrefix;
        for (size_t index = keyPrefix; index < keyForeignEquations.size(); ++index) {
            if (keyForeignEquations[index]) {
                YT_VERIFY(keySelfEquations[index].first);
                keyForeignEquations[lastEmptyIndex] = std::move(keyForeignEquations[index]);
                keySelfEquations[lastEmptyIndex] = std::move(keySelfEquations[index]);
                ++lastEmptyIndex;
            }
        }

        keyForeignEquations.resize(lastEmptyIndex);
        keySelfEquations.resize(lastEmptyIndex);

        joinClause->SelfEquations = std::move(keySelfEquations);
        joinClause->ForeignEquations = std::move(keyForeignEquations);
        joinClause->ForeignKeyPrefix = keyPrefix;
        joinClause->CommonKeyPrefix = commonKeyPrefix;

        YT_LOG_DEBUG("Creating join (CommonKeyPrefix: %v, ForeignKeyPrefix: %v)",
            commonKeyPrefix,
            keyPrefix);

        if (join.Predicate) {
            joinClause->Predicate = BuildPredicate(
                *join.Predicate,
                foreignSourceProxy,
                builder,
                "JOIN-PREDICATE-clause");
        }

        schemaProxy = New<TJoinSchemaProxy>(
            &joinClause->SelfJoinedColumns,
            &joinClause->ForeignJoinedColumns,
            sharedColumns,
            schemaProxy,
            foreignSourceProxy);

        joinClauses.push_back(std::move(joinClause));
    }

    PrepareQuery(query, ast, schemaProxy, builder);

    query->JoinClauses.assign(joinClauses.begin(), joinClauses.end());

    if (auto groupClause = query->GroupClause) {
        auto keyColumns = query->GetKeyColumns();

        std::vector<bool> touchedKeyColumns(keyColumns.size(), false);
        for (const auto& item : groupClause->GroupItems) {
            if (auto referenceExpr = item.Expression->As<TReferenceExpression>()) {
                int keyPartIndex = ColumnNameToKeyPartIndex(keyColumns, referenceExpr->ColumnName);
                if (keyPartIndex >= 0) {
                    touchedKeyColumns[keyPartIndex] = true;
                }
            }
        }

        size_t keyPrefix = 0;
        for (; keyPrefix < touchedKeyColumns.size(); ++keyPrefix) {
            if (touchedKeyColumns[keyPrefix]) {
                continue;
            }

            const auto& expression = query->OriginalSchema.Columns()[keyPrefix].Expression();

            if (!expression) {
                break;
            }

            THashSet<TString> references;
            auto evaluatedColumnExpression = PrepareExpression(
                *expression,
                query->OriginalSchema,
                functions,
                &references);

            auto canEvaluate = true;
            for (const auto& reference : references) {
                int referenceIndex = query->OriginalSchema.GetColumnIndexOrThrow(reference);
                if (!touchedKeyColumns[referenceIndex]) {
                    canEvaluate = false;
                }
            }

            if (!canEvaluate) {
                break;
            }
        }

        bool containsPrimaryKey = keyPrefix == keyColumns.size();
        // not prefix, because of equal prefixes near borders

        query->UseDisjointGroupBy = containsPrimaryKey;

        YT_LOG_DEBUG("Group key contains primary key, can omit top-level GROUP BY");
    }


    if (ast.Limit) {
        query->Limit = *ast.Limit;
    } else if (query->OrderClause) {
        THROW_ERROR_EXCEPTION("ORDER BY used without LIMIT");
    }

    if (ast.Offset) {
        if (!query->OrderClause) {
            THROW_ERROR_EXCEPTION("OFFSET used without ORDER BY");
        }
        query->Offset = *ast.Offset;
    }

    auto queryFingerprint = InferName(query, true);
    YT_LOG_DEBUG("Prepared query (Fingerprint: %v, ReadSchema: %v, ResultSchema: %v)",
        queryFingerprint,
        query->GetReadSchema(),
        query->GetTableSchema());

    auto range = GetBothBoundsFromDataSplit(selfDataSplit);

    SmallVector<TRowRange, 1> rowRanges;
    auto buffer = New<TRowBuffer>(TQueryPreparerBufferTag());
    rowRanges.push_back({
        buffer->Capture(range.first.Get()),
        buffer->Capture(range.second.Get())});

    auto fragment = std::make_unique<TPlanFragment>();
    fragment->Query = query;
    fragment->Ranges.Id = GetObjectIdFromDataSplit(selfDataSplit);
    fragment->Ranges.Ranges = MakeSharedRange(std::move(rowRanges), std::move(buffer));
    return fragment;
}

TQueryPtr PrepareJobQuery(
    const TString& source,
    const TTableSchema& tableSchema,
    const TFunctionsFetcher& functionsFetcher)
{
    auto astHead = NAst::TAstHead::MakeQuery();
    ParseQueryString(
        &astHead,
        source,
        NAst::TParser::token::StrayWillParseJobQuery);

    const auto& ast = std::get<NAst::TQuery>(astHead.Ast);
    const auto& aliasMap = astHead.AliasMap;

    if (ast.Offset) {
        THROW_ERROR_EXCEPTION("OFFSET is not supported in map-reduce queries");
    }

    if (ast.Limit) {
        THROW_ERROR_EXCEPTION("LIMIT is not supported in map-reduce queries");
    }

    if (ast.GroupExprs) {
        THROW_ERROR_EXCEPTION("GROUP BY is not supported in map-reduce queries");
    }

    auto query = New<TQuery>(TGuid::Create());
    query->OriginalSchema = tableSchema;

    TSchemaProxyPtr schemaProxy = New<TScanSchemaProxy>(
        tableSchema,
        std::nullopt,
        &query->SchemaMapping);

    auto functionNames = ExtractFunctionNames(ast, aliasMap);

    auto functions = New<TTypeInferrerMap>();
    functionsFetcher(functionNames, functions);

    TTypedExpressionBuilder builder{
        source,
        functions,
        aliasMap,
        std::set<TString>(),
        false,
        0};

    PrepareQuery(
        query,
        ast,
        schemaProxy,
        builder);

    return query;
}

TConstExpressionPtr PrepareExpression(
    const TString& source,
    const TTableSchema& tableSchema,
    const TConstTypeInferrerMapPtr& functions,
    THashSet<TString>* references)
{
    return PrepareExpression(
        *ParseSource(source, EParseMode::Expression),
        tableSchema,
        functions,
        references);
}

TConstExpressionPtr PrepareExpression(
    const TParsedSource& parsedSource,
    const TTableSchema& tableSchema,
    const TConstTypeInferrerMapPtr& functions,
    THashSet<TString>* references)
{
    const auto& expr = std::get<NAst::TExpressionPtr>(parsedSource.AstHead.Ast);
    const auto& aliasMap = parsedSource.AstHead.AliasMap;

    std::vector<TColumnDescriptor> mapping;
    auto schemaProxy = New<TScanSchemaProxy>(
        tableSchema,
        std::nullopt,
        &mapping);

    TTypedExpressionBuilder builder{
        parsedSource.Source,
        functions,
        aliasMap,
        std::set<TString>(),
        false,
        0};

    auto result = builder.BuildTypedExpression(expr.Get(), schemaProxy);

    if (references) {
        for (const auto& item : mapping) {
            references->insert(item.Name);
        }
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient
