#pragma once

#include <yt/ytlib/misc/public.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/core/misc/public.h>
#include <yt/core/misc/small_vector.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TChunkInfo;
class TChunkSpec;
class TChunkMeta;
class TMiscExt;

class TReadRange;

class TReqFetch;

class TDataStatistics;

class TReqExportChunks;
class TRspExportChunks;

class TReqImportChunks;
class TRspImportChunks;

class TReqExecuteBatch;
class TRspExecuteBatch;

} // namespace NProto

///////////////////////////////////////////////////////////////////////////////

typedef NObjectClient::TObjectId TChunkId;
extern const TChunkId NullChunkId;

typedef NObjectClient::TObjectId TChunkListId;
extern const TChunkListId NullChunkListId;

typedef NObjectClient::TObjectId TChunkTreeId;
extern const TChunkTreeId NullChunkTreeId;

const int MinReplicationFactor = 1;
const int MaxReplicationFactor = 10;

//! Estimated memory overhead per chunk reader.
const i64 ChunkReaderMemorySize = (i64) 16 * 1024;

//! Used as an expected upper bound in SmallVector.
/*
 *  Maximum regular number of replicas is 16 (for LRC codec).
 *  Additional +8 enables some flexibility during balancing.
 */
const int TypicalReplicaCount = 24;

class TChunkReplica;
typedef SmallVector<TChunkReplica, TypicalReplicaCount> TChunkReplicaList;

//! Represents an offset inside a chunk.
typedef i64 TBlockOffset;

//! A |(chunkId, blockIndex)| pair.
struct TBlockId;

DEFINE_BIT_ENUM(EBlockType,
    ((None)              (0x0000))
    ((CompressedData)    (0x0001))
    ((UncompressedData)  (0x0002))
);

DEFINE_ENUM(EChunkType,
    ((Unknown) (0))
    ((File)    (1))
    ((Table)   (2))
    ((Journal) (3))
);

DEFINE_ENUM(EErrorCode,
    ((AllTargetNodesFailed)     (700))
    ((SendBlocksFailed)         (701))
    ((NoSuchSession)            (702))
    ((SessionAlreadyExists)     (703))
    ((ChunkAlreadyExists)       (704))
    ((WindowError)              (705))
    ((BlockContentMismatch)     (706))
    ((NoSuchBlock)              (707))
    ((NoSuchChunk)              (708))
    ((NoLocationAvailable)      (710))
    ((IOError)                  (711))
    ((MasterCommunicationFailed)(712))
    ((NoSuchChunkTree)          (713))
    ((NoSuchChunkList)          (717))
    ((MasterNotConnected)       (714))
    ((ChunkCreationFailed)      (715))
    ((ChunkUnavailable)         (716))
    ((WriteThrottlingActive)    (718))
);

////////////////////////////////////////////////////////////////////////////////

//! Values must be contiguous.
DEFINE_ENUM(ESessionType,
    ((User)                     (0))
    ((Replication)              (1))
    ((Repair)                   (2))
);

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TReplicationReaderConfig)
DECLARE_REFCOUNTED_CLASS(TRemoteReaderOptions)
DECLARE_REFCOUNTED_CLASS(TEncodingWriterOptions)
DECLARE_REFCOUNTED_CLASS(TDispatcherConfig)
DECLARE_REFCOUNTED_CLASS(TMultiChunkWriterConfig)
DECLARE_REFCOUNTED_CLASS(TMultiChunkWriterOptions)
DECLARE_REFCOUNTED_CLASS(TMultiChunkReaderConfig)
DECLARE_REFCOUNTED_CLASS(TMultiChunkReaderOptions)
DECLARE_REFCOUNTED_CLASS(TBlockFetcherConfig)
DECLARE_REFCOUNTED_CLASS(TReplicationWriterConfig)
DECLARE_REFCOUNTED_CLASS(TRemoteWriterOptions)
DECLARE_REFCOUNTED_CLASS(TErasureWriterConfig)
DECLARE_REFCOUNTED_CLASS(TEncodingWriterConfig)
DECLARE_REFCOUNTED_CLASS(TFetcherConfig)
DECLARE_REFCOUNTED_CLASS(TBlockCacheConfig)
DECLARE_REFCOUNTED_CLASS(TChunkScraperConfig)
DECLARE_REFCOUNTED_CLASS(TChunkTeleporterConfig)

DECLARE_REFCOUNTED_CLASS(TEncodingWriter)
DECLARE_REFCOUNTED_CLASS(TEncodingChunkWriter)
DECLARE_REFCOUNTED_CLASS(TBlockFetcher)
DECLARE_REFCOUNTED_CLASS(TSequentialBlockFetcher)

DECLARE_REFCOUNTED_STRUCT(IChunkReader)
DECLARE_REFCOUNTED_STRUCT(IChunkWriter)

DECLARE_REFCOUNTED_STRUCT(IReaderBase)
DECLARE_REFCOUNTED_STRUCT(IReaderFactory)

DECLARE_REFCOUNTED_STRUCT(IChunkWriterBase)
DECLARE_REFCOUNTED_STRUCT(IMultiChunkWriter)

DECLARE_REFCOUNTED_STRUCT(IBlockCache)

DECLARE_REFCOUNTED_CLASS(TFileReader)
DECLARE_REFCOUNTED_CLASS(TFileWriter)

DECLARE_REFCOUNTED_CLASS(TMemoryWriter)

using TRefCountedChunkSpec = TRefCountedProto<NProto::TChunkSpec>;
DECLARE_REFCOUNTED_TYPE(TRefCountedChunkSpec)
DECLARE_REFCOUNTED_CLASS(TChunkSlice)

DECLARE_REFCOUNTED_CLASS(TChunkScraper)
DECLARE_REFCOUNTED_CLASS(TScraperTask)
DECLARE_REFCOUNTED_CLASS(TThrottlerManager)
DECLARE_REFCOUNTED_CLASS(TChunkTeleporter)

class TReadLimit;

class TChannel;
typedef std::vector<TChannel> TChannels;

using TRefCountedChunkMeta = TRefCountedProto<NChunkClient::NProto::TChunkMeta>;
DECLARE_REFCOUNTED_TYPE(TRefCountedChunkMeta)

using TRefCountedChunkSpec = TRefCountedProto<NChunkClient::NProto::TChunkSpec>;
DECLARE_REFCOUNTED_TYPE(TRefCountedChunkSpec)

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

