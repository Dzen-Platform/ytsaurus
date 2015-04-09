#include "stdafx.h"
#include "key_trie.h"
#include "plan_helpers.h"

namespace NYT {
namespace NQueryClient {

using namespace NVersionedTableClient;

////////////////////////////////////////////////////////////////////////////////

TKeyTriePtr ReduceKeyTrie(TKeyTriePtr keyTrie)
{
    // TODO(lukyan): If keyTrie is too big, reduce its size
    return keyTrie;
}

struct TKeyTrieComparer
{
    bool operator () (const std::pair<TValue, TKeyTriePtr>& element, TValue pivot) const {
        return element.first < pivot;
    }
    bool operator () (TValue pivot, const std::pair<TValue, TKeyTriePtr>& element) const {
        return pivot < element.first;
    }
    bool operator () (const std::pair<TValue, TKeyTriePtr>& lhs, const std::pair<TValue, TKeyTriePtr>& rhs) const {
        return lhs.first < rhs.first;
    }
};

int CompareBound(const TBound& lhs, const TBound& rhs, bool lhsDir, bool rhsDir)
{
    auto rank = [] (bool direction, bool included) {
        // <  - (false, fasle)
        // >  - (true, false)
        // <= - (false, true)
        // >= - (true, true)

        // (< x) < (>= x) < (<= x) < (> x)
        return (included? -1 : 2) * (direction? 1 : -1);
    };

    int result = CompareRowValues(lhs.Value, rhs.Value);
    return result == 0
        ? rank(lhsDir, lhs.Included) - rank(rhsDir, rhs.Included)
        : result;
};

template <class TEachCallback>
void MergeBounds(
    const std::vector<TBound>& lhs,
    const std::vector<TBound>& rhs,
    TEachCallback eachCallback)
{
    auto first = lhs.begin();
    auto second = rhs.begin();

    bool firstIsOpen = true;
    bool secondIsOpen = true;

    while (first != lhs.end() && second != rhs.end()) {
        if (CompareBound(*first, *second, firstIsOpen, secondIsOpen) < 0) {
            eachCallback(*first, firstIsOpen);
            ++first;
            firstIsOpen = !firstIsOpen;
        } else {
            eachCallback(*second, secondIsOpen);
            ++second;
            secondIsOpen = !secondIsOpen;
        }
    }

    while (first != lhs.end()) {
        eachCallback(*first, firstIsOpen);
        ++first;
        firstIsOpen = !firstIsOpen;
    }

    while (second != rhs.end()) {
        eachCallback(*second, secondIsOpen);
        ++second;
        secondIsOpen = !secondIsOpen;
    }
}

std::vector<TBound> UniteBounds(
    const std::vector<TBound>& lhs,
    const std::vector<TBound>& rhs)
{
    int cover = 0;
    std::vector<TBound> result;
    bool resultIsOpen = false;

    MergeBounds(lhs, rhs, [&] (TBound bound, bool isOpen) {
        if (isOpen? ++cover == 1 : --cover == 0) {
            if (result.empty() || !(result.back() == bound && isOpen == resultIsOpen)) {
                result.push_back(bound);
                resultIsOpen = !resultIsOpen;
            }
        }
    });

    return result;
}

std::vector<TBound> IntersectBounds(
    const std::vector<TBound>& lhs,
    const std::vector<TBound>& rhs)
{
    int cover = 0;
    std::vector<TBound> result;
    bool resultIsOpen = false;

    MergeBounds(
        lhs,
        rhs,
        [&] (TBound bound, bool isOpen) {
            if (isOpen ? ++cover == 2 : --cover == 1) {
                if (result.empty() || !(result.back() == bound && isOpen == resultIsOpen)) {
                    result.push_back(bound);
                    resultIsOpen = !resultIsOpen;
                }
            }
        });

    return result;
}

TKeyTriePtr TKeyTrie::Unite(TKeyTriePtr rhs)
{
    if (Offset < rhs->Offset) {
        return rhs;
    } else if (Offset > rhs->Offset) {
        return this;
    }

    std::vector<std::pair<TValue, TKeyTriePtr>> other;
    for (auto& next : rhs->Next) {
        auto eq = std::equal_range(Next.begin(), Next.end(), next.first, TKeyTrieComparer());
        if (eq.first != eq.second) {
            eq.first->second->Unite(next.second);
        } else {
            other.push_back(std::move(next));
        }
    }
    
    auto middle = Next.size();
    std::move(other.begin(), other.end(), std::back_inserter(Next));
    std::inplace_merge(Next.begin(), Next.begin() + middle, Next.end(), TKeyTrieComparer());

    if (!Bounds.empty() || !rhs->Bounds.empty()) {
        std::vector<TBound> deletedPoints;

        deletedPoints.emplace_back(MakeUnversionedSentinelValue(EValueType::Min), true);
        for (const auto& next : Next) {
            deletedPoints.emplace_back(next.first, false);
            deletedPoints.emplace_back(next.first, false);
        }
        deletedPoints.emplace_back(MakeUnversionedSentinelValue(EValueType::Max), true);

        std::vector<TBound> bounds = UniteBounds(Bounds, rhs->Bounds);

        Bounds = IntersectBounds(bounds, deletedPoints);
    }

    return this;
}

TKeyTriePtr UniteKeyTrie(const std::vector<TKeyTriePtr>& tries)
{
    if (tries.empty()) {
        return TKeyTrie::Empty();
    } else if (tries.size() == 1) {
        return tries.front();
    }

    std::vector<TKeyTriePtr> maxTries;
    size_t offset = 0;
    for (const auto& trie : tries) {
        if (trie->Offset > offset) {
            maxTries.clear();
            offset = trie->Offset;
        }

        if (trie->Offset == offset) {
            maxTries.push_back(trie);
        }
    }

    std::vector<std::pair<TValue, TKeyTriePtr>> groups;
    for (const auto& trie : maxTries) {
        for (auto& next : trie->Next) {
            groups.push_back(std::move(next));
        }
    }

    std::sort(groups.begin(), groups.end(), TKeyTrieComparer());

    TKeyTriePtr result = TKeyTrie::Empty();

    auto it = groups.begin();
    auto end = groups.end();
    while (it != end) {
        std::vector<TKeyTriePtr> unique;
        auto same = it;
        for (; same != end && *same == *it; ++same) {
            unique.push_back(same->second);
        }
        result->Next.emplace_back(it->first, UniteKeyTrie(unique));
        it = same;
    }

    std::vector<std::vector<TBound>> bounds;
    for (const auto& trie : maxTries) {
        if (!trie->Bounds.empty()) {
            bounds.push_back(std::move(trie->Bounds));
        }
    }

    size_t size = bounds.size();
    while (size > 1) {
        size_t i = 0;
        while (2 * i + 1 < bounds.size()) {
            bounds[i] = UniteBounds(bounds[2 * i], bounds[2 * i + 1]);
            ++i;
        }
        size = i;
    }

    YCHECK(bounds.size () <= 1);
    if (!bounds.empty()) {
        std::vector<TBound> deletedPoints;

        deletedPoints.emplace_back(MakeUnversionedSentinelValue(EValueType::Min), true);
        for (const auto& next : result->Next) {
            deletedPoints.emplace_back(next.first, false);
            deletedPoints.emplace_back(next.first, false);
        }
        deletedPoints.emplace_back(MakeUnversionedSentinelValue(EValueType::Max), true);

        result->Bounds = IntersectBounds(bounds.front(), deletedPoints);
    }

    return result;
}

TKeyTriePtr TKeyTrie::FromLowerBound(const TKey& bound)
{
    auto result = TKeyTrie::Universal();

    for (int offset = 0; offset < bound.GetCount(); ++offset) {
        if (bound[offset].Type != EValueType::Min && bound[offset].Type != EValueType::Max) {
            auto node = New<TKeyTrie>(offset);

            if (offset + 1 < bound.GetCount()) {
                if (bound[offset + 1].Type == EValueType::Min) {
                    node->Bounds.emplace_back(bound[offset], true);
                    node->Bounds.emplace_back(MakeUnversionedSentinelValue(EValueType::Max), true);
                } else if (bound[offset + 1].Type == EValueType::Max) {
                    node->Bounds.emplace_back(bound[offset], false);
                    node->Bounds.emplace_back(MakeUnversionedSentinelValue(EValueType::Max), true);
                } else {
                    node->Next.emplace_back(bound[offset], TKeyTrie::Universal());
                }
            } else {
                node->Bounds.emplace_back(bound[offset], true);
                node->Bounds.emplace_back(MakeUnversionedSentinelValue(EValueType::Max), true);
            }

            result = IntersectKeyTrie(result, node);
        }
    }

    return result;
}

TKeyTriePtr TKeyTrie::FromUpperBound(const TKey& bound)
{
    auto result = TKeyTrie::Universal();

    for (int offset = 0; offset < bound.GetCount(); ++offset) {
        if (bound[offset].Type != EValueType::Min && bound[offset].Type != EValueType::Max) {
            auto node = New<TKeyTrie>(offset);

            if (offset + 1 < bound.GetCount()) {
                if (bound[offset + 1].Type == EValueType::Min) {
                    node->Bounds.emplace_back(MakeUnversionedSentinelValue(EValueType::Min), true);
                    node->Bounds.emplace_back(bound[offset], false);
                } else if (bound[offset + 1].Type == EValueType::Max) {
                    node->Bounds.emplace_back(MakeUnversionedSentinelValue(EValueType::Min), true);
                    node->Bounds.emplace_back(bound[offset], true);
                } else {
                    node->Next.emplace_back(bound[offset], TKeyTrie::Universal());
                }
            } else {
                node->Bounds.emplace_back(MakeUnversionedSentinelValue(EValueType::Min), true);
                node->Bounds.emplace_back(bound[offset], false);
            }

            result = IntersectKeyTrie(result, node);
        }
    }

    return result;
}

TKeyTriePtr TKeyTrie::FromRange(const TKeyRange& range)
{
    return IntersectKeyTrie(FromLowerBound(range.first), FromUpperBound(range.second));
}

TKeyTriePtr UniteKeyTrie(TKeyTriePtr lhs, TKeyTriePtr rhs)
{
    return lhs->Unite(rhs);
};

TKeyTriePtr IntersectKeyTrie(TKeyTriePtr lhs, TKeyTriePtr rhs)
{
    if (lhs->Offset < rhs->Offset) {
        auto result = lhs;
        for (auto& next : result->Next) {
            next.second = IntersectKeyTrie(next.second, rhs);
        }
        return result;
    }

    if (lhs->Offset > rhs->Offset) {
        auto result = rhs;
        for (auto& next : result->Next) {
            next.second = IntersectKeyTrie(next.second, lhs);
        }
        return result;
    }

    auto result = New<TKeyTrie>(lhs->Offset);
    result->Bounds = IntersectBounds(lhs->Bounds, rhs->Bounds);

    // Iterate through resulting bounds and convert singleton ranges into
    // new edges in the trie. This enables futher range limiting.
    auto it = result->Bounds.begin();
    auto jt = result->Bounds.begin();
    auto kt = result->Bounds.end();
    while (it < kt) {
        const auto& lhs = *it++;
        const auto& rhs = *it++;
        if (lhs == rhs) {
            result->Next.emplace_back(lhs.Value, TKeyTrie::Universal());
        } else {
            if (std::distance(jt, it) > 2) {
                *jt++ = lhs;
                *jt++ = rhs;
            } else {
                ++jt; ++jt;
            }
        }
    }

    result->Bounds.erase(jt, kt);

    auto covers = [] (const std::vector<TBound>& bounds, const TValue& point) {
        auto found = std::lower_bound(
            bounds.begin(),
            bounds.end(),
            point,
            [] (const TBound& bound, const TValue& point) {
                return bound.Value < point;
            });

        bool isClose = (found - bounds.begin()) & 1;
        if (found != bounds.end()) {
            return (found->Value != point) == isClose;
        } else {
            return false;
        }
    };

    for (const auto& next : lhs->Next) {
        if (covers(rhs->Bounds, next.first)) {
            result->Next.push_back(next);
        }
    }

    for (const auto& next : rhs->Next) {
        if (covers(lhs->Bounds, next.first)) {
            result->Next.push_back(next);
        }
    }

    // TODO: merge and then adjacent_find
    for (const auto& next : lhs->Next) {
        auto eq = std::equal_range(rhs->Next.begin(), rhs->Next.end(), next.first, TKeyTrieComparer());
        if (eq.first != eq.second) {
            result->Next.emplace_back(next.first, IntersectKeyTrie(eq.first->second, next.second));
        }
    }

    std::sort(result->Next.begin(), result->Next.end(), TKeyTrieComparer());
    return result;
}

void GetRangesFromTrieWithinRangeImpl(
    const TKeyRange& keyRange,
    TKeyTriePtr trie,
    std::vector<std::pair<TRow, TRow>>* result,
    TRowBuffer* rowBuffer, 
    std::vector<TValue> prefix = std::vector<TValue>(),
    bool refineLower = true,
    bool refineUpper = true)
{
    auto lowerBoundSize = keyRange.first.GetCount();
    auto upperBoundSize = keyRange.second.GetCount();

    size_t offset = prefix.size();
    
    if (refineLower && offset >= lowerBoundSize) {
        refineLower = false;
    }

    if (refineUpper && offset >= upperBoundSize) {
        return;
    }

    YCHECK(!refineLower || offset < lowerBoundSize);
    YCHECK(!refineUpper || offset < upperBoundSize);

    TUnversionedRowBuilder builder(offset);

    if (trie->Offset > offset) {
        if (refineLower && refineUpper && keyRange.first[offset] == keyRange.second[offset]) {
            prefix.emplace_back(keyRange.first[offset]);
            GetRangesFromTrieWithinRangeImpl(
                keyRange,
                trie,
                result,
                rowBuffer,
                prefix,
                true,
                true);
        } else {
            std::pair<TRow, TRow> range;
            for (size_t i = 0; i < offset; ++i) {
                builder.AddValue(prefix[i]);
            }

            if (refineLower) {
                for (size_t i = offset; i < lowerBoundSize; ++i) {
                    builder.AddValue(keyRange.first[i]);
                }
            }
            range.first = rowBuffer->Capture(builder.GetRow());
            builder.Reset();


            for (size_t i = 0; i < offset; ++i) {
                builder.AddValue(prefix[i]);
            }

            if (refineUpper) {
                for (size_t i = offset; i < upperBoundSize; ++i) {
                    builder.AddValue(keyRange.second[i]);
                }
            } else {
                builder.AddValue(MakeUnversionedSentinelValue(EValueType::Max));
            }
            range.second = rowBuffer->Capture(builder.GetRow());
            builder.Reset();

            if (!IsEmpty(range)) {
                result->push_back(range);
            }            
        }
        return;
    }

    YCHECK(trie->Offset == offset);

    const auto& resultBounds = trie->Bounds;
    YCHECK(!(resultBounds.size() & 1));

    for (size_t i = 0; i + 1 < resultBounds.size(); i += 2) {
        auto lower = resultBounds[i];
        auto upper = resultBounds[i + 1];

        YCHECK(CompareBound(lower, upper, true, false) < 0);

        auto keyRangeLowerBound = TBound(keyRange.first[offset], true);
        auto keyRangeUpperBound = TBound(keyRange.second[offset], offset + 1 < upperBoundSize);

        bool lowerBoundRefined = false;
        if (refineLower) {
            if (CompareBound(upper, keyRangeLowerBound, false, true) < 0) {
                continue;
            } else if (CompareBound(lower, keyRangeLowerBound, true, true) <= 0) {
                lowerBoundRefined = true;
            }
        }

        bool upperBoundRefined = false;
        if (refineUpper) {
            if (CompareBound(lower, keyRangeUpperBound, true, false) > 0) {
                continue;
            } else if (CompareBound(upper, keyRangeUpperBound, false, false) >= 0) {
                upperBoundRefined = true;
            }
        }

        std::pair<TRow, TRow> range;
        for (size_t j = 0; j < offset; ++j) {
            builder.AddValue(prefix[j]);
        }

        if (lowerBoundRefined) {
            for (size_t j = offset; j < lowerBoundSize; ++j) {
                builder.AddValue(keyRange.first[j]);
            }
        } else {
            builder.AddValue(lower.Value);

            if (!lower.Included) {
                builder.AddValue(MakeUnversionedSentinelValue(EValueType::Max));
            }
        }
        
        range.first = rowBuffer->Capture(builder.GetRow());
        builder.Reset();

        for (size_t j = 0; j < offset; ++j) {
            builder.AddValue(prefix[j]);
        }

        if (upperBoundRefined) {
            for (size_t j = offset; j < upperBoundSize; ++j) {
                builder.AddValue(keyRange.second[j]);
            }
        } else {
            builder.AddValue(upper.Value);

            if (upper.Included) {
                builder.AddValue(MakeUnversionedSentinelValue(EValueType::Max));
            }
        }

        range.second = rowBuffer->Capture(builder.GetRow());
        builder.Reset();
        result->push_back(range);
    }

    prefix.emplace_back();

    for (const auto& next : trie->Next) {
        auto value = next.first;

        bool refineLowerNext = false;
        if (refineLower) {
            if (value < keyRange.first[offset]) {
                continue;
            } else if (value == keyRange.first[offset]) {
                refineLowerNext = true;
            }
        }

        bool refineUpperNext = false;
        if (refineUpper) {
            if (value > keyRange.second[offset]) {
                continue;
            } else if (value == keyRange.second[offset]) {
                refineUpperNext = true;
            }
        }

        prefix.back() = value;

        GetRangesFromTrieWithinRangeImpl(
            keyRange,
            next.second,
            result,
            rowBuffer,
            prefix,
            refineLowerNext,
            refineUpperNext);
    }
}

std::vector<std::pair<TRow, TRow>> GetRangesFromTrieWithinRange(
    const TKeyRange& keyRange,
    TKeyTriePtr trie,
    TRowBuffer* rowBuffer)
{
    std::vector<std::pair<TRow, TRow>> result;

    GetRangesFromTrieWithinRangeImpl(keyRange, trie, &result, rowBuffer);

    if (!result.empty()) {
        std::sort(result.begin(), result.end());
        std::vector<std::pair<TRow, TRow>> mergedResult;
        mergedResult.push_back(result.front());

        for (size_t i = 1; i < result.size(); ++i) {
            if (mergedResult.back().second == result[i].first) {
                mergedResult.back().second = std::move(result[i].second);
            } else {
                mergedResult.push_back(std::move(result[i]));
            }
        }

        result = std::move(mergedResult);
    }

    return result;
}

Stroka ToString(TKeyTriePtr node) {
    auto printOffset = [](int offset) {
        Stroka str;
        for (int i = 0; i < offset; ++i) {
            str += "  ";
        }
        return str;
    };

    std::function<Stroka(TKeyTriePtr, size_t)> printNode =
        [&](TKeyTriePtr node, size_t offset) {
            Stroka str;
            str += printOffset(offset);
            if (node->Offset == std::numeric_limits<size_t>::max()) {
                str += "(universe)";
            } else {
                str += "(key";
                str += NYT::ToString(node->Offset);
                str += ", { ";

                for (int i = 0; i < node->Bounds.size(); i += 2) {
                    str += node->Bounds[i].Included ? "[" : "(";
                    str += ToString(node->Bounds[i].Value);
                    str += ":";
                    str += ToString(node->Bounds[i+1].Value);
                    str += node->Bounds[i+1].Included ? "]" : ")";
                    if (i + 2 < node->Bounds.size()) {
                        str += ", ";
                    }
                }

                str += " })";

                for (const auto& next : node->Next) {
                    str += "\n";
                    str += printOffset(node->Offset);
                    str += ToString(next.first);
                    str += ":\n";
                    str += printNode(next.second, offset + 1);
                }
            }
            return str;
        };

    return printNode(node, 0);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
