#pragma once

#include "public.h"
#include "lock.h"
#include "node.h"
#include "node_proxy.h"
#include "type_handler.h"

#include <yt/server/cell_master/automaton.h>

#include <yt/server/cypress_server/cypress_manager.pb.h>

#include <yt/server/hydra/composite_automaton.h>
#include <yt/server/hydra/entity_map.h>
#include <yt/server/hydra/mutation.h>

#include <yt/server/object_server/object_manager.h>

#include <yt/server/security_server/public.h>

#include <yt/server/transaction_server/transaction.h>
#include <yt/server/transaction_server/transaction_manager.h>

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/id_generator.h>
#include <yt/core/misc/small_vector.h>

#include <yt/core/ytree/tree_builder.h>
#include <yt/core/ytree/ypath_service.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

struct TNodeFactoryOptions
{
    bool PreserveAccount = false;
    bool PreserveExpirationTime = false;
};

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
    const INodeTypeHandlerPtr& FindHandler(NObjectClient::EObjectType type);
    const INodeTypeHandlerPtr& GetHandler(NObjectClient::EObjectType type);
    const INodeTypeHandlerPtr& GetHandler(const TCypressNodeBase* node);

    typedef NRpc::TTypedServiceRequest<NCypressClient::NProto::TReqCreate> TReqCreate;
    typedef NRpc::TTypedServiceResponse<NCypressClient::NProto::TRspCreate> TRspCreate;

    //! Creates a factory for creating nodes.
    std::unique_ptr<ICypressNodeFactory> CreateNodeFactory(
        NTransactionServer::TTransaction* transaction,
        NSecurityServer::TAccount* account,
        const TNodeFactoryOptions& options);

    //! Creates a new node and registers it.
    TCypressNodeBase* CreateNode(
        const TNodeId& hintId,
        NObjectClient::TCellTag externalCellTag,
        INodeTypeHandlerPtr handler,
        NSecurityServer::TAccount* account,
        bool enableAccounting,
        NTransactionServer::TTransaction* transaction,
        NYTree::IAttributeDictionary* attributes);

    //! Creates a new node and registers it.
    TCypressNodeBase* InstantiateNode(
        const TNodeId& id,
        NObjectClient::TCellTag externalCellTag);

    //! Clones a node and registers its clone.
    TCypressNodeBase* CloneNode(
        TCypressNodeBase* sourceNode,
        ICypressNodeFactory* factory,
        ENodeCloneMode mode);

    //! Returns the root node.
    TMapNode* GetRootNode() const;

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

    void SetExpirationTime(TCypressNodeBase* trunkNode, TNullable<TInstant> time);

    typedef SmallVector<TCypressNodeBase*, 1> TSubtreeNodes;
    TSubtreeNodes ListSubtreeNodes(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        bool includeRoot = true);

    void AbortSubtreeTransactions(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);
    void AbortSubtreeTransactions(NYTree::INodePtr node);

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


    DECLARE_ENTITY_MAP_ACCESSORS(Node, TCypressNodeBase);
    DECLARE_ENTITY_MAP_ACCESSORS(Lock, TLock);

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
