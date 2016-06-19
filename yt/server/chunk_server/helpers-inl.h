#ifndef HELPERS_INL_H_
#error "Direct inclusion of this file is not allowed, include helpers.h"
#endif

#include "chunk.h"
#include "chunk_list.h"

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

template <class F>
void VisitUniqueAncestors(TChunkList* chunkList, F functor)
{
    while (chunkList != nullptr) {
        functor(chunkList);
        const auto& parents = chunkList->Parents();
        if (parents.empty())
            break;
        YCHECK(parents.size() == 1);
        chunkList = *parents.begin();
    }
}

template <class F>
void VisitAncestors(TChunkList* chunkList, F functor)
{
    // BFS queue. Try to avoid allocations.
    SmallVector<TChunkList*, 64> queue;
    size_t frontIndex = 0;

    // Put seed into the queue.
    queue.push_back(chunkList);

    // The main loop.
    while (frontIndex < queue.size()) {
        auto* chunkList = queue[frontIndex++];

        // Fast lane: handle unique parents.
        while (chunkList != nullptr) {
            functor(chunkList);
            const auto& parents = chunkList->Parents();
            if (parents.size() != 1)
                break;
            chunkList = *parents.begin();
        }

        if (chunkList != nullptr) {
            // Proceed to parents.
            for (auto* parent : chunkList->Parents()) {
                queue.push_back(parent);
            }
        }
    }
}

template <class F>
void AttachToChunkList(
    TChunkList* chunkList,
    TChunkTree** childrenBegin,
    TChunkTree** childrenEnd,
    F childAction)
{
    // A shortcut.
    if (childrenBegin == childrenEnd)
        return;

    // NB: Accumulate statistics from left to right to get Sealed flag correct.
    TChunkTreeStatistics statisticsDelta;
    for (auto it = childrenBegin; it != childrenEnd; ++it) {
        chunkList->ValidateSealed();
        auto* child = *it;
        AppendChunkTreeChild(chunkList, child, &statisticsDelta);
        SetChunkTreeParent(chunkList, child);
        childAction(child);
    }

    chunkList->IncrementVersion();

    // Go upwards and apply delta.
    AccumulateUniqueAncestorsStatistics(chunkList, statisticsDelta);
}

template <class F>
void DetachFromChunkList(
    TChunkList* chunkList,
    TChunkTree** childrenBegin,
    TChunkTree** childrenEnd,
    F childAction)
{
    // A shortcut.
    if (childrenBegin == childrenEnd) {
        return;
    }

    YCHECK(!chunkList->GetOrdered());

    chunkList->IncrementVersion();

    auto& childToIndex = chunkList->ChildToIndex();
    auto& children = chunkList->Children();
    TChunkTreeStatistics statisticsDelta;
    for (auto childIt = childrenBegin; childIt != childrenEnd; ++childIt) {
        auto* child = *childIt;
        auto indexIt = childToIndex.find(child);
        YCHECK(indexIt != childToIndex.end());
        int index = indexIt->second;
        if (index != children.size() - 1) {
            children[index] = children.back();
            childToIndex[children[index]] = index;
        }
        children.pop_back();
        statisticsDelta.Accumulate(GetChunkTreeStatistics(child));
    }

    // Go upwards and recompute statistics.
    VisitUniqueAncestors(
        chunkList,
        [&] (TChunkList* current) {
            current->Statistics().Deaccumulate(statisticsDelta);
        });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
