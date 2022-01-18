#pragma once

#include <yt/yt/server/lib/hydra_common/public.h>

#include <yt/yt/ytlib/hydra/public.h>

#include <yt/yt/ytlib/tablet_client/public.h>
#include <yt/yt/ytlib/tablet_client/backup.h>

#include <yt/yt/core/misc/arithmetic_formula.h>
#include <yt/yt/core/misc/enum.h>
#include <yt/yt/core/misc/public.h>

////////////////////////////////////////////////////////////////////////////////

namespace NYT::NTableClient::NProto {

class TRspCheckBackupCheckpoint;

} // namespace NYT::NTableClient::NProto

////////////////////////////////////////////////////////////////////////////////

namespace NYT::NTabletServer {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TTabletCellStatistics;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

using NHydra::TPeerId;
using NHydra::InvalidPeerId;
using NHydra::EPeerState;

using NTabletClient::TTabletCellBundleId;
using NTabletClient::NullTabletCellBundleId;
using NTabletClient::TTabletCellId;
using NTabletClient::NullTabletCellId;
using NTabletClient::TTabletId;
using NTabletClient::NullTabletId;
using NTabletClient::TStoreId;
using NTabletClient::ETabletState;
using NTabletClient::ETableReplicaMode;
using NTabletClient::TypicalPeerCount;
using NTabletClient::TTableReplicaId;
using NTabletClient::TTabletActionId;
using NTabletClient::TTableReplicaId;

using NTabletClient::TTabletCellOptions;
using NTabletClient::TTabletCellOptionsPtr;
using NTabletClient::TDynamicTabletCellOptions;
using NTabletClient::TDynamicTabletCellOptionsPtr;
using NTabletClient::ETabletCellHealth;
using NTabletClient::ETableReplicaState;
using NTabletClient::ETabletActionKind;
using NTabletClient::ETabletActionState;
using NTabletClient::ETableBackupState;
using NTabletClient::ETabletBackupState;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TTabletManager)
DECLARE_REFCOUNTED_CLASS(TTabletService)
DECLARE_REFCOUNTED_CLASS(TTabletBalancer)
DECLARE_REFCOUNTED_CLASS(TTabletCellDecommissioner)
DECLARE_REFCOUNTED_CLASS(TTabletActionManager)
DECLARE_REFCOUNTED_CLASS(TReplicatedTableTracker)
DECLARE_REFCOUNTED_STRUCT(ITabletCellBalancerProvider)
DECLARE_REFCOUNTED_STRUCT(ITabletNodeTracker)
DECLARE_REFCOUNTED_STRUCT(IBackupManager)

struct ITabletCellBalancer;

DECLARE_REFCOUNTED_CLASS(TTabletBalancerConfig)
DECLARE_REFCOUNTED_CLASS(TTabletBalancerMasterConfig)
DECLARE_REFCOUNTED_CLASS(TTabletCellDecommissionerConfig)
DECLARE_REFCOUNTED_CLASS(TTabletActionManagerMasterConfig)
DECLARE_REFCOUNTED_CLASS(TReplicatedTableTrackerExpiringCacheConfig)
DECLARE_REFCOUNTED_CLASS(TReplicatedTableTrackerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicTabletCellBalancerMasterConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicTabletManagerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicReplicatedTableTrackerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicTablesMulticellGossipConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicTabletNodeTrackerConfig)

class TTableReplica;

DECLARE_ENTITY_TYPE(TTabletCellBundle, TTabletCellBundleId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TTabletCell, TTabletCellId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TTablet, TTabletId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TTableReplica, TTableReplicaId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TTabletAction, TTabletActionId, NObjectClient::TDirectObjectIdHash)

struct TTabletStatistics;
struct TTabletPerformanceCounter;
struct TTabletPerformanceCounters;

extern const TString DefaultTabletCellBundleName;

extern const TTimeFormula DefaultTabletBalancerSchedule;

constexpr i64 EdenStoreIdsSizeLimit = 100;

constexpr auto DefaultSyncTabletActionKeepalivePeriod = TDuration::Minutes(1);

constexpr int DefaultTabletCountLimit = 1000;

constexpr int MaxStoresPerBackupMutation = 10000;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
