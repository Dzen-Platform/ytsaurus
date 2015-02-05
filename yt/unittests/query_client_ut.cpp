#include "stdafx.h"
#include "framework.h"
#include "versioned_table_client_ut.h"

#include <core/concurrency/action_queue.h>

#include <ytlib/object_client/helpers.h>

#include <ytlib/query_client/config.h>
#include <ytlib/query_client/plan_fragment.h>
#include <ytlib/query_client/callbacks.h>
#include <ytlib/query_client/helpers.h>
#include <ytlib/query_client/coordinator.h>
#include <ytlib/query_client/evaluator.h>
#include <ytlib/query_client/plan_helpers.h>
#include <ytlib/query_client/helpers.h>
#include <ytlib/query_client/plan_fragment.pb.h>

#include <ytlib/new_table_client/schema.h>
#include <ytlib/new_table_client/name_table.h>
#include <ytlib/new_table_client/schemaful_reader.h>
#include <ytlib/new_table_client/schemaful_writer.h>

#include <tuple>

#define _MIN_ "<\"type\"=\"min\">#"
#define _MAX_ "<\"type\"=\"max\">#"

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

void PrintTo(const TOwningKey& key, ::std::ostream* os)
{
    *os << KeyToYson(key.Get());
}

void PrintTo(const TKey& key, ::std::ostream* os)
{
    *os << KeyToYson(key);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT


namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

void PrintTo(const TConstExpressionPtr& expr, ::std::ostream* os)
{
    *os << InferName(expr);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT


namespace NYT {
namespace NQueryClient {
namespace {

////////////////////////////////////////////////////////////////////////////////

using namespace NConcurrency;
using namespace NYPath;
using namespace NObjectClient;
using namespace NVersionedTableClient;
using namespace NNodeTrackerClient;

using ::testing::_;
using ::testing::StrictMock;
using ::testing::HasSubstr;
using ::testing::ContainsRegex;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::AllOf;

////////////////////////////////////////////////////////////////////////////////

class TPrepareCallbacksMock
    : public IPrepareCallbacks
{
public:
    MOCK_METHOD2(GetInitialSplit, TFuture<TDataSplit>(
        const TYPath&,
        TTimestamp));
};

MATCHER_P(HasCounter, expectedCounter, "")
{
    auto objectId = GetObjectIdFromDataSplit(arg);
    auto cellTag = CellTagFromId(objectId);
    auto counter = CounterFromId(objectId);

    if (cellTag != 0x42) {
        *result_listener << "cell id is bad";
        return false;
    }

    if (counter != expectedCounter) {
        *result_listener
            << "actual counter id is " << counter << " while "
            << "expected counter id is " << expectedCounter;
        return false;
    }

    return true;
}

MATCHER_P(HasSplitsCount, expectedCount, "")
{
    if (arg.size() != expectedCount) {
        *result_listener
            << "actual splits count is " << arg.size() << " while "
            << "expected count is " << expectedCount;
        return false;
    }

    return true;
}

MATCHER_P(HasLowerBound, encodedLowerBound, "")
{
    auto expected = BuildKey(encodedLowerBound);
    auto actual = GetLowerBoundFromDataSplit(arg);

    auto result = CompareRows(expected, actual);

    if (result != 0 && result_listener->IsInterested()) {
        *result_listener << "expected lower bound to be ";
        PrintTo(expected, result_listener->stream());
        *result_listener << " while actual is ";
        PrintTo(actual, result_listener->stream());
        *result_listener
            << " which is "
            << (result > 0 ? "greater" : "lesser")
            << " than expected";
    }

    return result == 0;
}

MATCHER_P(HasUpperBound, encodedUpperBound, "")
{
    auto expected = BuildKey(encodedUpperBound);
    auto actual = GetUpperBoundFromDataSplit(arg);

    auto result = CompareRows(expected, actual);

    if (result != 0) {
        *result_listener << "expected upper bound to be ";
        PrintTo(expected, result_listener->stream());
        *result_listener << " while actual is ";
        PrintTo(actual, result_listener->stream());
        *result_listener
            << " which is "
            << (result > 0 ? "greater" : "lesser")
            << " than expected";
        return false;
    }

    return true;
}

MATCHER_P(HasSchema, expectedSchema, "")
{
    auto schema = GetTableSchemaFromDataSplit(arg);

    if (schema != expectedSchema) {
        //*result_listener
        //    << "actual counter id is " << schema << " while "
        //    << "expected counter id is " << expectedSchema;
        return false;
    }

    return true;
}

TKeyColumns GetSampleKeyColumns()
{
    TKeyColumns keyColumns;
    keyColumns.push_back("k");
    keyColumns.push_back("l");
    keyColumns.push_back("m");
    return keyColumns;
}

TKeyColumns GetSampleKeyColumns2()
{
    TKeyColumns keyColumns;
    keyColumns.push_back("k");
    keyColumns.push_back("l");
    keyColumns.push_back("m");
    keyColumns.push_back("s");
    return keyColumns;
}

TTableSchema GetSampleTableSchema()
{
    TTableSchema tableSchema;
    tableSchema.Columns().push_back({ "k", EValueType::Int64 });
    tableSchema.Columns().push_back({ "l", EValueType::Int64 });
    tableSchema.Columns().push_back({ "m", EValueType::Int64 });
    tableSchema.Columns().push_back({ "a", EValueType::Int64 });
    tableSchema.Columns().push_back({ "b", EValueType::Int64 });
    tableSchema.Columns().push_back({ "c", EValueType::Int64 });
    tableSchema.Columns().push_back({ "s", EValueType::String });
    tableSchema.Columns().push_back({ "u", EValueType::String });
    return tableSchema;
}

template <class T>
TFuture<T> WrapInFuture(const T& value)
{
    return MakeFuture(TErrorOr<T>(value));
}

TFuture<void> WrapVoidInFuture()
{
    return MakeFuture(TErrorOr<void>());
}

TDataSplit MakeSimpleSplit(const TYPath& path, ui64 counter = 0)
{
    TDataSplit dataSplit;

    ToProto(
        dataSplit.mutable_chunk_id(),
        MakeId(EObjectType::Table, 0x42, counter, 0xdeadbabe));

    SetKeyColumns(&dataSplit, GetSampleKeyColumns());
    SetTableSchema(&dataSplit, GetSampleTableSchema());

    return dataSplit;
}

TDataSplit MakeSplit(const std::vector<TColumnSchema>& columns)
{
    TDataSplit dataSplit;

    ToProto(
        dataSplit.mutable_chunk_id(),
        MakeId(EObjectType::Table, 0x42, 0, 0xdeadbabe));

    TKeyColumns keyColumns;
    SetKeyColumns(&dataSplit, keyColumns);

    TTableSchema tableSchema;
    tableSchema.Columns() = columns;
    SetTableSchema(&dataSplit, tableSchema);

    return dataSplit;
}

TFuture<TDataSplit> RaiseTableNotFound(
    const TYPath& path,
    TTimestamp)
{
    return MakeFuture(TErrorOr<TDataSplit>(TError(Format(
        "Could not find table %v",
        path))));
}

template <class TFunctor, class TMatcher>
void EXPECT_THROW_THAT(TFunctor functor, TMatcher matcher)
{
    bool exceptionThrown = false;
    try {
        functor();
    } catch (const std::exception& ex) {
        exceptionThrown = true;
        EXPECT_THAT(ex.what(), matcher);
    }
    EXPECT_TRUE(exceptionThrown);
}

class TQueryPrepareTest
    : public ::testing::Test
{
protected:
    template <class TMatcher>
    void ExpectPrepareThrowsWithDiagnostics(
        const Stroka& query,
        TMatcher matcher)
    {
        EXPECT_THROW_THAT(
            [&] { PreparePlanFragment(&PrepareMock_, query); },
            matcher);
    }

    StrictMock<TPrepareCallbacksMock> PrepareMock_;

};

TEST_F(TQueryPrepareTest, Simple)
{
    EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
        .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));

    PreparePlanFragment(&PrepareMock_, "a, b FROM [//t] WHERE k > 3");
}

TEST_F(TQueryPrepareTest, BadSyntax)
{
    ExpectPrepareThrowsWithDiagnostics(
        "bazzinga mu ha ha ha",
        HasSubstr("syntax error"));
}

TEST_F(TQueryPrepareTest, BadTableName)
{
    EXPECT_CALL(PrepareMock_, GetInitialSplit("//bad/table", _))
        .WillOnce(Invoke(&RaiseTableNotFound));

    ExpectPrepareThrowsWithDiagnostics(
        "a, b from [//bad/table]",
        HasSubstr("Could not find table //bad/table"));
}

TEST_F(TQueryPrepareTest, BadColumnNameInProject)
{
    EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
        .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));

    ExpectPrepareThrowsWithDiagnostics(
        "foo from [//t]",
        HasSubstr("Undefined reference \"foo\""));
}

TEST_F(TQueryPrepareTest, BadColumnNameInFilter)
{
    EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
        .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));

    ExpectPrepareThrowsWithDiagnostics(
        "k from [//t] where bar = 1",
        HasSubstr("Undefined reference \"bar\""));
}

TEST_F(TQueryPrepareTest, BadTypecheck)
{
    EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
        .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));

    ExpectPrepareThrowsWithDiagnostics(
        "k from [//t] where a > \"xyz\"",
        ContainsRegex("Type mismatch in expression .*"));
}

TEST_F(TQueryPrepareTest, TooBigQuery)
{
    Stroka query = "k from [//t] where a ";
    for (int i = 0; i < 50 ; ++i) {
        query += "+ " + ToString(i);
    }
    query += " > 0";

    EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
        .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));

    ExpectPrepareThrowsWithDiagnostics(
        query,
        ContainsRegex("Plan fragment depth limit exceeded"));
}

TEST_F(TQueryPrepareTest, BigQuery)
{
    Stroka query = "k from [//t] where a in (0";
    for (int i = 1; i < 1000; ++i) {
        query += ", " + ToString(i);
    }
    query += ")";

    EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
        .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));

    PreparePlanFragment(&PrepareMock_, query);
}

TEST_F(TQueryPrepareTest, ResultSchemaCollision)
{
    EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
        .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));

    ExpectPrepareThrowsWithDiagnostics(
        "a as x, b as x FROM [//t] WHERE k > 3",
        ContainsRegex("Duplicate column .*"));
}

TEST_F(TQueryPrepareTest, MisuseAggregateFunction)
{
    EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
        .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));

    ExpectPrepareThrowsWithDiagnostics(
        "sum(sum(a)) from [//t] group by k",
        ContainsRegex("Misuse of aggregate function .*"));

    EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
        .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));

    ExpectPrepareThrowsWithDiagnostics(
        "sum(a) from [//t]",
        ContainsRegex("Misuse of aggregate function .*"));
}

////////////////////////////////////////////////////////////////////////////////

class TQueryCoordinateTest
    : public ::testing::Test
{
protected:
    virtual void SetUp() override
    {
        EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
            .WillOnce(Return(WrapInFuture(MakeSimpleSplit("//t"))));
    }

    void Coordinate(const Stroka& source, const TDataSplits& dataSplits, size_t subqueriesCount)
    {
        auto planFragment = PreparePlanFragment(&PrepareMock_, source);

        auto prunedSplits = GetPrunedSplits(planFragment->Query, dataSplits);

        EXPECT_EQ(prunedSplits.size(), subqueriesCount);
    }

    StrictMock<TPrepareCallbacksMock> PrepareMock_;
};

TEST_F(TQueryCoordinateTest, EmptySplit)
{
    TDataSplits emptySplits;

    EXPECT_NO_THROW({
        Coordinate("k from [//t]", emptySplits, 0);
    });
}

TEST_F(TQueryCoordinateTest, SingleSplit)
{
    TDataSplits singleSplit;
    singleSplit.emplace_back(MakeSimpleSplit("//t", 1));

    EXPECT_NO_THROW({
        Coordinate("k from [//t]", singleSplit, 1);
    });
}

TEST_F(TQueryCoordinateTest, UsesKeyToPruneSplits)
{
    TDataSplits splits;

    splits.emplace_back(MakeSimpleSplit("//t", 1));
    SetSorted(&splits.back(), true);
    SetLowerBound(&splits.back(), BuildKey("0;0;0"));
    SetUpperBound(&splits.back(), BuildKey("1;0;0"));

    splits.emplace_back(MakeSimpleSplit("//t", 2));
    SetSorted(&splits.back(), true);
    SetLowerBound(&splits.back(), BuildKey("1;0;0"));
    SetUpperBound(&splits.back(), BuildKey("2;0;0"));

    splits.emplace_back(MakeSimpleSplit("//t", 3));
    SetSorted(&splits.back(), true);
    SetLowerBound(&splits.back(), BuildKey("2;0;0"));
    SetUpperBound(&splits.back(), BuildKey("3;0;0"));

    EXPECT_NO_THROW({
        Coordinate("a from [//t] where k = 1 and l = 2 and m = 3", splits, 1);
    });
}

TEST_F(TQueryCoordinateTest, SimpleIn)
{
    TDataSplits singleSplit;
    singleSplit.emplace_back(MakeSimpleSplit("//t", 1));

    EXPECT_NO_THROW({
        Coordinate("k from [//t] where k in (1, 2, 3)", singleSplit, 3);
    });
}

TEST(TKeyRangeTest, Unite)
{
    auto k1 = BuildKey("1"); auto k2 = BuildKey("2");
    auto k3 = BuildKey("3"); auto k4 = BuildKey("4");
    auto mp = [] (const TKey& a, const TKey& b) {
        return std::make_pair(a, b);
    };

    EXPECT_EQ(mp(k1, k4), Unite(mp(k1, k2), mp(k3, k4)));
    EXPECT_EQ(mp(k1, k4), Unite(mp(k1, k3), mp(k2, k4)));
    EXPECT_EQ(mp(k1, k4), Unite(mp(k1, k4), mp(k2, k3)));
    EXPECT_EQ(mp(k1, k4), Unite(mp(k2, k3), mp(k1, k4)));
    EXPECT_EQ(mp(k1, k4), Unite(mp(k2, k4), mp(k1, k3)));
    EXPECT_EQ(mp(k1, k4), Unite(mp(k3, k4), mp(k1, k2)));
}

TEST(TKeyRangeTest, Intersect)
{
    auto k1 = BuildKey("1"); auto k2 = BuildKey("2");
    auto k3 = BuildKey("3"); auto k4 = BuildKey("4");
    auto mp = [] (const TKey& a, const TKey& b) {
        return std::make_pair(a, b);
    };

    EXPECT_TRUE(IsEmpty(Intersect(mp(k1, k2), mp(k3, k4))));
    EXPECT_EQ(mp(k2, k3), Intersect(mp(k1, k3), mp(k2, k4)));
    EXPECT_EQ(mp(k2, k3), Intersect(mp(k1, k4), mp(k2, k3)));
    EXPECT_EQ(mp(k2, k3), Intersect(mp(k2, k3), mp(k1, k4)));
    EXPECT_EQ(mp(k2, k3), Intersect(mp(k2, k4), mp(k1, k3)));
    EXPECT_TRUE(IsEmpty(Intersect(mp(k3, k4), mp(k1, k2))));

    EXPECT_EQ(mp(k1, k2), Intersect(mp(k1, k2), mp(k1, k3)));
    EXPECT_EQ(mp(k1, k2), Intersect(mp(k1, k3), mp(k1, k2)));

    EXPECT_EQ(mp(k3, k4), Intersect(mp(k3, k4), mp(k2, k4)));
    EXPECT_EQ(mp(k3, k4), Intersect(mp(k2, k4), mp(k3, k4)));

    EXPECT_EQ(mp(k1, k4), Intersect(mp(k1, k4), mp(k1, k4)));
}

TEST(TKeyRangeTest, IsEmpty)
{
    auto k1 = BuildKey("1"); auto k2 = BuildKey("2");
    auto mp = [] (const TKey& a, const TKey& b) {
        return std::make_pair(a, b);
    };

    EXPECT_TRUE(IsEmpty(mp(k1, k1)));
    EXPECT_TRUE(IsEmpty(mp(k2, k2)));

    EXPECT_TRUE(IsEmpty(mp(k2, k1)));
    EXPECT_FALSE(IsEmpty(mp(k1, k2)));

    EXPECT_TRUE(IsEmpty(mp(BuildKey("0;0;1"), BuildKey("0;0;0"))));
    EXPECT_FALSE(IsEmpty(mp(BuildKey("0;0;0"), BuildKey("0;0;1"))));
}

////////////////////////////////////////////////////////////////////////////////
// Refinement tests.

template <class TTypedExpression, class... TArgs>
static TConstExpressionPtr Make(TArgs&&... args)
{
    return New<TTypedExpression>(
        NullSourceLocation,
        EValueType::TheBottom,
        std::forward<TArgs>(args)...);
}

static TValue MakeInt64(i64 value)
{
    return MakeUnversionedInt64Value(value);
}

static TValue MakeString(const TStringBuf& value)
{
    return MakeUnversionedStringValue(value);
}

TKeyRange RefineKeyRange(
    const TKeyColumns& keyColumns,
    const TKeyRange& keyRange,
    const TConstExpressionPtr& predicate)
{
    TRowBuffer rowBuffer;

    auto keyTrie = ExtractMultipleConstraints(
        predicate,
        keyColumns,
        &rowBuffer);

    auto result = GetRangesFromTrieWithinRange(keyRange, keyTrie);

    if (result.empty()) {
        return std::make_pair(EmptyKey(), EmptyKey());
    } else if (result.size() == 1) {
        return result[0];
    } else {
        return keyRange;
    }
}

struct TRefineKeyRangeTestCase
{
    const char* InitialLeftBoundAsYson;
    const char* InitialRightBoundAsYson;

    const char* ConstraintColumnName;
    EBinaryOp ConstraintOpcode;
    i64 ConstraintValue;

    bool ResultIsEmpty;
    const char* ResultingLeftBoundAsYson;
    const char* ResultingRightBoundAsYson;

    TKey GetInitialLeftBound() const
    {
        return BuildKey(InitialLeftBoundAsYson);
    }

    TKey GetInitialRightBound() const
    {
        return BuildKey(InitialRightBoundAsYson);
    }

    TKey GetResultingLeftBound() const
    {
        return BuildKey(ResultingLeftBoundAsYson);
    }

    TKey GetResultingRightBound() const
    {
        return BuildKey(ResultingRightBoundAsYson);
    }
};

class TRefineKeyRangeTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<TRefineKeyRangeTestCase>
{
protected:
    virtual void SetUp() override
    { }

    void ExpectIsEmpty(const TKeyRange& keyRange)
    {
        EXPECT_TRUE(IsEmpty(keyRange))
            << "Left bound: " << ::testing::PrintToString(keyRange.first) << "; "
            << "Right bound: " << ::testing::PrintToString(keyRange.second);
    }
};

class TPrepareExpressionTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::tuple<TConstExpressionPtr, const char*>>
{
protected:
    virtual void SetUp() override
    { }

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
                || inLhs->Values.size() != inRhs->Values.size()
                || inLhs->Arguments.size() != inRhs->Arguments.size()) {
                return false;
            }
            for (int index = 0; index < inLhs->Values.size(); ++index) {
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

void PrintTo(const TRefineKeyRangeTestCase& testCase, ::std::ostream* os)
{
    *os
        << "{ "
        << "P: "
        << testCase.ConstraintColumnName << " "
        << GetBinaryOpcodeLexeme(testCase.ConstraintOpcode) << " "
        << testCase.ConstraintValue << ", "
        << "E: "
        << (testCase.ResultIsEmpty ? "True" : "False") << ", "
        << "L: "
        << ::testing::PrintToString(testCase.GetResultingLeftBound()) << ", "
        << "R: "
        << ::testing::PrintToString(testCase.GetResultingRightBound())
        << " }";
}

TEST_P(TRefineKeyRangeTest, Basic)
{
    auto testCase = GetParam();

    auto expr = Make<TBinaryOpExpression>(testCase.ConstraintOpcode,
        Make<TReferenceExpression>(testCase.ConstraintColumnName),
        Make<TLiteralExpression>(MakeInt64(testCase.ConstraintValue)));

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(
            testCase.GetInitialLeftBound(),
            testCase.GetInitialRightBound()),
        expr);

    if (testCase.ResultIsEmpty) {
        ExpectIsEmpty(result);
    } else {
        EXPECT_EQ(testCase.GetResultingLeftBound(), result.first);
        EXPECT_EQ(testCase.GetResultingRightBound(), result.second);
    }
}

TEST_P(TRefineKeyRangeTest, BasicReversed)
{
    auto testCase = GetParam();

    auto expr = Make<TBinaryOpExpression>(GetReversedBinaryOpcode(testCase.ConstraintOpcode),
        Make<TLiteralExpression>(MakeInt64(testCase.ConstraintValue)),
        Make<TReferenceExpression>(testCase.ConstraintColumnName));

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(
            testCase.GetInitialLeftBound(),
            testCase.GetInitialRightBound()),
        expr);

    if (testCase.ResultIsEmpty) {
        ExpectIsEmpty(result);
    } else {
        EXPECT_EQ(testCase.GetResultingLeftBound(), result.first);
        EXPECT_EQ(testCase.GetResultingRightBound(), result.second);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Here is a guideline on how to read this cases table.
//
// Basically, initial key range is specified in the first line
// (e. g. from `[0;0;0]` to `[100;100;100]`) and the constraint is on the second
// line (e. g. `k = 50`). Then there is a flag whether result is empty or not
// and also resulting boundaries.
//
// Keep in mind that there are three columns in schema (`k`, `l` and `m`).
//
// TODO(sandello): Plug in an expression parser here.
////////////////////////////////////////////////////////////////////////////////

// Equal, First component.
TRefineKeyRangeTestCase refineCasesForEqualOpcodeInFirstComponent[] = {
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Equal, 50,
        false, ("50"), ("50;" _MAX_)
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Equal, 1,
        false, ("1;1;1"), ("1;" _MAX_)
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Equal, 99,
        false, ("99"), ("99;" _MAX_)
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Equal, 100,
        false, ("100"), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Equal, 200,
        true, (""), ("")
    },
};
INSTANTIATE_TEST_CASE_P(
    EqualInFirstComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForEqualOpcodeInFirstComponent));

// NotEqual, First component.
TRefineKeyRangeTestCase refineCasesForNotEqualOpcodeInFirstComponent[] = {
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::NotEqual, 50,
        false, ("1;1;1"), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::NotEqual, 1,
        false, ("1;" _MAX_), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::NotEqual, 100,
        false, ("1;1;1"), ("100;")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::NotEqual, 200,
        false, ("1;1;1"), ("100;100;100")
    },
};
INSTANTIATE_TEST_CASE_P(
    NotEqualInFirstComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForNotEqualOpcodeInFirstComponent));

// Less, First component.
TRefineKeyRangeTestCase refineCasesForLessOpcodeInFirstComponent[] = {
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Less, 50,
        false, ("1;1;1"), ("50")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Less, 1,
        true, (""), ("")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Less, 100,
        false, ("1;1;1"), ("100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Less, 200,
        false, ("1;1;1"), ("100;100;100")
    },
};
INSTANTIATE_TEST_CASE_P(
    LessInFirstComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForLessOpcodeInFirstComponent));

// LessOrEqual, First component.
TRefineKeyRangeTestCase refineCasesForLessOrEqualOpcodeInFirstComponent[] = {
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::LessOrEqual, 50,
        false, ("1;1;1"), ("50;" _MAX_)
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::LessOrEqual, 1,
        false, ("1;1;1"), ("1;" _MAX_)
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::LessOrEqual, 99,
        false, ("1;1;1"), ("99;" _MAX_)
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::LessOrEqual, 100,
        false, ("1;1;1"), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::LessOrEqual, 200,
        false, ("1;1;1"), ("100;100;100")
    },
};
INSTANTIATE_TEST_CASE_P(
    LessOrEqualInFirstComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForLessOrEqualOpcodeInFirstComponent));

// Greater, First component.
TRefineKeyRangeTestCase refineCasesForGreaterOpcodeInFirstComponent[] = {
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Greater, 50,
        false, ("50;" _MAX_), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Greater, 0,
        false, ("1;1;1"), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Greater, 1,
        false, ("1;" _MAX_), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Greater, 100,
        true, (""), ("")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::Greater, 200,
        true, (""), ("")
    },
};
INSTANTIATE_TEST_CASE_P(
    GreaterInFirstComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForGreaterOpcodeInFirstComponent));

// GreaterOrEqual, First component.
TRefineKeyRangeTestCase refineCasesForGreaterOrEqualOpcodeInFirstComponent[] = {
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::GreaterOrEqual, 50,
        false, ("50"), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::GreaterOrEqual, 1,
        false, ("1;1;1"), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::GreaterOrEqual, 100,
        false, ("100"), ("100;100;100")
    },
    {
        ("1;1;1"), ("100;100;100"),
        "k", EBinaryOp::GreaterOrEqual, 200,
        true, (""), ("")
    },
};
INSTANTIATE_TEST_CASE_P(
    GreaterOrEqualInFirstComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForGreaterOrEqualOpcodeInFirstComponent));

////////////////////////////////////////////////////////////////////////////////

// Equal, Last component.
TRefineKeyRangeTestCase refineCasesForEqualOpcodeInLastComponent[] = {
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Equal, 50,
        false, ("1;1;50"), ("1;1;50;" _MAX_)
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Equal, 1,
        false, ("1;1;1"), ("1;1;1;" _MAX_)
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Equal, 99,
        false, ("1;1;99"), ("1;1;99;" _MAX_)
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Equal, 100,
        true, (""), ("")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Equal, 200,
        true, (""), ("")
    },
};
INSTANTIATE_TEST_CASE_P(
    EqualInLastComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForEqualOpcodeInLastComponent));

// NotEqual, Last component.
TRefineKeyRangeTestCase refineCasesForNotEqualOpcodeInLastComponent[] = {
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::NotEqual, 50,
        false, ("1;1;1"), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::NotEqual, 1,
        false, ("1;1;1;" _MAX_), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::NotEqual, 100,
        false, ("1;1;1"), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::NotEqual, 200,
        false, ("1;1;1"), ("1;1;100")
    },
};
INSTANTIATE_TEST_CASE_P(
    NotEqualInLastComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForNotEqualOpcodeInLastComponent));

// Less, Last component.
TRefineKeyRangeTestCase refineCasesForLessOpcodeInLastComponent[] = {
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Less, 50,
        false, ("1;1;1"), ("1;1;50")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Less, 1,
        true, (""), ("")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Less, 100,
        false, ("1;1;1"), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Less, 200,
        false, ("1;1;1"), ("1;1;100")
    },
};
INSTANTIATE_TEST_CASE_P(
    LessInLastComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForLessOpcodeInLastComponent));

// LessOrEqual, Last component.
TRefineKeyRangeTestCase refineCasesForLessOrEqualOpcodeInLastComponent[] = {
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::LessOrEqual, 50,
        false, ("1;1;1"), ("1;1;50;" _MAX_)
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::LessOrEqual, 1,
        false, ("1;1;1"), ("1;1;1;" _MAX_)
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::LessOrEqual, 99,
        false, ("1;1;1"), ("1;1;99;" _MAX_)
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::LessOrEqual, 100,
        false, ("1;1;1"), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::LessOrEqual, 200,
        false, ("1;1;1"), ("1;1;100")
    },
};
INSTANTIATE_TEST_CASE_P(
    LessOrEqualInLastComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForLessOrEqualOpcodeInLastComponent));

// Greater, Last component.
TRefineKeyRangeTestCase refineCasesForGreaterOpcodeInLastComponent[] = {
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Greater, 50,
        false, ("1;1;50;" _MAX_), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Greater, 0,
        false, ("1;1;1"), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Greater, 1,
        false, ("1;1;1;" _MAX_), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Greater, 100,
        true, (""), ("")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::Greater, 200,
        true, (""), ("")
    },
};
INSTANTIATE_TEST_CASE_P(
    GreaterInLastComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForGreaterOpcodeInLastComponent));

// GreaterOrEqual, Last component.
TRefineKeyRangeTestCase refineCasesForGreaterOrEqualOpcodeInLastComponent[] = {
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::GreaterOrEqual, 50,
        false, ("1;1;50"), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::GreaterOrEqual, 1,
        false, ("1;1;1"), ("1;1;100")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::GreaterOrEqual, 100,
        true, (""), ("")
    },
    {
        ("1;1;1"), ("1;1;100"),
        "m", EBinaryOp::GreaterOrEqual, 200,
        true, (""), ("")
    },
};
INSTANTIATE_TEST_CASE_P(
    GreaterOrEqualInLastComponent,
    TRefineKeyRangeTest,
    ::testing::ValuesIn(refineCasesForGreaterOrEqualOpcodeInLastComponent));

////////////////////////////////////////////////////////////////////////////////

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
    PrepareExpressionTest,
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
            "is_prefix(\"abc\", s)")
    ));

////////////////////////////////////////////////////////////////////////////////

TEST_F(TRefineKeyRangeTest, ContradictiveConjuncts)
{
    auto expr = PrepareExpression("k >= 90 and k < 10", GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(BuildKey("1;1;1"), BuildKey("100;100;100")),
        expr);

    ExpectIsEmpty(result);
}

TEST_F(TRefineKeyRangeTest, Lookup1)
{
    auto expr = PrepareExpression("k = 50 and l = 50", GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(BuildKey("1;1;1"), BuildKey("100;100;100")),
        expr);

    EXPECT_EQ(BuildKey("50;50"), result.first);
    EXPECT_EQ(BuildKey("50;50;" _MAX_), result.second);
}

TEST_F(TRefineKeyRangeTest, Lookup2)
{
    auto expr = PrepareExpression("k = 50 and l = 50 and m = 50", GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(BuildKey("1;1;1"), BuildKey("100;100;100")),
        expr);

    EXPECT_EQ(BuildKey("50;50;50"), result.first);
    EXPECT_EQ(BuildKey("50;50;50;" _MAX_), result.second);
}

TEST_F(TRefineKeyRangeTest, Range1)
{
    auto expr = PrepareExpression("k > 0 and k < 100", GetSampleTableSchema());

    TKeyColumns keyColumns;
    keyColumns.push_back("k");
    auto result = RefineKeyRange(
        keyColumns,
        std::make_pair(BuildKey(""), BuildKey("1000000000")),
        expr);

    EXPECT_EQ(BuildKey("0;" _MAX_), result.first);
    EXPECT_EQ(BuildKey("100"), result.second);
}

TEST_F(TRefineKeyRangeTest, MultipleConjuncts1)
{
    auto expr = PrepareExpression("k >= 10 and k < 90", GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(BuildKey("1;1;1"), BuildKey("100;100;100")),
        expr);

    EXPECT_EQ(BuildKey("10"), result.first);
    EXPECT_EQ(BuildKey("90"), result.second);
}

TEST_F(TRefineKeyRangeTest, MultipleConjuncts2)
{
    auto expr = PrepareExpression(
        "k = 50 and l >= 10 and l < 90 and m = 50",
        GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(BuildKey("1;1;1"), BuildKey("100;100;100")),
        expr);

    EXPECT_EQ(BuildKey("50;10"), result.first);
    EXPECT_EQ(BuildKey("50;90"), result.second);
}

TEST_F(TRefineKeyRangeTest, MultipleConjuncts3)
{
    auto expr = PrepareExpression("k = 50 and m = 50", GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(BuildKey("1;1;1"), BuildKey("100;100;100")),
        expr);

    EXPECT_EQ(BuildKey("50"), result.first);
    EXPECT_EQ(BuildKey("50;" _MAX_), result.second);
}

TEST_F(TRefineKeyRangeTest, MultipleDisjuncts)
{
    auto expr = PrepareExpression(
        "k = 50 and m = 50 or k = 75 and m = 50",
        GetSampleTableSchema());

    TRowBuffer rowBuffer;

    auto keyColumns = GetSampleKeyColumns();

    auto keyTrie = ExtractMultipleConstraints(
        expr,
        keyColumns,
        &rowBuffer);

    std::vector<TKeyRange> result = GetRangesFromTrieWithinRange(
        std::make_pair(BuildKey("1;1;1"), BuildKey("100;100;100")),
        keyTrie);

    EXPECT_EQ(result.size(), 2);

    EXPECT_EQ(BuildKey("50"), result[0].first);
    EXPECT_EQ(BuildKey("50;" _MAX_), result[0].second);

    EXPECT_EQ(BuildKey("75"), result[1].first);
    EXPECT_EQ(BuildKey("75;" _MAX_), result[1].second);
}

TEST_F(TRefineKeyRangeTest, NotEqualToMultipleRanges)
{
    auto expr = PrepareExpression(
        "(k = 50 and l != 50) and (l > 40 and l < 60)",
        GetSampleTableSchema());

    TRowBuffer rowBuffer;

    auto keyColumns = GetSampleKeyColumns();

    auto keyTrie = ExtractMultipleConstraints(
        expr,
        keyColumns,
        &rowBuffer);

    std::vector<TKeyRange> result = GetRangesFromTrieWithinRange(
        std::make_pair(BuildKey("1;1;1"), BuildKey("100;100;100")),
        keyTrie);

    EXPECT_EQ(result.size(), 2);

    EXPECT_EQ(BuildKey("50;40;" _MAX_), result[0].first);
    EXPECT_EQ(BuildKey("50;50"), result[0].second);

    EXPECT_EQ(BuildKey("50;50;" _MAX_), result[1].first);
    EXPECT_EQ(BuildKey("50;60"), result[1].second);
}

TEST_F(TRefineKeyRangeTest, RangesProduct)
{
    auto expr = PrepareExpression(
        "(k = 40 or k = 50 or k = 60) and (l = 40 or l = 50 or l = 60)",
        GetSampleTableSchema());

    TRowBuffer rowBuffer;

    auto keyColumns = GetSampleKeyColumns();

    auto keyTrie = ExtractMultipleConstraints(
        expr,
        keyColumns,
        &rowBuffer);

    std::vector<TKeyRange> result = GetRangesFromTrieWithinRange(
        std::make_pair(BuildKey("1;1;1"), BuildKey("100;100;100")),
        keyTrie);

    EXPECT_EQ(result.size(), 9);

    EXPECT_EQ(BuildKey("40;40"), result[0].first);
    EXPECT_EQ(BuildKey("40;40;" _MAX_), result[0].second);

    EXPECT_EQ(BuildKey("40;50"), result[1].first);
    EXPECT_EQ(BuildKey("40;50;" _MAX_), result[1].second);

    EXPECT_EQ(BuildKey("40;60"), result[2].first);
    EXPECT_EQ(BuildKey("40;60;" _MAX_), result[2].second);

    EXPECT_EQ(BuildKey("50;40"), result[3].first);
    EXPECT_EQ(BuildKey("50;40;" _MAX_), result[3].second);

    EXPECT_EQ(BuildKey("50;50"), result[4].first);
    EXPECT_EQ(BuildKey("50;50;" _MAX_), result[4].second);

    EXPECT_EQ(BuildKey("50;60"), result[5].first);
    EXPECT_EQ(BuildKey("50;60;" _MAX_), result[5].second);

    EXPECT_EQ(BuildKey("60;40"), result[6].first);
    EXPECT_EQ(BuildKey("60;40;" _MAX_), result[6].second);

    EXPECT_EQ(BuildKey("60;50"), result[7].first);
    EXPECT_EQ(BuildKey("60;50;" _MAX_), result[7].second);

    EXPECT_EQ(BuildKey("60;60"), result[8].first);
    EXPECT_EQ(BuildKey("60;60;" _MAX_), result[8].second);
}

TEST_F(TRefineKeyRangeTest, NormalizeShortKeys)
{
    auto expr = PrepareExpression(
        "k = 1 and l = 2 and m = 3",
        GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(BuildKey("1"), BuildKey("2")),
        expr);

    EXPECT_EQ(BuildKey("1;2;3"), result.first);
    EXPECT_EQ(BuildKey("1;2;3;" _MAX_), result.second);
}

TEST_F(TRefineKeyRangeTest, PrefixQuery)
{
    auto expr = PrepareExpression(
        "k = 50 and l = 50 and m = 50 and is_prefix(\"abc\", s)",
        GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns2(),
        std::make_pair(BuildKey("1;1;1;aaa"), BuildKey("100;100;100;bbb")),
        expr);

    EXPECT_EQ(BuildKey("50;50;50;abc"), result.first);
    EXPECT_EQ(BuildKey("50;50;50;abd"), result.second);
}

TEST_F(TRefineKeyRangeTest, EmptyRange)
{
    auto expr = PrepareExpression(
        "k between 1 and 1",
        GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(BuildKey("0;0;0"), BuildKey("2;2;2")),
        expr);

    EXPECT_EQ(BuildKey("1"), result.first);
    EXPECT_EQ(BuildKey("1;" _MAX_), result.second);
}

TEST_F(TRefineKeyRangeTest, RangeToPointCollapsing)
{
    auto expr = PrepareExpression(
        "k >= 1 and k <= 1 and l = 1",
        GetSampleTableSchema());

    auto result = RefineKeyRange(
        GetSampleKeyColumns(),
        std::make_pair(BuildKey("0;0;0"), BuildKey("2;2;2")),
        expr);

    EXPECT_EQ(BuildKey("1;1"), result.first);
    EXPECT_EQ(BuildKey("1;1;" _MAX_), result.second);
}

////////////////////////////////////////////////////////////////////////////////

#ifdef YT_USE_LLVM

class TReaderMock
    : public ISchemafulReader
{
public:
    MOCK_METHOD1(Open, TFuture<void>(const TTableSchema&));
    MOCK_METHOD1(Read, bool(std::vector<TUnversionedRow>*));
    MOCK_METHOD0(GetReadyEvent, TFuture<void>());
};

class TWriterMock
    : public ISchemafulWriter
{
public:
    MOCK_METHOD2(Open, TFuture<void>(const TTableSchema&, const TNullable<TKeyColumns>&));
    MOCK_METHOD0(Close, TFuture<void>());
    MOCK_METHOD1(Write, bool(const std::vector<TUnversionedRow>&));
    MOCK_METHOD0(GetReadyEvent, TFuture<void>());
};

TOwningRow BuildRow(
    const Stroka& yson,
    TDataSplit& dataSplit,
    bool treatMissingAsNull = true)
{
    auto keyColumns = GetKeyColumnsFromDataSplit(dataSplit);
    auto tableSchema = GetTableSchemaFromDataSplit(dataSplit);

    return NVersionedTableClient::BuildRow(
            yson, keyColumns, tableSchema, treatMissingAsNull);
}

class TQueryEvaluateTest
    : public ::testing::Test
{
protected:
    virtual void SetUp() override
    {
        ReaderMock_ = New<StrictMock<TReaderMock>>();
        WriterMock_ = New<StrictMock<TWriterMock>>();

        ActionQueue_ = New<TActionQueue>("Test");
    }

    virtual void TearDown() override
    {
        ActionQueue_->Shutdown();
    }

    void Evaluate(
        const Stroka& query,
        const TDataSplit& dataSplit,
        const std::vector<TOwningRow>& owningSource,
        const std::vector<TOwningRow>& owningResult,
        i64 inputRowLimit = std::numeric_limits<i64>::max(),
        i64 outputRowLimit = std::numeric_limits<i64>::max())
    {
        auto result = BIND(&TQueryEvaluateTest::DoEvaluate, this)
            .AsyncVia(ActionQueue_->GetInvoker())
            .Run(
                query,
                dataSplit,
                owningSource,
                owningResult,
                inputRowLimit,
                outputRowLimit)
            .Get();
        THROW_ERROR_EXCEPTION_IF_FAILED(result);
    }

    void DoEvaluate(
        const Stroka& query,
        const TDataSplit& dataSplit,
        const std::vector<TOwningRow>& owningSource,
        const std::vector<TOwningRow>& owningResult,
        i64 inputRowLimit,
        i64 outputRowLimit)
    {
        std::vector<TRow> source(owningSource.size());
        std::vector<std::vector<TRow>> results;
        typedef const TRow(TOwningRow::*TGetFunction)() const;

        std::transform(
            owningSource.begin(),
            owningSource.end(),
            source.begin(),
            std::mem_fn(TGetFunction(&TOwningRow::Get)));

        for (auto iter = owningResult.begin(), end = owningResult.end(); iter != end;) {
            size_t writeSize = std::min(static_cast<int>(end - iter), NQueryClient::MaxRowsPerWrite);
            std::vector<TRow> result(writeSize);

            std::transform(
                iter,
                iter + writeSize,
                result.begin(),
                std::mem_fn(TGetFunction(&TOwningRow::Get)));

            results.push_back(result);

            iter += writeSize;
        }

        EXPECT_CALL(PrepareMock_, GetInitialSplit("//t", _))
            .WillOnce(Return(WrapInFuture(dataSplit)));

        EXPECT_CALL(*ReaderMock_, Open(_))
            .WillOnce(Return(WrapVoidInFuture()));

        EXPECT_CALL(*ReaderMock_, Read(_))
            .WillOnce(DoAll(SetArgPointee<0>(source), Return(false)));

        {
            testing::InSequence s;

            EXPECT_CALL(*WriterMock_, Open(_, _))
                .WillOnce(Return(WrapVoidInFuture()));
            for (auto& result : results) {
                EXPECT_CALL(*WriterMock_, Write(result))
                    .WillOnce(Return(true));
            }
            
            EXPECT_CALL(*WriterMock_, Close())
                .WillOnce(Return(WrapVoidInFuture()));
        }

        auto evaluator = New<TEvaluator>(New<TExecutorConfig>());
        evaluator->Run(
            PreparePlanFragment(&PrepareMock_, query, inputRowLimit, outputRowLimit)->Query,
            ReaderMock_,
            WriterMock_);
    }

    StrictMock<TPrepareCallbacksMock> PrepareMock_;
    TIntrusivePtr<StrictMock<TReaderMock>> ReaderMock_; 
    TIntrusivePtr<StrictMock<TWriterMock>> WriterMock_;
    TActionQueuePtr ActionQueue_;

};

TEST_F(TQueryEvaluateTest, Simple)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("a=4;b=5", simpleSplit, false));
    source.push_back(BuildRow("a=10;b=11", simpleSplit, false));

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("a=4;b=5", simpleSplit, false));
    result.push_back(BuildRow("a=10;b=11", simpleSplit, false));

    Evaluate("a, b FROM [//t]", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, SimpleCmpInt)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("a=4;b=5", simpleSplit, false));
    source.push_back(BuildRow("a=6;b=6", simpleSplit, false));

    std::vector<TColumnSchema> resultColumns;
    resultColumns.emplace_back("r1", EValueType::Boolean);
    resultColumns.emplace_back("r2", EValueType::Boolean);
    resultColumns.emplace_back("r3", EValueType::Boolean);
    resultColumns.emplace_back("r4", EValueType::Boolean);
    resultColumns.emplace_back("r5", EValueType::Boolean);
    auto resultSplit = MakeSplit(resultColumns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("r1=%true;r2=%false;r3=%true;r4=%false;r5=%false", resultSplit, false));
    result.push_back(BuildRow("r1=%false;r2=%false;r3=%true;r4=%true;r5=%true", resultSplit, false));

    Evaluate("a < b as r1, a > b as r2, a <= b as r3, a >= b as r4, a = b as r5 FROM [//t]", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, SimpleCmpString)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::String);
    columns.emplace_back("b", EValueType::String);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("a=\"a\";b=\"aa\"", simpleSplit, false));
    source.push_back(BuildRow("a=\"aa\";b=\"aa\"", simpleSplit, false));

    std::vector<TColumnSchema> resultColumns;
    resultColumns.emplace_back("r1", EValueType::Boolean);
    resultColumns.emplace_back("r2", EValueType::Boolean);
    resultColumns.emplace_back("r3", EValueType::Boolean);
    resultColumns.emplace_back("r4", EValueType::Boolean);
    resultColumns.emplace_back("r5", EValueType::Boolean);
    auto resultSplit = MakeSplit(resultColumns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("r1=%true;r2=%false;r3=%true;r4=%false;r5=%false", resultSplit, false));
    result.push_back(BuildRow("r1=%false;r2=%false;r3=%true;r4=%true;r5=%true", resultSplit, false));

    Evaluate("a < b as r1, a > b as r2, a <= b as r3, a >= b as r4, a = b as r5 FROM [//t]", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, SimpleBetweenAnd)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("a=4;b=5", simpleSplit, false));
    source.push_back(BuildRow("a=10;b=11", simpleSplit, false));
    source.push_back(BuildRow("a=15;b=11", simpleSplit, false));

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("a=10;b=11", simpleSplit, false));

    Evaluate("a, b FROM [//t] where a between 9 and 11", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, SimpleIn)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("a=4;b=5", simpleSplit, false));
    source.push_back(BuildRow("a=10;b=11", simpleSplit, false));
    source.push_back(BuildRow("a=15;b=11", simpleSplit, false));

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("a=4;b=5", simpleSplit, false));
    result.push_back(BuildRow("a=10;b=11", simpleSplit, false));

    Evaluate("a, b FROM [//t] where a in (4, 10)", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, SimpleWithNull)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    columns.emplace_back("c", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("a=4;b=5", simpleSplit, true));
    source.push_back(BuildRow("a=10;b=11;c=9", simpleSplit, true));
    source.push_back(BuildRow("a=16", simpleSplit, true));

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("a=4;b=5", simpleSplit, true));
    result.push_back(BuildRow("a=10;b=11;c=9", simpleSplit, true));
    result.push_back(BuildRow("a=16", simpleSplit, true));

    Evaluate("a, b, c FROM [//t] where a > 3", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, SimpleWithNull2)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    columns.emplace_back("c", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("a=1;b=2;c=3", simpleSplit, true));
    source.push_back(BuildRow("a=4", simpleSplit, true));
    source.push_back(BuildRow("a=5;b=5", simpleSplit, true));
    source.push_back(BuildRow("a=7;c=8", simpleSplit, true));
    source.push_back(BuildRow("a=10;b=1", simpleSplit, true));
    source.push_back(BuildRow("a=10;c=1", simpleSplit, true));

    std::vector<TColumnSchema> resultColumns;
    resultColumns.emplace_back("a", EValueType::Int64);
    resultColumns.emplace_back("x", EValueType::Int64);
    auto resultSplit = MakeSplit(resultColumns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("a=1;x=5", resultSplit, true));
    result.push_back(BuildRow("a=4;", resultSplit, true));
    result.push_back(BuildRow("a=5;", resultSplit, true));
    result.push_back(BuildRow("a=7;", resultSplit, true));

    Evaluate("a, b + c as x FROM [//t] where a < 10", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, SimpleStrings)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("s", EValueType::String);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("s=foo", simpleSplit, true));
    source.push_back(BuildRow("s=bar", simpleSplit, true));
    source.push_back(BuildRow("s=baz", simpleSplit, true));

    auto resultSplit = MakeSplit(columns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("s=foo", resultSplit, true));
    result.push_back(BuildRow("s=bar", resultSplit, true));
    result.push_back(BuildRow("s=baz", resultSplit, true));

    Evaluate("s FROM [//t]", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, SimpleStrings2)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("s", EValueType::String);
    columns.emplace_back("u", EValueType::String);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("s=foo; u=x", simpleSplit, true));
    source.push_back(BuildRow("s=bar; u=y", simpleSplit, true));
    source.push_back(BuildRow("s=baz; u=x", simpleSplit, true));
    source.push_back(BuildRow("s=olala; u=z", simpleSplit, true));

    auto resultSplit = MakeSplit(columns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("s=foo; u=x", resultSplit, true));
    result.push_back(BuildRow("s=baz; u=x", resultSplit, true));

    Evaluate("s, u FROM [//t] where u = \"x\"", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, HasPrefixStrings)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("s", EValueType::String);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    source.push_back(BuildRow("s=foobar", simpleSplit, true));
    source.push_back(BuildRow("s=bar", simpleSplit, true));
    source.push_back(BuildRow("s=baz", simpleSplit, true));

    auto resultSplit = MakeSplit(columns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("s=foobar", resultSplit, true));

    Evaluate("s FROM [//t] where is_prefix(\"foo\", s)", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, Complex)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=1;b=10",
        "a=2;b=20",
        "a=3;b=30",
        "a=4;b=40",
        "a=5;b=50",
        "a=6;b=60",
        "a=7;b=70",
        "a=8;b=80",
        "a=9;b=90"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, false));
    }

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("x=0;t=200", simpleSplit, false));
    result.push_back(BuildRow("x=1;t=241", simpleSplit, false));

    Evaluate("x, sum(b) + x as t FROM [//t] where a > 1 group by a % 2 as x", simpleSplit, source, result);

    SUCCEED();
}

TEST_F(TQueryEvaluateTest, Complex2)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=1;b=10",
        "a=2;b=20",
        "a=3;b=30",
        "a=4;b=40",
        "a=5;b=50",
        "a=6;b=60",
        "a=7;b=70",
        "a=8;b=80",
        "a=9;b=90"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, false));
    }

    std::vector<TColumnSchema> resultColumns;
    resultColumns.emplace_back("x", EValueType::Int64);
    resultColumns.emplace_back("q", EValueType::Int64);
    resultColumns.emplace_back("t", EValueType::Int64);
    auto resultSplit = MakeSplit(resultColumns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("x=0;q=0;t=200", resultSplit, false));
    result.push_back(BuildRow("x=1;q=0;t=241", resultSplit, false));

    Evaluate("x, q, sum(b) + x as t FROM [//t] where a > 1 group by a % 2 as x, 0 as q", simpleSplit, source, result);

    SUCCEED();
}

TEST_F(TQueryEvaluateTest, ComplexBigResult)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    std::vector<TOwningRow> source;
    for (size_t i = 0; i < 10000; ++i) {
        source.push_back(BuildRow(Stroka() + "a=" + ToString(i) + ";b=" + ToString(i * 10), simpleSplit, false));
    }

    std::vector<TOwningRow> result;

    for (size_t i = 2; i < 10000; ++i) {
        result.push_back(BuildRow(Stroka() + "x=" + ToString(i) + ";t=" + ToString(i * 10 + i), simpleSplit, false));
    }

    Evaluate("x, sum(b) + x as t FROM [//t] where a > 1 group by a as x", simpleSplit, source, result);
}

TEST_F(TQueryEvaluateTest, ComplexWithNull)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=1;b=10",
        "a=2;b=20",
        "a=3;b=30",
        "a=4;b=40",
        "a=5;b=50",
        "a=6;b=60",
        "a=7;b=70",
        "a=8;b=80",
        "a=9;b=90",
        "a=10",
        "b=1",
        "b=2",
        "b=3"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, true));
    }

    std::vector<TColumnSchema> resultColumns;
    resultColumns.emplace_back("x", EValueType::Int64);
    resultColumns.emplace_back("t", EValueType::Int64);
    resultColumns.emplace_back("y", EValueType::Int64);
    auto resultSplit = MakeSplit(resultColumns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("x=1;t=251;y=250", resultSplit, true));
    result.push_back(BuildRow("x=0;t=200;y=200", resultSplit, true));
    result.push_back(BuildRow("y=6", resultSplit, true));

    Evaluate("x, sum(b) + x as t, sum(b) as y FROM [//t] group by a % 2 as x", simpleSplit, source, result);

    SUCCEED();
}


TEST_F(TQueryEvaluateTest, IsNull)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=1;b=10",
        "a=2;b=20",
        "a=9;b=90",
        "a=10",
        "b=1",
        "b=2",
        "b=3"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, true));
    }

    std::vector<TColumnSchema> resultColumns;
    resultColumns.emplace_back("b", EValueType::Int64);
    auto resultSplit = MakeSplit(resultColumns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("b=1", resultSplit, true));
    result.push_back(BuildRow("b=2", resultSplit, true));
    result.push_back(BuildRow("b=3", resultSplit, true));

    Evaluate("b FROM [//t] where is_null(a)", simpleSplit, source, result);

    SUCCEED();
}


TEST_F(TQueryEvaluateTest, ComplexStrings)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("s", EValueType::String);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=10;s=x",
        "a=20;s=y",
        "a=30;s=x",
        "a=40;s=x",
        "a=42",
        "a=50;s=x",
        "a=60;s=y",
        "a=70;s=z",
        "a=72",
        "a=80;s=y",
        "a=85",
        "a=90;s=z"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, true));
    }

    std::vector<TColumnSchema> resultColumns;
    resultColumns.emplace_back("x", EValueType::String);
    resultColumns.emplace_back("t", EValueType::Int64);
    auto resultSplit = MakeSplit(resultColumns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("x=y;t=160", resultSplit, true));
    result.push_back(BuildRow("x=x;t=120", resultSplit, true));
    result.push_back(BuildRow("t=199", resultSplit, true));
    result.push_back(BuildRow("x=z;t=160", resultSplit, true));

    Evaluate("x, sum(a) as t FROM [//t] where a > 10 group by s as x", simpleSplit, source, result);

    SUCCEED();
}

TEST_F(TQueryEvaluateTest, ComplexStringsLower)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::String);
    columns.emplace_back("s", EValueType::String);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=XyZ;s=one",
        "a=aB1C;s=two",
        "a=cs1dv;s=three",
        "a=HDs;s=four",
        "a=kIu;s=five",
        "a=trg1t;s=six"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, true));
    }

    std::vector<TColumnSchema> resultColumns;
    resultColumns.emplace_back("s", EValueType::String);
    auto resultSplit = MakeSplit(resultColumns);

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("s=one", resultSplit, true));
    result.push_back(BuildRow("s=two", resultSplit, true));
    result.push_back(BuildRow("s=four", resultSplit, true));
    result.push_back(BuildRow("s=five", resultSplit, true));

    Evaluate("s FROM [//t] where lower(a) in (\"xyz\",\"ab1c\",\"hds\",\"kiu\")", simpleSplit, source, result);

    SUCCEED();
}

TEST_F(TQueryEvaluateTest, TestIf)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=1;b=10",
        "a=2;b=20",
        "a=3;b=30",
        "a=4;b=40",
        "a=5;b=50",
        "a=6;b=60",
        "a=7;b=70",
        "a=8;b=80",
        "a=9;b=90"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, false));
    }

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("x=b;t=251.", simpleSplit, false));
    result.push_back(BuildRow("x=a;t=201.", simpleSplit, false));
    
    Evaluate("if(x = 4, \"a\", \"b\") as x, double(sum(b)) + 1.0 as t FROM [//t] group by if(a % 2 = 0, 4, 5) as x", simpleSplit, source, result);

    SUCCEED();
}

TEST_F(TQueryEvaluateTest, TestInputRowLimit)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=1;b=10",
        "a=2;b=20",
        "a=3;b=30",
        "a=4;b=40",
        "a=5;b=50",
        "a=6;b=60",
        "a=7;b=70",
        "a=8;b=80",
        "a=9;b=90"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, false));
    }

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("a=2;b=20", simpleSplit, false));
    result.push_back(BuildRow("a=3;b=30", simpleSplit, false));

    Evaluate("a, b FROM [//t] where uint64(a) > 1u and uint64(a) < 9u", simpleSplit, source, result, 3);

    SUCCEED();
}

TEST_F(TQueryEvaluateTest, TestOutputRowLimit)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=1;b=10",
        "a=2;b=20",
        "a=3;b=30",
        "a=4;b=40",
        "a=5;b=50",
        "a=6;b=60",
        "a=7;b=70",
        "a=8;b=80",
        "a=9;b=90"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, false));
    }

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("a=2;b=20", simpleSplit, false));
    result.push_back(BuildRow("a=3;b=30", simpleSplit, false));
    result.push_back(BuildRow("a=4;b=40", simpleSplit, false));

    Evaluate("a, b FROM [//t] where a > 1 and a < 9", simpleSplit, source, result, std::numeric_limits<i64>::max(), 3);

    SUCCEED();
}

TEST_F(TQueryEvaluateTest, TestGroupRowLimit)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=1;b=10",
        "a=2;b=20",
        "a=3;b=30",
        "a=4;b=40",
        "a=5;b=50",
        "a=6;b=60",
        "a=7;b=70",
        "a=8;b=80",
        "a=9;b=90"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, false));
    }

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("a=1;b=250", simpleSplit, false));
    result.push_back(BuildRow("a=0;b=200", simpleSplit, false));

    Evaluate("a, sum(b) as b FROM [//t] group by a % 2 as a", simpleSplit, source, result, std::numeric_limits<i64>::max(), 3);

    result.clear();
    result.push_back(BuildRow("a=1;b=10", simpleSplit, false));
    result.push_back(BuildRow("a=2;b=20", simpleSplit, false));
    result.push_back(BuildRow("a=3;b=30", simpleSplit, false));

    Evaluate("a, sum(b) as b FROM [//t] group by a % 5 as a", simpleSplit, source, result, std::numeric_limits<i64>::max(), 3);

    SUCCEED();
}

TEST_F(TQueryEvaluateTest, TestTypeInference)
{
    std::vector<TColumnSchema> columns;
    columns.emplace_back("a", EValueType::Int64);
    columns.emplace_back("b", EValueType::Int64);
    auto simpleSplit = MakeSplit(columns);

    const char* sourceRowsData[] = {
        "a=1;b=10",
        "a=2;b=20",
        "a=3;b=30",
        "a=4;b=40",
        "a=5;b=50",
        "a=6;b=60",
        "a=7;b=70",
        "a=8;b=80",
        "a=9;b=90"
    };

    std::vector<TOwningRow> source;
    for (auto row : sourceRowsData) {
        source.push_back(BuildRow(row, simpleSplit, false));
    }

    std::vector<TOwningRow> result;
    result.push_back(BuildRow("x=b;t=251.", simpleSplit, false));
    result.push_back(BuildRow("x=a;t=201.", simpleSplit, false));
    
    Evaluate("if(int64(x) = 4, \"a\", \"b\") as x, double(sum(uint64(b) * 1u)) + 1.0 as t FROM [//t] group by if(a % 2 = 0, double(4u), 5.0) as x", simpleSplit, source, result);

    SUCCEED();
}

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NQueryClient
} // namespace NYT
