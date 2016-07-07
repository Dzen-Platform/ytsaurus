#include "chunk_manager.h"
#include "private.h"
#include "chunk.h"
#include "chunk_list.h"
#include "chunk_list_proxy.h"
#include "chunk_owner_base.h"
#include "chunk_placement.h"
#include "chunk_proxy.h"
#include "chunk_replicator.h"
#include "chunk_sealer.h"
#include "chunk_tree_balancer.h"
#include "config.h"
#include "helpers.h"
#include "job.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>
#include <yt/server/cell_master/multicell_manager.h>
#include <yt/server/cell_master/serialize.h>

#include <yt/server/chunk_server/chunk_manager.pb.h>

#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/hydra/composite_automaton.h>
#include <yt/server/hydra/entity_map.h>

#include <yt/server/node_tracker_server/config.h>
#include <yt/server/node_tracker_server/node_directory_builder.h>
#include <yt/server/node_tracker_server/node_tracker.h>

#include <yt/server/object_server/object_manager.h>
#include <yt/server/object_server/type_handler_detail.h>

#include <yt/server/security_server/account.h>
#include <yt/server/security_server/group.h>
#include <yt/server/security_server/security_manager.h>

#include <yt/server/transaction_server/transaction.h>
#include <yt/server/transaction_server/transaction_manager.h>

#include <yt/server/journal_server/journal_node.h>
#include <yt/server/journal_server/journal_manager.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/schema.h>
#include <yt/ytlib/chunk_client/chunk_service.pb.h>

#include <yt/ytlib/journal_client/helpers.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/core/compression/codec.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/string.h>

#include <yt/core/profiling/profile_manager.h>

namespace NYT {
namespace NChunkServer {

using namespace NConcurrency;
using namespace NRpc;
using namespace NHydra;
using namespace NNodeTrackerServer;
using namespace NTransactionServer;
using namespace NObjectServer;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NYTree;
using namespace NCellMaster;
using namespace NCypressServer;
using namespace NSecurityServer;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJournalClient;
using namespace NJournalServer;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ChunkServerLogger;
static const auto ProfilingPeriod = TDuration::MilliSeconds(1000);
// NB: Changing this value will invalidate all changelogs!
static const auto ReplicaApproveTimeout = TDuration::Seconds(60);

////////////////////////////////////////////////////////////////////////////////

class TChunkTreeBalancerCallbacks
    : public IChunkTreeBalancerCallbacks
{
public:
    explicit TChunkTreeBalancerCallbacks(NCellMaster::TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    { }

    virtual void RefObject(TObjectBase* object) override
    {
        Bootstrap_->GetObjectManager()->RefObject(object);
    }

    virtual void UnrefObject(TObjectBase* object) override
    {
        Bootstrap_->GetObjectManager()->UnrefObject(object);
    }

    virtual TChunkList* CreateChunkList() override
    {
        return Bootstrap_->GetChunkManager()->CreateChunkList();
    }

    virtual void ClearChunkList(TChunkList* chunkList) override
    {
        Bootstrap_->GetChunkManager()->ClearChunkList(chunkList);
    }

    virtual void AttachToChunkList(
        TChunkList* chunkList,
        const std::vector<TChunkTree*>& children) override
    {
        Bootstrap_->GetChunkManager()->AttachToChunkList(chunkList, children);
    }

    virtual void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree* child) override
    {
        Bootstrap_->GetChunkManager()->AttachToChunkList(chunkList, child);
    }

    virtual void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree** childrenBegin,
        TChunkTree** childrenEnd) override
    {
        Bootstrap_->GetChunkManager()->AttachToChunkList(chunkList, childrenBegin, childrenEnd);
    }

private:
    NCellMaster::TBootstrap* const Bootstrap_;

};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TChunkTypeHandlerBase
    : public TObjectTypeHandlerWithMapBase<TChunk>
{
public:
    explicit TChunkTypeHandlerBase(TImpl* owner);

    virtual void ResetAllObjects() override
    {
        // NB: All chunk type handlers share the same map.
        // No need to reset chunks multiple times.
        if (GetType() == EObjectType::Chunk) {
            TObjectTypeHandlerWithMapBase::ResetAllObjects();
        }
    }

    virtual TObjectBase* FindObject(const TObjectId& id) override
    {
        return Map_->Find(DecodeChunkId(id).Id);
    }

protected:
    TImpl* const Owner_;


    virtual IObjectProxyPtr DoGetProxy(TChunk* chunk, TTransaction* transaction) override;

    virtual void DoDestroyObject(TChunk* chunk) override;

    virtual void DoUnstageObject(TChunk* chunk, bool recursive) override;

    virtual void DoResetObject(TChunk* chunk) override
    {
        TObjectTypeHandlerWithMapBase::DoResetObject(chunk);
        chunk->Reset();
    }

    virtual void DoExportObject(
        TChunk* chunk,
        TCellTag destinationCellTag) override
    {
        auto multicellManager = Bootstrap_->GetMulticellManager();
        auto cellIndex = multicellManager->GetRegisteredMasterCellIndex(destinationCellTag);
        chunk->Export(cellIndex);
    }

    virtual void DoUnexportObject(
        TChunk* chunk,
        TCellTag destinationCellTag,
        int importRefCounter) override
    {
        auto multicellManager = Bootstrap_->GetMulticellManager();
        auto cellIndex = multicellManager->GetRegisteredMasterCellIndex(destinationCellTag);
        chunk->Unexport(cellIndex, importRefCounter);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TRegularChunkTypeHandler
    : public TChunkTypeHandlerBase
{
public:
    explicit TRegularChunkTypeHandler(TImpl* owner)
        : TChunkTypeHandlerBase(owner)
    { }

    virtual EObjectType GetType() const override
    {
        return EObjectType::Chunk;
    }

private:
    virtual Stroka DoGetName(const TChunk* chunk) override
    {
        return Format("chunk %v", chunk->GetId());
    }

};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TErasureChunkTypeHandler
    : public TChunkTypeHandlerBase
{
public:
    TErasureChunkTypeHandler(TImpl* owner, EObjectType type)
        : TChunkTypeHandlerBase(owner)
        , Type_(type)
    { }

    virtual EObjectType GetType() const override
    {
        return Type_;
    }

private:
    const EObjectType Type_;

    virtual Stroka DoGetName(const TChunk* chunk) override
    {
        return Format("erasure chunk %v", chunk->GetId());
    }

};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TJournalChunkTypeHandler
    : public TChunkTypeHandlerBase
{
public:
    explicit TJournalChunkTypeHandler(TImpl* owner)
        : TChunkTypeHandlerBase(owner)
    { }

    virtual EObjectType GetType() const override
    {
        return EObjectType::JournalChunk;
    }

private:
    virtual Stroka DoGetName(const TChunk* chunk) override
    {
        return Format("journal chunk %v", chunk->GetId());
    }

};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TChunkListTypeHandler
    : public TObjectTypeHandlerWithMapBase<TChunkList>
{
public:
    explicit TChunkListTypeHandler(TImpl* owner);

    virtual EObjectType GetType() const override
    {
        return EObjectType::ChunkList;
    }

private:
    TImpl* const Owner_;


    virtual Stroka DoGetName(const TChunkList* chunkList) override
    {
        return Format("chunk list %v", chunkList->GetId());
    }

    virtual IObjectProxyPtr DoGetProxy(TChunkList* chunkList, TTransaction* transaction) override;

    virtual void DoDestroyObject(TChunkList* chunkList) override;

    virtual void DoUnstageObject(TChunkList* chunkList, bool recursive) override;
};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TImpl
    : public TMasterAutomatonPart
{
public:
    TImpl(
        TChunkManagerConfigPtr config,
        TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap)
        , Config_(config)
        , ChunkTreeBalancer_(New<TChunkTreeBalancerCallbacks>(Bootstrap_))
    {
        RegisterMethod(BIND(&TImpl::HydraUpdateChunkProperties, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraExportChunks, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraImportChunks, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraExecuteBatch, Unretained(this)));

        RegisterLoader(
            "ChunkManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "ChunkManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "ChunkManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "ChunkManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));

        auto* profileManager = NProfiling::TProfileManager::Get();
        Profiler.TagIds().push_back(profileManager->RegisterTag("cell_tag", Bootstrap_->GetCellTag()));
    }

    void Initialize()
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(New<TRegularChunkTypeHandler>(this));
        objectManager->RegisterHandler(New<TErasureChunkTypeHandler>(this, EObjectType::ErasureChunk));
        for (auto type = MinErasureChunkPartType;
             type <= MaxErasureChunkPartType;
             type = static_cast<EObjectType>(static_cast<int>(type) + 1))
        {
            objectManager->RegisterHandler(New<TErasureChunkTypeHandler>(this, type));
        }
        objectManager->RegisterHandler(New<TJournalChunkTypeHandler>(this));
        objectManager->RegisterHandler(New<TChunkListTypeHandler>(this));

        auto nodeTracker = Bootstrap_->GetNodeTracker();
        nodeTracker->SubscribeNodeRegistered(BIND(&TImpl::OnNodeRegistered, MakeWeak(this)));
        nodeTracker->SubscribeNodeUnregistered(BIND(&TImpl::OnNodeUnregistered, MakeWeak(this)));
        nodeTracker->SubscribeNodeDisposed(BIND(&TImpl::OnNodeDisposed, MakeWeak(this)));
        nodeTracker->SubscribeNodeRackChanged(BIND(&TImpl::OnNodeChanged, MakeWeak(this)));
        nodeTracker->SubscribeNodeDecommissionChanged(BIND(&TImpl::OnNodeChanged, MakeWeak(this)));
        nodeTracker->SubscribeFullHeartbeat(BIND(&TImpl::OnFullHeartbeat, MakeWeak(this)));
        nodeTracker->SubscribeIncrementalHeartbeat(BIND(&TImpl::OnIncrementalHeartbeat, MakeWeak(this)));

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(),
            BIND(&TImpl::OnProfiling, MakeWeak(this)),
            ProfilingPeriod);
        ProfilingExecutor_->Start();
    }


    TMutationPtr CreateUpdateChunkPropertiesMutation(const NProto::TReqUpdateChunkProperties& request)
    {
        return CreateMutation(
            Bootstrap_->GetHydraFacade()->GetHydraManager(),
            request,
            &TImpl::HydraUpdateChunkProperties,
            this);
    }

    TMutationPtr CreateExportChunksMutation(TCtxExportChunksPtr context)
    {
        return CreateMutation(
            Bootstrap_->GetHydraFacade()->GetHydraManager(),
            std::move(context),
            &TImpl::HydraExportChunks,
            this);
    }

    TMutationPtr CreateImportChunksMutation(TCtxImportChunksPtr context)
    {
        return CreateMutation(
            Bootstrap_->GetHydraFacade()->GetHydraManager(),
            std::move(context),
            &TImpl::HydraImportChunks,
            this);
    }

    TMutationPtr CreateExecuteBatchMutation(TCtxExecuteBatchPtr context)
    {
        return CreateMutation(
            Bootstrap_->GetHydraFacade()->GetHydraManager(),
            std::move(context),
            &TImpl::HydraExecuteBatch,
            this);
    }

    TNodeList AllocateWriteTargets(
        TChunk* chunk,
        int desiredCount,
        int minCount,
        TNullable<int> replicationFactorOverride,
        const TNodeList* forbiddenNodes,
        const TNullable<Stroka>& preferredHostName)
    {
        return ChunkPlacement_->AllocateWriteTargets(
            chunk,
            desiredCount,
            minCount,
            replicationFactorOverride,
            forbiddenNodes,
            preferredHostName,
            ESessionType::User);
    }

    void ConfirmChunk(
        TChunk* chunk,
        const NChunkClient::TChunkReplicaList& replicas,
        TChunkInfo* chunkInfo,
        TChunkMeta* chunkMeta)
    {
        const auto& id = chunk->GetId();

        if (chunk->IsConfirmed()) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk is already confirmed (ChunkId: %v)",
                id);
            return;
        }

        chunk->Confirm(chunkInfo, chunkMeta);

        auto nodeTracker = Bootstrap_->GetNodeTracker();

        const auto* mutationContext = GetCurrentMutationContext();
        auto mutationTimestamp = mutationContext->GetTimestamp();

        for (auto replica : replicas) {
            auto* node = nodeTracker->FindNode(replica.GetNodeId());
            if (!IsObjectAlive(node)) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Tried to confirm chunk %v at an unknown node %v",
                    id,
                    replica.GetNodeId());
                continue;
            }

            auto chunkWithIndex = chunk->IsJournal()
                ? TChunkPtrWithIndex(chunk, ActiveChunkReplicaIndex)
                : TChunkPtrWithIndex(chunk, replica.GetIndex());

            if (node->GetLocalState() != ENodeState::Online) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Tried to confirm chunk %v at %v which has invalid state %Qlv",
                    id,
                    node->GetDefaultAddress(),
                    node->GetLocalState());
                continue;
            }

            if (!node->HasReplica(chunkWithIndex, false)) {
                AddChunkReplica(
                    node,
                    chunkWithIndex,
                    false,
                    EAddReplicaReason::Confirmation);
                node->AddUnapprovedReplica(chunkWithIndex, mutationTimestamp);
            }
        }

        // NB: This is true for non-journal chunks.
        if (chunk->IsSealed()) {
            OnChunkSealed(chunk);
        }

        // Increase staged resource usage.
        if (chunk->IsStaged() && !chunk->IsJournal()) {
            auto* stagingTransaction = chunk->GetStagingTransaction();
            auto* stagingAccount = chunk->GetStagingAccount();
            auto securityManager = Bootstrap_->GetSecurityManager();
            auto delta = chunk->GetResourceUsage();
            securityManager->UpdateAccountStagingUsage(stagingTransaction, stagingAccount, delta);
        }

        ScheduleChunkRefresh(chunk);
    }

    void SealChunk(TChunk* chunk, const TMiscExt& miscExt)
    {
        if (!chunk->IsJournal()) {
            THROW_ERROR_EXCEPTION("Not a journal chunk");
        }

        if (!chunk->IsConfirmed()) {
            THROW_ERROR_EXCEPTION("Chunk is not confirmed");
        }

        if (chunk->IsSealed()) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk is already sealed (ChunkId: %v)",
                chunk->GetId());
            return;
        }

        chunk->Seal(miscExt);
        OnChunkSealed(chunk);

        ScheduleChunkRefresh(chunk);
    }

    TChunkList* CreateChunkList()
    {
        ++ChunkListsCreated_;
        auto objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::ChunkList, NullObjectId);
        auto chunkListHolder = std::make_unique<TChunkList>(id);
        auto* chunk = ChunkListMap_.Insert(id, std::move(chunkListHolder));
        LOG_DEBUG_UNLESS(IsRecovery(), "Chunk list created (Id: %v)",
            id);
        return chunk;
    }


    void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree** childrenBegin,
        TChunkTree** childrenEnd)
    {
        if (childrenBegin == childrenEnd)
            return;

        auto objectManager = Bootstrap_->GetObjectManager();
        NChunkServer::AttachToChunkList(
            chunkList,
            childrenBegin,
            childrenEnd,
            [&] (TChunkTree* chunkTree) {
                objectManager->RefObject(chunkTree);
            });
    }

    void AttachToChunkList(
        TChunkList* chunkList,
        const std::vector<TChunkTree*>& children)
    {
        AttachToChunkList(
            chunkList,
            const_cast<TChunkTree**>(children.data()),
            const_cast<TChunkTree**>(children.data() + children.size()));
    }

    void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree* child)
    {
        AttachToChunkList(
            chunkList,
            &child,
            &child + 1);
    }


    void DetachFromChunkList(
        TChunkList* chunkList,
        TChunkTree** childrenBegin,
        TChunkTree** childrenEnd)
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        NChunkServer::DetachFromChunkList(
            chunkList,
            childrenBegin,
            childrenEnd,
            [&] (TChunkTree* chunkTree) {
                objectManager->UnrefObject(chunkTree);
            });
    }

    void DetachFromChunkList(
        TChunkList* chunkList,
        const std::vector<TChunkTree*>& children)
    {
        DetachFromChunkList(
            chunkList,
            const_cast<TChunkTree**>(children.data()),
            const_cast<TChunkTree**>(children.data() + children.size()));
    }

    void DetachFromChunkList(
        TChunkList* chunkList,
        TChunkTree* child)
    {
        DetachFromChunkList(
            chunkList,
            &child,
            &child + 1);
    }


    void RebalanceChunkTree(TChunkList* chunkList)
    {
        if (!ChunkTreeBalancer_.IsRebalanceNeeded(chunkList))
            return;

        PROFILE_TIMING ("/chunk_tree_rebalance_time") {
            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk tree rebalancing started (RootId: %v)",
                chunkList->GetId());
            ChunkTreeBalancer_.Rebalance(chunkList);
            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk tree rebalancing completed");
        }
    }


    void StageChunkTree(TChunkTree* chunkTree, TTransaction* transaction, TAccount* account)
    {
        Y_ASSERT(transaction);
        Y_ASSERT(!chunkTree->IsStaged());

        chunkTree->SetStagingTransaction(transaction);

        if (account) {
            chunkTree->SetStagingAccount(account);
            auto objectManager = Bootstrap_->GetObjectManager();
            objectManager->RefObject(account);
        }
    }

    void UnstageChunk(TChunk* chunk)
    {
        auto* transaction = chunk->GetStagingTransaction();
        auto* account = chunk->GetStagingAccount();

        if (account) {
            auto objectManager = Bootstrap_->GetObjectManager();
            objectManager->UnrefObject(account);
        }

        if (account && chunk->IsConfirmed() && !chunk->IsJournal()) {
            auto securityManager = Bootstrap_->GetSecurityManager();
            auto delta = -chunk->GetResourceUsage();
            securityManager->UpdateAccountStagingUsage(transaction, account, delta);
        }

        chunk->SetStagingTransaction(nullptr);
        chunk->SetStagingAccount(nullptr);
    }

    void UnstageChunkList(TChunkList* chunkList, bool recursive)
    {
        auto* account = chunkList->GetStagingAccount();
        if (account) {
            auto objectManager = Bootstrap_->GetObjectManager();
            objectManager->UnrefObject(account);
        }

        chunkList->SetStagingTransaction(nullptr);
        chunkList->SetStagingAccount(nullptr);

        if (recursive) {
            auto transactionManager = Bootstrap_->GetTransactionManager();
            for (auto* child : chunkList->Children()) {
                transactionManager->UnstageObject(child->GetStagingTransaction(), child, recursive);
            }
        }
    }


    TNodePtrWithIndexList LocateChunk(TChunkPtrWithIndex chunkWithIndex)
    {
        auto* chunk = chunkWithIndex.GetPtr();
        int index = chunkWithIndex.GetIndex();

        if (ChunkReplicator_) {
            ChunkReplicator_->TouchChunk(chunk);
        }

        TNodePtrWithIndexList result;
        auto replicas = chunk->GetReplicas();
        for (auto replica : replicas) {
            if (index == AllChunkReplicasIndex || replica.GetIndex() == index) {
                result.push_back(replica);
            }
        }

        return result;
    }


    void ClearChunkList(TChunkList* chunkList)
    {
        // TODO(babenko): currently we only support clearing a chunklist with no parents.
        YCHECK(chunkList->Parents().empty());
        chunkList->IncrementVersion();

        auto objectManager = Bootstrap_->GetObjectManager();
        for (auto* child : chunkList->Children()) {
            ResetChunkTreeParent(chunkList, child);
            objectManager->UnrefObject(child);
        }

        chunkList->Children().clear();
        ResetChunkListStatistics(chunkList);

        LOG_DEBUG_UNLESS(IsRecovery(), "Chunk list cleared (ChunkListId: %v)", chunkList->GetId());
    }


    TJobPtr FindJob(const TJobId& id)
    {
        return ChunkReplicator_->FindJob(id);
    }


    void ScheduleJobs(
        TNode* node,
        const std::vector<TJobPtr>& currentJobs,
        std::vector<TJobPtr>* jobsToStart,
        std::vector<TJobPtr>* jobsToAbort,
        std::vector<TJobPtr>* jobsToRemove)
    {
        ChunkReplicator_->ScheduleJobs(
            node,
            currentJobs,
            jobsToStart,
            jobsToAbort,
            jobsToRemove);
    }


    DECLARE_BYREF_RO_PROPERTY(yhash_set<TChunk*>, LostChunks);
    DECLARE_BYREF_RO_PROPERTY(yhash_set<TChunk*>, LostVitalChunks);
    DECLARE_BYREF_RO_PROPERTY(yhash_set<TChunk*>, OverreplicatedChunks);
    DECLARE_BYREF_RO_PROPERTY(yhash_set<TChunk*>, UnderreplicatedChunks);
    DECLARE_BYREF_RO_PROPERTY(yhash_set<TChunk*>, DataMissingChunks);
    DECLARE_BYREF_RO_PROPERTY(yhash_set<TChunk*>, ParityMissingChunks);
    DECLARE_BYREF_RO_PROPERTY(yhash_set<TChunk*>, QuorumMissingChunks);
    DECLARE_BYREF_RO_PROPERTY(yhash_set<TChunk*>, UnsafelyPlacedChunks);
    DEFINE_BYREF_RO_PROPERTY(yhash_set<TChunk*>, ForeignChunks);


    int GetTotalReplicaCount()
    {
        return TotalReplicaCount_;
    }

    bool IsReplicatorEnabled()
    {
        return ChunkReplicator_ && ChunkReplicator_->IsEnabled();
    }


    void ScheduleChunkRefresh(TChunk* chunk)
    {
        if (ChunkReplicator_) {
            ChunkReplicator_->ScheduleChunkRefresh(chunk);
        }
    }

    void ScheduleNodeRefresh(TNode* node)
    {
        if (ChunkReplicator_) {
            ChunkReplicator_->ScheduleNodeRefresh(node);
        }
    }

    void ScheduleChunkPropertiesUpdate(TChunkTree* chunkTree)
    {
        if (ChunkReplicator_) {
            ChunkReplicator_->SchedulePropertiesUpdate(chunkTree);
        }
    }

    void ScheduleChunkSeal(TChunk* chunk)
    {
        if (ChunkSealer_) {
            ChunkSealer_->ScheduleSeal(chunk);
        }
    }


    TChunk* GetChunkOrThrow(const TChunkId& id)
    {
        auto* chunk = FindChunk(id);
        if (!IsObjectAlive(chunk)) {
            THROW_ERROR_EXCEPTION(
                NChunkClient::EErrorCode::NoSuchChunk,
                "No such chunk %v",
                id);
        }
        return chunk;
    }

    TChunkList* GetChunkListOrThrow(const TChunkListId& id)
    {
        auto* chunkList = FindChunkList(id);
        if (!IsObjectAlive(chunkList)) {
            THROW_ERROR_EXCEPTION(
                NChunkClient::EErrorCode::NoSuchChunkList,
                "No such chunk list %v",
                id);
        }
        return chunkList;
    }


    TChunkTree* FindChunkTree(const TChunkTreeId& id)
    {
        auto type = TypeFromId(id);
        switch (type) {
            case EObjectType::Chunk:
            case EObjectType::ErasureChunk:
            case EObjectType::JournalChunk:
                return FindChunk(id);
            case EObjectType::ChunkList:
                return FindChunkList(id);
            default:
                return nullptr;
        }
    }

    TChunkTree* GetChunkTree(const TChunkTreeId& id)
    {
        auto* chunkTree = FindChunkTree(id);
        YCHECK(chunkTree);
        return chunkTree;
    }

    TChunkTree* GetChunkTreeOrThrow(const TChunkTreeId& id)
    {
        auto* chunkTree = FindChunkTree(id);
        if (!IsObjectAlive(chunkTree)) {
            THROW_ERROR_EXCEPTION(
                NChunkClient::EErrorCode::NoSuchChunkTree,
                "No such chunk tree %v",
                id);
        }
        return chunkTree;
    }


    EChunkStatus ComputeChunkStatus(TChunk* chunk)
    {
        return ChunkReplicator_->ComputeChunkStatus(chunk);
    }


    TFuture<TMiscExt> GetChunkQuorumInfo(TChunk* chunk)
    {
        if (chunk->IsSealed()) {
            return MakeFuture(chunk->MiscExt());
        }

        std::vector<NNodeTrackerClient::TNodeDescriptor> replicas;
        for (auto nodeWithIndex : chunk->StoredReplicas()) {
            const auto* node = nodeWithIndex.GetPtr();
            replicas.push_back(node->GetDescriptor());
        }

        return ComputeQuorumInfo(
            chunk->GetId(),
            replicas,
            Config_->JournalRpcTimeout,
            chunk->GetReadQuorum(),
            Bootstrap_->GetLightNodeChannelFactory());
    }


    DECLARE_ENTITY_MAP_ACCESSORS(Chunk, TChunk);
    DECLARE_ENTITY_MAP_ACCESSORS(ChunkList, TChunkList);

private:
    friend class TChunkTypeHandlerBase;
    friend class TRegularChunkTypeHandler;
    friend class TErasureChunkTypeHandler;
    friend class TChunkListTypeHandler;

    const TChunkManagerConfigPtr Config_;

    TChunkTreeBalancer ChunkTreeBalancer_;

    int TotalReplicaCount_ = 0;

    bool NeedToRecomputeStatistics_ = false;

    TPeriodicExecutorPtr ProfilingExecutor_;

    NProfiling::TProfiler Profiler = ChunkServerProfiler;
    i64 ChunksCreated_ = 0;
    i64 ChunksDestroyed_ = 0;
    i64 ChunkReplicasAdded_ = 0;
    i64 ChunkReplicasRemoved_ = 0;
    i64 ChunkListsCreated_ = 0;
    i64 ChunkListsDestroyed_ = 0;

    TChunkPlacementPtr ChunkPlacement_;
    TChunkReplicatorPtr ChunkReplicator_;
    TChunkSealerPtr ChunkSealer_;

    NHydra::TEntityMap<TChunk> ChunkMap_;
    NHydra::TEntityMap<TChunkList> ChunkListMap_;


    void DestroyChunk(TChunk* chunk)
    {
        if (chunk->IsForeign()) {
            YCHECK(ForeignChunks_.erase(chunk) == 1);
        }

        // Decrease staging resource usage; release account.
        UnstageChunk(chunk);

        // Cancel all jobs, reset status etc.
        if (ChunkReplicator_) {
            ChunkReplicator_->OnChunkDestroyed(chunk);
        }

        // Unregister chunk replicas from all known locations.
        // Schedule removal jobs.
        auto unregisterReplica = [&] (TNodePtrWithIndex nodeWithIndex, bool cached) {
            auto* node = nodeWithIndex.GetPtr();
            TChunkPtrWithIndex chunkWithIndex(chunk, nodeWithIndex.GetIndex());
            if (!node->RemoveReplica(chunkWithIndex, cached)) {
                return;
            }
            if (!ChunkReplicator_) {
                return;
            }
            if (node->GetLocalState() != ENodeState::Online) {
                return;
            }
            ChunkReplicator_->ScheduleReplicaRemoval(node, chunkWithIndex);
        };

        for (auto replica : chunk->StoredReplicas()) {
            unregisterReplica(replica, false);
        }

        for (auto replica : chunk->CachedReplicas()) {
            unregisterReplica(replica, true);
        }

        ++ChunksDestroyed_;
    }

    void DestroyChunkList(TChunkList* chunkList)
    {
        // Release account.
        UnstageChunkList(chunkList, false);

        // Drop references to children.
        auto objectManager = Bootstrap_->GetObjectManager();
        for (auto* child : chunkList->Children()) {
            ResetChunkTreeParent(chunkList, child);
            objectManager->UnrefObject(child);
        }

        ++ChunkListsDestroyed_;
    }


    void OnNodeRegistered(TNode* node)
    {
        if (ChunkPlacement_) {
            ChunkPlacement_->OnNodeRegistered(node);
        }

        if (ChunkReplicator_) {
            ChunkReplicator_->OnNodeRegistered(node);
        }

        ScheduleNodeRefresh(node);
    }

    void OnNodeUnregistered(TNode* node)
    {
        if (ChunkPlacement_) {
            ChunkPlacement_->OnNodeUnregistered(node);
        }

        if (ChunkReplicator_) {
            ChunkReplicator_->OnNodeUnregistered(node);
        }
    }

    void OnNodeDisposed(TNode* node)
    {
        for (auto replica : node->StoredReplicas()) {
            RemoveChunkReplica(node, replica, false, ERemoveReplicaReason::NodeDisposed);
        }

        for (auto replica : node->CachedReplicas()) {
            RemoveChunkReplica(node, replica, true, ERemoveReplicaReason::NodeDisposed);
        }

        node->ClearReplicas();

        if (ChunkPlacement_) {
            ChunkPlacement_->OnNodeDisposed(node);
        }

        if (ChunkReplicator_) {
            ChunkReplicator_->OnNodeDisposed(node);
        }
    }

    void OnNodeChanged(TNode* node)
    {
        if (node->GetLocalState() == ENodeState::Online) {
            ScheduleNodeRefresh(node);
        }
    }

    void OnFullHeartbeat(
        TNode* node,
        NNodeTrackerServer::NProto::TReqFullHeartbeat* request)
    {
        YCHECK(node->StoredReplicas().empty());
        YCHECK(node->CachedReplicas().empty());

        node->ReserveStoredReplicas(request->stored_chunk_count());
        node->ReserveCachedReplicas(request->cached_chunk_count());

        for (const auto& chunkInfo : request->chunks()) {
            ProcessAddedChunk(node, chunkInfo, false);
        }

        if (ChunkPlacement_) {
            ChunkPlacement_->OnNodeUpdated(node);
        }
    }

    void OnIncrementalHeartbeat(
        TNode* node,
        TReqIncrementalHeartbeat* request,
        TRspIncrementalHeartbeat* /*response*/)
    {
        node->ShrinkHashTables();

        for (const auto& chunkInfo : request->added_chunks()) {
            ProcessAddedChunk(node, chunkInfo, true);
        }

        for (const auto& chunkInfo : request->removed_chunks()) {
            ProcessRemovedChunk(node, chunkInfo);
        }

        const auto* mutationContext = GetCurrentMutationContext();
        auto mutationTimestamp = mutationContext->GetTimestamp();

        auto& unapprovedReplicas = node->UnapprovedReplicas();
        for (auto it = unapprovedReplicas.begin(); it != unapprovedReplicas.end();) {
            auto jt = it++;
            auto replica = jt->first;
            auto registerTimestamp = jt->second;
            auto reason = ERemoveReplicaReason::None;
            if (!IsObjectAlive(replica.GetPtr())) {
                reason = ERemoveReplicaReason::ChunkDestroyed;
            } else if (mutationTimestamp > registerTimestamp + ReplicaApproveTimeout) {
                reason = ERemoveReplicaReason::ApproveTimeout;
            }
            if (reason != ERemoveReplicaReason::None) {
                // This also removes replica from unapprovedReplicas.
                RemoveChunkReplica(node, replica, false, reason);
            }
        }

        if (ChunkPlacement_) {
            ChunkPlacement_->OnNodeUpdated(node);
        }
    }


    void HydraUpdateChunkProperties(NProto::TReqUpdateChunkProperties* request)
    {
        // NB: Ordered map is a must to make the behavior deterministic.
        std::map<TCellTag, NProto::TReqUpdateChunkProperties> crossCellRequestMap;
        auto getCrossCellRequest = [&] (const TChunk* chunk) -> NProto::TReqUpdateChunkProperties& {
            auto cellTag = CellTagFromId(chunk->GetId());
            auto it = crossCellRequestMap.find(cellTag);
            if (it == crossCellRequestMap.end()) {
                it = crossCellRequestMap.insert(std::make_pair(cellTag, NProto::TReqUpdateChunkProperties())).first;
                it->second.set_cell_tag(Bootstrap_->GetCellTag());
            }
            return it->second;
        };

        bool local = request->cell_tag() == Bootstrap_->GetCellTag();

        auto multicellManager = Bootstrap_->GetMulticellManager();
        int cellIndex = local ? -1 : multicellManager->GetRegisteredMasterCellIndex(request->cell_tag());

        for (const auto& update : request->updates()) {
            auto chunkId = FromProto<TChunkId>(update.chunk_id());
            auto* chunk = FindChunk(chunkId);
            if (!IsObjectAlive(chunk)) {
                continue;
            }

            TChunkProperties properties;
            properties.ReplicationFactor = update.replication_factor();
            properties.Vital = update.vital();

            bool updated = local
                ? chunk->UpdateLocalProperties(properties)
                : chunk->UpdateExternalProprties(cellIndex, properties);
            if (!updated) {
                continue;
            }

            if (chunk->IsForeign()) {
                Y_ASSERT(local);
                auto& crossCellRequest = getCrossCellRequest(chunk);
                *crossCellRequest.add_updates() = update;
            } else {
                ScheduleChunkRefresh(chunk);
            }
        }

        for (const auto& pair : crossCellRequestMap) {
            auto cellTag = pair.first;
            const auto& request = pair.second;
            multicellManager->PostToMaster(request, cellTag);
            LOG_DEBUG_UNLESS(IsRecovery(), "Requesting to update properties of imported chunks (CellTag: %v, Count: %v)",
                cellTag,
                request.updates_size());
        }
    }

    void HydraExportChunks(TCtxExportChunksPtr /*context*/, TReqExportChunks* request, TRspExportChunks* response)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionManager->GetTransactionOrThrow(transactionId);
        if (transaction->GetPersistentState() != ETransactionState::Active) {
            transaction->ThrowInvalidState();
        }

        auto multicellManager = Bootstrap_->GetMulticellManager();

        std::vector<TChunkId> chunkIds;
        for (const auto& exportData : request->chunks()) {
            auto chunkId = FromProto<TChunkId>(exportData.id());
            auto* chunk = GetChunkOrThrow(chunkId);

            if (chunk->IsForeign()) {
                THROW_ERROR_EXCEPTION("Cannot export a foreign chunk %v", chunkId);
            }

            auto cellTag = exportData.destination_cell_tag();
            if (!multicellManager->IsRegisteredMasterCell(cellTag)) {
                THROW_ERROR_EXCEPTION("Cell %v is not registered");
            }

            transactionManager->ExportObject(transaction, chunk, cellTag);

            if (response) {
                auto* importData = response->add_chunks();
                ToProto(importData->mutable_id(), chunkId);
                importData->mutable_info()->CopyFrom(chunk->ChunkInfo());
                importData->mutable_meta()->CopyFrom(chunk->ChunkMeta());
                importData->set_erasure_codec(static_cast<int>(chunk->GetErasureCodec()));
            }

            chunkIds.push_back(chunk->GetId());
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Chunks exported (TransactionId: %v, ChunkIds: %v)",
            transactionId,
            chunkIds);
    }

    void HydraImportChunks(TCtxImportChunksPtr /*context*/, TReqImportChunks* request, TRspImportChunks* /*response*/)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionManager->GetTransactionOrThrow(transactionId);

        if (transaction->GetPersistentState() != ETransactionState::Active) {
            transaction->ThrowInvalidState();
        }

        std::vector<TChunkId> chunkIds;
        for (auto& importData : *request->mutable_chunks()) {
            auto chunkId = FromProto<TChunkId>(importData.id());
            if (CellTagFromId(chunkId) == Bootstrap_->GetCellTag()) {
                THROW_ERROR_EXCEPTION("Cannot import a native chunk %v", chunkId);
            }

            auto* chunk = ChunkMap_.Find(chunkId);
            if (!chunk) {
                auto chunkHolder = std::make_unique<TChunk>(chunkId);
                chunk = ChunkMap_.Insert(chunkId, std::move(chunkHolder));
                chunk->SetForeign();
                chunk->Confirm(importData.mutable_info(), importData.mutable_meta());
                chunk->SetErasureCodec(NErasure::ECodec(importData.erasure_codec()));
                YCHECK(ForeignChunks_.insert(chunk).second);
            }

            transactionManager->ImportObject(transaction, chunk);

            chunkIds.push_back(chunk->GetId());
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Chunks imported (TransactionId: %v, ChunkIds: %v)",
            transactionId,
            chunkIds);
    }

    void HydraExecuteBatch(TCtxExecuteBatchPtr /*context*/, TReqExecuteBatch* request, TRspExecuteBatch* response)
    {
        auto executeSubrequests = [&] (
            auto* subrequests,
            auto* subresponses,
            auto handler,
            const char* errorMessage)
        {
            for (auto& subrequest : *subrequests) {
                auto* subresponse = subresponses ? subresponses->Add() : nullptr;
                try {
                    (this->*handler)(&subrequest, subresponse);
                } catch (const std::exception& ex) {
                    LOG_DEBUG_UNLESS(IsRecovery(), ex, errorMessage);
                    if (subresponse) {
                        ToProto(subresponse->mutable_error(), TError(ex));
                    }
                }
            }
        };

        executeSubrequests(
            request->mutable_create_chunk_subrequests(),
            response ? response->mutable_create_chunk_subresponses() : nullptr,
            &TImpl::ExecuteCreateChunkSubrequest,
            "Error creating chunk");

        executeSubrequests(
            request->mutable_confirm_chunk_subrequests(),
            response ? response->mutable_confirm_chunk_subresponses() : nullptr,
            &TImpl::ExecuteConfirmChunkSubrequest,
            "Error confirming chunk");

        executeSubrequests(
            request->mutable_seal_chunk_subrequests(),
            response ? response->mutable_seal_chunk_subresponses() : nullptr,
            &TImpl::ExecuteSealChunkSubrequest,
            "Error sealing chunk");

        executeSubrequests(
            request->mutable_create_chunk_lists_subrequests(),
            response ? response->mutable_create_chunk_lists_subresponses() : nullptr,
            &TImpl::ExecuteCreateChunkListsSubrequest,
            "Error creating chunk lists");

        executeSubrequests(
            request->mutable_unstage_chunk_tree_subrequests(),
            response ? response->mutable_unstage_chunk_tree_subresponses() : nullptr,
            &TImpl::ExecuteUnstageChunkTreeSubrequest,
            "Error unstaging chunk tree");

        executeSubrequests(
            request->mutable_attach_chunk_trees_subrequests(),
            response ? response->mutable_attach_chunk_trees_subresponses() : nullptr,
            &TImpl::ExecuteAttachChunkTreesSubrequest,
            "Error attaching chunk trees");
    }

    void ExecuteCreateChunkSubrequest(
        TReqExecuteBatch::TCreateChunkSubrequest* subrequest,
        TRspExecuteBatch::TCreateChunkSubresponse* subresponse)
    {
        auto transactionId = FromProto<TTransactionId>(subrequest->transaction_id());
        auto chunkType = EObjectType(subrequest->type());
        bool isErasure = (chunkType == EObjectType::ErasureChunk);
        bool isJournal = (chunkType == EObjectType::JournalChunk);
        auto erasureCodecId = isErasure ? NErasure::ECodec(subrequest->erasure_codec()) : NErasure::ECodec::None;
        int replicationFactor = isErasure ? 1 : subrequest->replication_factor();
        int readQuorum = isJournal ? subrequest->read_quorum() : 0;
        int writeQuorum = isJournal ? subrequest->write_quorum() : 0;

        auto transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionManager->GetTransactionOrThrow(transactionId);

        auto securityManager = Bootstrap_->GetSecurityManager();
        auto* account = securityManager->GetAccountByNameOrThrow(subrequest->account());
        securityManager->ValidateResourceUsageIncrease(account, TClusterResources(1, 0, 1));

        TChunkList* chunkList = nullptr;
        if (subrequest->has_chunk_list_id()) {
            auto chunkListId = FromProto<TChunkListId>(subrequest->chunk_list_id());
            chunkList = GetChunkListOrThrow(chunkListId);
            chunkList->ValidateSealed();
        }

        // NB: Once the chunk is created, no exceptions could be thrown.
        ChunksCreated_++;
        auto id = Bootstrap_->GetObjectManager()->GenerateId(chunkType, NullObjectId);
        auto chunkHolder = std::make_unique<TChunk>(id);
        auto* chunk = ChunkMap_.Insert(id, std::move(chunkHolder));
        chunk->SetLocalReplicationFactor(replicationFactor);
        chunk->SetReadQuorum(readQuorum);
        chunk->SetWriteQuorum(writeQuorum);
        chunk->SetErasureCodec(erasureCodecId);
        chunk->SetMovable(subrequest->movable());
        chunk->SetLocalVital(subrequest->vital());

        StageChunkTree(chunk, transaction, account);

        transactionManager->StageObject(transaction, chunk);

        if (chunkList) {
            AttachToChunkList(chunkList, chunk);
        }

        if (subresponse) {
            ToProto(subresponse->mutable_chunk_id(), chunk->GetId());
        }

        LOG_DEBUG_UNLESS(IsRecovery(),
            "Chunk created "
            "(ChunkId: %v, ChunkListId: %v, TransactionId: %v, Account: %v, ReplicationFactor: %v, "
            "ReadQuorum: %v, WriteQuorum: %v, ErasureCodec: %v, Movable: %v, Vital: %v)",
            chunk->GetId(),
            GetObjectId(chunkList),
            transaction->GetId(),
            account->GetName(),
            chunk->GetLocalReplicationFactor(),
            chunk->GetReadQuorum(),
            chunk->GetWriteQuorum(),
            erasureCodecId,
            subrequest->movable(),
            subrequest->vital());
    }

    void ExecuteConfirmChunkSubrequest(
        TReqExecuteBatch::TConfirmChunkSubrequest* subrequest,
        TRspExecuteBatch::TConfirmChunkSubresponse* subresponse)
    {
        auto chunkId = FromProto<TChunkId>(subrequest->chunk_id());
        auto replicas = FromProto<TChunkReplicaList>(subrequest->replicas());

        auto* chunk = GetChunkOrThrow(chunkId);

        ConfirmChunk(
            chunk,
            replicas,
            subrequest->mutable_chunk_info(),
            subrequest->mutable_chunk_meta());

        if (subresponse && subrequest->request_statistics()) {
            *subresponse->mutable_statistics() = chunk->GetStatistics().ToDataStatistics();
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Chunk confirmed (ChunkId: %v)",
            chunkId);
    }

    void ExecuteSealChunkSubrequest(
        TReqExecuteBatch::TSealChunkSubrequest* subrequest,
        TRspExecuteBatch::TSealChunkSubresponse* subresponse)
    {
        auto chunkId = FromProto<TChunkId>(subrequest->chunk_id());
        auto* chunk = GetChunkOrThrow(chunkId);

        const auto& miscExt = subrequest->misc();

        SealChunk(
            chunk,
            miscExt);

        LOG_DEBUG_UNLESS(IsRecovery(), "Chunk sealed "
            "(ChunkId: %v, RowCount: %v, UncompressedDataSize: %v, CompressedDataSize: %v)",
            chunk->GetId(),
            miscExt.row_count(),
            miscExt.uncompressed_data_size(),
            miscExt.compressed_data_size());
    }

    void ExecuteCreateChunkListsSubrequest(
        TReqExecuteBatch::TCreateChunkListsSubrequest* subrequest,
        TRspExecuteBatch::TCreateChunkListsSubresponse* subresponse)
    {
        auto transactionId = FromProto<TTransactionId>(subrequest->transaction_id());
        int count = subrequest->count();

        auto transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionManager->GetTransactionOrThrow(transactionId);

        auto objectManager = Bootstrap_->GetObjectManager();

        std::vector<TChunkListId> chunkListIds;
        chunkListIds.reserve(count);
        for (int index = 0; index < count; ++index) {
            auto* chunkList = CreateChunkList();
            StageChunkTree(chunkList, transaction, nullptr);
            transactionManager->StageObject(transaction, chunkList);
            ToProto(subresponse->add_chunk_list_ids(), chunkList->GetId());
            chunkListIds.push_back(chunkList->GetId());
        }

        LOG_DEBUG_UNLESS(IsRecovery(),
            "Chunk lists created (ChunkListIds: %v, TransactionId: %v)",
            chunkListIds,
            transaction->GetId());
    }

    void ExecuteUnstageChunkTreeSubrequest(
        TReqExecuteBatch::TUnstageChunkTreeSubrequest* subrequest,
        TRspExecuteBatch::TUnstageChunkTreeSubresponse* subresponse)
    {
        auto chunkTreeId = FromProto<TTransactionId>(subrequest->chunk_tree_id());
        auto recursive = subrequest->recursive();

        auto* chunkTree = GetChunkTreeOrThrow(chunkTreeId);
        auto transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->UnstageObject(chunkTree->GetStagingTransaction(), chunkTree, recursive);

        LOG_DEBUG_UNLESS(IsRecovery(), "Chunk tree unstaged (ChunkTreeId: %v, Recursive: %v)",
            chunkTreeId,
            recursive);
    }

    void ExecuteAttachChunkTreesSubrequest(
        TReqExecuteBatch::TAttachChunkTreesSubrequest* subrequest,
        TRspExecuteBatch::TAttachChunkTreesSubresponse* subresponse)
    {
        auto parentId = FromProto<TTransactionId>(subrequest->parent_id());
        auto* parent = GetChunkListOrThrow(parentId);

        std::vector<TChunkTree*> children;
        children.reserve(subrequest->child_ids_size());
        for (const auto& protoChildId : subrequest->child_ids()) {
            auto childId = FromProto<TChunkTreeId>(protoChildId);
            auto* child = GetChunkTreeOrThrow(childId);
            children.push_back(child);
        }

        AttachToChunkList(parent, children);

        if (subrequest->request_statistics()) {
            *subresponse->mutable_statistics() = parent->Statistics().ToDataStatistics();
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Chunk trees attached (ParentId: %v, ChildIds: %v)",
            parentId,
            MakeFormattableRange(children, TObjectIdFormatter()));
    }


    void SaveKeys(NCellMaster::TSaveContext& context) const
    {
        ChunkMap_.SaveKeys(context);
        ChunkListMap_.SaveKeys(context);
    }

    void SaveValues(NCellMaster::TSaveContext& context) const
    {
        ChunkMap_.SaveValues(context);
        ChunkListMap_.SaveValues(context);
    }


    void LoadKeys(NCellMaster::TLoadContext& context)
    {
        ChunkMap_.LoadKeys(context);
        ChunkListMap_.LoadKeys(context);
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        ChunkMap_.LoadValues(context);
        ChunkListMap_.LoadValues(context);

        // COMPAT(savrus): Cf. YT-5120
        if (context.GetVersion() < 302) {
            NeedToRecomputeStatistics_ = true;
        }
    }

    virtual void OnAfterSnapshotLoaded() override
    {
        TMasterAutomatonPart::OnAfterSnapshotLoaded();

        // Populate nodes' chunk replica sets.
        // Compute chunk replica count.

        LOG_INFO("Started initializing chunks");

        TotalReplicaCount_ = 0;
        for (const auto& pair : ChunkMap_) {
            auto* chunk = pair.second;

            auto addReplica = [&] (TNodePtrWithIndex nodePtrWithIndex, bool cached) {
                TChunkPtrWithIndex chunkPtrWithIndex(chunk, nodePtrWithIndex.GetIndex());
                nodePtrWithIndex.GetPtr()->AddReplica(chunkPtrWithIndex, cached);
                ++TotalReplicaCount_;
            };

            for (auto nodePtrWithIndex : chunk->StoredReplicas()) {
                addReplica(nodePtrWithIndex, false);
            }
            for (auto nodePtrWithIndex : chunk->CachedReplicas()) {
                addReplica(nodePtrWithIndex, true);
            }

            if (chunk->IsForeign()) {
                YCHECK(ForeignChunks_.insert(chunk).second);
            }
        }

        LOG_INFO("Finished initializing chunks");
    }


    virtual void Clear() override
    {
        TMasterAutomatonPart::Clear();

        ChunkMap_.Clear();
        ChunkListMap_.Clear();
        ForeignChunks_.clear();
        TotalReplicaCount_ = 0;

        ChunksCreated_ = 0;
        ChunksDestroyed_ = 0;
        ChunkReplicasAdded_ = 0;
        ChunkReplicasRemoved_ = 0;
        ChunkListsCreated_ = 0;
        ChunkListsDestroyed_ = 0;
    }

    void ScheduleRecomputeStatistics()
    {
        NeedToRecomputeStatistics_ = true;
    }

    void RecomputeStatistics()
    {
        LOG_INFO("Started recomputing statistics");

        auto visitMark = TChunkList::GenerateVisitMark();

        std::vector<TChunkList*> chunkLists;
        std::vector<std::pair<TChunkList*, int>> stack;

        auto visit = [&] (TChunkList* chunkList) {
            if (chunkList->GetVisitMark() != visitMark) {
                chunkList->SetVisitMark(visitMark);
                stack.emplace_back(chunkList, 0);
            }
        };

        // Sort chunk lists in topological order
        for (const auto& pair : ChunkListMap_) {
            auto* chunkList = pair.second;
            visit(chunkList);

            while (!stack.empty()) {
                chunkList = stack.back().first;
                int childIndex = stack.back().second;
                int childCount = chunkList->Children().size();

                if (childIndex == childCount) {
                    chunkLists.push_back(chunkList);
                    stack.pop_back();
                } else {
                    ++stack.back().second;
                    auto* child = chunkList->Children()[childIndex];
                    if (child && child->GetType() == EObjectType::ChunkList) {
                        visit(child->AsChunkList());
                    }
                }
            }
        }

        // Recompute statistics
        for (auto* chunkList : chunkLists) {
            auto& statistics = chunkList->Statistics();
            auto oldStatistics = statistics;
            statistics = TChunkTreeStatistics();
            statistics.Rank = 1;
            int childCount = chunkList->Children().size();

            auto& rowCountSums = chunkList->RowCountSums();
            rowCountSums.clear();

            auto& chunkCountSums = chunkList->ChunkCountSums();
            chunkCountSums.clear();

            auto& dataSizeSums = chunkList->DataSizeSums();
            dataSizeSums.clear();

            for (int childIndex = 0; childIndex < childCount; ++childIndex) {
                auto* child = chunkList->Children()[childIndex];
                if (!child) {
                    continue;
                }

                TChunkTreeStatistics childStatistics;
                switch (child->GetType()) {
                    case EObjectType::Chunk:
                    case EObjectType::ErasureChunk:
                    case EObjectType::JournalChunk:
                        childStatistics.Accumulate(child->AsChunk()->GetStatistics());
                        break;

                    case EObjectType::ChunkList:
                        childStatistics.Accumulate(child->AsChunkList()->Statistics());
                        break;

                    default:
                        YUNREACHABLE();
                }

                if (childIndex + 1 < childCount) {
                    rowCountSums.push_back(statistics.RowCount + childStatistics.RowCount);
                    chunkCountSums.push_back(statistics.ChunkCount + childStatistics.ChunkCount);
                    dataSizeSums.push_back(statistics.UncompressedDataSize + childStatistics.UncompressedDataSize);
                }

                statistics.Accumulate(childStatistics);
            }

            if (!chunkList->Children().empty()) {
                ++statistics.Rank;
            }
            ++statistics.ChunkListCount;

            if (statistics != oldStatistics) {
                LOG_DEBUG("Chunk list statistics changed (ChunkList: %v, OldStatistics: %v, NewStatistics: %v)",
                    chunkList->GetId(),
                    ConvertToYsonString(oldStatistics, NYson::EYsonFormat::Text).Data(),
                    ConvertToYsonString(statistics, NYson::EYsonFormat::Text).Data());
            }
        }

        LOG_INFO("Finished recomputing statistics");
    }

    virtual void OnRecoveryStarted() override
    {
        TMasterAutomatonPart::OnRecoveryStarted();

        Profiler.SetEnabled(false);

        NeedToRecomputeStatistics_ = false;
    }

    virtual void OnRecoveryComplete() override
    {
        TMasterAutomatonPart::OnRecoveryComplete();

        Profiler.SetEnabled(true);

        if (NeedToRecomputeStatistics_) {
            RecomputeStatistics();
            NeedToRecomputeStatistics_ = false;
        }
    }

    virtual void OnLeaderRecoveryComplete() override
    {
        TMasterAutomatonPart::OnLeaderRecoveryComplete();

        ChunkPlacement_ = New<TChunkPlacement>(Config_, Bootstrap_);
        ChunkReplicator_ = New<TChunkReplicator>(Config_, Bootstrap_, ChunkPlacement_);
        ChunkSealer_ = New<TChunkSealer>(Config_, Bootstrap_);

        LOG_INFO("Scheduling full chunk refresh");
        PROFILE_TIMING ("/full_chunk_refresh_schedule_time") {
            auto nodeTracker = Bootstrap_->GetNodeTracker();
            for (const auto& pair : nodeTracker->Nodes()) {
                auto* node = pair.second;
                ChunkReplicator_->OnNodeRegistered(node);
                ChunkPlacement_->OnNodeRegistered(node);
            }

            for (const auto& pair : ChunkMap_) {
                auto* chunk = pair.second;
                if (!IsObjectAlive(chunk))
                    continue;

                ChunkReplicator_->ScheduleChunkRefresh(chunk);
                ChunkReplicator_->SchedulePropertiesUpdate(chunk);

                if (chunk->IsJournal()) {
                    ChunkSealer_->ScheduleSeal(chunk);
                }
            }

        }
        LOG_INFO("Full chunk refresh scheduled");
    }

    virtual void OnLeaderActive() override
    {
        TMasterAutomatonPart::OnLeaderActive();

        ChunkReplicator_->Start();
        ChunkSealer_->Start();
    }

    virtual void OnStopLeading() override
    {
        TMasterAutomatonPart::OnStopLeading();

        ChunkPlacement_.Reset();

        if (ChunkReplicator_) {
            ChunkReplicator_->Stop();
            ChunkReplicator_.Reset();
        }

        if (ChunkSealer_) {
            ChunkSealer_->Stop();
            ChunkSealer_.Reset();
        }
    }


    void AddChunkReplica(TNode* node, TChunkPtrWithIndex chunkWithIndex, bool cached, EAddReplicaReason reason)
    {
        auto* chunk = chunkWithIndex.GetPtr();
        auto nodeId = node->GetId();
        TNodePtrWithIndex nodeWithIndex(node, chunkWithIndex.GetIndex());

        if (!node->AddReplica(chunkWithIndex, cached))
            return;

        chunk->AddReplica(nodeWithIndex, cached);

        if (!IsRecovery()) {
            LOG_EVENT(
                Logger,
                reason == EAddReplicaReason::FullHeartbeat ? NLogging::ELogLevel::Trace : NLogging::ELogLevel::Debug,
                "Chunk replica added (ChunkId: %v, Cached: %v, NodeId: %v, Address: %v)",
                chunkWithIndex,
                cached,
                nodeId,
                node->GetDefaultAddress());
        }

        if (!cached) {
            ScheduleChunkRefresh(chunk);
        }

        if (ChunkSealer_ && !cached && chunk->IsJournal()) {
            ChunkSealer_->ScheduleSeal(chunk);
        }

        if (reason == EAddReplicaReason::IncrementalHeartbeat || reason == EAddReplicaReason::Confirmation) {
            ++ChunkReplicasAdded_;
        }
    }

    void RemoveChunkReplica(TNode* node, TChunkPtrWithIndex chunkWithIndex, bool cached, ERemoveReplicaReason reason)
    {
        auto* chunk = chunkWithIndex.GetPtr();
        auto nodeId = node->GetId();
        TNodePtrWithIndex nodeWithIndex(node, chunkWithIndex.GetIndex());
        TChunkIdWithIndex chunkIdWithIndex(chunk->GetId(), nodeWithIndex.GetIndex());

        if (reason == ERemoveReplicaReason::IncrementalHeartbeat && !node->HasReplica(chunkWithIndex, cached)) {
            return;
        }

        chunk->RemoveReplica(nodeWithIndex, cached);

        switch (reason) {
            case ERemoveReplicaReason::IncrementalHeartbeat:
            case ERemoveReplicaReason::ApproveTimeout:
            case ERemoveReplicaReason::ChunkDestroyed:
                node->RemoveReplica(chunkWithIndex, cached);
                if (ChunkReplicator_ && !cached) {
                    ChunkReplicator_->OnReplicaRemoved(node, chunkWithIndex, reason);
                }
                break;
            case ERemoveReplicaReason::NodeDisposed:
                // Do nothing.
                break;
            default:
                YUNREACHABLE();
        }

        if (!IsRecovery()) {
            LOG_EVENT(
                Logger,
                reason == ERemoveReplicaReason::NodeDisposed ||
                reason == ERemoveReplicaReason::ChunkDestroyed
                ? NLogging::ELogLevel::Trace : NLogging::ELogLevel::Debug,
                "Chunk replica removed (ChunkId: %v, Cached: %v, Reason: %v, NodeId: %v, Address: %v)",
                chunkWithIndex,
                cached,
                reason,
                nodeId,
                node->GetDefaultAddress());
        }

        if (!cached) {
            ScheduleChunkRefresh(chunk);
        }

        ++ChunkReplicasRemoved_;
    }


    static int GetAddedChunkReplicaIndex(
        TChunk* chunk,
        const TChunkAddInfo& chunkAddInfo,
        const TChunkIdWithIndex& chunkIdWithIndex)
    {
        if (!chunk->IsJournal()) {
            return chunkIdWithIndex.Index;
        }

        if (chunkAddInfo.active()) {
            return ActiveChunkReplicaIndex;
        } else if (chunkAddInfo.sealed()) {
            return SealedChunkReplicaIndex;
        } else {
            return UnsealedChunkReplicaIndex;
        }
    }

    void ProcessAddedChunk(
        TNode* node,
        const TChunkAddInfo& chunkAddInfo,
        bool incremental)
    {
        auto nodeId = node->GetId();
        auto chunkId = FromProto<TChunkId>(chunkAddInfo.chunk_id());
        auto chunkIdWithIndex = DecodeChunkId(chunkId);
        bool cached = chunkAddInfo.cached();

        auto* chunk = FindChunk(chunkIdWithIndex.Id);
        if (!IsObjectAlive(chunk)) {
            if (cached) {
                // Nodes may still contain cached replicas of chunks that no longer exist.
                // We just silently ignore this case.
                return;
            }

            LOG_DEBUG_UNLESS(IsRecovery(), "Unknown chunk added, removal scheduled (NodeId: %v, Address: %v, ChunkId: %v, Cached: %v)",
                nodeId,
                node->GetDefaultAddress(),
                chunkIdWithIndex,
                cached);

            if (ChunkReplicator_) {
                ChunkReplicator_->ScheduleUnknownReplicaRemoval(node, chunkIdWithIndex);
            }

            return;
        }

        int replicaIndex = GetAddedChunkReplicaIndex(chunk, chunkAddInfo, chunkIdWithIndex);
        TChunkPtrWithIndex chunkWithIndex(chunk, replicaIndex);
        TNodePtrWithIndex nodeWithIndex(node, replicaIndex);

        if (!cached && node->HasUnapprovedReplica(chunkWithIndex)) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk approved (NodeId: %v, Address: %v, ChunkId: %v)",
                nodeId,
                node->GetDefaultAddress(),
                chunkWithIndex);

            node->ApproveReplica(chunkWithIndex);
            chunk->ApproveReplica(nodeWithIndex);
            return;
        }

        AddChunkReplica(
            node,
            chunkWithIndex,
            cached,
            incremental ? EAddReplicaReason::IncrementalHeartbeat : EAddReplicaReason::FullHeartbeat);
    }

    void ProcessRemovedChunk(
        TNode* node,
        const TChunkRemoveInfo& chunkInfo)
    {
        auto nodeId = node->GetId();
        auto chunkIdWithIndex = DecodeChunkId(FromProto<TChunkId>(chunkInfo.chunk_id()));
        bool cached = chunkInfo.cached();

        auto* chunk = FindChunk(chunkIdWithIndex.Id);
        // NB: Chunk could already be a zombie but we still need to remove the replica.
        if (!chunk) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Unknown chunk replica removed (ChunkId: %v, Cached: %v, Address: %v, NodeId: %v)",
                chunkIdWithIndex,
                cached,
                node->GetDefaultAddress(),
                nodeId);
            return;
        }

        TChunkPtrWithIndex chunkWithIndex(chunk, chunkIdWithIndex.Index);
        RemoveChunkReplica(
            node,
            chunkWithIndex,
            cached,
            ERemoveReplicaReason::IncrementalHeartbeat);
    }


    void OnChunkSealed(TChunk* chunk)
    {
        Y_ASSERT(chunk->IsSealed());

        if (chunk->Parents().empty())
            return;

        // Go upwards and apply delta.
        YCHECK(chunk->Parents().size() == 1);
        auto* chunkList = chunk->Parents()[0];

        auto statisticsDelta = chunk->GetStatistics();
        AccumulateUniqueAncestorsStatistics(chunkList, statisticsDelta);

        auto securityManager = Bootstrap_->GetSecurityManager();

        auto owningNodes = GetOwningNodes(chunk);

        bool journalNodeLocked = false;
        TJournalNode* trunkJournalNode = nullptr;
        for (auto* node : owningNodes) {
            securityManager->UpdateAccountNodeUsage(node);
            if (node->GetType() == EObjectType::Journal) {
                auto* journalNode = static_cast<TJournalNode*>(node);
                if (journalNode->GetUpdateMode() != EUpdateMode::None) {
                    journalNodeLocked = true;
                }
                if (trunkJournalNode) {
                    YCHECK(journalNode->GetTrunkNode() == trunkJournalNode);
                } else {
                    trunkJournalNode = journalNode->GetTrunkNode();
                }
            }
        }

        if (!journalNodeLocked && IsObjectAlive(trunkJournalNode)) {
            auto journalManager = Bootstrap_->GetJournalManager();
            journalManager->SealJournal(trunkJournalNode, nullptr);
        }
    }


    void OnProfiling()
    {
        if (!IsLeader()) {
            return;
        }

        Profiler.Enqueue("/refresh_list_size", ChunkReplicator_->GetRefreshListSize());
        Profiler.Enqueue("/properties_update_list_size", ChunkReplicator_->GetPropertiesUpdateListSize());

        Profiler.Enqueue("/chunk_count", ChunkMap_.GetSize());
        Profiler.Enqueue("/chunks_created", ChunksCreated_);
        Profiler.Enqueue("/chunks_destroyed", ChunksDestroyed_);

        Profiler.Enqueue("/chunk_replica_count", TotalReplicaCount_);
        Profiler.Enqueue("/chunk_replicas_added", ChunkReplicasAdded_);
        Profiler.Enqueue("/chunk_replicas_removed", ChunkReplicasRemoved_);

        Profiler.Enqueue("/chunk_list_count", ChunkListMap_.GetSize());
        Profiler.Enqueue("/chunk_lists_created", ChunkListsCreated_);
        Profiler.Enqueue("/chunk_lists_destroyed", ChunkListsDestroyed_);
    }

};

DEFINE_ENTITY_MAP_ACCESSORS(TChunkManager::TImpl, Chunk, TChunk, ChunkMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TChunkManager::TImpl, ChunkList, TChunkList, ChunkListMap_)

DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunk*>, LostChunks, *ChunkReplicator_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunk*>, LostVitalChunks, *ChunkReplicator_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunk*>, OverreplicatedChunks, *ChunkReplicator_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunk*>, UnderreplicatedChunks, *ChunkReplicator_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunk*>, DataMissingChunks, *ChunkReplicator_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunk*>, ParityMissingChunks, *ChunkReplicator_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunk*>, QuorumMissingChunks, *ChunkReplicator_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager::TImpl, yhash_set<TChunk*>, UnsafelyPlacedChunks, *ChunkReplicator_);

///////////////////////////////////////////////////////////////////////////////

TChunkManager::TChunkTypeHandlerBase::TChunkTypeHandlerBase(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->ChunkMap_)
    , Owner_(owner)
{ }

IObjectProxyPtr TChunkManager::TChunkTypeHandlerBase::DoGetProxy(
    TChunk* chunk,
    TTransaction* /*transaction*/)
{
    return CreateChunkProxy(Bootstrap_, &Metadata_, chunk);
}

void TChunkManager::TChunkTypeHandlerBase::DoDestroyObject(TChunk* chunk)
{
    TObjectTypeHandlerWithMapBase::DoDestroyObject(chunk);
    Owner_->DestroyChunk(chunk);
}

void TChunkManager::TChunkTypeHandlerBase::DoUnstageObject(
    TChunk* chunk,
    bool recursive)
{
    TObjectTypeHandlerWithMapBase::DoUnstageObject(chunk, recursive);
    Owner_->UnstageChunk(chunk);
}

////////////////////////////////////////////////////////////////////////////////

TChunkManager::TChunkListTypeHandler::TChunkListTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->ChunkListMap_)
    , Owner_(owner)
{ }

IObjectProxyPtr TChunkManager::TChunkListTypeHandler::DoGetProxy(
    TChunkList* chunkList,
    TTransaction* /*transaction*/)
{
    return CreateChunkListProxy(Bootstrap_, &Metadata_, chunkList);
}

void TChunkManager::TChunkListTypeHandler::DoDestroyObject(TChunkList* chunkList)
{
    TObjectTypeHandlerWithMapBase::DoDestroyObject(chunkList);
    Owner_->DestroyChunkList(chunkList);
}

void TChunkManager::TChunkListTypeHandler::DoUnstageObject(
    TChunkList* chunkList,
    bool recursive)
{
    TObjectTypeHandlerWithMapBase::DoUnstageObject(chunkList, recursive);
    Owner_->UnstageChunkList(chunkList, recursive);
}

////////////////////////////////////////////////////////////////////////////////

TChunkManager::TChunkManager(
    TChunkManagerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TChunkManager::~TChunkManager() = default;

void TChunkManager::Initialize()
{
    Impl_->Initialize();
}

TChunk* TChunkManager::GetChunkOrThrow(const TChunkId& id)
{
    return Impl_->GetChunkOrThrow(id);
}

TChunkList* TChunkManager::GetChunkListOrThrow(const TChunkListId& id)
{
    return Impl_->GetChunkListOrThrow(id);
}

TChunkTree* TChunkManager::FindChunkTree(const TChunkTreeId& id)
{
    return Impl_->FindChunkTree(id);
}

TChunkTree* TChunkManager::GetChunkTree(const TChunkTreeId& id)
{
    return Impl_->GetChunkTree(id);
}

TChunkTree* TChunkManager::GetChunkTreeOrThrow(const TChunkTreeId& id)
{
    return Impl_->GetChunkTreeOrThrow(id);
}

TNodeList TChunkManager::AllocateWriteTargets(
    TChunk* chunk,
    int desiredCount,
    int minCount,
    TNullable<int> replicationFactorOverride,
    const TNodeList* forbiddenNodes,
    const TNullable<Stroka>& preferredHostName)
{
    return Impl_->AllocateWriteTargets(
        chunk,
        desiredCount,
        minCount,
        replicationFactorOverride,
        forbiddenNodes,
        preferredHostName);
}

TMutationPtr TChunkManager::CreateUpdateChunkPropertiesMutation(const NProto::TReqUpdateChunkProperties& request)
{
    return Impl_->CreateUpdateChunkPropertiesMutation(request);
}

TMutationPtr TChunkManager::CreateExportChunksMutation(TCtxExportChunksPtr context)
{
    return Impl_->CreateExportChunksMutation(std::move(context));
}

TMutationPtr TChunkManager::CreateImportChunksMutation(TCtxImportChunksPtr context)
{
    return Impl_->CreateImportChunksMutation(std::move(context));
}

TMutationPtr TChunkManager::CreateExecuteBatchMutation(TCtxExecuteBatchPtr context)
{
    return Impl_->CreateExecuteBatchMutation(std::move(context));
}

TChunkList* TChunkManager::CreateChunkList()
{
    return Impl_->CreateChunkList();
}

void TChunkManager::UnstageChunk(TChunk* chunk)
{
    Impl_->UnstageChunk(chunk);
}

void TChunkManager::UnstageChunkList(TChunkList* chunkList, bool recursive)
{
    Impl_->UnstageChunkList(chunkList, recursive);
}

TNodePtrWithIndexList TChunkManager::LocateChunk(TChunkPtrWithIndex chunkWithIndex)
{
    return Impl_->LocateChunk(chunkWithIndex);
}

void TChunkManager::AttachToChunkList(
    TChunkList* chunkList,
    TChunkTree** childrenBegin,
    TChunkTree** childrenEnd)
{
    Impl_->AttachToChunkList(chunkList, childrenBegin, childrenEnd);
}

void TChunkManager::AttachToChunkList(
    TChunkList* chunkList,
    const std::vector<TChunkTree*>& children)
{
    Impl_->AttachToChunkList(chunkList, children);
}

void TChunkManager::AttachToChunkList(
    TChunkList* chunkList,
    TChunkTree* child)
{
    Impl_->AttachToChunkList(chunkList, child);
}

void TChunkManager::DetachFromChunkList(
    TChunkList* chunkList,
    TChunkTree** childrenBegin,
    TChunkTree** childrenEnd)
{
    Impl_->DetachFromChunkList(chunkList, childrenBegin, childrenEnd);
}

void TChunkManager::DetachFromChunkList(
    TChunkList* chunkList,
    const std::vector<TChunkTree*>& children)
{
    Impl_->DetachFromChunkList(chunkList, children);
}

void TChunkManager::DetachFromChunkList(
    TChunkList* chunkList,
    TChunkTree* child)
{
    Impl_->DetachFromChunkList(chunkList, child);
}

void TChunkManager::RebalanceChunkTree(TChunkList* chunkList)
{
    Impl_->RebalanceChunkTree(chunkList);
}

void TChunkManager::ClearChunkList(TChunkList* chunkList)
{
    Impl_->ClearChunkList(chunkList);
}

TJobPtr TChunkManager::FindJob(const TJobId& id)
{
    return Impl_->FindJob(id);
}

void TChunkManager::ScheduleJobs(
    TNode* node,
    const std::vector<TJobPtr>& currentJobs,
    std::vector<TJobPtr>* jobsToStart,
    std::vector<TJobPtr>* jobsToAbort,
    std::vector<TJobPtr>* jobsToRemove)
{
    Impl_->ScheduleJobs(
        node,
        currentJobs,
        jobsToStart,
        jobsToAbort,
        jobsToRemove);
}

bool TChunkManager::IsReplicatorEnabled()
{
    return Impl_->IsReplicatorEnabled();
}

void TChunkManager::ScheduleChunkRefresh(TChunk* chunk)
{
    Impl_->ScheduleChunkRefresh(chunk);
}

void TChunkManager::ScheduleNodeRefresh(TNode* node)
{
    Impl_->ScheduleNodeRefresh(node);
}

void TChunkManager::ScheduleChunkPropertiesUpdate(TChunkTree* chunkTree)
{
    Impl_->ScheduleChunkPropertiesUpdate(chunkTree);
}

void TChunkManager::ScheduleChunkSeal(TChunk* chunk)
{
    Impl_->ScheduleChunkSeal(chunk);
}

int TChunkManager::GetTotalReplicaCount()
{
    return Impl_->GetTotalReplicaCount();
}

EChunkStatus TChunkManager::ComputeChunkStatus(TChunk* chunk)
{
    return Impl_->ComputeChunkStatus(chunk);
}

TFuture<TMiscExt> TChunkManager::GetChunkQuorumInfo(TChunk* chunk)
{
    return Impl_->GetChunkQuorumInfo(chunk);
}

DELEGATE_ENTITY_MAP_ACCESSORS(TChunkManager, Chunk, TChunk, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TChunkManager, ChunkList, TChunkList, *Impl_)

DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, LostChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, LostVitalChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, OverreplicatedChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, UnderreplicatedChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, DataMissingChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, ParityMissingChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, QuorumMissingChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, UnsafelyPlacedChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, ForeignChunks, *Impl_);

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
