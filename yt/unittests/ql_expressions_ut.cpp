#include "framework.h"
#include "ql_helpers.h"

#include <yt/ytlib/query_client/column_evaluator.h>
#include <yt/ytlib/query_client/config.h>
#include <yt/ytlib/query_client/folding_profiler.h>
#include <yt/ytlib/query_client/plan_helpers.h>
#include <yt/ytlib/query_client/query_preparer.h>
#include <yt/ytlib/query_client/query_statistics.h>
#include <yt/ytlib/query_client/functions.h>
#include <yt/ytlib/query_client/functions_cg.h>
#include <yt/ytlib/query_client/coordinator.h>

#include <yt/core/yson/string.h>
#include <yt/core/ytree/convert.h>

// Tests:
// TCompareExpressionTest
// TEliminateLookupPredicateTest
// TEliminatePredicateTest
// TPrepareExpressionTest
// TArithmeticTest
// TCompareWithNullTest
// TEvaluateExpressionTest
// TEvaluateAggregationTest

namespace NYT {
namespace NQueryClient {
namespace {

using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TCompareExpressionTest
{
protected:
    bool Equal(TConstExpressionPtr lhs, TConstExpressionPtr rhs)
    {
        if (auto literalLhs = lhs->As<TLiteralExpression>()) {
            auto literalRhs = rhs->As<TLiteralExpression>();
            if (literalRhs == nullptr || literalLhs->Value != literalRhs->Value) {
                return false;
            }
        } else if (auto referenceLhs = lhs->As<TReferenceExpression>()) {
            auto referenceRhs = rhs->As<TReferenceExpression>();
            if (referenceRhs == nullptr
                || referenceLhs->ColumnName != referenceRhs->ColumnName) {
                return false;
            }
        } else if (auto functionLhs = lhs->As<TFunctionExpression>()) {
            auto functionRhs = rhs->As<TFunctionExpression>();
            if (functionRhs == nullptr
                || functionLhs->FunctionName != functionRhs->FunctionName
                || functionLhs->Arguments.size() != functionRhs->Arguments.size()) {
                return false;
            }
            for (int index = 0; index < functionLhs->Arguments.size(); ++index) {
                if (!Equal(functionLhs->Arguments[index], functionRhs->Arguments[index])) {
                    return false;
                }
            }
        } else if (auto unaryLhs = lhs->As<TUnaryOpExpression>()) {
            auto unaryRhs = rhs->As<TUnaryOpExpression>();
            if (unaryRhs == nullptr
                || unaryLhs->Opcode != unaryRhs->Opcode
                || !Equal(unaryLhs->Operand, unaryRhs->Operand)) {
                return false;
            }
        } else if (auto binaryLhs = lhs->As<TBinaryOpExpression>()) {
            auto binaryRhs = rhs->As<TBinaryOpExpression>();
            if (binaryRhs == nullptr
                || binaryLhs->Opcode != binaryRhs->Opcode
                || !Equal(binaryLhs->Lhs, binaryRhs->Lhs)
                || !Equal(binaryLhs->Rhs, binaryRhs->Rhs)) {
                return false;
            }
        } else if (auto inLhs = lhs->As<TInOpExpression>()) {
            auto inRhs = rhs->As<TInOpExpression>();
            if (inRhs == nullptr
                || inLhs->Values.Size() != inRhs->Values.Size()
                || inLhs->Arguments.size() != inRhs->Arguments.size()) {
                return false;
            }
            for (int index = 0; index < inLhs->Values.Size(); ++index) {
                if (inLhs->Values[index] != inRhs->Values[index]) {
                    return false;
                }
            }
            for (int index = 0; index < inLhs->Arguments.size(); ++index) {
                if (!Equal(inLhs->Arguments[index], inRhs->Arguments[index])) {
                    return false;
                }
            }
        } else {
            YUNREACHABLE();
        }

        return true;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TExtractSubexpressionPredicateTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::tuple<
        const char*,
        const char*,
        const char*,
        const char*>>
    , public TCompareExpressionTest
{
protected:
    virtual void SetUp() override
    { }

};

TEST_P(TExtractSubexpressionPredicateTest, Simple)
{
    const auto& args = GetParam();
    const auto& schemaString = std::get<0>(args);
    const auto& subschemaString = std::get<1>(args);
    const auto& predicateString = std::get<2>(args);
    const auto& extractedString = std::get<3>(args);

    TTableSchema tableSchema;
    TTableSchema tableSubschema;
    Deserialize(tableSchema, ConvertToNode(TYsonString(schemaString)));
    Deserialize(tableSubschema, ConvertToNode(TYsonString(subschemaString)));

    auto predicate = PrepareExpression(predicateString, tableSchema);
    auto expected = PrepareExpression(extractedString, tableSubschema);

    auto extracted = ExtractPredicateForColumnSubset(predicate, tableSubschema);

    TConstExpressionPtr extracted2;
    TConstExpressionPtr remaining;
    std::tie(extracted2, remaining) = SplitPredicateByColumnSubset(predicate, tableSubschema);

    EXPECT_TRUE(Equal(extracted, expected))
        << "schema: " << schemaString << std::endl
        << "subschema: " << subschemaString << std::endl
        << "predicate: " << ::testing::PrintToString(predicate) << std::endl
        << "extracted: " << ::testing::PrintToString(extracted) << std::endl
        << "expected: " << ::testing::PrintToString(expected);

    EXPECT_TRUE(Equal(extracted2, expected))
        << "schema: " << schemaString << std::endl
        << "subschema: " << subschemaString << std::endl
        << "predicate: " << ::testing::PrintToString(predicate) << std::endl
        << "extracted2: " << ::testing::PrintToString(extracted2) << std::endl
        << "expected: " << ::testing::PrintToString(expected);
}

INSTANTIATE_TEST_CASE_P(
    TExtractSubexpressionPredicateTest,
    TExtractSubexpressionPredicateTest,
    ::testing::Values(
        std::make_tuple(
            "[{name=a;type=boolean;}; {name=b;type=boolean}; {name=c;type=boolean}]",
            "[{name=a;type=boolean;}]",
            "a and b and c",
            "a"),
        std::make_tuple(
            "[{name=a;type=boolean;}; {name=b;type=boolean}; {name=c;type=boolean}]",
            "[{name=a;type=boolean;}]",
            "not a and b and c",
            "not a"),
        std::make_tuple(
            "[{name=a;type=int64;}; {name=b;type=boolean}; {name=c;type=boolean}]",
            "[{name=a;type=int64;}]",
            "not is_null(a) and b and c",
            "not is_null(a)"),
        std::make_tuple(
            "[{name=a;type=int64;}; {name=b;type=boolean}; {name=c;type=boolean}]",
            "[{name=a;type=int64;}]",
            "a in (1, 2, 3) and b and c",
            "a in (1, 2, 3)"),
        std::make_tuple(
            "[{name=a;type=int64;}; {name=b;type=boolean}; {name=c;type=boolean}]",
            "[{name=a;type=int64;}]",
            "a = 1 and b and c",
            "a = 1"),
        std::make_tuple(
            "[{name=a;type=int64;}; {name=b;type=int64}; {name=c;type=boolean}]",
            "[{name=a;type=int64;}; {name=b;type=int64}]",
            "a = b and c",
            "a = b"),
        std::make_tuple(
            "[{name=a;type=boolean;}; {name=b;type=int64}; {name=c;type=boolean}]",
            "[{name=a;type=boolean;}; {name=b;type=int64}]",
            "if(a, b = 1, false) and c",
            "if(a, b = 1, false)"),
        std::make_tuple(
            "[{name=a;type=boolean;}; {name=b;type=boolean}]",
            "[{name=a;type=boolean;};]",
            "a or b",
            "true")
));

////////////////////////////////////////////////////////////////////////////////

class TEliminateLookupPredicateTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::tuple<
        const char*,
        const char*,
        const char*,
        const char*,
        std::vector<const char*>>>
    , public TCompareExpressionTest
{
protected:
    virtual void SetUp() override
    { }

    TConstExpressionPtr Eliminate(
        std::vector<TOwningKey>& lookupKeys,
        TConstExpressionPtr expr,
        const TKeyColumns& keyColumns)
    {
        std::vector<TRow> keys;
        keys.reserve(lookupKeys.size());

        for (const auto& lookupKey : lookupKeys) {
            keys.push_back(lookupKey);
        }

        return EliminatePredicate(
            keys,
            expr,
            keyColumns);
    }
};

TEST_P(TEliminateLookupPredicateTest, Simple)
{
    const auto& args = GetParam();
    const auto& schemaString = std::get<0>(args);
    const auto& keyString = std::get<1>(args);
    const auto& predicateString = std::get<2>(args);
    const auto& refinedString = std::get<3>(args);
    const auto& keyStrings = std::get<4>(args);

    TTableSchema tableSchema;
    TKeyColumns keyColumns;
    Deserialize(tableSchema, ConvertToNode(TYsonString(schemaString)));
    Deserialize(keyColumns, ConvertToNode(TYsonString(keyString)));

    std::vector<TOwningKey> keys;
    Stroka keysString;
    for (const auto& keyString : keyStrings) {
        keys.push_back(BuildKey(keyString));
        keysString += Stroka(keysString.size() > 0 ? ", " : "") + "[" + keyString + "]";
    }

    auto predicate = PrepareExpression(predicateString, tableSchema);
    auto expected = PrepareExpression(refinedString, tableSchema);
    auto refined = Eliminate(keys, predicate, keyColumns);

    EXPECT_TRUE(Equal(refined, expected))
        << "schema: " << schemaString << std::endl
        << "key_columns: " << keyString << std::endl
        << "keys: " << keysString << std::endl
        << "predicate: " << predicateString << std::endl
        << "refined: " << ::testing::PrintToString(refined) << std::endl
        << "expected: " << ::testing::PrintToString(expected);
}

INSTANTIATE_TEST_CASE_P(
    TEliminateLookupPredicateTest,
    TEliminateLookupPredicateTest,
    ::testing::Values(
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(k,l) in ((1,2),(3,4))",
            "false",
            std::vector<const char*>{"1;3"}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(k,l) in ((1,2),(3,4))",
            "true",
            std::vector<const char*>{"1;2"}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(k,l) in ((1,2),(3,4))",
            "true",
            std::vector<const char*>{"1;2", "3;4"}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(l,k) in ((1,2),(3,4))",
            "false",
            std::vector<const char*>{"3;1"}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(l,k) in ((1,2),(3,4))",
            "true",
            std::vector<const char*>{"2;1"}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(l,k) in ((1,2),(3,4))",
            "true",
            std::vector<const char*>{"2;1", "4;3"}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((1),(3))",
            "true",
            std::vector<const char*>{"1;2", "3;4"}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((1),(3))",
            "true",
            std::vector<const char*>{"1", "3"}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "l in ((2),(4))",
            "true",
            std::vector<const char*>{"1;2", "3;4"})
));

////////////////////////////////////////////////////////////////////////////////

class TEliminatePredicateTest0
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::vector<const char*>>
    , public TCompareExpressionTest
{
protected:
    TConstExpressionPtr Eliminate(
        const TKeyRange& keyRange,
        TConstExpressionPtr expr,
        const TTableSchema& tableSchema,
        const TKeyColumns& keyColumns)
    {
        auto rowRange = TRowRange(keyRange.first, keyRange.second);

        return EliminatePredicate(
            MakeRange(&rowRange, 1),
            expr,
            keyColumns);
    }
};

TEST_P(TEliminatePredicateTest0, Simple)
{
    const auto& args = GetParam();
    const auto& schemaString = args[0];
    const auto& keyString = args[1];
    const auto& predicateString = args[2];
    const auto& refinedString = args[3];
    const auto& lowerString = args[4];
    const auto& upperString = args[5];

    TTableSchema tableSchema;
    TKeyColumns keyColumns;
    Deserialize(tableSchema, ConvertToNode(TYsonString(schemaString)));
    Deserialize(keyColumns, ConvertToNode(TYsonString(keyString)));

    auto predicate = PrepareExpression(predicateString, tableSchema);
    auto expected = PrepareExpression(refinedString, tableSchema);
    auto range = TKeyRange{BuildKey(lowerString), BuildKey(upperString)};
    auto refined = Eliminate(range, predicate, tableSchema, keyColumns);

    EXPECT_TRUE(Equal(refined, expected))
        << "schema: " << schemaString << std::endl
        << "key_columns: " << keyString << std::endl
        << "range: [" << lowerString << ", " << upperString << "]" << std::endl
        << "predicate: " << predicateString << std::endl
        << "refined: " << ::testing::PrintToString(refined) << std::endl
        << "expected: " << ::testing::PrintToString(expected);
}



////////////////////////////////////////////////////////////////////////////////

class TEliminatePredicateTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::tuple<
        const char*,
        const char*,
        const char*,
        const char*,
        std::vector<const char*>>>
    , public TCompareExpressionTest
{
protected:
    TConstExpressionPtr Eliminate(
        const std::vector<TKeyRange>& keyRanges,
        TConstExpressionPtr expr,
        const TTableSchema& tableSchema,
        const TKeyColumns& keyColumns)
    {
        TRowRanges rowRanges;
        for (const auto& keyRange : keyRanges) {
            rowRanges.emplace_back(keyRange.first.Get(), keyRange.second.Get());
        }

        return EliminatePredicate(
            rowRanges,
            expr,
            keyColumns);
    }
};

TEST_P(TEliminatePredicateTest, Simple)
{
    const auto& args = GetParam();
    const auto& schemaString = std::get<0>(args);
    const auto& keyString = std::get<1>(args);
    const auto& predicateString = std::get<2>(args);
    const auto& refinedString = std::get<3>(args);
    const auto& keyStrings = std::get<4>(args);

    const auto& lowerString = keyStrings[0];
    const auto& upperString = keyStrings[1];

    TTableSchema tableSchema;
    TKeyColumns keyColumns;
    Deserialize(tableSchema, ConvertToNode(TYsonString(schemaString)));
    Deserialize(keyColumns, ConvertToNode(TYsonString(keyString)));

    auto predicate = PrepareExpression(predicateString, tableSchema);
    auto expected = PrepareExpression(refinedString, tableSchema);
    auto range = TKeyRange{BuildKey(lowerString), BuildKey(upperString)};



    std::vector<TKeyRange> owningRanges;
    for (size_t i = 0; i < keyStrings.size() / 2; ++i) {
        owningRanges.emplace_back(BuildKey(keyStrings[2 * i]), BuildKey(keyStrings[2 * i + 1]));
    }

    auto refined = Eliminate(owningRanges, predicate, tableSchema, keyColumns);



    EXPECT_TRUE(Equal(refined, expected))
        << "schema: " << schemaString << std::endl
        << "key_columns: " << keyString << std::endl
        << "range: [" << lowerString << ", " << upperString << "]" << std::endl
        << "predicate: " << predicateString << std::endl
        << "refined: " << ::testing::PrintToString(refined) << std::endl
        << "expected: " << ::testing::PrintToString(expected);
}

INSTANTIATE_TEST_CASE_P(
    TEliminatePredicateTestOld,
    TEliminatePredicateTest,
    ::testing::Values(
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(k,l) in ((1,2),(3,4))",
            "(k,l) in ((1,2),(3,4))",
            std::vector<const char*>{_MIN_, _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(k,l) in ((1,2),(3,4))",
            "(k,l) in ((1,2))",
            std::vector<const char*>{"1", "2"}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(k) in ((2),(4))",
            "(k) in ((2),(4))",
            std::vector<const char*>{_MIN_, _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(l) in ((2),(4))",
            "(l) in ((2),(4))",
            std::vector<const char*>{_MIN_, _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(k) in ((2),(4))",
            "(k) in ((2))",
            std::vector<const char*>{"2;1", "3;3"}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending;expression=l}; {name=l;type=int64;sort_order=ascending}; {name=a;type=int64}]",
            "[k;l]",
            "l in ((2),(4))",
            "l in ((2),(4))",
            std::vector<const char*>{_MIN_, _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((0),(2),(4))",
            "k in ((2))",
            std::vector<const char*>{"2;1", "3;3"}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((0),(2),(4))",
            "k in ((2))",
            std::vector<const char*>{"2;1", "3;3"}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((0),(2),(4),(6))",
            "k in ((2),(4))",
            std::vector<const char*>{"2;1", "4;5"}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((0),(2),(4),(6))",
            "k in ((2))",
            std::vector<const char*>{"2", "3"}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=m;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((0),(2),(4))",
            "k in ((2))",
            std::vector<const char*>{"2;2;2", "3;3;3"})
));

INSTANTIATE_TEST_CASE_P(
    TEliminatePredicateTest,
    TEliminatePredicateTest,
    ::testing::Values(
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "k = 1 and l in (1,2,3)",
            "true",
            std::vector<const char*>{"1;1", "1;1;" _MAX_, "1;2", "1;2;" _MAX_, "1;3", "1;3;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "k in (1,2,3) and l = 1",
            "true",
            std::vector<const char*>{"1;1", "1;1;" _MAX_, "2;1", "2;1;" _MAX_, "3;1", "3;1;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(k,l) in ((1,2),(3,4))",
            "true",
            std::vector<const char*>{"1;2", "1;2;" _MAX_, "3;4", "3;4;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(k) in ((2),(4))",
            "true",
            std::vector<const char*>{"2", "2;" _MAX_, "4", "4;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;}; {name=l;type=int64}; {name=a;type=int64}]",
            "[k;l]",
            "(l) in ((2),(4))",
            "(l) in ((2),(4))",
            std::vector<const char*>{_MIN_, _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending;expression=l}; {name=l;type=int64;sort_order=ascending}; {name=a;type=int64}]",
            "[k;l]",
            "l in ((2),(4))",
            "true",
            std::vector<const char*>{"2;2", "2;2;" _MAX_, "4;4", "4;4;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending;expression=\"l+1\"}; {name=l;type=int64;sort_order=ascending}; {name=a;type=int64}]",
            "[k;l]",
            "l in ((2),(4))",
            "true",
            std::vector<const char*>{"3;2", "3;2;" _MAX_, "5;4", "5;4;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending;expression=l}; {name=l;type=int64;sort_order=ascending}; {name=a;type=int64}]",
            "[k;l]",
            "l in ((0),(2),(4))",
            "true",
            std::vector<const char*>{"0;0", "0;0;" _MAX_, "2;2", "2;2;" _MAX_, "4;4", "4;4;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((0),(2),(4))",
            "true",
            std::vector<const char*>{"0;0", "0;0;" _MAX_, "2;2", "2;2;" _MAX_, "4;4", "4;4;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((0),(2),(4),(6))",
            "true",
            std::vector<const char*>{"0;0", "0;0;" _MAX_, "2;2", "2;2;" _MAX_, "4;4", "4;4;" _MAX_, "6;6", "6;6;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in (1,2,3,4,5) or k > 10",
            "k in (1,2,3,4,5) or k > 10",
            std::vector<const char*>{"1;1", "1;1;" _MAX_, "2;2", "2;2;" _MAX_, "3;3", "3;3;" _MAX_, "4;4", "4;4;" _MAX_, "5;5", "5;5;" _MAX_, "10;" _MAX_, _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in (1,2,3,4,5) or k > 10",
            "true",
            std::vector<const char*>{"1;1", "1;1;" _MAX_, "2;2", "2;2;" _MAX_, "3;3", "3;3;" _MAX_, "4;4", "4;4;" _MAX_, "5;5", "5;5;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in (1,2,3,4,5) or k in (11,12,14,15)",
            "k in (4,5) or k in (11,12)",
            std::vector<const char*>{"4;4", "4;4;" _MAX_, "5;5", "5;5;" _MAX_, "11;11", "11;11;" _MAX_, "12;12", "12;12;" _MAX_}),
        std::make_tuple(
            "[{name=k;type=int64;sort_order=ascending}; {name=l;type=int64;sort_order=ascending;expression=k}; {name=a;type=int64}]",
            "[k;l]",
            "k in ((0),(2)) or k in ((4),(6))",
            "k in ((0),(2)) or k in ((4),(6))",
            std::vector<const char*>{"0;0", "0;0;" _MAX_, "2;2", "2;2;" _MAX_, "4;4", "4;4;" _MAX_, "6;6", "6;6;" _MAX_})
));

////////////////////////////////////////////////////////////////////////////////

class TPrepareExpressionTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::tuple<TConstExpressionPtr, const char*>>
    , public TCompareExpressionTest
{
protected:
    virtual void SetUp() override
    { }
};

TEST_F(TPrepareExpressionTest, Basic)
{
    auto schema = GetSampleTableSchema();

    auto expr1 = Make<TReferenceExpression>("k");
    auto expr2 = PrepareExpression(Stroka("k"), schema);

    EXPECT_TRUE(Equal(expr1, expr2))
        << "expr1: " << ::testing::PrintToString(expr1) << std::endl
        << "expr2: " << ::testing::PrintToString(expr2);

    expr1 = Make<TLiteralExpression>(MakeInt64(90));
    expr2 = PrepareExpression(Stroka("90"), schema);

    EXPECT_TRUE(Equal(expr1, expr2))
        << "expr1: " << ::testing::PrintToString(expr1) << std::endl
        << "expr2: " << ::testing::PrintToString(expr2);

    expr1 = Make<TReferenceExpression>("a"),
    expr2 = PrepareExpression(Stroka("k"), schema);

    EXPECT_FALSE(Equal(expr1, expr2))
        << "expr1: " << ::testing::PrintToString(expr1) << std::endl
        << "expr2: " << ::testing::PrintToString(expr2);

    auto str1 = Stroka("k + 3 - a > 4 * l and (k <= m or k + 1 < 3* l)");
    auto str2 = Stroka("k + 3 - a > 4 * l and (k <= m or k + 2 < 3* l)");

    expr1 = PrepareExpression(str1, schema);
    expr2 = PrepareExpression(str1, schema);

    EXPECT_TRUE(Equal(expr1, expr2))
        << "expr1: " << ::testing::PrintToString(expr1) << std::endl
        << "expr2: " << ::testing::PrintToString(expr2);

    expr2 = PrepareExpression(str2, schema);

    EXPECT_FALSE(Equal(expr1, expr2))
        << "expr1: " << ::testing::PrintToString(expr1) << std::endl
        << "expr2: " << ::testing::PrintToString(expr2);
}

TEST_P(TPrepareExpressionTest, Simple)
{
    auto schema = GetSampleTableSchema();
    auto& param = GetParam();

    auto expr1 = std::get<0>(param);
    auto expr2 = PrepareExpression(std::get<1>(param), schema);

    EXPECT_TRUE(Equal(expr1, expr2))
        << "expr1: " << ::testing::PrintToString(expr1) << std::endl
        << "expr2: " << ::testing::PrintToString(expr2);
}

INSTANTIATE_TEST_CASE_P(
    CheckExpressions,
    TPrepareExpressionTest,
    ::testing::Values(
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::GreaterOrEqual,
                Make<TReferenceExpression>("k"),
                Make<TLiteralExpression>(MakeInt64(90))),
            "k >= 90"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::Greater,
                Make<TReferenceExpression>("k"),
                Make<TLiteralExpression>(MakeInt64(90))),
            "k > 90"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::Equal,
                Make<TReferenceExpression>("k"),
                Make<TBinaryOpExpression>(EBinaryOp::Plus,
                    Make<TReferenceExpression>("a"),
                    Make<TReferenceExpression>("b"))),
            "k = a + b"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TFunctionExpression>("is_prefix",
                std::initializer_list<TConstExpressionPtr>({
                    Make<TLiteralExpression>(MakeString("abc")),
                    Make<TReferenceExpression>("s")})),
            "is_prefix(\"abc\", s)"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::Greater,
                Make<TUnaryOpExpression>(EUnaryOp::Minus,
                    Make<TReferenceExpression>("a")),
                Make<TLiteralExpression>(MakeInt64(-2))),
            "-a > -2"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::Minus,
                Make<TUnaryOpExpression>(EUnaryOp::Minus,
                    Make<TReferenceExpression>("a")),
                Make<TLiteralExpression>(MakeInt64(2))),
            "-a - 2"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::NotEqual,
                Make<TReferenceExpression>("a"),
                Make<TLiteralExpression>(MakeInt64(2))),
            "not a = 2"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::Or,
                Make<TBinaryOpExpression>(EBinaryOp::GreaterOrEqual,
                    Make<TReferenceExpression>("a"),
                    Make<TLiteralExpression>(MakeInt64(3))),
                Make<TBinaryOpExpression>(EBinaryOp::Less,
                    Make<TReferenceExpression>("a"),
                    Make<TLiteralExpression>(MakeInt64(2)))),
            "not ((a < 3) and (a >= 2))")
));

INSTANTIATE_TEST_CASE_P(
    CheckPriorities,
    TPrepareExpressionTest,
    ::testing::Values(
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::Modulo,
                Make<TBinaryOpExpression>(EBinaryOp::Divide,
                    Make<TBinaryOpExpression>(EBinaryOp::Multiply,
                        Make<TUnaryOpExpression>(EUnaryOp::Minus, Make<TReferenceExpression>("a")),
                        Make<TUnaryOpExpression>(EUnaryOp::Plus, Make<TReferenceExpression>("b"))),
                    Make<TUnaryOpExpression>(EUnaryOp::BitNot, Make<TReferenceExpression>("c"))),
                Make<TLiteralExpression>(MakeInt64(100))),
            "-a * +b / ~c % 100"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::Plus,
                Make<TBinaryOpExpression>(EBinaryOp::Multiply,
                    Make<TUnaryOpExpression>(EUnaryOp::Minus, Make<TReferenceExpression>("a")),
                    Make<TUnaryOpExpression>(EUnaryOp::Plus, Make<TReferenceExpression>("b"))),
                Make<TBinaryOpExpression>(EBinaryOp::Divide,
                    Make<TUnaryOpExpression>(EUnaryOp::BitNot, Make<TReferenceExpression>("c")),
                    Make<TLiteralExpression>(MakeInt64(100)))),
            "-a * +b + ~c / 100"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::BitOr,
                Make<TBinaryOpExpression>(EBinaryOp::BitAnd,
                    Make<TReferenceExpression>("k"),
                    Make<TBinaryOpExpression>(EBinaryOp::LeftShift,
                        Make<TBinaryOpExpression>(EBinaryOp::Plus,
                            Make<TReferenceExpression>("a"),
                            Make<TReferenceExpression>("b")),
                        Make<TReferenceExpression>("c"))),
                Make<TBinaryOpExpression>(EBinaryOp::RightShift,
                    Make<TReferenceExpression>("l"),
                    Make<TReferenceExpression>("m"))),
            "k & a + b << c | l >> m"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::NotEqual,
                Make<TBinaryOpExpression>(EBinaryOp::Greater,
                    Make<TReferenceExpression>("c"),
                    Make<TReferenceExpression>("b")),
                Make<TBinaryOpExpression>(EBinaryOp::Less,
                    Make<TReferenceExpression>("a"),
                    Make<TReferenceExpression>("b"))),
            "c > b != a < b"),
        std::tuple<TConstExpressionPtr, const char*>(
            Make<TBinaryOpExpression>(EBinaryOp::Or,
                Make<TBinaryOpExpression>(EBinaryOp::NotEqual,
                    Make<TBinaryOpExpression>(EBinaryOp::Less,
                        Make<TReferenceExpression>("a"),
                        Make<TReferenceExpression>("b")),
                    Make<TBinaryOpExpression>(EBinaryOp::Greater,
                        Make<TReferenceExpression>("c"),
                        Make<TReferenceExpression>("b"))),
                Make<TBinaryOpExpression>(EBinaryOp::And,
                    Make<TBinaryOpExpression>(EBinaryOp::GreaterOrEqual,
                        Make<TReferenceExpression>("k"),
                        Make<TReferenceExpression>("l")),
                    Make<TBinaryOpExpression>(EBinaryOp::LessOrEqual,
                        Make<TReferenceExpression>("k"),
                        Make<TReferenceExpression>("m")))),
            "NOT a < b = c > b OR k BETWEEN l AND m")
));

////////////////////////////////////////////////////////////////////////////////

using TArithmeticTestParam = std::tuple<EValueType, const char*, const char*, const char*, TUnversionedValue>;

class TArithmeticTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<TArithmeticTestParam>
    , public TCompareExpressionTest
{
protected:
    virtual void SetUp() override
    { }
};

TEST_P(TArithmeticTest, ConstantFolding)
{
    auto schema = GetSampleTableSchema();
    auto& param = GetParam();
    auto& lhs = std::get<1>(param);
    auto& op = std::get<2>(param);
    auto& rhs = std::get<3>(param);
    auto expected = Make<TLiteralExpression>(std::get<4>(param));

    auto got = PrepareExpression(Stroka(lhs) + op + rhs, schema);

    EXPECT_TRUE(Equal(got, expected))
        << "got: " <<  ::testing::PrintToString(got) << std::endl
        << "expected: " <<  ::testing::PrintToString(expected) << std::endl;
}

TEST_F(TArithmeticTest, ConstantDivisorsFolding)
{
    auto schema = GetSampleTableSchema();
    auto expr1 = PrepareExpression("k / 100 / 2", schema);
    auto expr2 = PrepareExpression("k / 200", schema);

    EXPECT_TRUE(Equal(expr1, expr2))
        << "expr1: " <<  ::testing::PrintToString(expr1) << std::endl
        << "expr2: " <<  ::testing::PrintToString(expr2) << std::endl;

    expr1 = PrepareExpression("k / 3102228988 / 4021316745", schema);
    expr2 = PrepareExpression("k / (3102228988 * 4021316745)", schema);

    EXPECT_FALSE(Equal(expr1, expr2))
        << "expr1: " <<  ::testing::PrintToString(expr1) << std::endl
        << "expr2: " <<  ::testing::PrintToString(expr2) << std::endl;

}

TEST_P(TArithmeticTest, Evaluate)
{
    auto& param = GetParam();
    auto type = std::get<0>(param);
    auto& lhs = std::get<1>(param);
    auto& op = std::get<2>(param);
    auto& rhs = std::get<3>(param);
    auto& expected = std::get<4>(param);

    TUnversionedValue result;
    TCGVariables variables;

    auto columns = GetSampleTableSchema().Columns();
    columns[0].Type = type;
    columns[1].Type = type;
    auto schema = TTableSchema(columns);

    auto expr = PrepareExpression(Stroka("k") + op + "l", schema);

    auto callback = Profile(expr, schema, nullptr, &variables)();
    auto row = NTableClient::BuildRow(Stroka("k=") + lhs + ";l=" + rhs, schema, true);

    TQueryStatistics statistics;
    auto permanentBuffer = New<TRowBuffer>();
    auto outputBuffer = New<TRowBuffer>();
    auto intermediateBuffer = New<TRowBuffer>();

    // NB: function contexts need to be destroyed before callback since it hosts destructors.
    TExecutionContext executionContext;
    executionContext.PermanentBuffer = permanentBuffer;
    executionContext.OutputBuffer = outputBuffer;
    executionContext.IntermediateBuffer = intermediateBuffer;
    executionContext.Statistics = &statistics;
#ifndef NDEBUG
    volatile int dummy;
    executionContext.StackSizeGuardHelper = reinterpret_cast<size_t>(&dummy);
#endif

    callback(variables.GetOpaqueData(), &result, row, &executionContext);

    EXPECT_EQ(result, expected)
        << "row: " << ::testing::PrintToString(row);
}

INSTANTIATE_TEST_CASE_P(
    TArithmeticTest,
    TArithmeticTest,
    ::testing::Values(
        TArithmeticTestParam(EValueType::Int64, "1", "+", "2", MakeInt64(3)),
        TArithmeticTestParam(EValueType::Int64, "1", "-", "2", MakeInt64(-1)),
        TArithmeticTestParam(EValueType::Int64, "3", "*", "2", MakeInt64(6)),
        TArithmeticTestParam(EValueType::Int64, "6", "/", "2", MakeInt64(3)),
        TArithmeticTestParam(EValueType::Int64, "6", "%", "4", MakeInt64(2)),
        TArithmeticTestParam(EValueType::Int64, "6", "<<", "2", MakeInt64(24)),
        TArithmeticTestParam(EValueType::Int64, "6", ">>", "1", MakeInt64(3)),
        TArithmeticTestParam(EValueType::Int64, "1234567", "|", "1111111", MakeInt64(1242823)),
        TArithmeticTestParam(EValueType::Int64, "1234567", "&", "1111111", MakeInt64(1102855)),
        TArithmeticTestParam(EValueType::Int64, "6", ">", "4", MakeBoolean(true)),
        TArithmeticTestParam(EValueType::Int64, "6", "<", "4", MakeBoolean(false)),
        TArithmeticTestParam(EValueType::Int64, "6", ">=", "4", MakeBoolean(true)),
        TArithmeticTestParam(EValueType::Int64, "6", "<=", "4", MakeBoolean(false)),
        TArithmeticTestParam(EValueType::Int64, "6", ">=", "6", MakeBoolean(true)),
        TArithmeticTestParam(EValueType::Int64, "6", "<=", "6", MakeBoolean(true)),
        TArithmeticTestParam(EValueType::Uint64, "1u", "+", "2u", MakeUint64(3)),
        TArithmeticTestParam(EValueType::Uint64, "1u", "-", "2u", MakeUint64(-1)),
        TArithmeticTestParam(EValueType::Uint64, "3u", "*", "2u", MakeUint64(6)),
        TArithmeticTestParam(EValueType::Uint64, "6u", "/", "2u", MakeUint64(3)),
        TArithmeticTestParam(EValueType::Uint64, "6u", "%", "4u", MakeUint64(2)),
        TArithmeticTestParam(EValueType::Uint64, "6u", "<<", "2u", MakeUint64(24)),
        TArithmeticTestParam(EValueType::Uint64, "6u", ">>", "1u", MakeUint64(3)),
        TArithmeticTestParam(EValueType::Uint64, "1234567u", "|", "1111111u", MakeUint64(1242823)),
        TArithmeticTestParam(EValueType::Uint64, "1234567u", "&", "1111111u", MakeUint64(1102855)),
        TArithmeticTestParam(EValueType::Uint64, "6u", ">", "4u", MakeBoolean(true)),
        TArithmeticTestParam(EValueType::Uint64, "6u", "<", "4u", MakeBoolean(false)),
        TArithmeticTestParam(EValueType::Uint64, "6u", ">=", "4u", MakeBoolean(true)),
        TArithmeticTestParam(EValueType::Uint64, "6u", "<=", "4u", MakeBoolean(false)),
        TArithmeticTestParam(EValueType::Uint64, "6u", ">=", "6u", MakeBoolean(true)),
        TArithmeticTestParam(EValueType::Uint64, "6u", "<=", "6u", MakeBoolean(true))
));

////////////////////////////////////////////////////////////////////////////////

class TTernaryLogicTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::tuple<EBinaryOp, TValue, TValue, TValue>>
    , public TCompareExpressionTest
{
protected:
    virtual void SetUp() override
    { }
};

TEST_P(TTernaryLogicTest, Evaluate)
{
    auto& param = GetParam();

    auto op = std::get<0>(param);
    auto lhs = std::get<1>(param);
    auto rhs = std::get<2>(param);
    auto expected = std::get<3>(param);

    TUnversionedValue result;
    TCGVariables variables;
    auto buffer = New<TRowBuffer>();
    auto row = NTableClient::BuildRow("", TTableSchema(), true);

    auto expr1 = New<TBinaryOpExpression>(EValueType::Boolean, op,
        New<TLiteralExpression>(EValueType::Boolean, lhs),
        New<TLiteralExpression>(EValueType::Boolean, rhs));

    auto expr2 = New<TBinaryOpExpression>(EValueType::Boolean, op,
        New<TLiteralExpression>(EValueType::Boolean, rhs),
        New<TLiteralExpression>(EValueType::Boolean, lhs));

    // NB: function contexts need to be destroyed before callback since it hosts destructors.
    TExecutionContext executionContext;
    executionContext.PermanentBuffer = buffer;
    executionContext.OutputBuffer = buffer;
    executionContext.IntermediateBuffer = buffer;
#ifndef NDEBUG
    volatile int dummy;
    executionContext.StackSizeGuardHelper = reinterpret_cast<size_t>(&dummy);
#endif

    TCGVariables variables1;
    auto compiledExpr1 = Profile(expr1, TTableSchema(), nullptr, &variables1)();
    compiledExpr1(variables1.GetOpaqueData(), &result, row, &executionContext);
    EXPECT_TRUE(CompareRowValues(result, expected) == 0);

    TCGVariables variables2;
    auto compiledExpr2 = Profile(expr2, TTableSchema(), nullptr, &variables2)();
    compiledExpr2(variables2.GetOpaqueData(), &result, row, &executionContext);
    EXPECT_TRUE(CompareRowValues(result, expected) == 0);
}

INSTANTIATE_TEST_CASE_P(
    AndOr,
    TTernaryLogicTest,
    ::testing::Values(
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::And,
            MakeBoolean(true),
            MakeBoolean(true),
            MakeBoolean(true)),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::And,
            MakeBoolean(true),
            MakeBoolean(false),
            MakeBoolean(false)),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::And,
            MakeBoolean(false),
            MakeBoolean(false),
            MakeBoolean(false)),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::And,
            MakeBoolean(false),
            MakeNull(),
            MakeBoolean(false)),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::And,
            MakeBoolean(true),
            MakeNull(),
            MakeNull()),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::And,
            MakeNull(),
            MakeNull(),
            MakeNull()),

        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::Or,
            MakeBoolean(true),
            MakeBoolean(true),
            MakeBoolean(true)),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::Or,
            MakeBoolean(true),
            MakeBoolean(false),
            MakeBoolean(true)),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::Or,
            MakeBoolean(false),
            MakeBoolean(false),
            MakeBoolean(false)),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::Or,
            MakeBoolean(false),
            MakeNull(),
            MakeNull()),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::Or,
            MakeBoolean(true),
            MakeNull(),
            MakeBoolean(true)),
        std::tuple<EBinaryOp, TValue, TValue, TValue>(
            EBinaryOp::Or,
            MakeNull(),
            MakeNull(),
            MakeNull())
));

////////////////////////////////////////////////////////////////////////////////

using TCompareWithNullTestParam = std::tuple<const char*, const char*, TUnversionedValue>;

class TCompareWithNullTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<TCompareWithNullTestParam>
    , public TCompareExpressionTest
{
protected:
    virtual void SetUp() override
    { }
};

TEST_P(TCompareWithNullTest, Simple)
{
    auto& param = GetParam();
    auto& rowString = std::get<0>(param);
    auto& exprString = std::get<1>(param);
    auto& expected = std::get<2>(param);

    TUnversionedValue result;
    TCGVariables variables;
    auto schema = GetSampleTableSchema();

    auto row = NTableClient::BuildRow(rowString, schema, true);
    auto expr = PrepareExpression(exprString, schema);
    auto callback = Profile(expr, schema, nullptr, &variables)();

    TQueryStatistics statistics;
    auto permanentBuffer = New<TRowBuffer>();
    auto outputBuffer = New<TRowBuffer>();
    auto intermediateBuffer = New<TRowBuffer>();

    // NB: function contexts need to be destroyed before callback since it hosts destructors.
    TExecutionContext executionContext;
    executionContext.PermanentBuffer = permanentBuffer;
    executionContext.OutputBuffer = outputBuffer;
    executionContext.IntermediateBuffer = intermediateBuffer;
    executionContext.Statistics = &statistics;
#ifndef NDEBUG
    volatile int dummy;
    executionContext.StackSizeGuardHelper = reinterpret_cast<size_t>(&dummy);
#endif

    callback(variables.GetOpaqueData(), &result, row, &executionContext);

    EXPECT_EQ(result, expected)
        << "row: " << ::testing::PrintToString(rowString) << std::endl
        << "expr: " << ::testing::PrintToString(exprString) << std::endl;
}

INSTANTIATE_TEST_CASE_P(
    TCompareWithNullTest,
    TCompareWithNullTest,
    ::testing::Values(
        TCompareWithNullTestParam("k=1", "l != k", MakeBoolean(true)),
        TCompareWithNullTestParam("k=1", "l = k", MakeBoolean(false)),
        TCompareWithNullTestParam("k=1", "l < k", MakeBoolean(true)),
        TCompareWithNullTestParam("k=1", "l > k", MakeBoolean(false)),
        TCompareWithNullTestParam("k=1", "k <= l", MakeBoolean(false)),
        TCompareWithNullTestParam("k=1", "k >= l", MakeBoolean(true)),
        TCompareWithNullTestParam("k=1", "l != m", MakeBoolean(false)),
        TCompareWithNullTestParam("k=1", "l = m", MakeBoolean(true)),
        TCompareWithNullTestParam("k=1", "l < m", MakeBoolean(false)),
        TCompareWithNullTestParam("k=1", "l > m", MakeBoolean(false)),
        TCompareWithNullTestParam("k=1", "m <= l", MakeBoolean(true)),
        TCompareWithNullTestParam("k=1", "m >= l", MakeBoolean(true))
));

////////////////////////////////////////////////////////////////////////////////

using TEvaluateAggregationParam = std::tuple<
    const char*,
    EValueType,
    TUnversionedValue,
    TUnversionedValue,
    TUnversionedValue>;

class TEvaluateAggregationTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<TEvaluateAggregationParam>
{ };

TEST_P(TEvaluateAggregationTest, Basic)
{
    const auto& param = GetParam();
    const auto& aggregateName = std::get<0>(param);
    auto type = std::get<1>(param);
    auto value1 = std::get<2>(param);
    auto value2 = std::get<3>(param);
    auto expected = std::get<4>(param);

    auto registry = BuiltinAggregateCG;
    auto aggregate = registry->GetAggregate(aggregateName);
    auto callbacks = CodegenAggregate(aggregate->Profile(type, type, type, aggregateName));

    auto permanentBuffer = New<TRowBuffer>();
    auto outputBuffer = New<TRowBuffer>();
    auto intermediateBuffer = New<TRowBuffer>();

    auto buffer = New<TRowBuffer>();
    TExecutionContext executionContext;
    executionContext.PermanentBuffer = buffer;
    executionContext.OutputBuffer = buffer;
    executionContext.IntermediateBuffer = buffer;
#ifndef NDEBUG
    volatile int dummy;
    executionContext.StackSizeGuardHelper = reinterpret_cast<size_t>(&dummy);
#endif

    TUnversionedValue tmp;
    TUnversionedValue state1;
    callbacks.Init(&executionContext, &state1);
    EXPECT_EQ(EValueType::Null, state1.Type);

    callbacks.Update(&executionContext, &tmp, &state1, &value1);
    state1 = tmp;
    EXPECT_EQ(value1, state1);

    TUnversionedValue state2;
    callbacks.Init(&executionContext, &state2);
    EXPECT_EQ(EValueType::Null, state2.Type);

    callbacks.Update(&executionContext, &tmp, &state2, &value2);
    state2 = tmp;
    EXPECT_EQ(value2, state2);

    callbacks.Merge(&executionContext, &tmp, &state1, &state2);
    EXPECT_EQ(expected, tmp);

    TUnversionedValue result;
    callbacks.Finalize(&executionContext, &result, &tmp);
    EXPECT_EQ(expected, result);
}

INSTANTIATE_TEST_CASE_P(
    EvaluateAggregationTest,
    TEvaluateAggregationTest,
    ::testing::Values(
        TEvaluateAggregationParam{
            "sum",
            EValueType::Int64,
            MakeUnversionedSentinelValue(EValueType::Null),
            MakeUnversionedSentinelValue(EValueType::Null),
            MakeUnversionedSentinelValue(EValueType::Null)},
        TEvaluateAggregationParam{
            "sum",
            EValueType::Int64,
            MakeUnversionedSentinelValue(EValueType::Null),
            MakeInt64(1),
            MakeInt64(1)},
        TEvaluateAggregationParam{
            "sum",
            EValueType::Int64,
            MakeInt64(1),
            MakeInt64(2),
            MakeInt64(3)},
        TEvaluateAggregationParam{
            "sum",
            EValueType::Uint64,
            MakeUint64(1),
            MakeUint64(2),
            MakeUint64(3)},
        TEvaluateAggregationParam{
            "max",
            EValueType::Int64,
            MakeInt64(10),
            MakeInt64(20),
            MakeInt64(20)},
        TEvaluateAggregationParam{
            "min",
            EValueType::Int64,
            MakeInt64(10),
            MakeInt64(20),
            MakeInt64(10)}
));

////////////////////////////////////////////////////////////////////////////////

void EvaluateExpression(
    TConstExpressionPtr expr,
    const Stroka& rowString,
    const TTableSchema& schema,
    TUnversionedValue* result,
    TRowBufferPtr buffer)
{
    TCGVariables variables;

    auto callback = Profile(expr, schema, nullptr, &variables)();

    auto row = NTableClient::BuildRow(rowString, schema, true);

    TQueryStatistics statistics;
    // NB: function contexts need to be destroyed before callback since it hosts destructors.
    TExecutionContext executionContext;
    executionContext.PermanentBuffer = buffer;
    executionContext.OutputBuffer = buffer;
    executionContext.IntermediateBuffer = buffer;
    executionContext.Statistics = &statistics;
#ifndef NDEBUG
    volatile int dummy;
    executionContext.StackSizeGuardHelper = reinterpret_cast<size_t>(&dummy);
#endif

    callback(variables.GetOpaqueData(), result, row, &executionContext);
}

class TEvaluateExpressionTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::tuple<const char*, const char*, TUnversionedValue>>
{ };

TEST_P(TEvaluateExpressionTest, Basic)
{
    const auto& param = GetParam();
    const auto& rowString = std::get<0>(param);
    const auto& exprString = std::get<1>(param);
    const auto& expected = std::get<2>(param);

    TTableSchema schema({
        TColumnSchema("i1", EValueType::Int64),
        TColumnSchema("i2", EValueType::Int64),
        TColumnSchema("u1", EValueType::Uint64),
        TColumnSchema("u2", EValueType::Uint64)
    });

    auto expr = PrepareExpression(exprString, schema);

    auto buffer = New<TRowBuffer>();
    TUnversionedValue result;
    EvaluateExpression(expr, rowString, schema, &result, buffer);

    EXPECT_EQ(result, expected);
}

INSTANTIATE_TEST_CASE_P(
    EvaluateExpressionTest,
    TEvaluateExpressionTest,
    ::testing::Values(
        std::tuple<const char*, const char*, TUnversionedValue>(
            "",
            "lower('')",
            MakeString("")),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "",
            "lower('ПрИвЕт, КаК ДеЛа?')",
            MakeString("привет, как дела?")),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "",
            "concat('', '')",
            MakeString("")),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "",
            "concat('abc', '')",
            MakeString("abc")),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "",
            "concat('', 'def')",
            MakeString("def")),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "",
            "concat('abc', 'def')",
            MakeString("abcdef")),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=33;i2=22",
            "i1 + i2",
            MakeInt64(33 + 22)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=33",
            "-i1",
            MakeInt64(-33)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=0",
            "uint64(i1)",
            MakeUint64(0)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "u1=0",
            "int64(u1)",
            MakeInt64(0)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "u1=18446744073709551615u",
            "int64(u1)",
            MakeInt64(-1)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=9223372036854775807",
            "uint64(i1)",
            MakeUint64(9223372036854775807ULL)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=-9223372036854775808",
            "uint64(i1)",
            MakeUint64(9223372036854775808ULL))

));

INSTANTIATE_TEST_CASE_P(
    EvaluateTimestampExpressionTest,
    TEvaluateExpressionTest,
    ::testing::Values(
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=1446325284",
            "format_timestamp(i1, '')",
            MakeString("")),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=1446325284",
            "format_timestamp(i1, '%Y-%m-%dT%H:%M:%S')",
            MakeString("2015-10-31T21:01:24")),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=1446325284",
            "timestamp_floor_hour(i1)",
            MakeInt64(1446325200)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=1446325284",
            "timestamp_floor_day(i1)",
            MakeInt64(1446249600)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=1446325284",
            "timestamp_floor_week(i1)",
            MakeInt64(1445817600)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=1446325284",
            "timestamp_floor_month(i1)",
            MakeInt64(1443657600)),
        std::tuple<const char*, const char*, TUnversionedValue>(
            "i1=1446325284",
            "timestamp_floor_year(i1)",
            MakeInt64(1420070400))
));

class TFormatTimestampExpressionTest
    : public ::testing::Test
{
protected:
    virtual void SetUp() override
    { }
};

TEST_F(TFormatTimestampExpressionTest, TooSmallTimestamp)
{
    TTableSchema schema;
    TKeyColumns keyColumns;

    auto expr = PrepareExpression("format_timestamp(-62135596801, '')", schema);
    auto buffer = New<TRowBuffer>();

    TUnversionedValue result;

    EXPECT_THROW_THAT(
        [&] { EvaluateExpression(expr, "", schema, &result, buffer); },
        HasSubstr("Timestamp is smaller than minimal value"));
}

TEST_F(TFormatTimestampExpressionTest, TooLargeTimestamp)
{
    TTableSchema schema;
    TKeyColumns keyColumns;

    auto expr = PrepareExpression("format_timestamp(253402300800, '%Y%m%d')", schema);
    auto buffer = New<TRowBuffer>();

    TUnversionedValue result;

    EXPECT_THROW_THAT(
        [&] { EvaluateExpression(expr, "", schema, &result, buffer); },
        HasSubstr("Timestamp is greater than maximal value"));
}

TEST_F(TFormatTimestampExpressionTest, InvalidFormat)
{
    TTableSchema schema;
    TKeyColumns keyColumns;

    auto expr = PrepareExpression("format_timestamp(0, '11111111112222222222333333333344')", schema);
    auto buffer = New<TRowBuffer>();

    TUnversionedValue result;

    EXPECT_THROW_THAT(
        [&] { EvaluateExpression(expr, "", schema, &result, buffer); },
        HasSubstr("Format string is too long"));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NQueryClient
} // namespace NYT
