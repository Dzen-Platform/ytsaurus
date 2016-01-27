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

} // namespace NProto

///////////////////////////////////////////////////////////////////////////////

typedef TGuid TJobId;
extern const TJobId NullJobId;

///////////////////////////////////////////////////////////////////////////////

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

DEFINE_ENUM(EJobState,
    ((Waiting)    (0))
    ((Running)    (1))
    ((Aborting)   (2))
    ((Completed)  (3))
    ((Failed)     (4))
    ((Aborted)    (5))
    ((Abandoning) (6))
);

DEFINE_ENUM(EJobPhase,
    ((Created)         (  0))
    ((PreparingConfig) (  1))
    ((PreparingProxy)  (  2))
    ((PreparingSandbox)( 10))
    ((PreparingTmpfs)  ( 15))
    ((PreparingFiles)  ( 20))
    ((Running)         ( 50))
    ((Cleanup)         ( 80))
    ((Finished)        (100))
);

///////////////////////////////////////////////////////////////////////////////

} // namespace NJobTrackerClient
} // namespace NYT
