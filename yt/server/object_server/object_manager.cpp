#include "stdafx.h"
#include "object_manager.h"
#include "object.h"
#include "config.h"
#include "private.h"
#include "garbage_collector.h"
#include "schema.h"
#include "master.h"

#include <core/ypath/tokenizer.h>

#include <core/rpc/response_keeper.h>

#include <core/erasure/public.h>

#include <core/ytree/exception_helpers.h>
#include <core/ytree/node_detail.h>

#include <core/profiling/profile_manager.h>
#include <core/profiling/scoped_timer.h>

#include <ytlib/object_client/helpers.h>
#include <ytlib/object_client/object_ypath_proxy.h>
#include <ytlib/object_client/master_ypath_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>
#include <ytlib/cypress_client/rpc_helpers.h>

#include <ytlib/election/cell_manager.h>

#include <ytlib/hive/cell_directory.h>

#include <server/election/election_manager.h>

#include <server/cell_master/serialize.h>

#include <server/transaction_server/transaction_manager.h>
#include <server/transaction_server/transaction.h>

#include <server/cypress_server/cypress_manager.h>
#include <server/cypress_server/node_proxy.h>

#include <server/chunk_server/chunk_list.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/hydra_facade.h>
#include <server/cell_master/multicell_manager.h>

#include <server/security_server/user.h>
#include <server/security_server/group.h>
#include <server/security_server/account.h>
#include <server/security_server/security_manager.h>

namespace NYT {
namespace NObjectServer {

using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NHydra;
using namespace NRpc;
using namespace NBus;
using namespace NCypressServer;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NTransactionServer;
using namespace NSecurityServer;
using namespace NChunkServer;
using namespace NObjectClient;
using namespace NHydra;
using namespace NCellMaster;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ObjectServerLogger;
static const auto ProfilingPeriod = TDuration::MilliSeconds(100);

////////////////////////////////////////////////////////////////////////////////

class TObjectManager::TRemoteProxy
    : public IYPathService
{
public:
    TRemoteProxy(TBootstrap* bootstrap, const TObjectId& objectId)
        : Bootstrap_(bootstrap)
        , ObjectId_(objectId)
    { }

    virtual TResolveResult Resolve(const TYPath& path, IServiceContextPtr context) override
    {
        const auto& ypathExt = context->RequestHeader().GetExtension(NYTree::NProto::TYPathHeaderExt::ypath_header_ext);
        if (ypathExt.mutating()) {
            THROW_ERROR_EXCEPTION("Mutating requests to remote cells are not allowed");
        }

        YCHECK(!HasMutationContext());

        return TResolveResult::Here(path);
    }

    virtual void Invoke(IServiceContextPtr context) override
    {
        auto requestMessage = context->GetRequestMessage();
        NRpc::NProto::TRequestHeader requestHeader;
        ParseRequestHeader(requestMessage, &requestHeader);

        auto updatedYPath = FromObjectId(ObjectId_) + GetRequestYPath(context);
        SetRequestYPath(&requestHeader, updatedYPath);
        auto updatedMessage = SetRequestHeader(requestMessage, requestHeader);

        auto cellTag = CellTagFromId(ObjectId_);
        auto objectManager = Bootstrap_->GetObjectManager();
        auto asyncResponseMessage = objectManager->ForwardToLeader(cellTag, updatedMessage);
        context->ReplyFrom(std::move(asyncResponseMessage));
    }

    virtual void WriteAttributesFragment(
        IAsyncYsonConsumer* /*consumer*/,
        const TAttributeFilter& /*filter*/,
        bool /*sortKeys*/) override
    {
        YUNREACHABLE();
    }

private:
    TBootstrap* const Bootstrap_;
    const TObjectId ObjectId_;

};

////////////////////////////////////////////////////////////////////////////////

class TObjectManager::TRootService
    : public IYPathService
{
public:
    explicit TRootService(TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    { }

    virtual TResolveResult Resolve(const TYPath& path, IServiceContextPtr context) override
    {
        const auto& ypathExt = context->RequestHeader().GetExtension(NYTree::NProto::TYPathHeaderExt::ypath_header_ext);
        if (ypathExt.mutating()) {
            // Mutating request.

            if (HasMutationContext()) {
                // Nested call or recovery.
                return DoResolveThere(path, std::move(context));
            }

            // Commit mutation.
            return DoResolveHere(path);
        } else {
            // Read-only request.
            return DoResolveThere(path, context);
        }
    }

    virtual void Invoke(IServiceContextPtr context) override
    {
        auto hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
        if (hydraManager->IsActiveFollower()) {
            ForwardToLeader(std::move(context));
            return;
        }

        auto mutationId = GetMutationId(context);
        if (mutationId) {
            auto responseKeeper = Bootstrap_->GetHydraFacade()->GetResponseKeeper();
            auto asyncResponseMessage = responseKeeper->TryBeginRequest(mutationId, context->IsRetry());
            if (asyncResponseMessage) {
                context->ReplyFrom(std::move(asyncResponseMessage));
                return;
            }
        }

        auto securityManager = Bootstrap_->GetSecurityManager();
        auto* user = securityManager->GetAuthenticatedUser();
        auto userId = user->GetId();

        NProto::TReqExecute request;
        ToProto(request.mutable_user_id(), userId);
        // TODO(babenko): optimize, use multipart records
        auto requestMessage = context->GetRequestMessage();
        for (const auto& part : requestMessage) {
            request.add_request_parts(part.Begin(), part.Size());
        }

        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager
            ->CreateExecuteMutation(request)
            ->SetAction(
                BIND(
                    &TObjectManager::HydraExecuteLeader,
                    objectManager,
                    userId,
                    mutationId,
                    context))
            ->Commit()
            .Subscribe(BIND([=] (const TErrorOr<TMutationResponse>& result) {
                if (!result.IsOK()) {
                    // Reply with commit error.
                    context->Reply(result);
                }
            }));
    }

    virtual void WriteAttributesFragment(
        IAsyncYsonConsumer* /*consumer*/,
        const TAttributeFilter& /*filter*/,
        bool /*sortKeys*/) override
    {
        YUNREACHABLE();
    }

private:
    TBootstrap* const Bootstrap_;


    static TResolveResult DoResolveHere(const TYPath& path)
    {
        return TResolveResult::Here(path);
    }

    TResolveResult DoResolveThere(const TYPath& path, IServiceContextPtr context)
    {
        auto cypressManager = Bootstrap_->GetCypressManager();
        auto objectManager = Bootstrap_->GetObjectManager();
        auto transactionManager = Bootstrap_->GetTransactionManager();

        TTransaction* transaction = nullptr;
        auto transactionId = GetTransactionId(context);
        if (transactionId) {
            transaction = transactionManager->GetTransactionOrThrow(transactionId);
        }

        NYPath::TTokenizer tokenizer(path);
        switch (tokenizer.Advance()) {
            case NYPath::ETokenType::EndOfStream:
                return TResolveResult::There(objectManager->GetMasterProxy(), tokenizer.GetSuffix());

            case NYPath::ETokenType::Slash: {
                auto root = cypressManager->GetNodeProxy(
                    cypressManager->GetRootNode(),
                    transaction);
                return TResolveResult::There(root, tokenizer.GetSuffix());
            }

            case NYPath::ETokenType::Literal: {
                const auto& token = tokenizer.GetToken();
                if (!token.has_prefix(ObjectIdPathPrefix)) {
                    tokenizer.ThrowUnexpected();
                }

                TStringBuf objectIdString(token.begin() + ObjectIdPathPrefix.length(), token.end());
                TObjectId objectId;
                if (!TObjectId::FromString(objectIdString, &objectId)) {
                    THROW_ERROR_EXCEPTION("Error parsing object id %v",
                        objectIdString);
                }

                bool suppressRedirect = false;
                if (tokenizer.Advance() == NYPath::ETokenType::Ampersand) {
                    suppressRedirect = true;
                    tokenizer.Advance();
                }

                IYPathServicePtr proxy;
                if (!suppressRedirect &&
                    CellTagFromId(objectId) != Bootstrap_->GetCellTag() &&
                    Bootstrap_->IsPrimaryMaster())
                {
                    proxy = objectManager->CreateRemoteProxy(objectId);
                } else {
                    auto* object = (context->GetMethod() == "Exists")
                        ? objectManager->FindObject(objectId)
                        : objectManager->GetObjectOrThrow(objectId);
                    proxy = IsObjectAlive(object)
                        ? objectManager->GetProxy(object, transaction)
                        : TNonexistingService::Get();
                }
                return TResolveResult::There(proxy, tokenizer.GetInput());
            }

            default:
                tokenizer.ThrowUnexpected();
                YUNREACHABLE();
        }
    }


    void ForwardToLeader(IServiceContextPtr context)
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        auto asyncResponseMessage = objectManager->ForwardToLeader(
            Bootstrap_->GetCellTag(),
            context->GetRequestMessage());
        context->ReplyFrom(std::move(asyncResponseMessage));
    }

};

////////////////////////////////////////////////////////////////////////////////

class TObjectManager::TObjectResolver
    : public IObjectResolver
{
public:
    explicit TObjectResolver(TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    { }

    virtual IObjectProxyPtr ResolvePath(const TYPath& path, TTransaction* transaction) override
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        auto cypressManager = Bootstrap_->GetCypressManager();

        NYPath::TTokenizer tokenizer(path);
        switch (tokenizer.Advance()) {
            case NYPath::ETokenType::EndOfStream:
                return objectManager->GetMasterProxy();

            case NYPath::ETokenType::Slash: {
                auto root = cypressManager->GetNodeProxy(
                    cypressManager->GetRootNode(),
                    transaction);
                return DoResolvePath(root, transaction, tokenizer.GetSuffix());
            }

            case NYPath::ETokenType::Literal: {
                const auto& token = tokenizer.GetToken();
                if (!token.has_prefix(ObjectIdPathPrefix)) {
                    tokenizer.ThrowUnexpected();
                }

                TStringBuf objectIdString(token.begin() + ObjectIdPathPrefix.length(), token.end());
                TObjectId objectId;
                if (!TObjectId::FromString(objectIdString, &objectId)) {
                    THROW_ERROR_EXCEPTION(
                        NYTree::EErrorCode::ResolveError,
                        "Error parsing object id %Qv",
                        objectIdString);
                }

                auto* object = objectManager->GetObjectOrThrow(objectId);
                auto proxy = objectManager->GetProxy(object, transaction);
                return DoResolvePath(proxy, transaction, tokenizer.GetSuffix());
            }

            default:
                tokenizer.ThrowUnexpected();
                YUNREACHABLE();
        }
    }

    virtual TYPath GetPath(IObjectProxyPtr proxy) override
    {
        const auto& id = proxy->GetId();
        if (IsVersionedType(TypeFromId(id))) {
            auto* nodeProxy = dynamic_cast<ICypressNodeProxy*>(proxy.Get());
            YASSERT(nodeProxy);
            auto resolver = nodeProxy->GetResolver();
            return resolver->GetPath(nodeProxy);
        } else {
            return FromObjectId(id);
        }
    }

private:
    TBootstrap* const Bootstrap_;


    IObjectProxyPtr DoResolvePath(
        IObjectProxyPtr proxy,
        TTransaction* transaction,
        const TYPath& path)
    {
        // Fast path.
        if (path.empty()) {
            return proxy;
        }

        // Slow path.
        auto req = TObjectYPathProxy::GetBasicAttributes(path);
        auto rsp = SyncExecuteVerb(proxy, req);
        auto objectId = FromProto<TObjectId>(rsp->object_id());

        auto objectManager = Bootstrap_->GetObjectManager();
        auto* object = objectManager->GetObjectOrThrow(objectId);
        return objectManager->GetProxy(object, transaction);
    }

};

////////////////////////////////////////////////////////////////////////////////

TObjectManager::TObjectManager(
    TObjectManagerConfigPtr config,
    TBootstrap* bootstrap)
    : TMasterAutomatonPart(bootstrap)
    , Config_(config)
    , Profiler(ObjectServerProfiler)
    , RootService_(New<TRootService>(Bootstrap_))
    , ObjectResolver_(new TObjectResolver(Bootstrap_))
    , GarbageCollector_(New<TGarbageCollector>(Config_, Bootstrap_))
{
    YCHECK(config);
    YCHECK(bootstrap);

    RegisterLoader(
        "ObjectManager.Keys",
        BIND(&TObjectManager::LoadKeys, Unretained(this)));
    RegisterLoader(
        "ObjectManager.Values",
        BIND(&TObjectManager::LoadValues, Unretained(this)));

    RegisterSaver(
        ESyncSerializationPriority::Keys,
        "ObjectManager.Keys",
        BIND(&TObjectManager::SaveKeys, Unretained(this)));
    RegisterSaver(
        ESyncSerializationPriority::Values,
        "ObjectManager.Values",
        BIND(&TObjectManager::SaveValues, Unretained(this)));

    RegisterHandler(CreateMasterTypeHandler(Bootstrap_));

    RegisterMethod(BIND(&TObjectManager::HydraExecuteFollower, Unretained(this)));
    RegisterMethod(BIND(&TObjectManager::HydraDestroyObjects, Unretained(this)));
    RegisterMethod(BIND(&TObjectManager::HydraCreateForeignObject, Unretained(this)));
    RegisterMethod(BIND(&TObjectManager::HydraRemoveForeignObject, Unretained(this)));
    RegisterMethod(BIND(&TObjectManager::HydraUnrefExportedObjects, Unretained(this)));

    MasterObjectId_ = MakeWellKnownId(EObjectType::Master, Bootstrap_->GetPrimaryCellTag());
}

void TObjectManager::Initialize()
{
    if (Bootstrap_->IsPrimaryMaster()) {
        auto multicellManager = Bootstrap_->GetMulticellManager();
        multicellManager->SubscribeSecondaryMasterRegistered(
            BIND(&TObjectManager::OnSecondaryMasterRegistered, MakeWeak(this)));
    }

    ProfilingExecutor_ = New<TPeriodicExecutor>(
        Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(),
        BIND(&TObjectManager::OnProfiling, MakeWeak(this)),
        ProfilingPeriod);
    ProfilingExecutor_->Start();
}

IYPathServicePtr TObjectManager::GetRootService()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return RootService_;
}

TObjectBase* TObjectManager::GetMasterObject()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return MasterObject_.get();
}

IObjectProxyPtr TObjectManager::GetMasterProxy()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return MasterProxy_;
}

TObjectBase* TObjectManager::FindSchema(EObjectType type)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return TypeToEntry_[type].SchemaObject;
}

TObjectBase* TObjectManager::GetSchema(EObjectType type)
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto* schema = FindSchema(type);
    YCHECK(schema);
    return schema;
}

IObjectProxyPtr TObjectManager::GetSchemaProxy(EObjectType type)
{
    VERIFY_THREAD_AFFINITY_ANY();

    const auto& entry = TypeToEntry_[type];
    YCHECK(entry.SchemaProxy);
    return entry.SchemaProxy;
}

void TObjectManager::RegisterHandler(IObjectTypeHandlerPtr handler)
{
    // No thread affinity check here.
    // This will be called during init-time only but from an unspecified thread.
    YCHECK(handler);

    auto type = handler->GetType();
    YCHECK(!TypeToEntry_[type].Handler);
    YCHECK(RegisteredTypes_.insert(type).second);
    auto& entry = TypeToEntry_[type];
    entry.Handler = handler;
    entry.TagId = NProfiling::TProfileManager::Get()->RegisterTag("type", type);

    if (HasSchema(type)) {
        auto schemaType = SchemaTypeFromType(type);
        auto& schemaEntry = TypeToEntry_[schemaType];
        schemaEntry.Handler = CreateSchemaTypeHandler(Bootstrap_, type);

        auto schemaObjectId = MakeSchemaObjectId(type, Bootstrap_->GetPrimaryCellTag());

        LOG_INFO("Type registered (Type: %v, SchemaObjectId: %v)",
            type,
            schemaObjectId);
    } else {
        LOG_INFO("Type registered (Type: %v)",
            type);
    }
}

static const IObjectTypeHandlerPtr NullTypeHandler;

const IObjectTypeHandlerPtr& TObjectManager::FindHandler(EObjectType type) const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return type >= MinObjectType && type <= MaxObjectType
        ? TypeToEntry_[type].Handler
        : NullTypeHandler;
}

const IObjectTypeHandlerPtr& TObjectManager::GetHandler(EObjectType type) const
{
    VERIFY_THREAD_AFFINITY_ANY();

    const auto& handler = FindHandler(type);
    YASSERT(handler);
    return handler;
}

const IObjectTypeHandlerPtr& TObjectManager::GetHandler(const TObjectBase* object) const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return GetHandler(object->GetType());
}

const std::set<EObjectType>& TObjectManager::GetRegisteredTypes() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return RegisteredTypes_;
}

TObjectId TObjectManager::GenerateId(EObjectType type, const TObjectId& hintId)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto* mutationContext = GetCurrentMutationContext();
    auto version = mutationContext->GetVersion();
    auto random = mutationContext->RandomGenerator().Generate<ui64>();

    auto cellTag = Bootstrap_->GetCellTag();

    auto id = hintId
        ? hintId
        : MakeRegularId(type, cellTag, random, version);
    YASSERT(TypeFromId(id) == type);

    ++CreatedObjectCount_;

    LOG_DEBUG_UNLESS(IsRecovery(), "Object created (Type: %v, Id: %v)",
        type,
        id);

    return id;
}

bool TObjectManager::IsForeign(const TObjectBase* object)
{
    return CellTagFromId(object->GetId()) != Bootstrap_->GetCellTag();
}

int TObjectManager::RefObject(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YASSERT(object->IsTrunk());

    int refCounter = object->RefObject();
    LOG_DEBUG_UNLESS(IsRecovery(), "Object referenced (Id: %v, RefCounter: %v, WeakRefCounter: %v)",
        object->GetId(),
        refCounter,
        object->GetObjectWeakRefCounter());

    if (refCounter == 1) {
        GarbageCollector_->UnregisterZombie(object);
    }

    return refCounter;
}

int TObjectManager::UnrefObject(TObjectBase* object, int count)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YASSERT(object->IsTrunk());

    int refCounter = object->UnrefObject(count);
    LOG_DEBUG_UNLESS(IsRecovery(), "Object unreferenced (Id: %v, RefCounter: %v, WeakRefCounter: %v)",
        object->GetId(),
        refCounter,
        object->GetObjectWeakRefCounter());

    if (refCounter == 0) {
        const auto& handler = GetHandler(object);
        handler->ZombifyObject(object);

        GarbageCollector_->RegisterZombie(object);

        if (Bootstrap_->IsPrimaryMaster()) {
            auto replicationFlags = handler->GetReplicationFlags();
            auto replicationCellTag = handler->GetReplicationCellTag(object);
            if (Any(replicationFlags & EObjectReplicationFlags::ReplicateDestroy) &&
                replicationCellTag != NotReplicatedCellTag)
            {
                NProto::TReqRemoveForeignObject request;
                ToProto(request.mutable_object_id(), object->GetId());
                auto multicellManager = Bootstrap_->GetMulticellManager();
                multicellManager->PostToMaster(request, replicationCellTag);
            }
        }
    }
    return refCounter;
}

int TObjectManager::WeakRefObject(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YASSERT(!IsRecovery());
    YASSERT(object->IsTrunk());

    int weakRefCounter = object->WeakRefObject();
    if (weakRefCounter == 1) {
        ++LockedObjectCount_;
    }
    return weakRefCounter;
}

int TObjectManager::WeakUnrefObject(TObjectBase* object)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YASSERT(!IsRecovery());
    YASSERT(object->IsTrunk());

    int weakRefCounter = object->WeakUnrefObject();
    if (weakRefCounter == 0) {
        --LockedObjectCount_;
        if (!object->IsAlive()) {
            GarbageCollector_->DisposeGhost(object);
        }
    }
    return weakRefCounter;
}

void TObjectManager::SaveKeys(NCellMaster::TSaveContext& context) const
{
    SchemaMap_.SaveKeys(context);
}

void TObjectManager::SaveValues(NCellMaster::TSaveContext& context) const
{
    SchemaMap_.SaveValues(context);
    GarbageCollector_->Save(context);
}

void TObjectManager::LoadKeys(NCellMaster::TLoadContext& context)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    SchemaMap_.LoadKeys(context);
}

void TObjectManager::LoadValues(NCellMaster::TLoadContext& context)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    std::vector<TGuid> keysToRemove;

    SchemaMap_.LoadValues(context);
    for (const auto& pair : SchemaMap_) {
        auto type = TypeFromSchemaType(TypeFromId(pair.first));
        // COMPAT(sandello): CellNodeMap (408) and CellNode (410) are now obsolete.
        if (type == 408 || type == 410) {
            keysToRemove.push_back(pair.first);
            continue;
        }
        YCHECK(RegisteredTypes_.find(type) != RegisteredTypes_.end());
        auto& entry = TypeToEntry_[type];
        entry.SchemaObject = pair.second;
        entry.SchemaProxy = CreateSchemaProxy(Bootstrap_, entry.SchemaObject);
    }

    // COMPAT(sandello): CellNodeMap (408) and CellNode (410) are now obsolete.
    for (const auto& key : keysToRemove) {
        SchemaMap_.Remove(key);
    }

    GarbageCollector_->Load(context);
}

void TObjectManager::Clear()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    TMasterAutomatonPart::Clear();

    MasterObject_.reset(new TMasterObject(MasterObjectId_));
    MasterObject_->RefObject();

    MasterProxy_ = CreateMasterProxy(Bootstrap_, MasterObject_.get());

    GarbageCollector_->Clear();

    CreatedObjectCount_ = 0;
    DestroyedObjectCount_ = 0;
    LockedObjectCount_ = 0;

    SchemaMap_.Clear();

    for (auto type : RegisteredTypes_) {
        auto& entry = TypeToEntry_[type];
        if (HasSchema(type)) {
            auto id = MakeSchemaObjectId(type, Bootstrap_->GetPrimaryCellTag());
            auto schemaObjectHolder = std::make_unique<TSchemaObject>(id);
            entry.SchemaObject = SchemaMap_.Insert(id, std::move(schemaObjectHolder));
            entry.SchemaObject->RefObject();
            entry.SchemaProxy = CreateSchemaProxy(Bootstrap_, entry.SchemaObject);
        }
    }
}

void TObjectManager::OnRecoveryStarted()
{
    Profiler.SetEnabled(false);

    GarbageCollector_->Reset();
    LockedObjectCount_ = 0;

    for (auto type : RegisteredTypes_) {
        const auto& handler = GetHandler(type);
        LOG_INFO("Started resetting objects (Type: %v)", type);
        handler->ResetAllObjects();
        LOG_INFO("Finished resetting objects (Type: %v)", type);
    }
}

void TObjectManager::OnRecoveryComplete()
{
    Profiler.SetEnabled(true);
}

void TObjectManager::OnLeaderActive()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    GarbageCollector_->Start();
}

void TObjectManager::OnStopLeading()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    GarbageCollector_->Stop();
}

TObjectBase* TObjectManager::FindObject(const TObjectId& id)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto handler = FindHandler(TypeFromId(id));
    if (!handler) {
        return nullptr;
    }

    return handler->FindObject(id);
}

TObjectBase* TObjectManager::GetObject(const TObjectId& id)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto* object = FindObject(id);
    YCHECK(object);
    return object;
}

TObjectBase* TObjectManager::GetObjectOrThrow(const TObjectId& id)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto* object = FindObject(id);
    if (!IsObjectAlive(object)) {
        THROW_ERROR_EXCEPTION(
            NYTree::EErrorCode::ResolveError,
            "No such object %v",
            id);
    }

    return object;
}

IYPathServicePtr TObjectManager::CreateRemoteProxy(const TObjectId& id)
{
    return New<TRemoteProxy>(Bootstrap_, id);
}

IObjectProxyPtr TObjectManager::GetProxy(
    TObjectBase* object,
    TTransaction* transaction)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    YCHECK(IsObjectAlive(object));

    const auto& id = object->GetId();
    auto handler = FindHandler(TypeFromId(id));
    if (!handler) {
        return nullptr;
    }

    return handler->GetProxy(object, transaction);
}

void TObjectManager::BranchAttributes(
    const TObjectBase* /*originatingObject*/,
    TObjectBase* /*branchedObject*/)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);
    // We don't store empty deltas at the moment
}

void TObjectManager::MergeAttributes(
    TObjectBase* originatingObject,
    const TObjectBase* branchedObject)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    const auto* branchedAttributes = branchedObject->GetAttributes();
    if (!branchedAttributes)
        return;

    auto* originatingAttributes = originatingObject->GetMutableAttributes();
    for (const auto& pair : branchedAttributes->Attributes()) {
        if (!pair.second && originatingObject->IsTrunk()) {
            originatingAttributes->Attributes().erase(pair.first);
        } else {
            originatingAttributes->Attributes()[pair.first] = pair.second;
        }
    }

    if (originatingAttributes->Attributes().empty()) {
        originatingObject->ClearAttributes();
    }
}

void TObjectManager::FillAttributes(
    TObjectBase* object,
    const IAttributeDictionary& attributes)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto keys = attributes.List();
    if (keys.empty())
        return;

    auto proxy = GetProxy(object, nullptr);
    std::vector<ISystemAttributeProvider::TAttributeDescriptor> systemDescriptors;
    proxy->ListBuiltinAttributes(&systemDescriptors);

    yhash_set<Stroka> systemAttributeKeys;
    for (const auto& descriptor : systemDescriptors) {
        YCHECK(systemAttributeKeys.insert(descriptor.Key).second);
    }

    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        auto value = attributes.GetYson(key);
        if (systemAttributeKeys.find(key) == systemAttributeKeys.end()) {
            proxy->MutableAttributes()->SetYson(key, value);
        } else {
            if (!proxy->SetBuiltinAttribute(key, value)) {
                ThrowCannotSetBuiltinAttribute(key);
            }
        }
    }
}

TMutationPtr TObjectManager::CreateExecuteMutation(const NProto::TReqExecute& request)
{
    return CreateMutation(
        Bootstrap_->GetHydraFacade()->GetHydraManager(),
        request,
        this,
        &TObjectManager::HydraExecuteFollower);
}

TMutationPtr TObjectManager::CreateDestroyObjectsMutation(const NProto::TReqDestroyObjects& request)
{
    return CreateMutation(
        Bootstrap_->GetHydraFacade()->GetHydraManager(),
        request,
        this,
        &TObjectManager::HydraDestroyObjects);
}

TFuture<void> TObjectManager::GCCollect()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    return GarbageCollector_->Collect();
}

TObjectBase* TObjectManager::CreateObject(
    const TObjectId& hintId,
    TTransaction* transaction,
    TAccount* account,
    EObjectType type,
    IAttributeDictionary* attributes,
    const NObjectClient::NProto::TObjectCreationExtensions& extensions)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    std::unique_ptr<IAttributeDictionary> attributeHolder;
    if (!attributes) {
        attributeHolder = CreateEphemeralAttributes();
        attributes = attributeHolder.get();
    }

    auto handler = FindHandler(type);
    if (!handler) {
        THROW_ERROR_EXCEPTION("Unknown object type %v",
            type);
    }

    auto options = handler->GetCreationOptions();
    if (!options) {
        THROW_ERROR_EXCEPTION("Instances of type %Qlv cannot be created directly",
            type);
    }

    switch (options->TransactionMode) {
        case EObjectTransactionMode::Required:
            if (!transaction) {
                THROW_ERROR_EXCEPTION("Cannot create an instance of %Qlv outside of a transaction",
                    type);
            }
            break;

        case EObjectTransactionMode::Forbidden:
            if (transaction) {
                THROW_ERROR_EXCEPTION("Cannot create an instance of %Qlv inside of a transaction",
                    type);
            }
            break;

        case EObjectTransactionMode::Optional:
            break;

        default:
            YUNREACHABLE();
    }

    switch (options->AccountMode) {
        case EObjectAccountMode::Required:
            if (!account) {
                THROW_ERROR_EXCEPTION("Cannot create an instance of %Qlv without an account",
                    type);
            }
            break;

        case EObjectAccountMode::Forbidden:
            if (account) {
                THROW_ERROR_EXCEPTION("Cannot create an instance of %Qlv with an account",
                    type);
            }
            break;

        case EObjectAccountMode::Optional:
            break;

        default:
            YUNREACHABLE();
    }

    auto replicationFlags = handler->GetReplicationFlags();
    bool replicate =
        Bootstrap_->IsPrimaryMaster() &&
        Any(replicationFlags & EObjectReplicationFlags::ReplicateCreate);

    auto securityManager = Bootstrap_->GetSecurityManager();
    auto* user = securityManager->GetAuthenticatedUser();

    auto* schema = FindSchema(type);
    if (schema) {
        securityManager->ValidatePermission(schema, user, EPermission::Create);
    }

    // ITypeHandler::CreateObject may modify the attributes.
    std::unique_ptr<IAttributeDictionary> replicatedAttributes;
    if (replicate) {
        replicatedAttributes = attributes->Clone();
    }

    auto* object = handler->CreateObject(
        hintId,
        transaction,
        account,
        attributes,
        extensions);

    FillAttributes(object, *attributes);

    auto* stagingTransaction = handler->GetStagingTransaction(object);
    if (stagingTransaction) {
        YCHECK(transaction == stagingTransaction);
        auto transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->StageObject(transaction, object);
    } else {
        YCHECK(object->GetObjectRefCounter() > 0);
    }

    auto* acd = securityManager->FindAcd(object);
    if (acd) {
        acd->SetOwner(user);
    }

    if (replicate) {
        YASSERT(handler->GetReplicationCellTag(object) == AllSecondaryMastersCellTag);

        NProto::TReqCreateForeignObject replicationRequest;
        ToProto(replicationRequest.mutable_object_id(), object->GetId());
        if (transaction) {
            ToProto(replicationRequest.mutable_transaction_id(), transaction->GetId());
        }
        replicationRequest.set_type(static_cast<int>(type));
        ToProto(replicationRequest.mutable_object_attributes(), *replicatedAttributes);
        if (account) {
            ToProto(replicationRequest.mutable_account_id(), account->GetId());
        }

        auto multicellManager = Bootstrap_->GetMulticellManager();
        multicellManager->PostToSecondaryMasters(replicationRequest);
    }

    return object;
}

IObjectResolver* TObjectManager::GetObjectResolver()
{
    return ObjectResolver_.get();
}

bool TObjectManager::AdviceYield(NProfiling::TCpuInstant startInstant) const
{
    return NProfiling::GetCpuInstant() > startInstant + NProfiling::DurationToCpuDuration(Config_->YieldTimeout);
}

void TObjectManager::ValidatePrerequisites(const NObjectClient::NProto::TPrerequisitesExt& prerequisites)
{
    auto transactionManager = Bootstrap_->GetTransactionManager();
    auto cypressManager = Bootstrap_->GetCypressManager();

    auto getPrerequisiteTransaction = [&] (const TTransactionId& transactionId) {
        auto* transaction = transactionManager->FindTransaction(transactionId);
        if (!IsObjectAlive(transaction)) {
            THROW_ERROR_EXCEPTION(
                NObjectClient::EErrorCode::PrerequisiteCheckFailed,
                "Prerequisite check failed: transaction %v is missing",
                transactionId);
        }
        if (transaction->GetPersistentState() != ETransactionState::Active) {
            THROW_ERROR_EXCEPTION(
                NObjectClient::EErrorCode::PrerequisiteCheckFailed,
                "Prerequisite check failed: transaction %v is not active",
                transactionId);
        }
        return transaction;
    };

    for (const auto& prerequisite : prerequisites.transactions()) {
        auto transactionId = FromProto<TTransactionId>(prerequisite.transaction_id());
        getPrerequisiteTransaction(transactionId);
    }

    for (const auto& prerequisite : prerequisites.revisions()) {
        auto transactionId = FromProto<TTransactionId>(prerequisite.transaction_id());
        const auto& path = prerequisite.path();
        i64 revision = prerequisite.revision();

        auto* transaction = transactionId
            ? getPrerequisiteTransaction(transactionId)
            : nullptr;

        auto resolver = cypressManager->CreateResolver(transaction);
        INodePtr nodeProxy;
        try {
            nodeProxy = resolver->ResolvePath(path);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION(
                NObjectClient::EErrorCode::PrerequisiteCheckFailed,
                "Prerequisite check failed: failed to resolve path %v",
                path)
                << ex;
        }

        auto* cypressNodeProxy = ICypressNodeProxy::FromNode(nodeProxy.Get());
        auto* node = cypressNodeProxy->GetTrunkNode();
        if (node->GetRevision() != revision) {
            THROW_ERROR_EXCEPTION(
                NObjectClient::EErrorCode::PrerequisiteCheckFailed,
                "Prerequisite check failed: node %v revision mismatch: expected %v, found %v",
                path,
                revision,
                node->GetRevision());
        }
    }
}

TFuture<TSharedRefArray> TObjectManager::ForwardToLeader(
    TCellTag cellTag,
    TSharedRefArray requestMessage,
    TNullable<TDuration> timeout)
{
    LOG_DEBUG("Forwarding request to leader (CellTag: %v)",
        cellTag);

    auto securityManager = Bootstrap_->GetSecurityManager();
    auto* user = securityManager->GetAuthenticatedUser();

    auto multicellManager = Bootstrap_->GetMulticellManager();
    auto channel = multicellManager->GetMasterChannelOrThrow(
        cellTag,
        EPeerKind::Leader);

    TObjectServiceProxy proxy(std::move(channel));
    auto batchReq = proxy.ExecuteBatch();
    batchReq->SetUser(user->GetName());
    batchReq->AddRequestMessage(requestMessage);

    return batchReq->Invoke().Apply(BIND([] (const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError) {
        THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Request forwarding failed");

        LOG_DEBUG("Request forwarding succeeded");

        const auto& batchRsp = batchRspOrError.Value();
        return batchRsp->GetResponseMessage(0);
    }));
}

void TObjectManager::ReplicateObjectCreationToSecondaryMaster(
    TObjectBase* object,
    TCellTag cellTag)
{
    if (object->IsBuiltin()) {
        ReplicateObjectAttributesToSecondaryMaster(object, cellTag);
        return;
    }

    NProto::TReqCreateForeignObject request;
    ToProto(request.mutable_object_id(), object->GetId());
    request.set_type(static_cast<int>(object->GetType()));
    ToProto(request.mutable_object_attributes(), *GetReplicatedAttributes(object));

    auto handler = GetHandler(object);
    handler->PopulateObjectReplicationRequest(object, &request);

    auto multicellManager = Bootstrap_->GetMulticellManager();
    multicellManager->PostToMaster(request, cellTag);
}

void TObjectManager::ReplicateObjectAttributesToSecondaryMaster(
    TObjectBase* object,
    TCellTag cellTag)
{
    auto req = TYPathProxy::Set(FromObjectId(object->GetId()) + "/@");
    req->set_value(ConvertToYsonString(GetReplicatedAttributes(object)->ToMap()).Data());

    auto multicellManager = Bootstrap_->GetMulticellManager();
    multicellManager->PostToMaster(req, cellTag);
}

void TObjectManager::HydraExecuteLeader(
    const TUserId& userId,
    const TMutationId& mutationId,
    IServiceContextPtr context)
{
    NProfiling::TScopedTimer timer;

    auto securityManager = Bootstrap_->GetSecurityManager();

    try {
        auto* user = securityManager->GetUserOrThrow(userId);
        TAuthenticatedUserGuard userGuard(securityManager, user);
        ExecuteVerb(RootService_, context);
    } catch (const std::exception& ex) {
        context->Reply(ex);
    }

    if (IsLeader()) {
        auto* user = securityManager->FindUser(userId);
        if (IsObjectAlive(user)) {
            // NB: Charge for zero requests here since we've already charged the user for one request
            // in TObjectService.
            securityManager->ChargeUser(user, 0, TDuration(), timer.GetElapsed());
        }
    }

    if (mutationId) {
        auto responseKeeper = Bootstrap_->GetHydraFacade()->GetResponseKeeper();
        // NB: Context must already be replied by now.
        responseKeeper->EndRequest(mutationId, context->GetResponseMessage());
    }
}

void TObjectManager::HydraExecuteFollower(const NProto::TReqExecute& request)
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    auto userId = FromProto<TUserId>(request.user_id());

    std::vector<TSharedRef> parts(request.request_parts_size());
    for (int partIndex = 0; partIndex < request.request_parts_size(); ++partIndex) {
        parts[partIndex] = TSharedRef::FromString(request.request_parts(partIndex));
    }

    auto requestMessage = TSharedRefArray(std::move(parts));

    auto context = CreateYPathContext(std::move(requestMessage));

    auto mutationId = GetMutationId(context);

    HydraExecuteLeader(
        userId,
        mutationId,
        std::move(context));
}

void TObjectManager::HydraDestroyObjects(const NProto::TReqDestroyObjects& request)
{
    // NB: Ordered map is a must to make the behavior deterministic.
    std::map<TCellTag, NProto::TReqUnrefExportedObjects> unrefRequestMap;

    for (const auto& protoId : request.object_ids()) {
        auto id = FromProto<TObjectId>(protoId);
        auto type = TypeFromId(id);

        const auto& handler = GetHandler(type);
        auto* object = handler->FindObject(id);

        if (!object || object->GetObjectRefCounter() > 0)
            continue;

        if (IsForeign(object) && object->GetImportRefCounter() > 0) {
            auto& request = unrefRequestMap[CellTagFromId(id)];
            request.set_cell_tag(Bootstrap_->GetCellTag());
            auto* entry = request.add_entries();
            ToProto(entry->mutable_object_id(), id);
            entry->set_import_ref_counter(object->GetImportRefCounter());
        }

        // NB: The order of Dequeue/Destroy/CheckEmpty calls matters.
        // CheckEmpty will raise CollectPromise_ when GC queue becomes empty.
        // To enable cascaded GC sweep we don't want this to happen
        // if some ids are added during DestroyObject.
        GarbageCollector_->DestroyZombie(object);
        ++DestroyedObjectCount_;

        LOG_DEBUG_UNLESS(IsRecovery(), "Object destroyed (Type: %v, Id: %v)",
            type,
            id);
    }

    auto multicellManager = Bootstrap_->GetMulticellManager();
    for (const auto& pair : unrefRequestMap) {
        auto cellTag = pair.first;
        const auto& request = pair.second;
        multicellManager->PostToMaster(request, cellTag);
        LOG_DEBUG_UNLESS(IsRecovery(), "Requesting to unreference imported objects (CellTag: %v, Count: %v)",
            cellTag,
            request.entries_size());
    }

    GarbageCollector_->CheckEmpty();
}

void TObjectManager::HydraCreateForeignObject(const NProto::TReqCreateForeignObject& request) noexcept
{
    auto objectId = FromProto<TObjectId>(request.object_id());
    auto transactionId = request.has_transaction_id()
        ? FromProto<TTransactionId>(request.transaction_id())
        : NullTransactionId;
    auto accountId = request.has_account_id()
        ? FromProto<TAccountId>(request.account_id())
        : NullObjectId;
    auto type = EObjectType(request.type());

    auto transactionManager = Bootstrap_->GetTransactionManager();
    auto* transaction =  transactionId
        ? transactionManager->GetTransaction(transactionId)
        : nullptr;

    auto securityManager = Bootstrap_->GetSecurityManager();
    auto* account = accountId
        ? securityManager->GetAccount(accountId)
        : nullptr;

    auto attributes = request.has_object_attributes()
        ? FromProto(request.object_attributes())
        : std::unique_ptr<IAttributeDictionary>();

    LOG_DEBUG_UNLESS(IsRecovery(), "Creating foreign object (ObjectId: %v, TransactionId: %v, Type: %v, Account: %v)",
        objectId,
        transactionId,
        type,
        account ? MakeNullable(account->GetName()) : Null);

    CreateObject(
        objectId,
        transaction,
        account,
        type,
        attributes.get(),
        request.extensions());
}

void TObjectManager::HydraRemoveForeignObject(const NProto::TReqRemoveForeignObject& request) noexcept
{
    auto objectId = FromProto<TObjectId>(request.object_id());

    auto* object = FindObject(objectId);
    if (object) {
        LOG_DEBUG_UNLESS(IsRecovery(), "Removing foreign object (ObjectId: %v, RefCounter: %v)",
            objectId,
            object->GetObjectRefCounter());
        UnrefObject(object);
    } else {
        LOG_DEBUG_UNLESS(IsRecovery(), "Attempt to remove a non-existing foreign object (ObjectId: %v)",
            objectId);
    }
}

void TObjectManager::HydraUnrefExportedObjects(const NProto::TReqUnrefExportedObjects& request) noexcept
{
    auto cellTag = request.cell_tag();

    for (const auto& entry : request.entries()) {
        auto objectId = FromProto<TObjectId>(entry.object_id());
        auto importRefCounter = entry.import_ref_counter();

        auto* object = GetObject(objectId);
        UnrefObject(object, importRefCounter);

        const auto& handler = GetHandler(object);
        handler->UnexportObject(object, cellTag, importRefCounter);
    }

    LOG_DEBUG_UNLESS(IsRecovery(), "Exported objects unreferenced (CellTag: %v, Count: %v)",
        cellTag,
        request.entries_size());
}

const NProfiling::TProfiler& TObjectManager::GetProfiler()
{
    return Profiler;
}

NProfiling::TTagId TObjectManager::GetTypeTagId(EObjectType type)
{
    return TypeToEntry_[type].TagId;
}

NProfiling::TTagId TObjectManager::GetMethodTagId(const Stroka& method)
{
    auto it = MethodToTag_.find(method);
    if (it != MethodToTag_.end()) {
        return it->second;
    }
    auto tag = NProfiling::TProfileManager::Get()->RegisterTag("method", method);
    YCHECK(MethodToTag_.insert(std::make_pair(method, tag)).second);
    return tag;
}

void TObjectManager::OnProfiling()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    Profiler.Enqueue("/zombie_object_coun", GarbageCollector_->GetZombieCount());
    Profiler.Enqueue("/ghost_object_count", GarbageCollector_->GetGhostCount());
    Profiler.Enqueue("/created_object_count", CreatedObjectCount_);
    Profiler.Enqueue("/destroyed_object_count", DestroyedObjectCount_);
    Profiler.Enqueue("/locked_object_count", LockedObjectCount_);
}

std::unique_ptr<NYTree::IAttributeDictionary> TObjectManager::GetReplicatedAttributes(TObjectBase* object)
{
    YCHECK(!IsVersionedType(object->GetType()));

    auto handler = GetHandler(object);
    auto proxy = handler->GetProxy(object, nullptr);

    auto attributes = CreateEphemeralAttributes();
    yhash_set<Stroka> replicatedKeys;
    auto replicateKey = [&] (const Stroka& key, const TYsonString& value) {
        if (replicatedKeys.insert(key).second) {
            attributes->SetYson(key, value);
        }
    };

    // Check system attributes.
    std::vector<ISystemAttributeProvider::TAttributeDescriptor> descriptors;
    proxy->ListBuiltinAttributes(&descriptors);
    for (const auto& descriptor : descriptors) {
        if (!descriptor.Replicated)
            continue;

        auto key = Stroka(descriptor.Key);
        auto maybeValue = proxy->GetBuiltinAttribute(key);
        if (maybeValue) {
            replicateKey(key, *maybeValue);
        }
    }

    // Check custom attributes.
    const auto* customAttributes = object->GetAttributes();
    if (customAttributes) {
        for (const auto& pair : object->GetAttributes()->Attributes()) {
            replicateKey(pair.first, *pair.second);
        }
    }
    return attributes;
}

void TObjectManager::OnSecondaryMasterRegistered(TCellTag cellTag)
{
    auto schemas = GetValuesSortedByKey(SchemaMap_);
    for (auto* schema : schemas) {
        ReplicateObjectCreationToSecondaryMaster(schema, cellTag);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

