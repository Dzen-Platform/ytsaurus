#include "helpers.h"

#include "chunk_pool.h"

namespace NYT::NChunkPools {

using namespace NNodeTrackerClient;
using namespace NControllerAgent;
using namespace NChunkClient;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

void AddStripeToList(
    TChunkStripePtr stripe,
    const TChunkStripeListPtr& list,
    std::optional<i64> stripeDataWeight,
    std::optional<i64> stripeRowCount,
    TNodeId nodeId)
{
    auto statistics = stripe->GetStatistics();
    list->TotalDataWeight += stripeDataWeight.value_or(statistics.DataWeight);
    list->TotalRowCount += stripeRowCount.value_or(statistics.RowCount);
    list->TotalChunkCount += statistics.ChunkCount;
    list->Stripes.emplace_back(std::move(stripe));

    if (nodeId == InvalidNodeId) {
        return;
    }

    for (const auto& dataSlice : list->Stripes.back()->DataSlices) {
        for (const auto& chunkSlice : dataSlice->ChunkSlices) {
            bool isLocal = false;
            for (auto replica : chunkSlice->GetInputChunk()->GetReplicaList()) {
                if (replica.GetNodeId() == nodeId) {
                    i64 locality = chunkSlice->GetLocality(replica.GetReplicaIndex());
                    if (locality > 0) {
                        list->LocalDataWeight += locality;
                        isLocal = true;
                    }
                }
            }

            if (isLocal) {
                ++list->LocalChunkCount;
            }
        }
    }
}

std::vector<TInputChunkPtr> GetStripeListChunks(const TChunkStripeListPtr& stripeList)
{
    std::vector<TInputChunkPtr> chunks;
    for (const auto& stripe : stripeList->Stripes) {
        for (const auto& dataSlice : stripe->DataSlices) {
            chunks.emplace_back(dataSlice->GetSingleUnversionedChunkOrThrow());
        }
    }
    return chunks;
}

////////////////////////////////////////////////////////////////////////////////

TSuspendableStripe::TSuspendableStripe(TChunkStripePtr stripe)
    : Stripe_(std::move(stripe))
    , Statistics_(Stripe_->GetStatistics())
{ }

const TChunkStripePtr& TSuspendableStripe::GetStripe() const
{
    return Stripe_;
}

const TChunkStripeStatistics& TSuspendableStripe::GetStatistics() const
{
    return Statistics_;
}

bool TSuspendableStripe::Suspend()
{
    return SuspendedStripeCount_++ == 0;
}

bool TSuspendableStripe::IsSuspended() const
{
    return SuspendedStripeCount_ > 0;
}

bool TSuspendableStripe::Resume()
{
    YT_VERIFY(SuspendedStripeCount_ > 0);

    return --SuspendedStripeCount_ == 0;
}

void TSuspendableStripe::Reset(TChunkStripePtr stripe)
{
    YT_VERIFY(stripe);

    Stripe_ = stripe;
}

void TSuspendableStripe::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Stripe_);
    Persist(context, Teleport_);
    Persist(context, SuspendedStripeCount_);
    Persist(context, Statistics_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkPools

