#pragma once

#include "public.h"

#include <core/misc/small_vector.h>

#include <core/rpc/service_detail.h>

#include <ytlib/cypress_client/public.h>

#include <server/transaction_server/public.h>

#include <server/security_server/public.h>

#include <server/hydra/entity_map.h>

#include <server/cell_master/public.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TCypressManager
    : public TRefCounted
{
public:
    TCypressManager(
        TCypressManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);
    ~TCypressManager();

    void Initialize();

    void RegisterHandler(INodeTypeHandlerPtr handler);
    INodeTypeHandlerPtr FindHandler(NObjectClient::EObjectType type);
    INodeTypeHandlerPtr GetHandler(NObjectClient::EObjectType type);
    INodeTypeHandlerPtr GetHandler(const TCypressNodeBase* node);

    typedef NRpc::TTypedServiceRequest<NCypressClient::NProto::TReqCreate> TReqCreate;
    typedef NRpc::TTypedServiceResponse<NCypressClient::NProto::TRspCreate> TRspCreate;

    //! Creates a factory for creating nodes.
    ICypressNodeFactoryPtr CreateNodeFactory(
        NTransactionServer::TTransaction* transaction,
        NSecurityServer::TAccount* account,
        bool preserveAccount);

    //! Creates a new node and registers it.
    TCypressNodeBase* CreateNode(
        INodeTypeHandlerPtr handler,
        ICypressNodeFactoryPtr factory,
        TReqCreate* request,
        TRspCreate* response);

    //! Creates a new node and registers it.
    TCypressNodeBase* CreateNode(const TNodeId& id);

    //! Clones a node and registers its clone.
    TCypressNodeBase* CloneNode(
        TCypressNodeBase* sourceNode,
        ICypressNodeFactoryPtr factory,
        ENodeCloneMode mode);

    //! Returns the root node.
    TCypressNodeBase* GetRootNode() const;

    //! Finds node by id, throws if nothing is found.
    TCypressNodeBase* GetNodeOrThrow(const TVersionedNodeId& id);

    //! Creates a resolver that provides a view in the context of a given transaction.
    NYTree::INodeResolverPtr CreateResolver(NTransactionServer::TTransaction* transaction = nullptr);

    //! Similar to |FindNode| provided by |DECLARE_ENTITY_ACCESSORS| but
    //! specially optimized for the case of null transaction.
    TCypressNodeBase* FindNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    TCypressNodeBase* GetVersionedNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    ICypressNodeProxyPtr GetNodeProxy(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction = nullptr);

    TCypressNodeBase* LockNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request,
        bool recursive = false);

    TLock* CreateLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request,
        bool waitable);

    void SetModified(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    void SetAccessed(TCypressNodeBase* trunkNode);

    typedef SmallVector<TCypressNodeBase*, 1> TSubtreeNodes;
    TSubtreeNodes ListSubtreeNodes(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        bool includeRoot = true);

    std::vector<TLock*> ListSubtreeLocks(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        bool includeRoot = true);

    bool IsOrphaned(TCypressNodeBase* trunkNode);
    bool IsAlive(TCypressNodeBase* trunkNode, NTransactionServer::TTransaction* transaction);

    //! Returns the list consisting of the trunk node
    //! and all of its existing versioned overrides up to #transaction;
    //! #trunkNode is the last element.
    TCypressNodeList GetNodeOriginators(
        NTransactionServer::TTransaction* transaction,
        TCypressNodeBase* trunkNode);

    //! Same as GetNodeOverrides but #trunkNode is the first element.
    TCypressNodeList GetNodeReverseOriginators(
        NTransactionServer::TTransaction* transaction,
        TCypressNodeBase* trunkNode);

    DECLARE_ENTITY_MAP_ACCESSORS(Node, TCypressNodeBase, TVersionedNodeId);
    DECLARE_ENTITY_MAP_ACCESSORS(Lock, TLock, TLockId);

private:
    class TNodeFactory;
    class TNodeTypeHandler;
    class TLockTypeHandler;
    class TYPathResolver;

    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TCypressManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
