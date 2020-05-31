#pragma once

#include <yt/core/misc/public.h>
#include <yt/core/misc/small_vector.h>
#include <yt/core/misc/dense_map.h>

#include <yt/client/object_client/public.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TChunkInfo;
class TChunkSpec;
class TChunkMeta;
class TBlocksExt;
class TMiscExt;

class TDataStatistics;

class TReadRange;

class TMediumDirectory;

} // namespace NProto

DEFINE_ENUM(EErrorCode,
    ((AllTargetNodesFailed)                  (700))
    ((SendBlocksFailed)                      (701))
    ((NoSuchSession)                         (702))
    ((SessionAlreadyExists)                  (703))
    ((ChunkAlreadyExists)                    (704))
    ((WindowError)                           (705))
    ((BlockContentMismatch)                  (706))
    ((NoSuchBlock)                           (707))
    ((NoSuchChunk)                           (708))
    ((NoLocationAvailable)                   (710))
    ((IOError)                               (711))
    ((MasterCommunicationFailed)             (712))
    ((NoSuchChunkTree)                       (713))
    ((NoSuchChunkList)                       (717))
    ((MasterNotConnected)                    (714))
    ((ChunkUnavailable)                      (716))
    ((WriteThrottlingActive)                 (718))
    ((NoSuchMedium)                          (719))
    ((OptimisticLockFailure)                 (720))
    ((InvalidBlockChecksum)                  (721))
    ((BlockOutOfRange)                       (722))
    ((MissingExtension)                      (724))
    ((BandwidthThrottlingFailed)             (725))
    ((ReaderTimeout)                         (726))
    ((NoSuchChunkView)                       (727))
    ((IncorrectChunkFileChecksum)            (728))
    ((IncorrectChunkFileHeaderSignature)     (729))
    ((IncorrectLayerFileSize)                (730))
    ((NoSpaceLeftOnDevice)                   (731))
    ((ConcurrentChunkUpdate)                 (732))
);

using TChunkId = NObjectClient::TObjectId;
extern const TChunkId NullChunkId;

using TChunkViewId = NObjectClient::TObjectId;
extern const TChunkViewId NullChunkViewId;

using TChunkListId = NObjectClient::TObjectId;
extern const TChunkListId NullChunkListId;

using TChunkTreeId = NObjectClient::TObjectId;
extern const TChunkTreeId NullChunkTreeId;

using TLocationUuid = TGuid;

constexpr int MinReplicationFactor = 1;
constexpr int MaxReplicationFactor = 20;
constexpr int DefaultReplicationFactor = 3;

constexpr int MaxMediumCount = 120; // leave some room for sentinels

template <typename T>
using TMediumMap = SmallDenseMap<int, T>;
using TMediumIntMap = TMediumMap<int>;

//! Used as an expected upper bound in SmallVector.
/*
 *  Maximum regular number of replicas is 16 (for LRC codec).
 *  Additional +8 enables some flexibility during balancing.
 */
constexpr int TypicalReplicaCount = 24;
constexpr int GenericChunkReplicaIndex = 16;  // no specific replica; the default one for non-erasure chunks

//! Valid indexes are in range |[0, ChunkReplicaIndexBound)|.
constexpr int ChunkReplicaIndexBound = 32;

constexpr int GenericMediumIndex      = 126; // internal sentinel meaning "no specific medium"
constexpr int AllMediaIndex           = 127; // passed to various APIs to indicate that any medium is OK
constexpr int DefaultStoreMediumIndex =   0;
constexpr int DefaultCacheMediumIndex =   1;
constexpr int DefaultSlotsMediumIndex =   0;

//! Valid indexes (including sentinels) are in range |[0, MediumIndexBound)|.
constexpr int MediumIndexBound = AllMediaIndex + 1;

class TChunkReplicaWithMedium;
using TChunkReplicaWithMediumList = SmallVector<TChunkReplicaWithMedium, TypicalReplicaCount>;

class TChunkReplica;
using TChunkReplicaList = SmallVector<TChunkReplica, TypicalReplicaCount>;

extern const TString DefaultStoreAccountName;
extern const TString DefaultStoreMediumName;
extern const TString DefaultCacheMediumName;
extern const TString DefaultSlotsMediumName;

DECLARE_REFCOUNTED_STRUCT(IReaderBase)

DECLARE_REFCOUNTED_CLASS(TFetchChunkSpecConfig)
DECLARE_REFCOUNTED_CLASS(TFetcherConfig)
DECLARE_REFCOUNTED_CLASS(TEncodingWriterConfig)
DECLARE_REFCOUNTED_CLASS(TErasureReaderConfig)
DECLARE_REFCOUNTED_CLASS(TMultiChunkReaderConfig)
DECLARE_REFCOUNTED_CLASS(TBlockFetcherConfig)
DECLARE_REFCOUNTED_CLASS(TReplicationReaderConfig)
DECLARE_REFCOUNTED_CLASS(TReplicationWriterConfig)
DECLARE_REFCOUNTED_CLASS(TErasureWriterConfig)
DECLARE_REFCOUNTED_CLASS(TMultiChunkWriterConfig)
DECLARE_REFCOUNTED_CLASS(TEncodingWriterOptions)

struct TCodecDuration;
class TCodecStatistics;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
