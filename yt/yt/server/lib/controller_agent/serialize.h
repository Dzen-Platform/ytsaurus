#pragma once

#include "public.h"

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ESnapshotVersion,
    // 21.3 start here
    ((HostObjects)                          (300701))
    ((FixSimpleSort)                        (300702))
    ((InputStreamPerRange)                  (300703))
    ((RemoteCopyNetworks)                   (300704))
    ((AddAccountIntoUserObject)             (300705))
    // 22.1 start here
    ((RefactorStatistics)                   (300801))
    ((AddTotalJobStatistics)                (300802))
    ((NeededResourcesByPoolTree)            (300803))
    ((DiskResourcesInSanityCheck)           (300804))
    ((AlertManager)                         (300805))
    ((MaxClipTimestamp)                     (300806))
    ((StripedErasureChunks)                 (300807))
    ((LowGpuPowerUsageOnWindow)             (300808))
    ((OptionalJobResults)                   (300809))
    ((MemoryReserve)                        (300810))
    ((FixDiskAccountPersistence)            (300811))
    // 22.1 but cherry-pick later
    ((StripedErasureTables)                 (300858))
    ((ProbingJobs)                          (300859))
    // 22.2 start here
    ((MajorUpdateTo22_2)                    (300901))
    ((DropUnavailableInputChunkCount)       (300902))
    ((ResourceOverdraftState)               (300903))
    ((ResourceOverdraftJobProxy)            (300904))
    ((ResourceOverdraftJobId)               (300905))
    ((DropLogAndProfile)                    (300906))
    ((ProbingJobsFix)                       (300907))
    // 22.3 start here
    ((DropUnusedOperationId)                (301001))
);

////////////////////////////////////////////////////////////////////////////////

ESnapshotVersion GetCurrentSnapshotVersion();
bool ValidateSnapshotVersion(int version);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
