#include "job.h"

#include <yt/server/node_tracker_server/node.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/string.h>

namespace NYT {
namespace NChunkServer {

using namespace NErasure;
using namespace NNodeTrackerServer;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NChunkClient;

////////////////////////////////////////////////////////////////////////////////

TJob::TJob(
    EJobType type,
    const TJobId& jobId,
    const TChunkIdWithIndex& chunkIdWithIndex,
    TNode* node,
    const TNodeList& targets,
    const TPartIndexList& erasedIndexes,
    TInstant startTime,
    const TNodeResources& resourceUsage)
    : JobId_(jobId)
    , Type_(type)
    , ChunkIdWithIndex_(chunkIdWithIndex)
    , Node_(node)
    , TargetAddresses_(ConvertToStrings(targets, TNodePtrAddressFormatter()))
    , ErasedIndexes_(erasedIndexes)
    , StartTime_(startTime)
    , ResourceUsage_(resourceUsage)
    , State_(EJobState::Running)
{ }

TJobPtr TJob::CreateForeign(
    const TJobId& jobId,
    const NNodeTrackerClient::NProto::TNodeResources& resourceUsage)
{
    return New<TJob>(
        EJobType::Foreign,
        jobId,
        TChunkIdWithIndex(NullChunkId, 0),
        static_cast<TNode*>(nullptr),
        TNodeList(),
        TPartIndexList(),
        TInstant::Zero(),
        resourceUsage);
}

TJobPtr TJob::CreateReplicate(
    const TChunkIdWithIndex& chunkIdWithIndex,
    TNode* node,
    const TNodeList& targets,
    const NNodeTrackerClient::NProto::TNodeResources& resourceUsage)
{
    return New<TJob>(
        EJobType::ReplicateChunk,
        TJobId::Create(),
        chunkIdWithIndex,
        node,
        targets,
        TPartIndexList(),
        TInstant::Now(),
        resourceUsage);
}

TJobPtr TJob::CreateRemove(
    const TChunkIdWithIndex& chunkIdWithIndex,
    NNodeTrackerServer::TNode* node,
    const NNodeTrackerClient::NProto::TNodeResources& resourceUsage)
{
    return New<TJob>(
        EJobType::RemoveChunk,
        TJobId::Create(),
        chunkIdWithIndex,
        node,
        TNodeList(),
        TPartIndexList(),
        TInstant::Now(),
        resourceUsage);
}

TJobPtr TJob::CreateRepair(
    const TChunkId& chunkId,
    NNodeTrackerServer::TNode* node,
    const TNodeList& targets,
    const TPartIndexList& erasedIndexes,
    const NNodeTrackerClient::NProto::TNodeResources& resourceUsage)
{
    return New<TJob>(
        EJobType::RepairChunk,
        TJobId::Create(),
        TChunkIdWithIndex(chunkId, 0),
        node,
        targets,
        erasedIndexes,
        TInstant::Now(),
        resourceUsage);
}

TJobPtr TJob::CreateSeal(
    const TChunkId& chunkId,
    TNode* node,
    const TNodeResources& resourceUsage)
{
    return New<TJob>(
        EJobType::SealChunk,
        TJobId::Create(),
        TChunkIdWithIndex(chunkId, 0),
        node,
        TNodeList(),
        TPartIndexList(),
        TInstant::Now(),
        resourceUsage);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
