#include "input_chunk_mapping.h"

#include <yt/yt/ytlib/chunk_client/input_chunk.h>
#include <yt/yt/ytlib/chunk_client/legacy_data_slice.h>

#include <yt/yt/ytlib/chunk_pools/chunk_stripe.h>

#include <yt/yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NChunkPools {

using namespace NChunkClient;
using namespace NTableClient;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TInputChunkMappingPtr IdentityChunkMapping = New<TInputChunkMapping>(EChunkMappingMode::Unordered);

////////////////////////////////////////////////////////////////////////////////

TInputChunkMapping::TInputChunkMapping(EChunkMappingMode mode)
    : Mode_(mode)
{ }

TChunkStripePtr TInputChunkMapping::GetMappedStripe(const TChunkStripePtr& stripe) const
{
    YT_VERIFY(stripe);

    if (Substitutes_.empty()) {
        return stripe;
    }

    auto mappedStripe = New<TChunkStripe>();
    for (const auto& dataSlice : stripe->DataSlices) {
        if (dataSlice->Type == EDataSourceType::UnversionedTable) {
            const auto& chunk = dataSlice->GetSingleUnversionedChunk();
            auto iterator = Substitutes_.find(chunk);
            if (iterator == Substitutes_.end()) {
                // The chunk was never substituted, so it remains as is.
                mappedStripe->DataSlices.emplace_back(dataSlice);
            } else {
                const auto& substitutes = iterator->second;
                if (substitutes.empty()) {
                    continue;
                }

                if (dataSlice->IsLegacy) {
                    // COMPAT(max42): keeping old code as is to ensure old behavior.
                    if (dataSlice->HasLimits()) {
                        YT_VERIFY(substitutes.size() == 1);
                        auto substituteChunk = substitutes.front();
                        auto chunkSlice = CreateInputChunkSlice(substituteChunk);
                        chunkSlice->LegacyLowerLimit() = dataSlice->ChunkSlices[0]->LegacyLowerLimit();
                        chunkSlice->LegacyUpperLimit() = dataSlice->ChunkSlices[0]->LegacyUpperLimit();
                        mappedStripe->DataSlices.emplace_back(New<TLegacyDataSlice>(
                            dataSlice->Type,
                            TLegacyDataSlice::TChunkSliceList{std::move(chunkSlice)},
                            dataSlice->LegacyLowerLimit(),
                            dataSlice->LegacyUpperLimit()));
                        mappedStripe->DataSlices.back()->SetInputStreamIndex(dataSlice->GetInputStreamIndex());
                    } else {
                        for (const auto& substituteChunk : substitutes) {
                            mappedStripe->DataSlices.emplace_back(New<TLegacyDataSlice>(
                                dataSlice->Type,
                                TLegacyDataSlice::TChunkSliceList{CreateInputChunkSlice(substituteChunk)} ));
                            mappedStripe->DataSlices.back()->SetInputStreamIndex(dataSlice->GetInputStreamIndex());
                        }
                    }
                } else {
                    if (dataSlice->HasLimits()) {
                        YT_VERIFY(substitutes.size() == 1);
                        auto substituteChunk = substitutes.front();

                        auto mappedDataSlice = CreateInputDataSlice(dataSlice);
                        mappedDataSlice->ChunkSlices[0]->SetInputChunk(substituteChunk);
                        mappedDataSlice->CopyPayloadFrom(*dataSlice);
                        mappedStripe->DataSlices.emplace_back(std::move(mappedDataSlice));
                    } else {
                        for (const auto& substituteChunk : substitutes) {
                            auto mappedDataSlice = CreateInputDataSlice(dataSlice);
                            mappedDataSlice->ChunkSlices[0]->SetInputChunk(substituteChunk);
                            mappedDataSlice->CopyPayloadFrom(*dataSlice);
                            mappedStripe->DataSlices.emplace_back(std::move(mappedDataSlice));
                        }
                    }
                }
            }
        } else {
            // Let's hope versioned chunks are never lost nor regenerated.
            for (const auto& chunkSlice : dataSlice->ChunkSlices) {
                YT_VERIFY(!Substitutes_.contains(chunkSlice->GetInputChunk()));
            }
            mappedStripe->DataSlices.emplace_back(dataSlice);
        }
    }

    return mappedStripe;
}

void TInputChunkMapping::OnStripeRegenerated(
    IChunkPoolInput::TCookie cookie,
    const NChunkPools::TChunkStripePtr& newStripe)
{
    YT_VERIFY(cookie != IChunkPoolInput::NullCookie);
    const auto& oldStripe = OriginalStripes_[cookie];
    YT_VERIFY(oldStripe);

    // NB(gritukan, max42): YT-14252.
    if (Mode_ == EChunkMappingMode::SortedWithoutKeyGuarantree) {
        THROW_ERROR_EXCEPTION("Chunk mapping invalidation cannot be reliably checked when key guarantee is disabled");
    }

    if (Mode_ == EChunkMappingMode::Sorted) {
        if (oldStripe->DataSlices.size() != newStripe->DataSlices.size()) {
            THROW_ERROR_EXCEPTION("New stripe has different number of data slices")
                << TErrorAttribute("old_data_slice_count", oldStripe->DataSlices.size())
                << TErrorAttribute("new_data_slice_count", newStripe->DataSlices.size());
        }

        for (int index = 0; index < std::ssize(oldStripe->DataSlices); ++index) {
            const auto& oldChunk = oldStripe->DataSlices[index]->GetSingleUnversionedChunk();
            const auto& newChunk = newStripe->DataSlices[index]->GetSingleUnversionedChunk();
            ValidateSortedChunkConsistency(oldChunk, newChunk);
        }
    }

    for (int index = 0; index < std::ssize(oldStripe->DataSlices); ++index) {
        const auto& oldSlice = oldStripe->DataSlices[index];
        // Versioned slices may not be lost and regenerated.
        YT_VERIFY(oldSlice->Type == EDataSourceType::UnversionedTable);
        const auto& oldChunk = oldSlice->GetSingleUnversionedChunk();

        // In case of unordered mode we distribute the substitutes uniformly
        // among the original chunks.
        int begin = (index * newStripe->DataSlices.size()) / oldStripe->DataSlices.size();
        int end = ((index + 1) * newStripe->DataSlices.size()) / oldStripe->DataSlices.size();

        auto& substitutes = Substitutes_[oldChunk];
        substitutes.clear();
        substitutes.reserve(end - begin);

        for (int newIndex = begin; newIndex < end; ++newIndex) {
            const auto& newChunk = newStripe->DataSlices[newIndex]->GetSingleUnversionedChunk();
            substitutes.emplace_back(newChunk);
        }
    }
}

void TInputChunkMapping::ValidateSortedChunkConsistency(
    const TInputChunkPtr& oldChunk,
    const TInputChunkPtr& newChunk) const
{
    std::optional<TOwningBoundaryKeys> oldBoundaryKeys =
        oldChunk->BoundaryKeys() ? std::make_optional(*oldChunk->BoundaryKeys()) : std::nullopt;
    std::optional<TOwningBoundaryKeys> newBoundaryKeys =
        newChunk->BoundaryKeys() ? std::make_optional(*newChunk->BoundaryKeys()) : std::nullopt;
    if (oldBoundaryKeys != newBoundaryKeys) {
        // Due to some weird linkage error, I cannot use bare oldBoundaryKeys/newBoundaryKeys
        // as arguments in TErrorAttribute.

        TYsonString oldBoundaryKeysYson;
        if (oldBoundaryKeys) {
            oldBoundaryKeysYson = BuildYsonStringFluently()
                .Value(*oldBoundaryKeys);
        } else {
            oldBoundaryKeysYson = TYsonString(TStringBuf("#"));
        }
        TYsonString newBoundaryKeysYson;
        if (newBoundaryKeys) {
            newBoundaryKeysYson = BuildYsonStringFluently()
                .Value(*newBoundaryKeys);
        } else {
            newBoundaryKeysYson = TYsonString(TStringBuf("#"));
        }
        THROW_ERROR_EXCEPTION("Corresponding chunks in old and new stripes have different boundary keys")
            << TErrorAttribute("old_chunk_id", oldChunk->GetChunkId())
            << TErrorAttribute("old_boundary_keys", oldBoundaryKeysYson)
            << TErrorAttribute("new_chunk_id", newChunk->GetChunkId())
            << TErrorAttribute("new_boundary_keys", newBoundaryKeysYson);
    }
    if (oldChunk->GetRowCount() != newChunk->GetRowCount()) {
        THROW_ERROR_EXCEPTION("Corresponding chunks in old and new stripes have different row counts")
            << TErrorAttribute("old_chunk_id", oldChunk->GetChunkId())
            << TErrorAttribute("old_row_count", oldChunk->GetRowCount())
            << TErrorAttribute("new_chunk_id", newChunk->GetChunkId())
            << TErrorAttribute("new_row_count", newChunk->GetRowCount());
    }
}

void TInputChunkMapping::OnChunkDisappeared(const TInputChunkPtr& chunk)
{
    Substitutes_[chunk].clear();
}

void TInputChunkMapping::Reset(IChunkPoolInput::TCookie resetCookie, const TChunkStripePtr& resetStripe)
{
    for (auto& [cookie, stripe] : OriginalStripes_) {
        if (cookie == resetCookie) {
            stripe = resetStripe;
        } else {
            stripe = GetMappedStripe(stripe);
        }
    }

    Substitutes_.clear();
}

void TInputChunkMapping::Add(IChunkPoolInput::TCookie cookie, const TChunkStripePtr& stripe)
{
    YT_VERIFY(OriginalStripes_.emplace(cookie, stripe).second);
}

void TInputChunkMapping::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist<TMapSerializer<TDefaultSerializer, TDefaultSerializer, TUnsortedTag>>(context, Substitutes_);
    Persist<TMapSerializer<TDefaultSerializer, TDefaultSerializer, TUnsortedTag>>(context, OriginalStripes_);
    Persist(context, Mode_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkPools

