#pragma once

#include <yt/yt/server/master/tablet_server/public.h>

#include <yt/yt/server/lib/hydra_common/public.h>

#include <yt/yt/ytlib/hydra/public.h>

#include <yt/yt/ytlib/cellar_client/public.h>

#include <yt/yt/ytlib/tablet_client/public.h>

#include <yt/yt/core/misc/compact_vector.h>
#include <yt/yt/core/misc/enum.h>
#include <yt/yt/core/misc/public.h>

namespace NYT::NCellServer {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TCellStatus;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

using NHydra::TPeerId;
using NHydra::InvalidPeerId;
using NHydra::EPeerState;

using TCellBundleId = NTabletClient::TTabletCellBundleId;
using NTabletClient::NullTabletCellBundleId;
using TTamedCellId = NTabletClient::TTabletCellId;
using NTabletClient::NullTabletCellId;
using NTabletClient::TypicalPeerCount;
using TAreaId = NObjectClient::TObjectId;

using NTabletClient::TTabletCellOptions;
using NTabletClient::TTabletCellOptionsPtr;
using NTabletClient::TDynamicTabletCellOptions;
using NTabletClient::TDynamicTabletCellOptionsPtr;
using ECellHealth = NTabletClient::ETabletCellHealth;
using ECellLifeStage = NTabletClient::ETabletCellLifeStage;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(ITamedCellManager)
DECLARE_REFCOUNTED_CLASS(TBundleNodeTracker)
DECLARE_REFCOUNTED_CLASS(TCellBaseDecommissioner)
DECLARE_REFCOUNTED_CLASS(TCellHydraJanitor)

DECLARE_REFCOUNTED_STRUCT(ICellBalancerProvider)
DECLARE_REFCOUNTED_STRUCT(ICellarNodeTracker)

DECLARE_REFCOUNTED_CLASS(TCellBalancerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicCellarNodeTrackerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicCellManagerConfig)

struct ICellBalancer;

DECLARE_ENTITY_TYPE(TCellBundle, TCellBundleId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TCellBase, TTamedCellId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TArea, TAreaId, NObjectClient::TDirectObjectIdHash)

extern const TString DefaultCellBundleName;
extern const TString DefaultAreaName;

using TCellSet = TCompactVector<std::pair<const TCellBase*, int>, NCellarClient::TypicalCellarSize>;

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_ERROR_ENUM(
    ((NodeDecommissioned)           (1401))
    ((NodeBanned)                   (1402))
    ((NodeTabletSlotsDisabled)      (1403))
    ((NodeFilterMismatch)           (1404))
    ((CellDidNotAppearWithinTimeout)(1405))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellServer
