#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/object_server/object_detail.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/ref_tracked.h>

namespace NYT {
namespace NNodeTrackerServer {

////////////////////////////////////////////////////////////////////////////////

class TRack
    : public NObjectServer::TObjectBase
    , public TRefTracked<TRack>
{
public:
    DEFINE_BYVAL_RW_PROPERTY(Stroka, Name);
    DEFINE_BYVAL_RW_PROPERTY(int, Index);

public:
    explicit TRack(const TRackId& id);

    TRackSet GetIndexMask() const;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
