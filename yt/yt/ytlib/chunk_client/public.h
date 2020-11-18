#pragma once

#include <yt/ytlib/misc/public.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/client/chunk_client/public.h>

#include <yt/core/concurrency/async_semaphore.h>

#include <yt/core/misc/small_vector.h>
#include <yt/core/misc/optional.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

////////////////////////////////////////////////////////////////////////////////

class TReqFetch;

class TReqExportChunks;
class TRspExportChunks;

class TReqImportChunks;
class TRspImportChunks;

class TReqExecuteBatch;
class TRspExecuteBatch;

class TDataSource;
class TDataSourceDirectoryExt;

class TReqGetChunkMeta;

////////////////////////////////////////////////////////////////////////////////

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

struct TBlock;

using TMediumId = NObjectClient::TObjectId;

using TReadSessionId = NObjectClient::TObjectId;

struct TSessionId;

constexpr int DefaultPartIndex = -1;

//! Estimated memory overhead per chunk reader.
constexpr i64 ChunkReaderMemorySize = 16_KB;

constexpr int MaxMediumPriority = 10;

constexpr i64 DefaultMaxBlockSize = 16_MB;
constexpr int MaxInputChunkReplicaCount = 16;

//! Represents an offset inside a chunk.
using TBlockOffset = i64;

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

DEFINE_ENUM(EIOEngineType,
    (ThreadPool)
    (Aio)
);

//! Values must be contiguous.
DEFINE_ENUM(ESessionType,
    ((User)                     (0))
    ((Replication)              (1))
    ((Repair)                   (2))
);

DEFINE_ENUM(EUpdateMode,
    ((None)                     (0))
    ((Append)                   (1))
    ((Overwrite)                (2))
);

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TRemoteReaderOptions)
DECLARE_REFCOUNTED_CLASS(TDispatcherConfig)
DECLARE_REFCOUNTED_CLASS(TMultiChunkWriterOptions)
DECLARE_REFCOUNTED_CLASS(TMultiChunkReaderOptions)
DECLARE_REFCOUNTED_CLASS(TRemoteWriterOptions)
DECLARE_REFCOUNTED_CLASS(TBlockCacheConfig)
DECLARE_REFCOUNTED_CLASS(TChunkScraperConfig)
DECLARE_REFCOUNTED_CLASS(TChunkTeleporterConfig)
DECLARE_REFCOUNTED_CLASS(TMediumDirectorySynchronizerConfig)

DECLARE_REFCOUNTED_STRUCT(IFetcherChunkScraper)

DECLARE_REFCOUNTED_CLASS(TEncodingWriter)
DECLARE_REFCOUNTED_CLASS(TEncodingChunkWriter)
DECLARE_REFCOUNTED_CLASS(TBlockFetcher)
DECLARE_REFCOUNTED_CLASS(TSequentialBlockFetcher)

DECLARE_REFCOUNTED_STRUCT(IChunkReader)
DECLARE_REFCOUNTED_STRUCT(IChunkWriter)

DECLARE_REFCOUNTED_STRUCT(IChunkReaderAllowingRepair)
DECLARE_REFCOUNTED_STRUCT(IRemoteChunkReader)

DECLARE_REFCOUNTED_STRUCT(IReaderBase)
DECLARE_REFCOUNTED_STRUCT(IReaderFactory)

DECLARE_REFCOUNTED_STRUCT(IMultiReaderManager)

DECLARE_REFCOUNTED_CLASS(TTrafficMeter)

DECLARE_REFCOUNTED_STRUCT(IChunkWriterBase)
DECLARE_REFCOUNTED_STRUCT(IMultiChunkWriter)

DECLARE_REFCOUNTED_STRUCT(IBlockCache)

DECLARE_REFCOUNTED_STRUCT(IIOEngine)

DECLARE_REFCOUNTED_CLASS(TFileReader)
DECLARE_REFCOUNTED_CLASS(TFileWriter)

DECLARE_REFCOUNTED_CLASS(TMemoryWriter)

DECLARE_REFCOUNTED_CLASS(TInputChunk)
DECLARE_REFCOUNTED_CLASS(TInputChunkSlice)

DECLARE_REFCOUNTED_STRUCT(TLegacyDataSlice)

DECLARE_REFCOUNTED_CLASS(TDataSourceDirectory)

DECLARE_REFCOUNTED_CLASS(TChunkScraper)
DECLARE_REFCOUNTED_CLASS(TScraperTask)
DECLARE_REFCOUNTED_CLASS(TThrottlerManager)
DECLARE_REFCOUNTED_CLASS(TChunkTeleporter)
DECLARE_REFCOUNTED_CLASS(TMediumDirectory)
DECLARE_REFCOUNTED_CLASS(TMediumDirectorySynchronizer)

DECLARE_REFCOUNTED_CLASS(TChunkMetaFetcher)

DECLARE_REFCOUNTED_CLASS(TChunkSpecFetcher)

DECLARE_REFCOUNTED_STRUCT(TChunkReaderStatistics)

DECLARE_REFCOUNTED_CLASS(IReaderMemoryManager)
DECLARE_REFCOUNTED_CLASS(TChunkReaderMemoryManager)

DECLARE_REFCOUNTED_CLASS(TChunkReplicaLocator)

struct TChunkReaderMemoryManagerOptions;

DECLARE_REFCOUNTED_STRUCT(TMemoryManagedData)

class TReadLimit;
class TReadRange;

struct TUserObject;

using TRefCountedChunkMeta = TRefCountedProto<NChunkClient::NProto::TChunkMeta>;
DECLARE_REFCOUNTED_TYPE(TRefCountedChunkMeta)

DECLARE_REFCOUNTED_CLASS(TDeferredChunkMeta)

// NB: TRefCountedBlocksExt needs weak pointers support.
using TRefCountedBlocksExt = TRefCountedProto<NChunkClient::NProto::TBlocksExt>;
DECLARE_REFCOUNTED_TYPE(TRefCountedBlocksExt)

using TRefCountedMiscExt = TRefCountedProto<NChunkClient::NProto::TMiscExt>;
DECLARE_REFCOUNTED_TYPE(TRefCountedMiscExt);

using TPlacementId = TGuid;

struct TDataSliceDescriptor;

struct TInterruptDescriptor;

class TCodecStatistics;

struct TClientBlockReadOptions;

DECLARE_REFCOUNTED_CLASS(TKeySetWriter)

using TDataCenterName = std::optional<TString>;

struct IBlocksExtCache;

DECLARE_REFCOUNTED_STRUCT(TMemoryUsageGuard)

DECLARE_REFCOUNTED_STRUCT(IMultiReaderMemoryManager)
DECLARE_REFCOUNTED_STRUCT(IReaderMemoryManagerHost)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
