#include "vanilla_chunk_pool.h"

#include "new_job_manager.h"

#include <yt/yt/server/lib/controller_agent/serialize.h>
#include <yt/yt/server/lib/controller_agent/structs.h>

#include <yt/yt/core/misc/ref_tracked.h>

#include <util/generic/cast.h>

namespace NYT::NChunkPools {

using namespace NNodeTrackerClient;
using namespace NScheduler;
using namespace NControllerAgent;

////////////////////////////////////////////////////////////////////////////////

class TVanillaChunkPool
    : public TChunkPoolOutputWithNewJobManagerBase
    , public NPhoenix::TFactoryTag<NPhoenix::TSimpleFactory>
{
public:
    explicit TVanillaChunkPool(const TVanillaChunkPoolOptions& options)
        : TChunkPoolOutputWithNewJobManagerBase(options.Logger)
        , RestartCompletedJobs_(options.RestartCompletedJobs)
    {
        // We use very small portion of job manager functionality. We fill it with dummy
        // jobs and make manager deal with extracting/completing/failing/aborting jobs for us.
        for (int index = 0; index < options.JobCount; ++index) {
            JobManager_->AddJob(std::make_unique<TNewJobStub>());
        }
    }

    //! Used only for persistence.
    TVanillaChunkPool() = default;

    virtual void Persist(const TPersistenceContext& context) override
    {
        TChunkPoolOutputWithJobManagerBase::Persist(context);

        using NYT::Persist;
        Persist(context, RestartCompletedJobs_);
    }

    virtual bool IsCompleted() const override
    {
        return
            JobManager_->JobCounter()->GetRunning() == 0 &&
            JobManager_->JobCounter()->GetPending() == 0;
    }

    virtual void Completed(IChunkPoolOutput::TCookie cookie, const TCompletedJobSummary& jobSummary) override
    {
        YT_VERIFY(
            jobSummary.InterruptReason == EInterruptReason::None ||
            jobSummary.InterruptReason == EInterruptReason::Preemption ||
            jobSummary.InterruptReason == EInterruptReason::Unknown ||
            jobSummary.InterruptReason == EInterruptReason::UserRequest);
        JobManager_->Completed(cookie, jobSummary.InterruptReason);
        if (jobSummary.InterruptReason != EInterruptReason::None || RestartCompletedJobs_) {
            // NB: it is important to lose this job intead of alloacting new job since we want
            // to keep range of cookies same as before (without growing infinitely). It is
            // significant to some of the vanilla operation applications like CHYT.
            JobManager_->Lost(cookie);
        }
    }

    virtual TChunkStripeListPtr GetStripeList(IChunkPoolOutput::TCookie cookie) override
    {
        return NullStripeList;
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TVanillaChunkPool, 0x42439a0a);
    bool RestartCompletedJobs_;
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TVanillaChunkPool);

////////////////////////////////////////////////////////////////////////////////

IChunkPoolOutputPtr CreateVanillaChunkPool(const TVanillaChunkPoolOptions& options)
{
    return New<TVanillaChunkPool>(options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkPools
