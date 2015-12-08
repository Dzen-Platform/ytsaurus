#include "folding_profiler.h"
#include "cg_fragment_compiler.h"
#include "function_registry.h"
#include "functions.h"
#include "plan_helpers.h"

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EFoldingObjectType,
    (ScanOp)
    (JoinOp)
    (FilterOp)
    (GroupOp)
    (HavingOp)
    (OrderOp)
    (ProjectOp)

    (LiteralExpr)
    (ReferenceExpr)
    (FunctionExpr)
    (UnaryOpExpr)
    (BinaryOpExpr)
    (InOpExpr)

    (NamedExpression)
    (AggregateItem)

    (TableSchema)
);

//! Computes a strong structural hash used to cache query fragments.
class TFoldingProfiler
    : private TNonCopyable
{
public:
    TFoldingProfiler(const IFunctionRegistryPtr functionRegistry);

    TCodegenSource Profile(TConstQueryPtr query);
    TCodegenExpression Profile(TConstExpressionPtr expr, const TTableSchema& tableSchema);
    void Profile(const TTableSchema& tableSchema, int keySize = std::numeric_limits<int>::max());

    void Set(llvm::FoldingSetNodeID* id);
    void Set(TCGVariables* variables);
    void Set(yhash_set<Stroka>* references);
    void Set(std::vector<std::vector<bool>>* literalArgs);

private:
    TCodegenExpression Profile(const TNamedItem& namedExpression, const TTableSchema& schema);
    std::pair<TCodegenExpression, TCodegenAggregate> Profile(
        const TAggregateItem& aggregateItem,
        IAggregateFunctionDescriptorPtr aggregateFunction,
        const TTableSchema& schema);

    void Fold(int numeric);
    void Fold(const char* str);
    void Refer(const TReferenceExpression* referenceExpr);

    llvm::FoldingSetNodeID* Id_ = nullptr;
    TCGVariables* Variables_ = nullptr;
    yhash_set<Stroka>* References_ = nullptr;
    std::vector<std::vector<bool>>* LiteralArgs_ = nullptr;

    const IFunctionRegistryPtr FunctionRegistry_;
};

////////////////////////////////////////////////////////////////////////////////

TFoldingProfiler::TFoldingProfiler(
    const IFunctionRegistryPtr functionRegistry)
    : FunctionRegistry_(functionRegistry)
{ }

void TFoldingProfiler::Set(llvm::FoldingSetNodeID* id)
{
    Id_ = id;
}

void TFoldingProfiler::Set(TCGVariables* variables)
{
    Variables_ = variables;
}

void TFoldingProfiler::Set(yhash_set<Stroka>* references)
{
    References_ = references;
}

void TFoldingProfiler::Set(std::vector<std::vector<bool>>* literalArgs)
{
    LiteralArgs_ = literalArgs;
}

