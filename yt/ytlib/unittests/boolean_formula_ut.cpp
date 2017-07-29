#include <yt/core/test_framework/framework.h>

#include <yt/core/misc/boolean_formula.h>
#include <yt/core/misc/error.h>

namespace NYT {
namespace {

////////////////////////////////////////////////////////////////////////////////

class TBooleanFormulaTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<std::tuple<
        const char*,
        std::vector<TString>,
        bool>>
{ };

TEST_P(TBooleanFormulaTest, Test)
{
    const auto& args = GetParam();
    const auto& formula = std::get<0>(args);
    const auto& values = std::get<1>(args);
    bool expected = std::get<2>(args);
   
    auto filter = MakeBooleanFormula(formula);
    
    EXPECT_EQ(expected, filter.IsSatisfiedBy(values))
        << "formula: " << formula << std::endl
        << "true variables: " << ::testing::PrintToString(values) << std::endl
        << "expected: " << expected;
}

INSTANTIATE_TEST_CASE_P(
    TBooleanFormulaTest,
    TBooleanFormulaTest,
    ::testing::Values(
        std::make_tuple("", std::vector<TString>{}, true),
        std::make_tuple("", std::vector<TString>{"b"}, true),
        std::make_tuple("a", std::vector<TString>{"b"}, false),
        std::make_tuple("!a", std::vector<TString>{"b"}, true),
        std::make_tuple("b", std::vector<TString>{"b"}, true),
        std::make_tuple("a|b", std::vector<TString>{"b"}, true),
        std::make_tuple("a & b", std::vector<TString>{"b"}, false),
        std::make_tuple("(b)", std::vector<TString>{"b"}, true),
        std::make_tuple("a|(a|b)", std::vector<TString>{"b"}, true),
        std::make_tuple("(a|b)&(!a&b)", std::vector<TString>{"b"}, true),
        std::make_tuple("a&b", std::vector<TString>{"a", "b"}, true),
        std::make_tuple("(a|c)&(b|c)", std::vector<TString>{"a", "b"}, true),
        std::make_tuple("(a|b)&c", std::vector<TString>{"a", "b"}, false),
        std::make_tuple("a|b|c", std::vector<TString>{"b"}, true),
        std::make_tuple("!a & b & !c", std::vector<TString>{"b"}, true)
));

////////////////////////////////////////////////////////////////////////////////

class TBooleanFormulaParseErrorTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<const char*>
{ };

TEST_P(TBooleanFormulaParseErrorTest, Test)
{
    const auto& formula = GetParam();

    EXPECT_THROW(MakeBooleanFormula(formula), std::exception)
        << "formula: " << formula;
}

INSTANTIATE_TEST_CASE_P(
    TBooleanFormulaParseErrorTest,
    TBooleanFormulaParseErrorTest,
    ::testing::Values(
        "!",
        "&",
        "|",
        "(",
        ")",
        "()",
        "()|a",
        "a&()",
        "a&(",
        "a|)",
        "&a",
        "a&",
        "a!",
        "a!b",
        "a|c!",
        "a!|c",
        "a|(c!)",
        "a|(c&)",
        "a|(|c)",
        "a|b&c",
        "a&b|c"
));

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT
