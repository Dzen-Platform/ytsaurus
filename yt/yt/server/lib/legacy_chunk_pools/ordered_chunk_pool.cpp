#include "ordered_chunk_pool.h"

#include "helpers.h"
#include "job_manager.h"
#include "output_order.h"

#include <yt/server/lib/controller_agent/job_size_constraints.h>
#include <yt/server/lib/controller_agent/structs.h>

#include <yt/library/random/bernoulli_sampler.h>

#include <yt/core/concurrency/periodic_yielder.h>

#include <yt/core/misc/numeric_helpers.h>
#include <yt/core/misc/ref_tracked.h>

namespace NYT::NLegacyChunkPools {

using namespace NChunkClient;
using namespace NConcurrency;
using namespace NControllerAgent;
using namespace NNodeTrackerClient;
using namespace NTableClient;
using namespace NLogging;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////////////////

void TOrderedChunkPoolOptions::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, MaxTotalSliceCount);
    Persist(context, MinTeleportChunkSize);
    Persist(context, JobSizeConstraints);
    Persist(context, SupportLocality);
    Persist(context, OperationId);
    Persist(context, EnablePeriodicYielder);
    Persist(context, ShouldSliceByRowIndices);
    Persist(context, Task);
}

////////////////////////////////////////////////////////////////////////////////

class TOrderedChunkPool
    : public TChunkPoolInputBase
    , public TChunkPoolOutputWithJobManagerBase
    , public IChunkPool
    , public NPhoenix::TFactoryTag<NPhoenix::TSimpleFactory>
{
public:
    //! Used only for persistence.
    TOrderedChunkPool()
    { }

    TOrderedChunkPool(
        const TOrderedChunkPoolOptions& options,
        TInputStreamDirectory inputStreamDirectory)
        : InputStreamDirectory_(std::move(inputStreamDirectory))
        , MinTeleportChunkSize_(options.MinTeleportChunkSize)
        , JobSizeConstraints_(options.JobSizeConstraints)
        , Sampler_(JobSizeConstraints_->GetSamplingRate())
        , SupportLocality_(options.SupportLocality)
        , OperationId_(options.OperationId)
        , Task_(options.Task)
        , MaxTotalSliceCount_(options.MaxTotalSliceCount)
        , ShouldSliceByRowIndices_(options.ShouldSliceByRowIndices)
        , EnablePeriodicYielder_(options.EnablePeriodicYielder)
        , OutputOrder_(options.KeepOutputOrder ? New<TOutputOrder>() : nullptr)
    {
        Logger.AddTag("ChunkPoolId: %v", ChunkPoolId_);
        Logger.AddTag("OperationId: %v", OperationId_);
        Logger.AddTag("Task: %v", Task_);
        JobManager_->SetLogger(Logger);

        YT_LOG_DEBUG("Ordered chunk pool created (DataWeightPerJob: %v, MaxDataSlicesPerJob: %v, "
            "InputSliceDataWeight: %v, InputSliceRowCount: %v)",
            JobSizeConstraints_->GetDataWeightPerJob(),
            JobSizeConstraints_->GetMaxDataSlicesPerJob(),
            JobSizeConstraints_->GetInputSliceDataWeight(),
            JobSizeConstraints_->GetInputSliceRowCount());
    }

    // IChunkPoolInput implementation.

    virtual IChunkPoolInput::TCookie Add(TChunkStripePtr stripe) override
    {
        YT_VERIFY(!Finished);

        if (stripe->DataSlices.empty()) {
            return IChunkPoolInput::NullCookie;
        }

        auto cookie = static_cast<int>(Stripes_.size());
        Stripes_.emplace_back(stripe);

        return cookie;
    }

    virtual void Finish() override
    {
        YT_VERIFY(!Finished);
        TChunkPoolInputBase::Finish();

        // NB: this method accounts all the stripes that were suspended before
        // the chunk pool was finished. It should be called only once.
        SetupSuspendedStripes();

        BuildJobsAndFindTeleportChunks();
    }

    virtual void Suspend(IChunkPoolInput::TCookie cookie) override
    {
        auto& suspendableStripe = Stripes_[cookie];
        suspendableStripe.Suspend();
        if (Finished) {
            JobManager_->Suspend(cookie);
        }
    }

    virtual void Resume(IChunkPoolInput::TCookie cookie) override
    {
        Stripes_[cookie].Resume();
        if (Finished) {
            JobManager_->Resume(cookie);
        }
    }

    virtual bool IsCompleted() const override
    {
        return
            Finished &&
            GetPendingJobCount() == 0 &&
            JobManager_->JobCounter()->GetRunning() == 0 &&
            JobManager_->GetSuspendedJobCount() == 0;
    }

    virtual void Completed(IChunkPoolOutput::TCookie cookie, const TCompletedJobSummary& jobSummary) override
    {
        if (jobSummary.InterruptReason != EInterruptReason::None) {
            YT_LOG_DEBUG("Splitting job (OutputCookie: %v, InterruptReason: %v, SplitJobCount: %v)",
                cookie,
                jobSummary.InterruptReason,
                jobSummary.SplitJobCount);
            JobManager_->Invalidate(cookie);
            SplitJob(jobSummary.UnreadInputDataSlices, jobSummary.SplitJobCount, cookie);
        }
        JobManager_->Completed(cookie, jobSummary.InterruptReason);
    }

    virtual TOutputOrderPtr GetOutputOrder() const override
    {
        return OutputOrder_;
    }

    virtual i64 GetDataSliceCount() const override
    {
        return TotalSliceCount_;
    }

    virtual void Persist(const TPersistenceContext& context) final override
    {
        TChunkPoolInputBase::Persist(context);
        TChunkPoolOutputWithJobManagerBase::Persist(context);

        using NYT::Persist;

        Persist(context, InputStreamDirectory_);
        Persist(context, MinTeleportChunkSize_);
        Persist(context, Stripes_);
        Persist(context, JobSizeConstraints_);
        Persist(context, Sampler_);
        Persist(context, SupportLocality_);
        Persist(context, OperationId_);
        Persist(context, Task_);
        Persist(context, ChunkPoolId_);
        Persist(context, MaxTotalSliceCount_);
        Persist(context, ShouldSliceByRowIndices_);
        Persist(context, EnablePeriodicYielder_);
        Persist(context, OutputOrder_);
        Persist(context, JobIndex_);
        Persist(context, TotalSliceCount_);
        if (context.IsLoad()) {
            Logger.AddTag("ChunkPoolId: %v", ChunkPoolId_);
            Logger.AddTag("OperationId: %v", OperationId_);
            Logger.AddTag("Task: %v", Task_);
            JobManager_->SetLogger(Logger);
        }
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TOrderedChunkPool, 0xffe92abd);

    //! Information about input sources (e.g. input tables for sorted reduce operation).
    TInputStreamDirectory InputStreamDirectory_;

    //! An option to control chunk teleportation logic. Only large complete
    //! chunks of at least that size will be teleported.
    i64 MinTeleportChunkSize_;

    //! All stripes that were added to this pool.
    std::vector<TSuspendableStripe> Stripes_;

    IJobSizeConstraintsPtr JobSizeConstraints_;
    //! Used both for job sampling and teleport chunk sampling.
    TBernoulliSampler Sampler_;

    bool SupportLocality_ = false;

    TLogger Logger = ChunkPoolLogger;

    TOperationId OperationId_;
    TString Task_;

    TGuid ChunkPoolId_ = TGuid::Create();

    i64 MaxTotalSliceCount_ = 0;

    bool ShouldSliceByRowIndices_ = false;

    bool EnablePeriodicYielder_;

    TOutputOrderPtr OutputOrder_ = nullptr;

    std::unique_ptr<TJobStub> CurrentJob_;

    int JobIndex_ = 0;
    int BuiltJobCount_ = 0;

    i64 TotalSliceCount_ = 0;
    i64 TotalDataWeight_ = 0;

    void SetupSuspendedStripes()
    {
        for (int inputCookie = 0; inputCookie < Stripes_.size(); ++inputCookie) {
            const auto& stripe = Stripes_[inputCookie];
            if (stripe.IsSuspended()) {
                JobManager_->Suspend(inputCookie);
            }
        }
    }

    TPeriodicYielder CreatePeriodicYielder()
    {
        if (EnablePeriodicYielder_) {
            return TPeriodicYielder(PrepareYieldPeriod);
        } else {
            return TPeriodicYielder();
        }
    }

    void BuildJobsAndFindTeleportChunks()
    {
        if (auto samplingRate = JobSizeConstraints_->GetSamplingRate()) {
            YT_LOG_DEBUG(
                "Building jobs with sampling "
                "(SamplingRate: %v, SamplingDataWeightPerJob: %v, SamplingPrimaryDataWeightPerJob: %v)",
                *JobSizeConstraints_->GetSamplingRate(),
                JobSizeConstraints_->GetSamplingDataWeightPerJob(),
                JobSizeConstraints_->GetSamplingPrimaryDataWeightPerJob());
        }

        int droppedTeleportChunkCount = 0;
        int chunksTeleported = 0;

        auto yielder = CreatePeriodicYielder();
        for (int inputCookie = 0; inputCookie < Stripes_.size(); ++inputCookie) {
            const auto& stripe = Stripes_[inputCookie].GetStripe();
            for (const auto& dataSlice : stripe->DataSlices) {
                yielder.TryYield();
                if (dataSlice->Type == EDataSourceType::UnversionedTable) {
                    auto inputChunk = dataSlice->GetSingleUnversionedChunkOrThrow();
                    if (InputStreamDirectory_.GetDescriptor(stripe->GetInputStreamIndex()).IsTeleportable() &&
                        inputChunk->IsLargeCompleteChunk(MinTeleportChunkSize_))
                    {
                        if (Sampler_.Sample()) {
                            EndJob();

                            // Add barrier.
                            CurrentJob()->SetIsBarrier(true);
                            JobManager_->AddJob(std::move(CurrentJob()));

                            ChunkTeleported_.Fire(inputChunk, /*tag=*/std::any{});
                            ++chunksTeleported;

                            if (OutputOrder_) {
                                OutputOrder_->Push(TOutputOrder::TEntry(inputChunk));
                            }
                        } else {
                            // This teleport chunk goes to /dev/null.
                            ++droppedTeleportChunkCount;
                        }
                        continue;
                    }
                }

                std::vector<TInputDataSlicePtr> slicedDataSlices;
                if (dataSlice->Type == EDataSourceType::UnversionedTable && ShouldSliceByRowIndices_) {
                    auto chunkSlices = CreateInputChunkSlice(dataSlice->GetSingleUnversionedChunkOrThrow())
                        ->SliceEvenly(JobSizeConstraints_->GetInputSliceDataWeight(), JobSizeConstraints_->GetInputSliceRowCount());
                    for (const auto& chunkSlice : chunkSlices) {
                        auto smallerDataSlice = CreateUnversionedInputDataSlice(chunkSlice);
                        AddPrimaryDataSlice(smallerDataSlice, inputCookie, JobSizeConstraints_->GetDataWeightPerJob());
                    }
                } else {
                    AddPrimaryDataSlice(dataSlice, inputCookie, JobSizeConstraints_->GetDataWeightPerJob());
                }

            }
        }
        EndJob();

        YT_LOG_INFO("Jobs created (Count: %v, TeleportChunkCount: %v, DroppedTeleportChunkCount: %v)",
            BuiltJobCount_,
            chunksTeleported,
            droppedTeleportChunkCount);

        if (JobSizeConstraints_->GetSamplingRate()) {
            JobManager_->Enlarge(
                JobSizeConstraints_->GetDataWeightPerJob(),
                JobSizeConstraints_->GetPrimaryDataWeightPerJob());
        }

        JobSizeConstraints_->UpdateInputDataWeight(TotalDataWeight_);
    }

    void SplitJob(
        std::vector<TInputDataSlicePtr> unreadInputDataSlices,
        int splitJobCount,
        IChunkPoolOutput::TCookie cookie)
    {
        i64 dataSize = 0;
        for (const auto& dataSlice : unreadInputDataSlices) {
            dataSize += dataSlice->GetDataWeight();
        }
        i64 dataSizePerJob;
        if (splitJobCount == 1) {
            dataSizePerJob = std::numeric_limits<i64>::max() / 4;
        } else {
            dataSizePerJob = DivCeil(dataSize, static_cast<i64>(splitJobCount));
        }

        auto jobIndex = JobIndex_;
        // Teleport chunks do not affect the job split process since each original
        // job is already located between the teleport chunks.
        std::vector<TInputChunkPtr> teleportChunks;
        if (OutputOrder_) {
            OutputOrder_->SeekCookie(cookie);
        }
        for (const auto& dataSlice : unreadInputDataSlices) {
            int inputCookie = *dataSlice->Tag;
            AddPrimaryDataSlice(dataSlice, inputCookie, dataSizePerJob);
        }
        // We wanted to create several jobs, but failed to do it => job is unsplittable.
        auto unsplittable = splitJobCount > 1 && jobIndex == JobIndex_;
        EndJob(unsplittable);
    }

    i64 GetDataWeightPerJob() const
    {
        return
            JobSizeConstraints_->GetSamplingRate()
            ? JobSizeConstraints_->GetSamplingDataWeightPerJob()
            : JobSizeConstraints_->GetDataWeightPerJob();
    }

    void AddPrimaryDataSlice(
        const TInputDataSlicePtr& dataSlice,
        IChunkPoolInput::TCookie cookie,
        i64 dataSizePerJob)
    {
        bool jobIsLargeEnough =
            CurrentJob()->GetPreliminarySliceCount() + 1 > JobSizeConstraints_->GetMaxDataSlicesPerJob() ||
            CurrentJob()->GetDataWeight() >= dataSizePerJob;
        if (jobIsLargeEnough) {
            EndJob();
        }
        auto dataSliceCopy = CreateInputDataSlice(dataSlice);
        dataSliceCopy->Tag = cookie;
        CurrentJob()->AddDataSlice(dataSliceCopy, cookie, true /* isPrimary */);
    }

    void EndJob(bool unsplittable = false)
    {
        if (CurrentJob()->GetSliceCount() > 0) {
            if (Sampler_.Sample()) {
                YT_LOG_DEBUG("Ordered job created (JobIndex: %v, BuiltJobCount: %v, PrimaryDataWeight: %v, RowCount: %v, SliceCount: %v)",
                    JobIndex_,
                    BuiltJobCount_,
                    CurrentJob()->GetPrimaryDataWeight(),
                    CurrentJob()->GetPrimaryRowCount(),
                    CurrentJob()->GetPrimarySliceCount());

                TotalSliceCount_ += CurrentJob()->GetSliceCount();
                TotalDataWeight_ += CurrentJob()->GetDataWeight();

                ++BuiltJobCount_;

                if (TotalSliceCount_ > MaxTotalSliceCount_) {
                    THROW_ERROR_EXCEPTION(EErrorCode::DataSliceLimitExceeded, "Total number of data slices in ordered pool is too large")
                        << TErrorAttribute("actual_total_slice_count", TotalSliceCount_)
                        << TErrorAttribute("max_total_slice_count", MaxTotalSliceCount_)
                        << TErrorAttribute("current_job_count", JobIndex_);
                }
                if (unsplittable) {
                    CurrentJob()->SetUnsplittable();
                }

                CurrentJob()->Finalize(false /* sortByPosition */);

                auto cookie = JobManager_->AddJob(std::move(CurrentJob()));
                if (OutputOrder_) {
                    OutputOrder_->Push(cookie);
                }

                YT_ASSERT(!CurrentJob_);
            } else {
                YT_LOG_DEBUG("Ordered job skipped (JobIndex: %v, BuiltJobCount: %v, PrimatyDataWeight: %v, DataWeight: %v, RowCount: %v, SliceCount: %v)",
                    JobIndex_,
                    BuiltJobCount_,
                    CurrentJob()->GetPrimaryDataWeight(),
                    CurrentJob()->GetPrimaryRowCount(),
                    CurrentJob()->GetPrimarySliceCount());
                CurrentJob().reset();
            }
            ++JobIndex_;
        }
    }

    std::unique_ptr<TJobStub>& CurrentJob()
    {
        if (!CurrentJob_) {
            CurrentJob_ = std::make_unique<TJobStub>();
        }
        return CurrentJob_;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TOrderedChunkPool);

////////////////////////////////////////////////////////////////////////////////

IChunkPoolPtr CreateOrderedChunkPool(
    const TOrderedChunkPoolOptions& options,
    TInputStreamDirectory inputStreamDirectory)
{
    return New<TOrderedChunkPool>(options, std::move(inputStreamDirectory));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NLegacyChunkPools
