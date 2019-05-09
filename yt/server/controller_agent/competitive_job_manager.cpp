#include "competitive_job_manager.h"
#include "job_info.h"
#include "serialize.h"

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

TCompetitiveJobManager::TCompetitiveJobManager(
    std::function<void(TJobId, EAbortReason)> abortJobCallback,
    const NLogging::TLogger& logger,
    int maxSpeculativeJobCount)
    : AbortJobCallback_(std::move(abortJobCallback))
    , JobCounter_(New<TProgressCounter>(0))
    , Logger(logger)
    , MaxSpeculativeJobCount_(maxSpeculativeJobCount)
{ }

bool TCompetitiveJobManager::TryRegisterSpeculativeCandidate(const TJobletPtr& joblet)
{
    YCHECK(CookieToCompetition_.contains(joblet->OutputCookie));
    TCompetition& competition = CookieToCompetition_[joblet->OutputCookie];
    std::optional<TString> rejectReason;

    if (JobCounter_->GetTotal() == MaxSpeculativeJobCount_) {
        rejectReason = Format("speculative job limit reached (Limit: %v)", MaxSpeculativeJobCount_);
    } else if (SpeculativeCandidates_.contains(joblet->OutputCookie)) {
        rejectReason = "speculative candidate is already in queue";
    } else if (competition.Status == ECompetitionStatus::TwoCompetitiveJobs) {
        rejectReason = "speculative job is already running";
    } else if (competition.Status == ECompetitionStatus::CompetitionCompleted) {
        rejectReason = "competitive job has already completed";
    }
    if (rejectReason.has_value()) {
        YT_LOG_DEBUG("Ignoring speculative request; %v (JobId: %v, Cookie: %v)",
            *rejectReason,
            joblet->JobId,
            joblet->OutputCookie);
        return false;
    }

    competition.PendingDataWeight = joblet->InputStripeList->TotalDataWeight;
    SpeculativeCandidates_.insert(joblet->OutputCookie);
    PendingDataWeight_ += joblet->InputStripeList->TotalDataWeight;
    JobCounter_->Increment(1);
    YT_LOG_DEBUG("Speculative request is registered (JobId: %v, Cookie: %v)",
        joblet->JobId,
        joblet->OutputCookie);

    return true;
}

int TCompetitiveJobManager::GetPendingSpeculativeJobCount() const
{
    return JobCounter_->GetPending();
}

int TCompetitiveJobManager::GetTotalSpeculativeJobCount() const
{
    return JobCounter_->GetTotal();
}

NChunkPools::IChunkPoolOutput::TCookie TCompetitiveJobManager::PeekSpeculativeCandidate() const
{
    YCHECK(!SpeculativeCandidates_.empty());
    return *SpeculativeCandidates_.begin();
}

void TCompetitiveJobManager::OnJobScheduled(const TJobletPtr& joblet)
{
    if (joblet->Speculative) {
        YT_LOG_DEBUG("Scheduling speculative job (JobId: %v, Cookie: %v)",
            joblet->JobId,
            joblet->OutputCookie);
        YCHECK(CookieToCompetition_.contains(joblet->OutputCookie));
        auto& competition = CookieToCompetition_[joblet->OutputCookie];
        YCHECK(competition.Status == ECompetitionStatus::SingleJobOnly);
        competition.Competitors.push_back(joblet->JobId);
        competition.Status = ECompetitionStatus::TwoCompetitiveJobs;
        PendingDataWeight_ -= competition.PendingDataWeight;
        SpeculativeCandidates_.erase(joblet->OutputCookie);
        JobCounter_->Start(1);
    } else {
        auto insertedIt = CookieToCompetition_.insert(std::make_pair(joblet->OutputCookie, TCompetition()));
        YCHECK(insertedIt.second);
        insertedIt.first->second.Competitors.push_back(joblet->JobId);
    }
}

void TCompetitiveJobManager::OnJobCompleted(const TJobletPtr& joblet)
{
    OnJobFinished(joblet);
    if (CookieToCompetition_.contains(joblet->OutputCookie)) {
        auto abortReason = joblet->Speculative
            ? EAbortReason::SpeculativeRunWon
            : EAbortReason::SpeculativeRunLost;

        auto& competition = CookieToCompetition_[joblet->OutputCookie];
        competition.Status = ECompetitionStatus::CompetitionCompleted;
        YT_LOG_DEBUG("Job has won the competition; aborting other competitors (Cookie: %v, WinnerJobId: %v, LoserJobIds: %v)",
            joblet->OutputCookie,
            joblet->JobId,
            competition.Competitors);
        for (const auto& competitiveJobId : competition.Competitors) {
            AbortJobCallback_(competitiveJobId, abortReason);
        }
    }
}

bool TCompetitiveJobManager::OnJobFailed(const TJobletPtr& joblet)
{
    return OnUnsuccessfulJobFinish(joblet, [=] (const TProgressCounterPtr& counter) { counter->Failed(1); });
}

bool TCompetitiveJobManager::OnJobAborted(const TJobletPtr& joblet, EAbortReason reason)
{
    return OnUnsuccessfulJobFinish(joblet, [=] (const TProgressCounterPtr& counter) { counter->Aborted(1, reason); });
}

bool TCompetitiveJobManager::OnUnsuccessfulJobFinish(
    const TJobletPtr& joblet,
    const std::function<void(const TProgressCounterPtr&)>& updateJobCounter)
{
    YCHECK(CookieToCompetition_.contains(joblet->OutputCookie));
    auto& competition = CookieToCompetition_[joblet->OutputCookie];
    bool jobIsLoser = competition.Status == ECompetitionStatus::CompetitionCompleted;

    OnJobFinished(joblet);

    // We are updating our counter for job losers and for non-last jobs only.
    if (jobIsLoser || CookieToCompetition_.contains(joblet->OutputCookie)) {
        updateJobCounter(JobCounter_);
        JobCounter_->Decrement(1);
        return false;
    }
    return true;
}

void TCompetitiveJobManager::OnJobFinished(const TJobletPtr& joblet)
{
    YCHECK(CookieToCompetition_.contains(joblet->OutputCookie));
    auto& competition = CookieToCompetition_[joblet->OutputCookie];
    auto jobIt = Find(competition.Competitors, joblet->JobId);
    YCHECK(jobIt != competition.Competitors.end());
    competition.Competitors.erase(jobIt);

    if (competition.Competitors.empty()) {
        PendingDataWeight_ -= competition.PendingDataWeight;
        CookieToCompetition_.erase(joblet->OutputCookie);
    } else {
        YCHECK(competition.Status == ECompetitionStatus::TwoCompetitiveJobs);
        competition.Status = ECompetitionStatus::SingleJobOnly;
    }

    if (SpeculativeCandidates_.contains(joblet->OutputCookie)) {
        YT_LOG_DEBUG("Canceling speculative request early since original job finished (JobId: %v, Cookie: %v)",
            joblet->JobId,
            joblet->OutputCookie);
        SpeculativeCandidates_.erase(joblet->OutputCookie);
        JobCounter_->Decrement(1);
    }
}

std::optional<EAbortReason> TCompetitiveJobManager::ShouldAbortJob(const TJobletPtr& joblet) const
{
    YCHECK(CookieToCompetition_.contains(joblet->OutputCookie));
    const auto& competition = CookieToCompetition_.find(joblet->OutputCookie)->second;
    if (competition.Status == ECompetitionStatus::CompetitionCompleted) {
        return joblet->Speculative
            ? EAbortReason::SpeculativeRunLost
            : EAbortReason::SpeculativeRunWon;
    }
    return std::nullopt;
}

i64 TCompetitiveJobManager::GetPendingCandidatesDataWeight() const
{
    return PendingDataWeight_;
}

bool TCompetitiveJobManager::IsFinished() const
{
    return SpeculativeCandidates_.empty() && CookieToCompetition_.empty();
}

TProgressCounterPtr TCompetitiveJobManager::GetProgressCounter()
{
    return JobCounter_;
}

void TCompetitiveJobManager::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, CookieToCompetition_);
    Persist(context, SpeculativeCandidates_);
    Persist(context, PendingDataWeight_);
    Persist(context, JobCounter_);
    Persist(context, MaxSpeculativeJobCount_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
