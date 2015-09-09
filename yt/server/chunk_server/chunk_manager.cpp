#include "stdafx.h"
#include "chunk_manager.h"
#include "config.h"
#include "chunk.h"
#include "chunk_list.h"
#include "chunk_owner_base.h"
#include "job.h"
#include "chunk_placement.h"
#include "chunk_replicator.h"
#include "chunk_sealer.h"
#include "chunk_tree_balancer.h"
#include "chunk_proxy.h"
#include "chunk_list_proxy.h"
#include "private.h"
#include "helpers.h"

#include <core/misc/string.h>

#include <core/compression/codec.h>

#include <core/erasure/codec.h>

#include <core/logging/log.h>

#include <core/profiling/profiler.h>

#include <ytlib/object_client/helpers.h>

#include <ytlib/chunk_client/chunk_ypath.pb.h>
#include <ytlib/chunk_client/chunk_list_ypath.pb.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/schema.h>

#include <ytlib/journal_client/helpers.h>

#include <server/hydra/composite_automaton.h>
#include <server/hydra/entity_map.h>

#include <server/chunk_server/chunk_manager.pb.h>

#include <server/node_tracker_server/node_tracker.h>

#include <server/cypress_server/cypress_manager.h>

#include <server/cell_master/serialize.h>
#include <server/cell_master/bootstrap.h>
#include <server/cell_master/hydra_facade.h>

#include <server/transaction_server/transaction_manager.h>
#include <server/transaction_server/transaction.h>

#include <server/object_server/type_handler_detail.h>

#include <server/node_tracker_server/node_directory_builder.h>
#include <server/node_tracker_server/config.h>

#include <server/security_server/security_manager.h>
#include <server/security_server/account.h>
#include <server/security_server/group.h>

#include <server/object_server/object_manager.h>

namespace NYT {
namespace NChunkServer {

using namespace NConcurrency;
using namespace NRpc;
using namespace NHydra;
using namespace NNodeTrackerServer;
using namespace NTransactionServer;
using namespace NObjectServer;
using namespace NObjectClient;
using namespace NYTree;
using namespace NCellMaster;
using namespace NCypressServer;
using namespace NSecurityServer;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJournalClient;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ChunkServerLogger;
static const auto ProfilingPeriod = TDuration::MilliSeconds(100);
// NB: Changing this value will invalidate all changelogs!
static const auto UnapprovedReplicaGracePeriod = TDuration::Seconds(15);

////////////////////////////////////////////////////////////////////////////////

class TChunkTreeBalancerCallbacks
    : public IChunkTreeBalancerCallbacks
{
public:
    TChunkTreeBalancerCallbacks(NCellMaster::TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    { }

    virtual void RefObject(NObjectServer::TObjectBase* object) override
    {
        Bootstrap_->GetObjectManager()->RefObject(object);
    }

    virtual void UnrefObject(NObjectServer::TObjectBase* object) override
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

    virtual TNullable<TTypeCreationOptions> GetCreationOptions() const override
    {
        return TTypeCreationOptions(
            EObjectTransactionMode::Required,
            EObjectAccountMode::Required);
    }

    virtual TObjectBase* CreateObject(
        TTransaction* transaction,
        TAccount* account,
        IAttributeDictionary* attributes,
        TReqCreateObjects* request,
        TRspCreateObjects* response) override;

    virtual void ResetAllObjects() override
    {
        // NB: All chunk type handlers share the same map.
        // No need to reset chunks multiple types.
        if (GetType() == EObjectType::Chunk) {
            TObjectTypeHandlerWithMapBase::ResetAllObjects();
        }
    }

protected:
    TImpl* const Owner_;


    virtual IObjectProxyPtr DoGetProxy(TChunk* chunk, TTransaction* transaction) override;

    virtual void DoDestroyObject(TChunk* chunk) override;

    virtual TTransaction* DoGetStagingTransaction(TChunk* chunk) override
    {
        return chunk->GetStagingTransaction();
    }

    virtual void DoUnstageObject(TChunk* chunk, bool recursive) override;

    virtual void DoResetObject(TChunk* chunk) override
    {
        TObjectTypeHandlerWithMapBase::DoResetObject(chunk);
        chunk->Reset();
    }
};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TChunkTypeHandler
    : public TChunkTypeHandlerBase
{
public:
    explicit TChunkTypeHandler(TImpl* owner)
        : TChunkTypeHandlerBase(owner)
    { }

    virtual EObjectType GetType() const override
    {
        return EObjectType::Chunk;
    }

private:
    virtual Stroka DoGetName(TChunk* chunk) override
    {
        return Format("chunk %v", chunk->GetId());
    }

};

////////////////////////////////////////////////////////////////////////////////

class TChunkManager::TErasureChunkTypeHandler
    : public TChunkTypeHandlerBase
{
public:
    explicit TErasureChunkTypeHandler(TImpl* owner)
        : TChunkTypeHandlerBase(owner)
    { }

    virtual EObjectType GetType() const override
    {
        return EObjectType::ErasureChunk;
    }

private:
    virtual Stroka DoGetName(TChunk* chunk) override
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
    virtual Stroka DoGetName(TChunk* chunk) override
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

    virtual TNullable<TTypeCreationOptions> GetCreationOptions() const override
    {
        return TTypeCreationOptions(
            EObjectTransactionMode::Required,
            EObjectAccountMode::Forbidden);
    }

    virtual TObjectBase* CreateObject(
        TTransaction* transaction,
        TAccount* account,
        IAttributeDictionary* attributes,
        TReqCreateObjects* request,
        TRspCreateObjects* response) override;

private:
    TImpl* const Owner_;


    virtual Stroka DoGetName(TChunkList* chunkList) override
    {
        return Format("chunk list %v", chunkList->GetId());
    }

    virtual IObjectProxyPtr DoGetProxy(TChunkList* chunkList, TTransaction* transaction) override;

    virtual void DoDestroyObject(TChunkList* chunkList) override;

    virtual TTransaction* DoGetStagingTransaction(TChunkList* chunkList) override
    {
        return chunkList->GetStagingTransaction();
    }

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
        , Profiler(ChunkServerProfiler)
        , AddedChunkCounter_("/added_chunks")
        , RemovedChunkCounter_("/removed_chunks")
        , AddedChunkReplicaCounter_("/added_chunk_replicas")
        , RemovedChunkReplicaCounter_("/removed_chunk_replicas")
    {
        RegisterMethod(BIND(&TImpl::UpdateChunkProperties, Unretained(this)));

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
    }

    void Initialize()
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(New<TChunkTypeHandler>(this));
        objectManager->RegisterHandler(New<TErasureChunkTypeHandler>(this));
        objectManager->RegisterHandler(New<TJournalChunkTypeHandler>(this));
        objectManager->RegisterHandler(New<TChunkListTypeHandler>(this));

        auto nodeTracker = Bootstrap_->GetNodeTracker();
        nodeTracker->SubscribeNodeRegistered(BIND(&TImpl::OnNodeRegistered, MakeWeak(this)));
        nodeTracker->SubscribeNodeUnregistered(BIND(&TImpl::OnNodeUnregistered, MakeWeak(this)));
        nodeTracker->SubscribeNodeRemoved(BIND(&TImpl::OnNodeRemoved, MakeWeak(this)));
        nodeTracker->SubscribeNodeConfigUpdated(BIND(&TImpl::OnNodeConfigUpdated, MakeWeak(this)));
        nodeTracker->SubscribeFullHeartbeat(BIND(&TImpl::OnFullHeartbeat, MakeWeak(this)));
        nodeTracker->SubscribeIncrementalHeartbeat(BIND(&TImpl::OnIncrementalHeartbeat, MakeWeak(this)));

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(),
            BIND(&TImpl::OnProfiling, MakeWeak(this)),
            ProfilingPeriod);
        ProfilingExecutor_->Start();
    }


    TMutationPtr CreateUpdateChunkPropertiesMutation(
        const NProto::TReqUpdateChunkProperties& request)
    {
        return CreateMutation(
            Bootstrap_->GetHydraFacade()->GetHydraManager(),
            request,
            this,
            &TImpl::UpdateChunkProperties);
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
            EWriteSessionType::User);
    }

    TChunk* CreateChunk(EObjectType type)
    {
        Profiler.Increment(AddedChunkCounter_);
        auto id = Bootstrap_->GetObjectManager()->GenerateId(type);
        auto chunkHolder = std::make_unique<TChunk>(id);
        return ChunkMap_.Insert(id, std::move(chunkHolder));
    }

    TChunkList* CreateChunkList()
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::ChunkList);
        auto chunkListHolder = std::make_unique<TChunkList>(id);
        return ChunkListMap_.Insert(id, std::move(chunkListHolder));
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


    void ConfirmChunk(
        TChunk* chunk,
        const std::vector<NChunkClient::TChunkReplica>& replicas,
        TChunkInfo* chunkInfo,
        TChunkMeta* chunkMeta)
    {
        YCHECK(!chunk->IsConfirmed());

        const auto& id = chunk->GetId();

        chunk->Confirm(chunkInfo, chunkMeta);

        auto nodeTracker = Bootstrap_->GetNodeTracker();

        const auto* mutationContext = GetCurrentMutationContext();
        auto mutationTimestamp = mutationContext->GetTimestamp();

        for (auto replica : replicas) {
            auto* node = nodeTracker->FindNode(replica.GetNodeId());
            if (!node) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Tried to confirm chunk %v at an unknown node %v",
                    id,
                    replica.GetNodeId());
                continue;
            }

            auto chunkWithIndex = chunk->IsJournal()
                ? TChunkPtrWithIndex(chunk, ActiveChunkReplicaIndex)
                : TChunkPtrWithIndex(chunk, replica.GetIndex());

            if (node->GetState() != ENodeState::Online) {
                LOG_DEBUG_UNLESS(IsRecovery(), "Tried to confirm chunk %v at %v which has invalid state %Qlv",
                    id,
                    node->GetDefaultAddress(),
                    node->GetState());
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

        if (ChunkReplicator_) {
            ChunkReplicator_->ScheduleChunkRefresh(chunk);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Chunk confirmed (ChunkId: %v)", id);
    }


    void StageChunkTree(TChunkTree* chunkTree, TTransaction* transaction, TAccount* account)
    {
        YASSERT(transaction);
        YASSERT(account);
        YASSERT(!chunkTree->IsStaged());

        chunkTree->SetStagingTransaction(transaction);
        chunkTree->SetStagingAccount(account);

        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->RefObject(account);
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
                transactionManager->UnstageObject(child, recursive);
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

    TJobListPtr FindJobList(TChunk* chunk)
    {
        return ChunkReplicator_->FindJobList(chunk);
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


    const yhash_set<TChunk*>& LostChunks() const;
    const yhash_set<TChunk*>& LostVitalChunks() const;
    const yhash_set<TChunk*>& OverreplicatedChunks() const;
    const yhash_set<TChunk*>& UnderreplicatedChunks() const;
    const yhash_set<TChunk*>& DataMissingChunks() const;
    const yhash_set<TChunk*>& ParityMissingChunks() const;
    const yhash_set<TChunk*>& QuorumMissingChunks() const;
    const yhash_set<TChunk*>& UnsafelyPlacedChunks() const;


    int GetTotalReplicaCount()
    {
        return TotalReplicaCount_;
    }

    bool IsReplicatorEnabled()
    {
        return ChunkReplicator_->IsEnabled();
    }


    void ScheduleChunkRefresh(TChunk* chunk)
    {
        ChunkReplicator_->ScheduleChunkRefresh(chunk);
    }

    void ScheduleNodeRefresh(TNode* node)
    {
        ChunkReplicator_->ScheduleNodeRefresh(node);
    }

    void ScheduleChunkPropertiesUpdate(TChunkTree* chunkTree)
    {
        ChunkReplicator_->SchedulePropertiesUpdate(chunkTree);
    }

    void MaybeScheduleChunkSeal(TChunk* chunk)
    {
        ChunkSealer_->ScheduleSeal(chunk);
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


    void SealChunk(TChunk* chunk, const TMiscExt& info)
    {
        if (!chunk->IsJournal()) {
            THROW_ERROR_EXCEPTION("Not a journal chunk");
        }

        if (!chunk->IsConfirmed()) {
            THROW_ERROR_EXCEPTION("Chunk is not confirmed");
        }

        if (chunk->IsSealed()) {
            THROW_ERROR_EXCEPTION("Chunk is already sealed");
        }

        chunk->Seal(info);

        OnChunkSealed(chunk);

        if (ChunkReplicator_) {
            ChunkReplicator_->ScheduleChunkRefresh(chunk);
        }

        LOG_DEBUG_UNLESS(IsRecovery(), "Chunk sealed (ChunkId: %v, RowCount: %v, UncompressedDataSize: %v, CompressedDataSize: %v)",
            chunk->GetId(),
            info.row_count(),
            info.uncompressed_data_size(),
            info.compressed_data_size());
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
            chunk->GetReadQuorum());
    }


    DECLARE_ENTITY_MAP_ACCESSORS(Chunk, TChunk, TChunkId);
    DECLARE_ENTITY_MAP_ACCESSORS(ChunkList, TChunkList, TChunkListId);

private:
    friend class TChunkTypeHandlerBase;
    friend class TChunkTypeHandler;
    friend class TErasureChunkTypeHandler;
    friend class TChunkListTypeHandler;

    const TChunkManagerConfigPtr Config_;

    TChunkTreeBalancer ChunkTreeBalancer_;

    int TotalReplicaCount_ = 0;

    bool NeedToRecomputeStatistics_ = false;

    NConcurrency::TPeriodicExecutorPtr ProfilingExecutor_;

    NProfiling::TProfiler Profiler;
    NProfiling::TSimpleCounter AddedChunkCounter_;
    NProfiling::TSimpleCounter RemovedChunkCounter_;
    NProfiling::TSimpleCounter AddedChunkReplicaCounter_;
    NProfiling::TSimpleCounter RemovedChunkReplicaCounter_;

    TChunkPlacementPtr ChunkPlacement_;
    TChunkReplicatorPtr ChunkReplicator_;
    TChunkSealerPtr ChunkSealer_;

    NHydra::TEntityMap<TChunkId, TChunk> ChunkMap_;
    NHydra::TEntityMap<TChunkListId, TChunkList> ChunkListMap_;


    void DestroyChunk(TChunk* chunk)
    {
        // Decrease staging resource usage; release account.
        UnstageChunk(chunk);

        // Cancel all jobs, reset status etc.
        if (ChunkReplicator_) {
            ChunkReplicator_->OnChunkDestroyed(chunk);
        }

        // Unregister chunk replicas from all known locations.
        auto unregisterReplica = [&] (TNodePtrWithIndex nodeWithIndex, bool cached) {
            auto* node = nodeWithIndex.GetPtr();
            TChunkPtrWithIndex chunkWithIndex(chunk, nodeWithIndex.GetIndex());
            node->RemoveReplica(chunkWithIndex, cached);
            if (ChunkReplicator_ && !cached) {
                ChunkReplicator_->ScheduleReplicaRemoval(node, chunkWithIndex);
            }
        };

        for (auto replica : chunk->StoredReplicas()) {
            unregisterReplica(replica, false);
        }

        if (chunk->CachedReplicas()) {
            for (auto replica : *chunk->CachedReplicas()) {
                unregisterReplica(replica, true);
            }
        }

        Profiler.Increment(RemovedChunkCounter_);
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
    }


    void OnNodeRegistered(TNode* node)
    {
        const auto& config = node->GetConfig();
        node->SetDecommissioned(config->Decommissioned);

        if (ChunkPlacement_) {
            ChunkPlacement_->OnNodeRegistered(node);
        }

        if (ChunkReplicator_) {
            ChunkReplicator_->OnNodeRegistered(node);
            ChunkReplicator_->ScheduleNodeRefresh(node);
        }
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

    void OnNodeRemoved(TNode* node)
    {
        for (auto replica : node->StoredReplicas()) {
            RemoveChunkReplica(node, replica, false, ERemoveReplicaReason::NodeRemoved);
        }

        for (auto replica : node->CachedReplicas()) {
            RemoveChunkReplica(node, replica, true, ERemoveReplicaReason::NodeRemoved);
        }

        if (ChunkPlacement_) {
            ChunkPlacement_->OnNodeRemoved(node);
        }

        if (ChunkReplicator_) {
            ChunkReplicator_->OnNodeRemoved(node);
        }
    }

    void OnNodeConfigUpdated(TNode* node)
    {
        const auto& config = node->GetConfig();
        if (config->Decommissioned != node->GetDecommissioned()) {
            if (config->Decommissioned) {
                LOG_INFO_UNLESS(IsRecovery(), "Node decommissioned (NodeId: %v, Address: %v)",
                    node->GetId(),
                    node->GetDefaultAddress());
            } else {
                LOG_INFO_UNLESS(IsRecovery(), "Node is no longer decommissioned (NodeId: %v, Address: %v)",
                    node->GetId(),
                    node->GetDefaultAddress());
            }
            node->SetDecommissioned(config->Decommissioned);
        }

        if (ChunkReplicator_) {
            ChunkReplicator_->ScheduleNodeRefresh(node);
        }
    }

    void OnFullHeartbeat(TNode* node, const NNodeTrackerServer::NProto::TReqFullHeartbeat& request)
    {
        YCHECK(node->StoredReplicas().empty());
        YCHECK(node->CachedReplicas().empty());

        for (const auto& chunkInfo : request.chunks()) {
            ProcessAddedChunk(node, chunkInfo, false);
        }

        if (ChunkPlacement_) {
            ChunkPlacement_->OnNodeUpdated(node);
        }
    }

    void OnIncrementalHeartbeat(
        TNode* node,
        const TReqIncrementalHeartbeat& request,
        TRspIncrementalHeartbeat* /*response*/)
    {
        node->ShrinkHashTables();

        for (const auto& chunkInfo : request.added_chunks()) {
            ProcessAddedChunk(node, chunkInfo, true);
        }

        for (const auto& chunkInfo : request.removed_chunks()) {
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
                reason = ERemoveReplicaReason::ChunkIsDead;
            } else if (mutationTimestamp > registerTimestamp + UnapprovedReplicaGracePeriod) {
                reason = ERemoveReplicaReason::FailedToApprove;
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


    void UpdateChunkProperties(const NProto::TReqUpdateChunkProperties& request)
    {
        for (const auto& update : request.updates()) {
            auto chunkId = FromProto<TChunkId>(update.chunk_id());
            auto* chunk = FindChunk(chunkId);
            if (!IsObjectAlive(chunk))
                continue;

            if (chunk->IsStaged()) {
                LOG_WARNING("Updating properties for staged chunk %v", chunkId);
                continue;
            }

            bool changed = false;
            if (update.has_replication_factor() && chunk->GetReplicationFactor() != update.replication_factor()) {
                YCHECK(!chunk->IsErasure());
                changed = true;
                chunk->SetReplicationFactor(update.replication_factor());
            }

            if (update.has_vital() && chunk->GetVital() != update.vital()) {
                changed = true;
                chunk->SetVital(update.vital());
            }

            if (ChunkReplicator_ && changed) {
                ChunkReplicator_->ScheduleChunkRefresh(chunk);
            }
        }
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
        // COMPAT(babenko): required to properly initialize partial sums for chunk lists.
        if (context.GetVersion() < 100) {
            ScheduleRecomputeStatistics();
        }
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        ChunkMap_.LoadValues(context);
        ChunkListMap_.LoadValues(context);
    }

    virtual void OnAfterSnapshotLoaded() override
    {
        TMasterAutomatonPart::OnAfterSnapshotLoaded();

        // Compute chunk replica count.
        auto nodeTracker = Bootstrap_->GetNodeTracker();
        TotalReplicaCount_ = 0;
        for (const auto& pair : nodeTracker->Nodes()) {
            auto* node = pair.second;
            TotalReplicaCount_ += node->StoredReplicas().size();
            TotalReplicaCount_ += node->CachedReplicas().size();
        }
    }


    virtual void Clear() override
    {
        TMasterAutomatonPart::Clear();

        ChunkMap_.Clear();
        ChunkListMap_.Clear();
        TotalReplicaCount_ = 0;
    }

    void ScheduleRecomputeStatistics()
    {
        NeedToRecomputeStatistics_ = true;
    }

    const TChunkTreeStatistics& ComputeStatisticsFor(TChunkList* chunkList, TAtomic visitMark)
    {
        auto& statistics = chunkList->Statistics();
        if (chunkList->GetVisitMark() != visitMark) {
            chunkList->SetVisitMark(visitMark);

            statistics = TChunkTreeStatistics();
            statistics.Rank = 1;
            int childrenCount = chunkList->Children().size();

            auto& rowCountSums = chunkList->RowCountSums();
            rowCountSums.clear();

            auto& chunkCountSums = chunkList->ChunkCountSums();
            chunkCountSums.clear();

            auto& dataSizeSums = chunkList->DataSizeSums();
            dataSizeSums.clear();

            for (int childIndex = 0; childIndex < childrenCount; ++childIndex) {
                auto* child = chunkList->Children()[childIndex];
                TChunkTreeStatistics childStatistics;
                switch (child->GetType()) {
                    case EObjectType::Chunk:
                    case EObjectType::ErasureChunk:
                    case EObjectType::JournalChunk:
                        childStatistics.Accumulate(child->AsChunk()->GetStatistics());
                        break;

                    case EObjectType::ChunkList:
                        childStatistics = ComputeStatisticsFor(child->AsChunkList(), visitMark);
                        break;

                    default:
                        YUNREACHABLE();
                }

                if (childIndex + 1 < childrenCount) {
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
        }
        return statistics;
    }

    void RecomputeStatistics()
    {
        // Chunk trees traversal with memoization.

        LOG_INFO("Started recomputing statistics");

        auto mark = TChunkList::GenerateVisitMark();

        // Force all statistics to be recalculated.
        for (const auto& pair : ChunkListMap_) {
            auto* chunkList = pair.second;
            ComputeStatisticsFor(chunkList, mark);
        }

        LOG_INFO("Finished recomputing statistics");
    }

    void OnChunkSealed(TChunk* chunk)
    {
        YASSERT(chunk->IsSealed());

        if (chunk->Parents().empty())
            return;

        // Go upwards and apply delta.
        YCHECK(chunk->Parents().size() == 1);
        auto* chunkList = chunk->Parents()[0];

        auto statisticsDelta = chunk->GetStatistics();
        AccumulateUniqueAncestorsStatistics(chunkList, statisticsDelta);

        auto owningNodes = GetOwningNodes(chunk);
        auto securityManager = Bootstrap_->GetSecurityManager();
        for (auto* node : owningNodes) {
            securityManager->UpdateAccountNodeUsage(node);
        }
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

        ChunkPlacement_->Start();
        ChunkReplicator_->Start();
        ChunkSealer_->Start();
    }

    virtual void OnStopLeading() override
    {
        TMasterAutomatonPart::OnStopLeading();

        if (ChunkPlacement_) {
            ChunkPlacement_->Stop();
            ChunkPlacement_.Reset();
        }

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

        if (ChunkReplicator_ && !cached) {
            ChunkReplicator_->ScheduleChunkRefresh(chunk);
        }

        if (ChunkSealer_ && !cached && chunk->IsJournal()) {
            ChunkSealer_->ScheduleSeal(chunk);
        }

        if (reason == EAddReplicaReason::IncrementalHeartbeat || reason == EAddReplicaReason::Confirmation) {
            Profiler.Increment(AddedChunkReplicaCounter_);
        }
    }

    void RemoveChunkReplica(TNode* node, TChunkPtrWithIndex chunkWithIndex, bool cached, ERemoveReplicaReason reason)
    {
        auto* chunk = chunkWithIndex.GetPtr();
        auto nodeId = node->GetId();
        TNodePtrWithIndex nodeWithIndex(node, chunkWithIndex.GetIndex());
        TChunkIdWithIndex chunkIdWithIndex(chunk->GetId(), nodeWithIndex.GetIndex());

        if (reason == ERemoveReplicaReason::IncrementalHeartbeat && !node->HasReplica(chunkWithIndex, cached)) {
            LOG_DEBUG_UNLESS(IsRecovery(), "Chunk replica is already removed (ChunkId: %v, Cached: %v, Reason: %v, NodeId: %v, Address: %v)",
                chunkWithIndex,
                cached,
                reason,
                nodeId,
                node->GetDefaultAddress());
            return;
        }

        chunk->RemoveReplica(nodeWithIndex, cached);

        switch (reason) {
            case ERemoveReplicaReason::IncrementalHeartbeat:
            case ERemoveReplicaReason::FailedToApprove:
            case ERemoveReplicaReason::ChunkIsDead:
                node->RemoveReplica(chunkWithIndex, cached);
                if (ChunkReplicator_ && !cached) {
                    ChunkReplicator_->OnReplicaRemoved(node, chunkWithIndex, reason);
                }
                break;
            case ERemoveReplicaReason::NodeRemoved:
                // Do nothing.
                break;
            default:
                YUNREACHABLE();
        }

        if (!IsRecovery()) {
            LOG_EVENT(
                Logger,
                reason == ERemoveReplicaReason::NodeRemoved ||
                reason == ERemoveReplicaReason::ChunkIsDead
                ? NLogging::ELogLevel::Trace : NLogging::ELogLevel::Debug,
                "Chunk replica removed (ChunkId: %v, Cached: %v, Reason: %v, NodeId: %v, Address: %v)",
                chunkWithIndex,
                cached,
                reason,
                nodeId,
                node->GetDefaultAddress());
        }

        if (ChunkReplicator_ && !cached) {
            ChunkReplicator_->ScheduleChunkRefresh(chunk);
        }

        Profiler.Increment(RemovedChunkReplicaCounter_);
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


    void OnProfiling()
    {
        if (ChunkReplicator_) {
            Profiler.Enqueue("/refresh_list_size", ChunkReplicator_->GetRefreshListSize());
            Profiler.Enqueue("/properties_update_list_size", ChunkReplicator_->GetPropertiesUpdateListSize());
        }
    }

};

DEFINE_ENTITY_MAP_ACCESSORS(TChunkManager::TImpl, Chunk, TChunk, TChunkId, ChunkMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TChunkManager::TImpl, ChunkList, TChunkList, TChunkListId, ChunkListMap_)

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
    return CreateChunkProxy(Bootstrap_, chunk);
}

TObjectBase* TChunkManager::TChunkTypeHandlerBase::CreateObject(
    TTransaction* transaction,
    TAccount* account,
    IAttributeDictionary* /*attributes*/,
    TReqCreateObjects* request,
    TRspCreateObjects* /*response*/)
{
    YCHECK(transaction);
    YCHECK(account);

    account->ValidateResourceUsageIncrease(TClusterResources(1, 0, 1));

    auto chunkType = GetType();
    bool isErasure = (chunkType == EObjectType::ErasureChunk);
    bool isJournal = (chunkType == EObjectType::JournalChunk);

    const auto& requestExt = request->GetExtension(TReqCreateChunkExt::create_chunk_ext);
    auto erasureCodecId = isErasure ? NErasure::ECodec(requestExt.erasure_codec()) : NErasure::ECodec::None;
    int replicationFactor = isErasure ? 1 : requestExt.replication_factor();
    int readQuorum = isJournal ? requestExt.read_quorum() : 0;
    int writeQuorum = isJournal ? requestExt.write_quorum() : 0;
    auto chunkListId = requestExt.has_chunk_list_id() ? FromProto<TChunkListId>(requestExt.chunk_list_id()) : NullChunkListId;

    auto* chunk = Owner_->CreateChunk(chunkType);
    chunk->SetReplicationFactor(replicationFactor);
    chunk->SetReadQuorum(readQuorum);
    chunk->SetWriteQuorum(writeQuorum);
    chunk->SetErasureCodec(erasureCodecId);
    chunk->SetMovable(requestExt.movable());
    chunk->SetVital(requestExt.vital());

    Owner_->StageChunkTree(chunk, transaction, account);

    if (chunkListId != NullChunkId) {
        auto* chunkList = Owner_->GetChunkListOrThrow(chunkListId);
        Owner_->AttachToChunkList(chunkList, chunk);
    }

    LOG_DEBUG_UNLESS(Owner_->IsRecovery(),
        "Chunk created "
        "(ChunkId: %v, ChunkListId: %v, TransactionId: %v, Account: %v, ReplicationFactor: %v, "
        "ReadQuorum: %v, WriteQuorum: %v, ErasureCodec: %v, Movable: %v, Vital: %v)",
        chunk->GetId(),
        chunkListId,
        transaction->GetId(),
        account->GetName(),
        chunk->GetReplicationFactor(),
        chunk->GetReadQuorum(),
        chunk->GetWriteQuorum(),
        erasureCodecId,
        requestExt.movable(),
        requestExt.vital());

    return chunk;
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
    return CreateChunkListProxy(Bootstrap_, chunkList);
}

TObjectBase* TChunkManager::TChunkListTypeHandler::CreateObject(
    TTransaction* transaction,
    TAccount* account,
    IAttributeDictionary* /*attributes*/,
    TReqCreateObjects* /*request*/,
    TRspCreateObjects* /*response*/)
{
    auto* chunkList = Owner_->CreateChunkList();
    chunkList->SetStagingTransaction(transaction);
    chunkList->SetStagingAccount(account);
    return chunkList;
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

TChunkManager::~TChunkManager()
{ }

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

TMutationPtr TChunkManager::CreateUpdateChunkPropertiesMutation(
    const NProto::TReqUpdateChunkProperties& request)
{
    return Impl_->CreateUpdateChunkPropertiesMutation(request);
}

TChunk* TChunkManager::CreateChunk(EObjectType type)
{
    return Impl_->CreateChunk(type);
}

TChunkList* TChunkManager::CreateChunkList()
{
    return Impl_->CreateChunkList();
}

void TChunkManager::ConfirmChunk(
    TChunk* chunk,
    const std::vector<NChunkClient::TChunkReplica>& replicas,
    TChunkInfo* chunkInfo,
    TChunkMeta* chunkMeta)
{
    Impl_->ConfirmChunk(
        chunk,
        replicas,
        chunkInfo,
        chunkMeta);
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

TJobListPtr TChunkManager::FindJobList(TChunk* chunk)
{
    return Impl_->FindJobList(chunk);
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

void TChunkManager::MaybeScheduleChunkSeal(TChunk* chunk)
{
    Impl_->MaybeScheduleChunkSeal(chunk);
}

int TChunkManager::GetTotalReplicaCount()
{
    return Impl_->GetTotalReplicaCount();
}

EChunkStatus TChunkManager::ComputeChunkStatus(TChunk* chunk)
{
    return Impl_->ComputeChunkStatus(chunk);
}

void TChunkManager::SealChunk(TChunk* chunk, const TMiscExt& info)
{
    Impl_->SealChunk(chunk, info);
}

TFuture<TMiscExt> TChunkManager::GetChunkQuorumInfo(TChunk* chunk)
{
    return Impl_->GetChunkQuorumInfo(chunk);
}

DELEGATE_ENTITY_MAP_ACCESSORS(TChunkManager, Chunk, TChunk, TChunkId, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TChunkManager, ChunkList, TChunkList, TChunkListId, *Impl_)

DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, LostChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, LostVitalChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, OverreplicatedChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, UnderreplicatedChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, DataMissingChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, ParityMissingChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, QuorumMissingChunks, *Impl_);
DELEGATE_BYREF_RO_PROPERTY(TChunkManager, yhash_set<TChunk*>, UnsafelyPlacedChunks, *Impl_);

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
