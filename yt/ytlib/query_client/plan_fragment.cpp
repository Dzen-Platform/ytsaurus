#include "plan_fragment.h"

#include <yt/ytlib/chunk_client/chunk_spec.pb.h>

#include <yt/ytlib/query_client/plan_fragment.pb.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/ytlib/tablet_client/wire_protocol.h>

#include <yt/core/ytree/serialize.h>

#include <limits>

namespace NYT {
namespace NQueryClient {

using namespace NTableClient;
using namespace NObjectClient;

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

Stroka InferName(TConstExpressionPtr expr, bool omitValues)
{
    bool newTuple = true;
    auto comma = [&] {
        bool isNewTuple = newTuple;
        newTuple = false;
        return Stroka(isNewTuple ? "" : ", ");
    };
    auto canOmitParenthesis = [] (TConstExpressionPtr expr) {
        return
            expr->As<TLiteralExpression>() ||
            expr->As<TReferenceExpression>() ||
            expr->As<TFunctionExpression>();
    };

    if (!expr) {
        return Stroka();
    } else if (auto literalExpr = expr->As<TLiteralExpression>()) {
        return omitValues
            ? ToString("?")
            : ToString(static_cast<TUnversionedValue>(literalExpr->Value));
    } else if (auto referenceExpr = expr->As<TReferenceExpression>()) {
        return referenceExpr->ColumnName;
    } else if (auto functionExpr = expr->As<TFunctionExpression>()) {
        auto str = functionExpr->FunctionName + "(";
        for (const auto& argument : functionExpr->Arguments) {
            str += comma() + InferName(argument, omitValues);
        }
        return str + ")";
    } else if (auto unaryOp = expr->As<TUnaryOpExpression>()) {
        auto rhsName = InferName(unaryOp->Operand, omitValues);
        if (!canOmitParenthesis(unaryOp->Operand)) {
            rhsName = "(" + rhsName + ")";
        }
        return Stroka() + GetUnaryOpcodeLexeme(unaryOp->Opcode) + " " + rhsName;
    } else if (auto binaryOp = expr->As<TBinaryOpExpression>()) {
        auto lhsName = InferName(binaryOp->Lhs, omitValues);
        if (!canOmitParenthesis(binaryOp->Lhs)) {
            lhsName = "(" + lhsName + ")";
        }
        auto rhsName = InferName(binaryOp->Rhs, omitValues);
        if (!canOmitParenthesis(binaryOp->Rhs)) {
            rhsName = "(" + rhsName + ")";
        }
        return
            lhsName +
            " " + GetBinaryOpcodeLexeme(binaryOp->Opcode) + " " +
            rhsName;
    } else if (auto inOp = expr->As<TInOpExpression>()) {
        Stroka str;
        for (const auto& argument : inOp->Arguments) {
            str += comma() + InferName(argument, omitValues);
        }
        if (inOp->Arguments.size() > 1) {
            str = "(" + str + ")";
        }
        str += " IN (";
        if (omitValues) {
            str += "??";
        } else {
            newTuple = true;
            for (const auto& row : inOp->Values) {
                str += comma() + ToString(row);
            }
        }
        return str + ")";
    } else {
        Y_UNREACHABLE();
    }
}

Stroka InferName(TConstQueryPtr query, bool omitValues)
{
    auto namedItemFormatter = [&] (TStringBuilder* builder, const TNamedItem& item) {
        builder->AppendFormat("%v AS %v",
            InferName(item.Expression, omitValues),
            item.Name);
    };

    auto orderItemFormatter = [&] (TStringBuilder* builder, const TOrderItem& item) {
        builder->AppendFormat("%v %v",
            InferName(item.first, omitValues),
            item.second ? "DESC" : "ASC");
    };

    std::vector<Stroka> clauses;
    Stroka str;

    if (query->ProjectClause) {
        str = JoinToString(query->ProjectClause->Projections, namedItemFormatter);
    } else {
        str = "*";
    }
    clauses.emplace_back("SELECT " + str);

    if (query->WhereClause) {
        str = InferName(query->WhereClause, omitValues);
        clauses.push_back(Stroka("WHERE ") + str);
    }
    if (query->GroupClause) {
        str = JoinToString(query->GroupClause->GroupItems, namedItemFormatter);
        clauses.push_back(Stroka("GROUP BY ") + str);
    }
    if (query->HavingClause) {
        str = InferName(query->HavingClause, omitValues);
        clauses.push_back(Stroka("HAVING ") + str);
    }
    if (query->OrderClause) {
        str = JoinToString(query->OrderClause->OrderItems, orderItemFormatter);
        clauses.push_back(Stroka("ORDER BY ") + str);
    }
    if (query->Limit < std::numeric_limits<i64>::max()) {
        str = ToString(query->Limit);
        clauses.push_back(Stroka("LIMIT ") + str);
    }

    return JoinToString(clauses, STRINGBUF(" "));
}

////////////////////////////////////////////////////////////////////////////////

void ThrowTypeMismatchError(
    EValueType lhsType,
    EValueType rhsType,
    const TStringBuf& source,
    const TStringBuf& lhsSource,
    const TStringBuf& rhsSource)
{
    THROW_ERROR_EXCEPTION(
            "Type mismatch in expression %Qv",
            source)
            << TErrorAttribute("lhs_source", lhsSource)
            << TErrorAttribute("rhs_source", rhsSource)
            << TErrorAttribute("lhs_type", lhsType)
            << TErrorAttribute("rhs_type", rhsType);
}

EValueType InferBinaryExprType(
    EBinaryOp opCode,
    EValueType lhsType,
    EValueType rhsType,
    const TStringBuf& source,
    const TStringBuf& lhsSource,
    const TStringBuf& rhsSource)
{
    if (lhsType != rhsType) {
        ThrowTypeMismatchError(lhsType, rhsType, source, lhsSource, rhsSource);
    }

    EValueType operandType = lhsType;

    switch (opCode) {
        case EBinaryOp::Plus:
        case EBinaryOp::Minus:
        case EBinaryOp::Multiply:
        case EBinaryOp::Divide:
            if (!IsArithmeticType(operandType)) {
                THROW_ERROR_EXCEPTION(
                    "Expression %Qv requires either integral or floating-point operands",
                    source)
                    << TErrorAttribute("lhs_source", lhsSource)
                    << TErrorAttribute("rhs_source", rhsSource)
                    << TErrorAttribute("operand_type", operandType);
            }
            return operandType;

        case EBinaryOp::Modulo:
        case EBinaryOp::LeftShift:
        case EBinaryOp::RightShift:
        case EBinaryOp::BitOr:
        case EBinaryOp::BitAnd:
            if (!IsIntegralType(operandType)) {
                THROW_ERROR_EXCEPTION(
                    "Expression %Qv requires integral operands",
                    source)
                    << TErrorAttribute("lhs_source", lhsSource)
                    << TErrorAttribute("rhs_source", rhsSource)
                    << TErrorAttribute("operand_type", operandType);
            }
            return operandType;

        case EBinaryOp::And:
        case EBinaryOp::Or:
            if (operandType != EValueType::Boolean) {
                THROW_ERROR_EXCEPTION(
                    "Expression %Qv requires boolean operands",
                    source)
                    << TErrorAttribute("lhs_source", lhsSource)
                    << TErrorAttribute("rhs_source", rhsSource)
                    << TErrorAttribute("operand_type", operandType);
            }
            return EValueType::Boolean;

        case EBinaryOp::Equal:
        case EBinaryOp::NotEqual:
        case EBinaryOp::Less:
        case EBinaryOp::Greater:
        case EBinaryOp::LessOrEqual:
        case EBinaryOp::GreaterOrEqual:
            if (!IsComparableType(operandType)) {
                THROW_ERROR_EXCEPTION(
                    "Expression %Qv requires either integral, floating-point or string operands",
                    source)
                    << TErrorAttribute("lhs_source", lhsSource)
                    << TErrorAttribute("rhs_source", rhsSource)
                    << TErrorAttribute("lhs_type", operandType);
            }
            return EValueType::Boolean;

        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TExpression* serialized, const TConstExpressionPtr& original)
{
    if (!original) {
        serialized->set_kind(static_cast<int>(EExpressionKind::None));
        return;
    }

    serialized->set_type(static_cast<int>(original->Type));

    if (auto literalExpr = original->As<TLiteralExpression>()) {
        serialized->set_kind(static_cast<int>(EExpressionKind::Literal));
        auto* proto = serialized->MutableExtension(NProto::TLiteralExpression::literal_expression);
        auto value = TValue(literalExpr->Value);
        auto data = value.Data;

        switch (value.Type) {
            case EValueType::Int64: {
                proto->set_int64_value(data.Int64);
                break;
            }

            case EValueType::Uint64: {
                proto->set_uint64_value(data.Uint64);
                break;
            }

            case EValueType::Double: {
                proto->set_double_value(data.Double);
                break;
            }

            case EValueType::String: {
                proto->set_string_value(data.String, value.Length);
                break;
            }

            case EValueType::Boolean: {
                proto->set_boolean_value(data.Boolean);
                break;
            }

            default:
                Y_UNREACHABLE();
        }

    } else if (auto referenceExpr = original->As<TReferenceExpression>()) {
        serialized->set_kind(static_cast<int>(EExpressionKind::Reference));
        auto* proto = serialized->MutableExtension(NProto::TReferenceExpression::reference_expression);
        proto->set_column_name(referenceExpr->ColumnName);
    } else if (auto functionExpr = original->As<TFunctionExpression>()) {
        serialized->set_kind(static_cast<int>(EExpressionKind::Function));
        auto* proto = serialized->MutableExtension(NProto::TFunctionExpression::function_expression);
        proto->set_function_name(functionExpr->FunctionName);
        ToProto(proto->mutable_arguments(), functionExpr->Arguments);
    } else if (auto unaryOpExpr = original->As<TUnaryOpExpression>()) {
        serialized->set_kind(static_cast<int>(EExpressionKind::UnaryOp));
        auto* proto = serialized->MutableExtension(NProto::TUnaryOpExpression::unary_op_expression);
        proto->set_opcode(static_cast<int>(unaryOpExpr->Opcode));
        ToProto(proto->mutable_operand(), unaryOpExpr->Operand);
    } else if (auto binaryOpExpr = original->As<TBinaryOpExpression>()) {
        serialized->set_kind(static_cast<int>(EExpressionKind::BinaryOp));
        auto* proto = serialized->MutableExtension(NProto::TBinaryOpExpression::binary_op_expression);
        proto->set_opcode(static_cast<int>(binaryOpExpr->Opcode));
        ToProto(proto->mutable_lhs(), binaryOpExpr->Lhs);
        ToProto(proto->mutable_rhs(), binaryOpExpr->Rhs);
    } else if (auto inOpExpr = original->As<TInOpExpression>()) {
        serialized->set_kind(static_cast<int>(EExpressionKind::InOp));
        auto* proto = serialized->MutableExtension(NProto::TInOpExpression::in_op_expression);
        ToProto(proto->mutable_arguments(), inOpExpr->Arguments);

        NTabletClient::TWireProtocolWriter writer;
        writer.WriteUnversionedRowset(inOpExpr->Values);
        ToProto(proto->mutable_values(), ToString(MergeRefs(writer.Flush())));
    }
}

void FromProto(TConstExpressionPtr* original, const NProto::TExpression& serialized)
{
    auto type = EValueType(serialized.type());
    auto kind = EExpressionKind(serialized.kind());
    switch (kind) {
        case EExpressionKind::None: {
            *original = nullptr;
            return;
        }

        case EExpressionKind::Literal: {
            auto result = New<TLiteralExpression>(type);
            const auto& ext = serialized.GetExtension(NProto::TLiteralExpression::literal_expression);

            switch (type) {
                case EValueType::Int64: {
                    result->Value = MakeUnversionedInt64Value(ext.int64_value());
                    break;
                }

                case EValueType::Uint64: {
                    result->Value = MakeUnversionedUint64Value(ext.uint64_value());
                    break;
                }

                case EValueType::Double: {
                    result->Value = MakeUnversionedDoubleValue(ext.double_value());
                    break;
                }

                case EValueType::String: {
                    result->Value = MakeUnversionedStringValue(ext.string_value());
                    break;
                }

                case EValueType::Boolean: {
                    result->Value = MakeUnversionedBooleanValue(ext.boolean_value());
                    break;
                }

                default:
                    Y_UNREACHABLE();
            }

            *original = result;
            return;
        }

        case EExpressionKind::Reference: {
            auto result = New<TReferenceExpression>(type);
            const auto& data = serialized.GetExtension(NProto::TReferenceExpression::reference_expression);
            result->ColumnName = data.column_name();
            *original = result;
            return;
        }

        case EExpressionKind::Function: {
            auto result = New<TFunctionExpression>(type);
            const auto& ext = serialized.GetExtension(NProto::TFunctionExpression::function_expression);
            result->FunctionName = ext.function_name();
            FromProto(&result->Arguments, ext.arguments());
            *original = result;
            return;
        }

        case EExpressionKind::UnaryOp: {
            auto result = New<TUnaryOpExpression>(type);
            const auto& ext = serialized.GetExtension(NProto::TUnaryOpExpression::unary_op_expression);
            result->Opcode = EUnaryOp(ext.opcode());
            FromProto(&result->Operand, ext.operand());
            *original = result;
            return;
        }

        case EExpressionKind::BinaryOp: {
            auto result = New<TBinaryOpExpression>(type);
            const auto& ext = serialized.GetExtension(NProto::TBinaryOpExpression::binary_op_expression);
            result->Opcode = EBinaryOp(ext.opcode());
            FromProto(&result->Lhs, ext.lhs());
            FromProto(&result->Rhs, ext.rhs());
            *original = result;
            return;
        }

        case EExpressionKind::InOp: {
            auto result = New<TInOpExpression>(type);
            const auto& ext = serialized.GetExtension(NProto::TInOpExpression::in_op_expression);
            FromProto(&result->Arguments, ext.arguments());

            NTabletClient::TWireProtocolReader reader(TSharedRef::FromString(ext.values()));
            result->Values = reader.ReadUnversionedRowset();

            *original = result;
            return;
        }

        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TNamedItem* serialized, const TNamedItem& original)
{
    ToProto(serialized->mutable_expression(), original.Expression);
    ToProto(serialized->mutable_name(), original.Name);
}

void FromProto(TNamedItem* original, const NProto::TNamedItem& serialized)
{
    *original = TNamedItem(
        FromProto<TConstExpressionPtr>(serialized.expression()),
        serialized.name());
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TAggregateItem* serialized, const TAggregateItem& original)
{
    ToProto(serialized->mutable_expression(), original.Expression);
    serialized->set_aggregate_function_name(original.AggregateFunction);
    serialized->set_state_type(static_cast<int>(original.StateType));
    serialized->set_result_type(static_cast<int>(original.ResultType));
    ToProto(serialized->mutable_name(), original.Name);
}

void FromProto(TAggregateItem* original, const NProto::TAggregateItem& serialized)
{
    *original = TAggregateItem(
        FromProto<TConstExpressionPtr>(serialized.expression()),
        serialized.aggregate_function_name(),
        serialized.name(),
        EValueType(serialized.state_type()),
        EValueType(serialized.result_type()));
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TSelfEquation* proto, const std::pair<TConstExpressionPtr, bool>& original)
{
    ToProto(proto->mutable_expression(), original.first);
    proto->set_is_key(original.second);
}

void FromProto(std::pair<TConstExpressionPtr, bool>* original, const NProto::TSelfEquation& serialized)
{
    FromProto(&original->first, serialized.expression());
    FromProto(&original->second, serialized.is_key());
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TJoinClause* proto, const TConstJoinClausePtr& original)
{
    ToProto(proto->mutable_foreign_equations(), original->ForeignEquations);
    ToProto(proto->mutable_self_equations(), original->SelfEquations);
    ToProto(proto->mutable_joined_table_schema(), original->JoinedTableSchema);
    ToProto(proto->mutable_foreign_table_schema(), original->ForeignTableSchema);
    ToProto(proto->mutable_renamed_table_schema(), original->RenamedTableSchema);
    proto->set_foreign_key_columns_count(original->ForeignKeyColumnsCount);
    ToProto(proto->mutable_foreign_data_id(), original->ForeignDataId);
    proto->set_is_left(original->IsLeft);
    proto->set_can_use_source_ranges(original->CanUseSourceRanges);
}

void FromProto(TConstJoinClausePtr* original, const NProto::TJoinClause& serialized)
{
    auto result = New<TJoinClause>();
    FromProto(&result->ForeignEquations, serialized.foreign_equations());
    FromProto(&result->SelfEquations, serialized.self_equations());
    FromProto(&result->JoinedTableSchema, serialized.joined_table_schema());
    FromProto(&result->ForeignTableSchema, serialized.foreign_table_schema());
    FromProto(&result->RenamedTableSchema, serialized.renamed_table_schema());
    FromProto(&result->ForeignKeyColumnsCount, serialized.foreign_key_columns_count());
    FromProto(&result->ForeignDataId, serialized.foreign_data_id());
    FromProto(&result->IsLeft, serialized.is_left());
    FromProto(&result->CanUseSourceRanges, serialized.can_use_source_ranges());
    *original = result;
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TGroupClause* proto, const TConstGroupClausePtr& original)
{
    ToProto(proto->mutable_group_items(), original->GroupItems);
    ToProto(proto->mutable_aggregate_items(), original->AggregateItems);
    ToProto(proto->mutable_grouped_table_schema(), original->GroupedTableSchema);
    proto->set_is_merge(original->IsMerge);
    proto->set_is_final(original->IsFinal);
    proto->set_totals_mode(static_cast<int>(original->TotalsMode));
}

void FromProto(TConstGroupClausePtr* original, const NProto::TGroupClause& serialized)
{
    auto result = New<TGroupClause>();
    FromProto(&result->GroupedTableSchema, serialized.grouped_table_schema());
    result->IsMerge = serialized.is_merge();
    result->IsFinal = serialized.is_final();
    result->TotalsMode = ETotalsMode(serialized.totals_mode());
    FromProto(&result->GroupItems, serialized.group_items());
    FromProto(&result->AggregateItems, serialized.aggregate_items());
    *original = result;
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TProjectClause* proto, const TConstProjectClausePtr& original)
{
    ToProto(proto->mutable_projections(), original->Projections);
}

void FromProto(TConstProjectClausePtr* original, const NProto::TProjectClause& serialized)
{
    auto result = New<TProjectClause>();
    result->Projections.reserve(serialized.projections_size());
    for (int i = 0; i < serialized.projections_size(); ++i) {
        result->AddProjection(FromProto<TNamedItem>(serialized.projections(i)));
    }
    *original = result;
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TOrderItem* serialized, const TOrderItem& original)
{
    ToProto(serialized->mutable_expression(), original.first);
    serialized->set_is_descending(original.second);
}

void FromProto(TOrderItem* original, const NProto::TOrderItem& serialized)
{
    FromProto(&original->first, serialized.expression());
    FromProto(&original->second, serialized.is_descending());
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TOrderClause* proto, const TConstOrderClausePtr& original)
{
    ToProto(proto->mutable_order_items(), original->OrderItems);
}

void FromProto(TConstOrderClausePtr* original, const NProto::TOrderClause& serialized)
{
    auto result = New<TOrderClause>();
    FromProto(&result->OrderItems, serialized.order_items());
    *original = result;
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TQuery* serialized, const TConstQueryPtr& original)
{
    serialized->set_input_row_limit(original->InputRowLimit);
    serialized->set_output_row_limit(original->OutputRowLimit);

    ToProto(serialized->mutable_id(), original->Id);

    serialized->set_limit(original->Limit);
    ToProto(serialized->mutable_table_schema(), original->TableSchema);
    ToProto(serialized->mutable_renamed_table_schema(), original->RenamedTableSchema);
    serialized->set_key_columns_count(original->KeyColumnsCount);

    ToProto(serialized->mutable_join_clauses(), original->JoinClauses);

    if (original->WhereClause) {
        ToProto(serialized->mutable_predicate(), original->WhereClause);
    }

    if (original->GroupClause) {
        ToProto(serialized->mutable_group_clause(), original->GroupClause);
    }

    if (original->HavingClause) {
        ToProto(serialized->mutable_having_clause(), original->HavingClause);
    }

    if (original->OrderClause) {
        ToProto(serialized->mutable_order_clause(), original->OrderClause);
    }

    if (original->ProjectClause) {
        ToProto(serialized->mutable_project_clause(), original->ProjectClause);
    }
}

void FromProto(TConstQueryPtr* original, const NProto::TQuery& serialized)
{
    auto result = New<TQuery>(
        serialized.input_row_limit(),
        serialized.output_row_limit(),
        FromProto<TGuid>(serialized.id()));

    result->Limit = serialized.limit();

    FromProto(&result->TableSchema, serialized.table_schema());
    FromProto(&result->RenamedTableSchema, serialized.renamed_table_schema());
    FromProto(&result->KeyColumnsCount, serialized.key_columns_count());

    FromProto(&result->JoinClauses, serialized.join_clauses());

    if (serialized.has_predicate()) {
        FromProto(&result->WhereClause, serialized.predicate());
    }

    if (serialized.has_group_clause()) {
        FromProto(&result->GroupClause, serialized.group_clause());
    }

    if (serialized.has_having_clause()) {
        FromProto(&result->HavingClause, serialized.having_clause());
    }

    if (serialized.has_order_clause()) {
        FromProto(&result->OrderClause, serialized.order_clause());
    }

    if (serialized.has_project_clause()) {
        FromProto(&result->ProjectClause, serialized.project_clause());
    }

    *original = result;
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TQueryOptions* serialized, const TQueryOptions& original)
{
    serialized->set_timestamp(original.Timestamp);
    serialized->set_verbose_logging(original.VerboseLogging);
    serialized->set_max_subqueries(original.MaxSubqueries);
    serialized->set_enable_code_cache(original.EnableCodeCache);
    ToProto(serialized->mutable_workload_descriptor(), original.WorkloadDescriptor);
}

void FromProto(TQueryOptions* original, const NProto::TQueryOptions& serialized)
{
    original->Timestamp = serialized.timestamp();
    original->VerboseLogging = serialized.verbose_logging();
    original->MaxSubqueries = serialized.max_subqueries();
    original->EnableCodeCache = serialized.enable_code_cache();
    if (serialized.has_workload_descriptor()) {
        FromProto(&original->WorkloadDescriptor, serialized.workload_descriptor());
    }
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TDataRanges* serialized, const TDataRanges& original)
{
    ToProto(serialized->mutable_id(), original.Id);
    serialized->set_mount_revision(original.MountRevision);

    NTabletClient::TWireProtocolWriter writer;
    for (const auto& range : original.Ranges) {
        writer.WriteUnversionedRow(range.first);
        writer.WriteUnversionedRow(range.second);
    }
    ToProto(serialized->mutable_ranges(), ToString(MergeRefs(writer.Flush())));

    serialized->set_lookup_supported(original.LookupSupported);
}

void FromProto(TDataRanges* original, const NProto::TDataRanges& serialized)
{
    FromProto(&original->Id, serialized.id());
    original->MountRevision = serialized.mount_revision();

    struct TDataRangesBufferTag
    { };

    auto rowBuffer = New<TRowBuffer>(TDataRangesBufferTag());

    TRowRanges ranges;
    NTabletClient::TWireProtocolReader reader(TSharedRef::FromString<TDataRangesBufferTag>(serialized.ranges()));
    while (!reader.IsFinished()) {
        auto lowerBound = rowBuffer->Capture(reader.ReadUnversionedRow());
        auto upperBound = rowBuffer->Capture(reader.ReadUnversionedRow());
        ranges.emplace_back(lowerBound, upperBound);
    }
    original->Ranges = MakeSharedRange(std::move(ranges), rowBuffer);

    original->LookupSupported = serialized.lookup_supported();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
