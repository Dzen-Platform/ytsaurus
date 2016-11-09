#include "cypress_manager.h"
#include "private.h"
#include "access_tracker.h"
#include "expiration_tracker.h"
#include "config.h"
#include "lock_proxy.h"
#include "node_detail.h"
#include "node_proxy_detail.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>
#include <yt/server/cell_master/multicell_manager.h>

#include <yt/server/object_server/object_detail.h>
#include <yt/server/object_server/type_handler_detail.h>

#include <yt/server/security_server/account.h>
#include <yt/server/security_server/group.h>
#include <yt/server/security_server/security_manager.h>
#include <yt/server/security_server/user.h>

// COMPAT(babenko)
#include <yt/server/chunk_server/chunk_owner_base.h>
#include <yt/server/chunk_server/chunk_list.h>

#include <yt/ytlib/cypress_client/cypress_ypath.pb.h>
#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/core/misc/singleton.h>

#include <yt/core/ytree/ephemeral_node_factory.h>
#include <yt/core/ytree/ypath_detail.h>

namespace NYT {
namespace NCypressServer {

using namespace NCellMaster;
using namespace NBus;
using namespace NRpc;
using namespace NYTree;
using namespace NTransactionServer;
using namespace NHydra;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NObjectServer;
using namespace NSecurityClient;
using namespace NSecurityServer;
using namespace NCypressClient::NProto;
using namespace NChunkServer; // COMPAT(babenko)

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CypressServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TCypressManager::TNodeFactory
    : public TTransactionalNodeFactoryBase
    , public ICypressNodeFactory
{
public:
    TNodeFactory(
        NCellMaster::TBootstrap* bootstrap,
        TCypressManagerConfigPtr config,
        TTransaction* transaction,
        TAccount* account,
        bool preserveAccount)
        : Bootstrap_(bootstrap)
        , Config_(std::move(config))
        , Transaction_(transaction)
        , Account_(account)
        , PreserveAccount_(preserveAccount)
    {
        YCHECK(Bootstrap_);
        YCHECK(Account_);

        RegisterCommitHandler([&] () { OnCommit(); });
        RegisterRollbackHandler([&] () { OnRollback(); });
    }

    virtual ~TNodeFactory() override
    {
        RollbackIfNeeded();
    }

    virtual IStringNodePtr CreateString() override
    {
        return CreateNode(EObjectType::StringNode)->AsString();
    }

    virtual IInt64NodePtr CreateInt64() override
    {
        return CreateNode(EObjectType::Int64Node)->AsInt64();
    }

    virtual IUint64NodePtr CreateUint64() override
    {
        return CreateNode(EObjectType::Uint64Node)->AsUint64();
    }

    virtual IDoubleNodePtr CreateDouble() override
    {
        return CreateNode(EObjectType::DoubleNode)->AsDouble();
    }

    virtual IBooleanNodePtr CreateBoolean() override
    {
        return CreateNode(EObjectType::BooleanNode)->AsBoolean();
    }

    virtual IMapNodePtr CreateMap() override
    {
        return CreateNode(EObjectType::MapNode)->AsMap();
    }

    virtual IListNodePtr CreateList() override
    {
        return CreateNode(EObjectType::ListNode)->AsList();
    }

    virtual IEntityNodePtr CreateEntity() override
    {
        THROW_ERROR_EXCEPTION("Entity nodes cannot be created inside Cypress");
    }

    virtual NTransactionServer::TTransaction* GetTransaction() override
    {
        return Transaction_;
    }

    virtual TAccount* GetNewNodeAccount() override
    {
        return Account_;
    }

    virtual TAccount* GetClonedNodeAccount(
        TCypressNodeBase* sourceNode) override
    {
        return PreserveAccount_ ? sourceNode->GetAccount() : Account_;
    }

    virtual ICypressNodeProxyPtr CreateNode(
        EObjectType type,
        bool enableAccounting = true,
        IAttributeDictionary* attributes = nullptr) override
    {
        ValidateCreatedNodeType(type);

        auto* account = GetNewNodeAccount();
        auto securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidateResourceUsageIncrease(account, TClusterResources(1, 0));

        auto cypressManager = Bootstrap_->GetCypressManager();
        auto handler = cypressManager->FindHandler(type);
        if (!handler) {
            THROW_ERROR_EXCEPTION("Unknown object type %Qlv",
                type);
        }

        std::unique_ptr<IAttributeDictionary> attributeHolder;
        if (!attributes) {
            attributeHolder = CreateEphemeralAttributes();
            attributes = attributeHolder.get();
        }

        // TODO(babenko): this is a temporary workaround until dynamic tables become fully supported in
        // multicell mode
        if (attributes->Get<bool>("dynamic", false)) {
            attributes->Set("external", false);
        }

        auto multicellManager = Bootstrap_->GetMulticellManager();
        bool isExternalDefault =
            Bootstrap_->IsPrimaryMaster() &&
            !multicellManager->GetRegisteredMasterCellTags().empty() &&
            handler->IsExternalizable();
        bool isExternal = attributes->GetAndRemove<bool>("external", isExternalDefault);

        double externalCellBias = attributes->GetAndRemove<double>("external_cell_bias", 1.0);
        if (externalCellBias < 0.0 || externalCellBias > 1.0) {
            THROW_ERROR_EXCEPTION("\"external_cell_bias\" must be in range [0, 1]");
        }

        auto cellTag = NotReplicatedCellTag;
        if (isExternal) {
            if (!Bootstrap_->IsPrimaryMaster()) {
                THROW_ERROR_EXCEPTION("External nodes are only created at primary masters");
            }

            if (!handler->IsExternalizable()) {
                THROW_ERROR_EXCEPTION("Type %Qlv is not externalizable",
                    handler->GetObjectType());
            }

            auto maybeExternalCellTag = attributes->FindAndRemove<TCellTag>("external_cell_tag");
            if (maybeExternalCellTag) {
                cellTag = *maybeExternalCellTag;
                if (!multicellManager->IsRegisteredMasterCell(cellTag)) {
                    THROW_ERROR_EXCEPTION("Unknown cell tag %v", cellTag);
                }
            } else {
                cellTag = multicellManager->PickSecondaryMasterCell(externalCellBias);
                if (cellTag == InvalidCellTag) {
                    THROW_ERROR_EXCEPTION("No secondary masters registered");
                }
            }
        }

        // INodeTypeHandler::Create may modify the attributes.
        std::unique_ptr<IAttributeDictionary> replicationAttributes;
        if (isExternal) {
            replicationAttributes = attributes->Clone();
        }

        auto* trunkNode = cypressManager->CreateNode(
            NullObjectId,
            cellTag,
            handler,
            account,
            enableAccounting,
            Transaction_,
            attributes);

        RegisterCreatedNode(trunkNode);

        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->FillAttributes(trunkNode, *attributes);

        cypressManager->LockNode(trunkNode, Transaction_, ELockMode::Exclusive);

        if (isExternal) {
            NProto::TReqCreateForeignNode replicationRequest;
            ToProto(replicationRequest.mutable_node_id(), trunkNode->GetId());
            if (Transaction_) {
                ToProto(replicationRequest.mutable_transaction_id(), Transaction_->GetId());
            }
            replicationRequest.set_type(static_cast<int>(type));
            ToProto(replicationRequest.mutable_node_attributes(), *replicationAttributes);
            ToProto(replicationRequest.mutable_account_id(), Account_->GetId());
            replicationRequest.set_enable_accounting(enableAccounting);
            multicellManager->PostToMaster(replicationRequest, cellTag);
        }

        return cypressManager->GetNodeProxy(trunkNode, Transaction_);
    }

    virtual TCypressNodeBase* InstantiateNode(
        const TNodeId& id,
        TCellTag externalCellTag) override
    {
        auto cypressManager = Bootstrap_->GetCypressManager();
        auto* node = cypressManager->InstantiateNode(id, externalCellTag);

        RegisterCreatedNode(node);

        return node;
    }

    virtual TCypressNodeBase* CloneNode(
        TCypressNodeBase* sourceNode,
        ENodeCloneMode mode) override
    {
        ValidateCreatedNodeType(sourceNode->GetType());

        auto* clonedAccount = GetClonedNodeAccount(sourceNode);
        // Resource limit check must be suppressed when moving nodes
        // without altering the account.
        if (mode != ENodeCloneMode::Move || clonedAccount != sourceNode->GetAccount()) {
            // NB: Ignore disk space increase since in multicell mode the primary cell
            // might not be aware of the actual resource usage.
            // This should be safe since chunk lists are shared anyway.
            auto securityManager = Bootstrap_->GetSecurityManager();
            securityManager->ValidateResourceUsageIncrease(clonedAccount, TClusterResources(1, 0));
        }

        auto cypressManager = Bootstrap_->GetCypressManager();
        auto* clonedTrunkNode = cypressManager->CloneNode(sourceNode, this, mode);
        auto* clonedNode = cypressManager->LockNode(clonedTrunkNode, Transaction_, ELockMode::Exclusive);

        // NB: No need to call RegisterCreatedNode since
        // cloning a node involves calling ICypressNodeFactory::InstantiateNode,
        // which calls RegisterCreatedNode.
        if (sourceNode->IsExternal()) {
            NProto::TReqCloneForeignNode protoRequest;
            ToProto(protoRequest.mutable_source_node_id(), sourceNode->GetId());
            if (sourceNode->GetTransaction()) {
                ToProto(protoRequest.mutable_source_transaction_id(), sourceNode->GetTransaction()->GetId());
            }
            ToProto(protoRequest.mutable_cloned_node_id(), clonedNode->GetId());
            if (clonedNode->GetTransaction()) {
                ToProto(protoRequest.mutable_cloned_transaction_id(), clonedNode->GetTransaction()->GetId());
            }
            protoRequest.set_mode(static_cast<int>(mode));
            ToProto(protoRequest.mutable_account_id(), clonedNode->GetAccount()->GetId());

            auto multicellManager = Bootstrap_->GetMulticellManager();
            multicellManager->PostToMaster(protoRequest, sourceNode->GetExternalCellTag());
        }

        return clonedTrunkNode;
    }

private:
    NCellMaster::TBootstrap* const Bootstrap_;
    const TCypressManagerConfigPtr Config_;
    TTransaction* const Transaction_;
    TAccount* const Account_;
    const bool PreserveAccount_;

    std::vector<TCypressNodeBase*> CreatedNodes_;


    void OnCommit()
    {
        if (Transaction_) {
            auto transactionManager = Bootstrap_->GetTransactionManager();
            for (auto* node : CreatedNodes_) {
                transactionManager->StageNode(Transaction_, node);
            }
        }
        ReleaseCreatedNodes();
    }

    void OnRollback()
    {
        ReleaseCreatedNodes();
    }

    void ValidateCreatedNodeType(EObjectType type)
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        auto* schema = objectManager->GetSchema(type);

        auto securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(schema, EPermission::Create);
    }

    void RegisterCreatedNode(TCypressNodeBase* trunkNode)
    {
        Y_ASSERT(trunkNode->IsTrunk());
        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(trunkNode);
        CreatedNodes_.push_back(trunkNode);
    }

    void ReleaseCreatedNodes()
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        for (auto* node : CreatedNodes_) {
            objectManager->UnrefObject(node);
        }
        CreatedNodes_.clear();
    }
};

////////////////////////////////////////////////////////////////////////////////

class TCypressManager::TNodeTypeHandler
    : public TObjectTypeHandlerBase<TCypressNodeBase>
{
public:
    TNodeTypeHandler(
        TImpl* owner,
        INodeTypeHandlerPtr underlyingHandler);

    virtual ETypeFlags GetFlags() const override
    {
        return
            ETypeFlags::ReplicateAttributes |
            ETypeFlags::ReplicateDestroy |
            ETypeFlags::Creatable;
    }

    virtual EObjectType GetType() const override
    {
        return UnderlyingHandler_->GetObjectType();
    }

    virtual TObjectBase* FindObject(const TObjectId& id) override
    {
        auto cypressManager = Bootstrap_->GetCypressManager();
        return cypressManager->FindNode(TVersionedNodeId(id));
    }

    virtual TObjectBase* CreateObject(
        const TObjectId& /*hintId*/,
        IAttributeDictionary* /*attributes*/) override
    {
        THROW_ERROR_EXCEPTION("Cypress nodes cannot be created via this call");
    }

    virtual void DestroyObject(TObjectBase* object) throw();

private:
    TImpl* const Owner_;
    const INodeTypeHandlerPtr UnderlyingHandler_;


    virtual TCellTagList DoGetReplicationCellTags(const TCypressNodeBase* node) override
    {
        auto externalCellTag = node->GetExternalCellTag();
        return externalCellTag == NotReplicatedCellTag ? TCellTagList() : TCellTagList{externalCellTag};
    }

    virtual Stroka DoGetName(const TCypressNodeBase* node);

    virtual IObjectProxyPtr DoGetProxy(
        TCypressNodeBase* node,
        TTransaction* transaction) override
    {
        auto cypressManager = Bootstrap_->GetCypressManager();
        return cypressManager->GetNodeProxy(node, transaction);
    }

    virtual TAccessControlDescriptor* DoFindAcd(TCypressNodeBase* node) override
    {
        return &node->GetTrunkNode()->Acd();
    }

    virtual TObjectBase* DoGetParent(TCypressNodeBase* node) override
    {
        return node->GetParent();
    }
};

////////////////////////////////////////////////////////////////////////////////

class TCypressManager::TLockTypeHandler
    : public TObjectTypeHandlerWithMapBase<TLock>
{
public:
    explicit TLockTypeHandler(TImpl* owner);

    virtual EObjectType GetType() const override
    {
        return EObjectType::Lock;
    }

private:
    virtual Stroka DoGetName(const TLock* lock) override
    {
        return Format("lock %v", lock->GetId());
    }

    virtual IObjectProxyPtr DoGetProxy(
        TLock* lock,
        TTransaction* /*transaction*/) override
    {
        return CreateLockProxy(Bootstrap_, &Metadata_, lock);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TCypressManager::TYPathResolver
    : public INodeResolver
{
public:
    TYPathResolver(
        TBootstrap* bootstrap,
        TTransaction* transaction)
        : Bootstrap_(bootstrap)
        , Transaction_(transaction)
    { }

    virtual INodePtr ResolvePath(const TYPath& path) override
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        auto* resolver = objectManager->GetObjectResolver();
        auto objectProxy = resolver->ResolvePath(path, Transaction_);
        auto* nodeProxy = dynamic_cast<ICypressNodeProxy*>(objectProxy.Get());
        if (!nodeProxy) {
            THROW_ERROR_EXCEPTION("Path %v points to a nonversioned %Qlv object instead of a node",
                path,
                TypeFromId(objectProxy->GetId()));
        }
        return nodeProxy;
    }

    virtual TYPath GetPath(INodePtr node) override
    {
        auto* nodeProxy = ICypressNodeProxy::FromNode(node.Get());

        auto cypressManager = Bootstrap_->GetCypressManager();
        if (!cypressManager->IsAlive(nodeProxy->GetTrunkNode(), nodeProxy->GetTransaction())) {
            return FromObjectId(nodeProxy->GetId());
        }

        INodePtr root;
        auto path = GetNodeYPath(node, &root);

        auto* rootProxy = ICypressNodeProxy::FromNode(root.Get());
        return rootProxy->GetId() == cypressManager->GetRootNode()->GetId()
            ? "/" + path
            : "?" + path;
    }

private:
    TBootstrap* const Bootstrap_;
    TTransaction* const Transaction_;

};

////////////////////////////////////////////////////////////////////////////////

class TCypressManager::TImpl
    : public NCellMaster::TMasterAutomatonPart
{
public:
    TImpl(
        TCypressManagerConfigPtr config,
        TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap)
        , Config_(config)
        , AccessTracker_(New<TAccessTracker>(config, bootstrap))
        , ExpirationTracker_(New<TExpirationTracker>(config, bootstrap))
        , NodeMap_(TNodeMapTraits(this))
    {
        auto hydraFacade = Bootstrap_->GetHydraFacade();
        VERIFY_INVOKER_THREAD_AFFINITY(hydraFacade->GetAutomatonInvoker(), AutomatonThread);

        RootNodeId_ = MakeWellKnownId(EObjectType::MapNode, Bootstrap_->GetCellTag());

        RegisterHandler(New<TStringNodeTypeHandler>(Bootstrap_));
        RegisterHandler(New<TInt64NodeTypeHandler>(Bootstrap_));
        RegisterHandler(New<TUint64NodeTypeHandler>(Bootstrap_));
        RegisterHandler(New<TDoubleNodeTypeHandler>(Bootstrap_));
        RegisterHandler(New<TBooleanNodeTypeHandler>(Bootstrap_));
        RegisterHandler(New<TMapNodeTypeHandler>(Bootstrap_));
        RegisterHandler(New<TListNodeTypeHandler>(Bootstrap_));
        RegisterHandler(New<TLinkNodeTypeHandler>(Bootstrap_));
        RegisterHandler(New<TDocumentNodeTypeHandler>(Bootstrap_));

        RegisterLoader(
            "CypressManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "CypressManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "CypressManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "CypressManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));

        RegisterMethod(BIND(&TImpl::HydraUpdateAccessStatistics, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraCreateForeignNode, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraCloneForeignNode, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRemoveExpiredNodes, Unretained(this)));
    }

    void Initialize()
    {
        auto transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->SubscribeTransactionCommitted(BIND(
            &TImpl::OnTransactionCommitted,
            MakeStrong(this)));
        transactionManager->SubscribeTransactionAborted(BIND(
            &TImpl::OnTransactionAborted,
            MakeStrong(this)));

        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(New<TLockTypeHandler>(this));
    }


    void RegisterHandler(INodeTypeHandlerPtr handler)
    {
        // No thread affinity is given here.
        // This will be called during init-time only.
        YCHECK(handler);

        auto type = handler->GetObjectType();
        YCHECK(!TypeToHandler_[type]);
        TypeToHandler_[type] = handler;

        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(New<TNodeTypeHandler>(this, handler));
    }

    INodeTypeHandlerPtr FindHandler(EObjectType type)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        if (type < TEnumTraits<EObjectType>::GetMinValue() || type > TEnumTraits<EObjectType>::GetMaxValue()) {
            return nullptr;
        }

        return TypeToHandler_[type];
    }

    INodeTypeHandlerPtr GetHandler(EObjectType type)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto handler = FindHandler(type);
        YCHECK(handler);
        return handler;
    }

    INodeTypeHandlerPtr GetHandler(const TCypressNodeBase* node)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return GetHandler(node->GetType());
    }


    std::unique_ptr<ICypressNodeFactory> CreateNodeFactory(
        TTransaction* transaction,
        TAccount* account,
        bool preserveAccount)
    {
        return std::unique_ptr<ICypressNodeFactory>(new TNodeFactory(
            Bootstrap_,
            Config_,
            transaction,
            account,
            preserveAccount));
    }

    TCypressNodeBase* CreateNode(
        const TNodeId& hintId,
        TCellTag externalCellTag,
        INodeTypeHandlerPtr handler,
        TAccount* account,
        bool enableAccounting,
        TTransaction* transaction,
        IAttributeDictionary* attributes)
    {
        YCHECK(handler);
        YCHECK(account);
        YCHECK(attributes);

        auto nodeHolder = handler->Create(
            hintId,
            externalCellTag,
            transaction,
            attributes);
        auto* node = RegisterNode(std::move(nodeHolder));

        // Set account.
        auto securityManager = Bootstrap_->GetSecurityManager();
        securityManager->SetAccount(node, account);
        securityManager->SetNodeResourceAccounting(node, enableAccounting);

        // Set owner.
        auto* user = securityManager->GetAuthenticatedUser();
        auto* acd = securityManager->GetAcd(node);
        acd->SetOwner(user);

        return node;
    }


    TCypressNodeBase* InstantiateNode(
        const TNodeId& id,
        TCellTag externalCellTag)
    {
        auto type = TypeFromId(id);
        auto handler = GetHandler(type);
        auto nodeHolder = handler->Instantiate(TVersionedNodeId(id), externalCellTag);
        return RegisterNode(std::move(nodeHolder));
    }

    TCypressNodeBase* CloneNode(
        TCypressNodeBase* sourceNode,
        ICypressNodeFactory* factory,
        ENodeCloneMode mode)
    {
        YCHECK(sourceNode);
        YCHECK(factory);

        // Validate account access _before_ creating the actual copy.
        auto securityManager = Bootstrap_->GetSecurityManager();
        auto* clonedAccount = factory->GetClonedNodeAccount(sourceNode);
        securityManager->ValidatePermission(clonedAccount, EPermission::Use);

        return DoCloneNode(
            sourceNode,
            factory,
            NullObjectId,
            mode);
    }


    TMapNode* GetRootNode() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return RootNode_;
    }

    TCypressNodeBase* GetNodeOrThrow(const TVersionedNodeId& id)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* node = FindNode(id);
        if (!IsObjectAlive(node)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No such node %v",
                id);
        }

        return node;
    }

    INodeResolverPtr CreateResolver(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return New<TYPathResolver>(Bootstrap_, transaction);
    }

    TCypressNodeBase* FindNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNode->IsTrunk());

        // Fast path -- no transaction.
        if (!transaction) {
            return trunkNode;
        }

        TVersionedNodeId versionedId(trunkNode->GetId(), GetObjectId(transaction));
        return FindNode(versionedId);
    }

    TCypressNodeBase* GetVersionedNode(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNode->IsTrunk());

        auto* currentTransaction = transaction;
        while (true) {
            auto* currentNode = FindNode(trunkNode, currentTransaction);
            if (currentNode) {
                return currentNode;
            }
            currentTransaction = currentTransaction->GetParent();
        }
    }

    ICypressNodeProxyPtr GetNodeProxy(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNode->IsTrunk());

        auto handler = GetHandler(trunkNode);
        return handler->GetProxy(trunkNode, transaction);
    }


    TCypressNodeBase* LockNode(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction,
        const TLockRequest& request,
        bool recursive = false)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNode->IsTrunk());
        YCHECK(request.Mode != ELockMode::None && request.Mode != ELockMode::Snapshot);
        YCHECK(!recursive || request.Key.Kind == ELockKeyKind::None);

        TSubtreeNodes childrenToLock;
        if (recursive) {
            ListSubtreeNodes(trunkNode, transaction, true, &childrenToLock);
        } else {
            childrenToLock.push_back(trunkNode);
        }

        auto error = CheckLock(
            trunkNode,
            transaction,
            request,
            recursive);
        error.ThrowOnError();

        if (IsLockRedundant(trunkNode, transaction, request)) {
            return GetVersionedNode(trunkNode, transaction);
        }

        // Ensure deterministic order of children.
        std::sort(childrenToLock.begin(), childrenToLock.end(), TCypressNodeRefComparer::Compare);

        TCypressNodeBase* lockedNode = nullptr;
        for (auto* child : childrenToLock) {
            auto* lock = DoCreateLock(child, transaction, request, true);
            auto* lockedChild = DoAcquireLock(lock);
            if (child == trunkNode) {
                lockedNode = lockedChild;
            }
        }

        YCHECK(lockedNode);
        return lockedNode;
    }

    TLock* CreateLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request,
        bool waitable)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNode->IsTrunk());
        YCHECK(transaction);
        YCHECK(request.Mode != ELockMode::None);

        if (waitable && !transaction) {
            THROW_ERROR_EXCEPTION("Waitable lock requires a transaction");
        }

        if (request.Mode == ELockMode::Snapshot && !transaction) {
            THROW_ERROR_EXCEPTION("%Qlv lock requires a transaction",
                request.Mode);
        }

        // Try to lock without waiting in the queue.
        auto error = CheckLock(
            trunkNode,
            transaction,
            request,
            true);

        // Is it OK?
        if (error.IsOK()) {
            auto* lock = DoCreateLock(trunkNode, transaction, request, false);
            DoAcquireLock(lock);
            return lock;
        }

        // Should we wait?
        if (!waitable) {
            THROW_ERROR error;
        }

        // Will wait.
        return DoCreateLock(trunkNode, transaction, request, false);
    }


    void SetModified(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNode->IsTrunk());

        AccessTracker_->SetModified(trunkNode, transaction);
    }

    void SetAccessed(TCypressNodeBase* trunkNode)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNode->IsTrunk());

        if (HydraManager_->IsLeader() || HydraManager_->IsFollower() && !HasMutationContext()) {
            AccessTracker_->SetAccessed(trunkNode);
        }
    }

    void SetExpirationTime(TCypressNodeBase* trunkNode, TNullable<TInstant> time)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNode->IsTrunk());

        trunkNode->SetExpirationTime(time);
        ExpirationTracker_->OnNodeExpirationTimeUpdated(trunkNode);
    }


    TSubtreeNodes ListSubtreeNodes(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction,
        bool includeRoot)
    {
        TSubtreeNodes result;
        ListSubtreeNodes(trunkNode, transaction, includeRoot, &result);
        return result;
    }

    void AbortSubtreeTransactions(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction)
    {
        SmallVector<TTransaction*, 16> transactions;

        auto addLock = [&] (const TLock* lock) {
            // Get the top-most transaction.
            auto* transaction = lock->GetTransaction();
            while (transaction->GetParent()) {
                transaction = transaction->GetParent();
            }
            transactions.push_back(transaction);
        };

        auto nodes = ListSubtreeNodes(trunkNode, transaction, true);
        for (const auto* node : nodes) {
            const auto& lockingState = node->LockingState();
            for (auto* lock : lockingState.AcquiredLocks) {
                addLock(lock);
            }
            for (auto* lock : lockingState.PendingLocks) {
                addLock(lock);
            }
        }

        std::sort(transactions.begin(), transactions.end(), TObjectRefComparer::Compare);
        transactions.erase(
            std::unique(transactions.begin(), transactions.end()),
            transactions.end());

        auto transactionManager = Bootstrap_->GetTransactionManager();
        for (auto* transaction : transactions) {
            transactionManager->AbortTransaction(transaction, true);
        }
    }

    void AbortSubtreeTransactions(INodePtr node)
    {
        auto* cypressNode = ICypressNodeProxy::FromNode(node.Get());
        AbortSubtreeTransactions(cypressNode->GetTrunkNode(), cypressNode->GetTransaction());
    }


    bool IsOrphaned(TCypressNodeBase* trunkNode)
    {
        auto* currentNode = trunkNode;
        while (true) {
            if (!IsObjectAlive(currentNode)) {
                return true;
            }
            if (currentNode == RootNode_) {
                return false;
            }
            currentNode = currentNode->GetParent();
        }
    }

    bool IsAlive(TCypressNodeBase* trunkNode, TTransaction* transaction)
    {
        auto hasChild = [&] (TCypressNodeBase* parentTrunkNode, TCypressNodeBase* childTrunkNode) {
            // Compute child key or index.
            auto parentOriginators = GetNodeOriginators(transaction, parentTrunkNode);
            TNullable<Stroka> key;
            for (const auto* parentNode : parentOriginators) {
                switch (parentNode->GetNodeType()) {
                    case ENodeType::Map: {
                        const auto* parentMapNode = parentNode->As<TMapNode>();
                        auto it = parentMapNode->ChildToKey().find(childTrunkNode);
                        if (it != parentMapNode->ChildToKey().end()) {
                            key = it->second;
                        }
                        break;
                    }

                    case ENodeType::List: {
                        const auto* parentListNode = parentNode->As<TListNode>();
                        auto it = parentListNode->ChildToIndex().find(childTrunkNode);
                        return it != parentListNode->ChildToIndex().end();
                    }

                    default:
                        Y_UNREACHABLE();
                }

                if (key) {
                    break;
                }
            }

            if (!key) {
                return false;
            }

            // Look for thombstones.
            for (const auto* parentNode : parentOriginators) {
                switch (parentNode->GetNodeType()) {
                    case ENodeType::Map: {
                        const auto* parentMapNode = parentNode->As<TMapNode>();
                        auto it = parentMapNode->KeyToChild().find(*key);
                        if (it != parentMapNode->KeyToChild().end() && it->second != childTrunkNode) {
                            return false;
                        }
                        break;
                    }

                    case ENodeType::List:
                        // Do nothing.
                        break;

                    default:
                        Y_UNREACHABLE();
                }
            }

            return true;
        };


        auto* currentNode = trunkNode;
        while (true) {
            if (!IsObjectAlive(currentNode)) {
                return false;
            }
            if (currentNode == RootNode_) {
                return true;
            }
            auto* parentNode = currentNode->GetParent();
            if (!parentNode) {
                return false;
            }
            if (!hasChild(parentNode, currentNode)) {
                return false;
            }
            currentNode = parentNode;
        }
    }


    TCypressNodeList GetNodeOriginators(
        TTransaction* transaction,
        TCypressNodeBase* trunkNode)
    {
        YCHECK(trunkNode->IsTrunk());

        // Fast path.
        if (!transaction) {
            return TCypressNodeList(1, trunkNode);
        }

        // Slow path.
        TCypressNodeList result;
        auto* currentNode = GetVersionedNode(trunkNode, transaction);
        while (currentNode) {
            result.push_back(currentNode);
            currentNode = currentNode->GetOriginator();
        }

        return result;
    }

    TCypressNodeList GetNodeReverseOriginators(
        TTransaction* transaction,
        TCypressNodeBase* trunkNode)
    {
        auto result = GetNodeOriginators(transaction, trunkNode);
        std::reverse(result.begin(), result.end());
        return result;
    }


    DECLARE_ENTITY_MAP_ACCESSORS(Node, TCypressNodeBase);
    DECLARE_ENTITY_MAP_ACCESSORS(Lock, TLock);

private:
    friend class TNodeTypeHandler;
    friend class TLockTypeHandler;

    class TNodeMapTraits
    {
    public:
        explicit TNodeMapTraits(TImpl* owner);

        std::unique_ptr<TCypressNodeBase> Create(const TVersionedNodeId& id) const;

    private:
        TImpl* const Owner_;

    };

    const TCypressManagerConfigPtr Config_;

    const TAccessTrackerPtr AccessTracker_;
    const TExpirationTrackerPtr ExpirationTracker_;

    NHydra::TEntityMap<TCypressNodeBase, TNodeMapTraits> NodeMap_;
    NHydra::TEntityMap<TLock> LockMap_;

    TEnumIndexedVector<INodeTypeHandlerPtr, NObjectClient::EObjectType> TypeToHandler_;

    TNodeId RootNodeId_;
    TMapNode* RootNode_ = nullptr;

    // COMPAT(babenko)
    bool RecomputeChunkOwnerStatistics_ = false;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void SaveKeys(NCellMaster::TSaveContext& context) const
    {
        NodeMap_.SaveKeys(context);
        LockMap_.SaveKeys(context);
    }

    void SaveValues(NCellMaster::TSaveContext& context) const
    {
        NodeMap_.SaveValues(context);
        LockMap_.SaveValues(context);
    }


    void LoadKeys(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        NodeMap_.LoadKeys(context);
        LockMap_.LoadKeys(context);
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        NodeMap_.LoadValues(context);
        LockMap_.LoadValues(context);

        // COMPAT(babenko)
        RecomputeChunkOwnerStatistics_ = (context.GetVersion() < 304);
    }


    virtual void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::Clear();

        ExpirationTracker_->Clear();

        NodeMap_.Clear();
        LockMap_.Clear();

        InitBuiltin();
    }

    virtual void OnAfterSnapshotLoaded() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnAfterSnapshotLoaded();

        auto transactionManager = Bootstrap_->GetTransactionManager();

        LOG_INFO("Started initializing nodes");
        for (const auto& pair : NodeMap_) {
            auto* node = pair.second;

            // Reconstruct immediate ancestor sets.
            auto* parent = node->GetParent();
            if (parent) {
                YCHECK(parent->ImmediateDescendants().insert(node).second);
            }

            // Reconstruct TrunkNode and Transaction.
            auto transactionId = node->GetVersionedId().TransactionId;
            if (transactionId) {
                node->SetTrunkNode(GetNode(TVersionedNodeId(node->GetId())));
                node->SetTransaction(transactionManager->GetTransaction(transactionId));
            }

            // Compute originators.
            if (!node->IsTrunk()) {
                auto* parentTransaction = node->GetTransaction()->GetParent();
                auto* originator = GetVersionedNode(node->GetTrunkNode(), parentTransaction);
                node->SetOriginator(originator);
            }

            // Reconstruct lock iterators.
            if (node->HasLockingState()) {
                auto* lockingState = node->MutableLockingState();
                for (auto it = lockingState->AcquiredLocks.begin(); it != lockingState->AcquiredLocks.end(); ++it) {
                    auto* lock = *it;
                    lock->SetLockListIterator(it);
                }
                for (auto it = lockingState->PendingLocks.begin(); it != lockingState->PendingLocks.end(); ++it) {
                    auto* lock = *it;
                    lock->SetLockListIterator(it);
                }
                for (auto it = lockingState->ExclusiveLocks.begin(); it != lockingState->ExclusiveLocks.end(); ++it) {
                    auto* lock = *it;
                    lock->SetExclusiveLocksIterator(it);
                }
                for (auto it = lockingState->SharedLocks.begin(); it != lockingState->SharedLocks.end(); ++it) {
                    auto* lock = it->second;
                    lock->SetSharedLocksIterator(it);
                }
                for (auto it = lockingState->SnapshotLocks.begin(); it != lockingState->SnapshotLocks.end(); ++it) {
                    auto* lock = it->second;
                    lock->SetSnapshotLocksIterator(it);
                }
            }

            // COMPAT(babenko)
            if (RecomputeChunkOwnerStatistics_ &&
                (node->GetType() == EObjectType::Table || node->GetType() == EObjectType::File))
            {
                auto* chunkOwnerNode = node->As<TChunkOwnerBase>();
                const auto* chunkList = chunkOwnerNode->GetChunkList();
                if (chunkList) {
                    chunkOwnerNode->SnapshotStatistics() = chunkList->Statistics().ToDataStatistics();
                }
            }

            if (node->IsTrunk() && node->GetExpirationTime()) {
                ExpirationTracker_->OnNodeExpirationTimeUpdated(node);
            }
        }
        LOG_INFO("Finished initializing nodes");

        InitBuiltin();
    }


    void InitBuiltin()
    {
        auto* untypedRootNode = FindNode(TVersionedNodeId(RootNodeId_));
        if (untypedRootNode) {
            // Root already exists.
            RootNode_ = untypedRootNode->As<TMapNode>();
        } else {
            // Create the root.
            auto securityManager = Bootstrap_->GetSecurityManager();
            auto rootNodeHolder = std::make_unique<TMapNode>(TVersionedNodeId(RootNodeId_));
            rootNodeHolder->SetTrunkNode(rootNodeHolder.get());
            rootNodeHolder->SetAccount(securityManager->GetSysAccount());
            rootNodeHolder->Acd().SetInherit(false);
            rootNodeHolder->Acd().AddEntry(TAccessControlEntry(
                ESecurityAction::Allow,
                securityManager->GetEveryoneGroup(),
                EPermission::Read));
            rootNodeHolder->Acd().SetOwner(securityManager->GetRootUser());

            RootNode_ = rootNodeHolder.get();
            NodeMap_.Insert(TVersionedNodeId(RootNodeId_), std::move(rootNodeHolder));
            YCHECK(RootNode_->RefObject() == 1);
        }
    }


    virtual void OnRecoveryComplete() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnRecoveryComplete();

        AccessTracker_->Start();
    }

    virtual void OnLeaderRecoveryComplete() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnLeaderRecoveryComplete();

        if (Bootstrap_->IsPrimaryMaster()) {
            ExpirationTracker_->Start();
        }
    }

    virtual void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnStopLeading();

        AccessTracker_->Stop();

        if (Bootstrap_->IsPrimaryMaster()) {
            ExpirationTracker_->Stop();
        }
    }

    virtual void OnStopFollowing() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnStopFollowing();

        AccessTracker_->Stop();
    }


    TCypressNodeBase* RegisterNode(std::unique_ptr<TCypressNodeBase> trunkNodeHolder)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNodeHolder->IsTrunk());

        const auto& nodeId = trunkNodeHolder->GetId();
        auto* node = NodeMap_.Insert(TVersionedNodeId(nodeId), std::move(trunkNodeHolder));

        const auto* mutationContext = GetCurrentMutationContext();
        node->SetCreationTime(mutationContext->GetTimestamp());
        node->SetModificationTime(mutationContext->GetTimestamp());
        node->SetAccessTime(mutationContext->GetTimestamp());
        node->SetRevision(mutationContext->GetVersion().ToRevision());
        if (CellTagFromId(nodeId) != Bootstrap_->GetCellTag()) {
            node->SetForeign();
        }

        if (node->IsExternal()) {
            LOG_DEBUG_UNLESS(IsRecovery(), "External node registered (NodeId: %v, Type: %v, ExternalCellTag: %v)",
                node->GetId(),
                node->GetType(),
                node->GetExternalCellTag());
        } else {
            LOG_DEBUG_UNLESS(IsRecovery(), "%v node registered (NodeId: %v, Type: %v)",
                node->IsForeign() ? "Foreign" : "Local",
                node->GetId(),
                node->GetType());
        }

        return node;
    }

    void DestroyNode(TCypressNodeBase* trunkNode)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(trunkNode->IsTrunk());

        const auto& lockingState = trunkNode->LockingState();

        for (auto* lock : lockingState.AcquiredLocks) {
            lock->SetTrunkNode(nullptr);
            // NB: Transaction may have more than one lock for a given node.
            lock->GetTransaction()->LockedNodes().erase(trunkNode);
        }

        auto objectManager = Bootstrap_->GetObjectManager();
        for (auto* lock : lockingState.PendingLocks) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Lock orphaned (LockId: %v)",
                lock->GetId());
            lock->SetTrunkNode(nullptr);
            auto* transaction = lock->GetTransaction();
            YCHECK(transaction->Locks().erase(lock) == 1);
            lock->SetTransaction(nullptr);
            objectManager->UnrefObject(lock);
        }

        trunkNode->ResetLockingState();

        ExpirationTracker_->OnNodeDestroyed(trunkNode);

        auto handler = GetHandler(trunkNode);
        handler->Destroy(trunkNode);

        // Remove the object from the map but keep it alive.
        NodeMap_.Release(trunkNode->GetVersionedId()).release();
    }


    void OnTransactionCommitted(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        MergeNodes(transaction);
        ReleaseLocks(transaction, transaction->GetParent());
    }

    void OnTransactionAborted(TTransaction* transaction)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        RemoveBranchedNodes(transaction);
        ReleaseLocks(transaction, false);
    }


    TError CheckLock(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction,
        const TLockRequest& request,
        bool recursive)
    {
        TSubtreeNodes childrenToLock;
        if (recursive) {
            ListSubtreeNodes(trunkNode, transaction, true, &childrenToLock);
        } else {
            childrenToLock.push_back(trunkNode);
        }

        // Validate all potential locks to see if we need to take at least one of them.
        // This throws an exception in case the validation fails.
        for (auto* child : childrenToLock) {
            auto* trunkChild = child->GetTrunkNode();

            auto error = DoCheckLock(
                trunkChild,
                transaction,
                request);
            if (!error.IsOK()) {
                return error;
            }
        }

        return TError();
    }

    TError DoCheckLock(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction,
        const TLockRequest& request)
    {
        YCHECK(trunkNode->IsTrunk());
        YCHECK(transaction || request.Mode != ELockMode::Snapshot);

        const auto& lockingState = trunkNode->LockingState();
        const auto& snapshotLocks = lockingState.SnapshotLocks;
        const auto& sharedLocks = lockingState.SharedLocks;
        const auto& exclusiveLocks = lockingState.ExclusiveLocks;

        // Handle snapshot locks.
        if (transaction && snapshotLocks.find(transaction) != snapshotLocks.end()) {
            if (request.Mode == ELockMode::Snapshot) {
                // Already taken by this transaction.
                return TError();
            } else {
                // Cannot take non-snapshot lock when a snapshot lock is already taken.
                return TError(
                    NCypressClient::EErrorCode::SameTransactionLockConflict,
                    "Cannot take %Qlv lock for node %v since %Qlv lock is already taken by same transaction %v",
                    request.Mode,
                    GetNodePath(trunkNode, transaction),
                    ELockMode::Snapshot,
                    transaction->GetId());
            }
        }

        // New snapshot lock.
        if (request.Mode == ELockMode::Snapshot) {
            return TError();
        }

        // Check if any of parent transactions has taken a snapshot lock.
        if (transaction) {
            auto* currentTransaction = transaction->GetParent();
            while (currentTransaction) {
                if (snapshotLocks.find(currentTransaction) != snapshotLocks.end()) {
                    return TError(
                        NCypressClient::EErrorCode::SameTransactionLockConflict,
                        "Cannot take %Qlv lock for node %v since %Qlv lock is already taken by parent transaction %v",
                        request.Mode,
                        GetNodePath(trunkNode, transaction),
                        ELockMode::Snapshot,
                        currentTransaction->GetId());
                }
                currentTransaction = currentTransaction->GetParent();
            }
        }

        auto checkExistingLock = [&] (const TLock* existingLock) {
            auto* existingTransaction = existingLock->GetTransaction();
            if (!IsConcurrentTransaction(transaction, existingTransaction)) {
                return TError();
            }
            switch (request.Key.Kind) {
                case ELockKeyKind::None:
                    return TError(
                        NCypressClient::EErrorCode::ConcurrentTransactionLockConflict,
                        "Cannot take %Qlv lock for node %v since %Qlv lock is taken by concurrent transaction %v",
                        request.Mode,
                        GetNodePath(trunkNode, transaction),
                        ELockMode::Exclusive,
                        existingTransaction->GetId())
                        << TErrorAttribute("winner_transaction", existingTransaction->GetErrorDescription());

                case ELockKeyKind::Child:
                    return TError(
                        NCypressClient::EErrorCode::ConcurrentTransactionLockConflict,
                        "Cannot take lock for child %Qv of node %v since this child is locked by concurrent transaction %v",
                        request.Key.Name,
                        GetNodePath(trunkNode, transaction),
                        existingTransaction->GetId())
                        << TErrorAttribute("winner_transaction", existingTransaction->GetErrorDescription());

                case ELockKeyKind::Attribute:
                    return TError(
                        NCypressClient::EErrorCode::ConcurrentTransactionLockConflict,
                        "Cannot take lock for attribute %Qv of node %v since this attribute is locked by concurrent transaction %v",
                        request.Key.Name,
                        GetNodePath(trunkNode, transaction),
                        existingTransaction->GetId())
                        << TErrorAttribute("winner_transaction", existingTransaction->GetErrorDescription());

                default:
                    Y_UNREACHABLE();
            }
        };

        for (auto* existingLock : exclusiveLocks) {
            auto error = checkExistingLock(existingLock);
            if (!error.IsOK()) {
                return error;
            }
        }

        switch (request.Mode) {
            case ELockMode::Exclusive:
                for (const auto& pair : sharedLocks) {
                    auto error = checkExistingLock(pair.second);
                    if (!error.IsOK()) {
                        return error;
                    }
                }
                break;

            case ELockMode::Shared:
                if (request.Key.Kind != ELockKeyKind::None) {
                    auto range = sharedLocks.equal_range(request.Key);
                    for (auto it = range.first; it != range.second; ++it) {
                        auto error = checkExistingLock(it->second);
                        if (!error.IsOK()) {
                            return error;
                        }
                    }
                }
                break;

            default:
                Y_UNREACHABLE();
        }

        return TError();
    }

    bool IsLockRedundant(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction,
        const TLockRequest& request,
        const TLock* lockToIgnore = nullptr)
    {
        YCHECK(trunkNode->IsTrunk());
        YCHECK(request.Mode != ELockMode::None && request.Mode != ELockMode::Snapshot);

        if (!transaction) {
            return true;
        }

        const auto& lockingState = trunkNode->LockingState();
        const auto& sharedLocks = lockingState.SharedLocks;
        const auto& exclusiveLocks = lockingState.ExclusiveLocks;

        auto checkExistingLock = [&] (const TLock* existingLock) {
            auto* existingTransaction = existingLock->GetTransaction();
            return
                transaction == existingTransaction &&
                existingLock->Request() == request &&
                existingLock != lockToIgnore;
        };

        switch (request.Mode) {
            case ELockMode::Exclusive:
                for (auto* existingLock : exclusiveLocks) {
                    if (checkExistingLock(existingLock)) {
                        return true;
                    }
                }
                break;

            case ELockMode::Shared: {
                auto range = sharedLocks.equal_range(request.Key);
                for (auto it = range.first; it != range.second; ++it) {
                    if (checkExistingLock(it->second)) {
                        return true;
                    }
                }
                break;
            }

            default:
                Y_UNREACHABLE();
        }

        return false;
    }

    static bool IsRedundantLockRequest(
        const TLockRequest& newRequest,
        const TLockRequest& existingRequest)
    {
        Y_ASSERT(newRequest.Mode != ELockMode::Snapshot);
        Y_ASSERT(existingRequest.Mode != ELockMode::Snapshot);

        return
            existingRequest.Mode > newRequest.Mode ||
            existingRequest.Mode == newRequest.Mode && existingRequest.Key == newRequest.Key;
    }

    static bool IsParentTransaction(
        TTransaction* transaction,
        TTransaction* parent)
    {
        auto* currentTransaction = transaction;
        while (currentTransaction) {
            if (currentTransaction == parent) {
                return true;
            }
            currentTransaction = currentTransaction->GetParent();
        }
        return false;
    }

    static bool IsConcurrentTransaction(
        TTransaction* requestingTransaction,
        TTransaction* existingTransaction)
    {
        return
            !requestingTransaction ||
            !IsParentTransaction(requestingTransaction, existingTransaction);
    }

    TCypressNodeBase* DoAcquireLock(TLock* lock)
    {
        auto* trunkNode = lock->GetTrunkNode();
        auto* transaction = lock->GetTransaction();
        const auto& request = lock->Request();

        LOG_DEBUG_UNLESS(IsRecovery(), "Lock acquired (LockId: %v)",
            lock->GetId());

        YCHECK(lock->GetState() == ELockState::Pending);
        lock->SetState(ELockState::Acquired);

        auto* lockingState = trunkNode->MutableLockingState();
        lockingState->PendingLocks.erase(lock->GetLockListIterator());
        lockingState->AcquiredLocks.push_back(lock);
        lock->SetLockListIterator(--lockingState->AcquiredLocks.end());

        switch (request.Mode) {
            case ELockMode::Exclusive: {
                auto pair = lockingState->ExclusiveLocks.insert(lock);
                YCHECK(pair.second);
                lock->SetExclusiveLocksIterator(pair.first);
                break;
            }
            case ELockMode::Shared: {
                auto it = lockingState->SharedLocks.emplace(request.Key, lock);
                lock->SetSharedLocksIterator(it);
                break;
            }
            case ELockMode::Snapshot: {
                auto it = lockingState->SnapshotLocks.emplace(transaction, lock);
                lock->SetSnapshotLocksIterator(it);
                break;
            }
            default:
                Y_UNREACHABLE();
        }

        if (transaction->LockedNodes().insert(trunkNode).second) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Node locked (NodeId: %v, TransactionId: %v)",
                trunkNode->GetId(),
                transaction->GetId());
        }

        // Branch node, if needed.
        auto* branchedNode = FindNode(trunkNode, transaction);
        if (branchedNode) {
            if (branchedNode->GetLockMode() < request.Mode) {
                branchedNode->SetLockMode(request.Mode);
            }
            return branchedNode;
        }

        TCypressNodeBase* originatingNode;
        std::vector<TTransaction*> intermediateTransactions;
        // Walk up to the root, find originatingNode, construct the list of
        // intermediate transactions.
        auto* currentTransaction = transaction;
        while (true) {
            originatingNode = FindNode(trunkNode, currentTransaction);
            if (originatingNode) {
                break;
            }
            if (!currentTransaction) {
                break;
            }
            intermediateTransactions.push_back(currentTransaction);
            currentTransaction = currentTransaction->GetParent();
        }

        YCHECK(originatingNode);
        YCHECK(!intermediateTransactions.empty());

        if (request.Mode == ELockMode::Snapshot) {
            // Branch at requested transaction only.
            return BranchNode(originatingNode, transaction, request.Mode);
        } else {
            // Branch at all intermediate transactions.
            std::reverse(intermediateTransactions.begin(), intermediateTransactions.end());
            auto* currentNode = originatingNode;
            for (auto* transactionToBranch : intermediateTransactions) {
                currentNode = BranchNode(currentNode, transactionToBranch, request.Mode);
            }
            return currentNode;
        }
    }

    TLock* DoCreateLock(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction,
        const TLockRequest& request,
        bool implicit)
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::Lock, NullObjectId);
        auto lockHolder = std::make_unique<TLock>(id);
        auto* lock = LockMap_.Insert(id, std::move(lockHolder));

        lock->SetImplicit(implicit);
        lock->SetState(ELockState::Pending);
        lock->SetTrunkNode(trunkNode);
        lock->SetTransaction(transaction);
        lock->Request() = request;

        auto* lockingState = trunkNode->MutableLockingState();
        lockingState->PendingLocks.push_back(lock);
        lock->SetLockListIterator(--lockingState->PendingLocks.end());

        YCHECK(transaction->Locks().insert(lock).second);
        objectManager->RefObject(lock);

        LOG_DEBUG_UNLESS(IsRecovery(), "Lock created (LockId: %v, Mode: %v, Key: %v, NodeId: %v, Implicit: %v)",
            id,
            request.Mode,
            request.Key,
            TVersionedNodeId(trunkNode->GetId(), transaction->GetId()),
            implicit);

        return lock;
    }

    void ReleaseLocks(TTransaction* transaction, bool promote)
    {
        auto* parentTransaction = transaction->GetParent();
        auto objectManager = Bootstrap_->GetObjectManager();

        SmallVector<TLock*, 16> locks(transaction->Locks().begin(), transaction->Locks().end());
        transaction->Locks().clear();
        std::sort(locks.begin(), locks.end(), TObjectRefComparer::Compare);

        SmallVector<TCypressNodeBase*, 16> lockedNodes(transaction->LockedNodes().begin(), transaction->LockedNodes().end());
        transaction->LockedNodes().clear();
        std::sort(lockedNodes.begin(), lockedNodes.end(), TCypressNodeRefComparer::Compare);

        for (auto* lock : locks) {
            auto* trunkNode = lock->GetTrunkNode();
            // Decide if the lock must be promoted.
            if (promote &&
                lock->Request().Mode != ELockMode::Snapshot &&
                (!lock->GetImplicit() || !IsLockRedundant(trunkNode, parentTransaction, lock->Request(), lock)))
            {
                lock->SetTransaction(parentTransaction);
                YCHECK(parentTransaction->Locks().insert(lock).second);
                // NB: Node could be locked more than once.
                parentTransaction->LockedNodes().insert(trunkNode);
                LOG_DEBUG_UNLESS(IsRecovery(), "Lock promoted (LockId: %v, TransactionId: %v->%v)",
                    lock->GetId(),
                    transaction->GetId(),
                    parentTransaction->GetId());
            } else {
                if (trunkNode) {
                    auto* lockingState = trunkNode->MutableLockingState();
                    switch (lock->GetState()) {
                        case ELockState::Acquired: {
                            lockingState->AcquiredLocks.erase(lock->GetLockListIterator());
                            const auto& request = lock->Request();
                            switch (request.Mode) {
                                case ELockMode::Exclusive:
                                    lockingState->ExclusiveLocks.erase(lock->GetExclusiveLocksIterator());
                                    break;

                                case ELockMode::Shared:
                                    lockingState->SharedLocks.erase(lock->GetSharedLocksIterator());
                                    break;

                                case ELockMode::Snapshot:
                                    lockingState->SnapshotLocks.erase(lock->GetSnapshotLocksIterator());
                                    break;

                                default:
                                    Y_UNREACHABLE();
                            }
                            break;
                        }

                        case ELockState::Pending:
                            lockingState->PendingLocks.erase(lock->GetLockListIterator());
                            break;

                        default:
                            Y_UNREACHABLE();
                    }

                    trunkNode->ResetLockingStateIfEmpty();
                    lock->SetTrunkNode(nullptr);
                }
                lock->SetTransaction(nullptr);
                objectManager->UnrefObject(lock);
                LOG_DEBUG_UNLESS(IsRecovery(), "Lock destroyed (LockId: %v, TransactionId: %v)",
                    lock->GetId(),
                    transaction->GetId());
            }
        }

        for (auto* trunkNode : lockedNodes) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Node unlocked (NodeId: %v, TransactionId: %v)",
                trunkNode->GetId(),
                transaction->GetId());
        }

        for (auto* trunkNode : lockedNodes) {
            CheckPendingLocks(trunkNode);
        }
    }

    void CheckPendingLocks(TCypressNodeBase* trunkNode)
    {
        // Ignore orphaned nodes.
        // Eventually the node will get destroyed and the lock will become
        // orphaned.
        if (IsOrphaned(trunkNode)) {
            return;
        }

        // Make as many acquisitions as possible.
        const auto& lockingState = trunkNode->LockingState();
        auto it = lockingState.PendingLocks.begin();
        // Be prepared for locking state to vanish.
        while (trunkNode->HasLockingState() && it != lockingState.PendingLocks.end()) {
            // Be prepared to possible iterator invalidation.
            auto* lock = *it++;
            auto error = CheckLock(
                trunkNode,
                lock->GetTransaction(),
                lock->Request(),
                false);
            if (error.IsOK()) {
                DoAcquireLock(lock);
            }
        }
    }


    void ListSubtreeNodes(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction,
        bool includeRoot,
        TSubtreeNodes* subtreeNodes)
    {
        YCHECK(trunkNode->IsTrunk());

        if (includeRoot) {
            subtreeNodes->push_back(trunkNode);
        }

        switch (trunkNode->GetNodeType()) {
            case ENodeType::Map: {
                auto originators = GetNodeReverseOriginators(transaction, trunkNode);
                yhash_map<Stroka, TCypressNodeBase*> children;
                for (const auto* node : originators) {
                    const auto* mapNode = node->As<TMapNode>();
                    for (const auto& pair : mapNode->KeyToChild()) {
                        if (pair.second) {
                            children[pair.first] = pair.second;
                        } else {
                            // NB: erase may fail.
                            children.erase(pair.first);
                        }
                    }
                }

                for (const auto& pair : children) {
                    ListSubtreeNodes(pair.second, transaction, true, subtreeNodes);
                }

                break;
            }

            case ENodeType::List: {
                auto* node = GetVersionedNode(trunkNode, transaction);
                auto* listRoot = node->As<TListNode>();
                for (auto* trunkChild : listRoot->IndexToChild()) {
                    ListSubtreeNodes(trunkChild, transaction, true, subtreeNodes);
                }
                break;
            }

            default:
                break;
        }
    }


    TCypressNodeBase* BranchNode(
        TCypressNodeBase* originatingNode,
        TTransaction* transaction,
        ELockMode mode)
    {
        YCHECK(originatingNode);
        YCHECK(transaction);
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto objectManager = Bootstrap_->GetObjectManager();
        auto securityManager = Bootstrap_->GetSecurityManager();

        const auto& id = originatingNode->GetId();

        // Create a branched node and initialize its state.
        auto handler = GetHandler(originatingNode);
        auto branchedNodeHolder = handler->Branch(originatingNode, transaction, mode);

        TVersionedNodeId versionedId(id, transaction->GetId());
        auto* branchedNode = NodeMap_.Insert(versionedId, std::move(branchedNodeHolder));

        YCHECK(branchedNode->GetLockMode() == mode);

        // Register the branched node with the transaction.
        transaction->BranchedNodes().push_back(branchedNode);

        // The branched node holds an implicit reference to its originator.
        objectManager->RefObject(originatingNode->GetTrunkNode());

        // Update resource usage.
        auto* account = originatingNode->GetAccount();
        securityManager->SetAccount(branchedNode, account);

        return branchedNode;
    }

    void MergeNode(
        TTransaction* transaction,
        TCypressNodeBase* branchedNode)
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        auto securityManager = Bootstrap_->GetSecurityManager();

        auto handler = GetHandler(branchedNode);

        auto* trunkNode = branchedNode->GetTrunkNode();
        auto branchedId = branchedNode->GetVersionedId();
        auto* parentTransaction = transaction->GetParent();
        auto originatingId = TVersionedNodeId(branchedId.ObjectId, GetObjectId(parentTransaction));

        if (branchedNode->GetLockMode() != ELockMode::Snapshot) {
            auto* originatingNode = NodeMap_.Get(originatingId);

            // Merge changes back.
            handler->Merge(originatingNode, branchedNode);

            // The root needs a special handling.
            // When Cypress gets cleared, the root is created and is assigned zero creation time.
            // (We don't have any mutation context at hand to provide a synchronized timestamp.)
            // Later on, Cypress is initialized and filled with nodes.
            // At this point we set the root's creation time.
            if (trunkNode == RootNode_ && !parentTransaction) {
                originatingNode->SetCreationTime(originatingNode->GetModificationTime());
            }

            // Update resource usage.
            securityManager->UpdateAccountNodeUsage(originatingNode);
        } else {
            // Destroy the branched copy.
            handler->Destroy(branchedNode);

            LOG_DEBUG_UNLESS(IsRecovery(), "Node snapshot destroyed (NodeId: %v)", branchedId);
        }

        // Drop the implicit reference to the originator.
        objectManager->UnrefObject(trunkNode);

        // Remove the branched copy.
        NodeMap_.Remove(branchedId);

        LOG_DEBUG_UNLESS(IsRecovery(), "Branched node removed (NodeId: %v)", branchedId);
    }

    void MergeNodes(TTransaction* transaction)
    {
        for (auto* node : transaction->BranchedNodes()) {
            MergeNode(transaction, node);
        }
        transaction->BranchedNodes().clear();
    }

    void RemoveBranchedNode(
        TTransaction* transaction,
        TCypressNodeBase* branchedNode)
    {
        auto objectManager = Bootstrap_->GetObjectManager();

        auto handler = GetHandler(branchedNode);

        auto* trunkNode = branchedNode->GetTrunkNode();
        auto branchedNodeId = branchedNode->GetVersionedId();

        // Drop the implicit reference to the originator.
        objectManager->UnrefObject(trunkNode);

        if (branchedNode->GetLockMode() != ELockMode::Snapshot) {
            // Cleanup the branched node.
            auto branchedId = branchedNode->GetVersionedId();
            auto* parentTransaction = transaction->GetParent();
            auto originatingId = TVersionedNodeId(branchedId.ObjectId, GetObjectId(parentTransaction));
            auto* originatingNode = NodeMap_.Get(originatingId);
            handler->Unbranch(originatingNode, branchedNode);
        }

        // Remove the node.
        handler->Destroy(branchedNode);
        NodeMap_.Remove(branchedNodeId);

        LOG_DEBUG_UNLESS(IsRecovery(), "Branched node removed (NodeId: %v)", branchedNodeId);
    }

    void RemoveBranchedNodes(TTransaction* transaction)
    {
        for (auto* branchedNode : transaction->BranchedNodes()) {
            RemoveBranchedNode(transaction, branchedNode);
        }
        transaction->BranchedNodes().clear();
    }


    TYPath GetNodePath(
        TCypressNodeBase* trunkNode,
        TTransaction* transaction)
    {
        YCHECK(trunkNode->IsTrunk());

        auto proxy = GetNodeProxy(trunkNode, transaction);
        return proxy->GetResolver()->GetPath(proxy);
    }


    TCypressNodeBase* DoCloneNode(
        TCypressNodeBase* sourceNode,
        ICypressNodeFactory* factory,
        const TNodeId& hintId,
        ENodeCloneMode mode)
    {
        auto handler = GetHandler(sourceNode);
        auto* clonedNode = handler->Clone(
            sourceNode,
            factory,
            hintId,
            mode);

        // Set account.
        auto securityManager = Bootstrap_->GetSecurityManager();
        auto* account = factory->GetClonedNodeAccount(sourceNode);
        securityManager->SetAccount(clonedNode, account);

        // Set owner.
        auto* user = securityManager->GetAuthenticatedUser();
        auto* acd = securityManager->GetAcd(clonedNode);
        acd->SetOwner(user);

        // Copy expiration time.
        auto expirationTime = sourceNode->GetTrunkNode()->GetExpirationTime(); 
        if (expirationTime) {
            SetExpirationTime(clonedNode, *expirationTime);
        }

        return clonedNode;
    }


    void HydraUpdateAccessStatistics(NProto::TReqUpdateAccessStatistics* request) throw()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        for (const auto& update : request->updates()) {
            auto nodeId = FromProto<TNodeId>(update.node_id());
            auto* node = FindNode(TVersionedNodeId(nodeId));
            if (!IsObjectAlive(node))
                continue;

            // Update access time.
            auto accessTime = FromProto<TInstant>(update.access_time());
            if (accessTime > node->GetAccessTime()) {
                node->SetAccessTime(accessTime);
            }

            // Update access counter.
            i64 accessCounter = node->GetAccessCounter() + update.access_counter_delta();
            node->SetAccessCounter(accessCounter);
        }
    }

    void HydraCreateForeignNode(NProto::TReqCreateForeignNode* request) throw()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(Bootstrap_->IsSecondaryMaster());

        auto nodeId = FromProto<TObjectId>(request->node_id());
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto accountId = FromProto<TAccountId>(request->account_id());
        auto type = EObjectType(request->type());

        auto transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionId
            ? transactionManager->GetTransaction(transactionId)
            : nullptr;

        auto securityManager = Bootstrap_->GetSecurityManager();
        auto* account = accountId
            ? securityManager->GetAccount(accountId)
            : nullptr;

        auto attributes = request->has_node_attributes()
            ? FromProto(request->node_attributes())
            : std::unique_ptr<IAttributeDictionary>();

        auto enableAccounting = request->enable_accounting();

        auto versionedNodeId = TVersionedNodeId(nodeId, transactionId);

        LOG_DEBUG_UNLESS(IsRecovery(), "Creating foreign node (NodeId: %v, Type: %v, Account: %v, EnableAccounting: %v)",
            versionedNodeId,
            type,
            account ? MakeNullable(account->GetName()) : Null,
            enableAccounting);

        auto handler = GetHandler(type);

        auto* trunkNode = CreateNode(
            nodeId,
            NotReplicatedCellTag,
            handler,
            account,
            enableAccounting,
            transaction,
            attributes.get());

        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(trunkNode);
        objectManager->FillAttributes(trunkNode, *attributes);

        LockNode(trunkNode, transaction, ELockMode::Exclusive);
    }

    void HydraCloneForeignNode(NProto::TReqCloneForeignNode* request) throw()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(Bootstrap_->IsSecondaryMaster());

        auto sourceNodeId = FromProto<TNodeId>(request->source_node_id());
        auto sourceTransactionId = FromProto<TTransactionId>(request->source_transaction_id());
        auto clonedNodeId = FromProto<TNodeId>(request->cloned_node_id());
        auto clonedTransactionId = FromProto<TTransactionId>(request->cloned_transaction_id());
        auto mode = ENodeCloneMode(request->mode());
        auto accountId = FromProto<TAccountId>(request->account_id());

        auto transactionManager = Bootstrap_->GetTransactionManager();
        auto* sourceTransaction = sourceTransactionId
            ? transactionManager->GetTransaction(sourceTransactionId)
            : nullptr;
        auto* clonedTransaction = clonedTransactionId
            ? transactionManager->GetTransaction(clonedTransactionId)
            : nullptr;

        auto* sourceTrunkNode = GetNode(TVersionedObjectId(sourceNodeId));
        auto* sourceNode = GetVersionedNode(sourceTrunkNode, sourceTransaction);

        auto securityManager = Bootstrap_->GetSecurityManager();
        auto* account = securityManager->GetAccount(accountId);

        auto factory = CreateNodeFactory(clonedTransaction, account, false);

        LOG_DEBUG_UNLESS(IsRecovery(), "Cloning foreign node (SourceNodeId: %v, ClonedNodeId: %v, Account: %v)",
            TVersionedNodeId(sourceNodeId, sourceTransactionId),
            TVersionedNodeId(clonedNodeId, clonedTransactionId),
            account->GetName());

        auto* clonedTrunkNode = DoCloneNode(
            sourceNode,
            factory.get(),
            clonedNodeId,
            mode);

        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(clonedTrunkNode);

        LockNode(clonedTrunkNode, clonedTransaction, ELockMode::Exclusive);

        factory->Commit();
    }

    void HydraRemoveExpiredNodes(NProto::TReqRemoveExpiredNodes* request) throw()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        for (const auto& protoId : request->node_ids()) {
            auto nodeId = FromProto<TNodeId>(protoId);

            auto* trunkNode = NodeMap_.Find(TVersionedNodeId(nodeId, NullTransactionId));
            if (!trunkNode) {
                continue;
            }

            if (IsOrphaned(trunkNode)) {
                continue;
            }

            auto error = CheckLock(
                trunkNode,
                nullptr,
                ELockMode::Exclusive,
                true);

            if (error.IsOK()) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Removing expired node (NodeId: %v)",
                    nodeId);

                auto nodeProxy = GetNodeProxy(trunkNode, nullptr);
                auto parentProxy = nodeProxy->GetParent();
                parentProxy->RemoveChild(nodeProxy);
            } else {
                LOG_DEBUG_UNLESS(IsRecovery(), error, "Cannot remove an expired node; backing off and retrying (NodeId: %v)",
                    nodeId);

                ExpirationTracker_->OnNodeRemovalFailed(trunkNode);
            }
        }
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TCypressManager::TImpl, Node, TCypressNodeBase, NodeMap_);
DEFINE_ENTITY_MAP_ACCESSORS(TCypressManager::TImpl, Lock, TLock, LockMap_);

////////////////////////////////////////////////////////////////////////////////

TCypressManager::TImpl::TNodeMapTraits::TNodeMapTraits(TImpl* owner)
    : Owner_(owner)
{ }

std::unique_ptr<TCypressNodeBase> TCypressManager::TImpl::TNodeMapTraits::Create(const TVersionedNodeId& id) const
{
    auto type = TypeFromId(id.ObjectId);
    auto handler = Owner_->GetHandler(type);
    // This cell tag is fake and will be overwritten on load
    // (unless this is a pre-multicell snapshot, in which case NotReplicatedCellTag is just what we want).
    return handler->Instantiate(id, NotReplicatedCellTag);
}

////////////////////////////////////////////////////////////////////////////////

TCypressManager::TNodeTypeHandler::TNodeTypeHandler(
    TImpl* owner,
    INodeTypeHandlerPtr underlyingHandler)
    : TObjectTypeHandlerBase(owner->Bootstrap_)
    , Owner_(owner)
    , UnderlyingHandler_(underlyingHandler)
{ }

void TCypressManager::TNodeTypeHandler::DestroyObject(TObjectBase* object) throw()
{
    Owner_->DestroyNode(object->As<TCypressNodeBase>());
}

Stroka TCypressManager::TNodeTypeHandler::DoGetName(const TCypressNodeBase* node)
{
    auto path = Owner_->GetNodePath(node->GetTrunkNode(), node->GetTransaction());
    return Format("node %v", path);
}

////////////////////////////////////////////////////////////////////////////////

TCypressManager::TLockTypeHandler::TLockTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->LockMap_)
{ }

////////////////////////////////////////////////////////////////////////////////

TCypressManager::TCypressManager(
    TCypressManagerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TCypressManager::~TCypressManager()
{ }

void TCypressManager::Initialize()
{
    Impl_->Initialize();
}

void TCypressManager::RegisterHandler(INodeTypeHandlerPtr handler)
{
    Impl_->RegisterHandler(std::move(handler));
}

INodeTypeHandlerPtr TCypressManager::FindHandler(EObjectType type)
{
    return Impl_->FindHandler(type);
}

INodeTypeHandlerPtr TCypressManager::GetHandler(EObjectType type)
{
    return Impl_->GetHandler(type);
}

INodeTypeHandlerPtr TCypressManager::GetHandler(const TCypressNodeBase* node)
{
    return Impl_->GetHandler(node);
}

std::unique_ptr<ICypressNodeFactory> TCypressManager::CreateNodeFactory(
    TTransaction* transaction,
    TAccount* account,
    bool preserveAccount)
{
    return Impl_->CreateNodeFactory(transaction, account, preserveAccount);
}

TCypressNodeBase* TCypressManager::CreateNode(
    const TNodeId& hintId,
    TCellTag externalCellTag,
    INodeTypeHandlerPtr handler,
    TAccount* account,
    bool enableAccounting,
    TTransaction* transaction,
    IAttributeDictionary* attributes)
{
    return Impl_->CreateNode(
        hintId,
        externalCellTag,
        std::move(handler),
        account,
        enableAccounting,
        transaction,
        attributes);
}

TCypressNodeBase* TCypressManager::InstantiateNode(
    const TNodeId& id,
    TCellTag externalCellTag)
{
    return Impl_->InstantiateNode(id, externalCellTag);
}

TCypressNodeBase* TCypressManager::CloneNode(
    TCypressNodeBase* sourceNode,
    ICypressNodeFactory* factory,
    ENodeCloneMode mode)
{
    return Impl_->CloneNode(sourceNode, factory, mode);
}

TMapNode* TCypressManager::GetRootNode() const
{
    return Impl_->GetRootNode();
}

TCypressNodeBase* TCypressManager::GetNodeOrThrow(const TVersionedNodeId& id)
{
    return Impl_->GetNodeOrThrow(id);
}

INodeResolverPtr TCypressManager::CreateResolver(TTransaction* transaction)
{
    return Impl_->CreateResolver(transaction);
}

TCypressNodeBase* TCypressManager::FindNode(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction)
{
    return Impl_->FindNode(trunkNode, transaction);
}

TCypressNodeBase* TCypressManager::GetVersionedNode(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction)
{
    return Impl_->GetVersionedNode(trunkNode, transaction);
}

ICypressNodeProxyPtr TCypressManager::GetNodeProxy(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction)
{
    return Impl_->GetNodeProxy(trunkNode, transaction);
}

TCypressNodeBase* TCypressManager::LockNode(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction,
    const TLockRequest& request,
    bool recursive)
{
    return Impl_->LockNode(trunkNode, transaction, request, recursive);
}

TLock* TCypressManager::CreateLock(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction,
    const TLockRequest& request,
    bool waitable)
{
    return Impl_->CreateLock(trunkNode, transaction, request, waitable);
}

void TCypressManager::SetModified(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction)
{
    Impl_->SetModified(trunkNode, transaction);
}

void TCypressManager::SetAccessed(TCypressNodeBase* trunkNode)
{
    Impl_->SetAccessed(trunkNode);
}

void TCypressManager::SetExpirationTime(TCypressNodeBase* trunkNode, TNullable<TInstant> time)
{
    Impl_->SetExpirationTime(trunkNode, time);
}

TCypressManager::TSubtreeNodes TCypressManager::ListSubtreeNodes(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction,
    bool includeRoot)
{
    return Impl_->ListSubtreeNodes(trunkNode, transaction, includeRoot);
}

void TCypressManager::AbortSubtreeTransactions(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction)
{
    Impl_->AbortSubtreeTransactions(trunkNode, transaction);
}

void TCypressManager::AbortSubtreeTransactions(INodePtr node)
{
    Impl_->AbortSubtreeTransactions(std::move(node));
}

bool TCypressManager::IsOrphaned(TCypressNodeBase* trunkNode)
{
    return Impl_->IsOrphaned(trunkNode);
}

bool TCypressManager::IsAlive(
    TCypressNodeBase* trunkNode,
    TTransaction* transaction)
{
    return Impl_->IsAlive(trunkNode, transaction);
}

TCypressNodeList TCypressManager::GetNodeOriginators(
    TTransaction* transaction,
    TCypressNodeBase* trunkNode)
{
    return Impl_->GetNodeOriginators(transaction, trunkNode);
}

TCypressNodeList TCypressManager::GetNodeReverseOriginators(
    TTransaction* transaction,
    TCypressNodeBase* trunkNode)
{
    return Impl_->GetNodeReverseOriginators(transaction, trunkNode);
}

DELEGATE_ENTITY_MAP_ACCESSORS(TCypressManager, Node, TCypressNodeBase, *Impl_);
DELEGATE_ENTITY_MAP_ACCESSORS(TCypressManager, Lock, TLock, *Impl_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
