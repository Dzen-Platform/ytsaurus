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

#include <yt/core/misc/common.h>

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

void CodegenForEachRow(
    TCGContext& builder,
    Value* rows,
    Value* size,
    Value* stopFlag,
    const TCodegenConsumer& codegenConsumer)
{
    auto* loopBB = builder.CreateBBHere("loop");
    auto* condBB = builder.CreateBBHere("cond");
    auto* endloopBB = builder.CreateBBHere("endloop");

    // index = 0
    Value* indexPtr = builder.CreateAlloca(builder.getInt32Ty(), nullptr, "indexPtr");
    builder.CreateStore(builder.getInt32(0), indexPtr);

    builder.CreateBr(condBB);

    builder.SetInsertPoint(condBB);

    // if (index != size) ...
    Value* index = builder.CreateLoad(indexPtr, "index");
    Value* condition = builder.CreateAnd(
        builder.CreateICmpNE(index, size),
        builder.CreateICmpEQ(
            builder.CreateLoad(stopFlag, "stopFlag"),
            builder.getInt8(0)));
    builder.CreateCondBr(condition, loopBB, endloopBB);

    builder.SetInsertPoint(loopBB);

    // row = rows[index]; consume(row);
    Value* stackState = builder.CreateStackSave("stackState");
    Value* row = builder.CreateLoad(builder.CreateGEP(rows, index, "rowPtr"), "row");
    codegenConsumer(builder, row);
    builder.CreateStackRestore(stackState);
    // index = index + 1
    builder.CreateStore(builder.CreateAdd(index, builder.getInt32(1)), indexPtr);
    builder.CreateBr(condBB);

    builder.SetInsertPoint(endloopBB);
}

////////////////////////////////////////////////////////////////////////////////
// Expressions
//

Function* CodegenGroupComparerFunction(
    const std::vector<EValueType>& types,
    const TCGModule& module)
{
    return MakeFunction<TComparerFunction>(module.GetModule(), "GroupComparer", [&] (
        TCGIRBuilder& builder,
        Value* lhsRow,
        Value* rhsRow
    ) {
        auto returnIf = [&] (Value* condition) {
            auto* thenBB = builder.CreateBBHere("then");
            auto* elseBB = builder.CreateBBHere("else");
            builder.CreateCondBr(condition, thenBB, elseBB);
            builder.SetInsertPoint(thenBB);
            builder.CreateRet(builder.getInt8(0));
            builder.SetInsertPoint(elseBB);
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

            CodegenIf<TCGIRBuilder>(
                builder,
                builder.CreateOr(lhsValue.IsNull(), rhsValue.IsNull()),
                [&] (TCGIRBuilder& builder) {
                    returnIf(builder.CreateICmpNE(lhsValue.IsNull(), rhsValue.IsNull()));
                },
                [&] (TCGIRBuilder& builder) {
                    auto* lhsData = lhsValue.GetData();
                    auto* rhsData = rhsValue.GetData();

                    switch (types[index]) {
                        case EValueType::Boolean:
                        case EValueType::Int64:
                        case EValueType::Uint64:
                            returnIf(builder.CreateICmpNE(lhsData, rhsData));
                            break;

                        case EValueType::Double:
                            returnIf(builder.CreateFCmpUNE(lhsData, rhsData));
                            break;

                        case EValueType::String: {
                            Value* lhsLength = lhsValue.GetLength();
                            Value* rhsLength = rhsValue.GetLength();

                            Value* minLength = builder.CreateSelect(
                                builder.CreateICmpULT(lhsLength, rhsLength),
                                lhsLength,
                                rhsLength);

                            Value* cmpResult = builder.CreateCall(
                                module.GetRoutine("memcmp"),
                                {
                                    lhsData,
                                    rhsData,
                                    builder.CreateZExt(minLength, builder.getSizeType())
                                });

                            returnIf(builder.CreateOr(
                                builder.CreateICmpNE(cmpResult, builder.getInt32(0)),
                                builder.CreateICmpNE(lhsLength, rhsLength)));
                            break;
                        }

                        default:
                            YUNREACHABLE();
                    }
                });
        };

        YCHECK(!types.empty());

        for (size_t index = 0; index < types.size(); ++index) {
            codegenEqualOp(index);
        }

        builder.CreateRet(builder.getInt8(1));
    });
}

Function* CodegenGroupHasherFunction(
    const std::vector<EValueType>& types,
    const TCGModule& module)
{
    return MakeFunction<THasherFunction>(module.GetModule(), "GroupHasher", [&] (
        TCGIRBuilder& builder,
        Value* row
    ) {
        auto codegenHashOp = [&] (size_t index, TCGIRBuilder& builder) -> Value* {
            auto value = TCGValue::CreateFromRow(
                builder,
                row,
                index,
                types[index]);

            auto* conditionBB = builder.CreateBBHere("condition");
            auto* thenBB = builder.CreateBBHere("then");
            auto* elseBB = builder.CreateBBHere("else");
            auto* endBB = builder.CreateBBHere("end");

            builder.CreateBr(conditionBB);

            builder.SetInsertPoint(conditionBB);
            builder.CreateCondBr(value.IsNull(), elseBB, thenBB);
            conditionBB = builder.GetInsertBlock();

            builder.SetInsertPoint(thenBB);

            Value* thenResult;

            switch (value.GetStaticType()) {
                case EValueType::Boolean:
                case EValueType::Int64:
                case EValueType::Uint64:
                    thenResult = builder.CreateCall(
                        module.GetRoutine("FarmHashUint64"),
                        {value.Cast(builder, EValueType::Uint64).GetData()});
                    break;

                case EValueType::Double:
                    thenResult = builder.CreateCall(
                        module.GetRoutine("FarmHashUint64"),
                        {value.Cast(builder, EValueType::Uint64, true).GetData()});
                    break;

                case EValueType::String:
                    thenResult = builder.CreateCall(
                        module.GetRoutine("StringHash"),
                        {
                            value.GetData(),
                            value.GetLength()
                        });
                    break;

                default:
                    YUNIMPLEMENTED();
            }

            builder.CreateBr(endBB);
            thenBB = builder.GetInsertBlock();

            builder.SetInsertPoint(elseBB);
            auto* elseResult = builder.getInt64(0);
            builder.CreateBr(endBB);
            elseBB = builder.GetInsertBlock();

            builder.SetInsertPoint(endBB);

            PHINode* result = builder.CreatePHI(thenResult->getType(), 2);
            result->addIncoming(thenResult, thenBB);
            result->addIncoming(elseResult, elseBB);

            return result;
        };

        auto codegenHashCombine = [&] (TCGIRBuilder& builder, Value* first, Value* second) -> Value* {
            //first ^ (second + 0x9e3779b9 + (second << 6) + (second >> 2));
            return builder.CreateXor(
                first,
                builder.CreateAdd(
                    builder.CreateAdd(
                        builder.CreateAdd(second, builder.getInt64(0x9e3779b9)),
                        builder.CreateLShr(second, builder.getInt64(2))),
                    builder.CreateShl(second, builder.getInt64(6))));
        };

        YCHECK(!types.empty());
        Value* result = builder.getInt64(0);
        for (size_t index = 0; index < types.size(); ++index) {
            result = codegenHashCombine(builder, result, codegenHashOp(index, builder));
        }
        builder.CreateRet(result);
    });
}

Function* CodegenTupleComparerFunction(
    const std::vector<std::function<TCGValue(TCGIRBuilder& builder, Value* row)>>& codegenArgs,
    const TCGModule& module,
    const std::vector<bool>& isDesc = std::vector<bool>())
{
    return MakeFunction<TComparerFunction>(module.GetModule(), "RowComparer", [&] (
        TCGIRBuilder& builder,
        Value* lhsRow,
        Value* rhsRow
    ) {
        auto returnIf = [&] (Value* condition, const TCodegenBlock& codegenInner) {
            auto* thenBB = builder.CreateBBHere("then");
            auto* elseBB = builder.CreateBBHere("else");
            builder.CreateCondBr(condition, thenBB, elseBB);
            builder.SetInsertPoint(thenBB);
            builder.CreateRet(builder.CreateSelect(codegenInner(builder), builder.getInt8(1), builder.getInt8(0)));
            builder.SetInsertPoint(elseBB);
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

            CodegenIf<TCGIRBuilder>(
                builder,
                builder.CreateOr(lhsValue.IsNull(), rhsValue.IsNull()),
                [&] (TCGIRBuilder& builder) {
                    returnIf(
                        builder.CreateICmpNE(lhsValue.IsNull(), rhsValue.IsNull()),
                        [&] (TCGIRBuilder&) {
                            return builder.CreateICmpULT(lhsValue.IsNull(), rhsValue.IsNull());
                        });
                },
                [&] (TCGIRBuilder& builder) {
                    auto* lhsData = lhsValue.GetData();
                    auto* rhsData = rhsValue.GetData();

                    switch (type) {
                        case EValueType::Boolean:
                        case EValueType::Int64:
                            returnIf(
                                builder.CreateICmpNE(lhsData, rhsData),
                                [&] (TCGIRBuilder&) {
                                    return builder.CreateICmpSLT(lhsData, rhsData);
                                });
                            break;

                        case EValueType::Uint64:
                            returnIf(
                                builder.CreateICmpNE(lhsData, rhsData),
                                [&] (TCGIRBuilder&) {
                                    return builder.CreateICmpULT(lhsData, rhsData);
                                });
                            break;

                        case EValueType::Double:
                            returnIf(
                                builder.CreateFCmpUNE(lhsData, rhsData),
                                [&] (TCGIRBuilder&) {
                                    return builder.CreateFCmpULT(lhsData, rhsData);
                                });
                            break;

                        case EValueType::String: {
                            Value* lhsLength = lhsValue.GetLength();
                            Value* rhsLength = rhsValue.GetLength();

                            Value* minLength = builder.CreateSelect(
                                builder.CreateICmpULT(lhsLength, rhsLength),
                                lhsLength,
                                rhsLength);

                            Value* cmpResult = builder.CreateCall(
                                module.GetRoutine("memcmp"),
                                {
                                    lhsData,
                                    rhsData,
                                    builder.CreateZExt(minLength, builder.getSizeType())
                                });

                            returnIf(
                                builder.CreateICmpNE(cmpResult, builder.getInt32(0)),
                                [&] (TCGIRBuilder&) {
                                    return builder.CreateICmpSLT(cmpResult, builder.getInt32(0));
                                });

                            returnIf(
                                builder.CreateICmpNE(lhsLength, rhsLength),
                                [&] (TCGIRBuilder&) {
                                    return builder.CreateICmpULT(lhsLength, rhsLength);
                                });

                            break;
                        }

                        default:
                            YUNREACHABLE();
                    }
                });
        };

        YCHECK(!codegenArgs.empty());

        for (int index = 0; index < codegenArgs.size(); ++index) {
            codegenEqualOrLessOp(index);
        }

        builder.CreateRet(builder.getInt8(0));
    });
}

Function* CodegenRowComparerFunction(
    const std::vector<EValueType>& types,
    const TCGModule& module)
{
    std::vector<std::function<TCGValue(TCGIRBuilder& builder, Value* row)>> compareArgs;
    for (int index = 0; index < types.size(); ++index) {
        compareArgs.push_back([index, type = types[index]] (TCGIRBuilder& builder, Value* row) {
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
    TCGContext& builder,
    Value* lhsData,
    Value* lhsLength,
    Value* rhsData,
    Value* rhsLength)
{
    Value* lhsLengthIsLess = builder.CreateICmpULT(lhsLength, rhsLength);
    Value* minLength = builder.CreateSelect(
        lhsLengthIsLess,
        lhsLength,
        rhsLength);

    Value* cmpResult = builder.CreateCall(
        builder.Module->GetRoutine("memcmp"),
        {
            lhsData,
            rhsData,
            builder.CreateZExt(minLength, builder.getSizeType())
        });

    return builder.CreateOr(
        builder.CreateICmpSLT(cmpResult, builder.getInt32(0)),
        builder.CreateAnd(
            builder.CreateICmpEQ(cmpResult, builder.getInt32(0)),
            lhsLengthIsLess));
};

TCodegenExpression MakeCodegenLiteralExpr(
    int index,
    EValueType type)
{
    return [
            index,
            type
        ] (TCGContext& builder, Value* row) {
            return TCGValue::CreateFromRow(
                builder,
                builder.GetConstantsRows(),
                index,
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
        ] (TCGContext& builder, Value* row) {
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
        ] (TCGContext& builder) {
            return builder.GetFunctionContextPtr(index);
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
    ] (TCGContext& builder, Value* row) {
        auto operandValue = codegenOperand(builder, row);

        return CodegenIf<TCGContext, TCGValue>(
            builder,
            operandValue.IsNull(),
            [&] (TCGIRBuilder& builder) {
                return TCGValue::CreateNull(builder, type);
            },
            [&] (TCGIRBuilder& builder) {
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
                                evalData = builder.CreateSub(builder.getInt64(0), operandData);
                                break;
                            case EValueType::Double:
                                evalData = builder.CreateFSub(ConstantFP::get(builder.getDoubleTy(), 0.0), operandData);
                                break;
                            default:
                                YUNREACHABLE();
                        }
                        break;


                    case EUnaryOp::BitNot:
                        evalData = builder.CreateNot(operandData);
                        break;

                    case EUnaryOp::Not:
                        evalData = builder.CreateXor(
                            builder.CreateZExtOrBitCast(
                                builder.getTrue(),
                                TDataTypeBuilder::TBoolean::get(builder.getContext())),
                            operandData);
			break;

                    default:
                        YUNREACHABLE();
                }

                return TCGValue::CreateFromValue(
                    builder,
                    builder.getFalse(),
                    nullptr,
                    evalData,
                    type);
            },
            Twine(name.c_str()));
    };
}

TCodegenExpression MakeCodegenBinaryOpExpr(
    EBinaryOp opcode,
    TCodegenExpression codegenLhs,
    TCodegenExpression codegenRhs,
    EValueType type,
    Stroka name)
{

    if (IsRelationalBinaryOp(opcode))
    {
        return [
            MOVE(opcode),
            MOVE(codegenLhs),
            MOVE(codegenRhs),
            MOVE(type),
            MOVE(name)
        ] (TCGContext& builder, Value* row) {
            auto nameTwine = Twine(name.c_str());
            auto lhsValue = codegenLhs(builder, row);
            auto rhsValue = codegenRhs(builder, row);

            #define CMP_OP(opcode, optype) \
                case EBinaryOp::opcode: \
                    evalData = builder.CreateZExtOrBitCast( \
                        builder.Create##optype(lhsData, rhsData), \
                        TDataTypeBuilder::TBoolean::get(builder.getContext())); \
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
                        YUNREACHABLE();
                }

                return TCGValue::CreateFromValue(
                    builder,
                    builder.getFalse(),
                    nullptr,
                    evalData,
                    type);
            };

            return CodegenIf<TCGContext, TCGValue>(
                builder,
                lhsValue.IsNull(),
                [&] (TCGContext& builder) {

                    return compareNulls();
                },
                [&] (TCGContext& builder) {

                    return CodegenIf<TCGContext, TCGValue>(
                        builder,
                        rhsValue.IsNull(),
                        [&] (TCGContext& builder) {

                            return compareNulls();
                        },
                        [&] (TCGContext& builder) {

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
                                            YUNREACHABLE();
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
                                            YUNREACHABLE();
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
                                            YUNREACHABLE();
                                    }
                                    break;
                                case EValueType::String: {
                                    Value* lhsLength = lhsValue.GetLength();
                                    Value* rhsLength = rhsValue.GetLength();

                                    auto codegenEqual = [&] () {
                                        return CodegenIf<TCGContext, Value*>(
                                            builder,
                                            builder.CreateICmpEQ(lhsLength, rhsLength),
                                            [&] (TCGContext& builder) {
                                                Value* minLength = builder.CreateSelect(
                                                    builder.CreateICmpULT(lhsLength, rhsLength),
                                                    lhsLength,
                                                    rhsLength);

                                                Value* cmpResult = builder.CreateCall(
                                                    builder.Module->GetRoutine("memcmp"),
                                                    {
                                                        lhsData,
                                                        rhsData,
                                                        builder.CreateZExt(minLength, builder.getSizeType())
                                                    });

                                                return builder.CreateICmpEQ(cmpResult, builder.getInt32(0));
                                            },
                                            [&] (TCGContext& builder) {
                                                return builder.getFalse();
                                            });
                                    };

                                    switch (opcode) {
                                        case EBinaryOp::Equal:
                                            evalData = codegenEqual();
                                            break;
                                        case EBinaryOp::NotEqual:
                                            evalData = builder.CreateNot(codegenEqual());
                                            break;
                                        case EBinaryOp::Less:
                                            evalData = CodegenLexicographicalCompare(builder, lhsData, lhsLength, rhsData, rhsLength);
                                            break;
                                        case EBinaryOp::Greater:
                                            evalData = CodegenLexicographicalCompare(builder, rhsData, rhsLength, lhsData, lhsLength);
                                            break;
                                        case EBinaryOp::LessOrEqual:
                                            evalData =  builder.CreateNot(
                                                CodegenLexicographicalCompare(builder, rhsData, rhsLength, lhsData, lhsLength));
                                            break;
                                        case EBinaryOp::GreaterOrEqual:
                                            evalData = builder.CreateNot(
                                                CodegenLexicographicalCompare(builder, lhsData, lhsLength, rhsData, rhsLength));
                                            break;
                                        default:
                                            YUNREACHABLE();
                                    }

                                    evalData = builder.CreateZExtOrBitCast(
                                        evalData,
                                        TDataTypeBuilder::TBoolean::get(builder.getContext()));
                                    break;
                                }
                                default:
                                    YUNREACHABLE();
                            }

                            return TCGValue::CreateFromValue(
                                builder,
                                builder.getFalse(),
                                nullptr,
                                evalData,
                                type);
                        });
                },
                nameTwine);

                #undef CMP_OP
        };

    } else {
        return [
            MOVE(opcode),
            MOVE(codegenLhs),
            MOVE(codegenRhs),
            MOVE(type),
            MOVE(name)
        ] (TCGContext& builder, Value* row) {
            auto nameTwine = Twine(name.c_str());

            auto lhsValue = codegenLhs(builder, row);

            return CodegenIf<TCGContext, TCGValue>(
                builder,
                lhsValue.IsNull(),
                [&] (TCGContext& builder) {
                    return TCGValue::CreateNull(builder, type);
                },
                [&] (TCGContext& builder) {
                    auto rhsValue = codegenRhs(builder, row);

                    return CodegenIf<TCGContext, TCGValue>(
                        builder,
                        rhsValue.IsNull(),
                        [&] (TCGContext& builder) {
                            return TCGValue::CreateNull(builder, type);
                        },
                        [&] (TCGContext& builder) {
                            YCHECK(lhsValue.GetStaticType() == rhsValue.GetStaticType());
                            auto operandType = lhsValue.GetStaticType();

                            Value* lhsData = lhsValue.GetData();
                            Value* rhsData = rhsValue.GetData();
                            Value* evalData = nullptr;

                            #define OP(opcode, optype) \
                                case EBinaryOp::opcode: \
                                    evalData = builder.Create##optype(lhsData, rhsData); \
                                    break;

                            switch (operandType) {

                                case EValueType::Boolean:
                                case EValueType::Int64:
                                    switch (opcode) {
                                        OP(Plus, Add)
                                        OP(Minus, Sub)
                                        OP(Multiply, Mul)
                                        OP(Divide, SDiv)
                                        OP(Modulo, SRem)
                                        OP(BitAnd, And)
                                        OP(BitOr, Or)
                                        OP(And, And)
                                        OP(Or, Or)
                                        OP(LeftShift, Shl)
                                        OP(RightShift, LShr)
                                        default:
                                            YUNREACHABLE();
                                    }
                                    break;
                                case EValueType::Uint64:
                                    switch (opcode) {
                                        OP(Plus, Add)
                                        OP(Minus, Sub)
                                        OP(Multiply, Mul)
                                        OP(Divide, UDiv)
                                        OP(Modulo, URem)
                                        OP(BitAnd, And)
                                        OP(BitOr, Or)
                                        OP(And, And)
                                        OP(Or, Or)
                                        OP(LeftShift, Shl)
                                        OP(RightShift, LShr)
                                        default:
                                            YUNREACHABLE();
                                    }
                                    break;
                                case EValueType::Double:
                                    switch (opcode) {
                                        OP(Plus, FAdd)
                                        OP(Minus, FSub)
                                        OP(Multiply, FMul)
                                        OP(Divide, FDiv)
                                        default:
                                            YUNREACHABLE();
                                    }
                                    break;
                                default:
                                    YUNREACHABLE();
                            }

                            #undef OP

                            return TCGValue::CreateFromValue(
                                builder,
                                builder.getFalse(),
                                nullptr,
                                evalData,
                                type);
                        });
               },
                nameTwine);
        };
    }
}

TCodegenExpression MakeCodegenInOpExpr(
    std::vector<TCodegenExpression> codegenArgs,
    int arrayIndex)
{
    return [
        MOVE(codegenArgs),
        MOVE(arrayIndex)
    ] (TCGContext& builder, Value* row) {
        size_t keySize = codegenArgs.size();

        Value* newRowPtr = builder.CreateAlloca(TypeBuilder<TRow, false>::get(builder.getContext()));
        Value* executionContextPtrRef = builder.GetExecutionContextPtr();

        builder.CreateCall(
            builder.Module->GetRoutine("AllocateRow"),
            {
                executionContextPtrRef,
                builder.getInt32(keySize),
                newRowPtr
            });

        Value* newRowRef = builder.CreateLoad(newRowPtr);

        std::vector<EValueType> keyTypes;
        for (int index = 0; index < keySize; ++index) {
            auto id = index;
            auto value = codegenArgs[index](builder, row);
            keyTypes.push_back(value.GetStaticType());        
            value.StoreToRow(builder, newRowRef, index, id);
        }

        Value* result = builder.CreateCall(
            builder.Module->GetRoutine("IsRowInArray"),
            {
                executionContextPtrRef,
                CodegenRowComparerFunction(keyTypes, *builder.Module),
                newRowRef,
                builder.getInt32(arrayIndex)
            });

        return TCGValue::CreateFromValue(
            builder,
            builder.getFalse(),
            nullptr,
            result,
            EValueType::Boolean);
    };
}

////////////////////////////////////////////////////////////////////////////////
// Operators
//

void CodegenScanOp(
    TCGContext& builder,
    const TCodegenConsumer& codegenConsumer)
{
    auto consume = MakeClosure<void(TRow*, int, char*)>(builder, "ScanOpInner", [&] (
        TCGContext& builder,
        Value* rows,
        Value* size,
        Value* stopFlag
    ) {
        CodegenForEachRow(builder, rows, size, stopFlag, codegenConsumer);
        builder.CreateRetVoid();
    });

    builder.CreateCall(
        builder.Module->GetRoutine("ScanOpHelper"),
        {
            builder.GetExecutionContextPtr(),
            consume.ClosurePtr,
            consume.Function
        });
}

TCodegenSource MakeCodegenJoinOp(
    int index,
    std::vector<TCodegenExpression> equations,
    TTableSchema sourceSchema,
    TCodegenSource codegenSource,
    size_t keyPrefix,
    std::vector<int> equationByIndex,
    std::vector<TCodegenExpression> evaluatedColumns)
{
    return [
        index,
        MOVE(equations),
        MOVE(sourceSchema),
        codegenSource = std::move(codegenSource),
        MOVE(keyPrefix),
        MOVE(equationByIndex),
        MOVE(evaluatedColumns)
    ] (TCGContext& builder, const TCodegenConsumer& codegenConsumer) {
        int lookupKeySize = keyPrefix;
        int joinKeySize = keyPrefix; //equations.size();
        std::vector<EValueType> lookupKeyTypes(lookupKeySize);
        std::vector<EValueType> joinKeyTypes(joinKeySize);

        auto collectRows = MakeClosure<void(void*, void*, void*)>(builder, "CollectRows", [&] (
            TCGContext& builder,
            Value* joinLookup,
            Value* keys,
            Value* chainedRows
        ) {
            Value* keyPtr = builder.CreateAlloca(TypeBuilder<TRow, false>::get(builder.getContext()));
            builder.CreateCall(
                builder.Module->GetRoutine("AllocatePermanentRow"),
                {
                    builder.GetExecutionContextPtr(),
                    builder.getInt32(lookupKeySize),
                    keyPtr
                });

            codegenSource(
                builder,
                [&] (TCGContext& builder, Value* row) {
                    Value* executionContextPtrRef = builder.GetExecutionContextPtr();
                    Value* keysRef = builder.ViaClosure(keys);
                    Value* chainedRowsRef = builder.ViaClosure(chainedRows);
                    Value* joinLookupRef = builder.ViaClosure(joinLookup);
                    Value* keyPtrRef = builder.ViaClosure(keyPtr);
                    Value* keyRef = builder.CreateLoad(keyPtrRef);

                    for (int column = 0; column < lookupKeySize; ++column) {
                        int index = equationByIndex[column];
                        if (index >= 0) {
                            auto joinKeyValue = equations[index](builder, row);
                            joinKeyTypes[index] = joinKeyValue.GetStaticType();
                            lookupKeyTypes[column] = joinKeyValue.GetStaticType();

                            joinKeyValue.StoreToRow(builder, keyRef, column, column);
                        }
                    }

                    for (int column = 0; column < lookupKeySize; ++column) {
                        if (equationByIndex[column] < 0) {
                            YCHECK(evaluatedColumns[column]);
                            auto evaluatedColumn = evaluatedColumns[column](builder, keyRef);
                            lookupKeyTypes[column] = evaluatedColumn.GetStaticType();

                            evaluatedColumn.StoreToRow(builder, keyRef, column, column);
                        }
                    }

                    builder.CreateCall(
                        builder.Module->GetRoutine("InsertJoinRow"),
                        {
                            executionContextPtrRef,
                            joinLookupRef,
                            keysRef,
                            chainedRowsRef,
                            keyPtrRef,
                            row,
                            builder.getInt32(lookupKeySize)
                        });
                });

            builder.CreateRetVoid();
        });


        auto consumeJoinedRows = MakeClosure<void(void*, char*)>(builder, "ConsumeJoinedRows", [&] (
            TCGContext& builder,
            Value* joinedRows,
            Value* stopFlag
        ) {
            CodegenForEachRow(
                builder,
                builder.CreateCall(builder.Module->GetRoutine("GetRowsData"), joinedRows),
                builder.CreateCall(builder.Module->GetRoutine("GetRowsSize"), joinedRows),
                stopFlag,
                codegenConsumer);

            builder.CreateRetVoid();
        });

        builder.CreateCall(
            builder.Module->GetRoutine("JoinOpHelper"),
            {
                builder.GetExecutionContextPtr(),
                builder.getInt32(index),

                CodegenGroupHasherFunction(lookupKeyTypes, *builder.Module),
                CodegenGroupComparerFunction(lookupKeyTypes, *builder.Module),
                CodegenRowComparerFunction(lookupKeyTypes, *builder.Module),

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
    ] (TCGContext& builder, const TCodegenConsumer& codegenConsumer) {
        codegenSource(
            builder,
            [&] (TCGContext& builder, Value* row) {
                auto predicateResult = codegenPredicate(builder, row);

                Value* result = builder.CreateZExtOrBitCast(
                    predicateResult.GetData(),
                    builder.getInt64Ty());

                auto* ifBB = builder.CreateBBHere("if");
                auto* endifBB = builder.CreateBBHere("endif");

                builder.CreateCondBr(
                    builder.CreateICmpNE(result, builder.getInt64(0)),
                    ifBB,
                    endifBB);

                builder.SetInsertPoint(ifBB);
                codegenConsumer(builder, row);
                builder.CreateBr(endifBB);

                builder.SetInsertPoint(endifBB);
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
    ] (TCGContext& builder, const TCodegenConsumer& codegenConsumer) {
        int projectionCount = codegenArgs.size();

        codegenSource(
            builder,
            [&] (TCGContext& builder, Value* row) {
                Value* newRowPtr = builder.CreateAlloca(TypeBuilder<TRow, false>::get(builder.getContext()));

                builder.CreateCall(
                    builder.Module->GetRoutine("AllocateRow"),
                    {
                        builder.GetExecutionContextPtr(),
                        builder.getInt32(projectionCount),
                        newRowPtr
                    });

                Value* newRow = builder.CreateLoad(newRowPtr);

                for (int index = 0; index < projectionCount; ++index) {
                    auto id = index;
                
                    codegenArgs[index](builder, row)
                        .StoreToRow(builder, newRow, index, id);
                }

                codegenConsumer(builder, newRow);
            });
    };
}

std::function<void(TCGContext&, Value*, Value*)> MakeCodegenEvaluateGroups(
    std::vector<TCodegenExpression> codegenGroupExprs)
{
    return [
        MOVE(codegenGroupExprs)
    ] (TCGContext& builder, Value* srcRow, Value* dstRow) {
        for (int index = 0; index < codegenGroupExprs.size(); index++) {
            auto id = index;
            auto value = codegenGroupExprs[index](builder, srcRow);
            value.StoreToRow(builder, dstRow, index, id);
        }
    };
}

std::function<void(TCGContext&, Value*, Value*)> MakeCodegenEvaluateAggregateArgs(
    std::vector<TCodegenExpression> codegenGroupExprs,
    std::vector<TCodegenExpression> codegenAggregateExprs,
    std::vector<TCodegenAggregate> codegenAggregates,
    bool isMerge,
    TTableSchema inputSchema)
{
    return [
        MOVE(codegenGroupExprs),
        MOVE(codegenAggregateExprs),
        MOVE(codegenAggregates),
        isMerge,
        inputSchema
    ] (TCGContext& builder, Value* srcRow, Value* dstRow) {
        auto keySize = codegenGroupExprs.size();
        auto aggregatesCount = codegenAggregates.size();

        if (!isMerge) {
            for (int index = 0; index < aggregatesCount; index++) {
                auto id = keySize + index;
                auto value = codegenAggregateExprs[index](builder, srcRow);
                value.StoreToRow(builder, dstRow, keySize + index, id);
            }
        } else {
            for (int index = 0; index < aggregatesCount; index++) {
                auto id = keySize + index;
                TCGValue::CreateFromRow(
                    builder,
                    srcRow,
                    keySize + index,
                    inputSchema.Columns()[id].Type)
                    .StoreToRow(
                        builder,
                        dstRow,
                        keySize + index,
                        id);
            }
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
            auto aggState = builder.CreateConstInBoundsGEP1_32(
                nullptr,
                CodegenValuesPtrFromRow(builder, groupRow),
                keySize + index);
            auto newValue = builder.CreateConstInBoundsGEP1_32(
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
                builder.CreateConstInBoundsGEP1_32(
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
    int groupRowSize)
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
        groupRowSize
    ] (TCGContext& builder, const TCodegenConsumer& codegenConsumer) {
        auto collect = MakeClosure<void(void*, void*)>(builder, "CollectGroups", [&] (
            TCGContext& builder,
            Value* groupedRows,
            Value* lookup
        ) {
            Value* newRowPtr = builder.CreateAlloca(TypeBuilder<TRow, false>::get(builder.getContext()));

            builder.CreateCall(
                builder.Module->GetRoutine("AllocatePermanentRow"),
                {
                    builder.GetExecutionContextPtr(),
                    builder.getInt32(groupRowSize),
                    newRowPtr
                });

            codegenSource(
                builder,
                [&] (TCGContext& builder, Value* row) {
                    Value* executionContextPtrRef = builder.GetExecutionContextPtr();
                    Value* groupedRowsRef = builder.ViaClosure(groupedRows);
                    Value* lookupRef = builder.ViaClosure(lookup);
                    Value* newRowPtrRef = builder.ViaClosure(newRowPtr);
                    Value* newRowRef = builder.CreateLoad(newRowPtrRef);

                    codegenEvaluateGroups(builder, row, newRowRef);

                    auto groupRowPtr = builder.CreateCall(
                        builder.Module->GetRoutine("InsertGroupRow"),
                        {
                            executionContextPtrRef,
                            lookupRef,
                            groupedRowsRef,
                            newRowRef,
                            builder.getInt32(keyTypes.size())
                        });

                    CodegenIf<TCGContext>(
                        builder,
                        builder.CreateIsNotNull(groupRowPtr),
                        [&] (TCGContext& builder) {
                            auto groupRow = builder.CreateLoad(groupRowPtr);

                            auto inserted = builder.CreateICmpEQ(
                                builder.CreateExtractValue(
                                    groupRow,
                                    TypeBuilder<TRow, false>::Fields::Header),
                                builder.CreateExtractValue(
                                    newRowRef,
                                    TypeBuilder<TRow, false>::Fields::Header));

                            CodegenIf<TCGContext>(
                                builder,
                                inserted,
                                [&] (TCGContext& builder) {
                                    codegenInitialize(builder, groupRow);

                                    builder.CreateCall(
                                        builder.Module->GetRoutine("AllocatePermanentRow"),
                                        {
                                            builder.GetExecutionContextPtr(),
                                            builder.getInt32(groupRowSize),
                                            newRowPtrRef
                                        });
                                });

                            // Here *newRowPtrRef != groupRow.
                            auto newRow = builder.CreateLoad(newRowPtrRef);

                            codegenEvaluateAggregateArgs(builder, row, newRow);
                            codegenUpdate(builder, newRow, groupRow);
                        });
                });

            builder.CreateRetVoid();
        });

        auto consume = MakeClosure<void(void*, char*)>(builder, "Consume", [&] (
            TCGContext& builder,
            Value* finalGroupedRows,
            Value* stopFlag
        ) {
            auto codegenFinalizingConsumer = [
                MOVE(codegenConsumer),
                MOVE(codegenFinalize)
            ] (TCGContext& builder, Value* row) {
                codegenFinalize(builder, row);
                codegenConsumer(builder, row);
            };

            CodegenForEachRow(
                builder,
                builder.CreateCall(builder.Module->GetRoutine("GetRowsData"), {finalGroupedRows}),
                builder.CreateCall(builder.Module->GetRoutine("GetRowsSize"), {finalGroupedRows}),
                stopFlag,
                codegenFinalizingConsumer);

            builder.CreateRetVoid();
        });

        builder.CreateCall(
            builder.Module->GetRoutine("GroupOpHelper"),
            {
                builder.GetExecutionContextPtr(),
                CodegenGroupHasherFunction(keyTypes, *builder.Module),
                CodegenGroupComparerFunction(keyTypes, *builder.Module),

                collect.ClosurePtr,
                collect.Function,

                consume.ClosurePtr,
                consume.Function,
            });

    };
}

TCodegenSource MakeCodegenOrderOp(
    std::vector<TCodegenExpression> codegenExprs,
    TTableSchema sourceSchema,
    TCodegenSource codegenSource,
    const std::vector<bool>& isDesc)
{
    return [
        isDesc,
        MOVE(codegenExprs),
        MOVE(sourceSchema),
        codegenSource = std::move(codegenSource)
    ] (TCGContext& builder, const TCodegenConsumer& codegenConsumer) {
        auto schemaSize = sourceSchema.Columns().size();
        std::vector<EValueType> orderColumnTypes;

        auto collectRows = MakeClosure<void(void*)>(builder, "CollectRows", [&] (
            TCGContext& builder,
            Value* topN
        ) {
            Value* newRowPtr = builder.CreateAlloca(TypeBuilder<TRow, false>::get(builder.getContext()));

            builder.CreateCall(
                builder.Module->GetRoutine("AllocatePermanentRow"),
                {
                    builder.GetExecutionContextPtr(),
                    builder.getInt32(sourceSchema.Columns().size() + codegenExprs.size()),
                    newRowPtr
                });

            Value* newRow = builder.CreateLoad(newRowPtr);

            codegenSource(
                builder,
                [&] (TCGContext& builder, Value* row) {
                    Value* topNRef = builder.ViaClosure(topN);
                    Value* newRowRef = builder.ViaClosure(newRow);

                    for (size_t index = 0; index < schemaSize; ++index) {
                        auto type = sourceSchema.Columns()[index].Type;
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

                    builder.CreateCall(
                        builder.Module->GetRoutine("AddRow"),
                        {topNRef, newRowRef});
                });

            builder.CreateRetVoid();
        });

        auto consumeOrderedRows = MakeClosure<void(void*, char*)>(builder, "ConsumeOrderedRows", [&] (
            TCGContext& builder,
            Value* orderedRows,
            Value* stopFlag
        ) {
            CodegenForEachRow(
                builder,
                builder.CreateCall(builder.Module->GetRoutine("GetRowsData"), {orderedRows}),
                builder.CreateCall(builder.Module->GetRoutine("GetRowsSize"), {orderedRows}),
                stopFlag,
                codegenConsumer);

            builder.CreateRetVoid();
        });

        std::vector<std::function<TCGValue(TCGIRBuilder& builder, Value* row)>> compareArgs;
        for (int index = 0; index < codegenExprs.size(); ++index) {
            auto columnIndex = schemaSize + index;
            auto type = orderColumnTypes[index];

            compareArgs.push_back([columnIndex, type] (TCGIRBuilder& builder, Value* row) {
                return TCGValue::CreateFromRow(
                    builder,
                    row,
                    columnIndex,
                    type);
            });
        }

        builder.CreateCall(
            builder.Module->GetRoutine("OrderOpHelper"),
            {
                builder.GetExecutionContextPtr(),
                CodegenTupleComparerFunction(compareArgs, *builder.Module, isDesc),

                collectRows.ClosurePtr,
                collectRows.Function,

                consumeOrderedRows.ClosurePtr,
                consumeOrderedRows.Function,
                builder.getInt32(schemaSize)
            });
    };
}

////////////////////////////////////////////////////////////////////////////////

TCGQueryCallback CodegenEvaluate(
    TCodegenSource codegenSource)
{
    auto module = TCGModule::Create(GetQueryRoutineRegistry());

    auto& context = module->GetContext();

    auto entryFunctionName = Stroka("Evaluate");

    Function* function = Function::Create(
        TypeBuilder<TCGQuerySignature, false>::get(context),
        Function::ExternalLinkage,
        entryFunctionName.c_str(),
        module->GetModule());

    function->addFnAttr(llvm::Attribute::AttrKind::UWTable);

    auto args = function->arg_begin();
    Value* constants = args; constants->setName("constants");
    Value* executionContextPtr = ++args; executionContextPtr->setName("passedFragmentParamsPtr");
    Value* functionContextsPtr = ++args; functionContextsPtr->setName("functionContextsPtr");
    YCHECK(++args == function->arg_end());

    TCGContext builder(
        module,
        constants,
        executionContextPtr,
        functionContextsPtr,
        BasicBlock::Create(context, "entry", function));

    codegenSource(
        builder,
        [&] (TCGContext& builder, Value* row) {
            builder.CreateCall(
                module->GetRoutine("WriteRow"),
                {row, builder.GetExecutionContextPtr()});
        });

    builder.CreateRetVoid();

    return module->GetCompiledFunction<TCGQuerySignature>(entryFunctionName);
}

TCGExpressionCallback CodegenExpression(TCodegenExpression codegenExpression)
{
    auto module = TCGModule::Create(GetQueryRoutineRegistry());
    auto& context = module->GetContext();

    auto entryFunctionName = Stroka("EvaluateExpression");

    Function* function = Function::Create(
        TypeBuilder<TCGExpressionSignature, false>::get(context),
        Function::ExternalLinkage,
        entryFunctionName.c_str(),
        module->GetModule());

    function->addFnAttr(llvm::Attribute::AttrKind::UWTable);

    auto args = function->arg_begin();
    Value* resultPtr = args; resultPtr->setName("resultPtr");
    Value* inputRow = ++args; inputRow->setName("inputRow");
    Value* constants = ++args; constants->setName("constants");
    Value* executionContextPtr = ++args; executionContextPtr->setName("passedFragmentParamsPtr");
    Value* functionContextsPtr = ++args; functionContextsPtr->setName("functionContextsPtr");
    YCHECK(++args == function->arg_end());

    TCGContext builder(
        module,
        constants,
        executionContextPtr,
        functionContextsPtr,
        BasicBlock::Create(context, "entry", function));

    auto result = codegenExpression(builder, inputRow);

    result.StoreToValue(builder, resultPtr, 0, "writeResult");

    builder.CreateRetVoid();

    return module->GetCompiledFunction<TCGExpressionSignature>(entryFunctionName);
}

TCGAggregateCallbacks CodegenAggregate(TCodegenAggregate codegenAggregate)
{
    auto module = TCGModule::Create(GetQueryRoutineRegistry());
    auto& context = module->GetContext();

    auto initName = Stroka("init");
    {
        Function* function = Function::Create(
            TypeBuilder<TCGAggregateInitSignature, false>::get(context),
            Function::ExternalLinkage,
            initName.c_str(),
            module->GetModule());

        function->addFnAttr(llvm::Attribute::AttrKind::UWTable);

        auto args = function->arg_begin();
        Value* executionContextPtr = args; executionContextPtr->setName("executionContextPtr");
        Value* resultPtr = ++args; resultPtr->setName("resultPtr");
        YCHECK(++args == function->arg_end());

        TCGContext builder(module, nullptr, executionContextPtr, nullptr, BasicBlock::Create(context, "entry", function));
        auto result = codegenAggregate.Initialize(builder, nullptr);
        result.StoreToValue(builder, resultPtr, 0, "writeResult");
        builder.CreateRetVoid();
    }

    auto updateName = Stroka("update");
    {
        Function* function = Function::Create(
            TypeBuilder<TCGAggregateUpdateSignature, false>::get(context),
            Function::ExternalLinkage,
            updateName.c_str(),
            module->GetModule());

        function->addFnAttr(llvm::Attribute::AttrKind::UWTable);

        auto args = function->arg_begin();
        Value* executionContextPtr = args; executionContextPtr->setName("executionContextPtr");
        Value* resultPtr = ++args; resultPtr->setName("resultPtr");
        Value* statePtr = ++args; resultPtr->setName("statePtr");
        Value* newValuePtr = ++args; resultPtr->setName("newValuePtr");
        YCHECK(++args == function->arg_end());

        TCGContext builder(module, nullptr, executionContextPtr, nullptr, BasicBlock::Create(context, "entry", function));
        auto result = codegenAggregate.Update(builder, statePtr, newValuePtr);
        result.StoreToValue(builder, resultPtr, 0, "writeResult");
        builder.CreateRetVoid();
    }

    auto mergeName = Stroka("merge");
    {
        Function* function = Function::Create(
            TypeBuilder<TCGAggregateMergeSignature, false>::get(context),
            Function::ExternalLinkage,
            mergeName.c_str(),
            module->GetModule());

        function->addFnAttr(llvm::Attribute::AttrKind::UWTable);

        auto args = function->arg_begin();
        Value* executionContextPtr = args; executionContextPtr->setName("executionContextPtr");
        Value* resultPtr = ++args; resultPtr->setName("resultPtr");
        Value* dstStatePtr = ++args; resultPtr->setName("dstStatePtr");
        Value* statePtr = ++args; resultPtr->setName("statePtr");
        YCHECK(++args == function->arg_end());

        TCGContext builder(module, nullptr, executionContextPtr, nullptr, BasicBlock::Create(context, "entry", function));
        auto result = codegenAggregate.Merge(builder, dstStatePtr, statePtr);
        result.StoreToValue(builder, resultPtr, 0, "writeResult");
        builder.CreateRetVoid();
    }

    auto finalizeName = Stroka("finalize");
    {
        Function* function = Function::Create(
            TypeBuilder<TCGAggregateFinalizeSignature, false>::get(context),
            Function::ExternalLinkage,
            finalizeName.c_str(),
            module->GetModule());

        function->addFnAttr(llvm::Attribute::AttrKind::UWTable);

        auto args = function->arg_begin();
        Value* executionContextPtr = args; executionContextPtr->setName("executionContextPtr");
        Value* resultPtr = ++args; resultPtr->setName("resultPtr");
        Value* statePtr = ++args; resultPtr->setName("statePtr");
        YCHECK(++args == function->arg_end());

        TCGContext builder(module, nullptr, executionContextPtr, nullptr, BasicBlock::Create(context, "entry", function));
        auto result = codegenAggregate.Finalize(builder, statePtr);
        result.StoreToValue(builder, resultPtr, 0, "writeResult");
        builder.CreateRetVoid();
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

