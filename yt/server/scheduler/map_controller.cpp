#include "map_controller.h"
#include "merge_controller.h"
#include "chunk_pool.h"
#include "chunk_list_pool.h"
#include "helpers.h"
#include "job_memory.h"
#include "private.h"
#include "operation_controller_detail.h"
#include "job_size_manager.h"

#include <yt/ytlib/chunk_client/input_chunk_slice.h>

#include <yt/ytlib/table_client/config.h>

#include <yt/ytlib/api/transaction.h>

#include <yt/core/concurrency/periodic_yielder.h>

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
using namespace NConcurrency;

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
        : TOperationControllerBase(config, spec, options, host, operation)
        , Spec(spec)
        , Options(options)
    { }

    // Persistence.
    virtual void Persist(const TPersistenceContext& context) override
    {
        TOperationControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, JobIOConfig);
        Persist(context, JobSpecTemplate);
        Persist(context, TableReaderOptions);
        Persist(context, UnorderedPool);
        Persist(context, UnorderedTask);
        Persist(context, UnorderedTaskGroup);
        Persist(context, JobSizeManager);
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

        TUnorderedTask(TUnorderedOperationControllerBase* controller, i64 dataSizePerJob)
            : TTask(controller)
            , Controller(controller)
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

        virtual TExtendedJobResources GetNeededResources(TJobletPtr joblet) const override
        {
            auto result = Controller->GetUnorderedOperationResources(
                joblet->InputStripeList->GetStatistics());
            AddFootprintAndUserJobResources(result);
            return result;
        }

        virtual IChunkPoolInput* GetChunkPoolInput() const override
        {
            return Controller->UnorderedPool.get();
        }

        virtual IChunkPoolOutput* GetChunkPoolOutput() const override
        {
            return Controller->UnorderedPool.get();
        }

        virtual void Persist(const TPersistenceContext& context) override
        {
            TTask::Persist(context);

            using NYT::Persist;
            Persist(context, Controller);
        }

    private:
        DECLARE_DYNAMIC_PHOENIX_TYPE(TUnorderedTask, 0x8ab75ee7);

        TUnorderedOperationControllerBase* Controller;

        virtual TExtendedJobResources GetMinNeededResourcesHeavy() const override
        {
            auto result = Controller->GetUnorderedOperationResources(
                Controller->UnorderedPool->GetApproximateStripeStatistics());
            AddFootprintAndUserJobResources(result);
            return result;
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
            return Controller->GetJobType();
        }

        virtual TUserJobSpecPtr GetUserJobSpec() const override
        {
            return Controller->GetUserJobSpec();
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

            if (Controller->JobSizeManager) {
                Controller->JobSizeManager->OnJobCompleted(jobSummary);
                Controller->UnorderedPool->SetDataSizePerJob(Controller->JobSizeManager->GetIdealDataSizePerJob());
                LOG_DEBUG("Set ideal data size per job (DataSizePerJob: %v)",
                    Controller->JobSizeManager->GetIdealDataSizePerJob());
            }
        }

        virtual void OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& jobSummary) override
        {
            TTask::OnJobAborted(joblet, jobSummary);
        }

    };

    typedef TIntrusivePtr<TUnorderedTask> TUnorderedTaskPtr;

    std::unique_ptr<IChunkPool> UnorderedPool;

    TUnorderedTaskPtr UnorderedTask;
    TTaskGroupPtr UnorderedTaskGroup;

    std::unique_ptr<IJobSizeManager> JobSizeManager;


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

    void InitUnorderedPool(i64 dataSizePerJob)
    {
        UnorderedPool = CreateUnorderedChunkPool(
            dataSizePerJob,
            Config->MaxChunkStripesPerJob);
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

            std::vector<TInputChunkPtr> mergedChunks;

            TPeriodicYielder yielder(PrepareYieldPeriod);
            for (const auto& chunk : CollectPrimaryUnversionedChunks()) {
                yielder.TryYield();
                if (IsTeleportChunk(chunk)) {
                    // Chunks not requiring merge go directly to the output chunk list.
                    LOG_TRACE("Teleport chunk added (ChunkId: %v, Partition: %v)",
                        chunk->ChunkId(),
                        currentPartitionIndex);

                    // Place the chunk directly to the output table.
                    RegisterOutput(chunk, currentPartitionIndex, 0);
                    ++currentPartitionIndex;
                } else {
                    mergedChunks.push_back(chunk);
                    totalDataSize += chunk->GetUncompressedDataSize();
                }
            }

            totalDataSize += CalculatePrimaryVersionedChunksSize();

            // Create the task, if any data.
            if (totalDataSize > 0) {
                TJobSizeLimits jobSizeLimits(
                    totalDataSize,
                    Spec->DataSizePerJob.Get(Options->DataSizePerJob),
                    Spec->JobCount,
                    GetMaxJobCount(Spec->MaxJobCount, Options->MaxJobCount));

                std::vector<TChunkStripePtr> stripes;
                auto sliceDataSize = CalculateSliceDataSize(Options->JobMaxSliceDataSize, jobSizeLimits);
                SliceUnversionedChunks(mergedChunks, sliceDataSize, &stripes);
                SlicePrimaryVersionedChunks(sliceDataSize, &stripes);
                jobSizeLimits.UpdateStripeCount(stripes.size(), Config->MaxChunkStripesPerJob);

                InitUnorderedPool(jobSizeLimits.GetDataSizePerJob());

                if (Config->EnableMapJobSizeManager && !Spec->JobCount && !Spec->DataSizePerJob) {
                    LOG_DEBUG("Activating job size manager (DataSizePerJob: %v, MaxJobDataSize: %v, MinJobTime: %v, ExecToPrepareTimeRatio: %v",
                        jobSizeLimits.GetDataSizePerJob(),
                        Spec->MaxDataSizePerJob,
                        Options->JobSizeManager->MinJobTime,
                        Options->JobSizeManager->ExecToPrepareTimeRatio);
                    JobSizeManager = CreateJobSizeManager(
                        jobSizeLimits.GetDataSizePerJob(),
                        Spec->MaxDataSizePerJob,
                        Options->JobSizeManager);
                    UnorderedPool->SetMaxDataSizePerJob(Spec->MaxDataSizePerJob);
                }

                UnorderedTask = New<TUnorderedTask>(this, jobSizeLimits.GetDataSizePerJob());
                UnorderedTask->Initialize();
                UnorderedTask->AddInput(stripes);
                UnorderedTask->FinishInput();
                RegisterTask(UnorderedTask);

                LOG_INFO("Inputs processed (JobCount: %v)",
                    jobSizeLimits.GetJobCount());
            } else {
                LOG_INFO("Inputs processed (JobCount: 0). All chunks were teleported");
            }
        }

        InitJobIOConfig();
        InitJobSpecTemplate();
    }


    // Resource management.
    TExtendedJobResources GetUnorderedOperationResources(
        const TChunkStripeStatisticsVector& statistics) const
    {
        TExtendedJobResources result;
        result.SetUserSlots(1);
        result.SetCpu(GetCpuLimit());
        result.SetJobProxyMemory(GetFinalIOMemorySize(Spec->JobIO, AggregateStatistics(statistics)));
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
            JobCounter.GetAbortedTotal(),
            UnavailableInputChunkCount);
    }


    // Unsorted helpers.
    virtual EJobType GetJobType() const = 0;

    virtual TUserJobSpecPtr GetUserJobSpec() const
    {
        return nullptr;
    }

    virtual int GetCpuLimit() const
    {
        return 1;
    }

    virtual i64 GetUserJobMemoryReserve() const
    {
        return 0;
    }

    void InitJobIOConfig()
    {
        JobIOConfig = CloneYsonSerializable(Spec->JobIO);
        InitFinalOutputConfig(JobIOConfig);

        TableReaderOptions = CreateTableReaderOptions(Spec->JobIO);
    }

    //! Returns |true| if the chunk can be included into the output as-is.
    virtual bool IsTeleportChunk(const TInputChunkPtr& chunkSpec) const
    {
        return false;
    }

    virtual void InitJobSpecTemplate()
    {
        JobSpecTemplate.set_type(static_cast<int>(GetJobType()));
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);

        if (Spec->InputQuery) {
            InitQuerySpec(schedulerJobSpecExt, *Spec->InputQuery, *Spec->InputSchema);
        }

        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
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
    {
        RegisterJobProxyMemoryDigest(EJobType::Map, spec->JobProxyMemoryDigest);
        RegisterUserJobMemoryDigest(EJobType::Map, spec->Mapper->MemoryReserveFactor);
    }

    virtual void BuildBriefSpec(IYsonConsumer* consumer) const override
    {
        TUnorderedOperationControllerBase::BuildBriefSpec(consumer);
        BuildYsonMapFluently(consumer)
            .Item("mapper").BeginMap()
                .Item("command").Value(TrimCommandForBriefSpec(Spec->Mapper->Command))
            .EndMap();
    }

    // Persistence.
    virtual void Persist(const TPersistenceContext& context) override
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

    virtual TUserJobSpecPtr GetUserJobSpec() const override
    {
        return Spec->Mapper;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return Spec->OutputTablePaths;
    }

    virtual TNullable<TRichYPath> GetStderrTablePath() const override
    {
        return Spec->StderrTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetStderrTableWriterConfig() const override
    {
        return Spec->StderrTableWriterConfig;
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

    virtual i64 GetUserJobMemoryReserve() const override
    {
        return ComputeUserJobMemoryReserve(EJobType::Map, Spec->Mapper);
    }

    virtual void InitJobSpecTemplate() override
    {
        TUnorderedOperationControllerBase::InitJobSpecTemplate();
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        InitUserJobSpecTemplate(
            schedulerJobSpecExt->mutable_user_job_spec(),
            Spec->Mapper,
            Files,
            Spec->JobNodeAccount);
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
            joblet);
    }

    virtual bool IsInputDataSizeHistogramSupported() const override
    {
        return true;
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
    {
        RegisterJobProxyMemoryDigest(EJobType::UnorderedMerge, spec->JobProxyMemoryDigest);
    }

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
    virtual bool IsTeleportChunk(const TInputChunkPtr& chunkSpec) const override
    {
        bool isSchemaCompatible =
            ValidateTableSchemaCompatibility(
                InputTables[chunkSpec->GetTableIndex()].Schema,
                OutputTables[0].TableUploadOptions.TableSchema,
                false)
            .IsOK();

        if (Spec->ForceTransform || chunkSpec->Channel() || !isSchemaCompatible) {
            return false;
        }

        return Spec->CombineChunks
            ? chunkSpec->IsLargeCompleteChunk(Spec->JobIO->TableWriter->DesiredChunkSize)
            : chunkSpec->IsCompleteChunk();
    }

    virtual void PrepareOutputTables() override
    {
        auto& table = OutputTables[0];

        auto validateOutputNotSorted = [&] () {
            if (table.TableUploadOptions.TableSchema.IsSorted()) {
                THROW_ERROR_EXCEPTION("Cannot perform unordered merge into a sorted table in a \"strong\" schema mode")
                    << TErrorAttribute("schema", table.TableUploadOptions.TableSchema);
            }
        };

        switch (Spec->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    InferSchemaFromInputUnordered();
                } else {
                    validateOutputNotSorted();

                    for (const auto& inputTable : InputTables) {
                        if (inputTable.SchemaMode == ETableSchemaMode::Strong) {
                            ValidateTableSchemaCompatibility(
                                inputTable.Schema,
                                table.TableUploadOptions.TableSchema,
                                /* ignoreSortOrder */ true)
                                .ThrowOnError();
                        }
                    }
                }
                break;

            case ESchemaInferenceMode::FromInput:
                InferSchemaFromInputUnordered();
                break;

            case ESchemaInferenceMode::FromOutput:
                validateOutputNotSorted();
                break;

            default:
                Y_UNREACHABLE();
        }
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

