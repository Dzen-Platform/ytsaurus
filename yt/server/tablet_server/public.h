#pragma once

#include <core/misc/public.h>
#include <core/misc/enum.h>

#include <ytlib/hydra/public.h>

#include <ytlib/tablet_client/public.h>

namespace NYT {
namespace NTabletServer {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETabletCellHealth,
    ((Initializing)(0))
    ((Good)        (1))
    ((Degraded)    (2))
    ((Failed)      (3))
);

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TTabletManager)

DECLARE_REFCOUNTED_CLASS(TTabletManagerConfig)

class TTabletCellBundle;
class TTabletCell;
class TTablet;

struct TTabletStatistics;
struct TTabletPerformanceCounter;
struct TTabletPerformanceCounters;

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
using NTabletClient::TypicalCellSize;

using NTabletClient::TTabletCellConfig;
using NTabletClient::TTabletCellConfigPtr;
using NTabletClient::TTabletCellOptions;
using NTabletClient::TTabletCellOptionsPtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
