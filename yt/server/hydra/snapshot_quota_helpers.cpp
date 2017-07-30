#include "snapshot_quota_helpers.h"

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

namespace {

TNullable<int> ChooseMaxThreshold(TNullable<int> firstThreshold, TNullable<int> secondThreshold)
{
    if (!secondThreshold) {
        return firstThreshold;
    }
    if (!firstThreshold) {
        return secondThreshold;
    }
    return MakeNullable(std::max(*firstThreshold, *secondThreshold));
}

} // namespace

int GetSnapshotThresholdId(
    std::vector<TSnapshotInfo> snapshots,
    TNullable<int> maxSnapshotCountToKeep,
    TNullable<i64> maxSnapshotSizeToKeep)
{
    if (snapshots.size() <= 1) {
        return -1;
    }

    std::sort(snapshots.begin(), snapshots.end(), [] (const TSnapshotInfo& lhs, const TSnapshotInfo& rhs) {
        return lhs.Id < rhs.Id;
    });

    int thresholdByCountId = -1;
    if (maxSnapshotCountToKeep && snapshots.size() > *maxSnapshotCountToKeep) {
        auto index = snapshots.size() - std::max(1, *maxSnapshotCountToKeep) - 1;
        thresholdByCountId = snapshots[index].Id;
    }

    i64 totalSize = 0;
    for (const auto& snapshot : snapshots) {
        totalSize += snapshot.Size;
    }

    int thresholdBySizeId = -1;
    if (maxSnapshotSizeToKeep && totalSize > *maxSnapshotSizeToKeep) {
        for (auto it = snapshots.begin(); it != snapshots.end() - 1; ++it) {
            const auto& snapshot = *it;
            totalSize -= snapshot.Size;
            thresholdBySizeId = snapshot.Id;
            if (totalSize <= *maxSnapshotSizeToKeep) {
                break;
            }
        }
    }

    int thresholdId = std::max(thresholdByCountId, thresholdBySizeId);

    // Make sure we never delete the latest snapshot.
    YCHECK(snapshots.back().Id > thresholdId);
    return thresholdId;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
