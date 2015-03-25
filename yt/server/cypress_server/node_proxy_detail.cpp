#include "stdafx.h"
#include "node_proxy_detail.h"
#include "cypress_traversing.h"
#include "helpers.h"
#include "private.h"

#include <core/misc/string.h>

#include <ytlib/object_client/helpers.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>
#include <ytlib/cypress_client/rpc_helpers.h>

#include <core/ytree/ypath_detail.h>
#include <core/ytree/node_detail.h>
#include <core/ytree/convert.h>
#include <core/ytree/ephemeral_node_factory.h>
#include <core/ytree/fluent.h>
#include <core/ytree/ypath_client.h>
#include <core/ytree/exception_helpers.h>

#include <core/ypath/tokenizer.h>

#include <server/security_server/account.h>
#include <server/security_server/security_manager.h>
#include <server/security_server/user.h>

namespace NYT {
namespace NCypressServer {

using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NRpc;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NCellMaster;
using namespace NTransactionServer;
using namespace NSecurityServer;
using namespace NCypressClient;

////////////////////////////////////////////////////////////////////////////////

class TNontemplateCypressNodeProxyBase::TCustomAttributeDictionary
    : public IAttributeDictionary
{
public:
    explicit TCustomAttributeDictionary(TNontemplateCypressNodeProxyBase* proxy)
        : Proxy(proxy)
    { }

    virtual std::vector<Stroka> List() const override
    {
        auto keys = ListNodeAttributes(
            Proxy->Bootstrap,
            Proxy->TrunkNode,
            Proxy->Transaction);
        return std::vector<Stroka>(keys.begin(), keys.end());
    }

    virtual TNullable<TYsonString> FindYson(const Stroka& name) const override
    {
        auto objectManager = Proxy->Bootstrap->GetObjectManager();
        auto transactionManager = Proxy->Bootstrap->GetTransactionManager();

        auto transactions = transactionManager->GetTransactionPath(Proxy->Transaction);

        for (const auto* transaction : transactions) {
            TVersionedObjectId versionedId(Proxy->TrunkNode->GetId(), GetObjectId(transaction));
            const auto* userAttributes = objectManager->FindAttributes(versionedId);
            if (userAttributes) {
                auto it = userAttributes->Attributes().find(name);
                if (it != userAttributes->Attributes().end()) {
                    return it->second;
                }
            }
        }

        return Null;
    }

    virtual void SetYson(const Stroka& key, const TYsonString& value) override
    {
        auto objectManager = Proxy->Bootstrap->GetObjectManager();
        auto cypressManager = Proxy->Bootstrap->GetCypressManager();

        auto oldValue = FindYson(key);
        Proxy->GuardedValidateCustomAttributeUpdate(key, oldValue, value);

        auto* node = cypressManager->LockNode(
            Proxy->TrunkNode,
            Proxy->Transaction,
            TLockRequest::SharedAttribute(key));
        auto versionedId = node->GetVersionedId();

        auto* userAttributes = objectManager->GetOrCreateAttributes(versionedId);
        userAttributes->Attributes()[key] = value;

        cypressManager->SetModified(Proxy->TrunkNode, Proxy->Transaction);
    }

    virtual bool Remove(const Stroka& key) override
    {
        auto cypressManager = Proxy->Bootstrap->GetCypressManager();
        auto objectManager = Proxy->Bootstrap->GetObjectManager();
        auto transactionManager = Proxy->Bootstrap->GetTransactionManager();

        auto oldValue = FindYson(key);
        Proxy->GuardedValidateCustomAttributeUpdate(key, oldValue, Null);

        auto transactions = transactionManager->GetTransactionPath(Proxy->Transaction);
        std::reverse(transactions.begin(), transactions.end());

        const TTransaction* containingTransaction = nullptr;
        bool contains = false;
        for (const auto* transaction : transactions) {
            TVersionedObjectId versionedId(Proxy->TrunkNode->GetId(), GetObjectId(transaction));
            const auto* userAttributes = objectManager->FindAttributes(versionedId);
            if (userAttributes) {
                auto it = userAttributes->Attributes().find(key);
                if (it != userAttributes->Attributes().end()) {
                    contains = it->second.HasValue();
                    if (contains) {
                        containingTransaction = transaction;
                    }
                    break;
                }
            }
        }

        if (!contains) {
            return false;
        }

        auto* node = cypressManager->LockNode(
            Proxy->TrunkNode,
            Proxy->Transaction,
            TLockRequest::SharedAttribute(key));
        auto versionedId = node->GetVersionedId();

        if (containingTransaction == Proxy->Transaction) {
            auto* userAttributes = objectManager->GetAttributes(versionedId);
            YCHECK(userAttributes->Attributes().erase(key) == 1);
        } else {
            YCHECK(!containingTransaction);
            auto* userAttributes = objectManager->GetOrCreateAttributes(versionedId);
            userAttributes->Attributes()[key] = Null;
        }

        cypressManager->SetModified(Proxy->TrunkNode, Proxy->Transaction);
        return true;
    }

protected:
    TNontemplateCypressNodeProxyBase* Proxy;

};

////////////////////////////////////////////////////////////////////////////////

class TNontemplateCypressNodeProxyBase::TResourceUsageVisitor
    : public ICypressNodeVisitor
{
public:
    explicit TResourceUsageVisitor(NCellMaster::TBootstrap* bootstrap, IYsonConsumer* consumer)
        : Bootstrap(bootstrap)
        , Consumer(consumer)
        , Result(NewPromise<void>())
    { }

    TFuture<void> Run(ICypressNodeProxyPtr rootNode)
    {
        TraverseCypress(Bootstrap, rootNode, this);
        return Result;
    }

private:
    NCellMaster::TBootstrap* Bootstrap;
    IYsonConsumer* Consumer;

    TPromise<void> Result;
    TClusterResources ResourceUsage;

    virtual void OnNode(ICypressNodeProxyPtr node) override
    {
        ResourceUsage += node->GetResourceUsage();
    }

    virtual void OnError(const TError& error) override
    {
        auto wrappedError = TError("Error computing recursive resource usage")
            << error;
        Result.Set(wrappedError);
    }

    virtual void OnCompleted() override
    {
        Consume(ResourceUsage, Consumer);
        Result.Set(TError());
    }

};

////////////////////////////////////////////////////////////////////////////////

TNontemplateCypressNodeProxyBase::TNontemplateCypressNodeProxyBase(
    INodeTypeHandlerPtr typeHandler,
    NCellMaster::TBootstrap* bootstrap,
    TTransaction* transaction,
    TCypressNodeBase* trunkNode)
    : TObjectProxyBase(bootstrap, trunkNode)
    , TypeHandler(typeHandler)
    , Bootstrap(bootstrap)
    , Transaction(transaction)
    , TrunkNode(trunkNode)
{
    YASSERT(typeHandler);
    YASSERT(bootstrap);
    YASSERT(trunkNode);
    YASSERT(trunkNode->IsTrunk());
}

INodeFactoryPtr TNontemplateCypressNodeProxyBase::CreateFactory() const
{
    return CreateCypressFactory(false);
}

ICypressNodeFactoryPtr TNontemplateCypressNodeProxyBase::CreateCypressFactory(
    bool preserveAccount) const
{
    const auto* impl = GetThisImpl();
    auto* account = impl->GetAccount();
    auto cypressManager = Bootstrap->GetCypressManager();
    return cypressManager->CreateNodeFactory(
        Transaction,
        account,
        preserveAccount);
}

INodeResolverPtr TNontemplateCypressNodeProxyBase::GetResolver() const
{
    if (!CachedResolver) {
        auto cypressManager = Bootstrap->GetCypressManager();
        CachedResolver = cypressManager->CreateResolver(Transaction);
    }
    return CachedResolver;
}

TTransaction* TNontemplateCypressNodeProxyBase::GetTransaction() const
{
    return Transaction;
}

TCypressNodeBase* TNontemplateCypressNodeProxyBase::GetTrunkNode() const
{
    return TrunkNode;
}

ENodeType TNontemplateCypressNodeProxyBase::GetType() const
{
    return TypeHandler->GetNodeType();
}

ICompositeNodePtr TNontemplateCypressNodeProxyBase::GetParent() const
{
    auto* parent = GetThisImpl()->GetParent();
    return parent ? GetProxy(parent)->AsComposite() : nullptr;
}

void TNontemplateCypressNodeProxyBase::SetParent(ICompositeNodePtr parent)
{
    auto* impl = LockThisImpl();
    impl->SetParent(parent ? ToProxy(INodePtr(parent))->GetTrunkNode() : nullptr);
}

const IAttributeDictionary& TNontemplateCypressNodeProxyBase::Attributes() const
{
    return TObjectProxyBase::Attributes();
}

IAttributeDictionary* TNontemplateCypressNodeProxyBase::MutableAttributes()
{
    return TObjectProxyBase::MutableAttributes();
}

TFuture<void> TNontemplateCypressNodeProxyBase::GetBuiltinAttributeAsync(
    const Stroka& key,
    IYsonConsumer* consumer)
{
    if (key == "recursive_resource_usage") {
        auto visitor = New<TResourceUsageVisitor>(Bootstrap, consumer);
        return visitor->Run(const_cast<TNontemplateCypressNodeProxyBase*>(this));
    }

    return TObjectProxyBase::GetBuiltinAttributeAsync(key, consumer);
}

bool TNontemplateCypressNodeProxyBase::SetBuiltinAttribute(const Stroka& key, const TYsonString& value)
{
    if (key == "account") {
        ValidateNoTransaction();
        ValidatePermission(EPermissionCheckScope::This, EPermission::Administer);
        
        auto securityManager = Bootstrap->GetSecurityManager();

        auto name = ConvertTo<Stroka>(value);
        auto* account = securityManager->GetAccountByNameOrThrow(name);

        ValidatePermission(account, EPermission::Use);

        auto* node = LockThisImpl();
        account->ValidateResourceUsageIncrease(node->GetResourceUsage());
        securityManager->SetAccount(node, account);

        return true;
    }

    return TObjectProxyBase::SetBuiltinAttribute(key, value);
}

TVersionedObjectId TNontemplateCypressNodeProxyBase::GetVersionedId() const
{
    return TVersionedObjectId(Object_->GetId(), GetObjectId(Transaction));
}

void TNontemplateCypressNodeProxyBase::ListSystemAttributes(std::vector<TAttributeInfo>* attributes)
{
    const auto* node = GetThisImpl();
    bool hasKey = NodeHasKey(Bootstrap, node);
    attributes->push_back(TAttributeInfo("parent_id", node->GetParent()));
    attributes->push_back("locks");
    attributes->push_back("lock_mode");
    attributes->push_back(TAttributeInfo("path", true, true));
    attributes->push_back(TAttributeInfo("key", hasKey, false));
    attributes->push_back("creation_time");
    attributes->push_back("modification_time");
    attributes->push_back("access_time");
    attributes->push_back("access_counter");
    attributes->push_back("revision");
    attributes->push_back("resource_usage");
    attributes->push_back(TAttributeInfo("recursive_resource_usage", true, true));
    attributes->push_back("account");
    attributes->push_back("user_attribute_keys");
    TObjectProxyBase::ListSystemAttributes(attributes);
}

bool TNontemplateCypressNodeProxyBase::GetBuiltinAttribute(
    const Stroka& key,
    IYsonConsumer* consumer)
{
    const auto* node = GetThisImpl();
    const auto* trunkNode = node->GetTrunkNode();
    bool hasKey = NodeHasKey(Bootstrap, node);

    if (key == "parent_id" && node->GetParent()) {
        BuildYsonFluently(consumer)
            .Value(node->GetParent()->GetId());
        return true;
    }

    if (key == "locks") {
        auto printLock = [=] (TFluentList fluent, const TLock* lock) {
            fluent.Item()
                .BeginMap()
                    .Item("id").Value(lock->GetId())
                    .Item("state").Value(lock->GetState())
                    .Item("transaction_id").Value(lock->GetTransaction()->GetId())
                    .Item("mode").Value(lock->Request().Mode)
                    .DoIf(lock->Request().ChildKey.HasValue(), [=] (TFluentMap fluent) {
                        fluent
                            .Item("child_key").Value(*lock->Request().ChildKey);
                    })
                    .DoIf(lock->Request().AttributeKey.HasValue(), [=] (TFluentMap fluent) {
                        fluent
                            .Item("attribute_key").Value(*lock->Request().AttributeKey);
                    })
                .EndMap();
        };

        BuildYsonFluently(consumer)
            .BeginList()
                .DoFor(trunkNode->AcquiredLocks(), printLock)
                .DoFor(trunkNode->PendingLocks(), printLock)
            .EndList();
        return true;
    }

    if (key == "lock_mode") {
        BuildYsonFluently(consumer)
            .Value(FormatEnum(node->GetLockMode()));
        return true;
    }

    if (key == "path") {
        BuildYsonFluently(consumer)
            .Value(GetPath());
        return true;
    }

    if (hasKey && key == "key") {
        BuildYsonFluently(consumer)
            .Value(GetParent()->AsMap()->GetChildKey(this));
        return true;
    }

    if (key == "creation_time") {
        BuildYsonFluently(consumer)
            .Value(node->GetCreationTime());
        return true;
    }

    if (key == "modification_time") {
        BuildYsonFluently(consumer)
            .Value(node->GetModificationTime());
        return true;
    }

    if (key == "access_time") {
        BuildYsonFluently(consumer)
            .Value(trunkNode->GetAccessTime());
        return true;
    }
 
    if (key == "access_counter") {
        BuildYsonFluently(consumer)
            .Value(trunkNode->GetAccessCounter());
        return true;
    }

    if (key == "revision") {
        BuildYsonFluently(consumer)
            .Value(node->GetRevision());
        return true;
    }

    if (key == "resource_usage") {
        BuildYsonFluently(consumer)
            .Value(GetResourceUsage());
        return true;
    }

    if (key == "account") {
        BuildYsonFluently(consumer)
            .Value(node->GetAccount()->GetName());
        return true;
    }

    if (key == "user_attribute_keys") {
        std::vector<TAttributeInfo> systemAttributes;
        ListSystemAttributes(&systemAttributes);

        auto customAttributes = GetCustomAttributes()->List();
        yhash_set<Stroka> customAttributesSet(customAttributes.begin(), customAttributes.end());

        for (const auto& attribute : systemAttributes) {
            if (attribute.IsCustom) {
                customAttributesSet.erase(attribute.Key);
            }
        }

        BuildYsonFluently(consumer)
            .Value(customAttributesSet);
        return true;
    }

    return TObjectProxyBase::GetBuiltinAttribute(key, consumer);
}

void TNontemplateCypressNodeProxyBase::BeforeInvoke(IServiceContextPtr context)
{
    AccessTrackingSuppressed = GetSuppressAccessTracking(context->RequestHeader());
    ModificationTrackingSuppressed = GetSuppressModificationTracking(context->RequestHeader());

    TObjectProxyBase::BeforeInvoke(std::move(context));
}

void TNontemplateCypressNodeProxyBase::AfterInvoke(IServiceContextPtr context)
{
    if (!AccessTrackingSuppressed) {
        SetAccessed();
    }

    TObjectProxyBase::AfterInvoke(std::move(context));
}

bool TNontemplateCypressNodeProxyBase::DoInvoke(NRpc::IServiceContextPtr context)
{
    DISPATCH_YPATH_SERVICE_METHOD(Lock);
    DISPATCH_YPATH_SERVICE_METHOD(Create);
    DISPATCH_YPATH_SERVICE_METHOD(Copy);

    if (TNodeBase::DoInvoke(context)) {
        return true;
    }

    if (TObjectProxyBase::DoInvoke(context)) {
        return true;
    }

    return false;
}

void TNontemplateCypressNodeProxyBase::GetAttribute(
    const TYPath& path,
    TReqGet* request,
    TRspGet* response,
    TCtxGetPtr context)
{
    SuppressAccessTracking();
    TObjectProxyBase::GetAttribute(path, request, response, context);
}

void TNontemplateCypressNodeProxyBase::ListAttribute(
    const TYPath& path,
    TReqList* request,
    TRspList* response,
    TCtxListPtr context)
{
    SuppressAccessTracking();
    TObjectProxyBase::ListAttribute(path, request, response, context);
}

void TNontemplateCypressNodeProxyBase::ExistsSelf(
    TReqExists* request,
    TRspExists* response,
    TCtxExistsPtr context)
{
    SuppressAccessTracking();
    TObjectProxyBase::ExistsSelf(request, response, context);
}

void TNontemplateCypressNodeProxyBase::ExistsRecursive(
    const TYPath& path,
    TReqExists* request,
    TRspExists* response,
    TCtxExistsPtr context)
{
    SuppressAccessTracking();
    TObjectProxyBase::ExistsRecursive(path, request, response, context);
}

void TNontemplateCypressNodeProxyBase::ExistsAttribute(
    const TYPath& path,
    TReqExists* request,
    TRspExists* response,
    TCtxExistsPtr context)
{
    SuppressAccessTracking();
    TObjectProxyBase::ExistsAttribute(path, request, response, context);
}

TCypressNodeBase* TNontemplateCypressNodeProxyBase::GetImpl(TCypressNodeBase* trunkNode) const
{
    auto cypressManager = Bootstrap->GetCypressManager();
    return cypressManager->GetVersionedNode(trunkNode, Transaction);
}

TCypressNodeBase* TNontemplateCypressNodeProxyBase::LockImpl(
    TCypressNodeBase* trunkNode,
    const TLockRequest& request /*= ELockMode::Exclusive*/,
    bool recursive /*= false*/) const
{
    auto cypressManager = Bootstrap->GetCypressManager();
    return cypressManager->LockNode(trunkNode, Transaction, request, recursive);
}

TCypressNodeBase* TNontemplateCypressNodeProxyBase::GetThisImpl()
{
    if (CachedNode) {
        return CachedNode;
    }
    auto* node = GetImpl(TrunkNode);
    if (node->GetTransaction() == Transaction) {
        CachedNode = node;
    }
    return node;
}

const TCypressNodeBase* TNontemplateCypressNodeProxyBase::GetThisImpl() const
{
    return const_cast<TNontemplateCypressNodeProxyBase*>(this)->GetThisImpl();
}

TCypressNodeBase* TNontemplateCypressNodeProxyBase::LockThisImpl(
    const TLockRequest& request /*= ELockMode::Exclusive*/,
    bool recursive /*= false*/)
{
    // NB: Cannot use |CachedNode| here.
    CachedNode = nullptr;
    return LockImpl(TrunkNode, request, recursive);
}

ICypressNodeProxyPtr TNontemplateCypressNodeProxyBase::GetProxy(TCypressNodeBase* trunkNode) const
{
    auto cypressManager = Bootstrap->GetCypressManager();
    return cypressManager->GetNodeProxy(trunkNode, Transaction);
}

ICypressNodeProxy* TNontemplateCypressNodeProxyBase::ToProxy(INodePtr node)
{
    return dynamic_cast<ICypressNodeProxy*>(node.Get());
}

const ICypressNodeProxy* TNontemplateCypressNodeProxyBase::ToProxy(IConstNodePtr node)
{
    return dynamic_cast<const ICypressNodeProxy*>(node.Get());
}

std::unique_ptr<IAttributeDictionary> TNontemplateCypressNodeProxyBase::DoCreateCustomAttributes()
{
    return std::unique_ptr<IAttributeDictionary>(new TCustomAttributeDictionary(this));
}

void TNontemplateCypressNodeProxyBase::ValidatePermission(
    EPermissionCheckScope scope,
    EPermission permission)
{
    auto* node = GetThisImpl();
    ValidatePermission(node, scope, permission);
}

void TNontemplateCypressNodeProxyBase::ValidatePermission(
    TCypressNodeBase* node,
    EPermissionCheckScope scope,
    EPermission permission)
{
    if ((scope & EPermissionCheckScope::This) != EPermissionCheckScope::None) {
        ValidatePermission(node, permission);
    }

    if ((scope & EPermissionCheckScope::Parent) != EPermissionCheckScope::None) {
        ValidatePermission(node->GetParent(), permission);
    }

    if ((scope & EPermissionCheckScope::Descendants) != EPermissionCheckScope::None) {
        auto cypressManager = Bootstrap->GetCypressManager();
        auto* trunkNode = node->GetTrunkNode();
        auto descendants = cypressManager->ListSubtreeNodes(trunkNode, Transaction, false);
        for (auto* descendant : descendants) {
            ValidatePermission(descendant, permission);
        }
    }
}

void TNontemplateCypressNodeProxyBase::SetModified()
{
    if (TrunkNode->IsAlive() && !ModificationTrackingSuppressed) {
        auto cypressManager = Bootstrap->GetCypressManager();
        cypressManager->SetModified(TrunkNode, Transaction);
    }
}

void TNontemplateCypressNodeProxyBase::SuppressModificationTracking()
{
    ModificationTrackingSuppressed = true;
}

void TNontemplateCypressNodeProxyBase::SetAccessed()
{
    if (TrunkNode->IsAlive()) {
        auto cypressManager = Bootstrap->GetCypressManager();
        cypressManager->SetAccessed(TrunkNode);
    }
}

void TNontemplateCypressNodeProxyBase::SuppressAccessTracking()
{
    AccessTrackingSuppressed = true;
}

bool TNontemplateCypressNodeProxyBase::CanHaveChildren() const
{
    return false;
}

void TNontemplateCypressNodeProxyBase::SetChild(
    INodeFactoryPtr /*factory*/,
    const TYPath& /*path*/,
    INodePtr /*value*/,
    bool /*recursive*/)
{
    YUNREACHABLE();
}

TClusterResources TNontemplateCypressNodeProxyBase::GetResourceUsage() const
{
    return TClusterResources(0, 1, 0);
}

NLogging::TLogger TNontemplateCypressNodeProxyBase::CreateLogger() const
{
    return CypressServerLogger;
}

DEFINE_YPATH_SERVICE_METHOD(TNontemplateCypressNodeProxyBase, Lock)
{
    DeclareMutating();

    auto mode = ELockMode(request->mode());
    bool waitable = request->waitable();

    auto lockRequest = TLockRequest(mode);

    if (request->has_child_key()) {
        if (mode != ELockMode::Shared) {
            THROW_ERROR_EXCEPTION("Only %Qlv locks are allowed on child keys, got %Qlv",
                ELockMode::Shared,
                mode);
        }
        lockRequest.ChildKey = request->child_key();
    }

    if (request->has_attribute_key()) {
        if (mode != ELockMode::Shared) {
            THROW_ERROR_EXCEPTION("Only %Qlv locks are allowed on attribute keys, got %Qlv",
                ELockMode::Shared,
                mode);
        }
        lockRequest.AttributeKey = request->attribute_key();
    }

    if (mode != ELockMode::Snapshot &&
        mode != ELockMode::Shared &&
        mode != ELockMode::Exclusive)
    {
        THROW_ERROR_EXCEPTION("Invalid lock mode %Qlv",
            mode);
    }

    context->SetRequestInfo("Mode: %v, Waitable: %v",
        mode,
        waitable);

    ValidateTransaction();
    ValidatePermission(
        EPermissionCheckScope::This,
        mode == ELockMode::Snapshot ? EPermission::Read : EPermission::Write);

    auto cypressManager = Bootstrap->GetCypressManager();
    auto* lock = cypressManager->CreateLock(
        TrunkNode,
        Transaction,
        lockRequest,
        waitable);
    auto lockId = GetObjectId(lock);
    ToProto(response->mutable_lock_id(), lockId);

    context->SetResponseInfo("LockId: %v",
        lockId);

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TNontemplateCypressNodeProxyBase, Create)
{
    DeclareMutating();

    auto type = EObjectType(request->type());
    auto ignoreExisting = request->ignore_existing();
    auto recursive = request->recursive();
    const auto& path = GetRequestYPath(context);

    context->SetRequestInfo("Type: %v, IgnoreExisting: %v, Recursive: %v",
        type,
        ignoreExisting,
        recursive);

    if (path.Empty()) {
        if (ignoreExisting && GetThisImpl()->GetType() == type) {
            ToProto(response->mutable_node_id(), GetId());
            context->Reply();
            return;
        }
        ThrowAlreadyExists(this);
    }

    if (!CanHaveChildren()) {
        ThrowCannotHaveChildren(this);
    }

    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);

    auto* node = GetThisImpl();
    auto* account = node->GetAccount();
    ValidatePermission(account, EPermission::Use);

    auto factory = CreateCypressFactory(false);

    auto attributes = request->has_node_attributes()
        ? FromProto(request->node_attributes())
        : CreateEphemeralAttributes();

    auto newProxy = factory->CreateNode(
        type,
        attributes.get(),
        request,
        response);

    SetChild(factory, path, newProxy, request->recursive());

    factory->Commit();

    context->SetResponseInfo("NodeId: %v", newProxy->GetId());

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TNontemplateCypressNodeProxyBase, Copy)
{
    DeclareMutating();

    auto sourcePath = request->source_path();
    bool preserveAccount = request->preserve_account();
    bool removeSource = request->remove_source();
    auto recursive = request->recursive();
    auto targetPath = GetRequestYPath(context);

    context->SetRequestInfo("SourcePath: %v, PreserveAccount: %v, RemoveSource: %v, Recursive: %v",
        sourcePath,
        preserveAccount,
        removeSource,
        recursive);

    auto ytreeSourceProxy = GetResolver()->ResolvePath(sourcePath);
    auto* sourceProxy = dynamic_cast<ICypressNodeProxy*>(ytreeSourceProxy.Get());
    YCHECK(sourceProxy);

    if (targetPath.empty()) {
        ThrowAlreadyExists(this);
    }

    if (!CanHaveChildren()) {
        ThrowCannotHaveChildren(this);
    }

    auto* trunkSourceImpl = sourceProxy->GetTrunkNode();
    auto* sourceImpl = removeSource
        ? LockImpl(trunkSourceImpl, ELockMode::Exclusive, true)
        : GetImpl(trunkSourceImpl);

    if (IsParentOf(sourceImpl, GetThisImpl())) {
        THROW_ERROR_EXCEPTION("Cannot copy or move a node to its descendant");
    }

    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);

    ValidatePermission(
        trunkSourceImpl,
        EPermissionCheckScope::This | EPermissionCheckScope::Descendants,
        removeSource ? EPermission::Read : EPermission::Read | EPermission::Write);

    auto sourceParent = sourceProxy->GetParent();
    if (removeSource) {
        // Cf. TNodeBase::RemoveSelf
        if (!sourceParent) {
            ThrowCannotRemoveRoot();
        }

        ValidatePermission(sourceImpl, EPermissionCheckScope::This, EPermission::Write);
        ValidatePermission(sourceImpl, EPermissionCheckScope::Descendants, EPermission::Write);
        ValidatePermission(sourceImpl, EPermissionCheckScope::Parent, EPermission::Write);
    }

    auto factory = CreateCypressFactory(preserveAccount);

    auto* clonedImpl = factory->CloneNode(
        sourceImpl,
        removeSource ? ENodeCloneMode::Move : ENodeCloneMode::Copy);
    auto* clonedTrunkImpl = clonedImpl->GetTrunkNode();
    auto clonedProxy = GetProxy(clonedTrunkImpl);

    SetChild(factory, targetPath, clonedProxy, recursive);

    factory->Commit();

    if (removeSource) {
        sourceParent->RemoveChild(sourceProxy);
    }

    ToProto(response->mutable_object_id(), clonedTrunkImpl->GetId());

    context->SetRequestInfo("NodeId: %v", clonedTrunkImpl->GetId());

    context->Reply();
}

TAccessControlDescriptor* TNontemplateCypressNodeProxyBase::FindThisAcd()
{
    auto securityManager = Bootstrap->GetSecurityManager();
    auto* node = GetThisImpl();
    return securityManager->FindAcd(node);
}

////////////////////////////////////////////////////////////////////////////////

TNontemplateCompositeCypressNodeProxyBase::TNontemplateCompositeCypressNodeProxyBase(
    INodeTypeHandlerPtr typeHandler,
    NCellMaster::TBootstrap* bootstrap,
    TTransaction* transaction,
    TCypressNodeBase* trunkNode)
    : TNontemplateCypressNodeProxyBase(
        typeHandler,
        bootstrap,
        transaction,
        trunkNode)
{ }

TIntrusivePtr<const ICompositeNode> TNontemplateCompositeCypressNodeProxyBase::AsComposite() const
{
    return this;
}

TIntrusivePtr<ICompositeNode> TNontemplateCompositeCypressNodeProxyBase::AsComposite()
{
    return this;
}

void TNontemplateCompositeCypressNodeProxyBase::ListSystemAttributes(std::vector<TAttributeInfo>* attributes)
{
    attributes->push_back("count");
    TNontemplateCypressNodeProxyBase::ListSystemAttributes(attributes);
}

bool TNontemplateCompositeCypressNodeProxyBase::GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer)
{
    if (key == "count") {
        BuildYsonFluently(consumer)
            .Value(GetChildCount());
        return true;
    }

    return TNontemplateCypressNodeProxyBase::GetBuiltinAttribute(key, consumer);
}

bool TNontemplateCompositeCypressNodeProxyBase::CanHaveChildren() const
{
    return true;
}

////////////////////////////////////////////////////////////////////////////////

TMapNodeProxy::TMapNodeProxy(
    INodeTypeHandlerPtr typeHandler,
    TBootstrap* bootstrap,
    TTransaction* transaction,
    TMapNode* trunkNode)
    : TBase(
        typeHandler,
        bootstrap,
        transaction,
        trunkNode)
{ }

void TMapNodeProxy::Clear()
{
    // Take shared lock for the node itself.
    auto* impl = LockThisTypedImpl(ELockMode::Shared);

    // Construct children list.
    auto keyToChild = GetMapNodeChildren(Bootstrap, TrunkNode, Transaction);

    // Take shared locks for children.
    typedef std::pair<Stroka, TCypressNodeBase*> TChild;
    std::vector<TChild> children;
    children.reserve(keyToChild.size());
    for (const auto& pair : keyToChild) {
        LockThisImpl(TLockRequest::SharedChild(pair.first));
        auto* childImpl = LockImpl(pair.second);
        children.push_back(std::make_pair(pair.first, childImpl));
    }

    // Insert tombstones (if in transaction).
    for (const auto& pair : children) {
        const auto& key = pair.first;
        auto* child = pair.second;
        DoRemoveChild(impl, key, child);
    }

    SetModified();
}

int TMapNodeProxy::GetChildCount() const
{
    auto cypressManager = Bootstrap->GetCypressManager();
    auto transactionManager = Bootstrap->GetTransactionManager();

    auto transactions = transactionManager->GetTransactionPath(Transaction);
    // NB: No need to reverse transactions.

    int result = 0;
    for (const auto* currentTransaction : transactions) {
        TVersionedNodeId versionedId(Object_->GetId(), GetObjectId(currentTransaction));
        const auto* node = cypressManager->FindNode(versionedId);
        if (node) {
            const auto* mapNode = static_cast<const TMapNode*>(node);
            result += mapNode->ChildCountDelta();
        }
    }
    return result;
}

std::vector< std::pair<Stroka, INodePtr> > TMapNodeProxy::GetChildren() const
{
    auto keyToChild = GetMapNodeChildren(Bootstrap, TrunkNode, Transaction);

    std::vector< std::pair<Stroka, INodePtr> > result;
    result.reserve(keyToChild.size());
    for (const auto& pair : keyToChild) {
        result.push_back(std::make_pair(pair.first, GetProxy(pair.second)));
    }

    return result;
}

std::vector<Stroka> TMapNodeProxy::GetKeys() const
{
    auto keyToChild = GetMapNodeChildren(Bootstrap, TrunkNode, Transaction);

    std::vector<Stroka> result;
    for (const auto& pair : keyToChild) {
        result.push_back(pair.first);
    }

    return result;
}

INodePtr TMapNodeProxy::FindChild(const Stroka& key) const
{
    auto* childTrunkNode = FindMapNodeChild(Bootstrap, TrunkNode, Transaction, key);
    return childTrunkNode ? GetProxy(childTrunkNode) : nullptr;
}

bool TMapNodeProxy::AddChild(INodePtr child, const Stroka& key)
{
    YASSERT(!key.empty());

    if (FindChild(key)) {
        return false;
    }

    auto* impl = LockThisTypedImpl(TLockRequest::SharedChild(key));
    auto* trunkChildImpl = ToProxy(child)->GetTrunkNode();
    auto* childImpl = LockImpl(trunkChildImpl);

    impl->KeyToChild()[key] = trunkChildImpl;
    YCHECK(impl->ChildToKey().insert(std::make_pair(trunkChildImpl, key)).second);
    ++impl->ChildCountDelta();

    AttachChild(Bootstrap, TrunkNode, childImpl);

    SetModified();

    return true;
}

bool TMapNodeProxy::RemoveChild(const Stroka& key)
{
    auto* trunkChildImpl = FindMapNodeChild(Bootstrap, TrunkNode, Transaction, key);
    if (!trunkChildImpl) {
        return false;
    }

    auto* childImpl = LockImpl(trunkChildImpl, ELockMode::Exclusive, true);
    auto* impl = LockThisTypedImpl(TLockRequest::SharedChild(key));
    DoRemoveChild(impl, key, childImpl);

    SetModified();

    return true;
}

void TMapNodeProxy::RemoveChild(INodePtr child)
{
    auto key = GetChildKey(child);
    auto* trunkChildImpl = ToProxy(child)->GetTrunkNode();

    auto* childImpl = LockImpl(trunkChildImpl, ELockMode::Exclusive, true);
    auto* impl = LockThisTypedImpl(TLockRequest::SharedChild(key));
    DoRemoveChild(impl, key, childImpl);

    SetModified();
}

void TMapNodeProxy::ReplaceChild(INodePtr oldChild, INodePtr newChild)
{
    if (oldChild == newChild)
        return;

    auto key = GetChildKey(oldChild);

    auto* oldTrunkChildImpl = ToProxy(oldChild)->GetTrunkNode();
    auto* oldChildImpl = LockImpl(oldTrunkChildImpl, ELockMode::Exclusive, true);

    auto* newTrunkChildImpl = ToProxy(newChild)->GetTrunkNode();
    auto* newChildImpl = LockImpl(newTrunkChildImpl);

    auto* impl = LockThisTypedImpl(TLockRequest::SharedChild(key));

    auto& keyToChild = impl->KeyToChild();
    auto& childToKey = impl->ChildToKey();

    bool ownsOldChild = keyToChild.find(key) != keyToChild.end();
    DetachChild(Bootstrap, TrunkNode, oldChildImpl, ownsOldChild);

    keyToChild[key] = newTrunkChildImpl;
    YCHECK(childToKey.insert(std::make_pair(newTrunkChildImpl, key)).second);
    AttachChild(Bootstrap, TrunkNode, newChildImpl);

    SetModified();
}

Stroka TMapNodeProxy::GetChildKey(IConstNodePtr child)
{
    auto cypressManager = Bootstrap->GetCypressManager();
    auto transactionManager = Bootstrap->GetTransactionManager();

    auto transactions = transactionManager->GetTransactionPath(Transaction);
    // NB: Use the latest key, don't reverse transactions.

    auto* trunkChildImpl = ToProxy(child)->GetTrunkNode();

    for (const auto* currentTransaction : transactions) {
        TVersionedNodeId versionedId(Object_->GetId(), GetObjectId(currentTransaction));
        const auto* node = cypressManager->FindNode(versionedId);
        if (node) {
            const auto* mapNode = static_cast<const TMapNode*>(node);
            auto it = mapNode->ChildToKey().find(trunkChildImpl);
            if (it != mapNode->ChildToKey().end()) {
                return it->second;
            }
        }
    }

    // COMPAT(babenko)
    return "?";
}

bool TMapNodeProxy::DoInvoke(NRpc::IServiceContextPtr context)
{
    DISPATCH_YPATH_SERVICE_METHOD(List);
    return TBase::DoInvoke(context);
}

void TMapNodeProxy::SetChild(
    INodeFactoryPtr factory,
    const TYPath& path,
    INodePtr value,
    bool recursive)
{
    TMapNodeMixin::SetChild(factory, path, value, recursive);
}

IYPathService::TResolveResult TMapNodeProxy::ResolveRecursive(
    const TYPath& path,
    IServiceContextPtr context)
{
    return TMapNodeMixin::ResolveRecursive(path, context);
}

void TMapNodeProxy::DoRemoveChild(
    TMapNode* impl,
    const Stroka& key,
    TCypressNodeBase* childImpl)
{
    auto* trunkChildImpl = childImpl->GetTrunkNode();
    auto& keyToChild = impl->KeyToChild();
    auto& childToKey = impl->ChildToKey();
    if (Transaction) {
        auto it = keyToChild.find(key);
        if (it == keyToChild.end()) {
            // TODO(babenko): remove cast when GCC supports native nullptr
            YCHECK(keyToChild.insert(std::make_pair(key, (TCypressNodeBase*) nullptr)).second);
            DetachChild(Bootstrap, TrunkNode, childImpl, false);
        } else {
            it->second = nullptr;
            YCHECK(childToKey.erase(trunkChildImpl) == 1);
            DetachChild(Bootstrap, TrunkNode, childImpl, true);
        }
    } else {
        YCHECK(keyToChild.erase(key) == 1);
        YCHECK(childToKey.erase(trunkChildImpl) == 1);
        DetachChild(Bootstrap, TrunkNode, childImpl, true);
    }
    --impl->ChildCountDelta();
}

////////////////////////////////////////////////////////////////////////////////

TListNodeProxy::TListNodeProxy(
    INodeTypeHandlerPtr typeHandler,
    TBootstrap* bootstrap,
    TTransaction* transaction,
    TListNode* trunkNode)
    : TBase(
        typeHandler,
        bootstrap,
        transaction,
        trunkNode)
{ }

void TListNodeProxy::Clear()
{
    auto* impl = LockThisTypedImpl();

    // Lock children and collect impls.
    std::vector<TCypressNodeBase*> children;
    for (auto* trunkChild : impl->IndexToChild()) {
        children.push_back(LockImpl(trunkChild));
    }

    // Detach children.
    for (auto* child : children) {
        DetachChild(Bootstrap, TrunkNode, child, true);
    }

    impl->IndexToChild().clear();
    impl->ChildToIndex().clear();

    SetModified();
}

int TListNodeProxy::GetChildCount() const
{
    const auto* impl = GetThisTypedImpl();
    return impl->IndexToChild().size();
}

std::vector<INodePtr> TListNodeProxy::GetChildren() const
{
    std::vector<INodePtr> result;
    const auto* impl = GetThisTypedImpl();
    const auto& indexToChild = impl->IndexToChild();
    result.reserve(indexToChild.size());
    for (auto* child : indexToChild) {
        result.push_back(GetProxy(child));
    }
    return result;
}

INodePtr TListNodeProxy::FindChild(int index) const
{
    const auto* impl = GetThisTypedImpl();
    const auto& indexToChild = impl->IndexToChild();
    return index >= 0 && index < indexToChild.size() ? GetProxy(indexToChild[index]) : nullptr;
}

void TListNodeProxy::AddChild(INodePtr child, int beforeIndex /*= -1*/)
{
    auto* impl = LockThisTypedImpl();
    auto& list = impl->IndexToChild();

    auto* trunkChildImpl = ToProxy(child)->GetTrunkNode();
    auto* childImpl = LockImpl(trunkChildImpl);

    if (beforeIndex < 0) {
        YCHECK(impl->ChildToIndex().insert(std::make_pair(trunkChildImpl, static_cast<int>(list.size()))).second);
        list.push_back(trunkChildImpl);
    } else {
        // Update indices.
        for (auto it = list.begin() + beforeIndex; it != list.end(); ++it) {
            ++impl->ChildToIndex()[*it];
        }

        // Insert the new child.
        YCHECK(impl->ChildToIndex().insert(std::make_pair(trunkChildImpl, beforeIndex)).second);
        list.insert(list.begin() + beforeIndex, trunkChildImpl);
    }

    AttachChild(Bootstrap, TrunkNode, childImpl);

    SetModified();
}

bool TListNodeProxy::RemoveChild(int index)
{
    auto* impl = LockThisTypedImpl();
    auto& list = impl->IndexToChild();

    if (index < 0 || index >= list.size()) {
        return false;
    }

    auto* trunkChildImpl = list[index];
    auto* childImpl = LockImpl(trunkChildImpl, ELockMode::Exclusive, true);

    // Update the indices.
    for (auto it = list.begin() + index + 1; it != list.end(); ++it) {
        --impl->ChildToIndex()[*it];
    }

    // Remove the child.
    list.erase(list.begin() + index);
    YCHECK(impl->ChildToIndex().erase(trunkChildImpl));
    DetachChild(Bootstrap, TrunkNode, childImpl, true);

    SetModified();
    return true;
}

void TListNodeProxy::RemoveChild(INodePtr child)
{
    int index = GetChildIndex(child);
    YCHECK(RemoveChild(index));
}

void TListNodeProxy::ReplaceChild(INodePtr oldChild, INodePtr newChild)
{
    if (oldChild == newChild)
        return;

    auto* impl = LockThisTypedImpl();

    auto* oldTrunkChildImpl = ToProxy(oldChild)->GetTrunkNode();
    auto* oldChildImpl = LockImpl(oldTrunkChildImpl);

    auto* newTrunkChildImpl = ToProxy(newChild)->GetTrunkNode();
    auto* newChildImpl = LockImpl(newTrunkChildImpl);

    auto it = impl->ChildToIndex().find(oldTrunkChildImpl);
    YASSERT(it != impl->ChildToIndex().end());

    int index = it->second;

    DetachChild(Bootstrap, TrunkNode, oldChildImpl, true);

    impl->IndexToChild()[index] = newTrunkChildImpl;
    impl->ChildToIndex().erase(it);
    YCHECK(impl->ChildToIndex().insert(std::make_pair(newTrunkChildImpl, index)).second);
    AttachChild(Bootstrap, TrunkNode, newChildImpl);

    SetModified();
}

int TListNodeProxy::GetChildIndex(IConstNodePtr child)
{
    const auto* impl = GetThisTypedImpl();

    auto* trunkChildImpl = ToProxy(child)->GetTrunkNode();

    auto it = impl->ChildToIndex().find(trunkChildImpl);
    YCHECK(it != impl->ChildToIndex().end());

    return it->second;
}

void TListNodeProxy::SetChild(
    INodeFactoryPtr factory,
    const TYPath& path,
    INodePtr value,
    bool recursive)
{
    TListNodeMixin::SetChild(factory, path, value, recursive);
}

IYPathService::TResolveResult TListNodeProxy::ResolveRecursive(
    const TYPath& path,
    IServiceContextPtr context)
{
    return TListNodeMixin::ResolveRecursive(path, context);
}

////////////////////////////////////////////////////////////////////////////////

class TLinkNodeProxy::TDoesNotExistService
    : public TYPathServiceBase
    , public TSupportsExists
{
private:
    bool DoInvoke(NRpc::IServiceContextPtr context)
    {
        DISPATCH_YPATH_SERVICE_METHOD(Exists);
        return TYPathServiceBase::DoInvoke(context);
    }

    virtual TResolveResult Resolve(
        const TYPath& path,
        IServiceContextPtr /*context*/) override
    {
        return TResolveResult::Here(path);
    }

    virtual void ExistsSelf(
        TReqExists* /*request*/,
        TRspExists* /*response*/,
        TCtxExistsPtr context) override
    {
        ExistsAny(context);
    }

    virtual void ExistsRecursive(
        const TYPath& /*path*/,
        TReqExists* /*request*/,
        TRspExists* /*response*/,
        TCtxExistsPtr context) override
    {
        ExistsAny(context);
    }

    virtual void ExistsAttribute(
        const TYPath& /*path*/,
        TReqExists* /*request*/,
        TRspExists* /*response*/,
        TCtxExistsPtr context) override
    {
        ExistsAny(context);
    }

    void ExistsAny(TCtxExistsPtr context)
    {
        context->SetRequestInfo();
        Reply(context, false);
    }
};

////////////////////////////////////////////////////////////////////////////////

TLinkNodeProxy::TLinkNodeProxy(
    INodeTypeHandlerPtr typeHandler,
    TBootstrap* bootstrap,
    TTransaction* transaction,
    TLinkNode* trunkNode)
    : TBase(
        typeHandler,
        bootstrap,
        transaction,
        trunkNode)
{ }

IYPathService::TResolveResult TLinkNodeProxy::Resolve(
    const TYPath& path,
    IServiceContextPtr context)
{
    const auto& method = context->GetMethod();

    auto propagate = [&] () -> TResolveResult {
        if (method == "Exists") {
            auto proxy = FindTargetProxy();
            static IYPathServicePtr doesNotExistService = New<TDoesNotExistService>();
            return
                proxy
                ? TResolveResult::There(proxy, path)
                : TResolveResult::There(doesNotExistService, path);
        } else {
            return TResolveResult::There(GetTargetProxy(), path);
        }
    };

    NYPath::TTokenizer tokenizer(path);
    switch (tokenizer.Advance()) {
        case NYPath::ETokenType::Ampersand:
            return TBase::Resolve(tokenizer.GetSuffix(), context);

        case NYPath::ETokenType::EndOfStream: {
            // NB: Always handle Remove and Create locally.
            if (method == "Remove" || method == "Create") {
                return TResolveResult::Here(path);
            } else if (method == "Exists") {
                return propagate();
            } else {
                return TResolveResult::There(GetTargetProxy(), path);
            }
        }

        default:
            return propagate();
    }
}

void TLinkNodeProxy::ListSystemAttributes(std::vector<TAttributeInfo>* attributes)
{
    TBase::ListSystemAttributes(attributes);
    attributes->push_back("target_id");
    attributes->push_back(TAttributeInfo("target_path", true, true));
    attributes->push_back("broken");
}

bool TLinkNodeProxy::GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer)
{
    const auto* impl = GetThisTypedImpl();
    const auto& targetId = impl->GetTargetId();

    if (key == "target_id") {
        BuildYsonFluently(consumer)
            .Value(targetId);
        return true;
    }

    if (key == "target_path") {
        auto target = FindTargetProxy();
        if (target) {
            auto objectManager = Bootstrap->GetObjectManager();
            auto* resolver = objectManager->GetObjectResolver();
            auto path = resolver->GetPath(target);
            BuildYsonFluently(consumer)
                .Value(path);
        } else {
            BuildYsonFluently(consumer)
                .Value(FromObjectId(impl->GetTargetId()));
        }
        return true;
    }

    if (key == "broken") {
        BuildYsonFluently(consumer)
            .Value(IsBroken(targetId));
        return true;
    }

    return TBase::GetBuiltinAttribute(key, consumer);
}

bool TLinkNodeProxy::SetBuiltinAttribute(const Stroka& key, const TYsonString& value)
{
    if (key == "target_id") {
        auto targetId = ConvertTo<TObjectId>(value);
        auto* impl = LockThisTypedImpl();
        impl->SetTargetId(targetId);
        return true;
    }

    if (key == "target_path") {
        auto targetPath = ConvertTo<Stroka>(value);
        auto objectManager = Bootstrap->GetObjectManager();
        auto* resolver = objectManager->GetObjectResolver();
        auto targetProxy = resolver->ResolvePath(targetPath, Transaction);
        auto* impl = LockThisTypedImpl();
        impl->SetTargetId(targetProxy->GetId());
        return true;
    }

    return TBase::SetBuiltinAttribute(key, value);
}

IObjectProxyPtr TLinkNodeProxy::FindTargetProxy() const
{
    const auto* impl = GetThisTypedImpl();
    const auto& targetId = impl->GetTargetId();

    if (IsBroken(targetId)) {
        return nullptr;
    }

    auto objectManager = Bootstrap->GetObjectManager();
    auto* target = objectManager->GetObject(targetId);
    return objectManager->GetProxy(target, Transaction);
}

IObjectProxyPtr TLinkNodeProxy::GetTargetProxy() const
{
    auto result = FindTargetProxy();
    if (!result) {
        const auto* impl = GetThisTypedImpl();
        THROW_ERROR_EXCEPTION("Link target %v does not exist",
            impl->GetTargetId());
    }
    return result;
}

bool TLinkNodeProxy::IsBroken(const NObjectServer::TObjectId& id) const
{
    if (IsVersionedType(TypeFromId(id))) {
        auto cypressManager = Bootstrap->GetCypressManager();
        auto* node = cypressManager->FindNode(TVersionedNodeId(id));
        return cypressManager->IsOrphaned(node);
    } else {
        auto objectManager = Bootstrap->GetObjectManager();
        auto* obj = objectManager->FindObject(id);
        return !IsObjectAlive(obj);
    }
}

////////////////////////////////////////////////////////////////////////////////

TDocumentNodeProxy::TDocumentNodeProxy(
    INodeTypeHandlerPtr typeHandler,
    TBootstrap* bootstrap,
    TTransaction* transaction,
    TDocumentNode* trunkNode)
    : TBase(
        typeHandler,
        bootstrap,
        transaction,
        trunkNode)
{ }

ENodeType TDocumentNodeProxy::GetType() const 
{
    return ENodeType::Entity;
}

TIntrusivePtr<const IEntityNode> TDocumentNodeProxy::AsEntity() const
{
    return this;
}

TIntrusivePtr<IEntityNode> TDocumentNodeProxy::AsEntity()
{
    return this;
}

IYPathService::TResolveResult TDocumentNodeProxy::ResolveRecursive(const TYPath& path, IServiceContextPtr context)
{
    return TResolveResult::Here("/" + path);
}

namespace {

template <class TServerRequest, class TServerResponse, class TContext>
void DelegateInvocation(
    IYPathServicePtr service,
    TServerRequest* serverRequest,
    TServerResponse* serverResponse,
    TIntrusivePtr<TContext> context)
{
    typedef typename TServerRequest::TMessage  TRequestMessage;
    typedef typename TServerResponse::TMessage TResponseMessage;
    
    typedef TTypedYPathRequest<TRequestMessage, TResponseMessage>  TClientRequest;
    typedef TTypedYPathResponse<TRequestMessage, TResponseMessage> TClientResponse;

    auto clientRequest = New<TClientRequest>(context->RequestHeader());
    clientRequest->MergeFrom(*serverRequest);

    auto clientResponseOrError = ExecuteVerb(service, clientRequest).Get();

    if (clientResponseOrError.IsOK()) {
        const auto& clientResponse = clientResponseOrError.Value();
        serverResponse->MergeFrom(*clientResponse);
        context->Reply();
    } else {
        context->Reply(clientResponseOrError);
    }
}

} // namespace

void TDocumentNodeProxy::GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisTypedImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::GetRecursive(const TYPath& /*path*/, TReqGet* request, TRspGet* response, TCtxGetPtr context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisTypedImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::SetSelf(TReqSet* request, TRspSet* /*response*/, TCtxSetPtr context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);
    auto* impl = LockThisTypedImpl();
    impl->SetValue(ConvertToNode(TYsonString(request->value())));
    context->Reply();
}

void TDocumentNodeProxy::SetRecursive(const TYPath& /*path*/, TReqSet* request, TRspSet* response, TCtxSetPtr context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);
    auto* impl = LockThisTypedImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::ListSelf(TReqList* request, TRspList* response, TCtxListPtr context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisTypedImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::ListRecursive(const TYPath& /*path*/, TReqList* request, TRspList* response, TCtxListPtr context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisTypedImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::RemoveRecursive(const TYPath& /*path*/, TReqRemove* request, TRspRemove* response, TCtxRemovePtr context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);
    auto* impl = LockThisTypedImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::ExistsRecursive(const TYPath& /*path*/, TReqExists* request, TRspExists* response, TCtxExistsPtr context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisTypedImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::ListSystemAttributes(std::vector<TAttributeInfo>* attributes)
{
    TBase::ListSystemAttributes(attributes);
    attributes->push_back(TAttributeInfo("value", true, true));
}

bool TDocumentNodeProxy::GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer)
{
    const auto* impl = GetThisTypedImpl();

    if (key == "value") {
        BuildYsonFluently(consumer)
            .Value(impl->GetValue());
        return true;
    }

    return TBase::GetBuiltinAttribute(key, consumer);
}

bool TDocumentNodeProxy::SetBuiltinAttribute(const Stroka& key, const TYsonString& value)
{
    if (key == "value") {
        auto* impl = LockThisTypedImpl();
        impl->SetValue(ConvertToNode(value));
        return true;
    }

    return TBase::SetBuiltinAttribute(key, value);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT

