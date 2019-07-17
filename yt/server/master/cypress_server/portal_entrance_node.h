#pragma once

#include "node_detail.h"

#include <yt/core/misc/property.h>

namespace NYT::NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TPortalEntranceNode
    : public TCypressNodeBase
{
public:
    DEFINE_BYVAL_RW_PROPERTY(NObjectClient::TCellTag, ExitCellTag);

public:
    using TCypressNodeBase::TCypressNodeBase;

    virtual NYTree::ENodeType GetNodeType() const override;

    virtual void Save(NCellMaster::TSaveContext& context) const override;
    virtual void Load(NCellMaster::TLoadContext& context) override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
