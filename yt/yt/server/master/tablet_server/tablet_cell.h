#pragma once

#include "public.h"
#include "tablet.h"
#include "tablet_action.h"

#include <yt/yt/server/master/cell_master/public.h>
#include <yt/yt/server/master/cell_master/gossip_value.h>

#include <yt/yt/server/master/cell_server/cell_base.h>

#include <yt/yt/server/master/node_tracker_server/public.h>

#include <yt/yt/server/master/object_server/object.h>

#include <yt/yt/server/master/transaction_server/public.h>

#include <yt/yt/ytlib/hive/cell_directory.h>

#include <yt/yt/ytlib/node_tracker_client/proto/node_tracker_service.pb.h>
#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/property.h>
#include <yt/yt/core/misc/ref_tracked.h>
#include <yt/yt/core/misc/compact_vector.h>

#include <yt/yt/core/yson/public.h>

namespace NYT::NTabletServer {

////////////////////////////////////////////////////////////////////////////////

class TTabletCell
    : public NCellServer::TCellBase
{
public:
    DEFINE_BYREF_RW_PROPERTY(THashSet<TTabletBase*>, Tablets);
    DEFINE_BYREF_RW_PROPERTY(THashSet<TTabletAction*>, Actions);

    using TGossipStatistics = NCellMaster::TGossipValue<TTabletCellStatistics>;
    DEFINE_BYREF_RW_PROPERTY(TGossipStatistics, GossipStatistics);

    DECLARE_BYVAL_RO_PROPERTY(TTabletCellBundle*, TabletCellBundle);

public:
    using TCellBase::TCellBase;

    TString GetLowercaseObjectName() const override;
    TString GetCapitalizedObjectName() const override;

    void Save(NCellMaster::TSaveContext& context) const override;
    void Load(NCellMaster::TLoadContext& context) override;

    NHiveClient::TCellDescriptor GetDescriptor() const override;

    //! Recompute cluster statistics from multicell statistics.
    void RecomputeClusterStatistics();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
