#pragma once

#include "node_detail.h"
#include "node_proxy.h"

#include <yt/server/cell_master/config.h>

#include <yt/server/object_server/public.h>

#include <yt/server/security_server/public.h>

#include <yt/server/transaction_server/public.h>

#include <yt/ytlib/cypress_client/cypress_ypath.pb.h>

#include <yt/core/ytree/node.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TNontemplateCypressNodeProxyBase
    : public NYTree::TNodeBase
    , public NObjectServer::TObjectProxyBase
    , public ICypressNodeProxy
{
public:
    TNontemplateCypressNodeProxyBase(
        NCellMaster::TBootstrap* bootstrap,
        NObjectServer::TObjectTypeMetadata* metadata,
        NTransactionServer::TTransaction* transaction,
        TCypressNodeBase* trunkNode);

    virtual std::unique_ptr<NYTree::ITransactionalNodeFactory> CreateFactory() const override;
    virtual std::unique_ptr<ICypressNodeFactory> CreateCypressFactory(
        NSecurityServer::TAccount* account,
        bool preserveAccount) const override;

    virtual NYTree::INodeResolverPtr GetResolver() const override;

    virtual NTransactionServer::TTransaction* GetTransaction() const override;

    virtual TCypressNodeBase* GetTrunkNode() const override;

    virtual NYTree::ICompositeNodePtr GetParent() const override;
    virtual void SetParent(NYTree::ICompositeNodePtr parent) override;

    virtual const NYTree::IAttributeDictionary& Attributes() const override;
    virtual NYTree::IAttributeDictionary* MutableAttributes() override;

protected:
    class TCustomAttributeDictionary;
    class TResourceUsageVisitor;

    const TCypressManagerConfigPtr Config;
    NTransactionServer::TTransaction* const Transaction;
    TCypressNodeBase* const TrunkNode;

    mutable TCypressNodeBase* CachedNode = nullptr;
    mutable NYTree::INodeResolverPtr CachedResolver;

    bool AccessTrackingSuppressed = false;
    bool ModificationTrackingSuppressed = false;


    virtual NObjectServer::TVersionedObjectId GetVersionedId() const override;
    virtual NSecurityServer::TAccessControlDescriptor* FindThisAcd() override;

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override;
    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override;
    virtual TFuture<NYson::TYsonString> GetBuiltinAttributeAsync(const Stroka& key) override;
    TFuture<NYson::TYsonString> GetExternalBuiltinAttributeAsync(const Stroka& key);
    virtual bool SetBuiltinAttribute(const Stroka& key, const NYson::TYsonString& value) override;
    virtual bool RemoveBuiltinAttribute(const Stroka& key) override;

    virtual void BeforeInvoke(NRpc::IServiceContextPtr context) override;
    virtual void AfterInvoke(NRpc::IServiceContextPtr context) override;
    virtual bool DoInvoke(NRpc::IServiceContextPtr context) override;

    virtual void RemoveSelf(
        TReqRemove* request,
        TRspRemove* response,
        TCtxRemovePtr context) override;

    // Suppress access handling in the cases below.
    virtual void GetAttribute(
        const NYTree::TYPath& path,
        TReqGet* request,
        TRspGet* response,
        TCtxGetPtr context) override;
    virtual void ListAttribute(
        const NYTree::TYPath& path,
        TReqList* request,
        TRspList* response,
        TCtxListPtr context) override;
    virtual void ExistsSelf(
        TReqExists* request,
        TRspExists* response,
        TCtxExistsPtr context) override;
    virtual void ExistsRecursive(
        const NYTree::TYPath& path,
        TReqExists* request,
        TRspExists* response,
        TCtxExistsPtr context) override;
    virtual void ExistsAttribute(
        const NYTree::TYPath& path,
        TReqExists* request,
        TRspExists* response,
        TCtxExistsPtr context) override;

    TCypressNodeBase* GetImpl(TCypressNodeBase* trunkNode) const;

    TCypressNodeBase* LockImpl(
        TCypressNodeBase* trunkNode,
        const TLockRequest& request = ELockMode::Exclusive,
        bool recursive = false) const;

    TCypressNodeBase* GetThisImpl();
    const TCypressNodeBase* GetThisImpl() const;

    TCypressNodeBase* LockThisImpl(
        const TLockRequest& request = ELockMode::Exclusive,
        bool recursive = false);


    template <class TImpl>
    TImpl* GetThisTypedImpl()
    {
        return static_cast<TImpl*>(GetThisImpl());
    }

    template <class TImpl>
    const TImpl* GetThisTypedImpl() const
    {
        return static_cast<const TImpl*>(GetThisImpl());
    }

    template <class TImpl>
    TImpl* LockThisTypedImpl(
        const TLockRequest& request = ELockMode::Exclusive,
        bool recursive = false)
    {
        return static_cast<TImpl*>(LockThisImpl(request, recursive));
    }


    ICypressNodeProxyPtr GetProxy(TCypressNodeBase* trunkNode) const;

    virtual std::unique_ptr<NYTree::IAttributeDictionary> DoCreateCustomAttributes() override;
    
    // TSupportsPermissions members
    virtual void ValidatePermission(
        NYTree::EPermissionCheckScope scope,
        NYTree::EPermission permission) override;

    // Cypress-specific overload.
    void ValidatePermission(
        TCypressNodeBase* node,
        NYTree::EPermissionCheckScope scope,
        NYTree::EPermission permission);

    // Inject other overloads into the scope.
    using TObjectProxyBase::ValidatePermission;

    void ValidateNotExternal();

    void SetModified();
    void SuppressModificationTracking();

    void SetAccessed();
    void SuppressAccessTracking();

    virtual bool CanHaveChildren() const;
    
    virtual void SetChildNode(
        NYTree::INodeFactory* factory,
        const NYPath::TYPath& path,
        NYTree::INodePtr child,
        bool recursive);

    NSecurityServer::TClusterResources GetResourceUsage() const;


    DECLARE_YPATH_SERVICE_METHOD(NCypressClient::NProto, Lock);
    DECLARE_YPATH_SERVICE_METHOD(NCypressClient::NProto, Create);
    DECLARE_YPATH_SERVICE_METHOD(NCypressClient::NProto, Copy);

};

