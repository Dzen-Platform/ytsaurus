#pragma once

#include "public.h"

#include <yt/core/misc/ref_tracked.h>

#include <yt/server/master/object_server/object.h>

#include <yt/server/master/cell_master/public.h>

#include <yt/server/master/cell_master/serialize.h>

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/tablet_client/public.h>

namespace NYT::NTabletServer {

////////////////////////////////////////////////////////////////////////////////

class TTabletAction
    : public NObjectServer::TNonversionedObjectBase
    , public TRefTracked<TTabletAction>
{
public:
    //! Action kind: move, reshard.
    DEFINE_BYVAL_RW_PROPERTY(ETabletActionKind, Kind);

    //! Current state.
    DEFINE_BYVAL_RW_PROPERTY(ETabletActionState, State);

    //! Participating tablets.
    DEFINE_BYREF_RW_PROPERTY(std::vector<TTablet*>, Tablets);

    //! Tablet cells to mount tablet into (if present).
    DEFINE_BYREF_RW_PROPERTY(std::vector<TTabletCell*>, TabletCells);

    //! Pivot keys for reshard (if present).
    DEFINE_BYREF_RW_PROPERTY(std::vector<NTableClient::TOwningKey>, PivotKeys);

    //! Desired number of tablets (for reshard).
    DEFINE_BYVAL_RW_PROPERTY(std::optional<int>, TabletCount);

    //! Skip initial freezing pass while performing tablet action.
    DEFINE_BYVAL_RW_PROPERTY(bool, SkipFreezing);

    //! Freeze tablets when action is completed.
    DEFINE_BYVAL_RW_PROPERTY(bool, Freeze);

    //! Contains error if tablet action failed.
    DEFINE_BYREF_RW_PROPERTY(TError, Error);

    //! Random guid to help connecting tablet balancer logs with tablet actions.
    DEFINE_BYVAL_RW_PROPERTY(TGuid, CorrelationId);

    //! When finished, action will not be destroyed until this time.
    DEFINE_BYVAL_RW_PROPERTY(TInstant, ExpirationTime);

    //! Tablet cell bundle of the participating tablets.
    DEFINE_BYVAL_RW_PROPERTY(TTabletCellBundle*, TabletCellBundle);

public:
    explicit TTabletAction(TTabletActionId id);

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    bool IsFinished() const;
};

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TTabletAction& action);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
