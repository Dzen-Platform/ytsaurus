#include "data_slice_descriptor.h"
#include "chunk_spec.h"
#include "helpers.h"

namespace NYT {
namespace NChunkClient {

using namespace NTableClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

TDataSliceDescriptor::TDataSliceDescriptor(std::vector<NProto::TChunkSpec> chunkSpecs)
    : ChunkSpecs(std::move(chunkSpecs))
{ }

TDataSliceDescriptor::TDataSliceDescriptor(const NProto::TChunkSpec& chunkSpec)
{
    ChunkSpecs.push_back(chunkSpec);
}

const NProto::TChunkSpec& TDataSliceDescriptor::GetSingleChunk() const
{
    YCHECK(ChunkSpecs.size() == 1);
    return ChunkSpecs[0];
}

int TDataSliceDescriptor::GetDataSourceIndex() const
{
    return ChunkSpecs.empty()
        ? 0
        : ChunkSpecs.front().table_index();
}

////////////////////////////////////////////////////////////////////////////////

TDataSliceDescriptor CreateIncompatibleDataSliceDescriptor()
{
    // This chunk spec is incompatible with old nodes since it doesn't contain required
    // chunk_meta() field and properly set version().
    // Newer nodes do well without it.
    NProto::TChunkSpec chunkSpec;
    ToProto(chunkSpec.mutable_chunk_id(), NullChunkId);

    return TDataSliceDescriptor(chunkSpec);
}

const TDataSliceDescriptor& GetIncompatibleDataSliceDescriptor()
{
    static auto incompatibleDataSliceDescriptor = CreateIncompatibleDataSliceDescriptor();
    return incompatibleDataSliceDescriptor;
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TDataSliceDescriptor* protoDataSliceDescriptor, const TDataSliceDescriptor& dataSliceDescriptor)
{
    for (const auto& chunkSpec : dataSliceDescriptor.ChunkSpecs) {
        *protoDataSliceDescriptor->add_chunks() = chunkSpec;
    }
}

void FromProto(TDataSliceDescriptor* dataSliceDescriptor, const NProto::TDataSliceDescriptor& protoDataSliceDescriptor)
{
    dataSliceDescriptor->ChunkSpecs = std::vector<NProto::TChunkSpec>(protoDataSliceDescriptor.chunks().begin(), protoDataSliceDescriptor.chunks().end());
}

void ToProto(
    ::google::protobuf::RepeatedPtrField<NProto::TChunkSpec>* chunkSpecs,
    ::google::protobuf::RepeatedField<int>* chunkSpecCountPerDataSlice,
    const std::vector<TDataSliceDescriptor>& dataSlices)
{
    for (const auto& dataSlice : dataSlices) {
        chunkSpecCountPerDataSlice->Add(dataSlice.ChunkSpecs.size());

        for (const auto& chunkSpec : dataSlice.ChunkSpecs) {
            *chunkSpecs->Add() = chunkSpec;
        }
    }
}

void FromProto(
    std::vector<TDataSliceDescriptor>* dataSlices,
    const ::google::protobuf::RepeatedPtrField<NProto::TChunkSpec>& chunkSpecs,
    const ::google::protobuf::RepeatedField<int>& chunkSpecCountPerDataSlice)
{
    dataSlices->clear();
    int currentIndex = 0;
    for (int chunkSpecCount : chunkSpecCountPerDataSlice) {
        std::vector<NProto::TChunkSpec> dataSliceSpecs(
            chunkSpecs.begin() + currentIndex,
            chunkSpecs.begin() + currentIndex + chunkSpecCount);

        dataSlices->emplace_back(std::move(dataSliceSpecs));
        currentIndex += chunkSpecCount;
    }
}

////////////////////////////////////////////////////////////////////////////////

i64 GetCumulativeRowCount(const std::vector<TDataSliceDescriptor>& dataSliceDescriptors)
{
    i64 result = 0;
    for (const auto& dataSliceDescriptor : dataSliceDescriptors) {
        result += GetCumulativeRowCount(dataSliceDescriptor.ChunkSpecs);
    }
    return result;
}

i64 GetDataSliceDescriptorReaderMemoryEstimate(const TDataSliceDescriptor& dataSliceDescriptor, TMultiChunkReaderConfigPtr config)
{
    i64 result = 0;
    for (const auto& chunkSpec : dataSliceDescriptor.ChunkSpecs) {
        result += GetChunkReaderMemoryEstimate(chunkSpec, config);
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
