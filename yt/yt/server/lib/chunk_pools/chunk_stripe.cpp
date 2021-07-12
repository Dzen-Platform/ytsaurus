#include "chunk_stripe.h"

#include <yt/yt/ytlib/chunk_client/input_chunk_slice.h>
#include <yt/yt/ytlib/chunk_client/legacy_data_slice.h>
#include <yt/yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/chunk_client/input_chunk.h>

#include <yt/yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/yt/client/object_client/helpers.h>

namespace NYT::NChunkPools {

using namespace NTableClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////////////////

void TChunkStripeStatistics::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, ChunkCount);
    Persist(context, DataWeight);
    Persist(context, RowCount);
    Persist(context, MaxBlockSize);
}

////////////////////////////////////////////////////////////////////////////////

TChunkStripe::TChunkStripe(bool foreign, bool solid)
    : Foreign(foreign)
    , Solid(solid)
{ }

TChunkStripe::TChunkStripe(TLegacyDataSlicePtr dataSlice, bool foreign, bool solid)
    : Foreign(foreign)
    , Solid(solid)
{
    DataSlices.emplace_back(std::move(dataSlice));
}

TChunkStripe::TChunkStripe(const std::vector<TLegacyDataSlicePtr>& dataSlices)
{
    DataSlices.insert(DataSlices.end(), dataSlices.begin(), dataSlices.end());
}

TChunkStripe::TChunkStripe(TChunkListId chunkListId, TBoundaryKeys boundaryKeys)
    : ChunkListId(chunkListId)
    , BoundaryKeys(boundaryKeys)
{ }

TChunkStripeStatistics TChunkStripe::GetStatistics() const
{
    TChunkStripeStatistics result;

    for (const auto& dataSlice : DataSlices) {
        result.DataWeight += dataSlice->GetDataWeight();
        result.RowCount += dataSlice->GetRowCount();
        result.ChunkCount += dataSlice->GetChunkCount();
        result.MaxBlockSize = std::max(result.MaxBlockSize, dataSlice->GetMaxBlockSize());
    }

    return result;
}

int TChunkStripe::GetChunkCount() const
{
    int result = 0;
    for (const auto& dataSlice : DataSlices) {
        result += dataSlice->GetChunkCount();
    }
    return result;
}

int TChunkStripe::GetTableIndex() const
{
    YT_VERIFY(!DataSlices.empty());
    YT_VERIFY(!DataSlices.front()->ChunkSlices.empty());
    return DataSlices.front()->ChunkSlices.front()->GetInputChunk()->GetTableIndex();
}

int TChunkStripe::GetInputStreamIndex() const
{
    YT_VERIFY(!DataSlices.empty());
    return DataSlices.front()->InputStreamIndex;
}

void TChunkStripe::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, DataSlices);
    Persist(context, WaitingChunkCount);
    Persist(context, Foreign);
    Persist(context, Solid);
    Persist(context, ChunkListId);
    Persist(context, BoundaryKeys);
    Persist(context, PartitionTag);
}

TChunkStripeStatistics operator + (
    const TChunkStripeStatistics& lhs,
    const TChunkStripeStatistics& rhs)
{
    TChunkStripeStatistics result;
    result.ChunkCount = lhs.ChunkCount + rhs.ChunkCount;
    result.DataWeight = lhs.DataWeight + rhs.DataWeight;
    result.RowCount = lhs.RowCount + rhs.RowCount;
    result.MaxBlockSize = std::max(lhs.MaxBlockSize, rhs.MaxBlockSize);
    return result;
}

TChunkStripeStatistics& operator += (
    TChunkStripeStatistics& lhs,
    const TChunkStripeStatistics& rhs)
{
    lhs.ChunkCount += rhs.ChunkCount;
    lhs.DataWeight += rhs.DataWeight;
    lhs.RowCount += rhs.RowCount;
    lhs.MaxBlockSize = std::max(lhs.MaxBlockSize, rhs.MaxBlockSize);
    return lhs;
}

TChunkStripeStatisticsVector AggregateStatistics(
    const TChunkStripeStatisticsVector& statistics)
{
    TChunkStripeStatistics sum;
    for (const auto& stat : statistics) {
        sum += stat;
    }
    return TChunkStripeStatisticsVector(1, sum);
}

////////////////////////////////////////////////////////////////////////////////

TChunkStripeList::TChunkStripeList(int stripeCount)
    : Stripes(stripeCount)
{ }

TChunkStripeStatisticsVector TChunkStripeList::GetStatistics() const
{
    TChunkStripeStatisticsVector result;
    result.reserve(Stripes.size());
    for (const auto& stripe : Stripes) {
        result.push_back(stripe->GetStatistics());
    }
    return result;
}

TChunkStripeStatistics TChunkStripeList::GetAggregateStatistics() const
{
    TChunkStripeStatistics result;
    result.ChunkCount = TotalChunkCount;
    if (IsApproximate) {
        result.RowCount = TotalRowCount * ApproximateSizesBoostFactor;
        result.DataWeight = TotalDataWeight * ApproximateSizesBoostFactor;
    } else {
        result.RowCount = TotalRowCount;
        result.DataWeight = TotalDataWeight;
    }
    return result;
}

void TChunkStripeList::AddStripe(TChunkStripePtr stripe)
{
    auto statistics = stripe->GetStatistics();
    TotalChunkCount += statistics.ChunkCount;
    TotalDataWeight += statistics.DataWeight;
    TotalRowCount += statistics.RowCount;
    Stripes.emplace_back(std::move(stripe));
}

void TChunkStripeList::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Stripes);
    Persist(context, PartitionTag);
    Persist(context, IsApproximate);
    Persist(context, TotalDataWeight);
    Persist(context, LocalDataWeight);
    Persist(context, TotalRowCount);
    Persist(context, TotalChunkCount);
    Persist(context, LocalChunkCount);
}

const TChunkStripeListPtr NullStripeList = New<TChunkStripeList>();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkPools
