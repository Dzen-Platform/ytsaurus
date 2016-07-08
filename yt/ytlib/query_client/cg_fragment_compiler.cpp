#include "cg_fragment_compiler.h"
#include "private.h"
#include "cg_ir_builder.h"
#include "cg_routines.h"

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schema.h>
#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/core/codegen/module.h>
#include <yt/core/codegen/public.h>

#include <yt/core/logging/log.h>

#include <llvm/IR/Module.h>

// TODO(sandello):
//  - Implement basic logging & profiling within evaluation code
//  - Sometimes we can write through scratch space; some simple cases:
//    * int/double/null expressions only,
//    * string expressions with references (just need to copy string data)
//    It is possible to do better memory management here.
//  - TBAA is a king
//  - Capture pointers by value in ViaClosure

namespace NYT {
namespace NQueryClient {

using namespace NTableClient;
using namespace NConcurrency;

using NCodegen::TCGModule;


////////////////////////////////////////////////////////////////////////////////
// Operator helpers
//

Value* CodegenAllocateRow(TCGIRBuilderPtr& builder, size_t valueCount)
{
    Value* newRowPtr = builder->CreateAlloca(TypeBuilder<TRow, false>::get(builder->getContext()));

    size_t size = sizeof(TUnversionedRowHeader) + sizeof(TUnversionedValue) * valueCount;

    Value* newRowData = builder->CreateAlignedAlloca(
        TypeBuilder<char, false>::get(builder->getContext()),
        8,
        builder->getInt32(size));

    builder->CreateStore(
        builder->CreatePointerCast(newRowData, TypeBuilder<TRowHeader*, false>::get(builder->getContext())),
        builder->CreateConstInBoundsGEP2_32(
            nullptr,
            newRowPtr,
            0,
            TypeBuilder<TRow, false>::Fields::Header));

    Value* newRow = builder->CreateLoad(newRowPtr);

    auto headerPtr = builder->CreateExtractValue(
        newRow,
        TypeBuilder<TRow, false>::Fields::Header);

    builder->CreateStore(
        builder->getInt32(valueCount),
        builder->CreateConstInBoundsGEP2_32(
            nullptr,
            headerPtr,
            0,
            TypeBuilder<TRowHeader, false>::Fields::Count));

    builder->CreateStore(
        builder->getInt32(valueCount),
        builder->CreateConstInBoundsGEP2_32(
            nullptr,
            headerPtr,
            0,
            TypeBuilder<TRowHeader, false>::Fields::Capacity));

    return newRow;
}

void CodegenForEachRow(
    TCGContext& builder,
    Value* rows,
    Value* size,
    const TCodegenConsumer& codegenConsumer)
{
    auto* loopBB = builder->CreateBBHere("loop");
    auto* condBB = builder->CreateBBHere("cond");
    auto* endloopBB = builder->CreateBBHere("endloop");

    // index = 0
    Value* indexPtr = builder->CreateAlloca(builder->getInt64Ty(), nullptr, "indexPtr");
    builder->CreateStore(builder->getInt64(0), indexPtr);

    builder->CreateBr(condBB);

    builder->SetInsertPoint(condBB);

    // if (index != size) ...
    Value* index = builder->CreateLoad(indexPtr, "index");
    Value* condition = builder->CreateICmpNE(index, size);
    builder->CreateCondBr(condition, loopBB, endloopBB);

    builder->SetInsertPoint(loopBB);

    // row = rows[index]; consume(row);
    Value* stackState = builder->CreateStackSave("stackState");
    Value* row = builder->CreateLoad(builder->CreateGEP(rows, index, "rowPtr"), "row");
    codegenConsumer(builder, row);
    builder->CreateStackRestore(stackState);
    // index = index + 1
    builder->CreateStore(builder->CreateAdd(index, builder->getInt64(1)), indexPtr);
    builder->CreateBr(condBB);

    builder->SetInsertPoint(endloopBB);
}

////////////////////////////////////////////////////////////////////////////////
// Expressions
//

Function* CodegenGroupComparerFunction(
    const std::vector<EValueType>& types,
    const TCGModule& module)
{
    return MakeFunction<TComparerFunction>(module.GetModule(), "GroupComparer", [&] (
        TCGIRBuilderPtr& builder,
        Value* lhsRow,
        Value* rhsRow
    ) {
        auto returnIf = [&] (Value* condition) {
            auto* thenBB = builder->CreateBBHere("then");
            auto* elseBB = builder->CreateBBHere("else");
            builder->CreateCondBr(condition, thenBB, elseBB);
            builder->SetInsertPoint(thenBB);
            builder->CreateRet(builder->getInt8(0));
            builder->SetInsertPoint(elseBB);
        };

        auto codegenEqualOp = [&] (size_t index) {
            auto lhsValue = TCGValue::CreateFromRow(
                builder,
                lhsRow,
                index,
                types[index]);

            auto rhsValue = TCGValue::CreateFromRow(
                builder,
                rhsRow,
                index,
                types[index]);

            CodegenIf<TCGIRBuilderPtr>(
                builder,
                builder->CreateOr(lhsValue.IsNull(), rhsValue.IsNull()),
                [&] (TCGIRBuilderPtr& builder) {
                    returnIf(builder->CreateICmpNE(lhsValue.IsNull(), rhsValue.IsNull()));
                },
                [&] (TCGIRBuilderPtr& builder) {
                    auto* lhsData = lhsValue.GetData();
                    auto* rhsData = rhsValue.GetData();

                    switch (types[index]) {
                        case EValueType::Boolean:
                        case EValueType::Int64:
                        case EValueType::Uint64:
                            returnIf(builder->CreateICmpNE(lhsData, rhsData));
                            break;

                        case EValueType::Double:
                            returnIf(builder->CreateFCmpUNE(lhsData, rhsData));
                            break;

                        case EValueType::String: {
                            Value* lhsLength = lhsValue.GetLength();
                            Value* rhsLength = rhsValue.GetLength();

                            Value* minLength = builder->CreateSelect(
                                builder->CreateICmpULT(lhsLength, rhsLength),
                                lhsLength,
                                rhsLength);

                            Value* cmpResult = builder->CreateCall(
                                module.GetRoutine("memcmp"),
                                {
                                    lhsData,
                                    rhsData,
                                    builder->CreateZExt(minLength, builder->getSizeType())
                                });

                            returnIf(builder->CreateOr(
                                builder->CreateICmpNE(cmpResult, builder->getInt32(0)),
                                builder->CreateICmpNE(lhsLength, rhsLength)));
                            break;
                        }

                        default:
                            Y_UNREACHABLE();
                    }
                });
        };

        YCHECK(!types.empty());

        for (size_t index = 0; index < types.size(); ++index) {
            codegenEqualOp(index);
        }

        builder->CreateRet(builder->getInt8(1));
    });
}

Function* CodegenGroupHasherFunction(
    const std::vector<EValueType>& types,
    const TCGModule& module)
{
    return MakeFunction<THasherFunction>(module.GetModule(), "GroupHasher", [&] (
        TCGIRBuilderPtr& builder,
        Value* row
    ) {
        auto codegenHashOp = [&] (size_t index, TCGIRBuilderPtr& builder) -> Value* {
            auto value = TCGValue::CreateFromRow(
                builder,
                row,
                index,
                types[index]);

            auto* conditionBB = builder->CreateBBHere("condition");
            auto* thenBB = builder->CreateBBHere("then");
            auto* elseBB = builder->CreateBBHere("else");
            auto* endBB = builder->CreateBBHere("end");

            builder->CreateBr(conditionBB);

            builder->SetInsertPoint(conditionBB);
            builder->CreateCondBr(value.IsNull(), elseBB, thenBB);
            conditionBB = builder->GetInsertBlock();

            builder->SetInsertPoint(thenBB);

            Value* thenResult;

            switch (value.GetStaticType()) {
                case EValueType::Boolean:
                case EValueType::Int64:
                case EValueType::Uint64:
                    thenResult = builder->CreateCall(
                        module.GetRoutine("FarmHashUint64"),
                        {value.Cast(builder, EValueType::Uint64).GetData()});
                    break;

                case EValueType::Double:
                    thenResult = builder->CreateCall(
                        module.GetRoutine("FarmHashUint64"),
                        {value.Cast(builder, EValueType::Uint64, true).GetData()});
                    break;

                case EValueType::String:
                    thenResult = builder->CreateCall(
                        module.GetRoutine("StringHash"),
                        {
                            value.GetData(),
                            value.GetLength()
                        });
                    break;

                default:
                    Y_UNIMPLEMENTED();
            }

            builder->CreateBr(endBB);
            thenBB = builder->GetInsertBlock();

            builder->SetInsertPoint(elseBB);
            auto* elseResult = builder->getInt64(0);
            builder->CreateBr(endBB);
            elseBB = builder->GetInsertBlock();

            builder->SetInsertPoint(endBB);

            PHINode* result = builder->CreatePHI(thenResult->getType(), 2);
            result->addIncoming(thenResult, thenBB);
            result->addIncoming(elseResult, elseBB);

            return result;
        };

        auto codegenHashCombine = [&] (TCGIRBuilderPtr& builder, Value* first, Value* second) -> Value* {
            //first ^ (second + 0x9e3779b9 + (second << 6) + (second >> 2));
            return builder->CreateXor(
                first,
                builder->CreateAdd(
                    builder->CreateAdd(
                        builder->CreateAdd(second, builder->getInt64(0x9e3779b9)),
                        builder->CreateLShr(second, builder->getInt64(2))),
                    builder->CreateShl(second, builder->getInt64(6))));
        };

        YCHECK(!types.empty());
        Value* result = builder->getInt64(0);
        for (size_t index = 0; index < types.size(); ++index) {
            result = codegenHashCombine(builder, result, codegenHashOp(index, builder));
        }
        builder->CreateRet(result);
    });
}

Function* CodegenTupleComparerFunction(
    const std::vector<std::function<TCGValue(TCGIRBuilderPtr& builder, Value* row)>>& codegenArgs,
    const TCGModule& module,
    const std::vector<bool>& isDesc = std::vector<bool>())
{
    return MakeFunction<TComparerFunction>(module.GetModule(), "RowComparer", [&] (
        TCGIRBuilderPtr& builder,
        Value* lhsRow,
        Value* rhsRow
    ) {
        auto returnIf = [&] (Value* condition, const TCodegenBlock& codegenInner) {
            auto* thenBB = builder->CreateBBHere("then");
            auto* elseBB = builder->CreateBBHere("else");
            builder->CreateCondBr(condition, thenBB, elseBB);
            builder->SetInsertPoint(thenBB);
            builder->CreateRet(builder->CreateSelect(codegenInner(builder), builder->getInt8(1), builder->getInt8(0)));
            builder->SetInsertPoint(elseBB);
        };

        auto codegenEqualOrLessOp = [&] (int index) {
            const auto& codegenArg = codegenArgs[index];
            auto lhsValue = codegenArg(builder, lhsRow);
            auto rhsValue = codegenArg(builder, rhsRow);

            if (index < isDesc.size() && isDesc[index]) {
                std::swap(lhsValue, rhsValue);
            }

            auto type = lhsValue.GetStaticType();

            YCHECK(type == rhsValue.GetStaticType());

            CodegenIf<TCGIRBuilderPtr>(
                builder,
                builder->CreateOr(lhsValue.IsNull(), rhsValue.IsNull()),
                [&] (TCGIRBuilderPtr& builder) {
                    returnIf(
                        builder->CreateICmpNE(lhsValue.IsNull(), rhsValue.IsNull()),
                        [&] (TCGIRBuilderPtr& builder) {
                            return builder->CreateICmpULT(lhsValue.IsNull(), rhsValue.IsNull());
                        });
                },
                [&] (TCGIRBuilderPtr& builder) {
                    auto* lhsData = lhsValue.GetData();
                    auto* rhsData = rhsValue.GetData();

                    switch (type) {
                        case EValueType::Boolean:
                        case EValueType::Int64:
                            returnIf(
                                builder->CreateICmpNE(lhsData, rhsData),
                                [&] (TCGIRBuilderPtr& builder) {
                                    return builder->CreateICmpSLT(lhsData, rhsData);
                                });
                            break;

                        case EValueType::Uint64:
                            returnIf(
                                builder->CreateICmpNE(lhsData, rhsData),
                                [&] (TCGIRBuilderPtr& builder) {
                                    return builder->CreateICmpULT(lhsData, rhsData);
                                });
                            break;

                        case EValueType::Double:
                            returnIf(
                                builder->CreateFCmpUNE(lhsData, rhsData),
                                [&] (TCGIRBuilderPtr& builder) {
                                    return builder->CreateFCmpULT(lhsData, rhsData);
                                });
                            break;

                        case EValueType::String: {
                            Value* lhsLength = lhsValue.GetLength();
                            Value* rhsLength = rhsValue.GetLength();

                            Value* minLength = builder->CreateSelect(
                                builder->CreateICmpULT(lhsLength, rhsLength),
                                lhsLength,
                                rhsLength);

                            Value* cmpResult = builder->CreateCall(
                                module.GetRoutine("memcmp"),
                                {
                                    lhsData,
                                    rhsData,
                                    builder->CreateZExt(minLength, builder->getSizeType())
                                });

                            returnIf(
                                builder->CreateICmpNE(cmpResult, builder->getInt32(0)),
                                [&] (TCGIRBuilderPtr& builder) {
                                    return builder->CreateICmpSLT(cmpResult, builder->getInt32(0));
                                });

                            returnIf(
                                builder->CreateICmpNE(lhsLength, rhsLength),
                                [&] (TCGIRBuilderPtr& builder) {
                                    return builder->CreateICmpULT(lhsLength, rhsLength);
                                });

                            break;
                        }

                        default:
                            Y_UNREACHABLE();
                    }
                });
        };

        YCHECK(!codegenArgs.empty());

        for (int index = 0; index < codegenArgs.size(); ++index) {
            codegenEqualOrLessOp(index);
        }

        builder->CreateRet(builder->getInt8(0));
    });
}

Function* CodegenRowComparerFunction(
    const std::vector<EValueType>& types,
    const TCGModule& module)
{
    std::vector<std::function<TCGValue(TCGIRBuilderPtr& builder, Value* row)>> compareArgs;
    for (int index = 0; index < types.size(); ++index) {
        compareArgs.push_back([index, type = types[index]] (TCGIRBuilderPtr& builder, Value* row) {
            return TCGValue::CreateFromRow(
                builder,
                row,
                index,
                type);
        });
    }

    return CodegenTupleComparerFunction(compareArgs, module);
}

Value* CodegenLexicographicalCompare(
    TCGBaseContext& builder,
    Value* lhsData,
    Value* lhsLength,
    Value* rhsData,
    Value* rhsLength)
{
    Value* lhsLengthIsLess = builder->CreateICmpULT(lhsLength, rhsLength);
    Value* minLength = builder->CreateSelect(
        lhsLengthIsLess,
        lhsLength,
        rhsLength);

    Value* cmpResult = builder->CreateCall(
        builder.Module->GetRoutine("memcmp"),
        {
            lhsData,
            rhsData,
            builder->CreateZExt(minLength, builder->getSizeType())
        });

    return builder->CreateOr(
        builder->CreateICmpSLT(cmpResult, builder->getInt32(0)),
        builder->CreateAnd(
            builder->CreateICmpEQ(cmpResult, builder->getInt32(0)),
            lhsLengthIsLess));
};

TCodegenExpression MakeCodegenLiteralExpr(
    int index,
    EValueType type)
{
    return [
            index,
            type
        ] (TCGExprContext& builder, Value* row) {
            auto valuePtr = builder->CreatePointerCast(
                builder.GetOpaqueValue(index),
                TypeBuilder<TValue*, false>::get(builder->getContext()));

            return TCGValue::CreateFromLlvmValue(
                builder,
                valuePtr,
                type,
                "literal." + Twine(index))
                .Steal();

        };
}

TCodegenExpression MakeCodegenReferenceExpr(
    int index,
    EValueType type,
    Stroka name)
{
    return [
            index,
            type,
            MOVE(name)
        ] (TCGExprContext& builder, Value* row) {
            return TCGValue::CreateFromRow(
                builder,
                row,
                index,
                type,
                "reference." + Twine(name.c_str()));
        };
}

TCodegenValue MakeCodegenFunctionContext(
    int index)
{
    return [
            index
        ] (TCGBaseContext& builder) {
            return builder.GetOpaqueValue(index);
        };
}

TCodegenExpression MakeCodegenUnaryOpExpr(
    EUnaryOp opcode,
    TCodegenExpression codegenOperand,
    EValueType type,
    Stroka name)
{
    return [
        MOVE(opcode),
        MOVE(codegenOperand),
        MOVE(type),
        MOVE(name)
    ] (TCGExprContext& builder, Value* row) {
        auto operandValue = codegenOperand(builder, row);

        return CodegenIf<TCGIRBuilderPtr, TCGValue>(
            builder,
            operandValue.IsNull(),
            [&] (TCGIRBuilderPtr& builder) {
                return TCGValue::CreateNull(builder, type);
            },
            [&] (TCGIRBuilderPtr& builder) {
                auto operandType = operandValue.GetStaticType();
                Value* operandData = operandValue.GetData();
                Value* evalData = nullptr;

                switch(opcode) {
                    case EUnaryOp::Plus:
                        evalData = operandData;
                        break;

                    case EUnaryOp::Minus:
                        switch (operandType) {
                            case EValueType::Int64:
                            case EValueType::Uint64:
                                evalData = builder->CreateSub(builder->getInt64(0), operandData);
                                break;
                            case EValueType::Double:
                                evalData = builder->CreateFSub(ConstantFP::get(builder->getDoubleTy(), 0.0), operandData);
                                break;
                            default:
                                Y_UNREACHABLE();
                        }
                        break;


                    case EUnaryOp::BitNot:
                        evalData = builder->CreateNot(operandData);
                        break;

                    case EUnaryOp::Not:
                        evalData = builder->CreateXor(
                            builder->CreateZExtOrBitCast(
                                builder->getTrue(),
                                TDataTypeBuilder::TBoolean::get(builder->getContext())),
                            operandData);
                        break;

                    default:
                        Y_UNREACHABLE();
                }

                return TCGValue::CreateFromValue(
                    builder,
                    builder->getFalse(),
                    nullptr,
                    evalData,
                    type);
            },
            Twine(name.c_str()));
    };
}

TCodegenExpression MakeCodegenRelationalBinaryOpExpr(
    EBinaryOp opcode,
    TCodegenExpression codegenLhs,
    TCodegenExpression codegenRhs,
    EValueType type,
    Stroka name)
{
    return [
        MOVE(opcode),
        MOVE(codegenLhs),
        MOVE(codegenRhs),
        MOVE(type),
        MOVE(name)
    ] (TCGExprContext& builder, Value* row) {
        auto nameTwine = Twine(name.c_str());
        auto lhsValue = codegenLhs(builder, row);
        auto rhsValue = codegenRhs(builder, row);

        #define CMP_OP(opcode, optype) \
            case EBinaryOp::opcode: \
                evalData = builder->CreateZExtOrBitCast( \
                    builder->Create##optype(lhsData, rhsData), \
                    TDataTypeBuilder::TBoolean::get(builder->getContext())); \
                break;

        auto compareNulls = [&] () {
            Value* lhsData = lhsValue.IsNull();
            Value* rhsData = rhsValue.IsNull();
            Value* evalData = nullptr;

            switch (opcode) {
                CMP_OP(Equal, ICmpEQ)
                CMP_OP(NotEqual, ICmpNE)
                CMP_OP(Less, ICmpSLT)
                CMP_OP(LessOrEqual, ICmpSLE)
                CMP_OP(Greater, ICmpSGT)
                CMP_OP(GreaterOrEqual, ICmpSGE)
                default:
                    Y_UNREACHABLE();
            }

            return TCGValue::CreateFromValue(
                builder,
                builder->getFalse(),
                nullptr,
                evalData,
                type);
        };

        return CodegenIf<TCGBaseContext, TCGValue>(
            builder,
            lhsValue.IsNull(),
            [&] (TCGBaseContext& builder) {

                return compareNulls();
            },
            [&] (TCGBaseContext& builder) {

                return CodegenIf<TCGBaseContext, TCGValue>(
                    builder,
                    rhsValue.IsNull(),
                    [&] (TCGBaseContext& builder) {

                        return compareNulls();
                    },
                    [&] (TCGBaseContext& builder) {

                        YCHECK(lhsValue.GetStaticType() == rhsValue.GetStaticType());
                        auto operandType = lhsValue.GetStaticType();

                        Value* lhsData = lhsValue.GetData();
                        Value* rhsData = rhsValue.GetData();
                        Value* evalData = nullptr;

                        switch (operandType) {

                            case EValueType::Boolean:
                            case EValueType::Int64:
                                switch (opcode) {
                                    CMP_OP(Equal, ICmpEQ)
                                    CMP_OP(NotEqual, ICmpNE)
                                    CMP_OP(Less, ICmpSLT)
                                    CMP_OP(LessOrEqual, ICmpSLE)
                                    CMP_OP(Greater, ICmpSGT)
                                    CMP_OP(GreaterOrEqual, ICmpSGE)
                                    default:
                                        Y_UNREACHABLE();
                                }
                                break;
                            case EValueType::Uint64:
                                switch (opcode) {
                                    CMP_OP(Equal, ICmpEQ)
                                    CMP_OP(NotEqual, ICmpNE)
                                    CMP_OP(Less, ICmpULT)
                                    CMP_OP(LessOrEqual, ICmpULE)
                                    CMP_OP(Greater, ICmpUGT)
                                    CMP_OP(GreaterOrEqual, ICmpUGE)
                                    default:
                                        Y_UNREACHABLE();
                                }
                                break;
                            case EValueType::Double:
                                switch (opcode) {
                                    CMP_OP(Equal, FCmpUEQ)
                                    CMP_OP(NotEqual, FCmpUNE)
                                    CMP_OP(Less, FCmpULT)
                                    CMP_OP(LessOrEqual, FCmpULE)
                                    CMP_OP(Greater, FCmpUGT)
                                    CMP_OP(GreaterOrEqual, FCmpUGE)
                                    default:
                                        Y_UNREACHABLE();
                                }
                                break;
                            case EValueType::String: {
                                Value* lhsLength = lhsValue.GetLength();
                                Value* rhsLength = rhsValue.GetLength();

                                auto codegenEqual = [&] () {
                                    return CodegenIf<TCGBaseContext, Value*>(
                                        builder,
                                        builder->CreateICmpEQ(lhsLength, rhsLength),
                                        [&] (TCGBaseContext& builder) {
                                            Value* minLength = builder->CreateSelect(
                                                builder->CreateICmpULT(lhsLength, rhsLength),
                                                lhsLength,
                                                rhsLength);

                                            Value* cmpResult = builder->CreateCall(
                                                builder.Module->GetRoutine("memcmp"),
                                                {
                                                    lhsData,
                                                    rhsData,
                                                    builder->CreateZExt(minLength, builder->getSizeType())
                                                });

                                            return builder->CreateICmpEQ(cmpResult, builder->getInt32(0));
                                        },
                                        [&] (TCGBaseContext& builder) {
                                            return builder->getFalse();
                                        });
                                };

                                switch (opcode) {
                                    case EBinaryOp::Equal:
                                        evalData = codegenEqual();
                                        break;
                                    case EBinaryOp::NotEqual:
                                        evalData = builder->CreateNot(codegenEqual());
                                        break;
                                    case EBinaryOp::Less:
                                        evalData = CodegenLexicographicalCompare(builder, lhsData, lhsLength, rhsData, rhsLength);
                                        break;
                                    case EBinaryOp::Greater:
                                        evalData = CodegenLexicographicalCompare(builder, rhsData, rhsLength, lhsData, lhsLength);
                                        break;
                                    case EBinaryOp::LessOrEqual:
                                        evalData =  builder->CreateNot(
                                            CodegenLexicographicalCompare(builder, rhsData, rhsLength, lhsData, lhsLength));
                                        break;
                                    case EBinaryOp::GreaterOrEqual:
                                        evalData = builder->CreateNot(
                                            CodegenLexicographicalCompare(builder, lhsData, lhsLength, rhsData, rhsLength));
                                        break;
                                    default:
                                        Y_UNREACHABLE();
                                }

                                evalData = builder->CreateZExtOrBitCast(
                                    evalData,
                                    TDataTypeBuilder::TBoolean::get(builder->getContext()));
                                break;
                            }
                            default:
                                Y_UNREACHABLE();
                        }

                        return TCGValue::CreateFromValue(
                            builder,
                            builder->getFalse(),
                            nullptr,
                            evalData,
                            type);
                    });
            },
            nameTwine);

            #undef CMP_OP
    };
}

TCodegenExpression MakeCodegenArithmeticBinaryOpExpr(
    EBinaryOp opcode,
    TCodegenExpression codegenLhs,
    TCodegenExpression codegenRhs,
    EValueType type,
    Stroka name)
{
    return [
        MOVE(opcode),
        MOVE(codegenLhs),
        MOVE(codegenRhs),
        MOVE(type),
        MOVE(name)
    ] (TCGExprContext& builder, Value* row) {
        auto nameTwine = Twine(name.c_str());

        auto lhsValue = codegenLhs(builder, row);

        return CodegenIf<TCGExprContext, TCGValue>(
            builder,
            lhsValue.IsNull(),
            [&] (TCGExprContext& builder) {
                return TCGValue::CreateNull(builder, type);
            },
            [&] (TCGExprContext& builder) {
                auto rhsValue = codegenRhs(builder, row);

                return CodegenIf<TCGBaseContext, TCGValue>(
                    builder,
                    rhsValue.IsNull(),
                    [&] (TCGBaseContext& builder) {
                        return TCGValue::CreateNull(builder, type);
                    },
                    [&] (TCGBaseContext& builder) {
                        YCHECK(lhsValue.GetStaticType() == rhsValue.GetStaticType());
                        auto operandType = lhsValue.GetStaticType();

                        Value* lhsData = lhsValue.GetData();
                        Value* rhsData = rhsValue.GetData();
                        Value* evalData = nullptr;

                        #define OP(opcode, optype) \
                            case EBinaryOp::opcode: \
                                evalData = builder->Create##optype(lhsData, rhsData); \
                                break;


                        auto checkZero = [&] (Value* value) {
                            CodegenIf<TCGBaseContext>(
                                builder,
                                builder->CreateIsNull(value),
                                [] (TCGBaseContext& builder) {
                                    builder->CreateCall(
                                        builder.Module->GetRoutine("ThrowQueryException"),
                                        {
                                            builder->CreateGlobalStringPtr("Division by zero")
                                        });
                                });
                        };

                        #define OP_ZERO_CHECKED(opcode, optype) \
                            case EBinaryOp::opcode: \
                                checkZero(rhsData); \
                                evalData = builder->Create##optype(lhsData, rhsData); \
                                break;

                        switch (operandType) {

                            case EValueType::Boolean:
                            case EValueType::Int64:
                                switch (opcode) {
                                    OP(Plus, Add)
                                    OP(Minus, Sub)
                                    OP(Multiply, Mul)
                                    OP_ZERO_CHECKED(Divide, SDiv)
                                    OP_ZERO_CHECKED(Modulo, SRem)
                                    OP(BitAnd, And)
                                    OP(BitOr, Or)
                                    OP(And, And)
                                    OP(Or, Or)
                                    OP(LeftShift, Shl)
                                    OP(RightShift, LShr)
                                    default:
                                        Y_UNREACHABLE();
                                }
                                break;
                            case EValueType::Uint64:
                                switch (opcode) {
                                    OP(Plus, Add)
                                    OP(Minus, Sub)
                                    OP(Multiply, Mul)
                                    OP_ZERO_CHECKED(Divide, UDiv)
                                    OP_ZERO_CHECKED(Modulo, URem)
                                    OP(BitAnd, And)
                                    OP(BitOr, Or)
                                    OP(And, And)
                                    OP(Or, Or)
                                    OP(LeftShift, Shl)
                                    OP(RightShift, LShr)
                                    default:
                                        Y_UNREACHABLE();
                                }
                                break;
                            case EValueType::Double:
                                switch (opcode) {
                                    OP(Plus, FAdd)
                                    OP(Minus, FSub)
                                    OP(Multiply, FMul)
                                    OP(Divide, FDiv)
                                    default:
                                        Y_UNREACHABLE();
                                }
                                break;
                            default:
                                Y_UNREACHABLE();
                        }

                        #undef OP

                        return TCGValue::CreateFromValue(
                            builder,
                            builder->getFalse(),
                            nullptr,
                            evalData,
                            type);
                    });
           },
            nameTwine);
    };
}

TCodegenExpression MakeCodegenBinaryOpExpr(
    EBinaryOp opcode,
    TCodegenExpression codegenLhs,
    TCodegenExpression codegenRhs,
    EValueType type,
    Stroka name)
{
    if (IsRelationalBinaryOp(opcode)) {
        return MakeCodegenRelationalBinaryOpExpr(
            opcode,
            std::move(codegenLhs),
            std::move(codegenRhs),
            type,
            std::move(name));
    } else {
        return MakeCodegenArithmeticBinaryOpExpr(
            opcode,
            std::move(codegenLhs),
            std::move(codegenRhs),
            type,
            std::move(name));
    }
}

TCodegenExpression MakeCodegenInOpExpr(
    std::vector<TCodegenExpression> codegenArgs,
    int arrayIndex)
{
    return [
        MOVE(codegenArgs),
        MOVE(arrayIndex)
    ] (TCGExprContext& builder, Value* row) {
        size_t keySize = codegenArgs.size();

        Value* newRow = CodegenAllocateRow(builder, keySize);

        std::vector<EValueType> keyTypes;
        for (int index = 0; index < keySize; ++index) {
            auto id = index;
            auto value = codegenArgs[index](builder, row);
            keyTypes.push_back(value.GetStaticType());
            value.StoreToRow(builder, newRow, index, id);
        }

        Value* result = builder->CreateCall(
            builder.Module->GetRoutine("IsRowInArray"),
            {
                CodegenRowComparerFunction(keyTypes, *builder.Module),
                newRow,
                builder.GetOpaqueValue(arrayIndex)
            });

        return TCGValue::CreateFromValue(
            builder,
            builder->getFalse(),
            nullptr,
            result,
            EValueType::Boolean);
    };
}

////////////////////////////////////////////////////////////////////////////////
// Operators
//

void CodegenScanOp(
    TCGOperatorContext& builder,
    const TCodegenConsumer& codegenConsumer)
{
    auto consume = MakeClosure<void(TRowBuffer*, TRow*, i64)>(builder, "ScanOpInner", [&] (
        TCGOperatorContext& builder,
        Value* buffer,
        Value* rows,
        Value* size
    ) {
        TCGContext innerBulder(builder, buffer);
        CodegenForEachRow(innerBulder, rows, size, codegenConsumer);
        innerBulder->CreateRetVoid();
    });

    builder->CreateCall(
        builder.Module->GetRoutine("ScanOpHelper"),
        {
            builder.GetExecutionContext(),
            consume.ClosurePtr,
            consume.Function
        });
}

TCodegenSource MakeCodegenJoinOp(
    int index,
    std::vector<std::pair<TCodegenExpression, bool>> equations,
    TCodegenSource codegenSource)
{
    return [
        index,
        MOVE(equations),
        codegenSource = std::move(codegenSource)
    ] (TCGOperatorContext& builder, const TCodegenConsumer& codegenConsumer) {
        int lookupKeySize = equations.size();
        std::vector<EValueType> lookupKeyTypes(lookupKeySize);

        auto collectRows = MakeClosure<void(TJoinClosure*, TRowBuffer*)>(builder, "CollectRows", [&] (
            TCGOperatorContext& builder,
            Value* joinClosure,
            Value* buffer
        ) {
            Value* keyPtr = builder->CreateAlloca(TypeBuilder<TRow, false>::get(builder->getContext()));
            builder->CreateCall(
                builder.Module->GetRoutine("AllocatePermanentRow"),
                {
                    builder.GetExecutionContext(),
                    buffer,
                    builder->getInt32(lookupKeySize),
                    keyPtr
                });

            codegenSource(
                builder,
                [&] (TCGContext& builder, Value* row) {
                    Value* bufferRef = builder->ViaClosure(buffer);
                    Value* keyPtrRef = builder->ViaClosure(keyPtr);
                    Value* keyRef = builder->CreateLoad(keyPtrRef);

                    for (int column = 0; column < lookupKeySize; ++column) {
                        if (!equations[column].second) {
                            auto joinKeyValue = equations[column].first(builder, row);
                            lookupKeyTypes[column] = joinKeyValue.GetStaticType();
                            joinKeyValue.StoreToRow(builder, keyRef, column, column);
                        }
                    }

                    for (int column = 0; column < lookupKeySize; ++column) {
                        if (equations[column].second) {
                            auto evaluatedColumn = equations[column].first(builder, keyRef);
                            lookupKeyTypes[column] = evaluatedColumn.GetStaticType();
                            evaluatedColumn.StoreToRow(builder, keyRef, column, column);
                        }
                    }

                    Value* joinClosureRef = builder->ViaClosure(joinClosure);

                    builder->CreateCall(
                        builder.Module->GetRoutine("InsertJoinRow"),
                        {
                            builder.GetExecutionContext(),
                            bufferRef,
                            joinClosureRef,
                            keyPtrRef,
                            row
                        });
                });

            builder->CreateRetVoid();
        });


        auto consumeJoinedRows = MakeClosure<void(TRowBuffer*, TRow*, i64)>(builder, "ConsumeJoinedRows", [&] (
            TCGOperatorContext& builder,
            Value* buffer,
            Value* joinedRows,
            Value* size
        ) {
            TCGContext innerBuilder(builder, buffer);
            CodegenForEachRow(
                innerBuilder,
                joinedRows,
                size,
                codegenConsumer);

            innerBuilder->CreateRetVoid();
        });

        builder->CreateCall(
            builder.Module->GetRoutine("JoinOpHelper"),
            {
                builder.GetExecutionContext(),
                builder.GetOpaqueValue(index),

                CodegenGroupHasherFunction(lookupKeyTypes, *builder.Module),
                CodegenGroupComparerFunction(lookupKeyTypes, *builder.Module),
                CodegenRowComparerFunction(lookupKeyTypes, *builder.Module),
                builder->getInt32(lookupKeySize),

                collectRows.ClosurePtr,
                collectRows.Function,

                consumeJoinedRows.ClosurePtr,
                consumeJoinedRows.Function
            });
    };
}

TCodegenSource MakeCodegenFilterOp(
    TCodegenExpression codegenPredicate,
    TCodegenSource codegenSource)
{
    return [
        MOVE(codegenPredicate),
        codegenSource = std::move(codegenSource)
    ] (TCGOperatorContext& builder, const TCodegenConsumer& codegenConsumer) {
        codegenSource(
            builder,
            [&] (TCGContext& builder, Value* row) {
                auto predicateResult = codegenPredicate(builder, row);

                Value* result = builder->CreateZExtOrBitCast(
                    predicateResult.GetData(),
                    builder->getInt64Ty());

                auto* ifBB = builder->CreateBBHere("if");
                auto* endifBB = builder->CreateBBHere("endif");

                builder->CreateCondBr(
                    builder->CreateICmpNE(result, builder->getInt64(0)),
                    ifBB,
                    endifBB);

                builder->SetInsertPoint(ifBB);
                codegenConsumer(builder, row);
                builder->CreateBr(endifBB);

                builder->SetInsertPoint(endifBB);
            });
    };
}

TCodegenSource MakeCodegenProjectOp(
    std::vector<TCodegenExpression> codegenArgs,
    TCodegenSource codegenSource)
{
    return [
        MOVE(codegenArgs),
        codegenSource = std::move(codegenSource)
    ] (TCGOperatorContext& builder, const TCodegenConsumer& codegenConsumer) {
        int projectionCount = codegenArgs.size();

        Value* newRow = CodegenAllocateRow(builder, projectionCount);

        codegenSource(
            builder,
            [&] (TCGContext& builder, Value* row) {
                Value* newRowRef = builder->ViaClosure(newRow);

                for (int index = 0; index < projectionCount; ++index) {
                    auto id = index;

                    codegenArgs[index](builder, row)
                        .StoreToRow(builder, newRowRef, index, id);
                }

                codegenConsumer(builder, newRowRef);
            });
    };
}

std::function<void(TCGContext&, Value*, Value*)> MakeCodegenEvaluateGroups(
    std::vector<TCodegenExpression> codegenGroupExprs,
    std::vector<EValueType> nullTypes)
{
    return [
        MOVE(codegenGroupExprs),
        MOVE(nullTypes)
    ] (TCGContext& builder, Value* srcRow, Value* dstRow) {
        for (int index = 0; index < codegenGroupExprs.size(); index++) {
            auto value = codegenGroupExprs[index](builder, srcRow);
            value.StoreToRow(builder, dstRow, index, index);
        }

        size_t offset = codegenGroupExprs.size();
        for (int index = 0; index < nullTypes.size(); ++index) {
            TCGValue::CreateNull(builder, nullTypes[index])
                .StoreToRow(builder, dstRow, offset + index, offset + index);
        }
    };
}

std::function<void(TCGContext&, Value*, Value*)> MakeCodegenEvaluateAggregateArgs(
    size_t keySize,
    std::vector<TCodegenExpression> codegenAggregateExprs)
{
    return [
        keySize,
        MOVE(codegenAggregateExprs)
    ] (TCGContext& builder, Value* srcRow, Value* dstRow) {
        for (int index = 0; index < codegenAggregateExprs.size(); index++) {
            auto id = keySize + index;
            auto value = codegenAggregateExprs[index](builder, srcRow);
            value.StoreToRow(builder, dstRow, keySize + index, id);
        }
    };
}

std::function<void(TCGContext& builder, Value* row)> MakeCodegenAggregateInitialize(
    std::vector<TCodegenAggregate> codegenAggregates,
    int keySize)
{
    return [
        MOVE(codegenAggregates),
        keySize
    ] (TCGContext& builder, Value* row) {
        for (int index = 0; index < codegenAggregates.size(); index++) {
            auto id = keySize + index;
            auto initState = codegenAggregates[index].Initialize(
                builder,
                row);
            initState.StoreToRow(
                builder,
                row,
                keySize + index,
                id);
        }
    };
}

std::function<void(TCGContext& builder, Value*, Value*)> MakeCodegenAggregateUpdate(
    std::vector<TCodegenAggregate> codegenAggregates,
    int keySize,
    bool isMerge)
{
    return [
        MOVE(codegenAggregates),
        keySize,
        isMerge
    ] (TCGContext& builder, Value* newRow, Value* groupRow) {
        for (int index = 0; index < codegenAggregates.size(); index++) {
            auto aggState = builder->CreateConstInBoundsGEP1_32(
                nullptr,
                CodegenValuesPtrFromRow(builder, groupRow),
                keySize + index);
            auto newValue = builder->CreateConstInBoundsGEP1_32(
                nullptr,
                CodegenValuesPtrFromRow(builder, newRow),
                keySize + index);

            auto id = keySize + index;
            TCodegenAggregateUpdate updateFunction;
            if (isMerge) {
                updateFunction = codegenAggregates[index].Merge;
            } else {
                updateFunction = codegenAggregates[index].Update;
            }
            updateFunction(
                builder,
                aggState,
                newValue)
                .StoreToRow(
                    builder,
                    groupRow,
                    keySize + index,
                    id);
        }
    };
}

std::function<void(TCGContext& builder, Value* row)> MakeCodegenAggregateFinalize(
    std::vector<TCodegenAggregate> codegenAggregates,
    int keySize,
    bool isFinal)
{
    return [
        MOVE(codegenAggregates),
        keySize,
        isFinal
    ] (TCGContext& builder, Value* row) {
        if (!isFinal) {
            return;
        }
        for (int index = 0; index < codegenAggregates.size(); index++) {
            auto id = keySize + index;
            auto valuesPtr = CodegenValuesPtrFromRow(builder, row);
            auto resultValue = codegenAggregates[index].Finalize(
                builder,
                builder->CreateConstInBoundsGEP1_32(
                    nullptr,
                    valuesPtr,
                    keySize + index));
            resultValue.StoreToRow(
                builder,
                row,
                keySize + index,
                id);
        }
    };
}

TCodegenSource MakeCodegenGroupOp(
    std::function<void(TCGContext&, Value*)> codegenInitialize,
    std::function<void(TCGContext&, Value*, Value*)> codegenEvaluateGroups,
    std::function<void(TCGContext&, Value*, Value*)> codegenEvaluateAggregateArgs,
    std::function<void(TCGContext&, Value*, Value*)> codegenUpdate,
    std::function<void(TCGContext&, Value*)> codegenFinalize,
    TCodegenSource codegenSource,
    std::vector<EValueType> keyTypes,
    bool isMerge,
    int groupRowSize,
    bool appendToSource,
    bool checkNulls)
{
    // codegenInitialize calls the aggregates' initialisation functions
    // codegenEvaluateGroups evaluates the group expressions
    // codegenEvaluateAggregateArgs evaluates the aggregates' arguments
    // codegenUpdate calls the aggregates' update or merge functions
    // codegenFinalize calls the aggregates' finalize functions if needed
    return [
        MOVE(codegenInitialize),
        MOVE(codegenEvaluateGroups),
        MOVE(codegenEvaluateAggregateArgs),
        MOVE(codegenUpdate),
        MOVE(codegenFinalize),
        MOVE(codegenSource),
        MOVE(keyTypes),
        isMerge,
        groupRowSize,
        appendToSource,
        checkNulls
    ] (TCGOperatorContext& builder, const TCodegenConsumer& codegenConsumer) {
        auto collect = MakeClosure<void(TGroupByClosure*, TRowBuffer*)>(builder, "CollectGroups", [&] (
            TCGOperatorContext& builder,
            Value* groupByClosure,
            Value* buffer
        ) {
            Value* newRowPtr = builder->CreateAlloca(TypeBuilder<TRow, false>::get(builder->getContext()));

            builder->CreateCall(
                builder.Module->GetRoutine("AllocatePermanentRow"),
                {
                    builder.GetExecutionContext(),
                    buffer,
                    builder->getInt32(groupRowSize),
                    newRowPtr
                });

            codegenSource(
                builder,
                [&] (TCGContext& builder, Value* row) {
                    if (appendToSource) {
                        codegenConsumer(builder, row);
                    }

                    Value* bufferRef = builder->ViaClosure(buffer);
                    Value* newRowPtrRef = builder->ViaClosure(newRowPtr);
                    Value* newRowRef = builder->CreateLoad(newRowPtrRef);

                    codegenEvaluateGroups(builder, row, newRowRef);

                    Value* groupByClosureRef = builder->ViaClosure(groupByClosure);

                    auto groupRowPtr = builder->CreateCall(
                        builder.Module->GetRoutine("InsertGroupRow"),
                        {
                            builder.GetExecutionContext(),
                            bufferRef,
                            groupByClosureRef,
                            newRowRef
                        });

                    auto groupRow = builder->CreateLoad(groupRowPtr);

                    auto inserted = builder->CreateICmpEQ(
                        builder->CreateExtractValue(
                            groupRow,
                            TypeBuilder<TRow, false>::Fields::Header),
                        builder->CreateExtractValue(
                            newRowRef,
                            TypeBuilder<TRow, false>::Fields::Header));

                    TCGContext innerBuilder(builder, bufferRef);

                    CodegenIf<TCGContext>(
                        innerBuilder,
                        inserted,
                        [&] (TCGContext& builder) {
                            codegenInitialize(builder, groupRow);

                            builder->CreateCall(
                                builder.Module->GetRoutine("AllocatePermanentRow"),
                                {
                                    builder.GetExecutionContext(),
                                    bufferRef,
                                    builder->getInt32(groupRowSize),
                                    newRowPtrRef
                                });
                        });

                    // Here *newRowPtrRef != groupRow.
                    if (!isMerge) {
                        auto newRow = builder->CreateLoad(newRowPtrRef);
                        codegenEvaluateAggregateArgs(builder, row, newRow);
                        codegenUpdate(innerBuilder, newRow, groupRow);
                    } else {
                        codegenUpdate(innerBuilder, row, groupRow);
                    }

                });

            builder->CreateRetVoid();
        });

        auto consume = MakeClosure<void(TRowBuffer*, TRow*, i64)>(builder, "Consume", [&] (
            TCGOperatorContext& builder,
            Value* buffer,
            Value* finalGroupedRows,
            Value* size
        ) {
            auto codegenFinalizingConsumer = [
                MOVE(codegenConsumer),
                MOVE(codegenFinalize)
            ] (TCGContext& builder, Value* row) {
                codegenFinalize(builder, row);
                codegenConsumer(builder, row);
            };

            TCGContext innerBuilder(builder, buffer);
            CodegenForEachRow(
                innerBuilder,
                finalGroupedRows,
                size,
                codegenFinalizingConsumer);

            innerBuilder->CreateRetVoid();
        });

        builder->CreateCall(
            builder.Module->GetRoutine("GroupOpHelper"),
            {
                builder.GetExecutionContext(),
                CodegenGroupHasherFunction(keyTypes, *builder.Module),
                CodegenGroupComparerFunction(keyTypes, *builder.Module),
                builder->getInt32(keyTypes.size()),
                builder->getInt8(checkNulls),

                collect.ClosurePtr,
                collect.Function,

                consume.ClosurePtr,
                consume.Function,
            });

    };
}

TCodegenSource MakeCodegenOrderOp(
    std::vector<TCodegenExpression> codegenExprs,
    std::vector<EValueType> sourceSchema,
    TCodegenSource codegenSource,
    const std::vector<bool>& isDesc)
{
    return [
        isDesc,
        MOVE(codegenExprs),
        MOVE(sourceSchema),
        codegenSource = std::move(codegenSource)
    ] (TCGOperatorContext& builder, const TCodegenConsumer& codegenConsumer) {
        auto schemaSize = sourceSchema.size();
        std::vector<EValueType> orderColumnTypes;

        auto collectRows = MakeClosure<void(TTopCollector*)>(builder, "CollectRows", [&] (
            TCGOperatorContext& builder,
            Value* topCollector
        ) {
            Value* newRow = CodegenAllocateRow(builder, schemaSize + codegenExprs.size());

            codegenSource(
                builder,
                [&] (TCGContext& builder, Value* row) {
                    Value* topCollectorRef = builder->ViaClosure(topCollector);
                    Value* newRowRef = builder->ViaClosure(newRow);

                    for (size_t index = 0; index < schemaSize; ++index) {
                        auto type = sourceSchema[index];
                        TCGValue::CreateFromRow(
                            builder,
                            row,
                            index,
                            type)
                            .StoreToRow(builder, newRowRef, index, index);
                    }

                    for (size_t index = 0; index < codegenExprs.size(); ++index) {
                        auto columnIndex = schemaSize + index;

                        auto orderValue = codegenExprs[index](builder, row);
                        orderColumnTypes.push_back(orderValue.GetStaticType());

                        orderValue.StoreToRow(builder, newRowRef, columnIndex, columnIndex);
                    }

                    builder->CreateCall(
                        builder.Module->GetRoutine("AddRow"),
                        {topCollectorRef, newRowRef});
                });

            builder->CreateRetVoid();
        });

        auto consumeOrderedRows = MakeClosure<void(TRowBuffer*, TRow*, i64)>(builder, "ConsumeOrderedRows", [&] (
            TCGOperatorContext& builder,
            Value* buffer,
            Value* orderedRows,
            Value* size
        ) {
            TCGContext innerBuilder(builder, buffer);
            CodegenForEachRow(
                innerBuilder,
                orderedRows,
                size,
                codegenConsumer);

            builder->CreateRetVoid();
        });

        std::vector<std::function<TCGValue(TCGIRBuilderPtr& builder, Value* row)>> compareArgs;
        for (int index = 0; index < codegenExprs.size(); ++index) {
            auto columnIndex = schemaSize + index;
            auto type = orderColumnTypes[index];

            compareArgs.push_back([columnIndex, type] (TCGIRBuilderPtr& builder, Value* row) {
                return TCGValue::CreateFromRow(
                    builder,
                    row,
                    columnIndex,
                    type);
            });
        }

        builder->CreateCall(
            builder.Module->GetRoutine("OrderOpHelper"),
            {
                builder.GetExecutionContext(),
                CodegenTupleComparerFunction(compareArgs, *builder.Module, isDesc),

                collectRows.ClosurePtr,
                collectRows.Function,

                consumeOrderedRows.ClosurePtr,
                consumeOrderedRows.Function,
                builder->getInt32(schemaSize)
            });
    };
}

////////////////////////////////////////////////////////////////////////////////

TCGQueryCallback CodegenEvaluate(TCodegenSource codegenSource, size_t opaqueValuesCount)
{
    auto module = TCGModule::Create(GetQueryRoutineRegistry());
    const auto entryFunctionName = Stroka("EvaluateQuery");

    MakeFunction<TCGQuerySignature>(module->GetModule(), entryFunctionName.c_str(), [&] (
        TCGIRBuilderPtr& baseBuilder,
        Value* opaqueValuesPtr,
        Value* executionContextPtr
    ) {
        auto opaqueValues = MakeOpaqueValues(baseBuilder, opaqueValuesPtr, opaqueValuesCount);
        TCGOperatorContext builder(TCGBaseContext(baseBuilder, &opaqueValues, module), executionContextPtr);

        auto collect = MakeClosure<void(TWriteOpClosure*)>(builder, "WriteOpInner", [&] (
            TCGOperatorContext& builder,
            Value* writeRowClosure
        ) {
            codegenSource(
                builder,
                [&] (TCGContext& builder, Value* row) {
                    Value* writeRowClosureRef = builder->ViaClosure(writeRowClosure);
                    builder->CreateCall(
                        module->GetRoutine("WriteRow"),
                        {builder.GetExecutionContext(), writeRowClosureRef, row});
                });

            builder->CreateRetVoid();
        });

        builder->CreateCall(
            builder.Module->GetRoutine("WriteOpHelper"),
            {
                builder.GetExecutionContext(),
                collect.ClosurePtr,
                collect.Function
            });

        builder->CreateRetVoid();
    });

    module->ExportSymbol(entryFunctionName);
    return module->GetCompiledFunction<TCGQuerySignature>(entryFunctionName);
}

TCGExpressionCallback CodegenExpression(TCodegenExpression codegenExpression, size_t opaqueValuesCount)
{
    auto module = TCGModule::Create(GetQueryRoutineRegistry());
    const auto entryFunctionName = Stroka("EvaluateExpression");

    MakeFunction<TCGExpressionSignature>(module->GetModule(), entryFunctionName.c_str(), [&] (
        TCGIRBuilderPtr& baseBuilder,
        Value* opaqueValuesPtr,
        Value* resultPtr,
        Value* inputRow,
        Value* buffer
    ) {
        auto opaqueValues = MakeOpaqueValues(baseBuilder, opaqueValuesPtr, opaqueValuesCount);
        TCGExprContext builder(TCGBaseContext(baseBuilder, &opaqueValues, module), buffer);

        auto result = codegenExpression(builder, inputRow);
        result.StoreToValue(builder, resultPtr, 0, "writeResult");
        builder->CreateRetVoid();
    });

    module->ExportSymbol(entryFunctionName);

    return module->GetCompiledFunction<TCGExpressionSignature>(entryFunctionName);
}

TCGAggregateCallbacks CodegenAggregate(TCodegenAggregate codegenAggregate)
{
    auto module = TCGModule::Create(GetQueryRoutineRegistry());

    const auto initName = Stroka("init");
    {
        MakeFunction<TCGAggregateInitSignature>(module->GetModule(), initName.c_str(), [&] (
            TCGIRBuilderPtr& baseBuilder,
            Value* buffer,
            Value* resultPtr
        ) {
            TCGExprContext builder(TCGBaseContext(baseBuilder, nullptr, module), buffer);

            auto result = codegenAggregate.Initialize(builder, nullptr);
            result.StoreToValue(builder, resultPtr, 0, "writeResult");
            builder->CreateRetVoid();
        });

        module->ExportSymbol(initName);
    }

    const auto updateName = Stroka("update");
    {
        MakeFunction<TCGAggregateUpdateSignature>(module->GetModule(), updateName.c_str(), [&] (
            TCGIRBuilderPtr& baseBuilder,
            Value* buffer,
            Value* resultPtr,
            Value* statePtr,
            Value* newValuePtr
        ) {
            TCGExprContext builder(TCGBaseContext(baseBuilder, nullptr, module), buffer);

            auto result = codegenAggregate.Update(builder, statePtr, newValuePtr);
            result.StoreToValue(builder, resultPtr, 0, "writeResult");
            builder->CreateRetVoid();
        });

        module->ExportSymbol(updateName);
    }

    const auto mergeName = Stroka("merge");
    {
        MakeFunction<TCGAggregateMergeSignature>(module->GetModule(), mergeName.c_str(), [&] (
            TCGIRBuilderPtr& baseBuilder,
            Value* buffer,
            Value* resultPtr,
            Value* dstStatePtr,
            Value* statePtr
        ) {
            TCGExprContext builder(TCGBaseContext(baseBuilder, nullptr, module), buffer);

            auto result = codegenAggregate.Merge(builder, dstStatePtr, statePtr);
            result.StoreToValue(builder, resultPtr, 0, "writeResult");
            builder->CreateRetVoid();
        });

        module->ExportSymbol(mergeName);
    }

    const auto finalizeName = Stroka("finalize");
    {
        MakeFunction<TCGAggregateFinalizeSignature>(module->GetModule(), finalizeName.c_str(), [&] (
            TCGIRBuilderPtr& baseBuilder,
            Value* buffer,
            Value* resultPtr,
            Value* statePtr
        ) {
            TCGExprContext builder(TCGBaseContext(baseBuilder, nullptr, module), buffer);

            auto result = codegenAggregate.Finalize(builder, statePtr);
            result.StoreToValue(builder, resultPtr, 0, "writeResult");
            builder->CreateRetVoid();
        });

        module->ExportSymbol(finalizeName);
    }

    return TCGAggregateCallbacks{
        module->GetCompiledFunction<TCGAggregateInitSignature>(initName),
        module->GetCompiledFunction<TCGAggregateUpdateSignature>(updateName),
        module->GetCompiledFunction<TCGAggregateMergeSignature>(mergeName),
        module->GetCompiledFunction<TCGAggregateFinalizeSignature>(finalizeName)};
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

