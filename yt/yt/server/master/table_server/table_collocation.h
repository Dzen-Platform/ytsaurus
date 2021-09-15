#pragma once

#include "public.h"

#include <yt/yt/server/master/object_server/object.h>

#include <yt/yt/server/master/table_server/public.h>

#include <yt/yt/client/object_client/public.h>

#include <yt/yt/core/misc/ref_tracked.h>

namespace NYT::NTableServer {

////////////////////////////////////////////////////////////////////////////////

class TTableCollocation
    : public NObjectServer::TObject
    , public TRefTracked<TTableCollocation>
{
public:
    DEFINE_BYVAL_RW_PROPERTY(NObjectClient::TCellTag, ExternalCellTag, NObjectClient::InvalidCellTag);
    DEFINE_BYREF_RW_PROPERTY(THashSet<TTableNode*>, Tables);
    DEFINE_BYVAL_RW_PROPERTY(ETableCollocationType, Type);

public:
    using TObject::TObject;

    virtual TString GetLowercaseObjectName() const override;
    virtual TString GetCapitalizedObjectName() const override;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableServer
