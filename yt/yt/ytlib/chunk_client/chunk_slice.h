#pragma once

#include "public.h"

#include <yt/yt/ytlib/chunk_client/proto/chunk_slice.pb.h>
#include <yt/yt/ytlib/chunk_client/proto/data_node_service.pb.h>
#include <yt/yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/yt/client/chunk_client/read_limit.h>

#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/core/misc/optional.h>
#include <yt/yt/core/misc/phoenix.h>

#include <library/cpp/yt/memory/new.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

// ToDo(psushin): move to NTableClient.

struct TChunkSlice
{
    TReadLimit LowerLimit;
    TReadLimit UpperLimit;

    i64 DataWeight;
    i64 RowCount;
};

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TChunkSlice& slice);

////////////////////////////////////////////////////////////////////////////////

std::vector<TChunkSlice> SliceChunk(
    const NProto::TSliceRequest& sliceReq,
    const NProto::TChunkMeta& meta);

void ToProto(
    const TKeySetWriterPtr& keysWriter,
    const TKeySetWriterPtr& keyBoundsWriter,
    NProto::TChunkSlice* protoChunkSlice,
    const TChunkSlice& chunkSlice);

////////////////////////////////////////////////////////////////////////////////

i64 GetChunkSliceDataWeight(
    const NProto::TReqGetChunkSliceDataWeights::TChunkSlice& weightedChunkRequest,
    const NProto::TChunkMeta& meta);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient

