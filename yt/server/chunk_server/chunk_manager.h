#pragma once

#include "public.h"
#include "chunk_replica.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/chunk_server/chunk_manager.pb.h>

#include <yt/server/hydra/composite_automaton.h>
#include <yt/server/hydra/entity_map.h>
#include <yt/server/hydra/mutation.h>

#include <yt/server/object_server/public.h>

#include <yt/ytlib/chunk_client/chunk_replica.h>

#include <yt/core/actions/signal.h>

#include <yt/core/misc/nullable.h>
#include <yt/core/misc/small_vector.h>

#include <yt/core/rpc/service_detail.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunkManager
    : public TRefCounted
{
public:
    TChunkManager(
        TChunkManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);

    ~TChunkManager();

    void Initialize();

    NHydra::TMutationPtr CreateUpdateChunkPropertiesMutation(
        const NProto::TReqUpdateChunkProperties& request);

    // Pass RPC service context to avoid copying request message.
    using TCtxExportChunks = NRpc::TTypedServiceContext<
        NChunkClient::NProto::TReqExportChunks,
        NChunkClient::NProto::TRspExportChunks>;
    using TCtxExportChunksPtr = TIntrusivePtr<TCtxExportChunks>;
    NHydra::TMutationPtr CreateExportChunksMutation(
        TCtxExportChunksPtr context);

    // Pass RPC service context to avoid copying request message.
    using TCtxImportChunks = NRpc::TTypedServiceContext<
        NChunkClient::NProto::TReqImportChunks,
        NChunkClient::NProto::TRspImportChunks>;
    using TCtxImportChunksPtr = TIntrusivePtr<TCtxImportChunks>;
    NHydra::TMutationPtr CreateImportChunksMutation(
        TCtxImportChunksPtr context);

    DECLARE_ENTITY_MAP_ACCESSORS(Chunk, TChunk, TChunkId);
    TChunk* GetChunkOrThrow(const TChunkId& id);

    DECLARE_ENTITY_MAP_ACCESSORS(ChunkList, TChunkList, TChunkListId);
    TChunkList* GetChunkListOrThrow(const TChunkListId& id);

    TChunkTree* FindChunkTree(const TChunkTreeId& id);
    TChunkTree* GetChunkTree(const TChunkTreeId& id);
    TChunkTree* GetChunkTreeOrThrow(const TChunkTreeId& id);

    TNodeList AllocateWriteTargets(
        TChunk* chunk,
        int desiredCount,
        int minCount,
        TNullable<int> replicationFactorOverride,
        const TNodeList* forbiddenNodes,
        const TNullable<Stroka>& preferredHostName);

    TChunk* CreateChunk(NObjectServer::EObjectType type);
    TChunkList* CreateChunkList();

    void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree** childrenBegin,
        TChunkTree** childrenEnd);
    void AttachToChunkList(
        TChunkList* chunkList,
        const std::vector<TChunkTree*>& children);
    void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree* child);

    void DetachFromChunkList(
        TChunkList* chunkList,
        TChunkTree** childrenBegin,
        TChunkTree** childrenEnd);
    void DetachFromChunkList(
        TChunkList* chunkList,
        const std::vector<TChunkTree*>& children);
    void DetachFromChunkList(
        TChunkList* chunkList,
        TChunkTree* child);

    void RebalanceChunkTree(TChunkList* chunkList);

    void ConfirmChunk(
        TChunk* chunk,
        const std::vector<NChunkClient::TChunkReplica>& replicas,
        NChunkClient::NProto::TChunkInfo* chunkInfo,
        NChunkClient::NProto::TChunkMeta* chunkMeta);
    
    void UnstageChunk(TChunk* chunk);
    void UnstageChunkList(TChunkList* chunkList, bool recursive);

    TNodePtrWithIndexList LocateChunk(TChunkPtrWithIndex chunkWithIndex);

    void ClearChunkList(TChunkList* chunkList);

    TJobPtr FindJob(const TJobId& id);
    TJobListPtr FindJobList(TChunk* chunk);

    void ScheduleJobs(
        TNode* node,
        const std::vector<TJobPtr>& currentJobs,
        std::vector<TJobPtr>* jobsToStart,
        std::vector<TJobPtr>* jobsToAbort,
        std::vector<TJobPtr>* jobsToRemove);

    bool IsReplicatorEnabled();

    void ScheduleChunkRefresh(TChunk* chunk);
    void ScheduleNodeRefresh(TNode* node);
    void ScheduleChunkPropertiesUpdate(TChunkTree* chunkTree);
    void ScheduleChunkSeal(TChunk* chunk);

    const yhash_set<TChunk*>& LostVitalChunks() const;
    const yhash_set<TChunk*>& LostChunks() const;
    const yhash_set<TChunk*>& OverreplicatedChunks() const;
    const yhash_set<TChunk*>& UnderreplicatedChunks() const;
    const yhash_set<TChunk*>& DataMissingChunks() const;
    const yhash_set<TChunk*>& ParityMissingChunks() const;
    const yhash_set<TChunk*>& QuorumMissingChunks() const;
    const yhash_set<TChunk*>& UnsafelyPlacedChunks() const;

    //! Returns the total number of all chunk replicas.
    int GetTotalReplicaCount();

    EChunkStatus ComputeChunkStatus(TChunk* chunk);

    //! Computes misc extension of a given journal chunk
    //! by querying a quorum of replicas (if the chunk is not sealed).
    TFuture<NChunkClient::NProto::TMiscExt> GetChunkQuorumInfo(NChunkServer::TChunk* chunk);

    //! Seals the chunk and the journals containing it.
    void SealChunk(
        NChunkServer::TChunk* chunk,
        const NChunkClient::NProto::TMiscExt& info);

private:
    class TImpl;
    class TChunkTypeHandlerBase;
    class TRegularChunkTypeHandler;
    class TErasureChunkTypeHandler;
    class TJournalChunkTypeHandler;
    class TChunkListTypeHandler;

    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TChunkManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
