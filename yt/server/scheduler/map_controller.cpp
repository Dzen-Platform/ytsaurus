#include "map_controller.h"
#include "merge_controller.h"
#include "private.h"
#include "chunk_pool.h"
#include "chunk_list_pool.h"
#include "helpers.h"
#include "job_memory.h"
#include "operation_controller_detail.h"

#include <yt/ytlib/chunk_client/chunk_slice.h>

#include <yt/ytlib/table_client/config.h>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NChunkServer;
using namespace NJobProxy;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////

static const NProfiling::TProfiler Profiler("/operations/unordered");

////////////////////////////////////////////////////////////////////

class TUnorderedOperationControllerBase
    : public TOperationControllerBase
{
public:
    TUnorderedOperationControllerBase(
        TSchedulerConfigPtr config,
        TUnorderedOperationSpecBasePtr spec,
        TSimpleOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TOperationControllerBase(config, spec, host, operation)
        , Spec(spec)
        , Options(options)
    { }

    // Persistence.
    virtual void Persist(TPersistenceContext& context) override
    {
        TOperationControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, JobIOConfig);
        Persist(context, JobSpecTemplate);
        Persist(context, UnorderedTask);
        Persist(context, UnorderedTaskGroup);
    }

protected:
    TUnorderedOperationSpecBasePtr Spec;
    TSimpleOperationOptionsPtr Options;

    //! Customized job IO config.
    TJobIOConfigPtr JobIOConfig;

    //! The template for starting new jobs.
    TJobSpec JobSpecTemplate;

    //! Table reader options for map jobs.
    TTableReaderOptionsPtr TableReaderOptions;


    class TUnorderedTask
        : public TTask
    {
    public:
        //! For persistence only.
        TUnorderedTask()
            : Controller(nullptr)
        { }

        explicit TUnorderedTask(TUnorderedOperationControllerBase* controller, int jobCount)
            : TTask(controller)
            , Controller(controller)
            , ChunkPool(CreateUnorderedChunkPool(
                jobCount,
                Controller->Config->MaxChunkStripesPerJob))
        { }

        virtual Stroka GetId() const override
        {
            return "Unordered";
        }

        virtual TTaskGroupPtr GetGroup() const override
        {
            return Controller->UnorderedTaskGroup;
        }

        virtual TDuration GetLocalityTimeout() const override
        {
            return Controller->Spec->LocalityTimeout;
        }

        virtual TJobResources GetNeededResources(TJobletPtr joblet) const override
        {
            return Controller->GetUnorderedOperationResources(
                joblet->InputStripeList->GetStatistics(),
                joblet->MemoryReserveEnabled);
        }

        virtual IChunkPoolInput* GetChunkPoolInput() const override
        {
            return ChunkPool.get();
        }

        virtual IChunkPoolOutput* GetChunkPoolOutput() const override
        {
            return ChunkPool.get();
        }

        virtual void Persist(TPersistenceContext& context) override
        {
            TTask::Persist(context);

            using NYT::Persist;
            Persist(context, Controller);
            Persist(context, ChunkPool);
        }

    private:
        DECLARE_DYNAMIC_PHOENIX_TYPE(TUnorderedTask, 0x8ab75ee7);

        TUnorderedOperationControllerBase* Controller;

        std::unique_ptr<IChunkPool> ChunkPool;

        virtual bool IsMemoryReserveEnabled() const override
        {
            return Controller->IsMemoryReserveEnabled(Controller->JobCounter);
        }

        virtual TJobResources GetMinNeededResourcesHeavy() const override
        {
            return Controller->GetUnorderedOperationResources(
                ChunkPool->GetApproximateStripeStatistics(),
                IsMemoryReserveEnabled());
        }

        virtual bool IsIntermediateOutput() const override
        {
            return false;
        }

        virtual TTableReaderOptionsPtr GetTableReaderOptions() const override
        {
            return Controller->TableReaderOptions;
        }

        virtual EJobType GetJobType() const override
        {
            return EJobType(Controller->GetJobType());
        }

        virtual void BuildJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
        {
            jobSpec->CopyFrom(Controller->JobSpecTemplate);
            AddSequentialInputSpec(jobSpec, joblet);
            AddFinalOutputSpecs(jobSpec, joblet);
        }

        virtual void OnJobCompleted(TJobletPtr joblet, const TCompletedJobSummary& jobSummary) override
        {
            TTask::OnJobCompleted(joblet, jobSummary);

            RegisterOutput(joblet, joblet->JobIndex, jobSummary);
        }

        virtual void OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& jobSummary) override
        {
            TTask::OnJobAborted(joblet, jobSummary);
            Controller->UpdateAllTasksIfNeeded(Controller->JobCounter);
        }

    };

    typedef TIntrusivePtr<TUnorderedTask> TUnorderedTaskPtr;

    TUnorderedTaskPtr UnorderedTask;
    TTaskGroupPtr UnorderedTaskGroup;


    // Custom bits of preparation pipeline.
    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec->InputTablePaths;
    }

    virtual void DoInitialize() override
    {
        TOperationControllerBase::DoInitialize();

        UnorderedTaskGroup = New<TTaskGroup>();
        UnorderedTaskGroup->MinNeededResources.SetCpu(GetCpuLimit());
        RegisterTaskGroup(UnorderedTaskGroup);
    }

    virtual bool IsCompleted() const override
    {
        return UnorderedTask->IsCompleted();
    }

    virtual void CustomPrepare() override
    {
        // The total data size for processing (except teleport chunks).
        i64 totalDataSize = 0;

        // The number of output partitions generated so far.
        // Each partition either corresponds to a teleport chunk.
        int currentPartitionIndex = 0;

        PROFILE_TIMING ("/input_processing_time") {
            LOG_INFO("Processing inputs");

            std::vector<TRefCountedChunkSpecPtr> mergedChunks;

            for (const auto& chunkSpec : CollectInputChunks()) {
                if (IsTeleportChunk(*chunkSpec)) {
                    // Chunks not requiring merge go directly to the output chunk list.
                    LOG_TRACE("Teleport chunk added (ChunkId: %v, Partition: %v)",
                        FromProto<TChunkId>(chunkSpec->chunk_id()),
                        currentPartitionIndex);

                    // Place the chunk directly to the output table.
                    RegisterOutput(chunkSpec, currentPartitionIndex, 0);
                    ++currentPartitionIndex;
                } else {
                    mergedChunks.push_back(chunkSpec);
                    i64 dataSize;
                    GetStatistics(*chunkSpec, &dataSize);
                    totalDataSize += dataSize;
                }
            }

            // Create the task, if any data.
            if (totalDataSize > 0) {
                auto jobCount = SuggestJobCount(
                    totalDataSize,
                    Spec->DataSizePerJob,
                    Spec->JobCount,
                    Options->MaxJobCount);
                auto stripes = SliceChunks(mergedChunks, Options->JobMaxSliceDataSize, &jobCount);

                UnorderedTask = New<TUnorderedTask>(this, jobCount);
                UnorderedTask->Initialize();
                UnorderedTask->AddInput(stripes);
                UnorderedTask->FinishInput();
                RegisterTask(UnorderedTask);

                LOG_INFO("Inputs processed (JobCount: %v)",
                    jobCount);
            } else {
                LOG_INFO("Inputs processed (JobCount: 0). All chunks were teleported");
            }
        }

        InitJobIOConfig();
        InitJobSpecTemplate();
    }


    // Resource management.
    virtual TJobResources GetUnorderedOperationResources(
        const TChunkStripeStatisticsVector& statistics,
        bool isReserveEnabled) const
    {
        TJobResources result;
        result.SetUserSlots(1);
        result.SetCpu(GetCpuLimit());
        result.SetMemory(
            GetFinalIOMemorySize(
                Spec->JobIO,
                AggregateStatistics(statistics)) +
            GetFootprintMemorySize() +
            GetAdditionalMemorySize(isReserveEnabled));
        return result;
    }


    // Progress reporting.
    virtual Stroka GetLoggingProgress() const override
    {
        return Format(
            "Jobs = {T: %v, R: %v, C: %v, P: %v, F: %v, A: %v}, "
            "UnavailableInputChunks: %v",
            JobCounter.GetTotal(),
            JobCounter.GetRunning(),
            JobCounter.GetCompleted(),
            GetPendingJobCount(),
            JobCounter.GetFailed(),
            JobCounter.GetAborted(),
            UnavailableInputChunkCount);
    }


    // Unsorted helpers.
    virtual EJobType GetJobType() const = 0;

    virtual int GetCpuLimit() const
    {
        return 1;
    }

    virtual i64 GetAdditionalMemorySize(bool memoryReserveEnabled) const
    {
        UNUSED(memoryReserveEnabled);
        return 0;
    }

    void InitJobIOConfig()
    {
        JobIOConfig = CloneYsonSerializable(Spec->JobIO);
        InitFinalOutputConfig(JobIOConfig);

        TableReaderOptions = CreateTableReaderOptions(Spec->JobIO);
    }

    //! Returns |true| if the chunk can be included into the output as-is.
    virtual bool IsTeleportChunk(const TChunkSpec& chunkSpec) const {
        return false;
    }

    virtual void InitJobSpecTemplate()
    {
        JobSpecTemplate.set_type(static_cast<int>(GetJobType()));
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);

        if (Spec->InputQuery) {
            InitQuerySpec(schedulerJobSpecExt, *Spec->InputQuery, *Spec->InputSchema);
        }

        AuxNodeDirectory->DumpTo(schedulerJobSpecExt->mutable_aux_node_directory());
        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransactionId);
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig).Data());
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TUnorderedOperationControllerBase::TUnorderedTask);

////////////////////////////////////////////////////////////////////

class TMapController
    : public TUnorderedOperationControllerBase
{
public:
    TMapController(
        TSchedulerConfigPtr config,
        TMapOperationSpecPtr spec,
        TMapOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TUnorderedOperationControllerBase(config, spec, options, host, operation)
        , Spec(spec)
    { }

    virtual void BuildBriefSpec(IYsonConsumer* consumer) const override
    {
        TUnorderedOperationControllerBase::BuildBriefSpec(consumer);
        BuildYsonMapFluently(consumer)
            .Item("mapper").BeginMap()
                .Item("command").Value(TrimCommandForBriefSpec(Spec->Mapper->Command))
            .EndMap();
    }

    // Persistence.
    virtual void Persist(TPersistenceContext& context) override
    {
        TUnorderedOperationControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, StartRowIndex);
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TMapController, 0xbac5fd82);

    TMapOperationSpecPtr Spec;

    i64 StartRowIndex = 0;


    // Custom bits of preparation pipeline.
    virtual EJobType GetJobType() const override
    {
        return EJobType::Map;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return Spec->OutputTablePaths;
    }

    virtual std::vector<TPathWithStage> GetFilePaths() const override
    {
        std::vector<TPathWithStage> result;
        for (const auto& path : Spec->Mapper->FilePaths) {
            result.push_back(std::make_pair(path, EOperationStage::Map));
        }
        return result;
    }

    virtual void DoInitialize() override
    {
        TUnorderedOperationControllerBase::DoInitialize();

        ValidateUserFileCount(Spec->Mapper, "mapper");
    }

    virtual bool IsOutputLivePreviewSupported() const override
    {
        return true;
    }

    // Unsorted helpers.
    virtual int GetCpuLimit() const override
    {
        return Spec->Mapper->CpuLimit;
    }

    virtual i64 GetAdditionalMemorySize(bool memoryReserveEnabled) const override
    {
        return GetMemoryReserve(memoryReserveEnabled, Spec->Mapper);
    }

    virtual bool IsSortedOutputSupported() const override
    {
        return true;
    }

    virtual void InitJobSpecTemplate() override
    {
        TUnorderedOperationControllerBase::InitJobSpecTemplate();
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        InitUserJobSpecTemplate(
            schedulerJobSpecExt->mutable_user_job_spec(),
            Spec->Mapper,
            Files);
    }

    virtual void CustomizeJoblet(TJobletPtr joblet) override
    {
        joblet->StartRowIndex = StartRowIndex;
        StartRowIndex += joblet->InputStripeList->TotalRowCount;
    }

    virtual void CustomizeJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
    {
        auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        InitUserJobSpec(
            schedulerJobSpecExt->mutable_user_job_spec(),
            joblet,
            GetAdditionalMemorySize(joblet->MemoryReserveEnabled));
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TMapController);

////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateMapController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TMapOperationSpec>(operation->GetSpec());
    return spec->Ordered
        ? CreateOrderedMapController(config, host, operation)
        : New<TMapController>(config, spec, config->MapOperationOptions, host, operation);
}

////////////////////////////////////////////////////////////////////

class TUnorderedMergeController
    : public TUnorderedOperationControllerBase
{
public:
    TUnorderedMergeController(
        TSchedulerConfigPtr config,
        TUnorderedMergeOperationSpecPtr spec,
        TUnorderedMergeOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TUnorderedOperationControllerBase(config, spec, options, host, operation)
        , Spec(spec)
    { }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TUnorderedMergeController, 0x9a17a41f);

    TUnorderedMergeOperationSpecPtr Spec;


    // Custom bits of preparation pipeline.
    virtual EJobType GetJobType() const override
    {
        return EJobType::UnorderedMerge;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        std::vector<TRichYPath> result;
        result.push_back(Spec->OutputTablePath);
        return result;
    }

    // Unsorted helpers.
    virtual bool IsRowCountPreserved() const override
    {
        return !Spec->InputQuery;
    }

    //! Returns |true| if the chunk can be included into the output as-is.
    //! A typical implementation of #IsTeleportChunk that depends on whether chunks must be combined or not.
    virtual bool IsTeleportChunk(const TChunkSpec& chunkSpec) const override
    {
        if (Spec->ForceTransform) {
            return false;
        }

        return Spec->CombineChunks
            ? IsLargeCompleteChunk(chunkSpec, Spec->JobIO->TableWriter->DesiredChunkSize)
            : IsCompleteChunk(chunkSpec);
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TUnorderedMergeController);

////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateUnorderedMergeController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TUnorderedMergeOperationSpec>(operation->GetSpec());
    return New<TUnorderedMergeController>(config, spec, config->UnorderedMergeOperationOptions, host, operation);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

