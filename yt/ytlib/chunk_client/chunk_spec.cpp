#include "chunk_spec.h"
#include "chunk_meta_extensions.h"
#include "chunk_replica.h"
#include "read_limit.h"

#include <yt/core/erasure/codec.h>

#include <yt/core/misc/protobuf_helpers.h>

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////////////////

bool IsUnavailable(const TChunkReplicaList& replicas, NErasure::ECodec codecId, bool checkParityParts)
{
    if (codecId == NErasure::ECodec::None) {
        return replicas.empty();
    } else {
        auto* codec = NErasure::GetCodec(codecId);
        int partCount = checkParityParts ? codec->GetTotalPartCount() : codec->GetDataPartCount();
        NErasure::TPartIndexSet missingIndexSet((1 << partCount) - 1);
        for (auto replica : replicas) {
            missingIndexSet.reset(replica.GetIndex());
        }
        return missingIndexSet.any();
    }
}

bool IsUnavailable(const NProto::TChunkSpec& chunkSpec, bool checkParityParts)
{
    auto codecId = NErasure::ECodec(chunkSpec.erasure_codec());
    auto replicas = NYT::FromProto<TChunkReplicaList>(chunkSpec.replicas());
    return IsUnavailable(replicas, codecId, checkParityParts);
}

void GetStatistics(
    const TChunkSpec& chunkSpec,
    i64* dataSize,
    i64* rowCount,
    i64* valueCount,
    i64* compressedDataSize)
{
    auto miscExt = GetProtoExtension<TMiscExt>(chunkSpec.chunk_meta().extensions());
    auto sizeOverrideExt = FindProtoExtension<TSizeOverrideExt>(chunkSpec.chunk_meta().extensions());

    if (sizeOverrideExt) {
        if (dataSize) {
            *dataSize = sizeOverrideExt->uncompressed_data_size();
        }
        if (rowCount) {
            *rowCount = sizeOverrideExt->row_count();
        }
    } else {
        if (dataSize) {
            *dataSize = miscExt.uncompressed_data_size();
        }
        if (rowCount) {
            *rowCount = miscExt.row_count();
        }
    }

    if (valueCount) {
        *valueCount = miscExt.value_count();
    }
    if (compressedDataSize) {
        *compressedDataSize = miscExt.compressed_data_size();
    }
}

i64 GetCumulativeRowCount(const std::vector<NProto::TChunkSpec>& chunkSpecs)
{
    i64 result = 0;
    for (const auto& chunkSpec : chunkSpecs) {
        auto miscExt = FindProtoExtension<TMiscExt>(chunkSpec.chunk_meta().extensions());
        if (!miscExt) {
            return std::numeric_limits<i64>::max();
        }

        i64 upperRowLimit = miscExt->row_count();
        i64 lowerRowLimit = 0;
        if (chunkSpec.has_lower_limit() && chunkSpec.lower_limit().has_row_index()) {
            lowerRowLimit = chunkSpec.lower_limit().row_index();
        }

        if (chunkSpec.has_upper_limit() && chunkSpec.upper_limit().has_row_index()) {
            upperRowLimit = chunkSpec.upper_limit().row_index();
        }

        result += upperRowLimit - lowerRowLimit;
    }
    return result;
}

TChunkId EncodeChunkId(
    const TChunkSpec& chunkSpec,
    NNodeTrackerClient::TNodeId nodeId)
{
    auto replicas = NYT::FromProto<TChunkReplicaList>(chunkSpec.replicas());
    auto replicaIt = std::find_if(
        replicas.begin(),
        replicas.end(),
        [=] (TChunkReplica replica) {
            return replica.GetNodeId() == nodeId;
        });
    YCHECK(replicaIt != replicas.end());

    TChunkIdWithIndex chunkIdWithIndex(
        NYT::FromProto<TChunkId>(chunkSpec.chunk_id()),
        replicaIt->GetIndex());
    return EncodeChunkId(chunkIdWithIndex);
}

//! Returns |false| iff the chunk has nontrivial limits.
bool IsCompleteChunk(const NProto::TChunkSpec& chunkSpec)
{
    return (!chunkSpec.has_lower_limit() || IsTrivial(chunkSpec.lower_limit()))
        && (!chunkSpec.has_upper_limit() || IsTrivial(chunkSpec.upper_limit()));
}

//! Returns |true| iff the chunk is complete and is large enough.
bool IsLargeCompleteChunk(const NProto::TChunkSpec& chunkSpec, i64 desiredChunkSize)
{
    if (!IsCompleteChunk(chunkSpec)) {
        return false;
    }

    auto miscExt = GetProtoExtension<TMiscExt>(chunkSpec.chunk_meta().extensions());

    // ChunkSequenceWriter may actually produce a chunk a bit smaller than desiredChunkSize,
    // so we have to be more flexible here.
    return 0.9 * miscExt.compressed_data_size() >= desiredChunkSize;
}

Stroka ToString(TRefCountedChunkSpecPtr spec)
{
    auto chunkLowerLimit = NYT::FromProto<NChunkClient::TReadLimit>(spec->lower_limit());
    auto chunkUpperLimit = NYT::FromProto<NChunkClient::TReadLimit>(spec->upper_limit());
    auto chunkId = NYT::FromProto<TChunkId>(spec->chunk_id());
    return Format(
        "ChunkId: %v, LowerLimit: {%v}, UpperLimit: {%v}",
        chunkId,
        ToString(chunkLowerLimit),
        ToString(chunkUpperLimit));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
