#include "job_manager.h"

namespace NYT {
namespace NChunkPools {

using namespace NControllerAgent;
using namespace NChunkClient;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

void TJobStub::AddDataSlice(const TInputDataSlicePtr& dataSlice, IChunkPoolInput::TCookie cookie, bool isPrimary)
{
    if (dataSlice->IsEmpty()) {
        return;
    }

    int streamIndex = dataSlice->InputStreamIndex;
    auto& stripe = GetStripe(streamIndex, isPrimary);
    stripe->DataSlices.emplace_back(dataSlice);
    InputCookies_.emplace_back(cookie);

    if (isPrimary) {
        if (LowerPrimaryKey_ > dataSlice->LowerLimit().Key) {
            LowerPrimaryKey_ = dataSlice->LowerLimit().Key;
        }
        if (UpperPrimaryKey_ < dataSlice->UpperLimit().Key) {
            UpperPrimaryKey_ = dataSlice->UpperLimit().Key;
        }
        PrimaryDataSize_ += dataSlice->GetDataSize();
        PrimaryRowCount_ += dataSlice->GetRowCount();
        ++PrimarySliceCount_;
    } else {
        ForeignDataSize_ += dataSlice->GetDataSize();
        ForeignRowCount_ += dataSlice->GetRowCount();
        ++ForeignSliceCount_;
    }
}

void TJobStub::AddPreliminaryForeignDataSlice(const TInputDataSlicePtr& dataSlice)
{
    PreliminaryForeignDataSize_ += dataSlice->GetDataSize();
    PreliminaryForeignRowCount_ += dataSlice->GetRowCount();
    ++PreliminaryForeignSliceCount_;
}

void TJobStub::Finalize()
{
    int nonEmptyStripeCount = 0;
    for (int index = 0; index < StripeList_->Stripes.size(); ++index) {
        if (StripeList_->Stripes[index]) {
            auto& stripe = StripeList_->Stripes[nonEmptyStripeCount];
            stripe = std::move(StripeList_->Stripes[index]);
            ++nonEmptyStripeCount;
            const auto& statistics = stripe->GetStatistics();
            StripeList_->TotalDataSize += statistics.DataSize;
            StripeList_->TotalRowCount += statistics.RowCount;
            StripeList_->TotalChunkCount += statistics.ChunkCount;
            // This is done to ensure that all the data slices inside a stripe
            // are not only sorted by key, but additionally by their position
            // in the original table.
            std::sort(
                stripe->DataSlices.begin(),
                stripe->DataSlices.end(),
                [] (const TInputDataSlicePtr& lhs, const TInputDataSlicePtr& rhs) {
                    if (lhs->Type == EDataSourceType::UnversionedTable) {
                        auto lhsChunk = lhs->GetSingleUnversionedChunkOrThrow();
                        auto rhsChunk = rhs->GetSingleUnversionedChunkOrThrow();
                        if (lhsChunk != rhsChunk) {
                            return lhsChunk->GetTableRowIndex() < rhsChunk->GetTableRowIndex();
                        }
                    }

                    if (lhs->LowerLimit().RowIndex &&
                        rhs->LowerLimit().RowIndex &&
                        *lhs->LowerLimit().RowIndex != *rhs->LowerLimit().RowIndex)
                    {
                        return *lhs->LowerLimit().RowIndex < *rhs->LowerLimit().RowIndex;
                    }

                    auto cmpResult = CompareRows(lhs->LowerLimit().Key, rhs->LowerLimit().Key);
                    if (cmpResult != 0) {
                        return cmpResult < 0;
                    }

                    return false;
                });
        }
    }
    StripeList_->Stripes.resize(nonEmptyStripeCount);
}

i64 TJobStub::GetDataSize()
{
    return PrimaryDataSize_ + ForeignDataSize_;
}

i64 TJobStub::GetRowCount()
{
    return PrimaryRowCount_ + ForeignRowCount_;
}

int TJobStub::GetSliceCount()
{
    return PrimarySliceCount_ + ForeignSliceCount_;
}

i64 TJobStub::GetPreliminaryDataSize()
{
    return PrimaryDataSize_ + PreliminaryForeignDataSize_;
}

i64 TJobStub::GetPreliminaryRowCount()
{
    return PrimaryRowCount_ + PreliminaryForeignRowCount_;
}

int TJobStub::GetPreliminarySliceCount()
{
    return PrimarySliceCount_ + PreliminaryForeignSliceCount_;
}

const TChunkStripePtr& TJobStub::GetStripe(int streamIndex, bool isStripePrimary)
{
    if (streamIndex >= StripeList_->Stripes.size()) {
        StripeList_->Stripes.resize(streamIndex + 1);
    }
    auto& stripe = StripeList_->Stripes[streamIndex];
    if (!stripe) {
        stripe = New<TChunkStripe>(!isStripePrimary /* foreign */);
    }
    return stripe;
}

////////////////////////////////////////////////////////////////////////////////

//! An internal representation of a finalized job.
TJobManager::TJob::TJob()
{ }

TJobManager::TJob::TJob(TJobManager* owner, std::unique_ptr<TJobStub> jobBuilder, IChunkPoolOutput::TCookie cookie)
    : DataSize_(jobBuilder->GetDataSize())
    , RowCount_(jobBuilder->GetRowCount())
    , StripeList_(std::move(jobBuilder->StripeList_))
    , Owner_(owner)
    , CookiePoolIterator_(Owner_->CookiePool_.end())
    , Cookie_(cookie)
{
    UpdateSelf();
}

void TJobManager::TJob::SetState(EJobState state)
{
    State_ = state;
    UpdateSelf();
}

void TJobManager::TJob::ChangeSuspendedStripeCountBy(int delta)
{
    SuspendedStripeCount_ += delta;
    YCHECK(SuspendedStripeCount_ >= 0);
    UpdateSelf();
}

void TJobManager::TJob::Invalidate()
{
    YCHECK(!Invalidated_);
    Invalidated_ = true;
    StripeList_->Stripes.clear();
    UpdateSelf();
}

bool TJobManager::TJob::IsInvalidated() const
{
    return Invalidated_;
}

void TJobManager::TJob::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Owner_);
    Persist(context, SuspendedStripeCount_);
    Persist(context, StripeList_);
    Persist(context, Cookie_);
    Persist(context, State_);
    Persist(context, DataSize_);
    Persist(context, RowCount_);
    Persist(context, Invalidated_);
    if (context.IsLoad()) {
        // We must add ourselves to the job pool.
        CookiePoolIterator_ = Owner_->CookiePool_.end();
        UpdateSelf();
    }
}

void TJobManager::TJob::UpdateSelf()
{
    bool inPoolDesired =
        State_ == EJobState::Pending &&
        SuspendedStripeCount_ == 0 &&
        !Invalidated_;
    if (InPool_ && !inPoolDesired) {
        RemoveSelf();
    } else if (!InPool_ && inPoolDesired) {
        AddSelf();
    }

    bool suspendedDesired =
        State_ == EJobState::Pending &&
        SuspendedStripeCount_ > 0 &&
        !Invalidated_;
    if (Suspended_ && !suspendedDesired) {
        ResumeSelf();
    } else if (!Suspended_ && suspendedDesired) {
        SuspendSelf();
    }
}

void TJobManager::TJob::RemoveSelf()
{
    YCHECK(CookiePoolIterator_ != Owner_->CookiePool_.end());
    Owner_->CookiePool_.erase(CookiePoolIterator_);
    --Owner_->CookiePoolSize_;
    CookiePoolIterator_ = Owner_->CookiePool_.end();
    InPool_ = false;
}

void TJobManager::TJob::AddSelf()
{
    YCHECK(CookiePoolIterator_ == Owner_->CookiePool_.end());
    ++Owner_->CookiePoolSize_;
    CookiePoolIterator_ = Owner_->CookiePool_.insert(Owner_->CookiePool_.end(), Cookie_);
    InPool_ = true;
}

void TJobManager::TJob::SuspendSelf()
{
    YCHECK(!Suspended_);
    Suspended_ = true;
    YCHECK(++Owner_->SuspendedJobCount_ > 0);
}

void TJobManager::TJob::ResumeSelf()
{
    YCHECK(Suspended_);
    YCHECK(--Owner_->SuspendedJobCount_ >= 0);
    Suspended_ = false;
}

////////////////////////////////////////////////////////////////////////////////

TJobManager::TJobManager()
{
    DataSizeCounter_.Set(0);
    RowCounter_.Set(0);
    JobCounter_.Set(0);
}

void TJobManager::AddJobs(std::vector<std::unique_ptr<TJobStub>> jobStubs)
{
    for (auto& jobStub : jobStubs) {
        AddJob(std::move(jobStub));
    }
}

//! Add a job that is built from the given stub.
void TJobManager::AddJob(std::unique_ptr<TJobStub> jobStub)
{
    YCHECK(jobStub);
    IChunkPoolOutput::TCookie outputCookie = Jobs_.size();

    LOG_DEBUG("Sorted job finished (Index: %v, PrimaryDataSize: %v, PrimaryRowCount: %v, "
        "PrimarySliceCount: %v, ForeignDataSize: %v, ForeignRowCount: %v, "
        "ForeignSliceCount: %v, LowerPrimaryKey: %v, UpperPrimaryKey: %v)",
        outputCookie,
        jobStub->GetPrimaryDataSize(),
        jobStub->GetPrimaryRowCount(),
        jobStub->GetPrimarySliceCount(),
        jobStub->GetForeignDataSize(),
        jobStub->GetForeignRowCount(),
        jobStub->GetForeignSliceCount(),
        jobStub->LowerPrimaryKey(),
        jobStub->UpperPrimaryKey());

    int initialSuspendedStripeCount = 0;

    //! We know which input cookie formed this job, so for each of them we
    //! have to remember newly created job in order to be able to suspend/resume it
    //! when some input cookie changes its state.
    for (auto inputCookie : jobStub->InputCookies_) {
        if (InputCookieToAffectedOutputCookies_.size() <= inputCookie) {
            InputCookieToAffectedOutputCookies_.resize(inputCookie + 1);
        }
        InputCookieToAffectedOutputCookies_[inputCookie].emplace_back(outputCookie);
        if (SuspendedInputCookies_.has(inputCookie)) {
            ++initialSuspendedStripeCount;
        }
    }

    Jobs_.emplace_back(this /* owner */, std::move(jobStub), outputCookie);
    Jobs_.back().ChangeSuspendedStripeCountBy(initialSuspendedStripeCount);

    JobCounter_.Increment(1);
    DataSizeCounter_.Increment(Jobs_.back().GetDataSize());
    RowCounter_.Increment(Jobs_.back().GetRowCount());
}

void TJobManager::Completed(IChunkPoolOutput::TCookie cookie, EInterruptReason reason)
{
    JobCounter_.Completed(1, reason);
    DataSizeCounter_.Completed(Jobs_[cookie].GetDataSize());
    RowCounter_.Completed(Jobs_[cookie].GetRowCount());
    if (reason == EInterruptReason::None) {
        Jobs_[cookie].SetState(EJobState::Completed);
    } else {
        JobCounter_.Increment(1);
        DataSizeCounter_.Increment(Jobs_[cookie].GetDataSize());
        RowCounter_.Increment(Jobs_[cookie].GetRowCount());
        Jobs_[cookie].SetState(EJobState::Pending);
    }
}

IChunkPoolOutput::TCookie TJobManager::ExtractCookie()
{
    auto cookie = *(CookiePool_.begin());

    JobCounter_.Start(1);
    DataSizeCounter_.Start(Jobs_[cookie].GetDataSize());
    RowCounter_.Start(Jobs_[cookie].GetRowCount());
    Jobs_[cookie].SetState(EJobState::Running);

    return cookie;
}

void TJobManager::Failed(IChunkPoolOutput::TCookie cookie)
{
    JobCounter_.Failed(1);
    DataSizeCounter_.Failed(Jobs_[cookie].GetDataSize());
    RowCounter_.Failed(Jobs_[cookie].GetRowCount());
    Jobs_[cookie].SetState(EJobState::Pending);
}

void TJobManager::Aborted(IChunkPoolOutput::TCookie cookie, EAbortReason reason)
{
    JobCounter_.Aborted(1, reason);
    DataSizeCounter_.Aborted(Jobs_[cookie].GetDataSize(), reason);
    RowCounter_.Aborted(Jobs_[cookie].GetRowCount(), reason);
    Jobs_[cookie].SetState(EJobState::Pending);
}

void TJobManager::Lost(IChunkPoolOutput::TCookie /* cookie */)
{
    // TODO(max42): YT-6565 =)
    Y_UNREACHABLE();
}

void TJobManager::Suspend(IChunkPoolInput::TCookie inputCookie)
{
    YCHECK(SuspendedInputCookies_.insert(inputCookie).second);

    if (InputCookieToAffectedOutputCookies_.size() <= inputCookie) {
        // This may happen if jobs that use this input were not added yet
        // (note that suspend may happen in Finish() before DoFinish()).
        return;
    }

    for (auto outputCookie : InputCookieToAffectedOutputCookies_[inputCookie]) {
        Jobs_[outputCookie].ChangeSuspendedStripeCountBy(+1);
    }
}

void TJobManager::Resume(IChunkPoolInput::TCookie inputCookie)
{
    YCHECK(SuspendedInputCookies_.erase(inputCookie) == 1);

    if (InputCookieToAffectedOutputCookies_.size() <= inputCookie) {
        // This may happen if jobs that use this input were not added yet
        // (note that suspend may happen in Finish() before DoFinish()).
        return;
    }

    for (auto outputCookie : InputCookieToAffectedOutputCookies_[inputCookie]) {
        Jobs_[outputCookie].ChangeSuspendedStripeCountBy(-1);
    }
}

void TJobManager::Invalidate(IChunkPoolInput::TCookie inputCookie)
{
    YCHECK(0 <= inputCookie && inputCookie < Jobs_.size());
    Jobs_[inputCookie].Invalidate();
}

std::vector<TInputDataSlicePtr> TJobManager::ReleaseForeignSlices(IChunkPoolInput::TCookie inputCookie)
{
    YCHECK(0 <= inputCookie && inputCookie < Jobs_.size());
    std::vector<TInputDataSlicePtr> foreignSlices;
    for (const auto& stripe : Jobs_[inputCookie].StripeList()->Stripes) {
        if (stripe->Foreign) {
            std::move(stripe->DataSlices.begin(), stripe->DataSlices.end(), std::back_inserter(foreignSlices));
            stripe->DataSlices.clear();
        }
    }
    return foreignSlices;
}

void TJobManager::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, InputCookieToAffectedOutputCookies_);
    Persist(context, DataSizeCounter_);
    Persist(context, RowCounter_);
    Persist(context, JobCounter_);
    Persist(context, Jobs_);
    Persist(context, FirstValidJobIndex_);
    Persist(context, SuspendedInputCookies_);
}

TChunkStripeStatisticsVector TJobManager::GetApproximateStripeStatistics() const
{
    if (CookiePoolSize_ == 0) {
        return TChunkStripeStatisticsVector();
    }
    auto cookie = *(CookiePool_.begin());
    const auto& job = Jobs_[cookie];
    return job.StripeList()->GetStatistics();
}

int TJobManager::GetPendingJobCount() const
{
    return CookiePoolSize_;
}

const TChunkStripeListPtr& TJobManager::GetStripeList(IChunkPoolOutput::TCookie cookie)
{
    YCHECK(cookie < Jobs_.size());
    YCHECK(Jobs_[cookie].GetState() == EJobState::Running);
    return Jobs_[cookie].StripeList();
}

void TJobManager::InvalidateAllJobs()
{
    while (FirstValidJobIndex_ < Jobs_.size()) {
        if (!Jobs_[FirstValidJobIndex_].IsInvalidated()) {
            Jobs_[FirstValidJobIndex_].Invalidate();
        }
        FirstValidJobIndex_++;
    }
}

void TJobManager::SetLogger(TLogger logger)
{
    Logger = logger;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkPools
} // namespace NYT