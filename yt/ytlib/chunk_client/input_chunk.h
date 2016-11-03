#pragma once

#include "public.h"
#include "read_limit.h"
#include "chunk_replica.h"
#include "chunk_spec.h"

#include <yt/ytlib/chunk_client/chunk_meta.pb.h>
#include <yt/ytlib/chunk_client/chunk_spec.pb.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/core/erasure/public.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/protobuf_helpers.h>

#include <array>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

const i64 DefaultMaxBlockSize = (i64) 16 * 1024 * 1024;
const int InputChunkReplicaCount = 16;

////////////////////////////////////////////////////////////////////////////////

//! Compact representation of some fields from NProto::TChunkSpec.
//! Used inside scheduler to reduce memory footprint.
//! The content of TInputChunkBase is stored in a scheduler snapshot as a POD.
class TInputChunkBase
{
    DEFINE_BYREF_RO_PROPERTY(TChunkId, ChunkId);

    typedef std::array<TChunkReplica, InputChunkReplicaCount> TInputChunkReplicas;
    DEFINE_BYREF_RO_PROPERTY(TInputChunkReplicas, Replicas);

    DEFINE_BYVAL_RW_PROPERTY(int, TableIndex, -1);
    DEFINE_BYVAL_RO_PROPERTY(NErasure::ECodec, ErasureCodec, NErasure::ECodec::None);
    DEFINE_BYVAL_RO_PROPERTY(i64, TableRowIndex);
    DEFINE_BYVAL_RO_PROPERTY(int, RangeIndex);
    DEFINE_BYVAL_RO_PROPERTY(NTableClient::ETableChunkFormat, TableChunkFormat);

    DEFINE_BYVAL_RO_PROPERTY(i64, UncompressedDataSize);
    DEFINE_BYVAL_RO_PROPERTY(i64, RowCount);
    DEFINE_BYVAL_RO_PROPERTY(i64, CompressedDataSize); // for TSortControllerBase
    DEFINE_BYVAL_RO_PROPERTY(i64, MaxBlockSize); // for TChunkStripeStatistics

    DEFINE_BYVAL_RO_PROPERTY(bool, UniqueKeys, false); // for TChunkStripeStatistics

public:
    TInputChunkBase() = default;
    TInputChunkBase(TInputChunkBase&& other) = default;
    explicit TInputChunkBase(const NProto::TChunkSpec& chunkSpec);

    TChunkReplicaList GetReplicaList() const;
    void SetReplicaList(const TChunkReplicaList& replicas);

private:
    void CheckOffsets();

};

////////////////////////////////////////////////////////////////////////////////

//! Compact representation of NProto::TChunkSpec.
//! Used inside scheduler to reduce memory footprint.
class TInputChunk
    : public TIntrinsicRefCounted
    , public TInputChunkBase
{
    // Here are read limits.
    typedef std::unique_ptr<TReadLimit> TReadLimitHolder;
    DEFINE_BYREF_RO_PROPERTY(TReadLimitHolder, LowerLimit);
    DEFINE_BYREF_RO_PROPERTY(TReadLimitHolder, UpperLimit);

    // Here are boundary keys.
    typedef std::unique_ptr<NTableClient::TBoundaryKeys> TInputChunkBoundaryKeys;
    DEFINE_BYREF_RO_PROPERTY(TInputChunkBoundaryKeys, BoundaryKeys);

    // These fields are not used directly by scheduler.
    typedef std::unique_ptr<NChunkClient::NProto::TChannel> TInputChunkChannel;
    DEFINE_BYREF_RO_PROPERTY(TInputChunkChannel, Channel);
    typedef std::unique_ptr<NTableClient::NProto::TPartitionsExt> TInputChunkPartitionsExt;
    DEFINE_BYREF_RO_PROPERTY(TInputChunkPartitionsExt, PartitionsExt);

public:
    TInputChunk() = default;
    TInputChunk(TInputChunk&& other) = default;
    explicit TInputChunk(const NProto::TChunkSpec& chunkSpec);

    void Persist(const TStreamPersistenceContext& context);

    size_t SpaceUsed() const;

    //! Returns |false| iff the chunk has nontrivial limits.
    bool IsCompleteChunk() const;
    //! Returns |true| iff the chunk is complete and is large enough.
    bool IsLargeCompleteChunk(i64 desiredChunkSize) const;

    //! Releases memory occupied by BoundaryKeys
    void ReleaseBoundaryKeys();
    //! Releases memory occupied by PartitionsExt
    void ReleasePartitionsExt();

    friend void ToProto(NProto::TChunkSpec* chunkSpec, const TInputChunkPtr& inputChunk);
};

DEFINE_REFCOUNTED_TYPE(TInputChunk)

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TChunkSpec* chunkSpec, const TInputChunkPtr& inputChunk);
Stroka ToString(const TInputChunkPtr& inputChunk);

////////////////////////////////////////////////////////////////////////////////

bool IsUnavailable(const TInputChunkPtr& inputChunk, bool checkParityParts = false);
TChunkId EncodeChunkId(const TInputChunkPtr& inputChunk, NNodeTrackerClient::TNodeId nodeId);

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

Y_DECLARE_PODTYPE(NYT::NChunkClient::TInputChunkBase);
