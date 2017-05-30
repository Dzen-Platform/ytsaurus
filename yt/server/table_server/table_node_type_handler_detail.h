#pragma once

#include "table_node.h"

#include <yt/server/chunk_server/chunk_owner_type_handler.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NTableServer {

////////////////////////////////////////////////////////////////////////////////

template <class TImpl>
class TTableNodeTypeHandlerBase
    : public NChunkServer::TChunkOwnerTypeHandler<TImpl>
{
public:
    typedef NChunkServer::TChunkOwnerTypeHandler<TImpl> TBase;

    explicit TTableNodeTypeHandlerBase(NCellMaster::TBootstrap* bootstrap);

protected:
    virtual std::unique_ptr<TImpl> DoCreate(
        const NCypressServer::TVersionedNodeId& id,
        NObjectClient::TCellTag cellTag,
        NTransactionServer::TTransaction* transaction,
        NYTree::IAttributeDictionary* attributes,
        NSecurityServer::TAccount* account,
        bool enableAccounting) override;

    virtual void DoDestroy(TImpl* table) override;

    virtual void DoBranch(
        const TImpl* originatingNode,
        TImpl* branchedNode,
        NCypressClient::ELockMode mode) override;
    virtual void DoMerge(
        TImpl* originatingNode,
        TImpl* branchedNode) override;
    virtual void DoClone(
        TImpl* sourceNode,
        TImpl* clonedNode,
        NCypressServer::ICypressNodeFactory* factory,
        NCypressServer::ENodeCloneMode mode,
        NSecurityServer::TAccount* account) override;

    virtual int GetDefaultReplicationFactor() const override;

    virtual NSecurityServer::TClusterResources GetAccountingResourceUsage(
        const NCypressServer::TCypressNodeBase* table) override;

};

////////////////////////////////////////////////////////////////////////////////

class TTableNodeTypeHandler
    : public TTableNodeTypeHandlerBase<TTableNode>
{
public:
    typedef TTableNodeTypeHandlerBase<TTableNode> TBase;

    explicit TTableNodeTypeHandler(NCellMaster::TBootstrap* bootstrap);

    virtual NObjectClient::EObjectType GetObjectType() const override;
    virtual bool IsExternalizable() const override;

protected:
    virtual NCypressServer::ICypressNodeProxyPtr DoGetProxy(
        TTableNode* trunkNode,
        NTransactionServer::TTransaction* transaction) override;

};

////////////////////////////////////////////////////////////////////////////////

class TReplicatedTableNodeTypeHandler
    : public TTableNodeTypeHandlerBase<TReplicatedTableNode>
{
public:
    typedef TTableNodeTypeHandlerBase<TReplicatedTableNode> TBase;

    explicit TReplicatedTableNodeTypeHandler(NCellMaster::TBootstrap* bootstrap);

    virtual NObjectClient::EObjectType GetObjectType() const override;

protected:
    virtual NCypressServer::ICypressNodeProxyPtr DoGetProxy(
        TReplicatedTableNode* trunkNode,
        NTransactionServer::TTransaction* transaction) override;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT

