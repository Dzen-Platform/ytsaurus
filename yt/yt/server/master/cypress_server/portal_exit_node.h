#pragma once

#include "node_detail.h"
#include "node_proxy_detail.h"

namespace NYT::NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TPortalExitNode
    : public TMapNode
{
public:
    DEFINE_BYVAL_RW_PROPERTY(bool, RemovalStarted);
    DEFINE_BYVAL_RW_PROPERTY(NObjectClient::TCellTag, EntranceCellTag);
    DEFINE_BYVAL_RW_PROPERTY(NYPath::TYPath, Path);
    DEFINE_BYVAL_RW_PROPERTY(std::optional<NYPath::TYPath>, Key);
    DEFINE_BYVAL_RW_PROPERTY(TNodeId, ParentId);

    DEFINE_BYREF_RW_PROPERTY(std::optional<TAttributes>, EffectiveInheritableAttributes);
    DEFINE_BYREF_RW_PROPERTY(std::optional<NYPath::TYPath>, EffectiveAnnotationPath);

    DEFINE_BYREF_RW_PROPERTY(NSecurityServer::TAccessControlDescriptor, DirectAcd);

public:
    friend class TPortalExitTypeHandler;

    explicit TPortalExitNode(TVersionedNodeId nodeId);
    explicit TPortalExitNode(NObjectClient::TObjectId objectId);

    void Save(NCellMaster::TSaveContext& context) const override;
    void Load(NCellMaster::TLoadContext& context) override;

    void FillTransientInheritableAttributes(TTransientAttributes* attributes) const override;
    void FillInheritableAttributes(TAttributes* attributes, bool legacyBehaviour) const override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
