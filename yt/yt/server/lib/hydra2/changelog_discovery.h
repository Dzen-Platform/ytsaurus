#pragma once

#include "public.h"

#include <yt/yt/ytlib/election/public.h>

#include <yt/yt/core/actions/future.h>

namespace NYT::NHydra2 {

////////////////////////////////////////////////////////////////////////////////

struct TChangelogInfo
{
    TPeerId PeerId = InvalidPeerId;
    int ChangelogId = InvalidSegmentId;
    int RecordCount = -1;
};

//! Looks for a changelog with a given id containing the desired number of records.
/*!
 *  If none are found, then |InvalidSegmentId| is returned in the info.
 */
TFuture<TChangelogInfo> DiscoverChangelog(
    TDistributedHydraManagerConfigPtr config,
    NElection::TCellManagerPtr cellManager,
    int changelogId,
    int minRecordCount);

////////////////////////////////////////////////////////////////////////////////

struct TChangelogQuorumInfo
{
    //! The lower bound for the number of committed records.
    int RecordCountLo;
    //! The upper bound for the number of committed records.
    int RecordCountHi;
};

//! Given #changelogId, computes the quorum info.
TFuture<TChangelogQuorumInfo> ComputeChangelogQuorumInfo(
    TDistributedHydraManagerConfigPtr config,
    NElection::TCellManagerPtr cellManager,
    int changelogId,
    int localRecordCount);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra2