////////////////////////////////////////////////////////////////////////////////

class TNontemplateCompositeCypressNodeProxyBase
    : public TNontemplateCypressNodeProxyBase
    , public virtual NYTree::ICompositeNode
{
public:
    virtual TIntrusivePtr<const NYTree::ICompositeNode> AsComposite() const override;
    virtual TIntrusivePtr<NYTree::ICompositeNode> AsComposite() override;

protected:
    TNontemplateCompositeCypressNodeProxyBase(
        NCellMaster::TBootstrap* bootstrap,
        NObjectServer::TObjectTypeMetadata* metadata,
        NTransactionServer::TTransaction* transaction,
        TCypressNodeBase* trunkNode);

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override;
    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override;

    virtual bool CanHaveChildren() const override;

};

////////////////////////////////////////////////////////////////////////////////

template <class TBase, class IBase, class TImpl>
class TCypressNodeProxyBase
    : public TBase
    , public virtual IBase
{
public:
    TCypressNodeProxyBase(
        NCellMaster::TBootstrap* bootstrap,
        NObjectServer::TObjectTypeMetadata* metadata,
        NTransactionServer::TTransaction* transaction,
        TImpl* trunkNode)
        : TBase(
            bootstrap,
            metadata,
            transaction,
            trunkNode)
    { }

protected:
    TImpl* GetThisTypedImpl()
    {
        return TNontemplateCypressNodeProxyBase::GetThisTypedImpl<TImpl>();
    }

    const TImpl* GetThisTypedImpl() const
    {
        return TNontemplateCypressNodeProxyBase::GetThisTypedImpl<TImpl>();
    }

    TImpl* LockThisTypedImpl(
        const TLockRequest& request = ELockMode::Exclusive,
        bool recursive = false)
    {
        return TNontemplateCypressNodeProxyBase::LockThisTypedImpl<TImpl>(request, recursive);
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class TValue, class IBase, class TImpl>
class TScalarNodeProxy
    : public TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, IBase, TImpl>
{
public:
    TScalarNodeProxy(
        NCellMaster::TBootstrap* bootstrap,
        NObjectServer::TObjectTypeMetadata* metadata,
        NTransactionServer::TTransaction* transaction,
        TScalarNode<TValue>* trunkNode)
        : TBase(
            bootstrap,
            metadata,
            transaction,
            trunkNode)
    { }

    virtual NYTree::ENodeType GetType() const override
    {
        return NDetail::TCypressScalarTypeTraits<TValue>::NodeType;
    }

    virtual typename NMpl::TCallTraits<TValue>::TType GetValue() const override
    {
        return this->GetThisTypedImpl()->Value();
    }

    virtual void SetValue(typename NMpl::TCallTraits<TValue>::TType value) override
    {
        this->ValidateValue(value);
        this->LockThisTypedImpl()->Value() = value;
        this->SetModified();
    }

private:
    typedef TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, IBase, TImpl> TBase;

    virtual void ValidateValue(typename NMpl::TCallTraits<TValue>::TType /*value*/)
    { }
};

////////////////////////////////////////////////////////////////////////////////

#define BEGIN_DEFINE_SCALAR_TYPE(key, type) \
    class T##key##NodeProxy \
        : public TScalarNodeProxy<type, NYTree::I##key##Node, T##key##Node> \
    { \
        YTREE_NODE_TYPE_OVERRIDES(key) \
    \
    public: \
        T##key##NodeProxy( \
            NCellMaster::TBootstrap* bootstrap, \
            NObjectServer::TObjectTypeMetadata* metadata, \
            NTransactionServer::TTransaction* transaction, \
            TScalarNode<type>* node) \
            : TScalarNodeProxy<type, NYTree::I##key##Node, T##key##Node>( \
                bootstrap, \
                metadata, \
                transaction, \
                node) \
        { }

#define END_DEFINE_SCALAR_TYPE(key, type) \
    }; \
    \
    template <> \
    inline ICypressNodeProxyPtr TScalarNodeTypeHandler<type>::DoGetProxy( \
        TScalarNode<type>* node, \
        NTransactionServer::TTransaction* transaction) \
    { \
        return New<T##key##NodeProxy>( \
            Bootstrap_, \
            &Metadata_, \
            transaction, \
            node); \
    }

BEGIN_DEFINE_SCALAR_TYPE(String, Stroka)
    protected:
        virtual void ValidateValue(const Stroka& value) override
        {
            auto length = value.length();
            auto limit = Bootstrap_->GetConfig()->CypressManager->MaxStringNodeLength;
            if (length > limit) {
                THROW_ERROR_EXCEPTION(
                    NYTree::EErrorCode::MaxStringLengthViolation,
                    "String node length limit exceeded: %v > %v",
                    length,
                    limit);
            }
        }
END_DEFINE_SCALAR_TYPE(String, Stroka)

BEGIN_DEFINE_SCALAR_TYPE(Int64, i64)
END_DEFINE_SCALAR_TYPE(Int64, i64)

BEGIN_DEFINE_SCALAR_TYPE(Uint64, ui64)
END_DEFINE_SCALAR_TYPE(Uint64, ui64)

BEGIN_DEFINE_SCALAR_TYPE(Double, double)
END_DEFINE_SCALAR_TYPE(Double, double)

BEGIN_DEFINE_SCALAR_TYPE(Boolean, bool)
END_DEFINE_SCALAR_TYPE(Boolean, bool)

#undef BEGIN_DEFINE_SCALAR_TYPE
#undef END_DEFINE_SCALAR_TYPE

////////////////////////////////////////////////////////////////////////////////

class TMapNodeProxy
    : public TCypressNodeProxyBase<TNontemplateCompositeCypressNodeProxyBase, NYTree::IMapNode, TMapNode>
    , public NYTree::TMapNodeMixin
{
    YTREE_NODE_TYPE_OVERRIDES(Map)

public:
    TMapNodeProxy(
        NCellMaster::TBootstrap* bootstrap,
        NObjectServer::TObjectTypeMetadata* metadata,
        NTransactionServer::TTransaction* transaction,
        TMapNode* trunkNode);

    virtual void Clear() override;
    virtual int GetChildCount() const override;
    virtual std::vector< std::pair<Stroka, NYTree::INodePtr> > GetChildren() const override;
    virtual std::vector<Stroka> GetKeys() const override;
    virtual NYTree::INodePtr FindChild(const Stroka& key) const override;
    virtual bool AddChild(NYTree::INodePtr child, const Stroka& key) override;
    virtual bool RemoveChild(const Stroka& key) override;
    virtual void ReplaceChild(NYTree::INodePtr oldChild, NYTree::INodePtr newChild) override;
    virtual void RemoveChild(NYTree::INodePtr child) override;
    virtual Stroka GetChildKey(NYTree::IConstNodePtr child) override;

private:
    typedef TCypressNodeProxyBase<TNontemplateCompositeCypressNodeProxyBase, NYTree::IMapNode, TMapNode> TBase;

    virtual bool DoInvoke(NRpc::IServiceContextPtr context) override;
    
    virtual void SetChildNode(
        NYTree::INodeFactory* factory,
        const NYPath::TYPath& path,
        NYTree::INodePtr child,
        bool recursive) override;

    virtual int GetMaxChildCount() const override;
    virtual int GetMaxKeyLength() const override;

    virtual TResolveResult ResolveRecursive(const NYPath::TYPath& path, NRpc::IServiceContextPtr context) override;

    void DoRemoveChild(TMapNode* impl, const Stroka& key, TCypressNodeBase* childImpl);

};

////////////////////////////////////////////////////////////////////////////////

class TListNodeProxy
    : public TCypressNodeProxyBase<TNontemplateCompositeCypressNodeProxyBase, NYTree::IListNode, TListNode>
    , public NYTree::TListNodeMixin
{
    YTREE_NODE_TYPE_OVERRIDES(List)

public:
    TListNodeProxy(
        NCellMaster::TBootstrap* bootstrap,
        NObjectServer::TObjectTypeMetadata* metadata,
        NTransactionServer::TTransaction* transaction,
        TListNode* trunkNode);

    virtual void Clear() override;
    virtual int GetChildCount() const override;
    virtual std::vector<NYTree::INodePtr> GetChildren() const override;
    virtual NYTree::INodePtr FindChild(int index) const override;
    virtual void AddChild(NYTree::INodePtr child, int beforeIndex = -1) override;
    virtual bool RemoveChild(int index) override;
    virtual void ReplaceChild(NYTree::INodePtr oldChild, NYTree::INodePtr newChild) override;
    virtual void RemoveChild(NYTree::INodePtr child) override;
    virtual int GetChildIndex(NYTree::IConstNodePtr child) override;

private:
    typedef TCypressNodeProxyBase<TNontemplateCompositeCypressNodeProxyBase, NYTree::IListNode, TListNode> TBase;

    virtual void SetChildNode(
        NYTree::INodeFactory* factory,
        const NYPath::TYPath& path,
        NYTree::INodePtr child,
        bool recursive) override;

    virtual int GetMaxChildCount() const override;

    virtual TResolveResult ResolveRecursive(
        const NYPath::TYPath& path,
        NRpc::IServiceContextPtr context) override;

};

////////////////////////////////////////////////////////////////////////////////

class TLinkNodeProxy
    : public TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, NYTree::IEntityNode, TLinkNode>
{
    YTREE_NODE_TYPE_OVERRIDES(Entity)

public:
    TLinkNodeProxy(
        NCellMaster::TBootstrap* bootstrap,
        NObjectServer::TObjectTypeMetadata* metadata,
        NTransactionServer::TTransaction* transaction,
        TLinkNode* trunkNode);

    virtual TResolveResult Resolve(
        const NYPath::TYPath& path,
        NRpc::IServiceContextPtr context) override;

private:
    typedef TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, NYTree::IEntityNode, TLinkNode> TBase;

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override;
    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override;
    virtual bool SetBuiltinAttribute(const Stroka& key, const NYson::TYsonString& value) override;

    NObjectServer::IObjectProxyPtr FindTargetProxy() const;
    NObjectServer::IObjectProxyPtr GetTargetProxy() const;

    bool IsBroken(const NObjectServer::TObjectId& id) const;

};

////////////////////////////////////////////////////////////////////////////////

class TDocumentNodeProxy
    : public TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, NYTree::IEntityNode, TDocumentNode>
{
public:
    TDocumentNodeProxy(
        NCellMaster::TBootstrap* bootstrap,
        NObjectServer::TObjectTypeMetadata* metadata,
        NTransactionServer::TTransaction* transaction,
        TDocumentNode* trunkNode);

    virtual NYTree::ENodeType GetType() const override;

    virtual TIntrusivePtr<const NYTree::IEntityNode> AsEntity() const override;
    virtual TIntrusivePtr<NYTree::IEntityNode> AsEntity() override;

private:
    typedef TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, NYTree::IEntityNode, TDocumentNode> TBase;

    virtual TResolveResult ResolveRecursive(const NYPath::TYPath& path, NRpc::IServiceContextPtr context) override;

    virtual void GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context) override;
    virtual void GetRecursive(const NYPath::TYPath& path, TReqGet* request, TRspGet* response, TCtxGetPtr context) override;

    virtual void SetSelf(TReqSet* request, TRspSet* response, TCtxSetPtr context) override;
    virtual void SetRecursive(const NYPath::TYPath& path, TReqSet* request, TRspSet* response, TCtxSetPtr context) override;

    virtual void ListSelf(TReqList* request, TRspList* response, TCtxListPtr context) override;
    virtual void ListRecursive(const NYPath::TYPath& path, TReqList* request, TRspList* response, TCtxListPtr context) override;

    virtual void RemoveRecursive(const NYPath::TYPath& path, TReqRemove* request, TRspRemove* response, TCtxRemovePtr context) override;

    virtual void ExistsRecursive(const NYPath::TYPath& path, TReqExists* request, TRspExists* response, TCtxExistsPtr context) override;

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override;
    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override;
    virtual bool SetBuiltinAttribute(const Stroka& key, const NYson::TYsonString& value) override;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
