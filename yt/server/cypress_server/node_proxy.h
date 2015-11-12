#pragma once

#include "public.h"
#include "node.h"

#include <yt/server/object_server/object_proxy.h>

#include <yt/server/security_server/cluster_resources.h>
#include <yt/server/security_server/public.h>

#include <yt/server/transaction_server/public.h>

#include <yt/ytlib/cypress_client/cypress_ypath.pb.h>

#include <yt/core/rpc/service_detail.h>

#include <yt/core/ytree/node.h>
#include <yt/core/ytree/system_attribute_provider.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

//! Extends NYTree::INodeFactory by adding Cypress-specific functionality.
struct ICypressNodeFactory
    : public NYTree::INodeFactory
{
    typedef NRpc::TTypedServiceRequest<NCypressClient::NProto::TReqCreate> TReqCreate;
    typedef NRpc::TTypedServiceResponse<NCypressClient::NProto::TRspCreate> TRspCreate;

    virtual NTransactionServer::TTransaction* GetTransaction() = 0;

    virtual NSecurityServer::TAccount* GetNewNodeAccount() = 0;
    virtual NSecurityServer::TAccount* GetClonedNodeAccount(
        TCypressNodeBase* sourceNode) = 0;

    virtual ICypressNodeProxyPtr CreateNode(
        NObjectClient::EObjectType type,
        NYTree::IAttributeDictionary* attributes,
        TReqCreate* request,
        TRspCreate* response) = 0;

    virtual TCypressNodeBase* CreateNode(
        const TNodeId& id) = 0;

    virtual TCypressNodeBase* CloneNode(
        TCypressNodeBase* sourceNode,
        ENodeCloneMode mode) = 0;

};

DEFINE_REFCOUNTED_TYPE(ICypressNodeFactory)

////////////////////////////////////////////////////////////////////////////////

//! Extends NYTree::INode by adding functionality that is common to all
//! logical Cypress nodes.
struct ICypressNodeProxy
    : public virtual NYTree::INode
    , public virtual NObjectServer::IObjectProxy
{
    //! Returns the transaction for which the proxy is created.
    virtual NTransactionServer::TTransaction* GetTransaction() const = 0;

    //! Returns the trunk node for which the proxy is created.
    virtual TCypressNodeBase* GetTrunkNode() const = 0;

    //! Returns resources used by the object.
    /*!
     *  This is displayed in @resource_usage attribute and is not used for accounting.
     *
     *  \see #ICypressNode::GetResourceUsage
     */
    virtual NSecurityServer::TClusterResources GetResourceUsage() const = 0;

    //! "Covariant" extension of NYTree::INode::CreateFactory.
    virtual ICypressNodeFactoryPtr CreateCypressFactory(
        NSecurityServer::TAccount* account,
        bool preserveAccount) const = 0;


    static ICypressNodeProxy* FromNode(NYTree::INode* ptr);
    static TIntrusivePtr<ICypressNodeProxy> FromNode(const TIntrusivePtr<NYTree::INode>& ptr);
    static const ICypressNodeProxy* FromNode(const NYTree::INode* ptr);
    static TIntrusivePtr<const ICypressNodeProxy> FromNode(const TIntrusivePtr<const NYTree::INode>& ptr);

};

DEFINE_REFCOUNTED_TYPE(ICypressNodeProxy)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
