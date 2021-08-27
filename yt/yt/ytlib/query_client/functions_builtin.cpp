#include "functions.h"
#include "functions_cg.h"
#include "cg_fragment_compiler.h"
#include "query.h"
#include "helpers.h"
#include "functions_builder.h"

#include <library/cpp/resource/resource.h>

#include <llvm/ADT/FoldingSet.h>

namespace NYT::NQueryClient {
namespace NBuiltins {

using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

class TIfFunctionCodegen
    : public IFunctionCodegen
{
public:
    static TCGValue CodegenValue(
        TCGExprContext& builder,
        std::vector<size_t> argIds,
        EValueType type,
        const TString& name)
    {
        auto nameTwine = Twine(name.c_str());

        YT_VERIFY(argIds.size() == 3);
        auto condition = CodegenFragment(builder, argIds[0]);

        // TODO(lukyan): Remove this
        if (condition.GetStaticType() == EValueType::Null) {
            return TCGValue::CreateNull(builder, type);
        }

        YT_VERIFY(condition.GetStaticType() == EValueType::Boolean);

        auto codegenIf = [&] (TCGExprContext& builder) {
            return CodegenIf<TCGExprContext, TCGValue>(
                builder,
                condition.GetTypedData(builder),
                [&] (TCGExprContext& builder) {
                    return CodegenFragment(builder, argIds[1]).Cast(builder, type);
                },
                [&] (TCGExprContext& builder) {
                    return CodegenFragment(builder, argIds[2]).Cast(builder, type);
                },
                nameTwine);
        };

        if (builder.ExpressionFragments.Items[argIds[0]].Nullable) {
            return CodegenIf<TCGExprContext, TCGValue>(
                builder,
                condition.GetIsNull(builder),
                [&] (TCGExprContext& builder) {
                    return TCGValue::CreateNull(builder, type);
                },
                codegenIf,
                nameTwine);
        } else {
            return codegenIf(builder);
        }
    }

    TCodegenExpression Profile(
        TCGVariables* /*variables*/,
        std::vector<size_t> argIds,
        std::unique_ptr<bool[]> /*literalArgs*/,
        std::vector<EValueType> /*argumentTypes*/,
        EValueType type,
        const TString& name,
        llvm::FoldingSetNodeID* /*id*/) const override
    {
        return [
            =,
            argIds = std::move(argIds)
        ] (TCGExprContext& builder) {
            return CodegenValue(
                builder,
                argIds,
                type,
                name);
        };
    }

    bool IsNullable(const std::vector<bool>& nullableArgs) const override
    {
        YT_VERIFY(nullableArgs.size() == 3);
        return nullableArgs[0] || nullableArgs[1] || nullableArgs[2];
    }

};

TKeyTriePtr IsPrefixRangeExtractor(
    const TConstFunctionExpressionPtr& expr,
    const TKeyColumns& keyColumns,
    const TRowBufferPtr& rowBuffer)
{
    auto result = TKeyTrie::Universal();
    auto lhsExpr = expr->Arguments[0];
    auto rhsExpr = expr->Arguments[1];

    auto referenceExpr = rhsExpr->As<TReferenceExpression>();
    auto constantExpr = lhsExpr->As<TLiteralExpression>();

    if (referenceExpr && constantExpr) {
        int keyPartIndex = ColumnNameToKeyPartIndex(keyColumns, referenceExpr->ColumnName);
        if (keyPartIndex >= 0) {
            auto value = TValue(constantExpr->Value);

            YT_VERIFY(value.Type == EValueType::String);

            result = New<TKeyTrie>(keyPartIndex);
            result->Bounds.emplace_back(value, true);

            ui32 length = value.Length;
            while (length > 0 && value.Data.String[length - 1] == std::numeric_limits<char>::max()) {
                --length;
            }

            if (length > 0) {
                char* newValue = rowBuffer->GetPool()->AllocateUnaligned(length);
                memcpy(newValue, value.Data.String, length);
                ++newValue[length - 1];

                value.Length = length;
                value.Data.String = newValue;
            } else {
                value = MakeSentinelValue<TUnversionedValue>(EValueType::Max);
            }
            result->Bounds.emplace_back(value, false);
        }
    }

    return result;
}

class TIsNullCodegen
    : public IFunctionCodegen
{
public:
    TCodegenExpression Profile(
        TCGVariables* /*variables*/,
        std::vector<size_t> argIds,
        std::unique_ptr<bool[]> /*literalArgs*/,
        std::vector<EValueType> /*argumentTypes*/,
        EValueType type,
        const TString& /*name*/,
        llvm::FoldingSetNodeID* /*id*/) const override
    {
        YT_VERIFY(argIds.size() == 1);

        return [
            =,
            argIds = std::move(argIds)
        ] (TCGExprContext& builder) {
            if (builder.ExpressionFragments.Items[argIds[0]].Nullable) {
                auto argValue = CodegenFragment(builder, argIds[0]);
                return TCGValue::Create(
                    builder,
                    builder->getFalse(),
                    nullptr,
                    argValue.GetIsNull(builder),
                    type);
            } else {
                return TCGValue::Create(
                    builder,
                    builder->getFalse(),
                    nullptr,
                    builder->getFalse(),
                    type);
            }
        };
    }

    bool IsNullable(const std::vector<bool>& /*nullableArgs*/) const override
    {
        return false;
    }

};

class TIfNullCodegen
    : public IFunctionCodegen
{
public:
    TCodegenExpression Profile(
        TCGVariables* /*variables*/,
        std::vector<size_t> argIds,
        std::unique_ptr<bool[]> /*literalArgs*/,
        std::vector<EValueType> /*argumentTypes*/,
        EValueType type,
        const TString& /*name*/,
        llvm::FoldingSetNodeID* /*id*/) const override
    {
        YT_VERIFY(argIds.size() == 2);

        return [
            =,
            argIds = std::move(argIds)
        ] (TCGExprContext& builder) {
            if (builder.ExpressionFragments.Items[argIds[0]].Nullable) {
                auto argValue = CodegenFragment(builder, argIds[0]);
                auto constant = CodegenFragment(builder, argIds[1]);

                Value* argIsNull = argValue.GetIsNull(builder);

                Value* length = nullptr;

                if (IsStringLikeType(argValue.GetStaticType())) {
                    length = builder->CreateSelect(
                        argIsNull,
                        constant.GetLength(),
                        argValue.GetLength());
                }

                return TCGValue::Create(
                    builder,
                    builder->CreateAnd(argIsNull, constant.GetIsNull(builder)),
                    length,
                    builder->CreateSelect(
                        argIsNull,
                        constant.GetTypedData(builder),
                        argValue.GetTypedData(builder)),
                    type);
            } else {
                return CodegenFragment(builder, argIds[0]);
            }
        };
    }

    bool IsNullable(const std::vector<bool>& nullableArgs) const override
    {
        YT_VERIFY(nullableArgs.size() == 2);
        return nullableArgs[1];
    }

};

class TIsNaNCodegen
    : public IFunctionCodegen
{
public:
    TCodegenExpression Profile(
        TCGVariables* /*variables*/,
        std::vector<size_t> argIds,
        std::unique_ptr<bool[]> /*literalArgs*/,
        std::vector<EValueType> /*argumentTypes*/,
        EValueType type,
        const TString& /*name*/,
        llvm::FoldingSetNodeID* /*id*/) const override
    {
        YT_VERIFY(argIds.size() == 1);

        return [
            =,
            argIds = std::move(argIds)
        ] (TCGExprContext& builder) {
            auto argValue = CodegenFragment(builder, argIds[0]);
            Value* data = CodegenFragment(builder, argIds[0]).GetTypedData(builder);
            if (builder.ExpressionFragments.Items[argIds[0]].Nullable) {
                return TCGValue::Create(
                    builder,
                    builder->getFalse(),
                    nullptr,
                    builder->CreateAnd(
                        builder->CreateNot(argValue.GetIsNull(builder)),
                        builder->CreateFCmpUNO(data, data)),
                    type);
            } else {
                return TCGValue::Create(
                    builder,
                    builder->getFalse(),
                    nullptr,
                    builder->CreateFCmpUNO(data, data),
                    type);
            }
        };
    }

    bool IsNullable(const std::vector<bool>& /*nullableArgs*/) const override
    {
        return false;
    }

};

class TUserCastCodegen
    : public IFunctionCodegen
{
public:

    TUserCastCodegen()
    { }

    TCodegenExpression Profile(
        TCGVariables* /*variables*/,
        std::vector<size_t> argIds,
        std::unique_ptr<bool[]> /*literalArgs*/,
        std::vector<EValueType> argumentTypes,
        EValueType type,
        const TString& /*name*/,
        llvm::FoldingSetNodeID* /*id*/) const override
    {
        YT_VERIFY(argIds.size() == 1);

        if (argumentTypes[0] == EValueType::Any) {
            return [
                =,
                argIds = std::move(argIds)
            ] (TCGExprContext& builder) {
                auto unversionedValueType = TTypeBuilder<TValue>::Get(builder->getContext());

                auto resultPtr = builder->CreateAlloca(unversionedValueType, nullptr, "resultPtr");
                auto valuePtr = builder->CreateAlloca(unversionedValueType);

                auto cgValue = CodegenFragment(builder, argIds[0]);
                cgValue.StoreToValue(builder, valuePtr);

                const char* routineName = nullptr;

                switch (type) {
                    case EValueType::Int64:
                        routineName = "AnyToInt64";
                        break;
                    case EValueType::Uint64:
                        routineName = "AnyToUint64";
                        break;
                    case EValueType::Double:
                        routineName = "AnyToDouble";
                        break;
                    case EValueType::Boolean:
                        routineName = "AnyToBoolean";
                        break;
                    case EValueType::String:
                        routineName = "AnyToString";
                        break;
                    default:
                        YT_ABORT();
                }

                builder->CreateCall(
                    builder.Module->GetRoutine(routineName),
                    {
                        builder.Buffer,
                        resultPtr,
                        valuePtr
                    });

                return TCGValue::LoadFromRowValue(
                    builder,
                    resultPtr,
                    type);
            };
        } else {
            YT_VERIFY(
                type == EValueType::Int64 ||
                type == EValueType::Uint64 ||
                type == EValueType::Double);

            return [
                =,
                argIds = std::move(argIds)
            ] (TCGExprContext& builder) {
                return CodegenFragment(builder, argIds[0]).Cast(builder, type);
            };
        }
    }

    bool IsNullable(const std::vector<bool>& nullableArgs) const override
    {
        YT_VERIFY(nullableArgs.size() == 1);
        return nullableArgs[0];
    }

};


class TSimpleAggregateCodegen
    : public IAggregateCodegen
{
public:
    TSimpleAggregateCodegen(const TString& function)
        : Function(function)
    { }

    TString Function;

    TCodegenAggregate Profile(
        EValueType argumentType,
        EValueType stateType,
        EValueType /*resultType*/,
        const TString& name,
        llvm::FoldingSetNodeID* id) const override
    {
        if (id) {
            id->AddString(ToStringRef(Function + "_aggregate"));
        }

        auto iteration = [
            this_ = MakeStrong(this),
            argumentType,
            stateType,
            name
        ] (TCGBaseContext& builder, Value* buffer, TCGValue aggregateValue, TCGValue newValue)
        {
            return CodegenIf<TCGBaseContext, TCGValue>(
                builder,
                builder->CreateNot(newValue.GetIsNull(builder)),
                [&] (TCGBaseContext& builder) {
                    Value* valueLength = nullptr;
                    if (argumentType == EValueType::String) {
                        valueLength = newValue.GetLength();
                    }
                    Value* newData = newValue.GetTypedData(builder);

                    return CodegenIf<TCGBaseContext, TCGValue>(
                        builder,
                        aggregateValue.GetIsNull(builder),
                        [&] (TCGBaseContext& builder) {
                            if (argumentType == EValueType::String) {
                                Value* permanentData = builder->CreateCall(
                                    builder.Module->GetRoutine("AllocateBytes"),
                                    {
                                        buffer,
                                        builder->CreateZExt(valueLength, builder->getInt64Ty())
                                    });
                                builder->CreateMemCpy(
                                    permanentData,
                                    llvm::Align(1),
                                    newData,
                                    llvm::Align(1),
                                    valueLength);
                                return TCGValue::Create(
                                    builder,
                                    builder->getFalse(),
                                    valueLength,
                                    permanentData,
                                    stateType);
                            } else {
                                return newValue;
                            }
                        },
                        [&] (TCGBaseContext& builder) {
                            Value* aggregateData = aggregateValue.GetTypedData(builder);
                            Value* resultData = nullptr;
                            Value* resultLength = nullptr;

                            if (this_->Function == "sum") {
                                switch (argumentType) {
                                    case EValueType::Int64:
                                    case EValueType::Uint64:
                                        resultData = builder->CreateAdd(
                                            aggregateData,
                                            newData);
                                        break;
                                    case EValueType::Double:
                                        resultData = builder->CreateFAdd(
                                            aggregateData,
                                            newData);
                                        break;
                                    default:
                                        YT_UNIMPLEMENTED();
                                }
                            } else if (this_->Function == "min") {
                                Value* compareResult = nullptr;
                                switch (argumentType) {
                                    case EValueType::Int64:
                                        compareResult = builder->CreateICmpSLT(newData, aggregateData);
                                        break;
                                    case EValueType::Uint64:
                                        compareResult = builder->CreateICmpULT(newData, aggregateData);
                                        break;
                                    case EValueType::Boolean:
                                        compareResult = builder->CreateICmpULT(newData, aggregateData);
                                        break;
                                    case EValueType::Double:
                                        compareResult = builder->CreateFCmpULT(newData, aggregateData);
                                        break;
                                    case EValueType::String: {
                                        compareResult = CodegenLexicographicalCompare(
                                            builder,
                                            newData,
                                            valueLength,
                                            aggregateData,
                                            aggregateValue.GetLength());

                                        newData = CodegenIf<TCGBaseContext, Value*>(
                                            builder,
                                            compareResult,
                                            [&] (TCGBaseContext& builder) {
                                                Value* permanentData = builder->CreateCall(
                                                    builder.Module->GetRoutine("AllocateBytes"),
                                                    {
                                                        buffer,
                                                        builder->CreateZExt(valueLength, builder->getInt64Ty())
                                                    });
                                                builder->CreateMemCpy(
                                                    permanentData,
                                                    llvm::Align(1),
                                                    newData,
                                                    llvm::Align(1),
                                                    valueLength);
                                                return permanentData;
                                            },
                                            [&] (TCGBaseContext& /*builder*/) {
                                                return newData;
                                            });
                                        break;
                                    }
                                    default:
                                        YT_UNIMPLEMENTED();
                                }

                                if (argumentType == EValueType::String) {
                                    resultLength = builder->CreateSelect(
                                        compareResult,
                                        valueLength,
                                        aggregateValue.GetLength());
                                }

                                resultData = builder->CreateSelect(
                                    compareResult,
                                    newData,
                                    aggregateData);
                            } else if (this_->Function == "max") {
                                Value* compareResult = nullptr;
                                switch (argumentType) {
                                    case EValueType::Int64:
                                        compareResult = builder->CreateICmpSGT(newData, aggregateData);
                                        break;
                                    case EValueType::Uint64:
                                        compareResult = builder->CreateICmpUGT(newData, aggregateData);
                                        break;
                                    case EValueType::Boolean:
                                        compareResult = builder->CreateICmpUGT(newData, aggregateData);
                                        break;
                                    case EValueType::Double:
                                        compareResult = builder->CreateFCmpUGT(newData, aggregateData);
                                        break;
                                    case EValueType::String: {
                                        compareResult = CodegenLexicographicalCompare(
                                            builder,
                                            aggregateData,
                                            aggregateValue.GetLength(),
                                            newData,
                                            valueLength);

                                        newData = CodegenIf<TCGBaseContext, Value*>(
                                            builder,
                                            compareResult,
                                            [&] (TCGBaseContext& builder) {
                                                Value* permanentData = builder->CreateCall(
                                                    builder.Module->GetRoutine("AllocateBytes"),
                                                    {
                                                        buffer,
                                                        builder->CreateZExt(valueLength, builder->getInt64Ty())
                                                    });
                                                builder->CreateMemCpy(
                                                    permanentData,
                                                    llvm::Align(1),
                                                    newData,
                                                    llvm::Align(1),
                                                    valueLength);
                                                return permanentData;
                                            },
                                            [&] (TCGBaseContext& /*builder*/) {
                                                return newData;
                                            });
                                        break;
                                    }
                                    default:
                                        YT_UNIMPLEMENTED();
                                }

                                if (argumentType == EValueType::String) {
                                    resultLength = builder->CreateSelect(
                                        compareResult,
                                        valueLength,
                                        aggregateValue.GetLength());
                                }

                                resultData = builder->CreateSelect(
                                    compareResult,
                                    newData,
                                    aggregateData);
                            } else {
                                YT_UNIMPLEMENTED();
                            }

                            return TCGValue::Create(
                                builder,
                                builder->getFalse(),
                                resultLength,
                                resultData,
                                stateType);
                        });
                },
                [&] (TCGBaseContext& /*builder*/) {
                    return aggregateValue;
                });
        };

        TCodegenAggregate codegenAggregate;
        codegenAggregate.Initialize = [
            this_ = MakeStrong(this),
            stateType,
            name
        ] (TCGBaseContext& builder, Value* /*buffer*/) {
            return TCGValue::CreateNull(builder, stateType);
        };

        codegenAggregate.Update = iteration;
        codegenAggregate.Merge = iteration;

        codegenAggregate.Finalize = [
            this_ = MakeStrong(this),
            name
        ] (TCGBaseContext& /*builder*/, Value* /*buffer*/, TCGValue aggState) {
            return aggState;
        };

        return codegenAggregate;
    }

    bool IsFirst() const override
    {
        return false;
    }
};

} // namespace NBuiltins

bool IsUserCastFunction(const TString& name)
{
    return name == "int64" || name == "uint64" || name == "double";
}

namespace {

void RegisterBuiltinFunctions(
    const TTypeInferrerMapPtr& typeInferrers,
    const TFunctionProfilerMapPtr& functionProfilers,
    const TAggregateProfilerMapPtr& aggregateProfilers)
{
    TFunctionRegistryBuilder builder(typeInferrers, functionProfilers, aggregateProfilers);

    builder.RegisterFunction(
        "is_substr",
        std::vector<TType>{EValueType::String, EValueType::String},
        EValueType::Boolean,
        UDF_BC(is_substr),
        ECallingConvention::Simple);

    builder.RegisterFunction(
        "lower",
        std::vector<TType>{EValueType::String},
        EValueType::String,
        UDF_BC(lower),
        ECallingConvention::Simple);

    builder.RegisterFunction(
        "concat",
        std::vector<TType>{EValueType::String, EValueType::String},
        EValueType::String,
        UDF_BC(concat),
        ECallingConvention::Simple);

    builder.RegisterFunction(
        "sleep",
        std::vector<TType>{EValueType::Int64},
        EValueType::Int64,
        UDF_BC(sleep),
        ECallingConvention::Simple);

    builder.RegisterFunction(
        "farm_hash",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{},
        TUnionType{
            EValueType::Int64,
            EValueType::Uint64,
            EValueType::Boolean,
            EValueType::String
        },
        EValueType::Uint64,
        UDF_BC(farm_hash));

    builder.RegisterFunction(
        "bigb_hash",
        std::vector<TType>{EValueType::String},
        EValueType::Uint64,
        UDF_BC(bigb_hash),
        ECallingConvention::Simple);

    builder.RegisterFunction(
        "make_map",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{},
        TUnionType{
            EValueType::Int64,
            EValueType::Uint64,
            EValueType::Boolean,
            EValueType::Double,
            EValueType::String,
            EValueType::Any
        },
        EValueType::Any,
        UDF_BC(make_map));

    builder.RegisterFunction(
        "numeric_to_string",
        std::vector<TType>{
            TUnionType{
                EValueType::Int64,
                EValueType::Uint64,
                EValueType::Double,
            }},
        EValueType::String,
        UDF_BC(str_conv),
        ECallingConvention::UnversionedValue);

    builder.RegisterFunction(
        "parse_int64",
        std::vector<TType>{EValueType::String},
        EValueType::Int64,
        UDF_BC(str_conv),
        ECallingConvention::UnversionedValue);

    builder.RegisterFunction(
        "parse_uint64",
        std::vector<TType>{EValueType::String},
        EValueType::Uint64,
        UDF_BC(str_conv),
        ECallingConvention::UnversionedValue);

    builder.RegisterFunction(
        "parse_double",
        std::vector<TType>{EValueType::String},
        EValueType::Double,
        UDF_BC(str_conv),
        ECallingConvention::UnversionedValue);

    if (typeInferrers) {
        typeInferrers->emplace("is_null", New<TFunctionTypeInferrer>(
            std::unordered_map<TTypeArgument, TUnionType>(),
            std::vector<TType>{0},
            EValueType::Null,
            EValueType::Boolean));
    }

    if (functionProfilers) {
        functionProfilers->emplace("is_null", New<NBuiltins::TIsNullCodegen>());
    }

    if (typeInferrers) {
        typeInferrers->emplace("is_nan", New<TFunctionTypeInferrer>(
            std::vector<TType>{EValueType::Double},
            EValueType::Boolean));
    }

    if (functionProfilers) {
        functionProfilers->emplace("is_nan", New<NBuiltins::TIsNaNCodegen>());
    }

    auto typeArg = 0;
    auto castConstraints = std::unordered_map<TTypeArgument, TUnionType>();
    castConstraints[typeArg] = std::vector<EValueType>{
        EValueType::Int64,
        EValueType::Uint64,
        EValueType::Double,
        EValueType::Any};

    if (typeInferrers) {
        typeInferrers->emplace("int64", New<TFunctionTypeInferrer>(
            castConstraints,
            std::vector<TType>{typeArg},
            EValueType::Null,
            EValueType::Int64));
    }

    if (functionProfilers) {
        functionProfilers->emplace("int64", New<NBuiltins::TUserCastCodegen>());
    }

    if (typeInferrers) {
        typeInferrers->emplace("uint64", New<TFunctionTypeInferrer>(
            castConstraints,
            std::vector<TType>{typeArg},
            EValueType::Null,
            EValueType::Uint64));
    }

    if (functionProfilers) {
        functionProfilers->emplace("uint64", New<NBuiltins::TUserCastCodegen>());
    }

    if (typeInferrers) {
        typeInferrers->emplace("double", New<TFunctionTypeInferrer>(
            castConstraints,
            std::vector<TType>{typeArg},
            EValueType::Null,
            EValueType::Double));
    }

    if (functionProfilers) {
        functionProfilers->emplace("double", New<NBuiltins::TUserCastCodegen>());
    }

    if (typeInferrers) {
        typeInferrers->emplace("boolean", New<TFunctionTypeInferrer>(
            std::vector<TType>{EValueType::Any},
            EValueType::Boolean));
    }

    if (functionProfilers) {
        functionProfilers->emplace("boolean", New<NBuiltins::TUserCastCodegen>());
    }

    if (typeInferrers) {
        typeInferrers->emplace("string", New<TFunctionTypeInferrer>(
            std::vector<TType>{EValueType::Any},
            EValueType::String));
    }

    if (functionProfilers) {
        functionProfilers->emplace("string", New<NBuiltins::TUserCastCodegen>());
    }

    if (typeInferrers) {
        typeInferrers->emplace("if_null", New<TFunctionTypeInferrer>(
            std::unordered_map<TTypeArgument, TUnionType>(),
            std::vector<TType>{0, 0},
            0));
    }

    if (functionProfilers) {
        functionProfilers->emplace("if_null", New<NBuiltins::TIfNullCodegen>());
    }


    builder.RegisterFunction(
        "regex_full_match",
        "regex_full_match",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{EValueType::String, EValueType::String},
        EValueType::Null,
        EValueType::Boolean,
        UDF_BC(regex),
        New<TUnversionedValueCallingConvention>(-1),
        true);

    builder.RegisterFunction(
        "regex_partial_match",
        "regex_partial_match",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{EValueType::String, EValueType::String},
        EValueType::Null,
        EValueType::Boolean,
        UDF_BC(regex),
        New<TUnversionedValueCallingConvention>(-1),
        true);

    builder.RegisterFunction(
        "regex_replace_first",
        "regex_replace_first",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{EValueType::String, EValueType::String, EValueType::String},
        EValueType::Null,
        EValueType::String,
        UDF_BC(regex),
        New<TUnversionedValueCallingConvention>(-1),
        true);

    builder.RegisterFunction(
        "regex_replace_all",
        "regex_replace_all",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{EValueType::String, EValueType::String, EValueType::String},
        EValueType::Null,
        EValueType::String,
        UDF_BC(regex),
        New<TUnversionedValueCallingConvention>(-1),
        true);

    builder.RegisterFunction(
        "regex_extract",
        "regex_extract",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{EValueType::String, EValueType::String, EValueType::String},
        EValueType::Null,
        EValueType::String,
        UDF_BC(regex),
        New<TUnversionedValueCallingConvention>(-1),
        true);

    builder.RegisterFunction(
        "regex_escape",
        "regex_escape",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{EValueType::String},
        EValueType::Null,
        EValueType::String,
        UDF_BC(regex),
        New<TUnversionedValueCallingConvention>(-1),
        true);

    auto constraints = std::unordered_map<TTypeArgument, TUnionType>();
    constraints[typeArg] = std::vector<EValueType>{
        EValueType::Int64,
        EValueType::Uint64,
        EValueType::Boolean,
        EValueType::Double,
        EValueType::String};
    auto sumConstraints = std::unordered_map<TTypeArgument, TUnionType>();
    sumConstraints[typeArg] = std::vector<EValueType>{
        EValueType::Int64,
        EValueType::Uint64,
        EValueType::Double};
    auto anyConstraints = std::unordered_map<TTypeArgument, TUnionType>();
    anyConstraints[typeArg] = std::vector<EValueType>{
        EValueType::Int64,
        EValueType::Uint64,
        EValueType::Boolean,
        EValueType::Double,
        EValueType::String,
        EValueType::Any};

    builder.RegisterAggregate(
        "first",
        anyConstraints,
        typeArg,
        typeArg,
        typeArg,
        UDF_BC(first),
        ECallingConvention::UnversionedValue,
        true);
    
    auto xdeltaConstraints = std::unordered_map<TTypeArgument, TUnionType>();
    xdeltaConstraints[typeArg] = std::vector<EValueType>{
        EValueType::Null,
        EValueType::String};
    builder.RegisterAggregate(
        "xdelta",
        xdeltaConstraints,
        typeArg,
        typeArg,
        typeArg,
        UDF_BC(xdelta),
        ECallingConvention::UnversionedValue);

    if (typeInferrers) {
        typeInferrers->emplace("sum", New<TAggregateTypeInferrer>(
            sumConstraints,
            typeArg,
            typeArg,
            typeArg));
    }

    if (aggregateProfilers) {
        aggregateProfilers->emplace("sum", New<NBuiltins::TSimpleAggregateCodegen>("sum"));
    }

    for (const auto& name : {"min", "max"}) {
        if (typeInferrers) {
            typeInferrers->emplace(name, New<TAggregateTypeInferrer>(
                constraints,
                typeArg,
                typeArg,
                typeArg));
        }

        if (aggregateProfilers) {
            aggregateProfilers->emplace(name, New<NBuiltins::TSimpleAggregateCodegen>(name));
        }
    }

    builder.RegisterAggregate(
        "avg",
        std::unordered_map<TTypeArgument, TUnionType>(),
        EValueType::Int64,
        EValueType::Double,
        EValueType::String,
        UDF_BC(avg),
        ECallingConvention::UnversionedValue);
    builder.RegisterAggregate(
        "cardinality",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<EValueType>{
            EValueType::String,
            EValueType::Uint64,
            EValueType::Int64,
            EValueType::Double,
            EValueType::Boolean},
        EValueType::Uint64,
        EValueType::String,
        UDF_BC(hyperloglog),
        ECallingConvention::UnversionedValue);

    builder.RegisterFunction(
        "format_timestamp",
        std::vector<TType>{EValueType::Int64, EValueType::String},
        EValueType::String,
        UDF_BC(dates),
        ECallingConvention::Simple);

    std::vector<TString> timestampFloorFunctions = {
        "timestamp_floor_hour",
        "timestamp_floor_day",
        "timestamp_floor_week",
        "timestamp_floor_month",
        "timestamp_floor_year"};

    for (const auto& name : timestampFloorFunctions) {
        builder.RegisterFunction(
            name,
            std::vector<TType>{EValueType::Int64},
            EValueType::Int64,
            UDF_BC(dates),
            ECallingConvention::Simple);
    }

    builder.RegisterFunction(
        "format_guid",
        std::vector<TType>{EValueType::Uint64, EValueType::Uint64},
        EValueType::String,
        UDF_BC(format_guid),
        ECallingConvention::Simple);

    std::vector<std::pair<TString, EValueType>> ypathGetFunctions = {
        {"try_get_int64", EValueType::Int64},
        {"get_int64", EValueType::Int64},
        {"try_get_uint64", EValueType::Uint64},
        {"get_uint64", EValueType::Uint64},
        {"try_get_double", EValueType::Double},
        {"get_double", EValueType::Double},
        {"try_get_boolean", EValueType::Boolean},
        {"get_boolean", EValueType::Boolean},
        {"try_get_string", EValueType::String},
        {"get_string", EValueType::String},
        {"try_get_any", EValueType::Any},
        {"get_any", EValueType::Any}};

    for (const auto& fns : ypathGetFunctions) {
        auto&& name = fns.first;
        auto&& type = fns.second;
        builder.RegisterFunction(
            name,
            std::vector<TType>{EValueType::Any, EValueType::String},
            type,
            UDF_BC(ypath_get),
            ECallingConvention::UnversionedValue);
    }

    builder.RegisterFunction(
        "to_any",
        std::vector<TType>{
            TUnionType{
                EValueType::String,
                EValueType::Uint64,
                EValueType::Int64,
                EValueType::Double,
                EValueType::Boolean,
                EValueType::Any}},
        EValueType::Any,
        UDF_BC(to_any),
        ECallingConvention::UnversionedValue);

    builder.RegisterFunction(
        "list_contains",
        std::vector<TType>{
            EValueType::Any,
            TUnionType{
                EValueType::Int64,
                EValueType::Uint64,
                EValueType::Double,
                EValueType::Boolean,
                EValueType::String,
            }},
        EValueType::Boolean,
        UDF_BC(list_contains),
        ECallingConvention::UnversionedValue);

    builder.RegisterFunction(
        "any_to_yson_string",
        std::vector<TType>{EValueType::Any},
        EValueType::String,
        UDF_BC(any_to_yson_string),
        ECallingConvention::Simple);

    builder.RegisterFunction(
        "_yt_has_permissions",
        "has_permissions",
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{EValueType::Any, EValueType::String, EValueType::String},
        EValueType::Null,
        EValueType::Boolean,
        UDF_BC(has_permissions),
        GetCallingConvention(ECallingConvention::UnversionedValue));
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TConstTypeInferrerMapPtr CreateBuiltinTypeInferrers()
{
    auto result = New<TTypeInferrerMap>();

    result->emplace("if", New<TFunctionTypeInferrer>(
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{ EValueType::Boolean, 0, 0 },
        0));

    result->emplace("is_prefix", New<TFunctionTypeInferrer>(
        std::unordered_map<TTypeArgument, TUnionType>(),
        std::vector<TType>{ EValueType::String, EValueType::String },
        EValueType::Boolean));

    RegisterBuiltinFunctions(result.Get(), nullptr, nullptr);

    return result;
}

const TConstTypeInferrerMapPtr BuiltinTypeInferrersMap = CreateBuiltinTypeInferrers();

TConstRangeExtractorMapPtr CreateBuiltinRangeExtractorMap()
{
    auto result = New<TRangeExtractorMap>();
    result->emplace("is_prefix", NBuiltins::IsPrefixRangeExtractor);

    return result;
}

const TConstRangeExtractorMapPtr BuiltinRangeExtractorMap = CreateBuiltinRangeExtractorMap();

TConstFunctionProfilerMapPtr CreateBuiltinFunctionProfilers()
{
    auto result = New<TFunctionProfilerMap>();

    result->emplace("if", New<NBuiltins::TIfFunctionCodegen>());

    result->emplace("is_prefix", New<TExternalFunctionCodegen>(
        "is_prefix",
        "is_prefix",
        UDF_BC(is_prefix),
        GetCallingConvention(ECallingConvention::Simple),
        TSharedRef()));

    RegisterBuiltinFunctions(nullptr, result.Get(), nullptr);

    return result;
}

const TConstFunctionProfilerMapPtr BuiltinFunctionProfilers = CreateBuiltinFunctionProfilers();

TConstAggregateProfilerMapPtr CreateBuiltinAggregateProfilers()
{
    auto result = New<TAggregateProfilerMap>();

    RegisterBuiltinFunctions(nullptr, nullptr, result.Get());

    return result;
}

const TConstAggregateProfilerMapPtr BuiltinAggregateProfilers = CreateBuiltinAggregateProfilers();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryClient
