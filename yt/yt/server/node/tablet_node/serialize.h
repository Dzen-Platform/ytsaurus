#pragma once

#include "public.h"

#include <yt/yt/server/lib/hydra/composite_automaton.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

// If reign change is disallowed, tablet node will crash if cell snapshot reign
// differs from node reign. This is useful for local mode where occasional cell
// state migration may end up with a disaster.
void SetReignChangeAllowed(bool allowed);
bool IsReignChangeAllowed();

////////////////////////////////////////////////////////////////////////////////

NHydra::TReign GetCurrentReign();
bool ValidateSnapshotReign(NHydra::TReign);
NHydra::EFinalRecoveryAction GetActionToRecoverFromReign(NHydra::TReign reign);

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETabletReign,
    ((SafeReplicatedLogSchema)            (100012)) // savrus
    ((BulkInsert)                         (100013)) // savrus
    ((GiantTabletProblem)                 (100014)) // akozhikhov
    ((ChunkViewsForPivots)                (100015)) // akozhikhov
    ((BulkInsertOverwrite)                (100016)) // ifsmirnov
    ((ChunkViewWideRange_YT_12532)        (100017)) // ifsmirnov
    ((DynamicStoreRead)                   (100100)) // ifsmirnov
    ((AuthenticationIdentity)             (100101)) // babenko
    ((MountHint)                          (100102)) // ifsmirnov
    ((ReplicationBarrier_YT_14346)        (100103)) // babenko
    ((AllowFlushWhenDecommissioned)       (100104)) // savrus
    ((PersistChunkTimestamp_20_3)         (100105)) // ifsmirnov
    // 21.2 starts here.
    ((RowBufferEmptyRowDeserialization)   (100200)) // max42
    ((Hunks1)                             (100201)) // babenko
    ((Hunks2)                             (100202)) // babenko
    ((PersistChunkTimestamp)              (100203)) // ifsmirnov
    ((SchemaIdUponMount)                  (100204)) // akozhikhov
    ((VersionedWriteToOrderedTablet)      (100205)) // gritukan
    // 21.3 starts here.
    ((WriteGenerations)                   (100301)) // max42
);

////////////////////////////////////////////////////////////////////////////////

class TSaveContext
    : public NHydra::TSaveContext
{
public:
    ETabletReign GetVersion() const;
};

////////////////////////////////////////////////////////////////////////////////

class TLoadContext
    : public NHydra::TLoadContext
{
public:
    ETabletReign GetVersion() const;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
