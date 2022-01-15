#pragma once

#include <yt/yt/server/master/node_tracker_server/public.h>

#include <yt/yt/server/master/object_server/public.h>

#include <yt/yt/server/lib/hydra_common/public.h>

#include <yt/yt/ytlib/chunk_client/block_id.h>

#include <yt/yt/ytlib/job_tracker_client/public.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/ytlib/object_client/public.h>

#include <yt/yt/client/tablet_client/public.h>

#include <yt/yt/library/erasure/impl/public.h>

#include <yt/yt/core/misc/compact_vector.h>

#include <map>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

using NChunkClient::TChunkId;
using NChunkClient::TChunkViewId;
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
using NChunkClient::DefaultCacheMediumIndex;
using NChunkClient::MaxMediumPriority;
using NChunkClient::TDataCenterName;
using NChunkClient::TChunkLocationUuid;
using NChunkClient::TMediumMap;
using NChunkClient::TMediumIntMap;
using NChunkClient::TConsistentReplicaPlacementHash;
using NChunkClient::NullConsistentReplicaPlacementHash;
using NChunkClient::ChunkReplicaIndexBound;

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

using NTabletClient::TDynamicStoreId;

using TChunkLocationId = NObjectClient::TObjectId;

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENTITY_TYPE(TChunkLocation, TChunkLocationId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TChunk, TChunkId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TChunkView, TChunkViewId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TDynamicStore, TDynamicStoreId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TChunkList, TChunkListId, NObjectClient::TDirectObjectIdHash)
DECLARE_ENTITY_TYPE(TMedium, TMediumId, NObjectClient::TDirectObjectIdHash)

DECLARE_MASTER_OBJECT_TYPE(TChunkLocation)
DECLARE_MASTER_OBJECT_TYPE(TChunk)
DECLARE_MASTER_OBJECT_TYPE(TChunkList)
DECLARE_MASTER_OBJECT_TYPE(TChunkOwnerBase)
DECLARE_MASTER_OBJECT_TYPE(TMedium)

class TChunkLocation;

class TChunkTree;
class TChunkOwnerBase;

struct TChunkViewMergeResult;

class TChunkReplication;
class TChunkRequisition;
class TChunkRequisitionRegistry;

template <class T>
class TPtrWithIndex;
template <class T>
class TPtrWithIndexes;

using TNodePtrWithIndexes = TPtrWithIndexes<NNodeTrackerServer::TNode>;
using TNodePtrWithIndexesList = TCompactVector<TNodePtrWithIndexes, TypicalReplicaCount>;

using TChunkPtrWithIndexes = TPtrWithIndexes<TChunk>;
using TChunkPtrWithIndex = NChunkServer::TPtrWithIndex<TChunk>;

using TChunkReplicaIndexList = TCompactVector<int, ChunkReplicaIndexBound>;

struct TChunkTreeStatistics;
struct TAggregatedNodeStatistics;

DECLARE_REFCOUNTED_CLASS(TJobRegistry)
DECLARE_REFCOUNTED_CLASS(TJobTracker)

DECLARE_REFCOUNTED_CLASS(TJob)

DECLARE_REFCOUNTED_STRUCT(IChunkAutotomizer)

DECLARE_REFCOUNTED_CLASS(TChunkManager)
DECLARE_REFCOUNTED_CLASS(TChunkMerger)
DECLARE_REFCOUNTED_CLASS(TChunkReplicator)
DECLARE_REFCOUNTED_CLASS(TChunkPlacement)
DECLARE_REFCOUNTED_CLASS(TConsistentChunkPlacement)

DECLARE_REFCOUNTED_CLASS(TChunkManagerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicDataNodeTrackerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicChunkAutotomizerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicChunkMergerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicChunkManagerTestingConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicChunkManagerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicChunkServiceConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicAllyReplicaManagerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicConsistentReplicaPlacementConfig)
DECLARE_REFCOUNTED_CLASS(TMediumConfig)

DECLARE_REFCOUNTED_STRUCT(IChunkSealer)

DECLARE_REFCOUNTED_STRUCT(IJobController)
DECLARE_REFCOUNTED_STRUCT(ICompositeJobController)

DECLARE_REFCOUNTED_STRUCT(IDataNodeTracker)

//! Used as an expected upper bound in TCompactVector.
constexpr int TypicalChunkParentCount = 2;

//! The number of supported replication priorities.
//! The smaller the more urgent.
/*! current RF == 1 -> priority = 0
 *  current RF == 2 -> priority = 1
 *  current RF >= 3 -> priority = 2
 */
constexpr int ReplicationPriorityCount = 3;

constexpr int DefaultConsistentReplicaPlacementReplicasPerChunk = 100;

DEFINE_BIT_ENUM(EChunkStatus,
    ((None)                    (0x0000))
    ((Underreplicated)         (0x0001))
    ((Overreplicated)          (0x0002))
    ((Lost)                    (0x0004))
    ((DataMissing)             (0x0008))
    ((ParityMissing)           (0x0010))
    ((Safe)                    (0x0040))
    ((UnsafelyPlaced)          (0x0100))
    ((DataDecommissioned)      (0x0200))
    ((ParityDecommissioned)    (0x0400))
    ((SealedMissing)           (0x0800)) // Sealed chunk without sealed replicas (on certain medium).
    ((InconsistentlyPlaced)    (0x1000)) // For chunks with non-null consistent placement hash.
);

DEFINE_BIT_ENUM(ECrossMediumChunkStatus,
    ((None)              (0x0000))
    ((Sealed)            (0x0001))
    ((Lost)              (0x0004))
    ((DataMissing)       (0x0008))
    ((ParityMissing)     (0x0010))
    ((QuorumMissing)     (0x0020))
    ((Precarious)        (0x0200)) // All replicas are on transient media.
    ((MediumWiseLost)    (0x0400)) // Lost on some media, but not others.
    ((Deficient)         (0x0800)) // Underreplicated or {data,parity}-{missing,decommissioned} on some media.
);

DEFINE_BIT_ENUM(EChunkScanKind,
    ((None)              (0x0000))
    ((Refresh)           (0x0001))
    ((RequisitionUpdate) (0x0002))
    ((Seal)              (0x0004))
);

DEFINE_ENUM(EChunkListKind,
    ((Static)                 (0))
    ((SortedDynamicRoot)      (1))
    ((SortedDynamicTablet)    (2))
    ((OrderedDynamicRoot)     (3))
    ((OrderedDynamicTablet)   (4))
    ((SortedDynamicSubtablet) (5))
    ((JournalRoot)            (6))
    ((HunkRoot)               (7))
);

DEFINE_ENUM_WITH_UNDERLYING_TYPE(EChunkReplicaState, i8,
    ((Generic)               (0))
    ((Active)                (1))
    ((Unsealed)              (2))
    ((Sealed)                (3))
);

DEFINE_ENUM(EChunkLocationState,
    // Belongs to a node that is not online.
    ((Offline) (0))
    // Belongs to a node that is online and reports presence of this location.
    ((Online)  (1))
    // Belongs to a node that is online but does not report presence of this location.
    ((Dangling)(2))
);

using TChunkRepairQueue = std::list<TChunkPtrWithIndexes> ;
using TChunkRepairQueueIterator = TChunkRepairQueue::iterator;

using TFillFactorToNodeMap = std::multimap<double, NNodeTrackerServer::TNode*>;
using TFillFactorToNodeIterator = TFillFactorToNodeMap::iterator;

using TLoadFactorToNodeMap = std::multimap<double, NNodeTrackerServer::TNode*>;
using TLoadFactorToNodeIterator = TLoadFactorToNodeMap::iterator;

using TChunkExpirationMap = std::multimap<TInstant, TChunk*>;
using TChunkExpirationMapIterator = TChunkExpirationMap::iterator;

struct TChunkPartLossTimeComparer
{
    bool operator()(const TChunk* lhs, const TChunk* rhs) const;
};

using TOldestPartMissingChunkSet = std::set<TChunk*, TChunkPartLossTimeComparer>;

using TMediumSet = std::bitset<MaxMediumCount>;

constexpr int MediumDefaultPriority = 0;

using TChunkRequisitionIndex = ui32;

//! Refers to a requisition specifying that a chunk is not required by any account
//! on any medium.
constexpr TChunkRequisitionIndex EmptyChunkRequisitionIndex = 0;

//! Refers to a requisition specifying default RF on default medium under the
//! special migration account.
// NB: After we've migrated to chunk-wise accounting, that account and this
// index will be removed.
constexpr TChunkRequisitionIndex MigrationChunkRequisitionIndex = EmptyChunkRequisitionIndex + 1;

//! Refers to a requisition specifying RF of 2 on default medium under the
//! special migration account.
// NB: After we've migrated to chunk-wise accounting, that account and this
// index will be removed.
constexpr TChunkRequisitionIndex MigrationRF2ChunkRequisitionIndex = MigrationChunkRequisitionIndex + 1;

//! Refers to a requisition specifying RF of 1 on default medium under the special
//! migration account. Such requisition is suitable for erasure-coded chunks.
// NB: After we've migrated to chunk-wise accounting, that account and this
// index will be removed.
constexpr TChunkRequisitionIndex MigrationErasureChunkRequisitionIndex = MigrationRF2ChunkRequisitionIndex + 1;

constexpr i64 MaxReplicaLagLimit = Max<i64>() / 4;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
