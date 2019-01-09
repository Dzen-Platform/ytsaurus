#pragma once

#include "public.h"

#include <yt/server/object_server/object.h>

#include <yt/server/security_server/acl.h>

#include <yt/server/cell_master/public.h>

#include <yt/server/cell_master/serialize.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/core/misc/ref_tracked.h>
#include <yt/core/misc/arithmetic_formula.h>

#include <yt/core/profiling/public.h>

namespace NYT::NTabletServer {

////////////////////////////////////////////////////////////////////////////////

class TTabletCellBundle
    : public NObjectServer::TNonversionedObjectBase
    , public TRefTracked<TTabletCellBundle>
{
public:
    DECLARE_BYVAL_RW_PROPERTY(TString, Name);

    DEFINE_BYREF_RW_PROPERTY(NSecurityServer::TAccessControlDescriptor, Acd);

    DEFINE_BYVAL_RW_PROPERTY(TTabletCellOptionsPtr, Options);
    DECLARE_BYVAL_RW_PROPERTY(TDynamicTabletCellOptionsPtr, DynamicOptions);
    DEFINE_BYVAL_RO_PROPERTY(int, DynamicConfigVersion);
    DEFINE_BYREF_RW_PROPERTY(TBooleanFormula, NodeTagFilter);
    DEFINE_BYREF_RW_PROPERTY(TTabletBalancerConfigPtr, TabletBalancerConfig);

    DEFINE_BYREF_RW_PROPERTY(THashSet<TTabletCell*>, TabletCells);
    DEFINE_BYREF_RW_PROPERTY(THashSet<TTabletAction*>, TabletActions);
    DEFINE_BYVAL_RO_PROPERTY(int, ActiveTabletActionCount);

    DEFINE_BYVAL_RO_PROPERTY(NProfiling::TTagId, ProfilingTag);

public:
    explicit TTabletCellBundle(TTabletCellBundleId id);

    void IncreaseActiveTabletActionCount();
    void DecreaseActiveTabletActionCount();

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

private:
    TString Name_;
    TDynamicTabletCellOptionsPtr DynamicOptions_;

    void FillProfilingTag();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
