#include "key_helpers.h"

#include <yt/client/table_client/key_bound.h>
#include <yt/client/table_client/comparator.h>
#include <yt/client/table_client/helpers.h>

#include <yt/core/test_framework/framework.h>

#include <library/cpp/iterator/zip.h>

namespace NYT::NTableClient {
namespace {

////////////////////////////////////////////////////////////////////////////////

static const auto IntValue1 = MakeUnversionedInt64Value(42);
static const auto IntValue2 = MakeUnversionedInt64Value(-7);
static const auto StrValue1 = MakeUnversionedStringValue("foo");
static const auto StrValue2 = MakeUnversionedStringValue("bar");
static const auto NullValue = MakeUnversionedNullValue();
static const auto MaxValue = MakeUnversionedSentinelValue(EValueType::Max);
static const auto MinValue = MakeUnversionedSentinelValue(EValueType::Min);

static const std::vector<TUnversionedValue> AllValues = {
    IntValue1,
    IntValue2,
    StrValue1,
    StrValue2,
    NullValue,
    MaxValue,
    MinValue,
};

static const std::vector<TUnversionedValue> NoSentinelValues = {
    IntValue1,
    IntValue2,
    StrValue1,
    StrValue2,
    NullValue,
};

class TComparatorTest
    : public ::testing::Test
{
protected:
    void InvokeForAllRows(
        const std::vector<TUnversionedValue>& possibleValues,
        int minLength,
        int maxLength,
        std::function<void(const TUnversionedOwningRow& row)> callback)
    {
        std::vector<TUnversionedValue> stack;
        auto recursiveFill = [&] (int index, auto recursiveFill) {
            if (index >= minLength && index <= maxLength) {
                callback(MakeRow(stack));
            }
            if (index == maxLength) {
                return;
            }
            for (const auto& value : possibleValues) {
                stack.push_back(value);
                recursiveFill(index + 1, recursiveFill);
                stack.pop_back();
            }
        };
        recursiveFill(0, recursiveFill);
    }

    std::vector<TKey> GenerateKeys(
        const std::vector<TUnversionedValue>& possibleValues,
        int keyLength)
    {
        RowStorage_.clear();
        InvokeForAllRows(possibleValues, keyLength, keyLength, [&] (auto row) { RowStorage_.push_back(row); });

        std::vector<TKey> keys;
        keys.reserve(RowStorage_.size());
        for (const auto& row : RowStorage_) {
            keys.push_back(TKey::FromRow(row));
        }
        return keys;
    }

private:
    std::vector<TUnversionedOwningRow> RowStorage_;
};

TEST_F(TComparatorTest, StressNewAndLegacyTestEquivalence)
{
    constexpr int KeyLength = 3;
    const auto Comparator = MakeComparator(KeyLength);
    constexpr int LegacyRowLength = 5;

    // Generate all possible keys of length 3.
    auto allKeys = GenerateKeys(NoSentinelValues, KeyLength);

    auto validateTestPreservation = [&] (const TKeyBound& keyBound, const TUnversionedRow& legacyRow) {
        bool isUpper = keyBound.IsUpper;
        for (const auto& key : allKeys) {
            auto legacyTest = isUpper ? key.AsOwningRow() < legacyRow : key.AsOwningRow() >= legacyRow;
            auto newTest = Comparator.TestKey(key, keyBound);

            if (legacyTest != newTest) {
                Cerr
                    << "Legacy row: " << ToString(legacyRow) << Endl
                    << "Key bound: " << ToString(keyBound) << Endl
                    << "Key: " << ToString(key.AsOwningRow()) << Endl
                    << "LegacyTest: " << legacyTest << Endl
                    << "NewTest: " << newTest << Endl;
                // Somehow ASSERTs do not stop execution in our gtest :(
                THROW_ERROR_EXCEPTION("Failure");
            }
        }
    };

    // Legacy -> New.
    // Check that all possible legacy bounds of length up to 5 produce
    // same test result as corresponding key bounds over all keys of length 3.

    auto validateCurrentLegacyRow = [&] (const auto& legacyRow) {
        for (bool isUpper : {false, true}) {
            auto keyBound = KeyBoundFromLegacyRow(legacyRow, isUpper, KeyLength);
            validateTestPreservation(keyBound, legacyRow);
        }
    };

    // Test all possible legacy bounds of length up to 5.
    InvokeForAllRows(AllValues, 0, LegacyRowLength, validateCurrentLegacyRow);

    // New -> Legacy.
    // Check that all possible key bounds of length up to 3 produce
    // same test result as corresponding legacy bounds over all keys of length 3.

    auto validateCurrentKeyBound = [&] (const auto& row) {
        for (bool isUpper : {false, true}) {
            for (bool isInclusive : {false, true}) {
                auto keyBound = TOwningKeyBound::FromRow(row, isInclusive, isUpper);
                auto legacyRow = KeyBoundToLegacyRow(keyBound);
                validateTestPreservation(keyBound, legacyRow);
            }
        }
    };

    // Test all possible key bounds of length up to 3.
    InvokeForAllRows(NoSentinelValues, 0, KeyLength, validateCurrentKeyBound);
}

TEST_F(TComparatorTest, KeyBoundComparisonWellFormedness)
{
    constexpr int KeyLength = 2;
    const auto Comparator = MakeComparator(KeyLength);

    std::vector<TOwningKeyBound> keyBounds;
    InvokeForAllRows(NoSentinelValues, 0, KeyLength, [&] (const auto& row) {
        for (bool isUpper : {false, true}) {
            for (bool isInclusive : {false, true}) {
                keyBounds.emplace_back(TOwningKeyBound::FromRow(row, isInclusive, isUpper));
            }
        }
    });

    for (int lowerVsUpperResult : {-1, 0, 1}) {
        for (const auto& keyBoundA : keyBounds) {
            // Reflexivity.
            EXPECT_EQ(0, Comparator.CompareKeyBounds(keyBoundA, keyBoundA, lowerVsUpperResult));
            for (const auto& keyBoundB : keyBounds) {
                // Antisymmetry.
                EXPECT_EQ(
                    Comparator.CompareKeyBounds(keyBoundA, keyBoundB, lowerVsUpperResult),
                    -Comparator.CompareKeyBounds(keyBoundB, keyBoundA, lowerVsUpperResult));
                for (const auto& keyBoundC : keyBounds) {
                    // Transitivity.
                    int compAB = Comparator.CompareKeyBounds(keyBoundA, keyBoundB, lowerVsUpperResult);
                    int compBC = Comparator.CompareKeyBounds(keyBoundB, keyBoundC, lowerVsUpperResult);
                    int compAC = Comparator.CompareKeyBounds(keyBoundA, keyBoundC, lowerVsUpperResult);
                    if (compAB == -1 && compBC == -1) {
                        EXPECT_EQ(compAC, -1);
                    } else if (compAB <= 0 && compBC <= 0) {
                        EXPECT_LE(compAC, 0);
                    }
                }
            }
        }
    }
}

TEST_F(TComparatorTest, KeyBoundLowerVsUpperResult)
{
    constexpr int KeyLength = 3;
    const auto Comparator = MakeComparator(KeyLength);

    std::vector<TOwningKeyBound> keyBounds;
    InvokeForAllRows(NoSentinelValues, 0, KeyLength, [&] (const auto& row) {
        for (bool isInclusive : {false, true}) {
            auto upperBound = TOwningKeyBound::FromRow(row, isInclusive, /* isUpper */ true);
            auto lowerBound = upperBound.Invert();
            for (int lowerVsUpperResult : {-1, 0, 1}) {
                EXPECT_EQ(lowerVsUpperResult, Comparator.CompareKeyBounds(lowerBound, upperBound, lowerVsUpperResult));
            }
        }
    });
}

TEST_F(TComparatorTest, KeyBoundMonotonicity)
{
    constexpr int KeyLength = 3;
    const auto Comparator = MakeComparator(KeyLength);

    // Generate all possible keys of length 3.
    auto allKeys = GenerateKeys(NoSentinelValues, KeyLength);

    // And all possible upper key bounds of length up to 3.
    std::vector<TOwningKeyBound> keyBounds;
    InvokeForAllRows(NoSentinelValues, 0, KeyLength, [&] (const auto& row) {
        for (bool isInclusive : {false, true}) {
            auto upperBound = TOwningKeyBound::FromRow(row, isInclusive, /* isUpper */ true);
            keyBounds.emplace_back(std::move(upperBound));
        }
    });

    std::sort(keyBounds.begin(), keyBounds.end(), [&] (const auto& lhs, const auto& rhs) {
        return Comparator.CompareKeyBounds(lhs, rhs) < 0;
    });

    // Check that for any key K, predicate "key bound KB admits K" is monotonic
    // while iterating with KB over #keyBounds (i.e. is false up to some moment, and
    // is true after that).

    for (const auto& key : allKeys) {
        bool previousTestResult = false;
        for (const auto& keyBound : keyBounds) {
            auto testResult = Comparator.TestKey(key, keyBound);
            EXPECT_LE(previousTestResult, testResult);
            previousTestResult = testResult;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTableClient
