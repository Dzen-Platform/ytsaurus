#pragma once

#include <yt/server/node_tracker_server/public.h>

#include <yt/server/hydra/public.h>

#include <yt/ytlib/chunk_client/block_id.h>
#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/core/erasure/public.h>

#include <yt/core/misc/public.h>
#include <yt/core/misc/small_vector.h>

#include <map>
#include <array>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

using NChunkClient::TChunkId;
using NChunkClient::TChunkListId;
using NChunkClient::TChunkTreeId;
using NChunkClient::TMediumId;
using NChunkClient::NullChunkId;
using NChunkClient::NullChunkListId;
using NChunkClient::NullChunkTreeId;
using NChunkClient::TBlockOffset;
using NChunkClient::EChunkType;
using NChunkClient::TBlockId;
using NChunkClient::TypicalReplicaCount;
using NChunkClient::MaxMediumCount;
using NChunkClient::MediumIndexBound;
using NChunkClient::DefaultStoreMediumIndex;
using NChunkClient::MaxMediumPriority;

using NJobTrackerClient::TJobId;
using NJobTrackerClient::EJobType;
using NJobTrackerClient::EJobState;

using NNodeTrackerClient::TNodeId;
using NNodeTrackerClient::InvalidNodeId;
using NNodeTrackerClient::MaxNodeId;

using NObjectClient::TTransactionId;
using NObjectClient::NullTransactionId;

using NNodeTrackerServer::TNode;
using NNodeTrackerServer::TNodeList;

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENTITY_TYPE(TChunk, TChunkId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TChunkList, TChunkListId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TMedium, TMediumId, NObjectClient::TDirectObjectIdHash)

class TChunkTree;
class TChunkOwnerBase;
class TDataNode;

template <class T>
class TPtrWithIndex;

template <class T>
class TPtrWithIndexes;

using TNodePtrWithIndexes = TPtrWithIndexes<NNodeTrackerServer::TNode>;
using TNodePtrWithIndexesList = SmallVector<TNodePtrWithIndexes, TypicalReplicaCount>;

using TChunkPtrWithIndexes = TPtrWithIndexes<TChunk>;
using TChunkPtrWithIndex = NChunkServer::TPtrWithIndex<TChunk>;

struct TChunkTreeStatistics;
struct TTotalNodeStatistics;

DECLARE_REFCOUNTED_CLASS(TJob)

DECLARE_REFCOUNTED_CLASS(TChunkManager)
DECLARE_REFCOUNTED_CLASS(TChunkReplicator)
DECLARE_REFCOUNTED_CLASS(TChunkSealer)
DECLARE_REFCOUNTED_CLASS(TChunkPlacement)

DECLARE_REFCOUNTED_CLASS(TChunkManagerConfig)

//! Used as an expected upper bound in SmallVector.
constexpr int TypicalChunkParentCount = 2;

//! The number of supported replication priorities.
//! The smaller the more urgent.
/*! current RF == 1 -> priority = 0
 *  current RF == 2 -> priority = 1
 *  current RF >= 3 -> priority = 2
 */
constexpr int ReplicationPriorityCount = 3;

constexpr int LastSeenReplicaCount = 16;
// Cf. #TChunk::LastSeenReplicas.
static_assert(LastSeenReplicaCount >= NErasure::MaxTotalPartCount, "LastSeenReplicaCount < NErasure::MaxTotalPartCount");

DEFINE_BIT_ENUM(EChunkStatus,
    ((None)              (0x0000))
    ((Underreplicated)   (0x0001))
    ((Overreplicated)    (0x0002))
    ((Lost)              (0x0004))
    ((DataMissing)       (0x0008))
    ((ParityMissing)     (0x0010))
    ((QuorumMissing)     (0x0020))
    ((Safe)              (0x0040))
    ((Sealed)            (0x0080))
    ((UnsafelyPlaced)    (0x0100))
);

DEFINE_BIT_ENUM(ECrossMediumChunkStatus,
    ((None)              (0x0000))
    ((Lost)              (0x0004))
    ((DataMissing)       (0x0008))
    ((ParityMissing)     (0x0010))
    ((Precarious)        (0x0200)) // All replicas are on transient media.
    ((MediumWiseLost)    (0x0400)) // Lost on some media, but not others.
);

DEFINE_BIT_ENUM(EChunkScanKind,
    ((None)             (0x0000))
    ((Refresh)          (0x0001))
    ((PropertiesUpdate) (0x0002))
    ((Seal)             (0x0004))
);

DEFINE_ENUM(EChunkListKind,
    ((Static)                (0))
    ((SortedDynamicRoot)     (1))
    ((SortedDynamicTablet)   (2))
    ((OrderedDynamicRoot)    (3))
    ((OrderedDynamicTablet)  (4))
);

typedef std::list<TChunkPtrWithIndexes> TChunkRepairQueue;
typedef TChunkRepairQueue::iterator TChunkRepairQueueIterator;

typedef std::multimap<double, NNodeTrackerServer::TNode*> TFillFactorToNodeMap;
typedef TFillFactorToNodeMap::iterator TFillFactorToNodeIterator;

typedef std::multimap<double, NNodeTrackerServer::TNode*> TLoadFactorToNodeMap;
typedef TLoadFactorToNodeMap::iterator TLoadFactorToNodeIterator;

using TMediumSet = std::bitset<MaxMediumCount>;

template <typename T>
using TPerMediumArray = std::array<T, MaxMediumCount>;
using TPerMediumIntArray = TPerMediumArray<int>;

constexpr int MediumDefaultPriority = 0;

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
