#pragma once

#include "public.h"

#include <core/misc/config.h>

#include <core/ytree/yson_serializable.h>

#include <ytlib/election/config.h>

#include <ytlib/hydra/config.h>

namespace NYT {
namespace NTabletClient {

///////////////////////////////////////////////////////////////////////////////

//! These options are directly controllable via object attributes.
class TTabletCellOptions
    : public NHydra::TRemoteSnapshotStoreOptions
    , public NHydra::TRemoteChangelogStoreOptions
{
public:
    TTabletCellOptions()
    { }
};

DEFINE_REFCOUNTED_TYPE(TTabletCellOptions)

///////////////////////////////////////////////////////////////////////////////

class TTabletCellConfig
    : public NYTree::TYsonSerializable
{
public:
    std::vector<TNullable<Stroka>> Addresses;

    TTabletCellConfig()
    {
        RegisterParameter("addresses", Addresses);
    }

    NElection::TCellConfigPtr ToElection(const NElection::TCellId& cellId) const
    {
        auto result = New<NElection::TCellConfig>();
        result->CellId = cellId;
        result->Addresses = Addresses;
        return result;
    }
};

DEFINE_REFCOUNTED_TYPE(TTabletCellConfig)

///////////////////////////////////////////////////////////////////////////////

class TTableMountCacheConfig
    : public TExpiringCacheConfig
{
public:
    TTableMountCacheConfig()
    { }
};

DEFINE_REFCOUNTED_TYPE(TTableMountCacheConfig)

///////////////////////////////////////////////////////////////////////////////

} // namespace NTabletClient
} // namespace NYT
