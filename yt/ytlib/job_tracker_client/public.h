#pragma once

#include <yt/core/misc/enum.h>
#include <yt/core/misc/guid.h>
#include <yt/core/misc/public.h>

namespace NYT {
namespace NJobTrackerClient {

///////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TJobSpec;
class TReqHeartbeat;
class TRspHeartbeat;
class TJobResult;
class TJobStatus;

} // namespace NProto

///////////////////////////////////////////////////////////////////////////////

using TJobId = TGuid;
extern const TJobId NullJobId;

using TOperationId = TGuid;
extern const TOperationId NullOperationId;

///////////////////////////////////////////////////////////////////////////////

// NB: Please keep the range of values small as this type
// is used as a key of TEnumIndexedVector.
DEFINE_ENUM(EJobType,
    // Scheduler jobs
    ((SchedulerFirst)    (  0)) // sentinel
    ((Map)               (  1))
    ((PartitionMap)      (  2))
    ((SortedMerge)       (  3))
    ((OrderedMerge)      (  4))
    ((UnorderedMerge)    (  5))
    ((Partition)         (  6))
    ((SimpleSort)        (  7))
    ((FinalSort)         (  8))
    ((SortedReduce)      (  9))
    ((PartitionReduce)   ( 10))
    ((ReduceCombiner)    ( 11))
    ((RemoteCopy)        ( 12))
    ((IntermediateSort)  ( 13))
    ((OrderedMap)        ( 14))
    ((SchedulerLast)     ( 99)) // sentinel

    // Master jobs
    ((ReplicateChunk)    (100))
    ((RemoveChunk)       (101))
    ((RepairChunk)       (102))
    ((SealChunk)         (103))
);

// NB: Please keep the range of values small as this type
// is used as a key of TEnumIndexedVector.
DEFINE_ENUM(EJobState,
    ((Waiting)    (0))
    ((Running)    (1))
    ((Aborting)   (2))
    ((Completed)  (3))
    ((Failed)     (4))
    ((Aborted)    (5))
    // This sentinel is only used in TJob::GetStatisticsSuffix.
    ((Lost)       (7))
    // Initial state of newly created job.
    ((None)       (8))
);

DEFINE_ENUM(EJobPhase,
    ((Created)               (  0))
    ((DownloadingArtifacts)  (  1))
    ((PreparingConfig)       (  4))
    ((PreparingProxy)        (  7))
    ((PreparingSandbox)      ( 10))
    ((PreparingTmpfs)        ( 15))
    ((PreparingArtifacts)    ( 20))
    ((Running)               ( 50))
    ((Cleanup)               ( 80))
    ((Finished)              (100))
);

///////////////////////////////////////////////////////////////////////////////

} // namespace NJobTrackerClient
} // namespace NYT
