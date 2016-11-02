#pragma once

#include "public.h"
#include "chunk_replica.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/node_tracker_server/node.h>

#include <yt/core/misc/nullable.h>

#include <util/generic/map.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunkPlacement
    : public TRefCounted
{
public:
    TChunkPlacement(
        TChunkManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);

    void OnNodeRegistered(TNode* node);
    void OnNodeUnregistered(TNode* node);
    void OnNodeUpdated(TNode* node);
    void OnNodeDisposed(TNode* node);

    TNodeList AllocateWriteTargets(
        int mediumIndex,
        TChunk* chunk,
        int desiredCount,
        int minCount,
        TNullable<int> replicationFactorOverride,
        const TNodeList* forbiddenNodes,
        const TNullable<Stroka>& preferredHostName,
        NChunkClient::ESessionType sessionType);

    TNodeList AllocateWriteTargets(
        int mediumIndex,
        TChunk* chunk,
        int desiredCount,
        int minCount,
        TNullable<int> replicationFactorOverride,
        NChunkClient::ESessionType sessionType);

    TNode* GetRemovalTarget(TChunkPtrWithIndexes chunkWithIndexes);

    bool HasBalancingTargets(int mediumIndex, double maxFillFactor);

    std::vector<TChunkPtrWithIndexes> GetBalancingChunks(
        int mediumIndex,
        TNode* node,
        int replicaCount);

    TNode* AllocateBalancingTarget(
        int mediumIndex,
        TChunk* chunk,
        double maxFillFactor);

private:
    class TTargetCollector;

    const TChunkManagerConfigPtr Config_;
    NCellMaster::TBootstrap* const Bootstrap_;

    using TFillFactorToNodeMaps = TPerMediumArray<TFillFactorToNodeMap>;
    using TLoadFactorToNodeMaps = TFillFactorToNodeMaps;

    TFillFactorToNodeMaps MediumToFillFactorToNode_;
    TLoadFactorToNodeMaps MediumToLoadFactorToNode_;

    void InsertToFillFactorMaps(TNode* node);
    void RemoveFromFillFactorMaps(TNode* node);

    void InsertToLoadFactorMaps(TNode* node);
    void RemoveFromLoadFactorMaps(TNode* node);

    TNodeList GetWriteTargets(
        int mediumIndex,
        TChunk* chunk,
        int desiredCount,
        int minCount,
        bool forceRackAwareness,
        TNullable<int> replicationFactorOverride,
        const TNodeList* forbiddenNodes = nullptr,
        const TNullable<Stroka>& preferredHostName = Null);

    TNode* GetBalancingTarget(
        int mediumIndex,
        TChunk* chunk,
        double maxFillFactor);

    static bool IsAcceptedChunkType(
        int mediumIndex,
        TNode* node,
        NObjectClient::EObjectType type);

    bool IsValidWriteTarget(
        int mediumIndex,
        TNode* node,
        NObjectClient::EObjectType chunkType,
        TTargetCollector* collector,
        bool enableRackAwareness);

    bool IsValidBalancingTarget(
        int mediumIndex,
        TNode* node,
        NObjectClient::EObjectType chunkType,
        TTargetCollector* collector,
        bool enableRackAwareness);

    bool IsValidRemovalTarget(TNode* node);

    void AddSessionHint(
        TNode* node,
        NChunkClient::ESessionType sessionType);

    int GetMaxReplicasPerRack(
        TChunk* chunk,
        int mediumIndex,
        TNullable<int> replicationFactorOverride);
};

DEFINE_REFCOUNTED_TYPE(TChunkPlacement)

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
