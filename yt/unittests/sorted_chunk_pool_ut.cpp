#include "framework.h"

#include "chunk_slice_fetcher_mock.h"

#include <util/stream/null.h>

#include <yt/core/misc/blob_output.h>
#include <yt/core/misc/phoenix.h>

#include <yt/server/scheduler/sorted_chunk_pool.h>

#include <yt/ytlib/table_client/row_buffer.h>

#include <random>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

//! We intentionally suppress the default output for TInputChunk as they are
//! identified by their addresses (already printed from the template function
//! for TIntrusivePtr) pretty well.
void PrintTo(const TInputChunk& /* chunk */, std::ostream* /* os */)
{ }

////////////////////////////////////////////////////////////////////////////////

} // namesapce NChunkClient
} // namespace NYT

namespace NYT {
namespace NScheduler {
namespace {

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NNodeTrackerClient;
using namespace NTableClient;

using namespace ::testing;

////////////////////////////////////////////////////////////////////////////////

//! A unit to measure all sizes in this file.
static constexpr i64 KB = 1024;
static constexpr i32 Inf32 = std::numeric_limits<i32>::max();
static constexpr i64 Inf64 = std::numeric_limits<i64>::max();

////////////////////////////////////////////////////////////////////////////////

class TSortedChunkPoolTest
    : public Test
{
protected:
    virtual void SetUp() override
    {
        ChunkSliceFetcher_ = New<StrictMock<TMockChunkSliceFetcher>>();
        Options_.MinTeleportChunkSize = Inf64;
        Options_.SortedJobOptions.MaxTotalSliceCount = Inf64;
        DataSizePerJob_ = Inf64;
        MaxDataSlicesPerJob_ = Inf32;
        InputSliceDataSize_ = Inf64;
    }

    void InitJobConstraints()
    {
        Options_.JobSizeConstraints = CreateExplicitJobSizeConstraints(
            false /* canAdjustDataSizePerJob */,
            false /* isExplicitJobCount */,
            0 /* jobCount */,
            DataSizePerJob_,
            MaxDataSlicesPerJob_,
            0 /* maxDataSizePerJob_ */,
            InputSliceDataSize_,
            Inf64 /* inputSliceRowCount */);
    }

    struct TMockChunkSliceFetcherBuilder
    {
        TMockChunkSliceFetcherBuilder(TSortedChunkPoolTest* owner)
            : Owner(owner)
        { }

        TStrictMockChunkSliceFetcherPtr Build()
        {
            Expectation expectation = EXPECT_CALL(*ChunkSliceFetcher, Fetch())
                .After(AllChunksAreAdded)
                .WillOnce(Return(VoidFuture));

            EXPECT_CALL(*ChunkSliceFetcher, GetChunkSlices())
                .After(expectation)
                .WillOnce(ReturnPointee(&ChunkSlices));

            return ChunkSliceFetcher;
        }

        void RegisterSliceableUnversionedChunk(const TInputChunkPtr& chunk, std::vector<TInputChunkSlicePtr> slices)
        {
            ChunkSlices.insert(ChunkSlices.end(), slices.begin(), slices.end());
            AllChunksAreAdded += EXPECT_CALL(*ChunkSliceFetcher, AddChunk(chunk));
        }

        void RegisterTriviallySliceableUnversionedChunk(const TInputChunkPtr& chunk)
        {
            auto chunkSlices = Owner->SliceUnversionedChunk(chunk, {}, {chunk->GetCompressedDataSize()});
            RegisterSliceableUnversionedChunk(chunk, std::move(chunkSlices));
        }

        ExpectationSet AllChunksAreAdded;
        TStrictMockChunkSliceFetcherPtr ChunkSliceFetcher = New<StrictMock<TMockChunkSliceFetcher>>();
        std::vector<TInputChunkSlicePtr> ChunkSlices;
        TSortedChunkPoolTest* Owner;
    };

    std::vector<TMockChunkSliceFetcherBuilder> MockBuilders_;
    std::vector<TStrictMockChunkSliceFetcherPtr> Fetchers_;

    IChunkSliceFetcherFactoryPtr BuildMockChunkSliceFetcherFactory()
    {
        YCHECK(Fetchers_.empty());
        for (auto& mockBuilder : MockBuilders_) {
            Fetchers_.emplace_back(mockBuilder.Build());
        }
        return New<TMockChunkSliceFetcherFactory>(&Fetchers_);
    }

    void PrepareNewMock()
    {
        MockBuilders_.emplace_back(this);
    }

    TMockChunkSliceFetcherBuilder& CurrentMock()
    {
        YCHECK(!MockBuilders_.empty());
        return MockBuilders_.back();
    }

    // In this test we will only deal with integral rows as
    // all the logic inside sorted chunk pool does not depend on
    // actual type of values in keys.
    TKey BuildRow(std::vector<i64> values)
    {
        auto row = RowBuffer_->Allocate(values.size());
        for (int index = 0; index < values.size(); ++index) {
            row[index] = MakeUnversionedInt64Value(values[index], index);
        }
        return row;
    }

    TInputChunkPtr CreateChunk(
        const TKey& minBoundaryKey,
        const TKey& maxBoundaryKey,
        int tableIndex,
        i64 size = 1 * KB,
        const TKey& lowerLimit = TKey(),
        const TKey& upperLimit = TKey(),
        i64 rowCount = 1000)
    {
        auto inputChunk = New<TInputChunk>();
        inputChunk->ChunkId() = TChunkId::Create();
        inputChunk->SetCompressedDataSize(size);
        inputChunk->SetUncompressedDataSize(size);
        inputChunk->BoundaryKeys() = std::make_unique<TBoundaryKeys>(TBoundaryKeys {
            TOwningKey(minBoundaryKey),
            TOwningKey(maxBoundaryKey)
        });
        inputChunk->SetTableIndex(tableIndex);
        inputChunk->SetTableRowIndex(UnversionedTableRowCounts_[tableIndex]);
        UnversionedTableRowCounts_[tableIndex] += rowCount;
        if (lowerLimit) {
            inputChunk->LowerLimit() = std::make_unique<TReadLimit>(TOwningKey(lowerLimit));
        }
        if (upperLimit) {
            inputChunk->UpperLimit() = std::make_unique<TReadLimit>(TOwningKey(upperLimit));
        }
        if (!InputTables_[tableIndex].IsVersioned() && !InputTables_[tableIndex].IsForeign()) {
            CreatedUnversionedPrimaryChunks_.insert(inputChunk);
        }
        inputChunk->SetRowCount(rowCount);
        return inputChunk;
    }

    TInputChunkPtr CopyChunk(const TInputChunkPtr& chunk)
    {
        TInputChunkPtr chunkCopy = New<TInputChunk>();
        chunkCopy->ChunkId() = chunk->ChunkId();
        chunkCopy->SetCompressedDataSize(chunk->GetCompressedDataSize());
        chunkCopy->BoundaryKeys() = std::make_unique<TBoundaryKeys>(*chunk->BoundaryKeys());
        int tableIndex = chunk->GetTableIndex();
        chunkCopy->SetTableIndex(tableIndex);
        chunkCopy->SetTableRowIndex(chunk->GetTableRowIndex());
        chunkCopy->SetRowCount(chunk->GetRowCount());
        if (chunk->LowerLimit()) {
            chunkCopy->LowerLimit() = std::make_unique<TReadLimit>(*chunk->LowerLimit());
        }
        if (chunk->UpperLimit()) {
            chunkCopy->UpperLimit() = std::make_unique<TReadLimit>(*chunk->UpperLimit());
        }
        if (!InputTables_[tableIndex].IsVersioned() && !InputTables_[tableIndex].IsForeign()) {
            CreatedUnversionedPrimaryChunks_.insert(chunkCopy);
        }
        return chunkCopy;
    }

    void InitTables(std::vector<bool> isForeign, std::vector<bool> isTeleportable, std::vector<bool> isVersioned)
    {
        YCHECK(isForeign.size() == isTeleportable.size() && isTeleportable.size() == isVersioned.size() && isForeign.size() > 0);
        for (int index = 0; index < isForeign.size(); ++index) {
            InputTables_.emplace_back(isTeleportable[index], !isForeign[index] /* isPrimary */, isVersioned[index]);
        }
        UnversionedTableRowCounts_.resize(InputTables_.size(), 0);
    }

    std::vector<TInputChunkSlicePtr> SliceUnversionedChunk(
        TInputChunkPtr chunk,
        std::vector<TKey> internalPoints,
        std::vector<i64> sliceSizes = std::vector<i64>(),
        std::vector<i64> sliceRowCounts = std::vector<i64>())
    {
        if (sliceSizes.empty()) {
            sliceSizes.assign(internalPoints.size() + 1, chunk->GetUncompressedDataSize() / (internalPoints.size() + 1));
            // Fix the first size to fix the error because of integer division.
            sliceSizes[0] += chunk->GetUncompressedDataSize() - (internalPoints.size() + 1) * sliceSizes[0];
        } else {
            YCHECK(internalPoints.size() + 1 == sliceSizes.size());
        }
        if (sliceRowCounts.empty()) {
            sliceRowCounts.assign(internalPoints.size() + 1, chunk->GetRowCount() / (internalPoints.size() + 1));
            sliceRowCounts[0] += chunk->GetRowCount() - (internalPoints.size() + 1) * sliceRowCounts[0];
        } else {
            YCHECK(internalPoints.size() + 1 == sliceSizes.size());
        }

        YCHECK(!InputTables_[chunk->GetTableIndex()].IsVersioned());

        TKey lastKey = chunk->LowerLimit() ? chunk->LowerLimit()->GetKey() : chunk->BoundaryKeys()->MinKey;
        i64 currentRow = 0;
        std::vector<TInputChunkSlicePtr> slices;
        for (int index = 0; index <= internalPoints.size(); ++index) {
            TKey upperLimit = index < internalPoints.size()
                ? GetKeySuccessor(internalPoints[index], RowBuffer_)
                : (chunk->UpperLimit()
                ? chunk->UpperLimit()->GetKey()
                : GetKeySuccessor(chunk->BoundaryKeys()->MaxKey, RowBuffer_));
            slices.emplace_back(New<TInputChunkSlice>(chunk, lastKey, upperLimit));
            if (!internalPoints.empty()) {
                slices.back()->LowerLimit().RowIndex = currentRow;
                currentRow += sliceRowCounts[index];
                slices.back()->UpperLimit().RowIndex = currentRow;
                slices.back()->OverrideSize(sliceRowCounts[index], sliceSizes[index]);
            }
            lastKey = upperLimit;
        }
        return slices;
    }

    void CreateChunkPool(bool useGenericInputStreamDirectory = false)
    {
        ChunkPool_ = CreateSortedChunkPool(
            Options_,
            !MockBuilders_.empty() ? BuildMockChunkSliceFetcherFactory() : nullptr,
            useGenericInputStreamDirectory ? IntermediateInputStreamDirectory : TInputStreamDirectory(InputTables_));
        ChunkPool_->SubscribePoolOutputInvalidated(BIND(&TSortedChunkPoolTest::StoreInvalidationError, this));
    }

    TInputDataSlicePtr BuildDataSliceByChunk(const TInputChunkPtr& chunk)
    {
        auto dataSlice = CreateUnversionedInputDataSlice(CreateInputChunkSlice(chunk));
        dataSlice->Tag = chunk->ChunkId().Parts64[0] ^ chunk->ChunkId().Parts64[1];
        return dataSlice;
    }

    IChunkPoolInput::TCookie AddChunk(const TInputChunkPtr& chunk)
    {
        auto dataSlice = BuildDataSliceByChunk(chunk);
        ActiveChunks_.insert(chunk->ChunkId());
        InferLimitsFromBoundaryKeys(dataSlice, RowBuffer_);
        return ChunkPool_->Add(New<TChunkStripe>(dataSlice));
    }

    IChunkPoolInput::TCookie AddMultiChunkStripe(std::vector<TInputChunkPtr> chunks)
    {
        std::vector<TInputDataSlicePtr> dataSlices;
        for (const auto& chunk : chunks) {
            auto dataSlice = BuildDataSliceByChunk(chunk);
            InferLimitsFromBoundaryKeys(dataSlice, RowBuffer_);
            dataSlices.emplace_back(std::move(dataSlice));
        }
        auto stripe = New<TChunkStripe>();
        std::move(dataSlices.begin(), dataSlices.end(), std::back_inserter(stripe->DataSlices));
        return ChunkPool_->Add(stripe);
    }

    void SuspendChunk(IChunkPoolInput::TCookie cookie, const TInputChunkPtr& chunk)
    {
        YCHECK(ActiveChunks_.erase(chunk->ChunkId()));
        ChunkPool_->Suspend(cookie);
    }

    void ResumeChunk(IChunkPoolInput::TCookie cookie, const TInputChunkPtr& chunk)
    {
        auto dataSlice = BuildDataSliceByChunk(chunk);
        InferLimitsFromBoundaryKeys(dataSlice, RowBuffer_);
        ActiveChunks_.insert(chunk->ChunkId());
        return ChunkPool_->Resume(cookie, New<TChunkStripe>(dataSlice));
    }

    void ExtractOutputCookiesWhilePossible()
    {
        while (ChunkPool_->GetPendingJobCount()) {
            ExtractedCookies_.emplace_back(ExtractCookie(TNodeId(0)));
        }
    }

    IChunkPoolOutput::TCookie ExtractCookie(TNodeId nodeId)
    {
        auto cookie = ChunkPool_->Extract(nodeId);
        if (cookie != IChunkPoolOutput::NullCookie) {
            OutputCookies_.insert(cookie);
        }
        return cookie;
    }

    void PersistAndRestore()
    {
        TBlobOutput output;
        TSaveContext saveContext;
        saveContext.SetOutput(&output);
        Save(saveContext, ChunkPool_);
        auto blob = output.Flush();
        ChunkPool_.reset();

        TMemoryInput input(blob.Begin(), blob.Size());
        TLoadContext loadContext;
        loadContext.SetRowBuffer(RowBuffer_);
        loadContext.SetInput(&input);
        Load(loadContext, ChunkPool_);
        ChunkPool_->SubscribePoolOutputInvalidated(BIND(&TSortedChunkPoolTest::StoreInvalidationError, this));
    }

    std::vector<TChunkStripeListPtr> GetAllStripeLists()
    {
        std::vector<TChunkStripeListPtr> stripeLists;
        for (auto cookie : OutputCookies_) {
            stripeLists.emplace_back(ChunkPool_->GetStripeList(cookie));
        }
        return stripeLists;
    }

    //! Check that:
    //! * The given stripe lists cover each input chunk with specified read limits without overlapping;
    //! * For each input table the input data slices follow in an ascending order with tie broken by:
    //! *** For the unversioned tables by chunk row index;
    //! *** For the versioned tables by the full key;
    void CheckDataIntegrity(const std::vector<TChunkStripeListPtr>& stripeLists, const std::vector<TInputChunkPtr>& teleportChunks)
    {
        yhash_map<TInputChunkPtr, std::vector<TInputChunkSlicePtr>> chunkSlicesByInputChunk;
        yhash_set<TInputChunkPtr> teleportChunksSet(teleportChunks.begin(), teleportChunks.end());

        // Check that data slices from each stripe are all from the same table.
        for (const auto& stripeList : stripeLists) {
            for (const auto& stripe : stripeList->Stripes) {
                ASSERT_TRUE(!stripe->DataSlices.empty());
                int tableIndex = stripe->DataSlices.front()->GetTableIndex();

                for (const auto& dataSlice : stripe->DataSlices) {
                    for (const auto& chunkSlice : dataSlice->ChunkSlices) {
                        const auto& inputChunk = chunkSlice->GetInputChunk();
                        EXPECT_EQ(tableIndex, inputChunk->GetTableIndex());
                        chunkSlicesByInputChunk[inputChunk].emplace_back(chunkSlice);
                    }
                }
            }
        }

        // First check.
        for (const auto& inputChunk : CreatedUnversionedPrimaryChunks_) {
            if (teleportChunksSet.has(inputChunk)) {
                continue;
            }
            TKey chunkLowerKey = inputChunk->LowerLimit() && inputChunk->LowerLimit()->HasKey()
                ? inputChunk->LowerLimit()->GetKey()
                : inputChunk->BoundaryKeys()->MinKey;
            TKey chunkUpperKey = inputChunk->UpperLimit() && inputChunk->UpperLimit()->HasKey()
                ? inputChunk->UpperLimit()->GetKey()
                : GetKeySuccessor(inputChunk->BoundaryKeys()->MaxKey, RowBuffer_);
            i64 chunkLowerRowIndex = inputChunk->LowerLimit() && inputChunk->LowerLimit()->HasRowIndex()
                ? inputChunk->LowerLimit()->GetRowIndex()
                : 0;
            i64 chunkUpperRowIndex = inputChunk->UpperLimit() && inputChunk->UpperLimit()->HasRowIndex()
                ? inputChunk->UpperLimit()->GetRowIndex()
                : inputChunk->GetRowCount();

            TKey lastLowerKey;
            TKey lastUpperKey = chunkLowerKey;
            i64 lastLeftRowIndex = -1;
            i64 lastRightRowIndex = chunkLowerRowIndex;
            auto it = chunkSlicesByInputChunk.find(inputChunk);
            ASSERT_TRUE(chunkSlicesByInputChunk.end() != it);
            auto& chunkSlices = it->second;
            for (const auto& chunkSlice : chunkSlices) {
                TKey chunkSliceLowerKey = chunkSlice->LowerLimit().Key;
                TKey chunkSliceUpperKey = chunkSlice->UpperLimit().Key;
                i64 chunkSliceLowerRowIndex = chunkSlice->LowerLimit().RowIndex
                    ? *chunkSlice->LowerLimit().RowIndex
                    : chunkLowerRowIndex;
                i64 chunkSliceUpperRowIndex = chunkSlice->UpperLimit().RowIndex
                    ? *chunkSlice->UpperLimit().RowIndex
                    : chunkUpperRowIndex;

                bool keysCoincide = lastUpperKey == chunkSliceLowerKey;
                bool rowIndicesCoincide = lastRightRowIndex == chunkSliceLowerRowIndex;
                EXPECT_TRUE(keysCoincide || rowIndicesCoincide);
                if (!keysCoincide) {
                    EXPECT_EQ(lastLowerKey, chunkSliceLowerKey);
                    EXPECT_EQ(lastUpperKey, chunkSliceUpperKey);
                }
                if (!rowIndicesCoincide) {
                    EXPECT_EQ(lastLeftRowIndex, chunkSliceLowerRowIndex);
                    EXPECT_EQ(lastRightRowIndex, chunkSliceUpperRowIndex);
                }
                lastLowerKey = chunkSliceLowerKey;
                lastUpperKey = chunkSliceUpperKey;
                lastLeftRowIndex = chunkSliceLowerRowIndex;
                lastRightRowIndex = chunkSliceUpperRowIndex;
            }
            EXPECT_EQ(lastUpperKey, chunkUpperKey);
            EXPECT_EQ(lastRightRowIndex, chunkUpperRowIndex);
        }

        // Second check.
        auto unversionedDataSliceComparator = [] (const TInputDataSlicePtr& lhs, const TInputDataSlicePtr& rhs) {
            auto lhsChunk = lhs->GetSingleUnversionedChunkOrThrow();
            auto rhsChunk = rhs->GetSingleUnversionedChunkOrThrow();
            if (lhsChunk != rhsChunk) {
                return lhsChunk->GetTableRowIndex() < rhsChunk->GetTableRowIndex();
            } else {
                return lhs->LowerLimit().Key <= rhs->LowerLimit().Key;
            }
        };
        auto versionedDataSliceComparator = [] (const TInputDataSlicePtr& lhs, const TInputDataSlicePtr& rhs) {
            return lhs->LowerLimit().Key <= rhs->LowerLimit().Key;
        };

        for (const auto& stripeList : stripeLists) {
            for (const auto& stripe : stripeList->Stripes) {
                ASSERT_TRUE(!stripe->DataSlices.empty());
                int tableIndex = stripe->DataSlices.front()->GetTableIndex();
                if (!InputTables_[tableIndex].IsForeign()) {
                    const auto& comparator = (InputTables_[tableIndex].IsVersioned()) ? versionedDataSliceComparator : unversionedDataSliceComparator;
                    for (int index = 0; index + 1 < stripe->DataSlices.size(); ++index) {
                        const auto& lhs = stripe->DataSlices[index];
                        const auto& rhs = stripe->DataSlices[index + 1];
                        EXPECT_TRUE(comparator(lhs, rhs));
                    }
                }
            }
        }
    }

    //! Check that:
    //! * All teleport chunks satisfy the Options_.MinTeleportChunkSize constraint;
    //! * All stripe lists have no more than Options_.MaxDataSlicesPerJob + InputTables_.size() - 1 data slices in total.
    //! Unfortunately we cannot check the Options_.MaxPrimaryDataSizePerJob constraint satisfaction as this is not an absolute
    //! restriction, but only a best-effort bound.
    void TryCheckJobConstraintsSatisfaction(
        const std::vector<TChunkStripeListPtr>& stripeLists,
        const std::vector<TInputChunkPtr>& teleportChunks)
    {
        for (const auto& teleportChunk : teleportChunks) {
            EXPECT_TRUE(teleportChunk->IsLargeCompleteChunk(Options_.MinTeleportChunkSize));
        }

        for (const auto& stripeList : stripeLists) {
            int dataSlicesTotalNumber = 0;
            for (const auto& stripe : stripeList->Stripes) {
                if (stripe) {
                    dataSlicesTotalNumber += stripe->DataSlices.size();
                }
            }
            EXPECT_LE(dataSlicesTotalNumber, MaxDataSlicesPerJob_ + InputTables_.size() - 1);
        }
    }

    //! Check that jobs do not overlap by keys. Applicable only when Options_.SortedJobOptions.EnableKeyGuarantee is true.
    void CheckKeyGuarantee(const std::vector<TChunkStripeListPtr>& stripeLists)
    {
        TKey lastUpperKey;
        for (const auto& stripeList : stripeLists) {
            TKey lowerKey = MaxKey();
            TKey upperKey = MinKey();
            for (const auto& stripe : stripeList->Stripes) {
                for (const auto& dataSlice : stripe->DataSlices) {
                    if (lowerKey > dataSlice->LowerLimit().Key) {
                        lowerKey = dataSlice->LowerLimit().Key;
                    }
                    if (upperKey < dataSlice->UpperLimit().Key) {
                        upperKey = dataSlice->UpperLimit().Key;
                    }
                }
            }
            EXPECT_LE(lastUpperKey, lowerKey);
            lastUpperKey = upperKey;
        }
    }

    //! Find all teleport chunks naively (in quadratic time) and check that chunk pool detected exactly
    //! the same chunks.
    void CheckTeleportChunks(const std::vector<TInputChunkPtr>& teleportChunks)
    {
        // TODO(max42): implement a naive procedure for finding the teleport chunks and compare
        // its result with `teleportChunks`.
    }

    //! Check the correctness of joined data (in quadratic time).
    void CheckCorrectnessOfJoin(const std::vector<TChunkStripeListPtr>& stripeLists)
    {
        // TODO(max42): implement a naive procedure here.
    }

    //! Perform all the correctness checks over the given result of sorted chunk pool invocation
    //! (without any suspends nor job interruptions).
    void CheckEverything(
        const std::vector<TChunkStripeListPtr>& stripeLists,
        const std::vector<TInputChunkPtr>& teleportChunks)
    {
        CheckDataIntegrity(stripeLists, teleportChunks);
        TryCheckJobConstraintsSatisfaction(stripeLists, teleportChunks);
        CheckTeleportChunks(teleportChunks);
        CheckStripeListsContainOnlyActiveChunks();
        CheckForeignStripesAreMarkedAsForeign();
        if (Options_.SortedJobOptions.EnableKeyGuarantee) {
            CheckKeyGuarantee(stripeLists);
        }
    }

    void CheckForeignStripesAreMarkedAsForeign()
    {
        for (auto cookie : OutputCookies_) {
            auto stripeList = ChunkPool_->GetStripeList(cookie);
            for (const auto& stripe : stripeList->Stripes) {
                int tableIndex = stripe->GetTableIndex();
                EXPECT_EQ(InputTables_[tableIndex].IsForeign(), stripe->Foreign);
            }
        }
    }

    void CheckStripeListsContainOnlyActiveChunks()
    {
        for (auto cookie : OutputCookies_) {
            auto stripeList = ChunkPool_->GetStripeList(cookie);
            for (const auto& stripe : stripeList->Stripes) {
                if (stripe) {
                    for (const auto& dataSlice : stripe->DataSlices) {
                        for (const auto& chunkSlice : dataSlice->ChunkSlices) {
                            auto chunk = chunkSlice->GetInputChunk();
                            EXPECT_TRUE(chunk);
                            EXPECT_TRUE(ActiveChunks_.has(chunk->ChunkId()));
                        }
                    }
                }
            }
        }
    }

    void StoreInvalidationError(const TError& error)
    {
        InvalidationErrors_.emplace_back(error);
    }

    std::unique_ptr<IChunkPool> ChunkPool_;

    //! Set containing all unversioned primary input chunks that have ever been created.
    yhash_set<TInputChunkPtr> CreatedUnversionedPrimaryChunks_;
    //! Set containing all chunks that are added to the pool without being suspended.
    yhash_set<TChunkId> ActiveChunks_;

    TIntrusivePtr<StrictMock<TMockChunkSliceFetcher>> ChunkSliceFetcher_;

    TRowBufferPtr RowBuffer_ = New<TRowBuffer>();

    std::vector<TInputStreamDescriptor> InputTables_;

    yhash_set<IChunkPoolOutput::TCookie> OutputCookies_;

    std::vector<int> UnversionedTableRowCounts_;

    TSortedChunkPoolOptions Options_;

    i64 DataSizePerJob_;

    i32 MaxDataSlicesPerJob_;

    i64 InputSliceDataSize_;

    std::vector<IChunkPoolOutput::TCookie> ExtractedCookies_;

    std::mt19937 Gen_;

    std::vector<TError> InvalidationErrors_;
};

////////////////////////////////////////////////////////////////////////////////

TEST_F(TSortedChunkPoolTest, SortedMergeTeleports1)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false, false} /* isForeign */,
        {true, true, true, true} /* isTeleportable */,
        {false, false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({0, 10}), BuildRow({1, 11}), 0);
    auto chunkB = CreateChunk(BuildRow({1, 12}), BuildRow({2, 10}), 1);
    auto chunkC = CreateChunk(BuildRow({1, 10}), BuildRow({1, 13}), 2);
    auto chunkD = CreateChunk(BuildRow({1, 12}), BuildRow({2, 10}), 3, 1 * KB, BuildRow({1, 13}), BuildRow({1, 17}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkD);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);
    AddChunk(chunkD);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, UnorderedElementsAreArray({chunkA, chunkB, chunkC}));
    EXPECT_EQ(1, stripeLists.size());

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, SortedMergeTeleports2)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false, false} /* isForeign */,
        {false, true, true, true} /* isTeleportable */,
        {false, false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({0, 10}), BuildRow({1, 11}), 0);
    auto chunkB = CreateChunk(BuildRow({1, 12}), BuildRow({2, 10}), 1);
    auto chunkC = CreateChunk(BuildRow({1, 10}), BuildRow({1, 13}), 2);
    auto chunkD = CreateChunk(BuildRow({1, 12}), BuildRow({2, 10}), 3, 1 * KB, BuildRow({1, 13}), BuildRow({1, 17}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkD);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);
    AddChunk(chunkD);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, UnorderedElementsAreArray({chunkB, chunkC}));
    // Non-teleportable chunks are separated with teleportable ones, so there should be two separate jobs.
    EXPECT_EQ(2, stripeLists.size());

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, SortedMergeTeleports3)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false} /* isForeign */,
        {true, true, true} /* isTeleportable */,
        {false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();

    auto chunkA = CreateChunk(BuildRow({0, 10}), BuildRow({1, 11}), 0);
    auto chunkB = CreateChunk(BuildRow({1, 12}), BuildRow({2, 10}), 1);
    auto chunkC = CreateChunk(BuildRow({1, 10}), BuildRow({1, 13}), 2);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, UnorderedElementsAreArray({chunkA, chunkB, chunkC}));
    EXPECT_EQ(0, stripeLists.size());

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, SortedMergeTeleports4)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false} /* isForeign */,
        {true, true, true} /* isTeleportable */,
        {false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 2;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({0, 10}), BuildRow({1, 11}), 0);
    auto chunkB = CreateChunk(BuildRow({1, 12}), BuildRow({2, 10}), 1);
    auto chunkC = CreateChunk(BuildRow({1, 10}), BuildRow({1, 13}), 2);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());
    EXPECT_EQ(1, stripeLists.size());

    CheckEverything(stripeLists, teleportChunks);
}

// NB(max42): completely getting into this test may take several hours of your life.
// Double-think before reading it :)
TEST_F(TSortedChunkPoolTest, SortedMergeAllKindOfTeleports)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false} /* isForeign */,
        {true, true} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 3;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();
    PrepareNewMock();

    // +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // Simple cases no read limits, keys of length exactly PrimaryPrefixLength.
    // +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    // Yes.
    // [==]_____
    // _____[==]
    auto chunkA1 = CreateChunk(BuildRow({1, 1, 0}), BuildRow({1, 1, 2}), 0);
    auto chunkB1 = CreateChunk(BuildRow({1, 1, 3}), BuildRow({1, 1, 5}), 1);

    // Yes (they share only one boundary key).
    // [==]___
    // ___[==]
    auto chunkA2 = CreateChunk(BuildRow({2, 1, 0}), BuildRow({2, 1, 2}), 0);
    auto chunkB2 = CreateChunk(BuildRow({2, 1, 2}), BuildRow({2, 1, 4}), 1);

    // No (they partially intersect).
    // [===]__
    // __[===]
    auto chunkA3 = CreateChunk(BuildRow({3, 1, 0}), BuildRow({3, 1, 2}), 0);
    auto chunkB3 = CreateChunk(BuildRow({3, 1, 1}), BuildRow({3, 1, 4}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA3);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB3);

    // No (one contained in another).
    // [====]__
    // _[==]___
    auto chunkA4 = CreateChunk(BuildRow({4, 1, 0}), BuildRow({4, 1, 3}), 0);
    auto chunkB4 = CreateChunk(BuildRow({4, 1, 1}), BuildRow({4, 1, 2}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA4);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB4);

    // No (single_key one contained in another).
    // [====]__
    // __[]____
    auto chunkA5 = CreateChunk(BuildRow({5, 1, 0}), BuildRow({5, 1, 3}), 0);
    auto chunkB5 = CreateChunk(BuildRow({5, 1, 1}), BuildRow({5, 1, 1}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA5);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB5);

    // No (they coincide).
    // [===]__
    // [===]__
    auto chunkA6 = CreateChunk(BuildRow({6, 1, 0}), BuildRow({6, 1, 3}), 0);
    auto chunkB6 = CreateChunk(BuildRow({6, 1, 0}), BuildRow({6, 1, 3}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA6);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB6);

    // No (one covers another).
    // [===]__
    // [====]_
    auto chunkA7 = CreateChunk(BuildRow({7, 1, 0}), BuildRow({7, 1, 3}), 0);
    auto chunkB7 = CreateChunk(BuildRow({7, 1, 0}), BuildRow({7, 1, 4}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA7);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB7);

    // No (one covers another).
    // _[===]__
    // [====]__
    auto chunkA8 = CreateChunk(BuildRow({8, 1, 0}), BuildRow({8, 1, 4}), 0);
    auto chunkB8 = CreateChunk(BuildRow({8, 1, 1}), BuildRow({8, 1, 4}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA8);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB8);

    // Yes (single-key is located exactly at the max boundary key of another).
    // [===]__
    // ___[]__
    auto chunkA9 = CreateChunk(BuildRow({9, 1, 0}), BuildRow({9, 1, 4}), 0);
    auto chunkB9 = CreateChunk(BuildRow({9, 1, 4}), BuildRow({9, 1, 4}), 1);

    // Yes (single-key is located exactly at the min boundary key of another).
    // [===]__
    // []_____
    auto chunkA10 = CreateChunk(BuildRow({10, 1, 0}), BuildRow({10, 1, 4}), 0);
    auto chunkB10 = CreateChunk(BuildRow({10, 1, 0}), BuildRow({10, 1, 0}), 1);

    // Yes (single-key chunks coincide).
    // _[]___
    // _[]___
    auto chunkA11 = CreateChunk(BuildRow({11, 1, 4}), BuildRow({11, 1, 4}), 0);
    auto chunkB11 = CreateChunk(BuildRow({11, 1, 4}), BuildRow({11, 1, 4}), 1);

    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // Cases with read limits, keys of length exactly PrimaryPrefixLength.
    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    // Yes/No (non-trivial lower limit).
    // NB: chunkB12 may not be teleported because it has non-trivial read limits.
    // _[==]_____
    // ___===[==]
    auto chunkA12 = CreateChunk(BuildRow({12, 1, 0}), BuildRow({12, 1, 4}), 0);
    auto chunkB12 = CreateChunk(BuildRow({12, 1, 2}), BuildRow({12, 1, 8}), 1, 1 * KB, BuildRow({12, 1, 5}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB12);

    // Yes/No (non-trivial lower limit coinciding with max key).
    // _[==]_____
    // ___=[====]
    auto chunkA13 = CreateChunk(BuildRow({13, 1, 0}), BuildRow({13, 1, 4}), 0);
    auto chunkB13 = CreateChunk(BuildRow({13, 1, 2}), BuildRow({13, 1, 8}), 1, 1 * KB, BuildRow({13, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB13);

    // No/No (they partially intersect with each other).
    // _[===]____
    // ___=[===]_
    auto chunkA14 = CreateChunk(BuildRow({14, 1, 0}), BuildRow({14, 1, 4}), 0);
    auto chunkB14 = CreateChunk(BuildRow({14, 1, 2}), BuildRow({14, 1, 8}), 1, 1 * KB, BuildRow({14, 1, 3}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA14);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB14);

    // Yes/No (second one is de-facto single-key coinciding with the max-key of the first one).
    // _[===]____
    // ___=[]____
    auto chunkA15 = CreateChunk(BuildRow({15, 1, 0}), BuildRow({15, 1, 4}), 0);
    auto chunkB15 = CreateChunk(BuildRow({15, 1, 2}), BuildRow({15, 1, 4}), 1, 1 * KB, BuildRow({15, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB15);

    // Yes/No (non-trivial upper limit).
    // ______[===]_
    // _[==)===____
    auto chunkA16 = CreateChunk(BuildRow({16, 1, 4}), BuildRow({16, 1, 8}), 0);
    auto chunkB16 = CreateChunk(BuildRow({16, 1, 0}), BuildRow({16, 1, 6}), 1, 1 * KB, TKey(), BuildRow({16, 1, 3}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB16);

    // Yes/No (non-trivial upper limit).
    // ____[===]_
    // _[==)===__
    auto chunkA17 = CreateChunk(BuildRow({17, 1, 4}), BuildRow({17, 1, 8}), 0);
    auto chunkB17 = CreateChunk(BuildRow({17, 1, 0}), BuildRow({17, 1, 6}), 1, 1 * KB, TKey(), BuildRow({17, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB17);

    // No/No (non-trivial upper limit).
    // ____[===]_
    // _[====)=__
    auto chunkA18 = CreateChunk(BuildRow({18, 1, 4}), BuildRow({18, 1, 8}), 0);
    auto chunkB18 = CreateChunk(BuildRow({18, 1, 0}), BuildRow({18, 1, 6}), 1, 1 * KB, TKey(), BuildRow({18, 1, 5}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA18);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB18);

    // Yes/No (first one is single-key touching the second one with non-trivial lower limit).
    // __[]_______
    // ===[==)____
    auto chunkA19 = CreateChunk(BuildRow({19, 1, 4}), BuildRow({19, 1, 4}), 0);
    auto chunkB19 = CreateChunk(BuildRow({19, 1, 0}), BuildRow({19, 1, 6}), 1, 1 * KB, BuildRow({19, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB19);

    // Yes/No (first one is single-key touching the second one with non-trivial upper limit).
    // _____[]___
    // ___[==)===_
    auto chunkA20 = CreateChunk(BuildRow({20, 1, 4}), BuildRow({20, 1, 4}), 0);
    auto chunkB20 = CreateChunk(BuildRow({20, 1, 0}), BuildRow({20, 1, 6}), 1, 1 * KB, TKey(), BuildRow({20, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB20);

    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // Cases with and without read limits, keys longer than PrimaryPrefixLength.
    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    // Yes (chunks have longer keys than the PrimaryPrefixLength).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6   <- 2-nd (0-based) component is shown here
    // ___________[======]____________________________________
    // ____________________________[======]___________________
    auto chunkA21 = CreateChunk(BuildRow({21, 1, 1, 42}), BuildRow({21, 1, 2, 42}), 0);
    auto chunkB21 = CreateChunk(BuildRow({21, 1, 3, 42}), BuildRow({21, 1, 4, 42}), 1);

    // Yes (after shortening chunks will be touching).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[========]__________________________________
    // __________________[========]___________________________

    auto chunkA22 = CreateChunk(BuildRow({22, 1, 1, 40}), BuildRow({22, 1, 2, 44}), 0);
    auto chunkB22 = CreateChunk(BuildRow({22, 1, 2, 42}), BuildRow({22, 1, 3, 46}), 1);

    // No (after shortening chunks will be intersecting).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // __________________[================]___________________

    auto chunkA23 = CreateChunk(BuildRow({23, 1, 1, 42}), BuildRow({23, 1, 3, 42}), 0);
    auto chunkB23 = CreateChunk(BuildRow({23, 1, 2, 42}), BuildRow({23, 1, 4, 46}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA23);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB23);

    // Yes (after shortening one of the chunks will be single-key touching the max-key).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // __________________________[==]_________________________

    auto chunkA24 = CreateChunk(BuildRow({24, 1, 1, 42}), BuildRow({24, 1, 3, 42}), 0);
    auto chunkB24 = CreateChunk(BuildRow({24, 1, 3, 42}), BuildRow({24, 1, 4, 42}), 1);

    // Yes (after shortening one of the chunks will be single-key touching the min-key).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // _________[==]__________________________________________

    auto chunkA25 = CreateChunk(BuildRow({25, 1, 1, 42}), BuildRow({25, 1, 3, 42}), 0);
    auto chunkB25 = CreateChunk(BuildRow({25, 1, 1, 42}), BuildRow({25, 1, 1, 42}), 1);

    // Yes (after shortening both chunks will be coinciding and single-key).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ________________[==]___________________________________
    // _________________[===]_________________________________

    auto chunkA26 = CreateChunk(BuildRow({26, 1, 2, 42}), BuildRow({26, 1, 2, 42}), 0);
    auto chunkB26 = CreateChunk(BuildRow({26, 1, 2, 42}), BuildRow({26, 1, 2, 42}), 1);

    // Yes/No (after shortening one of the chunks will be single-key touching the min-key with non-trivial read limits).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // _________[==)======____________________________________

    auto chunkA27 = CreateChunk(BuildRow({27, 1, 1, 42}), BuildRow({27, 1, 3, 42}), 0);
    auto chunkB27 = CreateChunk(BuildRow({27, 1, 1, 42}), BuildRow({27, 1, 2, 42}), 1, 1 * KB, TKey(), BuildRow({27, 1, 1, 46}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB27);

    // No/No (after shortening chunks will be intersecting).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // _________[========)======______________________________

    auto chunkA28 = CreateChunk(BuildRow({28, 1, 1, 42}), BuildRow({28, 1, 3, 42}), 0);
    auto chunkB28 = CreateChunk(BuildRow({28, 1, 1, 42}), BuildRow({28, 1, 3, 42}), 1, 1 * KB, TKey(), BuildRow({28, 1, 2, 46}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA28);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB28);

    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // Cases with and without read limits, read limits shorter than PrimaryPrefixLength.
    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    // No/No (after shortening one chunks will be intersecting).
    //                0              ||              1              ||              2               <- 1-st component is shown here
    //   ...  |  0,0  |  0,1  |  ... || ...  |  1,0  |  1,1  |  ... || ...  |  2,0  |  2,1  |  ...  <- 1-st and 2-nd component are shown here
    // _____________________==========[======================]______________________________________
    // _______________[==============================]______________________________________________

    auto chunkA29 = CreateChunk(BuildRow({29, 0, 1}), BuildRow({29, 1, 1}), 0, 1 * KB, BuildRow({29, 1}));
    auto chunkB29 = CreateChunk(BuildRow({29, 0, 1}), BuildRow({29, 1, 0}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA29);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB29);

    // No/Yes (after shortening one chunks will be intersecting).
    //                0              ||              1              ||              2               <- 1-st component is shown here
    //   ...  |  0,0  |  0,1  |  ... || ...  |  1,0  |  1,1  |  ... || ...  |  2,0  |  2,1  |  ...  <- 1-st and 2-nd component are shown here
    // ______________________________________[========================)==============________________
    // _____________________________________________________________________[=======]________________

    auto chunkA30 = CreateChunk(BuildRow({30, 1, 0}), BuildRow({30, 2, 1}), 0, 1 * KB, TKey(), BuildRow({30, 2}));
    auto chunkB30 = CreateChunk(BuildRow({30, 2, 0}), BuildRow({30, 2, 0}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA30);

    CreateChunkPool();

    for (const auto& unversionedInputChunk : CreatedUnversionedPrimaryChunks_) {
        AddChunk(unversionedInputChunk);
    }

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, UnorderedElementsAreArray({
        chunkA1, chunkB1,
        chunkA2, chunkB2,
        chunkA9, chunkB9,
        chunkA10, chunkB10,
        chunkA11, chunkB11,
        chunkA12,
        chunkA13,
        chunkA15,
        chunkA16,
        chunkA17,
        chunkA19,
        chunkA20,
        chunkA21, chunkB21,
        chunkA22, chunkB22,
        chunkA24, chunkB24,
        chunkA25, chunkB25,
        chunkA26, chunkB26,
        chunkA27,
        chunkB30,
    }));

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, SortedMergeSimple)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false} /* isForeign */,
        {true, true, true} /* isTeleportable */,
        {false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({3}), BuildRow({3}), 0);
    auto chunkB = CreateChunk(BuildRow({2}), BuildRow({15}), 1);
    auto chunkC = CreateChunk(BuildRow({1}), BuildRow({3}), 2);
    auto chunkBSlices = SliceUnversionedChunk(chunkB, {BuildRow({3}), BuildRow({6})}, {KB / 4, KB / 2, KB / 4});
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);
    CurrentMock().RegisterSliceableUnversionedChunk(chunkB, chunkBSlices);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());
    EXPECT_EQ(1, stripeLists.size());

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, SortedMergeWithPersistBeforeFinish)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false} /* isForeign */,
        {true, true, true} /* isTeleportable */,
        {false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    InitJobConstraints();

    auto chunkA = CreateChunk(BuildRow({3}), BuildRow({3}), 0);
    auto chunkB = CreateChunk(BuildRow({2}), BuildRow({15}), 1);
    auto chunkC = CreateChunk(BuildRow({1}), BuildRow({3}), 2);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);

    PersistAndRestore();

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());
    EXPECT_EQ(1, stripeLists.size());
    EXPECT_EQ(3, stripeLists.front()->Stripes.size());
}

TEST_F(TSortedChunkPoolTest, SortedMergeSimpleWithGenericInputStreamDirectory)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false} /* isForeign */,
        {true, true, true} /* isTeleportable */,
        {false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({3}), BuildRow({3}), 0);
    auto chunkB = CreateChunk(BuildRow({2}), BuildRow({15}), 1);
    auto chunkC = CreateChunk(BuildRow({1}), BuildRow({3}), 2);
    auto chunkBSlices = SliceUnversionedChunk(chunkB, {BuildRow({3}), BuildRow({6})}, {KB / 4, KB / 2, KB / 4});
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);
    CurrentMock().RegisterSliceableUnversionedChunk(chunkB, chunkBSlices);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC);

    CreateChunkPool(true /* useGenericInputStreamDirectory */);

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());
    EXPECT_EQ(1, stripeLists.size());

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, SlicingManiacs)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false} /* isForeign */,
        {true, true} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    MaxDataSlicesPerJob_ = 3;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({1}), BuildRow({5}), 0);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);
    std::vector<TInputChunkPtr> maniacChunksB;
    for (int i = 0; i < 100; i++) {
        maniacChunksB.emplace_back(CreateChunk(BuildRow({3}), BuildRow({3}), 1));
        CurrentMock().RegisterTriviallySliceableUnversionedChunk(maniacChunksB.back());
    }

    CreateChunkPool();

    AddChunk(chunkA);
    for (const auto& chunkB : maniacChunksB) {
        AddChunk(chunkB);
    }

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());

    // In an ideal world we would've split all this stuff into (100 + 2) / 3 == 34 jobs.
    // Since our implementation is not perfect, we ensure that there is at least 34 jobs
    // and at most 100 / 2 + 2
    EXPECT_LE((100 + 2) / 3, stripeLists.size());
    EXPECT_LE(stripeLists.size(), 100 / 2 + 2);

    CheckEverything(stripeLists, teleportChunks);
}

////////////////////////////////////////////////////////////////////////////////

TEST_F(TSortedChunkPoolTest, SortedReduceSimple)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = true;
    InitTables(
        {false, false} /* isForeign */,
        {true, true} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.MinTeleportChunkSize = 0;
    MaxDataSlicesPerJob_ = 1;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({0, 1}), BuildRow({2, 2}), 0);
    auto chunkB = CreateChunk(BuildRow({2, 6}), BuildRow({5, 8}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());
    ASSERT_EQ(2, stripeLists.size());
    // At least one stripe list should be responsible for the shared key {2}.
    EXPECT_TRUE(
        (stripeLists[0]->Stripes[0] && stripeLists[0]->Stripes[1]) ||
        (stripeLists[0]->Stripes[1] && stripeLists[1]->Stripes[1]));

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, SortedReduceManiacs)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = true;
    InitTables(
        {false, false} /* isForeign */,
        {true, true} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({0, 1}), BuildRow({2, 9}), 0);
    auto chunkB = CreateChunk(BuildRow({2, 6}), BuildRow({2, 8}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, SortedReduceAllKindOfTeleports)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = true;
    InitTables(
        {false, false} /* isForeign */,
        {true, true} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 3;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();
    PrepareNewMock();

    // +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // Simple cases no read limits, keys of length exactly PrimaryPrefixLength.
    // +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    // Yes.
    // [==]_____
    // _____[==]
    auto chunkA1 = CreateChunk(BuildRow({1, 1, 0}), BuildRow({1, 1, 2}), 0);
    auto chunkB1 = CreateChunk(BuildRow({1, 1, 3}), BuildRow({1, 1, 5}), 1);

    // No (they share only one boundary key).
    // [==]___
    // ___[==]
    auto chunkA2 = CreateChunk(BuildRow({2, 1, 0}), BuildRow({2, 1, 2}), 0);
    auto chunkB2 = CreateChunk(BuildRow({2, 1, 2}), BuildRow({2, 1, 4}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA2);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB2);

    // No (they partially intersect).
    // [===]__
    // __[===]
    auto chunkA3 = CreateChunk(BuildRow({3, 1, 0}), BuildRow({3, 1, 2}), 0);
    auto chunkB3 = CreateChunk(BuildRow({3, 1, 1}), BuildRow({3, 1, 4}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA3);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB3);

    // No (one contained in another).
    // [====]__
    // _[==]___
    auto chunkA4 = CreateChunk(BuildRow({4, 1, 0}), BuildRow({4, 1, 3}), 0);
    auto chunkB4 = CreateChunk(BuildRow({4, 1, 1}), BuildRow({4, 1, 2}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA4);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB4);

    // No (single_key one contained in another).
    // [====]__
    // __[]____
    auto chunkA5 = CreateChunk(BuildRow({5, 1, 0}), BuildRow({5, 1, 3}), 0);
    auto chunkB5 = CreateChunk(BuildRow({5, 1, 1}), BuildRow({5, 1, 1}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA5);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB5);

    // No (they coincide).
    // [===]__
    // [===]__
    auto chunkA6 = CreateChunk(BuildRow({6, 1, 0}), BuildRow({6, 1, 3}), 0);
    auto chunkB6 = CreateChunk(BuildRow({6, 1, 0}), BuildRow({6, 1, 3}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA6);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB6);

    // No (one covers another).
    // [===]__
    // [====]_
    auto chunkA7 = CreateChunk(BuildRow({7, 1, 0}), BuildRow({7, 1, 3}), 0);
    auto chunkB7 = CreateChunk(BuildRow({7, 1, 0}), BuildRow({7, 1, 4}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA7);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB7);

    // No (one covers another).
    // _[===]__
    // [====]__
    auto chunkA8 = CreateChunk(BuildRow({8, 1, 0}), BuildRow({8, 1, 4}), 0);
    auto chunkB8 = CreateChunk(BuildRow({8, 1, 1}), BuildRow({8, 1, 4}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA8);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB8);

    // No (single-key is located exactly at the max boundary key of another).
    // [===]__
    // ___[]__
    auto chunkA9 = CreateChunk(BuildRow({9, 1, 0}), BuildRow({9, 1, 4}), 0);
    auto chunkB9 = CreateChunk(BuildRow({9, 1, 4}), BuildRow({9, 1, 4}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA9);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB9);

    // No (single-key is located exactly at the min boundary key of another).
    // [===]__
    // []_____
    auto chunkA10 = CreateChunk(BuildRow({10, 1, 0}), BuildRow({10, 1, 4}), 0);
    auto chunkB10 = CreateChunk(BuildRow({10, 1, 0}), BuildRow({10, 1, 0}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA10);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB10);

    // No (single-key chunks coincide).
    // _[]___
    // _[]___
    auto chunkA11 = CreateChunk(BuildRow({11, 1, 4}), BuildRow({11, 1, 4}), 0);
    auto chunkB11 = CreateChunk(BuildRow({11, 1, 4}), BuildRow({11, 1, 4}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA11);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB11);

    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // Cases with read limits, keys of length exactly PrimaryPrefixLength.
    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    // Yes/No (non-trivial lower limit).
    // NB: chunkB12 may not be teleported because it has non-trivial read limits.
    // _[==]_____
    // ___===[==]
    auto chunkA12 = CreateChunk(BuildRow({12, 1, 0}), BuildRow({12, 1, 4}), 0);
    auto chunkB12 = CreateChunk(BuildRow({12, 1, 2}), BuildRow({12, 1, 8}), 1, 1 * KB, BuildRow({12, 1, 5}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB12);

    // No/No (non-trivial lower limit coinciding with max key).
    // _[==]_____
    // ___=[====]
    auto chunkA13 = CreateChunk(BuildRow({13, 1, 0}), BuildRow({13, 1, 4}), 0);
    auto chunkB13 = CreateChunk(BuildRow({13, 1, 2}), BuildRow({13, 1, 8}), 1, 1 * KB, BuildRow({13, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA13);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB13);

    // No/No (they partially intersect with each other).
    // _[===]____
    // ___=[===]_
    auto chunkA14 = CreateChunk(BuildRow({14, 1, 0}), BuildRow({14, 1, 4}), 0);
    auto chunkB14 = CreateChunk(BuildRow({14, 1, 2}), BuildRow({14, 1, 8}), 1, 1 * KB, BuildRow({14, 1, 3}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA14);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB14);

    // No/No (second one is de-facto single-key coinciding with the max-key of the first one).
    // _[===]____
    // ___=[]____
    auto chunkA15 = CreateChunk(BuildRow({15, 1, 0}), BuildRow({15, 1, 4}), 0);
    auto chunkB15 = CreateChunk(BuildRow({15, 1, 2}), BuildRow({15, 1, 4}), 1, 1 * KB, BuildRow({15, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA15);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB15);

    // Yes/No (non-trivial upper limit).
    // ______[===]_
    // _[==)===____
    auto chunkA16 = CreateChunk(BuildRow({16, 1, 4}), BuildRow({16, 1, 8}), 0);
    auto chunkB16 = CreateChunk(BuildRow({16, 1, 0}), BuildRow({16, 1, 6}), 1, 1 * KB, TKey(), BuildRow({16, 1, 3}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB16);

    // Yes/No (non-trivial upper limit).
    // ____[===]_
    // _[==)===__
    auto chunkA17 = CreateChunk(BuildRow({17, 1, 4}), BuildRow({17, 1, 8}), 0);
    auto chunkB17 = CreateChunk(BuildRow({17, 1, 0}), BuildRow({17, 1, 6}), 1, 1 * KB, TKey(), BuildRow({17, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB17);

    // No/No (non-trivial upper limit).
    // ____[===]_
    // _[====)=__
    auto chunkA18 = CreateChunk(BuildRow({18, 1, 4}), BuildRow({18, 1, 8}), 0);
    auto chunkB18 = CreateChunk(BuildRow({18, 1, 0}), BuildRow({18, 1, 6}), 1, 1 * KB, TKey(), BuildRow({18, 1, 5}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA18);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB18);

    // No/No (first one is single-key touching the second one with non-trivial lower limit).
    // __[]_______
    // ===[==)____
    auto chunkA19 = CreateChunk(BuildRow({19, 1, 4}), BuildRow({19, 1, 4}), 0);
    auto chunkB19 = CreateChunk(BuildRow({19, 1, 0}), BuildRow({19, 1, 6}), 1, 1 * KB, BuildRow({19, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA19);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB19);

    // Yes/No (first one is single-key touching the second one with non-trivial upper limit).
    // _____[]___
    // ___[==)===_
    auto chunkA20 = CreateChunk(BuildRow({20, 1, 4}), BuildRow({20, 1, 4}), 0);
    auto chunkB20 = CreateChunk(BuildRow({20, 1, 0}), BuildRow({20, 1, 6}), 1, 1 * KB, TKey(), BuildRow({20, 1, 4}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB20);

    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // Cases with and without read limits, keys longer than PrimaryPrefixLength.
    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    // Yes (chunks have longer keys than the PrimaryPrefixLength).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6   <- 2-nd (0-based) component is shown here
    // ___________[======]____________________________________
    // ____________________________[======]___________________
    auto chunkA21 = CreateChunk(BuildRow({21, 1, 1, 42}), BuildRow({21, 1, 2, 42}), 0);
    auto chunkB21 = CreateChunk(BuildRow({21, 1, 3, 42}), BuildRow({21, 1, 4, 42}), 1);

    // No (after shortening chunks will be touching).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[========]__________________________________
    // __________________[========]___________________________

    auto chunkA22 = CreateChunk(BuildRow({22, 1, 1, 40}), BuildRow({22, 1, 2, 44}), 0);
    auto chunkB22 = CreateChunk(BuildRow({22, 1, 2, 42}), BuildRow({22, 1, 3, 46}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA22);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB22);

    // No (after shortening chunks will be intersecting).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // __________________[================]___________________

    auto chunkA23 = CreateChunk(BuildRow({23, 1, 1, 42}), BuildRow({23, 1, 3, 42}), 0);
    auto chunkB23 = CreateChunk(BuildRow({23, 1, 2, 42}), BuildRow({23, 1, 4, 46}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA23);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB23);

    // No (after shortening one of the chunks will be single-key touching the max-key).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // __________________________[==]_________________________

    auto chunkA24 = CreateChunk(BuildRow({24, 1, 1, 42}), BuildRow({24, 1, 3, 42}), 0);
    auto chunkB24 = CreateChunk(BuildRow({24, 1, 3, 42}), BuildRow({24, 1, 4, 42}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA24);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB24);

    // No (after shortening one of the chunks will be single-key touching the min-key).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // _________[==]__________________________________________

    auto chunkA25 = CreateChunk(BuildRow({25, 1, 1, 42}), BuildRow({25, 1, 3, 42}), 0);
    auto chunkB25 = CreateChunk(BuildRow({25, 1, 1, 42}), BuildRow({25, 1, 1, 42}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA25);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB25);

    // No (after shortening both chunks will be coinciding and single-key).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ________________[==]___________________________________
    // _________________[===]_________________________________

    auto chunkA26 = CreateChunk(BuildRow({26, 1, 2, 42}), BuildRow({26, 1, 2, 42}), 0);
    auto chunkB26 = CreateChunk(BuildRow({26, 1, 2, 42}), BuildRow({26, 1, 2, 42}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA26);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB26);

    // No/No (after shortening one of the chunks will be single-key touching the min-key with non-trivial read limits).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // _________[==)======____________________________________

    auto chunkA27 = CreateChunk(BuildRow({27, 1, 1, 42}), BuildRow({27, 1, 3, 42}), 0);
    auto chunkB27 = CreateChunk(BuildRow({27, 1, 1, 42}), BuildRow({27, 1, 2, 42}), 1, 1 * KB, TKey(), BuildRow({27, 1, 1, 46}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA27);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB27);

    // No/No (after shortening chunks will be intersecting).
    //    0   |   1   |   2   |   3   |   4   |   5   |   6
    // ___________[===============]___________________________
    // _________[========)======______________________________

    auto chunkA28 = CreateChunk(BuildRow({28, 1, 1, 42}), BuildRow({28, 1, 3, 42}), 0);
    auto chunkB28 = CreateChunk(BuildRow({28, 1, 1, 42}), BuildRow({28, 1, 3, 42}), 1, 1 * KB, TKey(), BuildRow({28, 1, 2, 46}));
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA28);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB28);

    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
    // Cases with and without read limits, read limits shorter than PrimaryPrefixLength.
    // ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

    // No/No (after shortening one chunks will be intersecting).
    //                0              ||              1              ||              2               <- 1-st component is shown here
    //   ...  |  0,0  |  0,1  |  ... || ...  |  1,0  |  1,1  |  ... || ...  |  2,0  |  2,1  |  ...  <- 1-st and 2-nd component are shown here
    // _____________________==========[======================]______________________________________
    // _______________[==============================]______________________________________________

    auto chunkA29 = CreateChunk(BuildRow({29, 0, 1}), BuildRow({29, 1, 1}), 0, 1 * KB, BuildRow({29, 1}));
    auto chunkB29 = CreateChunk(BuildRow({29, 0, 1}), BuildRow({29, 1, 0}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA29);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB29);

    // No/No (after shortening one chunks will be intersecting).
    //                0              ||              1              ||              2               <- 1-st component is shown here
    //   ...  |  0,0  |  0,1  |  ... || ...  |  1,0  |  1,1  |  ... || ...  |  2,0  |  2,1  |  ...  <- 1-st and 2-nd component are shown here
    // ______________________________________[========================)==============________________
    // _____________________________________________________________________[=======]________________

    auto chunkA30 = CreateChunk(BuildRow({30, 1, 0}), BuildRow({30, 2, 1}), 0, 1 * KB, TKey(), BuildRow({30, 2}));
    auto chunkB30 = CreateChunk(BuildRow({30, 2, 0}), BuildRow({30, 2, 0}), 1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA30);

    CreateChunkPool();

    for (const auto& chunk : CreatedUnversionedPrimaryChunks_) {
        AddChunk(chunk);
    }

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, UnorderedElementsAreArray({
        chunkA1, chunkB1,
        chunkA12,
        chunkA16,
        chunkA17,
        chunkA20,
        chunkA21, chunkB21,
        chunkB30,
    }));

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, SortedReduceWithJoin)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = true;
    InitTables(
        {true, true, false, false} /* isForeign */,
        {false, false, false, false} /* isTeleportable */,
        {false, false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 2;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({1, 21}), BuildRow({4, 24}), 0);
    auto chunkB = CreateChunk(BuildRow({2, 62}), BuildRow({4, 64}), 1);
    auto chunkC = CreateChunk(BuildRow({1, 101, 11}), BuildRow({4, 402, 18}), 2);
    auto chunkD = CreateChunk(BuildRow({1, 102, 42}), BuildRow({4, 402, 48}), 3);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkD);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);
    AddChunk(chunkD);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, JoinReduce)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {true, true, false, false} /* isForeign */,
        {false, false, false, false} /* isTeleportable */,
        {false, false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 2;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({1, 21}), BuildRow({4, 24}), 0);
    auto chunkB = CreateChunk(BuildRow({2, 62}), BuildRow({4, 64}), 1);
    auto chunkC = CreateChunk(BuildRow({1, 101, 11}), BuildRow({4, 402, 18}), 2);
    auto chunkD = CreateChunk(BuildRow({1, 102, 42}), BuildRow({4, 402, 48}), 3);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkD);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);
    AddChunk(chunkD);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, ResumeSuspendMappingTest)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false} /* isForeign */,
        {false, false} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    MaxDataSlicesPerJob_ = 1;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkAv1 = CreateChunk(BuildRow({5}), BuildRow({15}), 0);
    auto chunkBv1 = CreateChunk(BuildRow({0}), BuildRow({20}), 1, 1 * KB, BuildRow({10}));
    auto chunkAv1Slices = SliceUnversionedChunk(chunkAv1, {BuildRow({8}), BuildRow({12})}, {KB / 4, KB / 2, KB / 4});
    CurrentMock().RegisterSliceableUnversionedChunk(chunkAv1, chunkAv1Slices);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkBv1);

    CreateChunkPool();

    int cookieA = AddChunk(chunkAv1);
    int cookieB = AddChunk(chunkBv1);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    CheckStripeListsContainOnlyActiveChunks();

    SuspendChunk(cookieA, chunkAv1);
    auto chunkAv2 = CopyChunk(chunkAv1);
    ResumeChunk(cookieA, chunkAv2);

    CheckStripeListsContainOnlyActiveChunks();

    SuspendChunk(cookieB, chunkBv1);
    auto chunkBv2 = CopyChunk(chunkBv1);
    ResumeChunk(cookieB, chunkBv2);

    EXPECT_TRUE(InvalidationErrors_.empty());
}

TEST_F(TSortedChunkPoolTest, ResumeSuspendInvalidationTest1)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false} /* isForeign */,
        {true, true} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();

    auto chunkAv1 = CreateChunk(BuildRow({5}), BuildRow({15}), 0);
    auto chunkBv1 = CreateChunk(BuildRow({0}), BuildRow({20}), 1);
    auto chunkAv1Slices = SliceUnversionedChunk(chunkAv1, {BuildRow({8}), BuildRow({12})}, {KB / 4, KB / 2, KB / 4});
    auto chunkAv2 = CopyChunk(chunkAv1);
    chunkAv2->BoundaryKeys()->MinKey = TOwningKey(BuildRow({25}));
    chunkAv2->BoundaryKeys()->MaxKey = TOwningKey(BuildRow({30}));
    auto chunkBv2 = CopyChunk(chunkBv1);
    chunkBv2->BoundaryKeys()->MinKey = TOwningKey(BuildRow({25}));
    chunkBv2->BoundaryKeys()->MaxKey = TOwningKey(BuildRow({30}));

    PrepareNewMock();
    CurrentMock().RegisterSliceableUnversionedChunk(chunkAv1, chunkAv1Slices);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkBv1);
    PrepareNewMock();
    PrepareNewMock();
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkAv2);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkBv2);

    CreateChunkPool();

    int cookieA = AddChunk(chunkAv1);
    int cookieB = AddChunk(chunkBv1);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    CheckStripeListsContainOnlyActiveChunks();

    ExtractOutputCookiesWhilePossible();
    ChunkPool_->Completed(*OutputCookies_.begin(), TCompletedJobSummary());

    SuspendChunk(cookieB, chunkBv1);
    ResumeChunk(cookieB, chunkBv2);

    EXPECT_EQ(InvalidationErrors_.size(), 1);

    OutputCookies_.clear();
    ExtractOutputCookiesWhilePossible();
    EXPECT_TRUE(OutputCookies_.empty());
    EXPECT_EQ(ChunkPool_->GetTeleportChunks(), (std::vector<TInputChunkPtr>{chunkAv1, chunkBv2}));

    SuspendChunk(cookieA, chunkAv1);
    ResumeChunk(cookieA, chunkAv2);

    EXPECT_EQ(InvalidationErrors_.size(), 2);
    OutputCookies_.clear();
    ExtractOutputCookiesWhilePossible();
    EXPECT_EQ(OutputCookies_.size(), 1);
    auto stripeLists = GetAllStripeLists();

    EXPECT_EQ(stripeLists.size(), 1);
    EXPECT_EQ(stripeLists[0]->Stripes.size(), 2);
    EXPECT_EQ(stripeLists[0]->Stripes[0]->DataSlices.size(), 1);
    EXPECT_EQ(stripeLists[0]->Stripes[0]->DataSlices[0]->GetSingleUnversionedChunkOrThrow(), chunkAv2);
    EXPECT_EQ(stripeLists[0]->Stripes[1]->DataSlices.size(), 1);
    EXPECT_EQ(stripeLists[0]->Stripes[1]->DataSlices[0]->GetSingleUnversionedChunkOrThrow(), chunkBv2);
}

TEST_F(TSortedChunkPoolTest, ResumeSuspendInvalidationTest2)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false} /* isForeign */,
        {true, true} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();

    auto chunkAv1 = CreateChunk(BuildRow({5}), BuildRow({15}), 0);
    auto chunkBv1 = CreateChunk(BuildRow({0}), BuildRow({20}), 1);
    auto chunkAv1Slices = SliceUnversionedChunk(chunkAv1, {BuildRow({8}), BuildRow({12})}, {KB / 4, KB / 2, KB / 4});
    auto chunkAv2 = CopyChunk(chunkAv1);
    chunkAv2->BoundaryKeys()->MinKey = TOwningKey(BuildRow({25}));
    chunkAv2->BoundaryKeys()->MaxKey = TOwningKey(BuildRow({30}));
    auto chunkBv2 = CopyChunk(chunkBv1);
    chunkBv2->BoundaryKeys()->MinKey = TOwningKey(BuildRow({25}));
    chunkBv2->BoundaryKeys()->MaxKey = TOwningKey(BuildRow({30}));

    CreateChunkPool();

    int cookieA = AddChunk(chunkAv1);
    int cookieB = AddChunk(chunkBv1);

    PersistAndRestore();

    ChunkPool_->Finish();

    PersistAndRestore();

    ExtractOutputCookiesWhilePossible();
    CheckStripeListsContainOnlyActiveChunks();

    PersistAndRestore();

    ExtractOutputCookiesWhilePossible();
    ChunkPool_->Completed(*OutputCookies_.begin(), TCompletedJobSummary());

    PersistAndRestore();

    SuspendChunk(cookieB, chunkBv1);

    PersistAndRestore();

    ResumeChunk(cookieB, chunkBv2);

    PersistAndRestore();

    EXPECT_EQ(InvalidationErrors_.size(), 1);

    OutputCookies_.clear();
    ExtractOutputCookiesWhilePossible();
    EXPECT_TRUE(OutputCookies_.empty());
    EXPECT_EQ(ChunkPool_->GetTeleportChunks().size(), 2);

    PersistAndRestore();

    SuspendChunk(cookieA, chunkAv1);

    PersistAndRestore();

    ResumeChunk(cookieA, chunkAv2);

    PersistAndRestore();

    EXPECT_EQ(InvalidationErrors_.size(), 2);
    OutputCookies_.clear();
    ExtractOutputCookiesWhilePossible();
    EXPECT_EQ(OutputCookies_.size(), 1);
    auto stripeLists = GetAllStripeLists();

    PersistAndRestore();

    EXPECT_EQ(stripeLists.size(), 1);
    EXPECT_EQ(stripeLists[0]->Stripes.size(), 2);
    EXPECT_EQ(stripeLists[0]->Stripes[0]->DataSlices.size(), 1);
    EXPECT_EQ(stripeLists[0]->Stripes[0]->DataSlices[0]->GetSingleUnversionedChunkOrThrow()->ChunkId(), chunkAv2->ChunkId());
    EXPECT_EQ(stripeLists[0]->Stripes[1]->DataSlices.size(), 1);
    EXPECT_EQ(stripeLists[0]->Stripes[1]->DataSlices[0]->GetSingleUnversionedChunkOrThrow()->ChunkId(), chunkBv2->ChunkId());
}

TEST_F(TSortedChunkPoolTest, ResumeSuspendInvalidationTest3)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false} /* isForeign */,
        {true, true} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.MinTeleportChunkSize = 0;
    InitJobConstraints();

    auto chunkAv1 = CreateChunk(BuildRow({5}), BuildRow({15}), 0);
    auto chunkBv1 = CreateChunk(BuildRow({0}), BuildRow({20}), 1);
    auto chunkAv1Slices = SliceUnversionedChunk(chunkAv1, {BuildRow({8}), BuildRow({12})}, {KB / 4, KB / 2, KB / 4});
    auto chunkAv2 = CopyChunk(chunkAv1);
    chunkAv2->BoundaryKeys()->MinKey = TOwningKey(BuildRow({25}));
    chunkAv2->BoundaryKeys()->MaxKey = TOwningKey(BuildRow({30}));
    auto chunkBv2 = CopyChunk(chunkBv1);
    chunkBv2->BoundaryKeys()->MinKey = TOwningKey(BuildRow({25}));
    chunkBv2->BoundaryKeys()->MaxKey = TOwningKey(BuildRow({30}));

    CreateChunkPool();

    int cookieA = AddChunk(chunkAv1);
    int cookieB = AddChunk(chunkBv1);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();

    SuspendChunk(cookieA, chunkAv1);

    PersistAndRestore();

    SuspendChunk(cookieB, chunkBv1);

    PersistAndRestore();

    ResumeChunk(cookieB, chunkBv2);
    ResumeChunk(cookieA, chunkAv1);

    auto invalidatedStripe = ChunkPool_->GetStripeList(*OutputCookies_.begin());
    EXPECT_EQ(invalidatedStripe->Stripes.size(), 0);

    PersistAndRestore();

    EXPECT_EQ(InvalidationErrors_.size(), 1);

    OutputCookies_.clear();
    ExtractOutputCookiesWhilePossible();
    EXPECT_TRUE(OutputCookies_.empty());
    EXPECT_EQ(ChunkPool_->GetTeleportChunks().size(), 2);
}

TEST_F(TSortedChunkPoolTest, ManiacIsSliced)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false} /* isForeign */,
        {false} /* isTeleportable */,
        {false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    MaxDataSlicesPerJob_ = 1;
    InputSliceDataSize_ = 10;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({1, 2}), BuildRow({1, 42}), 0);
    chunkA->SetRowCount(10000);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);

    CreateChunkPool();

    AddChunk(chunkA);

    ChunkPool_->Finish();
    EXPECT_GE(ChunkPool_->GetPendingJobCount(), 100 / 2);
}

TEST_F(TSortedChunkPoolTest, MaxTotalSliceCount)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false} /* isForeign */,
        {false, false, false} /* isTeleportable */,
        {false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.SortedJobOptions.MaxTotalSliceCount = 6;
    DataSizePerJob_ = 1;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({1}), BuildRow({3}), 0);
    auto chunkB = CreateChunk(BuildRow({1}), BuildRow({3}), 1);
    auto chunkC1 = CreateChunk(BuildRow({1}), BuildRow({1}), 2);
    auto chunkC2 = CreateChunk(BuildRow({2}), BuildRow({2}), 2);
    auto chunkC3 = CreateChunk(BuildRow({3}), BuildRow({3}), 2);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC1);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC2);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC3);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC1);
    AddChunk(chunkC2);
    AddChunk(chunkC3);

    EXPECT_THROW(ChunkPool_->Finish(), std::exception);
}

TEST_F(TSortedChunkPoolTest, TestJobInterruption)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false, true} /* isForeign */,
        {false, false, false, false} /* isTeleportable */,
        {false, false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({1}), BuildRow({20}), 0);
    auto chunkB = CreateChunk(BuildRow({2}), BuildRow({42}), 1);
    auto chunkC = CreateChunk(BuildRow({10}), BuildRow({12}), 2);
    auto chunkD = CreateChunk(BuildRow({1}), BuildRow({42}), 3);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkA);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);
    AddChunk(chunkD);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    ASSERT_EQ(stripeLists.size(), 1);
    ASSERT_EQ(ExtractedCookies_.size(), 1);
    const auto& stripeList = stripeLists[0];
    std::vector<TInputDataSlicePtr> unreadDataSlices = {
        CreateInputDataSlice(stripeList->Stripes[0]->DataSlices.front(), BuildRow({13})),
        CreateInputDataSlice(stripeList->Stripes[1]->DataSlices.front(), BuildRow({14})),
    };
    TCompletedJobSummary jobSummary;
    jobSummary.InterruptReason = EInterruptReason::Preemption;
    jobSummary.UnreadInputDataSlices = unreadDataSlices;
    ChunkPool_->Completed(ExtractedCookies_.front(), jobSummary);

    ExtractOutputCookiesWhilePossible();
    ASSERT_EQ(ExtractedCookies_.size(), 2);
    auto newStripeList = ChunkPool_->GetStripeList(ExtractedCookies_.back());
    ASSERT_EQ(newStripeList->Stripes.size(), 3);
    ASSERT_EQ(newStripeList->Stripes[0]->DataSlices.size(), 1);
    ASSERT_EQ(newStripeList->Stripes[0]->DataSlices.front()->LowerLimit().Key, BuildRow({13}));
    ASSERT_EQ(newStripeList->Stripes[1]->DataSlices.size(), 1);
    ASSERT_EQ(newStripeList->Stripes[1]->DataSlices.front()->LowerLimit().Key, BuildRow({14}));
    ASSERT_EQ(newStripeList->Stripes[2]->DataSlices.size(), 1);
}

TEST_F(TSortedChunkPoolTest, TestJobSplitSimple)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false} /* isForeign */,
        {false} /* isTeleportable */,
        {false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    DataSizePerJob_ = Inf64;
    InitJobConstraints();
    PrepareNewMock();

    const int chunkCount = 100;
    for (int index = 0; index < chunkCount; ++index) {
        auto chunk = CreateChunk(BuildRow({2 * index}), BuildRow({2 * index + 1}), 0);
        CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunk);
    }

    CreateChunkPool();

    for (const auto& chunk : CreatedUnversionedPrimaryChunks_) {
        AddChunk(chunk);
    }

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();

    auto stripeLists = GetAllStripeLists();
    TCompletedJobSummary jobSummary;
    jobSummary.InterruptReason = EInterruptReason::JobSplit;
    jobSummary.UnreadInputDataSlices = std::vector<TInputDataSlicePtr>(
        stripeLists[0]->Stripes[0]->DataSlices.begin(),
        stripeLists[0]->Stripes[0]->DataSlices.end());
    jobSummary.SplitJobCount = 10;
    ChunkPool_->Completed(*OutputCookies_.begin(), jobSummary);

    OutputCookies_.clear();

    ExtractOutputCookiesWhilePossible();
    stripeLists = GetAllStripeLists();
    ASSERT_LE(8, stripeLists.size());
    ASSERT_LE(stripeLists.size(), 12);
}

TEST_F(TSortedChunkPoolTest, TestJobSplitWithForeign)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, true} /* isForeign */,
        {false, false} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.SortedJobOptions.ForeignPrefixLength = 1;
    DataSizePerJob_ = Inf64;
    InitJobConstraints();
    PrepareNewMock();

    std::vector<TInputChunkPtr> allChunks;
    const int chunkCount = 100;
    for (int index = 0; index < chunkCount; ++index) {
        auto chunk = CreateChunk(BuildRow({2 * index}), BuildRow({2 * index + 1}), 0);
        CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunk);
        allChunks.emplace_back(std::move(chunk));
    }

    const int foreignChunkCount = 5;

    for (int index = 0; index < foreignChunkCount; ++index) {
        auto chunk = CreateChunk(BuildRow({index * 40}), BuildRow({index * 40 + 39}), 1);
        allChunks.emplace_back(std::move(chunk));
    }

    CreateChunkPool();

    for (const auto& chunk : allChunks) {
        AddChunk(std::move(chunk));
    }

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();

    auto stripeLists = GetAllStripeLists();
    TCompletedJobSummary jobSummary;
    jobSummary.InterruptReason = EInterruptReason::JobSplit;
    std::vector<TInputDataSlicePtr> unreadSlices;
    unreadSlices.insert(
        unreadSlices.end(),
        stripeLists[0]->Stripes[0]->DataSlices.begin(),
        stripeLists[0]->Stripes[0]->DataSlices.end());
    jobSummary.SplitJobCount = 10;
    jobSummary.UnreadInputDataSlices = std::move(unreadSlices);
    ChunkPool_->Completed(*OutputCookies_.begin(), jobSummary);

    OutputCookies_.clear();

    ExtractOutputCookiesWhilePossible();
    stripeLists = GetAllStripeLists();
    ASSERT_LE(8, stripeLists.size());
    ASSERT_LE(stripeLists.size(), 12);

    for (const auto& stripeList : stripeLists) {
        ASSERT_EQ(stripeList->Stripes.size(), 2);
        ASSERT_LE(stripeList->Stripes[1]->DataSlices.size(), 2);
    }
}

TEST_F(TSortedChunkPoolTest, TestJobSplitStripeSuspension)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, true} /* isForeign */,
        {false, false} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    Options_.SortedJobOptions.ForeignPrefixLength = 1;
    DataSizePerJob_ = Inf64;
    InitJobConstraints();
    PrepareNewMock();

    std::vector<TInputChunkPtr> allChunks;
    const int chunkCount = 100;
    for (int index = 0; index < chunkCount; ++index) {
        auto chunk = CreateChunk(BuildRow({2 * index}), BuildRow({2 * index + 1}), 0);
        CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunk);
        allChunks.emplace_back(std::move(chunk));
    }

    const int foreignChunkCount = 5;

    for (int index = 0; index < foreignChunkCount; ++index) {
        auto chunk = CreateChunk(BuildRow({index * 40}), BuildRow({index * 40 + 39}), 1);
        allChunks.emplace_back(std::move(chunk));
    }

    CreateChunkPool();

    for (const auto& chunk : allChunks) {
        AddChunk(std::move(chunk));
    }

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();

    auto stripeLists = GetAllStripeLists();
    TCompletedJobSummary jobSummary;
    jobSummary.InterruptReason = EInterruptReason::JobSplit;
    std::vector<TInputDataSlicePtr> unreadSlices;
    unreadSlices.insert(
        unreadSlices.end(),
        stripeLists[0]->Stripes[0]->DataSlices.begin(),
        stripeLists[0]->Stripes[0]->DataSlices.end());
    jobSummary.SplitJobCount = 10;
    jobSummary.UnreadInputDataSlices = std::move(unreadSlices);
    ChunkPool_->Completed(*OutputCookies_.begin(), jobSummary);

    OutputCookies_.clear();

    int pendingJobCount = ChunkPool_->GetPendingJobCount();
    ASSERT_LE(8, pendingJobCount);
    ASSERT_LE(pendingJobCount, 12);
    ChunkPool_->Suspend(0);
    ASSERT_EQ(ChunkPool_->GetPendingJobCount(), pendingJobCount - 1);
    for (int cookie = chunkCount; cookie < chunkCount + foreignChunkCount; ++cookie) {
        ChunkPool_->Suspend(cookie);
    }
    ASSERT_EQ(0, ChunkPool_->GetPendingJobCount());
}

TEST_F(TSortedChunkPoolTest, TestCorrectOrderInsideStripe)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false} /* isForeign */,
        {false} /* isTeleportable */,
        {false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    DataSizePerJob_ = Inf64;
    InitJobConstraints();
    PrepareNewMock();

    auto chunk = CreateChunk(BuildRow({10}), BuildRow({20}), 0);
    std::vector<TInputChunkSlicePtr> slices;
    for (int index = 0; index < 100; ++index) {
        slices.emplace_back(New<TInputChunkSlice>(chunk, 0 /* partIndex */, 10 * index, 10 * (index + 1), 1 * KB));
        slices.back()->LowerLimit().Key = BuildRow({10});
        slices.back()->UpperLimit().Key = BuildRow({20});
    }
    shuffle(slices.begin(), slices.end(), Gen_);

    CurrentMock().RegisterSliceableUnversionedChunk(chunk, slices);

    CreateChunkPool();

    AddChunk(chunk);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    ASSERT_EQ(ExtractedCookies_.size(), 1);
    auto stripeList = ChunkPool_->GetStripeList(ExtractedCookies_.back());
    ASSERT_EQ(stripeList->Stripes.size(), 1);
    const auto& stripe = stripeList->Stripes.front();
    ASSERT_EQ(stripe->DataSlices.size(), 100);
    for (int index = 0; index + 1 < stripe->DataSlices.size(); ++index) {
        ASSERT_EQ(*stripe->DataSlices[index]->UpperLimit().RowIndex, *stripe->DataSlices[index + 1]->LowerLimit().RowIndex);
    }
}

TEST_F(TSortedChunkPoolTest, TestTrickyCase)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false},
        {false},
        {false}
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    DataSizePerJob_ = 10 * KB;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({100}), BuildRow({100}), 0, 12 * KB);
    auto chunkB = CreateChunk(BuildRow({100}), BuildRow({200}), 0, 3 * KB);
    auto chunkASlices = SliceUnversionedChunk(chunkA, {BuildRow({100})}, {9 * KB, 3 * KB}, {500, 500});
    chunkASlices[1]->LowerLimit().Key = BuildRow({100});
    CurrentMock().RegisterSliceableUnversionedChunk(chunkA, chunkASlices);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();

    auto stripeLists = GetAllStripeLists();
    ASSERT_EQ(stripeLists.size(), 2);
    ASSERT_EQ(stripeLists[0]->Stripes.size(), 1);
    ASSERT_EQ(stripeLists[1]->Stripes.size(), 1);
    std::vector<TInputChunkPtr> chunkSequence;
    for (const auto& dataSlice : stripeLists[0]->Stripes[0]->DataSlices) {
        chunkSequence.push_back(dataSlice->GetSingleUnversionedChunkOrThrow());
    }
    for (const auto& dataSlice : stripeLists[1]->Stripes[0]->DataSlices) {
        chunkSequence.push_back(dataSlice->GetSingleUnversionedChunkOrThrow());
    }
    chunkSequence.erase(std::unique(chunkSequence.begin(), chunkSequence.end()), chunkSequence.end());
    ASSERT_EQ(chunkSequence.size(), 2);

    auto teleportChunks = ChunkPool_->GetTeleportChunks();

    CheckEverything(stripeLists, teleportChunks);
}


TEST_F(TSortedChunkPoolTest, TestTrickyCase2)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false},
        {false},
        {false}
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    DataSizePerJob_ = 10 * KB;
    InitJobConstraints();
    PrepareNewMock();

    auto chunkA = CreateChunk(BuildRow({100}), BuildRow({100}), 0, 12 * KB);
    auto chunkB = CreateChunk(BuildRow({100}), BuildRow({100}), 0, KB / 10);
    auto chunkC = CreateChunk(BuildRow({100}), BuildRow({200}), 0, 3 * KB);
    auto chunkASlices = SliceUnversionedChunk(chunkA, {BuildRow({100})}, {9 * KB, 3 * KB}, {500, 500});
    chunkASlices[1]->LowerLimit().Key = BuildRow({100});
    CurrentMock().RegisterSliceableUnversionedChunk(chunkA, chunkASlices);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkB);
    CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunkC);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();

    auto stripeLists = GetAllStripeLists();
    ASSERT_EQ(stripeLists.size(), 2);
    ASSERT_EQ(stripeLists[0]->Stripes.size(), 1);
    ASSERT_EQ(stripeLists[1]->Stripes.size(), 1);
    std::vector<TInputChunkPtr> chunkSequence;
    for (const auto& dataSlice : stripeLists[0]->Stripes[0]->DataSlices) {
        chunkSequence.push_back(dataSlice->GetSingleUnversionedChunkOrThrow());
    }
    for (const auto& dataSlice : stripeLists[1]->Stripes[0]->DataSlices) {
        chunkSequence.push_back(dataSlice->GetSingleUnversionedChunkOrThrow());
    }
    chunkSequence.erase(std::unique(chunkSequence.begin(), chunkSequence.end()), chunkSequence.end());
    ASSERT_EQ(chunkSequence.size(), 3);

    auto teleportChunks = ChunkPool_->GetTeleportChunks();

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, TestNoChunkSliceFetcher)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false} /* isForeign */,
        {true, true, true} /* isTeleportable */,
        {false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    InitJobConstraints();

    auto chunkA = CreateChunk(BuildRow({3}), BuildRow({3}), 0);
    auto chunkB = CreateChunk(BuildRow({2}), BuildRow({15}), 1);
    auto chunkC = CreateChunk(BuildRow({1}), BuildRow({3}), 2);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());
    EXPECT_EQ(1, stripeLists.size());

    CheckEverything(stripeLists, teleportChunks);
}

TEST_F(TSortedChunkPoolTest, TestStripeListStatisticsAreSet)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false, false} /* isForeign */,
        {true, true, true} /* isTeleportable */,
        {false, false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    InitJobConstraints();

    auto chunkA = CreateChunk(BuildRow({3}), BuildRow({3}), 0);
    auto chunkB = CreateChunk(BuildRow({2}), BuildRow({15}), 1);
    auto chunkC = CreateChunk(BuildRow({1}), BuildRow({3}), 2);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());
    EXPECT_EQ(1, stripeLists.size());

    EXPECT_GT(stripeLists[0]->TotalChunkCount, 0);
    EXPECT_GT(stripeLists[0]->TotalRowCount, 0);
    EXPECT_GT(stripeLists[0]->TotalDataSize, 0);
}

TEST_F(TSortedChunkPoolTest, TestSeveralSlicesInInputStripe)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false, false} /* isForeign */,
        {false, false} /* isTeleportable */,
        {false, false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    InitJobConstraints();

    auto chunkAA = CreateChunk(BuildRow({1}), BuildRow({1}), 0);
    auto chunkAB = CreateChunk(BuildRow({2}), BuildRow({2}), 0);
    auto chunkBA = CreateChunk(BuildRow({3}), BuildRow({3}), 1);
    auto chunkBB = CreateChunk(BuildRow({4}), BuildRow({4}), 1);

    CreateChunkPool();

    AddMultiChunkStripe({chunkAA, chunkAB});
    AddMultiChunkStripe({chunkBA, chunkBB});

    ChunkPool_->Finish();

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());
    EXPECT_EQ(1, stripeLists.size());
    EXPECT_EQ(2, stripeLists[0]->Stripes.size());
    EXPECT_EQ(2, stripeLists[0]->Stripes[0]->DataSlices.size());
    EXPECT_EQ(2, stripeLists[0]->Stripes[1]->DataSlices.size());
}

TEST_F(TSortedChunkPoolTest, SuspendFinishResumeTest)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false} /* isForeign */,
        {false} /* isTeleportable */,
        {false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    InitJobConstraints();

    auto chunkA = CreateChunk(BuildRow({1}), BuildRow({1}), 0);
    auto chunkB = CreateChunk(BuildRow({2}), BuildRow({2}), 0);
    auto chunkC = CreateChunk(BuildRow({3}), BuildRow({3}), 0);

    CreateChunkPool();

    AddChunk(chunkA);
    AddChunk(chunkB);
    AddChunk(chunkC);

    SuspendChunk(0, chunkA);
    SuspendChunk(2, chunkC);

    ChunkPool_->Finish();

    ResumeChunk(0, chunkA);
    ResumeChunk(2, chunkC);

    ExtractOutputCookiesWhilePossible();
    auto stripeLists = GetAllStripeLists();
    const auto& teleportChunks = ChunkPool_->GetTeleportChunks();

    EXPECT_THAT(teleportChunks, IsEmpty());
    EXPECT_EQ(1, stripeLists.size());
    EXPECT_EQ(1, stripeLists[0]->Stripes.size());
    EXPECT_EQ(3, stripeLists[0]->Stripes[0]->DataSlices.size());
}

////////////////////////////////////////////////////////////////////////////////

class TSortedChunkPoolTestRandomized
    : public WithParamInterface<int>
    , public TSortedChunkPoolTest
{
public:
    TSortedChunkPoolTestRandomized() = default;

    virtual void SetUp() override final
    {
        TSortedChunkPoolTest::SetUp();
        Gen_.seed(GetParam());
    }
};

static constexpr int NumberOfRepeats = 15;

TEST_P(TSortedChunkPoolTestRandomized, VariousOperationsWithPoolTest)
{
    Options_.SortedJobOptions.EnableKeyGuarantee = false;
    InitTables(
        {false} /* isForeign */,
        {false} /* isTeleportable */,
        {false} /* isVersioned */
    );
    Options_.SortedJobOptions.PrimaryPrefixLength = 1;
    DataSizePerJob_ = 1 * KB;
    InitJobConstraints();
    PrepareNewMock();

    const int chunkCount = 50;

    for (int index = 0; index < chunkCount; ++index) {
        auto chunk = CreateChunk(BuildRow({2 * index}), BuildRow({2 * index + 1}), 0);
        CurrentMock().RegisterTriviallySliceableUnversionedChunk(chunk);
    }

    CreateChunkPool();

    std::uniform_int_distribution<> dice(0, 99);

    auto chooseRandomElement = [&] (auto container) -> TNullable<typename decltype(container)::value_type> {
        if (container.empty()) {
            return Null;
        } else {
            auto it = container.begin();
            std::advance(it, std::uniform_int_distribution<>(0, container.size() - 1)(Gen_));
            return MakeNullable(*it);
        }
    };

    // All chunks from the IChunkPoolInput point of view.
    yhash_map<TChunkId, IChunkPoolInput::TCookie> chunkIdToInputCookie;
    yhash_set<TChunkId> suspendedChunks;
    yhash_set<TChunkId> resumedChunks;
    // All chunks from the IChunkPoolOutput point of view.
    yhash_map<TChunkId, IChunkPoolOutput::TCookie> chunkIdToOutputCookie;
    yhash_set<TChunkId> pendingChunks;
    yhash_set<TChunkId> startedChunks;
    yhash_set<TChunkId> completedChunks;
    yhash_map<TChunkId, TInputChunkPtr> chunkIdToChunk;

    for (const auto& chunk : CreatedUnversionedPrimaryChunks_) {
        const auto& chunkId = chunk->ChunkId();
        chunkIdToInputCookie[chunkId] = AddChunk(chunk);
        chunkIdToChunk[chunkId] = chunk;
        resumedChunks.insert(chunkId);
        pendingChunks.insert(chunkId);
    }

    ChunkPool_->Finish();

    ASSERT_EQ(ChunkPool_->GetPendingJobCount(), chunkCount);

    // Set this to true when debugging locally. It helps a lot to understand what happens.
    constexpr bool EnableDebugOutput = false;
    TOutputStream& Cdebug = EnableDebugOutput ? Cerr : Cnull;

    while (completedChunks.size() < chunkCount) {
        EXPECT_FALSE(ChunkPool_->IsCompleted());

        // 0..0 - pool is persisted and restored;
        // 1..29 - chunk is suspended;
        // 30..59 - chunk is resumed;
        // 60..69 - chunk is extracted;
        // 70..79 - chunk is completed;
        // 80..89 - chunk is failed;
        // 90..99 - chunk is aborted.
        int eventType = dice(Gen_);
        if (eventType <= 0) {
            Cdebug << "Persisting and restoring the pool" << Endl;
            PersistAndRestore();
        } else if (eventType <= 29) {
            if (auto randomElement = chooseRandomElement(resumedChunks)) {
                const auto& chunkId = *randomElement;
                Cdebug << Format("Suspending chunk %v", chunkId) << Endl;
                ASSERT_TRUE(resumedChunks.erase(chunkId));
                ASSERT_TRUE(suspendedChunks.insert(chunkId).second);
                auto inputCookie = chunkIdToInputCookie.at(chunkId);
                auto chunk = chunkIdToChunk.at(chunkId);
                SuspendChunk(inputCookie, chunk);
            }
        } else if (eventType <= 59) {
            if (auto randomElement = chooseRandomElement(suspendedChunks)) {
                const auto& chunkId = *randomElement;
                Cdebug << Format("Resuming chunk %v", chunkId) << Endl;
                ASSERT_TRUE(suspendedChunks.erase(chunkId));
                ASSERT_TRUE(resumedChunks.insert(chunkId).second);
                auto inputCookie = chunkIdToInputCookie.at(chunkId);
                auto chunk = chunkIdToChunk.at(chunkId);
                ResumeChunk(inputCookie, chunk);
            }
        } else if (eventType <= 69) {
            if (ChunkPool_->GetPendingJobCount()) {
                auto outputCookie = ExtractCookie(TNodeId(0));
                Cdebug << Format("Extracted cookie %v...", outputCookie);
                // TODO(max42): why the following line leads to the linkage error?
                // ASSERT_NE(outputCookie, IChunkPoolOutput::NullCookie);
                // error: undefined reference to 'NYT::NScheduler::IChunkPoolOutput::NullCookie'
                auto stripeList = ChunkPool_->GetStripeList(outputCookie);
                ASSERT_TRUE(stripeList->Stripes[0]);
                const auto& stripe = stripeList->Stripes[0];
                const auto& dataSlice = stripe->DataSlices.front();
                const auto& chunk = dataSlice->GetSingleUnversionedChunkOrThrow();
                const auto& chunkId = chunk->ChunkId();
                Cdebug << Format(" that corresponds to a chunk %v", chunkId) << Endl;
                ASSERT_TRUE(resumedChunks.has(chunkId));
                ASSERT_TRUE(!suspendedChunks.has(chunkId));
                ASSERT_TRUE(pendingChunks.erase(chunkId));
                ASSERT_TRUE(startedChunks.insert(chunkId).second);
                ASSERT_TRUE(chunkIdToOutputCookie.insert(std::make_pair(chunkId, outputCookie)).second);
            }
        } else if (eventType <= 79) {
            if (auto randomElement = chooseRandomElement(startedChunks)) {
                const auto& chunkId = *randomElement;
                Cdebug << Format("Completed chunk %v", chunkId) << Endl;
                auto outputCookie = chunkIdToOutputCookie.at(chunkId);
                ASSERT_TRUE(startedChunks.erase(chunkId));
                ASSERT_TRUE(chunkIdToOutputCookie.erase(chunkId));
                ASSERT_TRUE(completedChunks.insert(chunkId).second);
                ChunkPool_->Completed(outputCookie, TCompletedJobSummary());
            }
        } else if (eventType <= 89) {
            if (auto randomElement = chooseRandomElement(startedChunks)) {
                const auto& chunkId = *randomElement;
                Cdebug << Format("Aborted chunk %v", chunkId) << Endl;
                auto outputCookie = chunkIdToOutputCookie.at(chunkId);
                ASSERT_TRUE(startedChunks.erase(chunkId));
                ASSERT_TRUE(chunkIdToOutputCookie.erase(chunkId));
                ASSERT_TRUE(pendingChunks.insert(chunkId).second);
                ChunkPool_->Aborted(outputCookie);
            }
        } else { // if (eventType <= 99)
            if (auto randomElement = chooseRandomElement(startedChunks)) {
                const auto& chunkId = *randomElement;
                Cdebug << Format("Failed chunk %v", chunkId) << Endl;
                auto outputCookie = chunkIdToOutputCookie.at(chunkId);
                ASSERT_TRUE(startedChunks.erase(chunkId));
                ASSERT_TRUE(chunkIdToOutputCookie.erase(chunkId));
                ASSERT_TRUE(pendingChunks.insert(chunkId).second);
                ChunkPool_->Failed(outputCookie);
            }
        }
    }
    ASSERT_TRUE(ChunkPool_->IsCompleted());
    ASSERT_EQ(ChunkPool_->GetPendingJobCount(), 0);
    ASSERT_EQ(completedChunks.size(), chunkCount);
    ASSERT_EQ(pendingChunks.size(), 0);
    ASSERT_EQ(startedChunks.size(), 0);
    ASSERT_EQ(resumedChunks.size() + suspendedChunks.size(), chunkCount);
}

INSTANTIATE_TEST_CASE_P(VariousOperationsWithPoolInstantiation,
    TSortedChunkPoolTestRandomized,
    ::testing::Range(0, NumberOfRepeats));

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NScheduler
} // namespace NYT
