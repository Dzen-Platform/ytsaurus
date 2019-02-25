#include "node_proxy_detail.h"
#include "private.h"
#include "cypress_traverser.h"
#include "helpers.h"

#include <yt/server/master/cell_master/config.h>
#include <yt/server/master/cell_master/multicell_manager.h>
#include <yt/server/master/cell_master/bootstrap.h>
#include <yt/server/master/cell_master/hydra_facade.h>

#include <yt/server/master/chunk_server/chunk_list.h>
#include <yt/server/master/chunk_server/chunk_manager.h>
#include <yt/server/master/chunk_server/chunk_owner_base.h>
#include <yt/server/master/chunk_server/medium.h>

#include <yt/server/lib/misc/interned_attributes.h>

#include <yt/server/master/security_server/account.h>
#include <yt/server/master/security_server/security_manager.h>
#include <yt/server/master/security_server/user.h>

#include <yt/server/master/tablet_server/tablet_cell_bundle.h>
#include <yt/server/master/tablet_server/tablet_manager.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>
#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/logging/fluent_log.h>

#include <yt/core/misc/string.h>

#include <yt/core/ypath/tokenizer.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/ephemeral_node_factory.h>
#include <yt/core/ytree/exception_helpers.h>
#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/node_detail.h>
#include <yt/core/ytree/ypath_client.h>
#include <yt/core/ytree/ypath_detail.h>

#include <yt/core/yson/async_writer.h>

#include <type_traits>

namespace NYT::NCypressServer {

using namespace NYTree;
using namespace NLogging;
using namespace NYson;
using namespace NYPath;
using namespace NRpc;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NCellMaster;
using namespace NChunkServer;
using namespace NTransactionServer;
using namespace NSecurityServer;
using namespace NCypressClient;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CypressServerLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

bool HasTrivialAcd(const TCypressNodeBase* node)
{
    const auto& acd = node->Acd();
    return acd.GetInherit() && acd.Acl().Entries.empty();
}

bool CheckItemReadPermissions(
    TCypressNodeBase* parent,
    TCypressNodeBase* child,
    const TSecurityManagerPtr& securityManager)
{
    // Fast path.
    if ((!parent || HasTrivialAcd(parent)) && HasTrivialAcd(child)) {
        return true;
    }

    // Slow path.
    auto* user = securityManager->GetAuthenticatedUser();
    return securityManager->CheckPermission(child, user, EPermission::Read).Action == ESecurityAction::Allow;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TNontemplateCypressNodeProxyBase::TCustomAttributeDictionary::TCustomAttributeDictionary(
    TNontemplateCypressNodeProxyBase* proxy)
    : Proxy_(proxy)
{ }

std::vector<TString> TNontemplateCypressNodeProxyBase::TCustomAttributeDictionary::List() const
{
    auto keys = ListNodeAttributes(
        Proxy_->Bootstrap_->GetCypressManager(),
        Proxy_->TrunkNode,
        Proxy_->Transaction);
    return std::vector<TString>(keys.begin(), keys.end());
}

TYsonString TNontemplateCypressNodeProxyBase::TCustomAttributeDictionary::FindYson(const TString& name) const
{
    const auto& cypressManager = Proxy_->Bootstrap_->GetCypressManager();
    auto originators = cypressManager->GetNodeOriginators(Proxy_->GetTransaction(), Proxy_->GetTrunkNode());
    for (const auto* node : originators) {
        const auto* userAttributes = node->GetAttributes();
        if (userAttributes) {
            auto it = userAttributes->Attributes().find(name);
            if (it != userAttributes->Attributes().end()) {
                return it->second;
            }
        }
    }

    return TYsonString();
}

void TNontemplateCypressNodeProxyBase::TCustomAttributeDictionary::SetYson(const TString& key, const TYsonString& value)
{
    Y_ASSERT(value);

    auto oldValue = FindYson(key);
    Proxy_->GuardedValidateCustomAttributeUpdate(key, oldValue, value);

    const auto& cypressManager = Proxy_->Bootstrap_->GetCypressManager();
    auto* node = cypressManager->LockNode(
        Proxy_->TrunkNode,
        Proxy_->Transaction,
        TLockRequest::MakeSharedAttribute(key));

    auto* userAttributes = node->GetMutableAttributes();
    userAttributes->Attributes()[key] = value;

    Proxy_->SetModified(EModificationType::Attributes);
}

bool TNontemplateCypressNodeProxyBase::TCustomAttributeDictionary::Remove(const TString& key)
{
    auto oldValue = FindYson(key);
    if (!oldValue) {
        return false;
    }

    Proxy_->GuardedValidateCustomAttributeUpdate(key, oldValue, TYsonString());

    const auto& cypressManager = Proxy_->Bootstrap_->GetCypressManager();
    auto* node = cypressManager->LockNode(
        Proxy_->TrunkNode,
        Proxy_->Transaction,
        TLockRequest::MakeSharedAttribute(key));

    auto* userAttributes = node->GetMutableAttributes();
    if (node->GetTransaction()) {
        userAttributes->Attributes()[key] = TYsonString();
    } else {
        YCHECK(userAttributes->Attributes().erase(key) == 1);
    }

    Proxy_->SetModified(EModificationType::Attributes);
    return true;
}

////////////////////////////////////////////////////////////////////////////////

class TNontemplateCypressNodeProxyBase::TResourceUsageVisitor
    : public ICypressNodeVisitor
{
public:
    TResourceUsageVisitor(
        NCellMaster::TBootstrap* bootstrap,
        ICypressNodeProxyPtr rootNode)
        : Bootstrap_(bootstrap)
        , RootNode_(std::move(rootNode))
    { }

    TPromise<TYsonString> Run()
    {
        TraverseCypress(
            Bootstrap_->GetCypressManager(),
            Bootstrap_->GetTransactionManager(),
            Bootstrap_->GetObjectManager(),
            Bootstrap_->GetSecurityManager(),
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::CypressTraverser),
            RootNode_->GetTrunkNode(),
            RootNode_->GetTransaction(),
            this);
        return Promise_;
    }

private:
    NCellMaster::TBootstrap* const Bootstrap_;
    const ICypressNodeProxyPtr RootNode_;

    TPromise<TYsonString> Promise_ = NewPromise<TYsonString>();
    TClusterResources ResourceUsage_;


    virtual void OnNode(TCypressNodeBase* trunkNode, TTransaction* transaction) override
    {
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* node = cypressManager->GetVersionedNode(trunkNode, transaction);
        ResourceUsage_ += node->GetTotalResourceUsage();
    }

    virtual void OnError(const TError& error) override
    {
        auto wrappedError = TError("Error computing recursive resource usage")
            << error;
        Promise_.Set(wrappedError);
    }

    virtual void OnCompleted() override
    {
        auto usage = New<TSerializableClusterResources>(Bootstrap_->GetChunkManager(), ResourceUsage_);
        Promise_.Set(ConvertToYsonString(usage));
    }
};

////////////////////////////////////////////////////////////////////////////////

TNontemplateCypressNodeProxyBase::TNontemplateCypressNodeProxyBase(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTransaction* transaction,
    TCypressNodeBase* trunkNode)
    : TObjectProxyBase(bootstrap, metadata, trunkNode)
    , CustomAttributesImpl_(this)
    , Transaction(transaction)
    , TrunkNode(trunkNode)
{
    Y_ASSERT(TrunkNode);
    Y_ASSERT(TrunkNode->IsTrunk());

    CustomAttributes_ = &CustomAttributesImpl_;
}

std::unique_ptr<ITransactionalNodeFactory> TNontemplateCypressNodeProxyBase::CreateFactory() const
{
    auto* account = GetThisImpl()->GetAccount();
    return CreateCypressFactory(account, TNodeFactoryOptions());
}

std::unique_ptr<ICypressNodeFactory> TNontemplateCypressNodeProxyBase::CreateCypressFactory(
    TAccount* account,
    const TNodeFactoryOptions& options) const
{
    const auto& cypressManager = Bootstrap_->GetCypressManager();
    return cypressManager->CreateNodeFactory(
        Transaction,
        account,
        options);
}

TYPath TNontemplateCypressNodeProxyBase::GetPath() const
{
    const auto& cypressManager = Bootstrap_->GetCypressManager();
    return cypressManager->GetNodePath(this);
}

TTransaction* TNontemplateCypressNodeProxyBase::GetTransaction() const
{
    return Transaction;
}

TCypressNodeBase* TNontemplateCypressNodeProxyBase::GetTrunkNode() const
{
    return TrunkNode;
}

ICompositeNodePtr TNontemplateCypressNodeProxyBase::GetParent() const
{
    auto* parent = GetThisImpl()->GetParent();
    return parent ? GetProxy(parent)->AsComposite() : nullptr;
}

void TNontemplateCypressNodeProxyBase::SetParent(const ICompositeNodePtr& parent)
{
    auto* impl = LockThisImpl();
    impl->SetParent(parent ? ICypressNodeProxy::FromNode(parent.Get())->GetTrunkNode() : nullptr);
}

const IAttributeDictionary& TNontemplateCypressNodeProxyBase::Attributes() const
{
    return TObjectProxyBase::Attributes();
}

IAttributeDictionary* TNontemplateCypressNodeProxyBase::MutableAttributes()
{
    return TObjectProxyBase::MutableAttributes();
}

TFuture<TYsonString> TNontemplateCypressNodeProxyBase::GetBuiltinAttributeAsync(TInternedAttributeKey key)
{
    switch (key) {
        case EInternedAttributeKey::RecursiveResourceUsage: {
            auto visitor = New<TResourceUsageVisitor>(Bootstrap_, this);
            return visitor->Run();
        }

        default:
            break;
    }

    auto asyncResult = GetExternalBuiltinAttributeAsync(key);
    if (asyncResult) {
        return asyncResult;
    }

    return TObjectProxyBase::GetBuiltinAttributeAsync(key);
}

TFuture<TYsonString> TNontemplateCypressNodeProxyBase::GetExternalBuiltinAttributeAsync(TInternedAttributeKey internedKey)
{
    const auto* node = GetThisImpl();
    if (!node->IsExternal()) {
        return std::nullopt;
    }

    auto optionalDescriptor = FindBuiltinAttributeDescriptor(internedKey);
    if (!optionalDescriptor) {
        return std::nullopt;
    }

    const auto& descriptor = *optionalDescriptor;
    if (!descriptor.External) {
        return std::nullopt;
    }

    auto cellTag = node->GetExternalCellTag();
    auto versionedId = GetVersionedId();

    const auto& multicellManager = Bootstrap_->GetMulticellManager();
    auto channel = multicellManager->GetMasterChannelOrThrow(
        cellTag,
        NHydra::EPeerKind::LeaderOrFollower);

    auto key = TString(GetUninternedAttributeKey(internedKey));
    auto req = TYPathProxy::Get(FromObjectId(versionedId.ObjectId) + "/@" + key);
    SetTransactionId(req, versionedId.TransactionId);

    TObjectServiceProxy proxy(channel);
    return proxy.Execute(req).Apply(BIND([=] (const TYPathProxy::TErrorOrRspGetPtr& rspOrError) {
        if (!rspOrError.IsOK()) {
            auto code = rspOrError.GetCode();
            if (code == NYTree::EErrorCode::ResolveError || code == NTransactionClient::EErrorCode::NoSuchTransaction) {
                return TYsonString();
            }
            THROW_ERROR_EXCEPTION("Error requesting attribute %Qv of object %v from cell %v",
                key,
                versionedId,
                cellTag)
                << rspOrError;
        }

        const auto& rsp = rspOrError.Value();
        return TYsonString(rsp->value());
    }));
}

bool TNontemplateCypressNodeProxyBase::SetBuiltinAttribute(TInternedAttributeKey key, const TYsonString& value)
{
    switch (key) {
        case EInternedAttributeKey::Account: {
            ValidateNoTransaction();

            const auto& securityManager = Bootstrap_->GetSecurityManager();

            auto name = ConvertTo<TString>(value);
            auto* account = securityManager->GetAccountByNameOrThrow(name);

            ValidateStorageParametersUpdate();
            ValidatePermission(account, EPermission::Use);

            auto* node = LockThisImpl();
            if (node->GetAccount() != account) {
                // TODO(savrus) See YT-7050
                securityManager->ValidateResourceUsageIncrease(account, TClusterResources().SetNodeCount(1));
                securityManager->SetAccount(node, node->GetAccount(), account, nullptr /* transaction */);
            }

            return true;
        }

        case EInternedAttributeKey::ExpirationTime: {
            ValidateNoTransaction();
            ValidatePermission(EPermissionCheckScope::This|EPermissionCheckScope::Descendants, EPermission::Remove);

            auto* node = GetThisImpl();
            const auto& cypressManager = Bootstrap_->GetCypressManager();
            if (node == cypressManager->GetRootNode()) {
                THROW_ERROR_EXCEPTION("Cannot set \"expiration_time\" for the root");
            }

            auto time = ConvertTo<TInstant>(value);
            cypressManager->SetExpirationTime(node, time);

            return true;
        }

        case EInternedAttributeKey::Opaque: {
            ValidateNoTransaction();
            ValidatePermission(EPermissionCheckScope::This, EPermission::Write);

            // NB: No locking, intentionally.
            auto* node = GetThisImpl();
            auto opaque = ConvertTo<bool>(value);
            node->SetOpaque(opaque);

            return true;
        }

        case EInternedAttributeKey::InheritAcl:
        case EInternedAttributeKey::Acl:
        case EInternedAttributeKey::Owner: {
            auto attributeApplied = TObjectProxyBase::SetBuiltinAttribute(key, value);
            if (attributeApplied && !GetThisImpl()->IsBeingCreated()) {
                LogStructuredEventFluently(Logger, ELogLevel::Info)
                    .Item("event").Value(EAccessControlEvent::ObjectAcdUpdated)
                    .Item("attribute").Value(GetUninternedAttributeKey(key))
                    .Item("path").Value(GetPath())
                    .Item("value").Value(value);
            }
            return attributeApplied;
        }

        default:
            break;
    }

    return TObjectProxyBase::SetBuiltinAttribute(key, value);
}

bool TNontemplateCypressNodeProxyBase::RemoveBuiltinAttribute(TInternedAttributeKey key)
{
    switch (key) {
        case EInternedAttributeKey::ExpirationTime: {
            ValidateNoTransaction();

            auto* node = GetThisImpl();
            const auto& cypressManager = Bootstrap_->GetCypressManager();
            cypressManager->SetExpirationTime(node, std::nullopt);

            return true;
        }

        case EInternedAttributeKey::Opaque: {
            ValidateNoTransaction();
            ValidatePermission(EPermissionCheckScope::This, EPermission::Write);

            // NB: No locking, intentionally.
            auto* node = GetThisImpl();
            node->SetOpaque(false);

            return true;
        }

        default:
            break;
    }

    return TObjectProxyBase::RemoveBuiltinAttribute(key);
}

TVersionedObjectId TNontemplateCypressNodeProxyBase::GetVersionedId() const
{
    return TVersionedObjectId(Object_->GetId(), GetObjectId(Transaction));
}

TAccessControlDescriptor* TNontemplateCypressNodeProxyBase::FindThisAcd()
{
    const auto& securityManager = Bootstrap_->GetSecurityManager();
    auto* node = GetThisImpl();
    return securityManager->FindAcd(node);
}

void TNontemplateCypressNodeProxyBase::ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors)
{
    TObjectProxyBase::ListSystemAttributes(descriptors);

    const auto* node = GetThisImpl();
    const auto* trunkNode = node->GetTrunkNode();
    bool hasKey = NodeHasKey(node);
    bool isExternal = node->IsExternal();

    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::ParentId)
        .SetPresent(node->GetParent()));
    descriptors->push_back(EInternedAttributeKey::External);
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::ExternalCellTag)
        .SetPresent(isExternal));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Locks)
        .SetOpaque(true));
    descriptors->push_back(EInternedAttributeKey::LockCount);
    descriptors->push_back(EInternedAttributeKey::LockMode);
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Path)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Key)
        .SetPresent(hasKey));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::ExpirationTime)
        .SetPresent(trunkNode->GetExpirationTime().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(EInternedAttributeKey::CreationTime);
    descriptors->push_back(EInternedAttributeKey::ModificationTime);
    descriptors->push_back(EInternedAttributeKey::AccessTime);
    descriptors->push_back(EInternedAttributeKey::AccessCounter);
    descriptors->push_back(EInternedAttributeKey::Revision);
    descriptors->push_back(EInternedAttributeKey::AttributesRevision);
    descriptors->push_back(EInternedAttributeKey::ContentRevision);
    descriptors->push_back(EInternedAttributeKey::ResourceUsage);
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::RecursiveResourceUsage)
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Account)
        .SetWritable(true)
        .SetReplicated(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Opaque)
        .SetWritable(true)
        .SetRemovable(true));
}

bool TNontemplateCypressNodeProxyBase::GetBuiltinAttribute(
    TInternedAttributeKey key,
    IYsonConsumer* consumer)
{
    const auto* node = GetThisImpl();
    const auto* trunkNode = node->GetTrunkNode();
    bool hasKey = NodeHasKey(node);
    bool isExternal = node->IsExternal();

    switch (key) {
        case EInternedAttributeKey::ParentId:
            if (!node->GetParent()) {
                break;
            }
            BuildYsonFluently(consumer)
                .Value(node->GetParent()->GetId());
            return true;

        case EInternedAttributeKey::External:
            BuildYsonFluently(consumer)
                .Value(isExternal);
            return true;

        case EInternedAttributeKey::ExternalCellTag:
            if (!isExternal) {
                break;
            }
            BuildYsonFluently(consumer)
                .Value(node->GetExternalCellTag());
            return true;

        case EInternedAttributeKey::Locks: {
            auto printLock = [=] (TFluentList fluent, const TLock* lock) {
                const auto& request = lock->Request();
                fluent.Item()
                    .BeginMap()
                        .Item("id").Value(lock->GetId())
                        .Item("state").Value(lock->GetState())
                        .Item("transaction_id").Value(lock->GetTransaction()->GetId())
                        .Item("mode").Value(request.Mode)
                        .DoIf(request.Key.Kind == ELockKeyKind::Child, [=] (TFluentMap fluent) {
                            fluent
                                .Item("child_key").Value(request.Key.Name);
                        })
                        .DoIf(request.Key.Kind == ELockKeyKind::Attribute, [=] (TFluentMap fluent) {
                            fluent
                                .Item("attribute_key").Value(request.Key.Name);
                        })
                    .EndMap();
            };

            BuildYsonFluently(consumer)
                .BeginList()
                    .DoFor(trunkNode->LockingState().AcquiredLocks, printLock)
                    .DoFor(trunkNode->LockingState().PendingLocks, printLock)
                .EndList();
            return true;
        }

        case EInternedAttributeKey::LockCount:
            BuildYsonFluently(consumer)
                .Value(trunkNode->LockingState().AcquiredLocks.size() + trunkNode->LockingState().PendingLocks.size());
            return true;

        case EInternedAttributeKey::LockMode:
            BuildYsonFluently(consumer)
                .Value(node->GetLockMode());
            return true;

        case EInternedAttributeKey::Path:
            BuildYsonFluently(consumer)
                .Value(GetPath());
            return true;

        case EInternedAttributeKey::Key: {
            if (!hasKey) {
                break;
            }
            static const TString NullKey("?");
            BuildYsonFluently(consumer)
                .Value(GetParent()->AsMap()->FindChildKey(this).value_or(NullKey));
            return true;
        }

        case EInternedAttributeKey::ExpirationTime:
            if (!trunkNode->GetExpirationTime()) {
                break;
            }
            BuildYsonFluently(consumer)
                .Value(*trunkNode->GetExpirationTime());
            return true;

        case EInternedAttributeKey::CreationTime:
            BuildYsonFluently(consumer)
                .Value(node->GetCreationTime());
            return true;

        case EInternedAttributeKey::ModificationTime:
            BuildYsonFluently(consumer)
                .Value(node->GetModificationTime());
            return true;

        case EInternedAttributeKey::AccessTime:
            BuildYsonFluently(consumer)
                .Value(trunkNode->GetAccessTime());
            return true;

        case EInternedAttributeKey::AccessCounter:
            BuildYsonFluently(consumer)
                .Value(trunkNode->GetAccessCounter());
            return true;

        case EInternedAttributeKey::Revision:
            BuildYsonFluently(consumer)
                // TODO(babenko): KWYT-630
                .Value(static_cast<i64>(node->GetRevision()));
            return true;

        case EInternedAttributeKey::AttributesRevision:
            BuildYsonFluently(consumer)
                .Value(node->GetAttributesRevision());
            return true;

        case EInternedAttributeKey::ContentRevision:
            BuildYsonFluently(consumer)
                .Value(node->GetContentRevision());
            return true;

        case EInternedAttributeKey::ResourceUsage: {
            const auto& chunkManager = Bootstrap_->GetChunkManager();
            auto resourceSerializer = New<TSerializableClusterResources>(chunkManager, node->GetTotalResourceUsage());
            BuildYsonFluently(consumer)
                .Value(resourceSerializer);
            return true;
        }

        case EInternedAttributeKey::Account:
            BuildYsonFluently(consumer)
                .Value(node->GetAccount()->GetName());
            return true;

        case EInternedAttributeKey::Opaque:
            BuildYsonFluently(consumer)
                .Value(node->GetOpaque());
            return true;

        default:
            break;
    }

    return TObjectProxyBase::GetBuiltinAttribute(key, consumer);
}

void TNontemplateCypressNodeProxyBase::ValidateStorageParametersUpdate()
{ }

void TNontemplateCypressNodeProxyBase::ValidateLockPossible()
{ }

void TNontemplateCypressNodeProxyBase::BeforeInvoke(const IServiceContextPtr& context)
{
    AccessTrackingSuppressed = GetSuppressAccessTracking(context->RequestHeader());
    ModificationTrackingSuppressed = GetSuppressModificationTracking(context->RequestHeader());

    TObjectProxyBase::BeforeInvoke(context);
}

void TNontemplateCypressNodeProxyBase::AfterInvoke(const IServiceContextPtr& context)
{
    if (!AccessTrackingSuppressed) {
        SetAccessed();
    }

    TObjectProxyBase::AfterInvoke(context);
}

bool TNontemplateCypressNodeProxyBase::DoInvoke(const NRpc::IServiceContextPtr& context)
{
    DISPATCH_YPATH_SERVICE_METHOD(Lock);
    DISPATCH_YPATH_SERVICE_METHOD(Create);
    DISPATCH_YPATH_SERVICE_METHOD(Copy);
    DISPATCH_YPATH_SERVICE_METHOD(Unlock);

    if (TNodeBase::DoInvoke(context)) {
        return true;
    }

    if (TObjectProxyBase::DoInvoke(context)) {
        return true;
    }

    return false;
}

void TNontemplateCypressNodeProxyBase::GetSelf(
    TReqGet* request,
    TRspGet* response,
    const TCtxGetPtr& context)
{
    class TVisitor
    {
    public:
        TVisitor(
            TCypressManagerPtr cypressManager,
            TSecurityManagerPtr securityManager,
            TTransaction* transaction,
            std::optional<std::vector<TString>> attributeKeys)
            : CypressManager_(std::move(cypressManager))
            , SecurityManager_(std::move(securityManager))
            , Transaction_(transaction)
            , AttributeKeys_(std::move(attributeKeys))
        { }

        void Run(TCypressNodeBase* root)
        {
            VisitAny(nullptr, root);
        }

        TFuture<TYsonString> Finish()
        {
            return Writer_.Finish();
        }

    private:
        const TCypressManagerPtr CypressManager_;
        const TSecurityManagerPtr SecurityManager_;
        TTransaction* const Transaction_;
        const std::optional<std::vector<TString>> AttributeKeys_;

        TAsyncYsonWriter Writer_;


        void VisitAny(TCypressNodeBase* trunkParent, TCypressNodeBase* trunkChild)
        {
            if (!CheckItemReadPermissions(trunkParent, trunkChild, SecurityManager_)) {
                Writer_.OnEntity();
                return;
            }

            auto proxy = CypressManager_->GetNodeProxy(trunkChild, Transaction_);
            proxy->WriteAttributes(&Writer_, AttributeKeys_, false);

            if (trunkParent && trunkChild->GetOpaque()) {
                Writer_.OnEntity();
                return;
            }

            switch (trunkChild->GetNodeType()) {
                case ENodeType::List:
                    VisitList(trunkChild->As<TListNode>());
                    break;
                case ENodeType::Map:
                    VisitMap(trunkChild->As<TMapNode>());
                    break;
                default:
                    VisitOther(trunkChild);
                    break;
            }
        }

        void VisitOther(TCypressNodeBase* trunkNode)
        {
            auto* node = CypressManager_->GetVersionedNode(trunkNode, Transaction_);
            switch (node->GetType()) {
                case EObjectType::StringNode:
                    Writer_.OnStringScalar(node->As<TStringNode>()->Value());
                    break;
                case EObjectType::Int64Node:
                    Writer_.OnInt64Scalar(node->As<TInt64Node>()->Value());
                    break;
                case EObjectType::Uint64Node:
                    Writer_.OnUint64Scalar(node->As<TUint64Node>()->Value());
                    break;
                case EObjectType::DoubleNode:
                    Writer_.OnDoubleScalar(node->As<TDoubleNode>()->Value());
                    break;
                case EObjectType::BooleanNode:
                    Writer_.OnBooleanScalar(node->As<TBooleanNode>()->Value());
                    break;
                default:
                    Writer_.OnEntity();
                    break;
            }
        }

        void VisitList(TCypressNodeBase* node)
        {
            Writer_.OnBeginList();
            const auto& childList = GetListNodeChildList(
                CypressManager_,
                node,
                Transaction_);
            for (auto* child : childList) {
                Writer_.OnListItem();
                VisitAny(node, child);
            }
            Writer_.OnEndList();
        }

        void VisitMap(TCypressNodeBase* node)
        {
            Writer_.OnBeginMap();
            THashMap<TString, TCypressNodeBase*> keyToChildMapStorage;
            const auto& keyToChildMap = GetMapNodeChildMap(
                CypressManager_,
                node,
                Transaction_,
                &keyToChildMapStorage);
            for (const auto& pair : keyToChildMap) {
                Writer_.OnKeyedItem(pair.first);
                VisitAny(node, pair.second);
            }
            Writer_.OnEndMap();
        }
    };

    auto attributeKeys = request->has_attributes()
        ? std::make_optional(FromProto<std::vector<TString>>(request->attributes().keys()))
        : std::nullopt;

    // TODO(babenko): make use of limit
    auto limit = request->has_limit()
        ? std::make_optional(request->limit())
        : std::nullopt;

    context->SetRequestInfo("AttributeKeys: %v, Limit: %v",
        attributeKeys,
        limit);

    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);

    TVisitor visitor(
        Bootstrap_->GetCypressManager(),
        Bootstrap_->GetSecurityManager(),
        Transaction,
        std::move(attributeKeys));
    visitor.Run(TrunkNode);
    visitor.Finish().Subscribe(BIND([=] (const TErrorOr<TYsonString>& resultOrError) {
        if (resultOrError.IsOK()) {
            response->set_value(resultOrError.Value().GetData());
            context->Reply();
        } else {
            context->Reply(resultOrError);
        }
    }));
}

void TNontemplateCypressNodeProxyBase::RemoveSelf(
    TReqRemove* request,
    TRspRemove* response,
    const TCtxRemovePtr& context)
{
    auto* node = GetThisImpl();
    if (node->IsForeign()) {
        YCHECK(node->IsTrunk());
        YCHECK(node->LockingState().AcquiredLocks.empty());
        const auto& objectManager = Bootstrap_->GetObjectManager();
        YCHECK(objectManager->GetObjectRefCounter(node) == 1);
        objectManager->UnrefObject(node);
    } else {
        TNodeBase::RemoveSelf(request, response, context);
    }
}

void TNontemplateCypressNodeProxyBase::GetAttribute(
    const TYPath& path,
    TReqGet* request,
    TRspGet* response,
    const TCtxGetPtr& context)
{
    SuppressAccessTracking();
    TObjectProxyBase::GetAttribute(path, request, response, context);
}

void TNontemplateCypressNodeProxyBase::ListAttribute(
    const TYPath& path,
    TReqList* request,
    TRspList* response,
    const TCtxListPtr& context)
{
    SuppressAccessTracking();
    TObjectProxyBase::ListAttribute(path, request, response, context);
}

void TNontemplateCypressNodeProxyBase::ExistsSelf(
    TReqExists* request,
    TRspExists* response,
    const TCtxExistsPtr& context)
{
    SuppressAccessTracking();
    TObjectProxyBase::ExistsSelf(request, response, context);
}

void TNontemplateCypressNodeProxyBase::ExistsRecursive(
    const TYPath& path,
    TReqExists* request,
    TRspExists* response,
    const TCtxExistsPtr& context)
{
    SuppressAccessTracking();
    TObjectProxyBase::ExistsRecursive(path, request, response, context);
}

void TNontemplateCypressNodeProxyBase::ExistsAttribute(
    const TYPath& path,
    TReqExists* request,
    TRspExists* response,
    const TCtxExistsPtr& context)
{
    SuppressAccessTracking();
    TObjectProxyBase::ExistsAttribute(path, request, response, context);
}

TCypressNodeBase* TNontemplateCypressNodeProxyBase::GetImpl(TCypressNodeBase* trunkNode) const
{
    const auto& cypressManager = Bootstrap_->GetCypressManager();
    return cypressManager->GetVersionedNode(trunkNode, Transaction);
}

TCypressNodeBase* TNontemplateCypressNodeProxyBase::LockImpl(
    TCypressNodeBase* trunkNode,
    const TLockRequest& request /*= ELockMode::Exclusive*/,
    bool recursive /*= false*/) const
{
    const auto& cypressManager = Bootstrap_->GetCypressManager();
    return cypressManager->LockNode(trunkNode, Transaction, request, recursive);
}

TCypressNodeBase* TNontemplateCypressNodeProxyBase::DoGetThisImpl()
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

TCypressNodeBase* TNontemplateCypressNodeProxyBase::DoLockThisImpl(
    const TLockRequest& request /*= ELockMode::Exclusive*/,
    bool recursive /*= false*/)
{
    // NB: Cannot use |CachedNode| here.
    CachedNode = nullptr;
    return LockImpl(TrunkNode, request, recursive);
}

void TNontemplateCypressNodeProxyBase::GatherInheritableAttributes(TCypressNodeBase* parent, TCompositeNodeBase::TAttributes* attributes)
{
    for (auto* ancestor = parent; ancestor && !attributes->AreFull(); ancestor = ancestor->GetParent())
    {
        auto* compositeAncestor = ancestor->As<TCompositeNodeBase>();

#define XX(camelCaseName, snakeCaseName) \
        { \
            auto inheritedValue = compositeAncestor->Get##camelCaseName(); \
            if (!attributes->camelCaseName && inheritedValue) { \
                attributes->camelCaseName = inheritedValue; \
            } \
        }

        if (compositeAncestor->HasInheritableAttributes()) {
            FOR_EACH_INHERITABLE_ATTRIBUTE(XX);
        }

#undef XX
    }
}

ICypressNodeProxyPtr TNontemplateCypressNodeProxyBase::GetProxy(TCypressNodeBase* trunkNode) const
{
    const auto& cypressManager = Bootstrap_->GetCypressManager();
    return cypressManager->GetNodeProxy(trunkNode, Transaction);
}

void TNontemplateCypressNodeProxyBase::ValidatePermission(
    EPermissionCheckScope scope,
    EPermission permission,
    const TString& /* user */)
{
    auto* node = GetThisImpl();
    // NB: Suppress permission checks for nodes upon construction.
    // Cf. YT-1191, YT-4628.
    auto* trunkNode = node->GetTrunkNode();
    const auto& cypressManager = Bootstrap_->GetCypressManager();
    if (trunkNode == cypressManager->GetRootNode() || trunkNode->GetParent()) {
        ValidatePermission(node, scope, permission);
    }
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
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* trunkNode = node->GetTrunkNode();
        auto descendants = cypressManager->ListSubtreeNodes(trunkNode, Transaction, false);
        for (auto* descendant : descendants) {
            ValidatePermission(descendant, permission);
        }
    }
}

void TNontemplateCypressNodeProxyBase::ValidateNotExternal()
{
    if (TrunkNode->IsExternal()) {
        THROW_ERROR_EXCEPTION("Operation cannot be performed at an external node");
    }
}

void TNontemplateCypressNodeProxyBase::ValidateMediaChange(
    const std::optional<TChunkReplication>& oldReplication,
    std::optional<int> primaryMediumIndex,
    const TChunkReplication& newReplication)
{
    if (newReplication == oldReplication) {
        return;
    }

    const auto& chunkManager = Bootstrap_->GetChunkManager();

    for (const auto& entry : newReplication) {
        if (entry.Policy()) {
            auto* medium = chunkManager->GetMediumByIndex(entry.GetMediumIndex());
            ValidatePermission(medium, EPermission::Use);
        }
    }

    if (primaryMediumIndex && !newReplication.Get(*primaryMediumIndex)) {
        const auto* primaryMedium = chunkManager->GetMediumByIndex(*primaryMediumIndex);
        THROW_ERROR_EXCEPTION("Cannot remove primary medium %Qv",
            primaryMedium->GetName());
    }

    ValidateChunkReplication(chunkManager, newReplication, primaryMediumIndex);
}

bool TNontemplateCypressNodeProxyBase::ValidatePrimaryMediumChange(
    TMedium* newPrimaryMedium,
    const TChunkReplication& oldReplication,
    std::optional<int> oldPrimaryMediumIndex,
    TChunkReplication* newReplication)
{
    auto newPrimaryMediumIndex = newPrimaryMedium->GetIndex();
    if (newPrimaryMediumIndex == oldPrimaryMediumIndex) {
        return false;
    }

    ValidatePermission(newPrimaryMedium, EPermission::Use);

    auto copiedReplication = oldReplication;
    if (!copiedReplication.Get(newPrimaryMediumIndex) && oldPrimaryMediumIndex) {
        // The user is trying to set a medium with zero replication count
        // as primary. This is regarded as a request to move from one medium to
        // another.
        copiedReplication.Set(newPrimaryMediumIndex, copiedReplication.Get(*oldPrimaryMediumIndex));
        copiedReplication.Erase(*oldPrimaryMediumIndex);
    }

    const auto& chunkManager = Bootstrap_->GetChunkManager();
    ValidateChunkReplication(chunkManager, copiedReplication, newPrimaryMediumIndex);

    *newReplication = copiedReplication;

    return true;
}

void TNontemplateCypressNodeProxyBase::SetModified(EModificationType modificationType)
{
    if (TrunkNode->IsAlive() && !ModificationTrackingSuppressed) {
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        cypressManager->SetModified(TrunkNode, Transaction, modificationType);
    }
}

void TNontemplateCypressNodeProxyBase::SuppressModificationTracking()
{
    ModificationTrackingSuppressed = true;
}

void TNontemplateCypressNodeProxyBase::SetAccessed()
{
    if (TrunkNode->IsAlive()) {
        const auto& cypressManager = Bootstrap_->GetCypressManager();
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

void TNontemplateCypressNodeProxyBase::SetChildNode(
    INodeFactory* /*factory*/,
    const TYPath& /*path*/,
    const INodePtr& /*child*/,
    bool /*recursive*/)
{
    Y_UNREACHABLE();
}

DEFINE_YPATH_SERVICE_METHOD(TNontemplateCypressNodeProxyBase, Lock)
{
    DeclareMutating();

    auto mode = ELockMode(request->mode());
    bool waitable = request->waitable();

    if (mode != ELockMode::Snapshot &&
        mode != ELockMode::Shared &&
        mode != ELockMode::Exclusive)
    {
        THROW_ERROR_EXCEPTION("Invalid lock mode %Qlv",
            mode);
    }

    TLockRequest lockRequest;
    if (request->has_child_key()) {
        if (mode != ELockMode::Shared) {
            THROW_ERROR_EXCEPTION("Only %Qlv locks are allowed on child keys, got %Qlv",
                ELockMode::Shared,
                mode);
        }
        lockRequest = TLockRequest::MakeSharedChild(request->child_key());
    } else if (request->has_attribute_key()) {
        if (mode != ELockMode::Shared) {
            THROW_ERROR_EXCEPTION("Only %Qlv locks are allowed on attribute keys, got %Qlv",
                ELockMode::Shared,
                mode);
        }
        lockRequest = TLockRequest::MakeSharedAttribute(request->attribute_key());
    } else {
        lockRequest = TLockRequest(mode);
    }

    lockRequest.Timestamp = static_cast<TTimestamp>(request->timestamp());

    context->SetRequestInfo("Mode: %v, Key: %v, Waitable: %v",
        mode,
        lockRequest.Key,
        waitable);

    ValidateTransaction();
    ValidatePermission(
        EPermissionCheckScope::This,
        mode == ELockMode::Snapshot ? EPermission::Read : EPermission::Write);
    ValidateLockPossible();

    const auto& cypressManager = Bootstrap_->GetCypressManager();
    auto* lock = cypressManager->CreateLock(
        TrunkNode,
        Transaction,
        lockRequest,
        waitable);

    auto lockId = lock->GetId();
    ToProto(response->mutable_lock_id(), lockId);
    ToProto(response->mutable_node_id(), lock->GetTrunkNode()->GetId());
    response->set_cell_tag(TrunkNode->GetExternalCellTag() == NotReplicatedCellTag
        ? Bootstrap_->GetCellTag()
        : TrunkNode->GetExternalCellTag());

    context->SetResponseInfo("LockId: %v",
        lockId);

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TNontemplateCypressNodeProxyBase, Unlock)
{
    DeclareMutating();

    context->SetRequestInfo();

    ValidateTransaction();
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);

    const auto& cypressManager = Bootstrap_->GetCypressManager();
    cypressManager->UnlockNode(TrunkNode, Transaction);

    context->SetResponseInfo();

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TNontemplateCypressNodeProxyBase, Create)
{
    DeclareMutating();

    auto type = EObjectType(request->type());
    auto ignoreExisting = request->ignore_existing();
    auto recursive = request->recursive();
    auto force = request->force();
    const auto& path = GetRequestYPath(context->RequestHeader());

    context->SetRequestInfo("Type: %v, IgnoreExisting: %v, Recursive: %v, Force: %v",
        type,
        ignoreExisting,
        recursive,
        force);

    if (ignoreExisting && force) {
        THROW_ERROR_EXCEPTION("Cannot specify both \"ignore_existing\" and \"force\" options simultaneously");
    }

    bool replace = path.empty();
    if (replace && !force) {
        if (!ignoreExisting) {
            ThrowAlreadyExists(this);
        }

        const auto* impl = GetThisImpl();
        if (impl->GetType() != type && !force) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "%v already exists and has type %Qlv while node of %Qlv type is about to be created",
                GetPath(),
                impl->GetType(),
                type);
        }
        ToProto(response->mutable_node_id(), impl->GetId());
        response->set_cell_tag(impl->GetExternalCellTag() == NotReplicatedCellTag
            ? Bootstrap_->GetCellTag()
            : impl->GetExternalCellTag());
        context->SetResponseInfo("ExistingNodeId: %v",
            impl->GetId());
        context->Reply();
        return;
    }

    if (!replace && !CanHaveChildren()) {
        ThrowCannotHaveChildren(this);
    }

    ICompositeNodePtr parent;
    if (replace) {
        parent = GetParent();
        if (!parent) {
            ThrowCannotReplaceRoot();
        }
    }

    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);

    auto* node = GetThisImpl();
    auto* account = replace ? node->GetParent()->GetAccount() : node->GetAccount();

    TInheritedAttributeDictionary inheritedAttributes(Bootstrap_);
    GatherInheritableAttributes(
            replace ? node->GetParent() : node,
            &inheritedAttributes.Attributes());

    std::unique_ptr<IAttributeDictionary> explicitAttributes;
    if (request->has_node_attributes()) {
        explicitAttributes = FromProto(request->node_attributes());

        auto optionalAccount = explicitAttributes->FindAndRemove<TString>("account");
        if (optionalAccount) {
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            account = securityManager->GetAccountByNameOrThrow(*optionalAccount);
        }
    }

    auto factory = CreateCypressFactory(account, TNodeFactoryOptions());
    auto newProxy = factory->CreateNode(type, &inheritedAttributes, explicitAttributes.get());

    if (replace) {
        parent->ReplaceChild(this, newProxy);
    } else {
        SetChildNode(
            factory.get(),
            path,
            newProxy,
            recursive);
    }

    factory->Commit();

    auto* newNode = newProxy->GetTrunkNode();
    const auto& newNodeId = newNode->GetId();
    auto newNodeCellTag = newNode->GetExternalCellTag() == NotReplicatedCellTag
        ? Bootstrap_->GetCellTag()
        : newNode->GetExternalCellTag();

    ToProto(response->mutable_node_id(), newNode->GetId());
    response->set_cell_tag(newNodeCellTag);

    context->SetResponseInfo("NodeId: %v, CellTag: %v, Account: %v",
        newNodeId,
        newNodeCellTag,
        newNode->GetAccount()->GetName());

    context->Reply();
}

DEFINE_YPATH_SERVICE_METHOD(TNontemplateCypressNodeProxyBase, Copy)
{
    DeclareMutating();

    auto sourcePath = request->source_path();
    bool preserveAccount = request->preserve_account();
    bool preserveExpirationTime = request->preserve_expiration_time();
    bool preserveCreationTime = request->preserve_creation_time();
    bool removeSource = request->remove_source();
    auto recursive = request->recursive();
    auto ignoreExisting = request->ignore_existing();
    auto force = request->force();
    auto pessimisticQuotaCheck = request->pessimistic_quota_check();
    auto targetPath = GetRequestYPath(context->RequestHeader());

    context->SetRequestInfo("SourcePath: %v, TransactionId: %v "
        "PreserveAccount: %v, PreserveExpirationTime: %v, PreserveCreationTime: %v, "
        "RemoveSource: %v, Recursive: %v, IgnoreExisting: %v, Force: %v, PessimisticQuotaCheck: %v",
        sourcePath,
        Transaction ? Transaction->GetId() : TTransactionId(),
        preserveAccount,
        preserveExpirationTime,
        preserveCreationTime,
        removeSource,
        recursive,
        ignoreExisting,
        force,
        pessimisticQuotaCheck);

    if (ignoreExisting && force) {
        THROW_ERROR_EXCEPTION("Cannot specify both \"ignore_existing\" and \"force\" options simultaneously");
    }

    if (ignoreExisting && removeSource) {
        THROW_ERROR_EXCEPTION("Cannot specify both \"ignore_existing\" and \"remove_source\" options simultaneously");
    }

    bool replace = targetPath.empty();
    if (replace && !force) {
        if (!ignoreExisting) {
            ThrowAlreadyExists(this);
        }
        const auto* impl = GetThisImpl();
        ToProto(response->mutable_node_id(), impl->GetId());
        context->SetResponseInfo("ExistingNodeId: %v",
            impl->GetId());
        context->Reply();
        return;
    }

    if (!replace && !CanHaveChildren()) {
        ThrowCannotHaveChildren(this);
    }

    ICompositeNodePtr parent;
    if (replace) {
        parent = GetParent();
        if (!parent) {
            ThrowCannotReplaceRoot();
        }
    }

    const auto& cypressManager = Bootstrap_->GetCypressManager();
    auto sourceProxy = cypressManager->ResolvePathToNodeProxy(sourcePath, Transaction);

    auto* trunkSourceImpl = sourceProxy->GetTrunkNode();
    auto* sourceImpl = removeSource
        ? LockImpl(trunkSourceImpl, ELockMode::Exclusive, true)
        : cypressManager->GetVersionedNode(trunkSourceImpl, Transaction);

    if (IsAncestorOf(trunkSourceImpl, TrunkNode)) {
        THROW_ERROR_EXCEPTION("Cannot copy or move a node to its descendant");
    }

    if (replace) {
        ValidatePermission(EPermissionCheckScope::This | EPermissionCheckScope::Descendants, EPermission::Remove);
        ValidatePermission(EPermissionCheckScope::Parent, EPermission::Write);
    } else {
        ValidatePermission(EPermissionCheckScope::This, EPermission::Write);
    }

    ValidatePermission(sourceImpl, EPermissionCheckScope::This | EPermissionCheckScope::Descendants, EPermission::Read);

    auto sourceParent = sourceProxy->GetParent();
    if (removeSource) {
        // Cf. TNodeBase::RemoveSelf
        if (!sourceParent) {
            ThrowCannotRemoveRoot();
        }
        ValidatePermission(sourceImpl, EPermissionCheckScope::This | EPermissionCheckScope::Descendants, EPermission::Remove);
        ValidatePermission(sourceImpl, EPermissionCheckScope::Parent, EPermission::Write);
    }

    auto* account = replace
        ? ICypressNodeProxy::FromNode(parent.Get())->GetTrunkNode()->GetAccount()
        : GetThisImpl()->GetAccount();

    TNodeFactoryOptions options;
    options.PreserveAccount = preserveAccount;
    options.PreserveExpirationTime = preserveExpirationTime;
    options.PreserveCreationTime = preserveCreationTime;
    options.PessimisticQuotaCheck = pessimisticQuotaCheck;
    auto factory = CreateCypressFactory(account, options);

    auto* clonedImpl = factory->CloneNode(
        sourceImpl,
        removeSource ? ENodeCloneMode::Move : ENodeCloneMode::Copy);
    auto* clonedTrunkImpl = clonedImpl->GetTrunkNode();
    auto clonedProxy = GetProxy(clonedTrunkImpl);

    if (replace) {
        parent->ReplaceChild(this, clonedProxy);
    } else {
        SetChildNode(
            factory.get(),
            targetPath,
            clonedProxy,
            recursive);
    }

    if (removeSource) {
        sourceParent->RemoveChild(sourceProxy);
    }

    factory->Commit();

    ToProto(response->mutable_node_id(), clonedTrunkImpl->GetId());

    context->SetResponseInfo("NodeId: %v", clonedTrunkImpl->GetId());

    context->Reply();
}

////////////////////////////////////////////////////////////////////////////////

TIntrusivePtr<const ICompositeNode> TNontemplateCompositeCypressNodeProxyBase::AsComposite() const
{
    return this;
}

TIntrusivePtr<ICompositeNode> TNontemplateCompositeCypressNodeProxyBase::AsComposite()
{
    return this;
}

void TNontemplateCompositeCypressNodeProxyBase::ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors)
{
    TNontemplateCypressNodeProxyBase::ListSystemAttributes(descriptors);

    const auto* node = GetThisImpl<TCompositeNodeBase>();

    descriptors->push_back(EInternedAttributeKey::Count);

    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::CompressionCodec)
        .SetPresent(node->GetCompressionCodec().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::ErasureCodec)
        .SetPresent(node->GetErasureCodec().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::PrimaryMedium)
        .SetPresent(node->GetPrimaryMediumIndex().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Media)
        .SetPresent(node->GetMedia().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Vital)
        .SetPresent(node->GetVital().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::ReplicationFactor)
        .SetPresent(node->GetReplicationFactor().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::TabletCellBundle)
        .SetPresent(node->GetTabletCellBundle())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Atomicity)
        .SetPresent(node->GetAtomicity().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::CommitOrdering)
        .SetPresent(node->GetCommitOrdering().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::InMemoryMode)
        .SetPresent(node->GetInMemoryMode().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::OptimizeFor)
        .SetPresent(node->GetOptimizeFor().operator bool())
        .SetWritable(true)
        .SetRemovable(true));
}

bool TNontemplateCompositeCypressNodeProxyBase::GetBuiltinAttribute(TInternedAttributeKey key, IYsonConsumer* consumer)
{
    const auto* node = GetThisImpl<TCompositeNodeBase>();

    switch (key) {
        case EInternedAttributeKey::Count:
            BuildYsonFluently(consumer)
                .Value(GetChildCount());
            return true;

#define XX(camelCaseName, snakeCaseName) \
        case EInternedAttributeKey::camelCaseName: \
            if (!node->Get##camelCaseName()) { \
                break; \
            } \
            BuildYsonFluently(consumer) \
                .Value(node->Get##camelCaseName()); \
            return true; \

        FOR_EACH_SIMPLE_INHERITABLE_ATTRIBUTE(XX)
#undef XX

        case EInternedAttributeKey::PrimaryMedium: {
            if (!node->GetPrimaryMediumIndex()) {
                break;
            }
            const auto& chunkManager = Bootstrap_->GetChunkManager();
            auto* medium = chunkManager->GetMediumByIndex(*node->GetPrimaryMediumIndex());
            BuildYsonFluently(consumer)
                .Value(medium->GetName());
            return true;
        }

        case EInternedAttributeKey::Media: {
            if (!node->GetMedia()) {
                break;
            }
            const auto& chunkManager = Bootstrap_->GetChunkManager();
            BuildYsonFluently(consumer)
                .Value(TSerializableChunkReplication(*node->GetMedia(), chunkManager));
            return true;
        }

        case EInternedAttributeKey::TabletCellBundle:
            if (!node->GetTabletCellBundle()) {
                break;
            }
            BuildYsonFluently(consumer)
                .Value(node->GetTabletCellBundle()->GetName());
            return true;

        default:
            break;
    }

    return TNontemplateCypressNodeProxyBase::GetBuiltinAttribute(key, consumer);
}

bool TNontemplateCompositeCypressNodeProxyBase::SetBuiltinAttribute(TInternedAttributeKey key, const TYsonString& value)
{
    auto* node = GetThisImpl<TCompositeNodeBase>();

    // Attributes "media", "primary_medium", "replication_factor" are interrelated
    // and nullable, which greatly complicates their modification.
    //
    // The rule of thumb is: if possible, consistency of non-null attributes is
    // checked, but an attribute is never required to be set just for the
    // purposes of validation of other attributes. For instance: "media" and
    // "replication_factor" are checked for consistency only when "primary_medium" is
    // set. Without it, it's impossible to tell which medium the "replication_factor"
    // pertains to, and these attributes may be modified virtually independently.

    const auto& chunkManager = Bootstrap_->GetChunkManager();

    auto throwReplicationFactorMismatch = [&] (int mediumIndex) {
        const auto& medium = chunkManager->GetMediumByIndexOrThrow(mediumIndex);
        THROW_ERROR_EXCEPTION(
            "Attributes \"media\" and \"replication_factor\" have contradicting values for medium %Qv",
            medium->GetName());
    };

    switch (key) {
        case EInternedAttributeKey::PrimaryMedium: {
            ValidateNoTransaction();

            auto mediumName = ConvertTo<TString>(value);
            auto* medium = chunkManager->GetMediumByNameOrThrow(mediumName);
            const auto mediumIndex = medium->GetIndex();
            const auto replication = node->GetMedia();

            if (!replication) {
                ValidatePermission(medium, EPermission::Use);
                node->SetPrimaryMediumIndex(mediumIndex);
                return true;
            }

            TChunkReplication newReplication;
            if (ValidatePrimaryMediumChange(
                medium,
                *replication,
                node->GetPrimaryMediumIndex(), // may be null
                &newReplication))
            {
                const auto replicationFactor = node->GetReplicationFactor();
                if (replicationFactor &&
                    *replicationFactor != newReplication.Get(mediumIndex).GetReplicationFactor())
                {
                    throwReplicationFactorMismatch(mediumIndex);
                }

                node->SetMedia(newReplication);
                node->SetPrimaryMediumIndex(mediumIndex);
            } // else no change is required

            return true;
        }

        case EInternedAttributeKey::Media: {
            ValidateNoTransaction();

            auto serializableReplication = ConvertTo<TSerializableChunkReplication>(value);
            TChunkReplication replication;
            // Vitality isn't a part of TSerializableChunkReplication, assume true.
            replication.SetVital(true);
            serializableReplication.ToChunkReplication(&replication, chunkManager);

            const auto oldReplication = node->GetMedia();

            if (replication == oldReplication) {
                return true;
            }

            const auto primaryMediumIndex = node->GetPrimaryMediumIndex();
            const auto replicationFactor = node->GetReplicationFactor();
            if (primaryMediumIndex && replicationFactor) {
                if (replication.Get(*primaryMediumIndex).GetReplicationFactor() != *replicationFactor) {
                    throwReplicationFactorMismatch(*primaryMediumIndex);
                }
            }

            // NB: primary medium index may be null, in which case corresponding
            // parts of validation will be skipped.
            ValidateMediaChange(oldReplication, primaryMediumIndex, replication);
            node->SetMedia(replication);

            return true;
        }

        case EInternedAttributeKey::ReplicationFactor: {
            ValidateNoTransaction();

            auto replicationFactor = ConvertTo<int>(value);
            if (replicationFactor == node->GetReplicationFactor()) {
                return true;
            }

            if (replicationFactor == 0) {
                THROW_ERROR_EXCEPTION("Inheritable replication factor must not be zero; consider removing the attribute altogether");
            }

            ValidateReplicationFactor(replicationFactor);

            const auto mediumIndex = node->GetPrimaryMediumIndex();
            if (mediumIndex) {
                const auto replication = node->GetMedia();
                if (replication) {
                    if (replication->Get(*mediumIndex).GetReplicationFactor() != replicationFactor) {
                        throwReplicationFactorMismatch(*mediumIndex);
                    }
                } else if (!node->GetReplicationFactor()) {
                    auto* medium = chunkManager->GetMediumByIndex(*mediumIndex);
                    ValidatePermission(medium, EPermission::Use);
                }
            }

            node->SetReplicationFactor(replicationFactor);

            return true;
        }

        case EInternedAttributeKey::TabletCellBundle: {
            ValidateNoTransaction();

            auto name = ConvertTo<TString>(value);

            auto* oldBundle = node->GetTabletCellBundle();
            const auto& tabletManager = Bootstrap_->GetTabletManager();
            auto* newBundle = tabletManager->GetTabletCellBundleByNameOrThrow(name);

            if (oldBundle == newBundle) {
                return true;
            }

            const auto& objectManager = Bootstrap_->GetObjectManager();

            if (oldBundle) {
                objectManager->UnrefObject(oldBundle);
            }

            node->SetTabletCellBundle(newBundle);
            objectManager->RefObject(newBundle);

            return true;
        }

#define XX(camelCaseName, snakeCaseName) \
        case EInternedAttributeKey::camelCaseName: \
            ValidateNoTransaction(); \
            node->Set##camelCaseName(ConvertTo<decltype(node->Get##camelCaseName())>(value)); \
            return true; \

        // Can't use FOR_EACH_SIMPLE_INHERITABLE_ATTRIBUTE here as
        // replication_factor is "simple" yet must be handled separately.
        XX(CompressionCodec, compression_codec)
        XX(ErasureCodec, erasure_codec)
        XX(Vital, vital)
        XX(Atomicity, atomicity)
        XX(CommitOrdering, commit_ordering)
        XX(InMemoryMode, in_memory_mode)
        XX(OptimizeFor, optimize_for)
#undef XX

        default:
            break;
    }

    return TNontemplateCypressNodeProxyBase::SetBuiltinAttribute(key, value);
}

bool TNontemplateCompositeCypressNodeProxyBase::RemoveBuiltinAttribute(TInternedAttributeKey key)
{
    auto* node = GetThisImpl<TCompositeNodeBase>();

    switch (key) {

#define XX(camelCaseName, snakeCaseName) \
        case EInternedAttributeKey::camelCaseName: \
            ValidateNoTransaction(); \
            node->Set##camelCaseName(std::nullopt); \
            return true; \

        FOR_EACH_SIMPLE_INHERITABLE_ATTRIBUTE(XX);
        XX(Media, media);
#undef XX

        case EInternedAttributeKey::PrimaryMedium:
            ValidateNoTransaction();
            node->SetPrimaryMediumIndex(std::nullopt);
            return true;

        case EInternedAttributeKey::TabletCellBundle: {
            ValidateNoTransaction();

            auto* bundle = node->GetTabletCellBundle();
            if (bundle) {
                const auto& objectManager = Bootstrap_->GetObjectManager();
                objectManager->UnrefObject(bundle);
                node->SetTabletCellBundle(nullptr);
            }

            return true;
        }

        default:
            break;
    }

    return TNontemplateCypressNodeProxyBase::RemoveBuiltinAttribute(key);
}

bool TNontemplateCompositeCypressNodeProxyBase::CanHaveChildren() const
{
    return true;
}

////////////////////////////////////////////////////////////////////////////////

TInheritedAttributeDictionary::TInheritedAttributeDictionary(TBootstrap* bootstrap)
    : Bootstrap_(bootstrap)
{ }

std::vector<TString> TInheritedAttributeDictionary::List() const
{
    std::vector<TString> result;
#define XX(camelCaseName, snakeCaseName) \
    if (InheritedAttributes_.camelCaseName) { \
        result.push_back(#snakeCaseName); \
    }

    FOR_EACH_INHERITABLE_ATTRIBUTE(XX)

#undef XX

    if (Fallback_) {
        auto fallbackList = Fallback_->List();
        result.insert(result.end(), fallbackList.begin(), fallbackList.end());
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
    }

    return result;
}

TYsonString TInheritedAttributeDictionary::FindYson(const TString& key) const
{
#define XX(camelCaseName, snakeCaseName) \
    if (key == #snakeCaseName) { \
        const auto& value = InheritedAttributes_.camelCaseName; \
        return value ? ConvertToYsonString(*value) : TYsonString(); \
    }

    FOR_EACH_SIMPLE_INHERITABLE_ATTRIBUTE(XX);

#undef XX

    if (key == "primary_medium") {
        const auto& primaryMediumIndex = InheritedAttributes_.PrimaryMediumIndex;
        if (!primaryMediumIndex) {
            return TYsonString();
        }
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* medium = chunkManager->GetMediumByIndex(*primaryMediumIndex);
        return ConvertToYsonString(medium->GetName());
    }

    if (key == "media") {
        const auto& replication = InheritedAttributes_.Media;
        if (!replication) {
            return TYsonString();
        }
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        return ConvertToYsonString(TSerializableChunkReplication(*replication, chunkManager));
    }

    if (key == "tablet_cell_bundle") {
        auto* tabletCellBundle = InheritedAttributes_.TabletCellBundle;
        if (!tabletCellBundle) {
            return TYsonString();
        }
        return ConvertToYsonString(tabletCellBundle->GetName());
    }

    return Fallback_ ? Fallback_->FindYson(key) : TYsonString();
}

void TInheritedAttributeDictionary::SetYson(const TString& key, const NYson::TYsonString& value)
{
#define XX(camelCaseName, snakeCaseName) \
    if (key == #snakeCaseName) { \
        auto& attr = InheritedAttributes_.camelCaseName; \
        using TAttr = std::remove_reference<decltype(*attr)>::type; \
        attr = ConvertTo<TAttr>(value); \
        return; \
    }

    FOR_EACH_SIMPLE_INHERITABLE_ATTRIBUTE(XX);

#undef XX

    if (key == "primary_medium") {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto& mediumName = ConvertTo<TString>(value);
        auto* medium = chunkManager->GetMediumByNameOrThrow(mediumName);
        InheritedAttributes_.PrimaryMediumIndex = medium->GetIndex();
        return;
    }

    if (key == "media") {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto serializableReplication = ConvertTo<TSerializableChunkReplication>(value);
        TChunkReplication replication;
        replication.SetVital(true);
        serializableReplication.ToChunkReplication(&replication, chunkManager);

        InheritedAttributes_.Media = replication;
        return;
    }

    if (key == "tablet_cell_bundle") {
        auto bundleName = ConvertTo<TString>(value);
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        auto* bundle = tabletManager->GetTabletCellBundleByNameOrThrow(bundleName);
        InheritedAttributes_.TabletCellBundle = bundle;
        return;
    }

    if (!Fallback_) {
        Fallback_ = CreateEphemeralAttributes();
    }

    Fallback_->SetYson(key, value);
}

bool TInheritedAttributeDictionary::Remove(const TString& key)
{
#define XX(camelCaseName, snakeCaseName) \
    if (key == #snakeCaseName) { \
        if (InheritedAttributes_.camelCaseName) { \
            InheritedAttributes_.camelCaseName = decltype(InheritedAttributes_.camelCaseName)(); \
        } \
        return true; \
    }

    FOR_EACH_INHERITABLE_ATTRIBUTE(XX);

#undef XX

    if (Fallback_) {
        return Fallback_->Remove(key);
    }

    return false;
}

TCompositeNodeBase::TAttributes& TInheritedAttributeDictionary::Attributes()
{
    return InheritedAttributes_;
}

////////////////////////////////////////////////////////////////////////////////

void TMapNodeProxy::SetRecursive(
    const TYPath& path,
    TReqSet* request,
    TRspSet* response,
    const TCtxSetPtr& context)
{
    context->SetRequestInfo();
    ValidateSetCommand(GetPath(), context->GetUser(), request->force());
    TMapNodeMixin::SetRecursive(path, request, response, context);
}

void TMapNodeProxy::Clear()
{
    // Take shared lock for the node itself.
    auto* impl = LockThisImpl(ELockMode::Shared);

    // Construct children list.
    THashMap<TString, TCypressNodeBase*> keyToChildMapStorage;
    const auto& keyToChildMap = GetMapNodeChildMap(
        Bootstrap_->GetCypressManager(),
        TrunkNode,
        Transaction,
        &keyToChildMapStorage);
    auto keyToChildList = SortKeyToChild(keyToChildMap);

    // Take shared locks for children.
    typedef std::pair<TString, TCypressNodeBase*> TChild;
    std::vector<TChild> children;
    children.reserve(keyToChildList.size());
    for (const auto& pair : keyToChildList) {
        LockThisImpl(TLockRequest::MakeSharedChild(pair.first));
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
    const auto& cypressManager = Bootstrap_->GetCypressManager();
    auto originators = cypressManager->GetNodeOriginators(Transaction, TrunkNode);

    int result = 0;
    for (const auto* node : originators) {
        const auto* mapNode = node->As<TMapNode>();
        result += mapNode->ChildCountDelta();
    }
    return result;
}

std::vector<std::pair<TString, INodePtr>> TMapNodeProxy::GetChildren() const
{
    THashMap<TString, TCypressNodeBase*> keyToChildStorage;
    const auto& keyToChildMap = GetMapNodeChildMap(
        Bootstrap_->GetCypressManager(),
        TrunkNode,
        Transaction,
        &keyToChildStorage);

    std::vector<std::pair<TString, INodePtr>> result;
    result.reserve(keyToChildMap.size());
    for (const auto& pair : keyToChildMap) {
        result.push_back(std::make_pair(pair.first, GetProxy(pair.second)));
    }

    return result;
}

std::vector<TString> TMapNodeProxy::GetKeys() const
{
    THashMap<TString, TCypressNodeBase*> keyToChildStorage;
    const auto& keyToChildMap = GetMapNodeChildMap(
        Bootstrap_->GetCypressManager(),
        TrunkNode,
        Transaction,
        &keyToChildStorage);

    std::vector<TString> result;
    for (const auto& pair : keyToChildMap) {
        result.push_back(pair.first);
    }

    return result;
}

INodePtr TMapNodeProxy::FindChild(const TString& key) const
{
    auto* childTrunkNode = FindMapNodeChild(
        Bootstrap_->GetCypressManager(),
        TrunkNode,
        Transaction,
        key);
    return childTrunkNode ? GetProxy(childTrunkNode) : nullptr;
}

bool TMapNodeProxy::AddChild(const TString& key, const NYTree::INodePtr& child)
{
    Y_ASSERT(!key.empty());

    if (FindChild(key)) {
        return false;
    }

    auto* impl = LockThisImpl(TLockRequest::MakeSharedChild(key));
    auto* trunkChildImpl = ICypressNodeProxy::FromNode(child.Get())->GetTrunkNode();
    auto* childImpl = LockImpl(trunkChildImpl);

    impl->KeyToChild()[key] = trunkChildImpl;
    YCHECK(impl->ChildToKey().insert(std::make_pair(trunkChildImpl, key)).second);
    ++impl->ChildCountDelta();

    AttachChild(Bootstrap_->GetObjectManager(), TrunkNode, childImpl);

    SetModified();

    return true;
}

bool TMapNodeProxy::RemoveChild(const TString& key)
{
    auto* trunkChildImpl = FindMapNodeChild(
        Bootstrap_->GetCypressManager(),
        TrunkNode,
        Transaction,
        key);
    if (!trunkChildImpl) {
        return false;
    }

    auto* childImpl = LockImpl(trunkChildImpl, ELockMode::Exclusive, true);
    auto* impl = LockThisImpl(TLockRequest::MakeSharedChild(key));
    DoRemoveChild(impl, key, childImpl);

    SetModified();

    return true;
}

void TMapNodeProxy::RemoveChild(const INodePtr& child)
{
    auto optionalKey = FindChildKey(child);
    if (!optionalKey) {
        THROW_ERROR_EXCEPTION("Node is not a child");
    }
    const auto& key = *optionalKey;

    auto* trunkChildImpl = ICypressNodeProxy::FromNode(child.Get())->GetTrunkNode();

    auto* childImpl = LockImpl(trunkChildImpl, ELockMode::Exclusive, true);
    auto* impl = LockThisImpl(TLockRequest::MakeSharedChild(key));
    DoRemoveChild(impl, key, childImpl);

    SetModified();
}

void TMapNodeProxy::ReplaceChild(const INodePtr& oldChild, const INodePtr& newChild)
{
    if (oldChild == newChild) {
        return;
    }

    auto optionalKey = FindChildKey(oldChild);
    if (!optionalKey) {
        THROW_ERROR_EXCEPTION("Node is not a child");
    }
    const auto& key = *optionalKey;

    auto* oldTrunkChildImpl = ICypressNodeProxy::FromNode(oldChild.Get())->GetTrunkNode();
    auto* oldChildImpl = LockImpl(oldTrunkChildImpl, ELockMode::Exclusive, true);

    auto* newTrunkChildImpl = ICypressNodeProxy::FromNode(newChild.Get())->GetTrunkNode();
    auto* newChildImpl = LockImpl(newTrunkChildImpl);

    auto* impl = LockThisImpl(TLockRequest::MakeSharedChild(key));

    auto& keyToChild = impl->KeyToChild();
    auto& childToKey = impl->ChildToKey();

    bool ownsOldChild = keyToChild.find(key) != keyToChild.end();
    const auto& objectManager = Bootstrap_->GetObjectManager();
    DetachChild(objectManager, TrunkNode, oldChildImpl, ownsOldChild);

    keyToChild[key] = newTrunkChildImpl;
    childToKey.erase(oldTrunkChildImpl);
    YCHECK(childToKey.insert(std::make_pair(newTrunkChildImpl, key)).second);
    AttachChild(objectManager, TrunkNode, newChildImpl);

    SetModified();
}

std::optional<TString> TMapNodeProxy::FindChildKey(const IConstNodePtr& child)
{
    auto* trunkChildImpl = ICypressNodeProxy::FromNode(child.Get())->GetTrunkNode();

    const auto& cypressManager = Bootstrap_->GetCypressManager();
    auto originators = cypressManager->GetNodeOriginators(Transaction, TrunkNode);

    for (const auto* node : originators) {
        const auto* mapNode = node->As<TMapNode>();
        auto it = mapNode->ChildToKey().find(trunkChildImpl);
        if (it != mapNode->ChildToKey().end()) {
            return it->second;
        }
    }

    return std::nullopt;
}

bool TMapNodeProxy::DoInvoke(const NRpc::IServiceContextPtr& context)
{
    DISPATCH_YPATH_SERVICE_METHOD(List);
    return TBase::DoInvoke(context);
}

void TMapNodeProxy::SetChildNode(
    INodeFactory* factory,
    const TYPath& path,
    const INodePtr& child,
    bool recursive)
{
    TMapNodeMixin::SetChild(
        factory,
        path,
        child,
        recursive);
}

int TMapNodeProxy::GetMaxChildCount() const
{
    return Bootstrap_->GetConfig()->CypressManager->MaxNodeChildCount;
}

int TMapNodeProxy::GetMaxKeyLength() const
{
    return Bootstrap_->GetConfig()->CypressManager->MaxMapNodeKeyLength;
}

IYPathService::TResolveResult TMapNodeProxy::ResolveRecursive(
    const TYPath& path,
    const IServiceContextPtr& context)
{
    return TMapNodeMixin::ResolveRecursive(path, context);
}

void TMapNodeProxy::DoRemoveChild(
    TMapNode* impl,
    const TString& key,
    TCypressNodeBase* childImpl)
{
    auto* trunkChildImpl = childImpl->GetTrunkNode();
    auto& keyToChild = impl->KeyToChild();
    auto& childToKey = impl->ChildToKey();
    const auto& objectManager = Bootstrap_->GetObjectManager();
    if (Transaction) {
        auto it = keyToChild.find(key);
        if (it == keyToChild.end()) {
            YCHECK(keyToChild.insert(std::make_pair(key, nullptr)).second);
            DetachChild(objectManager, TrunkNode, childImpl, false);
        } else {
            it->second = nullptr;
            YCHECK(childToKey.erase(trunkChildImpl) == 1);
            DetachChild(objectManager, TrunkNode, childImpl, true);
        }
    } else {
        YCHECK(keyToChild.erase(key) == 1);
        YCHECK(childToKey.erase(trunkChildImpl) == 1);
        DetachChild(objectManager, TrunkNode, childImpl, true);
    }
    --impl->ChildCountDelta();
}

void TMapNodeProxy::ListSelf(
    TReqList* request,
    TRspList* response,
    const TCtxListPtr& context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);

    auto attributeKeys = request->has_attributes()
        ? std::make_optional(FromProto<std::vector<TString>>(request->attributes().keys()))
        : std::nullopt;

    auto limit = request->has_limit()
        ? std::make_optional(request->limit())
        : std::nullopt;

    context->SetRequestInfo("AttributeKeys: %v, Limit: %v",
        attributeKeys,
        limit);

    TAsyncYsonWriter writer;

    const auto& cypressManager = Bootstrap_->GetCypressManager();
    const auto& securityManager = Bootstrap_->GetSecurityManager();

    THashMap<TString, TCypressNodeBase*> keyToChildMapStorage;
    const auto& keyToChildMap = GetMapNodeChildMap(
        cypressManager,
        TrunkNode,
        Transaction,
        &keyToChildMapStorage);

    if (limit && keyToChildMap.size() > *limit) {
        writer.OnBeginAttributes();
        writer.OnKeyedItem("incomplete");
        writer.OnBooleanScalar(true);
        writer.OnEndAttributes();
    }

    i64 counter = 0;

    writer.OnBeginList();
    for (const auto& pair : keyToChildMap) {
        const auto& key = pair.first;
        auto* trunkChild  = pair.second;
        writer.OnListItem();

        if (CheckItemReadPermissions(TrunkNode, trunkChild, securityManager)) {
            auto proxy = cypressManager->GetNodeProxy(trunkChild, Transaction);
            proxy->WriteAttributes(&writer, attributeKeys, false);
        }

        writer.OnStringScalar(key);

        if (limit && ++counter >= *limit) {
            break;
        }
    }
    writer.OnEndList();

    writer.Finish().Subscribe(BIND([=] (const TErrorOr<TYsonString>& resultOrError) {
        if (resultOrError.IsOK()) {
            response->set_value(resultOrError.Value().GetData());
            context->Reply();
        } else {
            context->Reply(resultOrError);
        }
    }));
}

////////////////////////////////////////////////////////////////////////////////

void TListNodeProxy::SetRecursive(
    const TYPath& path,
    TReqSet* request,
    TRspSet* response,
    const TCtxSetPtr& context)
{
    context->SetRequestInfo();

    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();
    auto token = tokenizer.GetToken();

    if (!token.StartsWith(ListBeginToken) &&
        !token.StartsWith(ListEndToken) &&
        !token.StartsWith(ListBeforeToken) &&
        !token.StartsWith(ListAfterToken))
    {
        ValidateSetCommand(GetPath(), context->GetUser(), request->force());
    }
    TListNodeMixin::SetRecursive(path, request, response, context);
}

void TListNodeProxy::Clear()
{
    auto* impl = LockThisImpl();

    // Lock children and collect impls.
    std::vector<TCypressNodeBase*> children;
    for (auto* trunkChild : impl->IndexToChild()) {
        children.push_back(LockImpl(trunkChild));
    }

    // Detach children.
    for (auto* child : children) {
        DetachChild(Bootstrap_->GetObjectManager(), TrunkNode, child, true);
    }

    impl->IndexToChild().clear();
    impl->ChildToIndex().clear();

    SetModified();
}

int TListNodeProxy::GetChildCount() const
{
    const auto* impl = GetThisImpl();
    return impl->IndexToChild().size();
}

std::vector<INodePtr> TListNodeProxy::GetChildren() const
{
    std::vector<INodePtr> result;
    const auto* impl = GetThisImpl();
    const auto& indexToChild = impl->IndexToChild();
    result.reserve(indexToChild.size());
    for (auto* child : indexToChild) {
        result.push_back(GetProxy(child));
    }
    return result;
}

INodePtr TListNodeProxy::FindChild(int index) const
{
    const auto* impl = GetThisImpl();
    const auto& indexToChild = impl->IndexToChild();
    return index >= 0 && index < indexToChild.size() ? GetProxy(indexToChild[index]) : nullptr;
}

void TListNodeProxy::AddChild(const INodePtr& child, int beforeIndex /*= -1*/)
{
    auto* impl = LockThisImpl();
    auto& list = impl->IndexToChild();

    auto* trunkChildImpl = ICypressNodeProxy::FromNode(child.Get())->GetTrunkNode();
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

    AttachChild(Bootstrap_->GetObjectManager(), TrunkNode, childImpl);

    SetModified();
}

bool TListNodeProxy::RemoveChild(int index)
{
    auto* impl = LockThisImpl();
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
    DetachChild(Bootstrap_->GetObjectManager(), TrunkNode, childImpl, true);

    SetModified();
    return true;
}

void TListNodeProxy::RemoveChild(const INodePtr& child)
{
    int index = GetChildIndexOrThrow(child);
    YCHECK(RemoveChild(index));
}

void TListNodeProxy::ReplaceChild(const INodePtr& oldChild, const INodePtr& newChild)
{
    if (oldChild == newChild)
        return;

    auto* impl = LockThisImpl();

    auto* oldTrunkChildImpl = ICypressNodeProxy::FromNode(oldChild.Get())->GetTrunkNode();
    auto* oldChildImpl = LockImpl(oldTrunkChildImpl);

    auto* newTrunkChildImpl = ICypressNodeProxy::FromNode(newChild.Get())->GetTrunkNode();
    auto* newChildImpl = LockImpl(newTrunkChildImpl);

    auto it = impl->ChildToIndex().find(oldTrunkChildImpl);
    Y_ASSERT(it != impl->ChildToIndex().end());

    int index = it->second;

    const auto& objectManager = Bootstrap_->GetObjectManager();
    DetachChild(objectManager, TrunkNode, oldChildImpl, true);

    impl->IndexToChild()[index] = newTrunkChildImpl;
    impl->ChildToIndex().erase(it);
    YCHECK(impl->ChildToIndex().insert(std::make_pair(newTrunkChildImpl, index)).second);
    AttachChild(objectManager, TrunkNode, newChildImpl);

    SetModified();
}

std::optional<int> TListNodeProxy::FindChildIndex(const IConstNodePtr& child)
{
    const auto* impl = GetThisImpl();

    auto* trunkChildImpl = ICypressNodeProxy::FromNode(child.Get())->GetTrunkNode();

    auto it = impl->ChildToIndex().find(trunkChildImpl);
    return it == impl->ChildToIndex().end() ? std::nullopt : std::make_optional(it->second);
}

void TListNodeProxy::SetChildNode(
    INodeFactory* factory,
    const TYPath& path,
    const INodePtr& child,
    bool recursive)
{
    TListNodeMixin::SetChild(
        factory,
        path,
        child,
        recursive);
}

int TListNodeProxy::GetMaxChildCount() const
{
    return Bootstrap_->GetConfig()->CypressManager->MaxNodeChildCount;
}

IYPathService::TResolveResult TListNodeProxy::ResolveRecursive(
    const TYPath& path,
    const IServiceContextPtr& context)
{
    return TListNodeMixin::ResolveRecursive(path, context);
}

////////////////////////////////////////////////////////////////////////////////

TLinkNodeProxy::TLinkNodeProxy(
    TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTransaction* transaction,
    TLinkNode* trunkNode)
    : TBase(
        bootstrap,
        metadata,
        transaction,
        trunkNode)
{ }

IYPathService::TResolveResult TLinkNodeProxy::Resolve(
    const TYPath& path,
    const IServiceContextPtr& context)
{
    auto propagate = [&] () {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto* impl = GetThisImpl();
        auto combinedPath = impl->GetTargetPath() + path;
        return TResolveResultThere{objectManager->GetRootService(), std::move(combinedPath)};
    };

    const auto& method = context->GetMethod();
    NYPath::TTokenizer tokenizer(path);
    switch (tokenizer.Advance()) {
        case NYPath::ETokenType::Ampersand:
            return TBase::Resolve(TYPath(tokenizer.GetSuffix()), context);

        case NYPath::ETokenType::EndOfStream: {
            // NB: Always handle mutating Cypress verbs locally.
            if (method == "Remove" ||
                method == "Create" ||
                method == "Copy")
            {
                return TResolveResultHere{path};
            } else {
                return propagate();
            }
        }

        default:
            return propagate();
    }
}

void TLinkNodeProxy::ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors)
{
    TBase::ListSystemAttributes(descriptors);

    descriptors->push_back(EInternedAttributeKey::TargetPath);
    descriptors->push_back(EInternedAttributeKey::Broken);
}

bool TLinkNodeProxy::GetBuiltinAttribute(TInternedAttributeKey key, IYsonConsumer* consumer)
{
    switch (key) {
        case EInternedAttributeKey::TargetPath: {
            const auto* impl = GetThisImpl();
            BuildYsonFluently(consumer)
                .Value(impl->GetTargetPath());
            return true;
        }

        case EInternedAttributeKey::Broken:
            BuildYsonFluently(consumer)
                .Value(IsBroken());
            return true;

        default:
            break;
    }

    return TBase::GetBuiltinAttribute(key, consumer);
}

bool TLinkNodeProxy::IsBroken() const
{
    try {
        const auto* impl = GetThisImpl();
        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->ResolvePathToObject(impl->GetTargetPath(), Transaction);
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

////////////////////////////////////////////////////////////////////////////////

TDocumentNodeProxy::TDocumentNodeProxy(
    TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTransaction* transaction,
    TDocumentNode* trunkNode)
    : TBase(
        bootstrap,
        metadata,
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

IYPathService::TResolveResult TDocumentNodeProxy::ResolveRecursive(
    const TYPath& path,
    const IServiceContextPtr& /*context*/)
{
    return TResolveResultHere{"/" + path};
}

namespace {

template <class TServerRequest, class TServerResponse, class TContext>
bool DelegateInvocation(
    IYPathServicePtr service,
    TServerRequest* serverRequest,
    TServerResponse* serverResponse,
    TIntrusivePtr<TContext> context)
{
    typedef typename TServerRequest::TMessage  TRequestMessage;
    typedef typename TServerResponse::TMessage TResponseMessage;

    typedef TTypedYPathRequest<TRequestMessage, TResponseMessage>  TClientRequest;

    auto clientRequest = New<TClientRequest>(context->RequestHeader());
    clientRequest->MergeFrom(*serverRequest);

    auto clientResponseOrError = ExecuteVerb(service, clientRequest).Get();

    if (clientResponseOrError.IsOK()) {
        const auto& clientResponse = clientResponseOrError.Value();
        serverResponse->MergeFrom(*clientResponse);
        context->Reply();
        return true;
    } else {
        context->Reply(clientResponseOrError);
        return false;
    }
}

} // namespace

void TDocumentNodeProxy::GetSelf(
    TReqGet* request,
    TRspGet* response,
    const TCtxGetPtr& context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::GetRecursive(
    const TYPath& /*path*/,
    TReqGet* request,
    TRspGet* response,
    const TCtxGetPtr& context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::SetSelf(
    TReqSet* request,
    TRspSet* /*response*/,
    const TCtxSetPtr& context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);
    SetImplValue(TYsonString(request->value()));
    context->Reply();
}

void TDocumentNodeProxy::SetRecursive(
    const TYPath& /*path*/,
    TReqSet* request,
    TRspSet* response,
    const TCtxSetPtr& context)
{
    context->SetRequestInfo();
    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);
    auto* impl = LockThisImpl();
    if (DelegateInvocation(impl->GetValue(), request, response, context)) {
        SetModified();
    }
}

void TDocumentNodeProxy::ListSelf(
    TReqList* request,
    TRspList* response,
    const TCtxListPtr& context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::ListRecursive(
    const TYPath& /*path*/,
    TReqList* request,
    TRspList* response,
    const TCtxListPtr& context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::RemoveRecursive(
    const TYPath& /*path*/,
    TReqRemove* request,
    TRspRemove* response,
    const TCtxRemovePtr& context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);
    auto* impl = LockThisImpl();
    if (DelegateInvocation(impl->GetValue(), request, response, context)) {
        SetModified();
    }
}

void TDocumentNodeProxy::ExistsRecursive(
    const TYPath& /*path*/,
    TReqExists* request,
    TRspExists* response, const TCtxExistsPtr& context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);
    const auto* impl = GetThisImpl();
    DelegateInvocation(impl->GetValue(), request, response, context);
}

void TDocumentNodeProxy::ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors)
{
    TBase::ListSystemAttributes(descriptors);

    descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Value)
        .SetWritable(true)
        .SetOpaque(true)
        .SetReplicated(true));
}

bool TDocumentNodeProxy::GetBuiltinAttribute(TInternedAttributeKey key, IYsonConsumer* consumer)
{
    const auto* impl = GetThisImpl();

    switch (key) {
        case EInternedAttributeKey::Value:
            BuildYsonFluently(consumer)
                .Value(impl->GetValue());
            return true;

        default:
            break;
    }

    return TBase::GetBuiltinAttribute(key, consumer);
}

bool TDocumentNodeProxy::SetBuiltinAttribute(TInternedAttributeKey key, const TYsonString& value)
{
    switch (key) {
        case EInternedAttributeKey::Value:
            SetImplValue(value);
            return true;

        default:
            break;
    }

    return TBase::SetBuiltinAttribute(key, value);
}

void TDocumentNodeProxy::SetImplValue(const TYsonString& value)
{
    auto* impl = LockThisImpl();
    impl->SetValue(ConvertToNode(value));
    SetModified();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
