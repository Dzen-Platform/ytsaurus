#include "grafting_manager.h"

#include "rootstock_node.h"
#include "scion_node.h"

#include <yt/yt/server/master/cypress_server/proto/grafting_manager.pb.h>

#include <yt/yt/server/master/cell_master/automaton.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>

#include <yt/yt/server/master/security_server/helpers.h>

#include <yt/yt/server/master/transaction_server/public.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/server/lib/transaction_supervisor/helpers.h>

#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>

namespace NYT::NCypressServer {

using namespace NCellMaster;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NHydra;
using namespace NObjectClient;
using namespace NSecurityClient;
using namespace NSecurityServer;
using namespace NSequoiaServer;
using namespace NTransactionServer;
using namespace NTransactionSupervisor;
using namespace NYson;
using namespace NYTree;

using NCypressClient::NProto::TReqCreateRootstock;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = CypressServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TGraftingManager
    : public IGraftingManager
    , public TMasterAutomatonPart
{
public:
    explicit TGraftingManager(TBootstrap* bootstrap)
        : TMasterAutomatonPart(
            bootstrap,
            EAutomatonThreadQueue::GraftingManager)
    {
        RegisterLoader(
            "GraftingManager.Keys",
            BIND(&TGraftingManager::LoadKeys, Unretained(this)));
        RegisterLoader(
            "GraftingManager.Values",
            BIND(&TGraftingManager::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "GraftingManager.Keys",
            BIND(&TGraftingManager::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "GraftingManager.Values",
            BIND(&TGraftingManager::SaveValues, Unretained(this)));

        RegisterMethod(BIND(&TGraftingManager::HydraCreateScion, Unretained(this)));
        RegisterMethod(BIND(&TGraftingManager::HydraRemoveRootstock, Unretained(this)));
        RegisterMethod(BIND(&TGraftingManager::HydraRemoveScion, Unretained(this)));
    }

    void Initialize() override
    {
        const auto& configManager = Bootstrap_->GetConfigManager();
        configManager->SubscribeConfigChanged(BIND(&TGraftingManager::OnDynamicConfigChanged, MakeWeak(this)));

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->RegisterTransactionActionHandlers(
            MakeTransactionActionHandlerDescriptor(BIND_NO_PROPAGATE(&TGraftingManager::HydraCreateRootstock, Unretained(this))),
            MakeTransactionActionHandlerDescriptor(
                MakeEmptyTransactionActionHandler<TTransaction, TReqCreateRootstock, const NTransactionSupervisor::TTransactionCommitOptions&>()),
            MakeTransactionActionHandlerDescriptor(
                MakeEmptyTransactionActionHandler<TTransaction, TReqCreateRootstock, const NTransactionSupervisor::TTransactionAbortOptions&>()));
    }

    void OnRootstockCreated(
        TRootstockNode* rootstockNode,
        const IAttributeDictionary& inheritedAttributes,
        const IAttributeDictionary& explicitAttributes) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        EmplaceOrCrash(RootstockNodes_, rootstockNode->GetId(), rootstockNode);

        PostScionCreationMessage(rootstockNode, inheritedAttributes, explicitAttributes);
    }

    void OnRootstockDestroyed(TRootstockNode* rootstockNode) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        if (RootstockNodes_.erase(rootstockNode->GetId()) != 1) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                "Unknown rootstock destroyed, ignored (RootstockNodeId: %v, ScionNodeId: %v)",
                rootstockNode->GetId(),
                rootstockNode->GetScionId());
            return;
        }

        auto scionNodeId = rootstockNode->GetScionId();
        auto scionCellTag = CellTagFromId(scionNodeId);

        NProto::TReqRemoveScion scionRequest;
        ToProto(scionRequest.mutable_scion_node_id(), scionNodeId);
        if (scionCellTag == Bootstrap_->GetCellTag()) {
            HydraRemoveScion(&scionRequest);
        } else {
            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            multicellManager->PostToMaster(scionRequest, scionCellTag);
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Rootstock unregistered (RootstockNodeId: %v, ScionNodeId: %v)",
            rootstockNode->GetId(),
            scionNodeId);
    }

    void OnScionDestroyed(TScionNode* scionNode) override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        if (ScionNodes_.erase(scionNode->GetId()) != 1) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                "Unknown scion destroyed, ignored (ScionNodeId: %v, RootstockNodeId: %v)",
                scionNode->GetId(),
                scionNode->GetRootstockId());
            return;
        }

        if (ScionIdsToRemove_.erase(scionNode->GetId())) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                "Scion removed from removal queue (ScionNodeId: %v, RootstockNodeId: %v)",
                scionNode->GetId(),
                scionNode->GetRootstockId());
        }

        auto rootstockNodeId = scionNode->GetRootstockId();
        auto rootstockCellTag = CellTagFromId(rootstockNodeId);
        YT_VERIFY(rootstockCellTag == Bootstrap_->GetPrimaryCellTag());

        NProto::TReqRemoveRootstock rootstockRequest;
        ToProto(rootstockRequest.mutable_rootstock_node_id(), rootstockNodeId);
        if (rootstockCellTag == Bootstrap_->GetCellTag()) {
            HydraRemoveRootstock(&rootstockRequest);
        } else {
            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            multicellManager->PostToMaster(rootstockRequest, rootstockCellTag);
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Scion unregistered (ScionNodeId: %v, RootstockNodeId: %v)",
            scionNode->GetId(),
            scionNode->GetRootstockId());
    }

    const TRootstockNodeMap& RootstockNodes() override
    {
        Bootstrap_->VerifyPersistentStateRead();

        return RootstockNodes_;
    }

    const TScionNodeMap& ScionNodes() override
    {
        Bootstrap_->VerifyPersistentStateRead();

        return ScionNodes_;
    }

private:
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

    TRootstockNodeMap RootstockNodes_;
    TScionNodeMap ScionNodes_;

    THashSet<TNodeId> ScionIdsToRemove_;

    TPeriodicExecutorPtr ScionRemovalExecutor_;

    void OnLeaderActive() override
    {
        YT_VERIFY(!ScionRemovalExecutor_);
        ScionRemovalExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(EAutomatonThreadQueue::GraftingManager),
            BIND(&TGraftingManager::OnRemoveScions, MakeWeak(this)));
        ScionRemovalExecutor_->Start();
    }

    void OnStopLeading() override
    {
        if (ScionRemovalExecutor_) {
            ScionRemovalExecutor_->Stop();
            ScionRemovalExecutor_.Reset();
        }
    }

    void SaveKeys(NCellMaster::TSaveContext& /*context*/) const
    { }

    void SaveValues(NCellMaster::TSaveContext& context) const
    {
        using NYT::Save;

        Save(context, RootstockNodes_);
        Save(context, ScionNodes_);
        Save(context, ScionIdsToRemove_);
    }

    void LoadKeys(NCellMaster::TLoadContext& /*context*/)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        using NYT::Load;

        Load(context, RootstockNodes_);
        Load(context, ScionNodes_);
        Load(context, ScionIdsToRemove_);
    }

    void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::Clear();

        RootstockNodes_.clear();
        ScionNodes_.clear();
        ScionIdsToRemove_.clear();
    }

    void OnDynamicConfigChanged(TDynamicClusterConfigPtr /*oldConfig*/)
    {
        const auto& configManager = Bootstrap_->GetConfigManager();
        const auto& config = configManager->GetConfig()->CypressManager;
        if (ScionRemovalExecutor_) {
            ScionRemovalExecutor_->SetPeriod(config->ScionRemovalPeriod);
        }
    }

    void OnRemoveScions()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (ScionIdsToRemove_.empty()) {
            YT_LOG_DEBUG("Skipping scions removal iteration since there are no enqueued scions");
            return;
        }

        auto scionNodeId = *ScionIdsToRemove_.begin();
        YT_LOG_DEBUG("Scion removal started (ScionNodeId: %v)",
            scionNodeId);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto rootService = objectManager->GetRootService();
        auto removeReq = TYPathProxy::Remove(FromObjectId(scionNodeId));
        removeReq->set_force(true);
        removeReq->set_recursive(true);

        auto rspOrError = WaitFor(ExecuteVerb(rootService, removeReq));
        if (rspOrError.IsOK()) {
            YT_LOG_DEBUG("Scion removal completed (ScionNodeId: %v)",
                scionNodeId);
        } else {
            YT_LOG_WARNING(rspOrError,
                "Failed to remove scion (ScionNodeId: %v)",
                scionNodeId);
        }
    }

    void HydraCreateRootstock(
        TTransaction* /*transaction*/,
        TReqCreateRootstock* request,
        const NTransactionSupervisor::TTransactionPrepareOptions& options)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(options.Persistent);
        YT_VERIFY(options.LatePrepare);

        auto req = TCypressYPathProxy::Create(request->path());
        req->CopyFrom(request->request());

        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto& rootService = objectManager->GetRootService();
        auto rsp = SyncExecuteVerb(rootService, req);
        auto rootstockNodeId = FromProto<TNodeId>(rsp->node_id());

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        // TODO: Support creation in transaction.
        auto* rootstockNode = cypressManager
            ->GetNode(TVersionedNodeId(rootstockNodeId))
            ->As<TRootstockNode>();
        YT_VERIFY(rootstockNode->GetId() == rootstockNodeId);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Rootstock created (RootstockId: %v, ScionId: %v)",
            rootstockNode->GetId(),
            rootstockNode->GetScionId());
    }

    // NB: This function should not throw since rootstock is already created.
    void PostScionCreationMessage(
        TRootstockNode* rootstockNode,
        const IAttributeDictionary& inheritedAttributes,
        const IAttributeDictionary& explicitAttributes) noexcept
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        auto* trunkNode = rootstockNode->GetTrunkNode()->As<TScionNode>();
        auto* transaction = rootstockNode->GetTransaction();

        NProto::TReqCreateScion request;
        ToProto(request.mutable_scion_node_id(), rootstockNode->GetScionId());
        ToProto(request.mutable_rootstock_node_id(), rootstockNode->GetId());
        ToProto(request.mutable_account_id(), rootstockNode->Account()->GetId());
        ToProto(request.mutable_explicit_node_attributes(), explicitAttributes);
        ToProto(request.mutable_inherited_node_attributes(), inheritedAttributes);
        ToProto(request.mutable_parent_id(), rootstockNode->GetParent()->GetId());

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto path = cypressManager->GetNodePath(trunkNode, transaction);
        request.set_path(path);

        if (auto key = FindNodeKey(cypressManager, trunkNode, transaction)) {
            request.set_key(*key);
        }

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        auto effectiveAcl = securityManager->GetEffectiveAcl(trunkNode);
        request.set_effective_acl(ConvertToYsonString(effectiveAcl).ToString());

        const auto& directAcd = trunkNode->Acd();
        request.set_direct_acl(ConvertToYsonString(directAcd.Acl()).ToString());
        request.set_inherit_acl(directAcd.GetInherit());

        if (auto effectiveAnnotation = GetEffectiveAnnotation(rootstockNode)) {
            auto* annotationNode = FindClosestAncestorWithAnnotation(rootstockNode);
            YT_VERIFY(annotationNode);

            request.set_effective_annotation(*effectiveAnnotation);
            auto annotationPath = cypressManager->GetNodePath(annotationNode, transaction);
            request.set_effective_annotation_path(annotationPath);
        }

        auto scionNodeId = rootstockNode->GetScionId();
        auto scionCellTag = CellTagFromId(scionNodeId);

        if (scionCellTag == Bootstrap_->GetCellTag()) {
            HydraCreateScion(&request);
        } else {
            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            multicellManager->PostToMaster(request, scionCellTag);
        }
    }

    static void SanitizeScionExplicitAttributes(IAttributeDictionary* attributes)
    {
        for (auto attr : {
            EInternedAttributeKey::Acl,
            EInternedAttributeKey::Annotation,
            EInternedAttributeKey::InheritAcl,
            EInternedAttributeKey::Owner})
        {
            attributes->Remove(attr.Unintern());
        }
    }

    void HydraCreateScion(NProto::TReqCreateScion* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        auto rootstockNodeId = FromProto<TNodeId>(request->rootstock_node_id());
        auto scionNodeId = FromProto<TNodeId>(request->scion_node_id());

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        auto accountId = FromProto<TAccountId>(request->account_id());
        auto* account = securityManager->GetAccountOrThrow(accountId);

        auto explicitAttributes = FromProto(request->explicit_node_attributes());

        auto inheritedAttributes = FromProto(request->inherited_node_attributes());
        auto effectiveInheritableAttributes = New<TInheritedAttributeDictionary>(Bootstrap_);
        if (inheritedAttributes) {
            effectiveInheritableAttributes->MergeFrom(*inheritedAttributes);
        } else {
            effectiveInheritableAttributes.Reset();
        }

        const auto& path = request->path();

        auto parentId = FromProto<TNodeId>(request->parent_id());
        const auto& key = request->key();

        auto effectiveAcl = DeserializeAcl(
            TYsonString(request->effective_acl()),
            securityManager);
        auto directAcl = request->has_direct_acl()
            ? std::optional(DeserializeAcl(
                TYsonString(request->direct_acl()),
                securityManager))
            : std::nullopt;
        auto inheritAcl = request->inherit_acl();

        auto effectiveAnnotation = request->has_effective_annotation()
            ? std::optional(request->effective_annotation())
            : std::nullopt;
        std::optional<NYPath::TYPath> effectiveAnnotationPath;
        if (request->has_effective_annotation_path()) {
            effectiveAnnotationPath = request->effective_annotation_path();
        } else if (effectiveAnnotation) {
            effectiveAnnotationPath = path;
        }

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        const auto& typeHandler = cypressManager->GetHandler(EObjectType::Scion);
        auto* shard = cypressManager->GetRootCypressShard();
        auto* scionNode = cypressManager->CreateNode(
            typeHandler,
            scionNodeId,
            TCreateNodeContext{
                .ExternalCellTag = NotReplicatedCellTagSentinel,
                .InheritedAttributes = inheritedAttributes.Get(),
                .ExplicitAttributes = explicitAttributes.Get(),
                .Account = account,
                .Shard = shard,
            })->As<TScionNode>();
        YT_VERIFY(scionNode->GetId() == scionNodeId);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(scionNode);

        cypressManager->SetShard(scionNode, shard);

        if (effectiveInheritableAttributes) {
            scionNode->EffectiveInheritableAttributes().emplace(effectiveInheritableAttributes->Attributes().ToPersistent());
        }

        scionNode->SetPath(path);
        scionNode->SetParentId(parentId);
        scionNode->SetKey(key);

        scionNode->Acd().SetEntries(effectiveAcl);
        scionNode->Acd().SetInherit(inheritAcl);
        if (directAcl) {
            scionNode->DirectAcd().SetEntries(*directAcl);
        }

        if (auto ownerName = explicitAttributes->FindAndRemove<TString>(EInternedAttributeKey::Owner.Unintern())) {
            if (auto* owner = securityManager->FindSubjectByNameOrAlias(*ownerName, /*activeLifeStageOnly*/ true)) {
                scionNode->Acd().SetOwner(owner);
            } else {
                YT_LOG_ALERT("Scion owner subject is missing (ScionNodeId: %v, SubjectName: %v)",
                    scionNode->GetId(),
                    ownerName);
            }
        }

        SanitizeScionExplicitAttributes(explicitAttributes.Get());
        try {
            typeHandler->FillAttributes(scionNode, inheritedAttributes.Get(), explicitAttributes.Get());
        } catch (const std::exception& ex) {
            YT_LOG_ALERT(ex, "Failed to set scion attributes during creation "
                "(RootstockNodeId: %v, ScionNodeId: %v)",
                rootstockNodeId,
                scionNodeId);
        }

        if (effectiveAnnotation) {
            scionNode->SetAnnotation(*effectiveAnnotation);
        } else {
            scionNode->RemoveAnnotation();
        }
        scionNode->EffectiveAnnotationPath() = std::move(effectiveAnnotationPath);

        scionNode->SetRootstockId(rootstockNodeId);

        EmplaceOrCrash(ScionNodes_, scionNodeId, scionNode);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Scion created "
            "(RootstockNodeId: %v, ScionNodeId: %v)",
            rootstockNodeId,
            scionNodeId);
    }

    void HydraRemoveRootstock(NProto::TReqRemoveRootstock* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        auto rootstockNodeId = FromProto<TNodeId>(request->rootstock_node_id());

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* rootstockNode = cypressManager
            ->FindNode(TVersionedObjectId(rootstockNodeId))
            ->As<TRootstockNode>();
        if (!IsObjectAlive(rootstockNode)) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                "Attempted to remove a non-existing rootstock, ignored (RootstockNodeId: %v)",
                rootstockNodeId);
            return;
        }

        auto* parentNode = rootstockNode->GetParent();
        if (!parentNode) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                "Attempted to remove rootstock that is already detached from a parent, ignored "
                "(RootstockNodeId: %v)",
                rootstockNodeId);
            return;
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Detaching rootstock from parent for future removal "
            "RootstockNodeId: %v, ParentNodeId: %v)",
            rootstockNode->GetId(),
            parentNode->GetId());

        auto rootstockProxy = cypressManager->GetNodeProxy(rootstockNode);
        auto parentProxy = cypressManager->GetNodeProxy(parentNode)->AsComposite();
        parentProxy->RemoveChild(rootstockProxy);
    }

    void HydraRemoveScion(NProto::TReqRemoveScion* request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasMutationContext());

        auto scionNodeId = FromProto<TNodeId>(request->scion_node_id());

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* scionNode = cypressManager
            ->FindNode(TVersionedNodeId(scionNodeId))
            ->As<TScionNode>();
        if (!IsObjectAlive(scionNode)) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                "Attempted to remove a non-existing scion, ignored (ScionNodeId: %v)",
                scionNodeId);
            return;
        }

        if (scionNode->GetRemovalStarted()) {
            YT_LOG_ALERT("Attempted to remove scion for which removal is "
                "already started, ignored (ScionNodeId: %v, RootstockNodeId: %v)",
                scionNodeId,
                scionNode->GetRootstockId());
            return;
        }

        scionNode->SetRemovalStarted(true);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Adding scion to removal queue "
            "(ScionNodeId: %v, RootstockNodeId: %v)",
            scionNodeId,
            scionNode->GetRootstockId());

        InsertOrCrash(ScionIdsToRemove_, scionNode->GetId());
    }
};

////////////////////////////////////////////////////////////////////////////////

IGraftingManagerPtr CreateGraftingManager(TBootstrap* bootstrap)
{
    return New<TGraftingManager>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
