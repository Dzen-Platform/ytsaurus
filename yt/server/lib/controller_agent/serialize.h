#pragma once

#include "private.h"

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

int GetCurrentSnapshotVersion();
bool ValidateSnapshotVersion(int version);

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ESnapshotVersion,
    ((JobMetricsJobStateFilter)             (300195))
    // 19.7 starts here
    ((ExternalizedTransactions)             (300200))
    ((DynamicTableWriterConfig)             (300201))
    ((JobSpeculationTimeout)                (300202))
    // 19.8 starts here
    ((GroupedSpeculativeJobs)               (300220))
    ((ImproveSpeculativeHeuristic)          (300221))
    // 20.1 starts here
    ((TabletIndexInMROverOrderedDynTables)  (300230))
    ((JobHasCompetitorsFlag)                (300231))
    ((RetainedJobsCoreInfoCount)            (300232))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