TCodegenSource TFoldingProfiler::Profile(TConstQueryPtr query)
{
    Fold(static_cast<int>(EFoldingObjectType::ScanOp));
    Profile(query->RenamedTableSchema);
    TCodegenSource codegenSource = &CodegenScanOp;

    TTableSchema schema = query->RenamedTableSchema;

    for (const auto& joinClause : query->JoinClauses) {
        Fold(static_cast<int>(EFoldingObjectType::JoinOp));

        Profile(schema);
        Profile(joinClause->RenamedTableSchema);

        std::vector<TCodegenExpression> selfKeys;
        for (const auto& column : joinClause->Equations) {
            selfKeys.push_back(Profile(column.first, schema));
            Profile(column.second, joinClause->RenamedTableSchema);
        }

        if (auto selfFilter = ExtractPredicateForColumnSubset(query->WhereClause, schema)) {
            codegenSource = MakeCodegenFilterOp(Profile(selfFilter, schema), std::move(codegenSource));
        }

        codegenSource = MakeCodegenJoinOp(
            Variables_->JoinEvaluators.size(),
            selfKeys,
            schema,
            std::move(codegenSource));


        Variables_->JoinEvaluators.push_back(GetJoinEvaluator(
            *joinClause,
            query->WhereClause,
            schema));

        schema = joinClause->JoinedTableSchema;
    }

    if (query->WhereClause) {
        Fold(static_cast<int>(EFoldingObjectType::FilterOp));
        codegenSource = MakeCodegenFilterOp(Profile(query->WhereClause, schema), std::move(codegenSource));
    }

    if (auto groupClause = query->GroupClause.Get()) {
        Fold(static_cast<int>(EFoldingObjectType::GroupOp));

        std::vector<TCodegenExpression> codegenGroupExprs;
        std::vector<TCodegenExpression> codegenAggregateExprs;
        std::vector<TCodegenAggregate> codegenAggregates;

        for (const auto& groupItem : groupClause->GroupItems) {
            codegenGroupExprs.push_back(Profile(groupItem, schema));
        }

        for (const auto& aggregateItem : groupClause->AggregateItems) {
            auto aggregateFunction = FunctionRegistry_->GetAggregateFunction(aggregateItem.AggregateFunction);

            auto aggregate = Profile(aggregateItem, aggregateFunction, schema);
            codegenAggregateExprs.push_back(aggregate.first);
            codegenAggregates.push_back(aggregate.second);
        }

        int keySize = codegenGroupExprs.size();

        auto keyTypes = std::vector<EValueType>();
        for (int id = 0; id < keySize; id++) {
            keyTypes.push_back(groupClause->GroupedTableSchema.Columns()[id].Type);
        }

        codegenSource = MakeCodegenGroupOp(
            MakeCodegenAggregateInitialize(
                codegenAggregates,
                keySize),
            MakeCodegenEvaluateGroups(
                codegenGroupExprs),
            MakeCodegenEvaluateAggregateArgs(
                codegenGroupExprs,
                codegenAggregateExprs,
                codegenAggregates,
                groupClause->IsMerge,
                schema),
            MakeCodegenAggregateUpdate(
                codegenAggregates,
                keySize,
                groupClause->IsMerge),
            MakeCodegenAggregateFinalize(
                codegenAggregates,
                keySize,
                groupClause->IsFinal),
            std::move(codegenSource),
            keyTypes,
            keySize + codegenAggregates.size());

        schema = groupClause->GetTableSchema();
    }

    if (query->HavingClause) {
        Fold(static_cast<int>(EFoldingObjectType::HavingOp));
        codegenSource = MakeCodegenFilterOp(Profile(query->HavingClause, schema), std::move(codegenSource));
    }

    if (auto orderClause = query->OrderClause.Get()) {
        Fold(static_cast<int>(EFoldingObjectType::OrderOp));

        std::vector<TCodegenExpression> codegenOrderExprs;
        std::vector<bool> isDesc;

        for (const auto& item : orderClause->OrderItems) {
            codegenOrderExprs.push_back(Profile(item.first, schema));
            Fold(item.second);
            isDesc.push_back(item.second);
        }

        codegenSource = MakeCodegenOrderOp(
            codegenOrderExprs,
            schema,
            std::move(codegenSource),
            isDesc);
    }

    if (auto projectClause = query->ProjectClause.Get()) {
        Fold(static_cast<int>(EFoldingObjectType::ProjectOp));

        std::vector<TCodegenExpression> codegenProjectExprs;

        for (const auto& item : projectClause->Projections) {
            codegenProjectExprs.push_back(Profile(item, schema));
        }

        codegenSource = MakeCodegenProjectOp(std::move(codegenProjectExprs), std::move(codegenSource));
        schema = query->ProjectClause->GetTableSchema();
    }

    return codegenSource;
}

TCodegenExpression TFoldingProfiler::Profile(TConstExpressionPtr expr, const TTableSchema& schema)
{
    Fold(static_cast<ui16>(expr->Type));
    if (auto literalExpr = expr->As<TLiteralExpression>()) {
        Fold(static_cast<int>(EFoldingObjectType::LiteralExpr));
        Fold(static_cast<ui16>(TValue(literalExpr->Value).Type));

        int index = Variables_
            ? Variables_->ConstantsRowBuilder.AddValue(TValue(literalExpr->Value))
            : -1;

        return MakeCodegenLiteralExpr(index, literalExpr->Type);
    } else if (auto referenceExpr = expr->As<TReferenceExpression>()) {
        Fold(static_cast<int>(EFoldingObjectType::ReferenceExpr));
        Fold(referenceExpr->ColumnName.c_str());
        Refer(referenceExpr);

        return MakeCodegenReferenceExpr(
            schema.GetColumnIndexOrThrow(referenceExpr->ColumnName),
            referenceExpr->Type,
            referenceExpr->ColumnName);
    } else if (auto functionExpr = expr->As<TFunctionExpression>()) {
        Fold(static_cast<int>(EFoldingObjectType::FunctionExpr));
        Fold(functionExpr->FunctionName.c_str());

        std::vector<TCodegenExpression> codegenArgs;
        std::vector<EValueType> argumentTypes;
        std::vector<bool> literalArgs;
        for (const auto& argument : functionExpr->Arguments) {
            codegenArgs.push_back(Profile(argument, schema));
            argumentTypes.push_back(argument->Type);
            literalArgs.push_back(argument->As<TLiteralExpression>() != nullptr);
        }

        int index = -1;
        if (LiteralArgs_) {
            index =  LiteralArgs_->size();
            LiteralArgs_->push_back(std::move(literalArgs));
        }

        return FunctionRegistry_->GetFunction(functionExpr->FunctionName)
            ->MakeCodegenExpr(
                MakeCodegenFunctionContext(index),
                std::move(codegenArgs),
                std::move(argumentTypes),
                functionExpr->Type,
                "{" + InferName(functionExpr, true) + "}");
    } else if (auto unaryOp = expr->As<TUnaryOpExpression>()) {
        Fold(static_cast<int>(EFoldingObjectType::UnaryOpExpr));
        Fold(static_cast<int>(unaryOp->Opcode));

        return MakeCodegenUnaryOpExpr(
            unaryOp->Opcode,
            Profile(unaryOp->Operand, schema),
            unaryOp->Type,
            "{" + InferName(unaryOp, true) + "}");
    } else if (auto binaryOp = expr->As<TBinaryOpExpression>()) {
        Fold(static_cast<int>(EFoldingObjectType::BinaryOpExpr));
        Fold(static_cast<int>(binaryOp->Opcode));

        return MakeCodegenBinaryOpExpr(
            binaryOp->Opcode,
            Profile(binaryOp->Lhs, schema),
            Profile(binaryOp->Rhs, schema),
            binaryOp->Type,
            "{" + InferName(binaryOp, true) + "}");
    } else if (auto inOp = expr->As<TInOpExpression>()) {
        Fold(static_cast<int>(EFoldingObjectType::InOpExpr));

        std::vector<TCodegenExpression> codegenArgs;
        for (const auto& argument : inOp->Arguments) {
            codegenArgs.push_back(Profile(argument, schema));
        }

        int index = -1;
        if (Variables_) {
            index = Variables_->LiteralRows.size();
            Variables_->LiteralRows.push_back(inOp->Values);
        }

        return MakeCodegenInOpExpr(codegenArgs, index);
    }

    YUNREACHABLE();
}

void TFoldingProfiler::Profile(const TTableSchema& tableSchema, int keySize)
{
    Fold(static_cast<int>(EFoldingObjectType::TableSchema));
    Fold(keySize);
    for (int index = 0; index < tableSchema.Columns().size() && index < keySize; ++index) {
        const auto& column = tableSchema.Columns()[index];
        Fold(static_cast<ui16>(column.Type));
        Fold(column.Name.c_str());
        Fold(static_cast<bool>(column.Expression));
        if (column.Expression) {
            Fold(column.Expression.Get().c_str());
        }
    }
    int aggregateColumnCount = 0;
    for (int index = keySize; index < tableSchema.Columns().size(); ++index) {
        if(tableSchema.Columns()[index].Aggregate) {
            ++aggregateColumnCount;
        }
    }
    Fold(aggregateColumnCount);
    for (int index = keySize; index < tableSchema.Columns().size(); ++index) {
        const auto& column = tableSchema.Columns()[index];
        Fold(index);
        if (column.Aggregate) {
            Fold(column.Aggregate.Get().c_str());
        }
    }
}

TCodegenExpression TFoldingProfiler::Profile(const TNamedItem& namedExpression, const TTableSchema& schema)
{
    Fold(static_cast<int>(EFoldingObjectType::NamedExpression));
    Fold(namedExpression.Name.c_str());

    return Profile(namedExpression.Expression, schema);
}

std::pair<TCodegenExpression, TCodegenAggregate> TFoldingProfiler::Profile(
    const TAggregateItem& aggregateItem,
    IAggregateFunctionDescriptorPtr aggregateFunction,
    const TTableSchema& schema)
{
    Fold(static_cast<int>(EFoldingObjectType::AggregateItem));
    Fold(aggregateItem.AggregateFunction.c_str());
    Fold(aggregateItem.Name.c_str());

    return std::make_pair(
        Profile(aggregateItem.Expression, schema),
        aggregateFunction->MakeCodegenAggregate(
            aggregateItem.Expression->Type,
            aggregateItem.StateType,
            aggregateItem.ResultType,
            aggregateItem.Name));
}

void TFoldingProfiler::Fold(int numeric)
{
    if (Id_) {
        Id_->AddInteger(numeric);
    }
}

void TFoldingProfiler::Fold(const char* str)
{
    if (Id_) {
        Id_->AddString(str);
    }
}

void TFoldingProfiler::Refer(const TReferenceExpression* referenceExpr)
{
    if (References_) {
        References_->insert(referenceExpr->ColumnName);
    }
}

////////////////////////////////////////////////////////////////////////////////

TCGQueryCallbackGenerator Profile(
    TConstQueryPtr query,
    llvm::FoldingSetNodeID* id,
    TCGVariables* variables,
    yhash_set<Stroka>* references,
    std::vector<std::vector<bool>>* literalArgs,
    const IFunctionRegistryPtr functionRegistry)
{
    TFoldingProfiler profiler(functionRegistry);
    profiler.Set(id);
    profiler.Set(variables);
    profiler.Set(references);
    profiler.Set(literalArgs);

    return [
            codegenSource = profiler.Profile(query)
        ] () {
            return CodegenEvaluate(std::move(codegenSource));
        };
}

TCGExpressionCallbackGenerator Profile(
    TConstExpressionPtr expr,
    const TTableSchema& schema,
    llvm::FoldingSetNodeID* id,
    TCGVariables* variables,
    yhash_set<Stroka>* references,
    std::vector<std::vector<bool>>* literalArgs,
    const IFunctionRegistryPtr functionRegistry)
{
    TFoldingProfiler profiler(functionRegistry);
    profiler.Set(variables);
    profiler.Set(references);
    profiler.Set(literalArgs);

    return [
            codegenExpr = profiler.Profile(expr, schema)
        ] () {
            return CodegenExpression(std::move(codegenExpr));
        };
}

void Profile(const TTableSchema& tableSchema, int keySize, llvm::FoldingSetNodeID* id, const IFunctionRegistryPtr functionRegistry)
{
    TFoldingProfiler profiler(functionRegistry);
    profiler.Set(id);

    profiler.Profile(tableSchema, keySize);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

