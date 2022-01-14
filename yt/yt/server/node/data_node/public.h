#pragma once

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/core/misc/public.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

using NChunkClient::TChunkId;
using NChunkClient::TSessionId;
using NChunkClient::ESessionType;
using NChunkClient::TBlockId;
using NChunkClient::TLocationUuid;

using NNodeTrackerClient::TNodeId;

////////////////////////////////////////////////////////////////////////////////

struct IBootstrap;

struct TChunkDescriptor;
struct TSessionOptions;
struct TChunkReadOptions;

class TPendingIOGuard;
class TChunkReadGuard;

struct TArtifactKey;

class TNetworkStatistics;

DECLARE_REFCOUNTED_STRUCT(IMasterConnector)

DECLARE_REFCOUNTED_CLASS(TLegacyMasterConnector)

DECLARE_REFCOUNTED_STRUCT(IChunkStoreHost)
DECLARE_REFCOUNTED_CLASS(TChunkStore)

DECLARE_REFCOUNTED_STRUCT(IAllyReplicaManager)
DECLARE_REFCOUNTED_STRUCT(IChunkRegistry)
DECLARE_REFCOUNTED_STRUCT(IChunkBlockManager)
DECLARE_REFCOUNTED_CLASS(TChunkReaderSweeper)
DECLARE_REFCOUNTED_STRUCT(IBlobReaderCache)
DECLARE_REFCOUNTED_STRUCT(IJournalDispatcher)

DECLARE_REFCOUNTED_CLASS(TCachedChunkMeta)
DECLARE_REFCOUNTED_CLASS(TCachedBlocksExt)
DECLARE_REFCOUNTED_STRUCT(IChunkMetaManager)

DECLARE_REFCOUNTED_CLASS(TLocation)
DECLARE_REFCOUNTED_CLASS(TStoreLocation)
DECLARE_REFCOUNTED_CLASS(TCacheLocation)
DECLARE_REFCOUNTED_CLASS(TJournalManager)
DECLARE_REFCOUNTED_STRUCT(TLocationPerformanceCounters)

DECLARE_REFCOUNTED_STRUCT(TChunkHost)
DECLARE_REFCOUNTED_STRUCT(IChunk)
DECLARE_REFCOUNTED_CLASS(TBlobChunkBase)
DECLARE_REFCOUNTED_CLASS(TStoredBlobChunk)
DECLARE_REFCOUNTED_CLASS(TCachedBlobChunk)
DECLARE_REFCOUNTED_CLASS(TJournalChunk)

DECLARE_REFCOUNTED_STRUCT(ISession)
DECLARE_REFCOUNTED_CLASS(TBlobWritePipeline)
DECLARE_REFCOUNTED_CLASS(TBlobSession)
DECLARE_REFCOUNTED_CLASS(TSessionManager)

DECLARE_REFCOUNTED_CLASS(TStoreLocationConfigBase)
DECLARE_REFCOUNTED_CLASS(TStoreLocationConfig)
DECLARE_REFCOUNTED_CLASS(TCacheLocationConfig)
DECLARE_REFCOUNTED_CLASS(TMultiplexedChangelogConfig)
DECLARE_REFCOUNTED_CLASS(TArtifactCacheReaderConfig)
DECLARE_REFCOUNTED_CLASS(TRepairReaderConfig)
DECLARE_REFCOUNTED_CLASS(TMediumUpdaterDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TSealReaderConfig)
DECLARE_REFCOUNTED_CLASS(TMasterConnectorConfig)
DECLARE_REFCOUNTED_CLASS(TMasterConnectorDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TAllyReplicaManagerDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TDataNodeConfig)
DECLARE_REFCOUNTED_CLASS(TDataNodeDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TLayerLocationConfig)
DECLARE_REFCOUNTED_CLASS(TTmpfsLayerCacheConfig)
DECLARE_REFCOUNTED_CLASS(TVolumeManagerConfig)
DECLARE_REFCOUNTED_CLASS(TTableSchemaCacheConfig)
DECLARE_REFCOUNTED_CLASS(TTableSchemaCacheDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TRepairReaderConfig)
DECLARE_REFCOUNTED_CLASS(TChunkAutotomizerConfig)

DECLARE_REFCOUNTED_CLASS(TTableSchemaCache)
DECLARE_REFCOUNTED_CLASS(TCachedTableSchemaWrapper)
DECLARE_REFCOUNTED_CLASS(TLookupSession)

DECLARE_REFCOUNTED_CLASS(TMediumDirectoryManager)
DECLARE_REFCOUNTED_CLASS(TMediumUpdater)

DECLARE_REFCOUNTED_CLASS(TP2PBlockCache)
DECLARE_REFCOUNTED_CLASS(TP2PSnooper)
DECLARE_REFCOUNTED_CLASS(TP2PDistributor)
DECLARE_REFCOUNTED_CLASS(TP2PConfig)
DECLARE_REFCOUNTED_STRUCT(TP2PChunkPeer)
DECLARE_REFCOUNTED_STRUCT(TP2PBlock)
DECLARE_REFCOUNTED_STRUCT(TP2PChunk)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EErrorCode,
    ((LocalChunkReaderFailed)    (1300))
    ((LayerUnpackingFailed)      (1301))
);

DEFINE_ENUM(EDataNodeThrottlerKind,
    //! Controls the total incoming bandwidth.
    (TotalIn)
    //! Controls the total outcoming bandwidth.
    (TotalOut)
    //! Controls incoming bandwidth used by replication jobs.
    (ReplicationIn)
    //! Controls outcoming bandwidth used by replication jobs.
    (ReplicationOut)
    //! Controls incoming bandwidth used by repair jobs.
    (RepairIn)
    //! Controls outcoming bandwidth used by repair jobs.
    (RepairOut)
    //! Controls incoming bandwidth used by merge jobs.
    (MergeIn)
    //! Controls outcoming bandwidth used by merge jobs.
    (MergeOut)
    //! Controls incoming bandwidth used by autotomy jobs.
    (AutotomyIn)
    //! Controls outcoming bandwidth used by autotomy jobs.
    (AutotomyOut)
    //! Controls incoming bandwidth used by Artifact Cache downloads.
    (ArtifactCacheIn)
    //! Controls outcoming bandwidth used by Artifact Cache downloads.
    (ArtifactCacheOut)
    //! Controls outcoming location bandwidth used by Skynet sharing.
    (SkynetOut)
    //! Controls incoming location bandwidth used by tablet compaction and partitioning.
    (TabletCompactionAndPartitioningIn)
    //! Controls outcoming location bandwidth used by tablet compaction and partitioning.
    (TabletCompactionAndPartitioningOut)
    //! Controls incoming location bandwidth used by tablet journals.
    (TabletLoggingIn)
    //! Controls outcoming location bandwidth used by tablet preload.
    (TabletPreloadOut)
    //! Controls outcoming location bandwidth used by tablet recovery.
    (TabletRecoveryOut)
    //! Controls incoming location bandwidth used by tablet snapshots.
    (TabletSnapshotIn)
    //! Controls incoming location bandwidth used by tablet store flush.
    (TabletStoreFlushIn)
    //! Controls outcoming location bandwidth used by tablet replication.
    (TabletReplicationOut)
    //! Controls incoming bandwidth consumed by local jobs.
    (JobIn)
    //! Controls outcoming bandwidth consumed by local jobs.
    (JobOut)
    //! Controls outcoming bandwidth consumed by P2P block distribution.
    (P2POut)
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
