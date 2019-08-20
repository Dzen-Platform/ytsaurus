#pragma once

#include "public.h"

#include <yt/server/master/transaction_server/public.h>

#include <yt/server/master/security_server/cluster_resources.h>

#include <yt/server/master/object_server/public.h>

#include <yt/ytlib/cypress_client/cypress_ypath.pb.h>

#include <yt/core/rpc/service_detail.h>

namespace NYT::NCypressServer {

////////////////////////////////////////////////////////////////////////////////

struct TCreateNodeContext
{
    NObjectClient::TCellTag ExternalCellTag = NObjectClient::InvalidCellTag;
    NTransactionServer::TTransaction* Transaction = nullptr;
    NYTree::IAttributeDictionary* InheritedAttributes = nullptr;
    NYTree::IAttributeDictionary* ExplicitAttributes = nullptr;
    NSecurityServer::TAccount* Account = nullptr;
    TCypressShard* Shard = nullptr;
};

//! Provides node type-specific services.
struct INodeTypeHandler
    : public virtual TRefCounted
{
    //! Returns the type-specific flags; see IObjectTypeHandler::GetFlags.
    virtual NObjectServer::ETypeFlags GetFlags() const =  0;

    //! Constructs a proxy.
    virtual ICypressNodeProxyPtr GetProxy(
        TCypressNode* trunkNode,
        NTransactionServer::TTransaction* transaction) = 0;

    //! Returns the (dynamic) node type.
    virtual NObjectClient::EObjectType GetObjectType() const = 0;

    //! Returns the (static) node type.
    virtual NYTree::ENodeType GetNodeType() const = 0;

    //! Create an empty instance of a node.
    //! Called during snapshot deserialization and node cloning.
    virtual std::unique_ptr<TCypressNode> Instantiate(
        const TVersionedNodeId& id,
        NObjectClient::TCellTag externalCellTag) = 0;

    //! Creates a new trunk node.
    /*!
     *  This is called during |Create| verb.
     *  The node is not yet linked into Cypress.
     */
    virtual std::unique_ptr<TCypressNode> Create(
        TNodeId hintId,
        const TCreateNodeContext& context) = 0;

    //! Fills attributes of a trunk node. Usually applied to newly created nodes.
    virtual void FillAttributes(
        TCypressNode* trunkNode,
        NYTree::IAttributeDictionary* inheritedAttributes,
        NYTree::IAttributeDictionary* explicitAttributes) = 0;

    //! Performs cleanup on node destruction.
    /*!
     *  This is called prior to the actual removal of the node from the meta-map.
     *  A typical implementation will release the resources held by the node,
     *  decrement the ref-counters of its children etc.
     */
    virtual void Destroy(TCypressNode* node) = 0;

    //! Branches a node into a given transaction.
    virtual std::unique_ptr<TCypressNode> Branch(
        TCypressNode* originatingNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& lockRquest) = 0;

    //! Called on transaction commit to merge the changes made in the branched node back into the originating one.
    /*!
     *  \note
     *  #branchedNode is non-const for performance reasons (i.e. to swap the data instead of copying).
     */
    virtual void Merge(
        TCypressNode* originatingNode,
        TCypressNode* branchedNode) = 0;

    //! Called on transaction abort to perform any cleanup necessary.
    /*!
     *  \note
     *  #Destroy is also called for #branchedNode.
     */
    virtual void Unbranch(
        TCypressNode* originatingNode,
        TCypressNode* branchedNode) = 0;

    //! Returns #true iff the branched node differs from (contains changes to) the originating node.
    /*!
     *  \note
     *  Called prior to unlocking a node (by an explicit request) to make sure no changes will be lost.
     */
    virtual bool HasBranchedChanges(
        TCypressNode* originatingNode,
        TCypressNode* branchedNode) = 0;

    //! Constructs a deep copy of the node.
    virtual TCypressNode* Clone(
        TCypressNode* sourceNode,
        ICypressNodeFactory* factory,
        TNodeId hintId,
        ENodeCloneMode mode,
        NSecurityServer::TAccount* account) = 0;
};

DEFINE_REFCOUNTED_TYPE(INodeTypeHandler)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
