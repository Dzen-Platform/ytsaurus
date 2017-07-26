#include "map_controller.h"
#include "legacy_merge_controller.h"
#include "chunk_list_pool.h"
#include "helpers.h"
#include "job_info.h"
#include "job_memory.h"
#include "private.h"
#include "operation_controller_detail.h"

#include <yt/server/chunk_pools/unordered_chunk_pool.h>
#include <yt/server/chunk_pools/chunk_pool.h>

#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/chunk_client/input_chunk_slice.h>

#include <yt/ytlib/table_client/config.h>

#include <yt/ytlib/query_client/query.h>

#include <yt/core/concurrency/periodic_yielder.h>

#include <yt/core/misc/numeric_helpers.h>

namespace NYT {
namespace NControllerAgent {

using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NChunkServer;
using namespace NJobProxy;
using namespace NChunkClient;
using namespace NChunkPools;
using namespace NChunkClient::NProto;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NTableClient;
using namespace NConcurrency;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////////////////

static const NProfiling::TProfiler Profiler("/operations/unordered");

////////////////////////////////////////////////////////////////////////////////

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
        Persist(context, IsExplicitJobCount);
        Persist(context, UnorderedPool);
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

    //! Flag set when job count was explicitly specified.
    bool IsExplicitJobCount;


    class TUnorderedTask
        : public TTask
    {
    public:
        //! For persistence only.
        TUnorderedTask()
            : Controller(nullptr)
        { }

        explicit TUnorderedTask(TUnorderedOperationControllerBase* controller)
            : TTask(controller)
            , Controller(controller)
        { }

        virtual TString GetId() const override
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

        virtual TExtendedJobResources GetNeededResources(const TJobletPtr& joblet) const override
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

        virtual TUserJobSpecPtr GetUserJobSpec() const override
        {
            return Controller->GetUserJobSpec();
        }

        virtual EJobType GetJobType() const override
        {
            return Controller->GetJobType();
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

            if (jobSummary.InterruptReason != EInterruptReason::None) {
                SplitByRowsAndReinstall(jobSummary.UnreadInputDataSlices, jobSummary.SplitJobCount);
            }
        }

        void SplitByRowsAndReinstall(
            const std::vector<TInputDataSlicePtr>& dataSlices,
            int jobCount)
        {
            i64 unreadRowCount = GetCumulativeRowCount(dataSlices);
            i64 rowsPerJob = DivCeil<i64>(unreadRowCount, jobCount);
            i64 rowsToAdd = rowsPerJob;
            int sliceIndex = 0;
            auto currentDataSlice = dataSlices[0];
            std::vector<TInputDataSlicePtr> jobSlices;
            while (true) {
                i64 sliceRowCount = currentDataSlice->GetRowCount();
                if (currentDataSlice->Type == EDataSourceType::UnversionedTable && sliceRowCount > rowsToAdd) {
                    auto split = currentDataSlice->SplitByRowIndex(rowsToAdd);
                    jobSlices.emplace_back(std::move(split.first));
                    rowsToAdd = 0;
                    currentDataSlice = std::move(split.second);
                } else {
                    jobSlices.emplace_back(std::move(currentDataSlice));
                    rowsToAdd -= sliceRowCount;
                    ++sliceIndex;
                    if (sliceIndex == static_cast<int>(dataSlices.size())) {
                        break;
                    }
                    currentDataSlice = dataSlices[sliceIndex];
                }
                if (rowsToAdd <= 0) {
                    ReinstallInputDataSlices(jobSlices);
                    jobSlices.clear();
                    rowsToAdd = rowsPerJob;
                }
            }
            if (!jobSlices.empty()) {
                ReinstallInputDataSlices(jobSlices);
            }
        }

        void ReinstallInputDataSlices(const std::vector<TInputDataSlicePtr>& inputDataSlices)
        {
            std::vector<TChunkStripePtr> stripes;
            auto chunkStripe = New<TChunkStripe>(false /*foreign*/, true /*solid*/);
            for (const auto& slice : inputDataSlices) {
                chunkStripe->DataSlices.push_back(slice);
            }
            stripes.emplace_back(std::move(chunkStripe));
            AddInput(stripes);
            FinishInput();
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

    void InitUnorderedPool(IJobSizeConstraintsPtr jobSizeConstraints, TJobSizeAdjusterConfigPtr jobSizeAdjusterConfig)
    {
        UnorderedPool = CreateUnorderedChunkPool(jobSizeConstraints, jobSizeAdjusterConfig);
    }

    virtual bool IsCompleted() const override
    {
        // Unordered task may be null, if all chunks were teleported.
        return !UnorderedTask || UnorderedTask->IsCompleted();
    }

    virtual void CustomPrepare() override
    {
        // The total data size for processing (except teleport chunks).
        i64 totalDataWeight = 0;
        i64 totalRowCount = 0;

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
                    RegisterTeleportChunk(chunk, currentPartitionIndex, 0);
                    ++currentPartitionIndex;
                } else {
                    mergedChunks.push_back(chunk);
                    totalDataWeight += chunk->GetDataWeight();
                    totalRowCount += chunk->GetRowCount();
                }
            }

            auto versionedInputStatistics = CalculatePrimaryVersionedChunksStatistics();
            totalDataWeight += versionedInputStatistics.first;
            totalRowCount += versionedInputStatistics.second;

            // Create the task, if any data.
            if (totalDataWeight > 0) {
                auto createJobSizeConstraints = [&] () -> IJobSizeConstraintsPtr {
                    switch (OperationType) {
                        case EOperationType::Merge:
                            return CreateMergeJobSizeConstraints(
                                Spec,
                                Options,
                                totalDataWeight,
                                static_cast<double>(TotalEstimatedInputCompressedDataSize) / TotalEstimatedInputDataWeight);

                        default:
                            return CreateSimpleJobSizeConstraints(
                                Spec,
                                Options,
                                GetOutputTablePaths().size(),
                                totalDataWeight,
                                totalRowCount);
                    }
                };

                auto jobSizeConstraints = createJobSizeConstraints();
                IsExplicitJobCount = jobSizeConstraints->IsExplicitJobCount();

                std::vector<TChunkStripePtr> stripes;
                SliceUnversionedChunks(mergedChunks, jobSizeConstraints, &stripes);
                SlicePrimaryVersionedChunks(jobSizeConstraints, &stripes);

                InitUnorderedPool(
                    std::move(jobSizeConstraints),
                    GetJobSizeAdjusterConfig());

                UnorderedTask = New<TUnorderedTask>(this);
                UnorderedTask->Initialize();
                UnorderedTask->AddInput(stripes);
                UnorderedTask->FinishInput();
                RegisterTask(UnorderedTask);

                LOG_INFO("Inputs processed (JobCount: %v, IsExplicitJobCount: %v)",
                    UnorderedTask->GetPendingJobCount(),
                    IsExplicitJobCount);
            } else {
                LOG_INFO("Inputs processed, all chunks were teleported");
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
    virtual TString GetLoggingProgress() const override
    {
        return Format(
            "Jobs = {T: %v, R: %v, C: %v, P: %v, F: %v, A: %v, I: %v}, "
            "UnavailableInputChunks: %v",
            JobCounter.GetTotal(),
            JobCounter.GetRunning(),
            JobCounter.GetCompletedTotal(),
            GetPendingJobCount(),
            JobCounter.GetFailed(),
            JobCounter.GetAbortedTotal(),
            JobCounter.GetInterruptedTotal(),
            GetUnavailableInputChunkCount());
    }


    // Unsorted helpers.
    virtual EJobType GetJobType() const = 0;

    virtual TJobSizeAdjusterConfigPtr GetJobSizeAdjusterConfig() const = 0;

    virtual TUserJobSpecPtr GetUserJobSpec() const
    {
        return nullptr;
    }

    virtual TCpuResource GetCpuLimit() const
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
    }

    //! Returns |true| if the chunk can be included into the output as-is.
    virtual bool IsTeleportChunk(const TInputChunkPtr& chunkSpec) const
    {
        return false;
    }

    virtual void PrepareInputQuery() override
    {
        if (Spec->InputQuery) {
            ParseInputQuery(*Spec->InputQuery, Spec->InputSchema);
        }
    }

    virtual void InitJobSpecTemplate()
    {
        JobSpecTemplate.set_type(static_cast<int>(GetJobType()));
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec->JobIO)).GetData());

        ToProto(schedulerJobSpecExt->mutable_data_source_directory(), MakeInputDataSources());
        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());

        if (Spec->InputQuery) {
            WriteInputQueryToJobSpec(schedulerJobSpecExt);
        }

        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig).GetData());
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TUnorderedOperationControllerBase::TUnorderedTask);

////////////////////////////////////////////////////////////////////////////////

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
        , Options(options)
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

protected:
    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        return STRINGBUF("data_weight_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::Map};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TMapController, 0xbac5fd82);

    TMapOperationSpecPtr Spec;
    TMapOperationOptionsPtr Options;

    i64 StartRowIndex = 0;


    // Custom bits of preparation pipeline.

    virtual EJobType GetJobType() const override
    {
        return EJobType::Map;
    }

    virtual TJobSizeAdjusterConfigPtr GetJobSizeAdjusterConfig() const override
    {
        return Config->EnableMapJobSizeAdjustment
            ? Options->JobSizeAdjuster
            : nullptr;
    }

    virtual TJobSplitterConfigPtr GetJobSplitterConfig() const override
    {
        return IsJobInterruptible() && Config->EnableJobSplitting && Spec->EnableJobSplitting
            ? Options->JobSplitter
            : nullptr;
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

    virtual TNullable<TRichYPath> GetCoreTablePath() const override
    {
        return Spec->CoreTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetCoreTableWriterConfig() const override
    {
        return Spec->CoreTableWriterConfig;
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
    virtual TCpuResource GetCpuLimit() const override
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

    virtual bool IsJobInterruptible() const override
    {
        // We don't let jobs to be interrupted if MaxOutputTablesTimesJobCount is too much overdrafted.
        return !IsExplicitJobCount &&
            2 * Options->MaxOutputTablesTimesJobsCount > JobCounter.GetTotal() * GetOutputTablePaths().size();
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TMapController);

////////////////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateMapController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TMapOperationSpec>(operation->GetSpec());
    return New<TMapController>(config, spec, config->MapOperationOptions, host, operation);
}

////////////////////////////////////////////////////////////////////////////////

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

protected:
    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        return STRINGBUF("data_weight_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::UnorderedMerge};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TUnorderedMergeController, 0x9a17a41f);

    TUnorderedMergeOperationSpecPtr Spec;


    // Custom bits of preparation pipeline.
    virtual EJobType GetJobType() const override
    {
        return EJobType::UnorderedMerge;
    }

    virtual TJobSizeAdjusterConfigPtr GetJobSizeAdjusterConfig() const override
    {
        return nullptr;
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

        if (Spec->ForceTransform ||
            Spec->InputQuery ||
            !isSchemaCompatible ||
            InputTables[chunkSpec->GetTableIndex()].Path.GetColumns())
        {
            return false;
        }

        return Spec->CombineChunks
            ? chunkSpec->IsLargeCompleteChunk(Spec->JobIO->TableWriter->DesiredChunkSize)
            : chunkSpec->IsCompleteChunk();
    }

    virtual void PrepareInputQuery() override
    {
        if (Spec->InputQuery) {
            ParseInputQuery(*Spec->InputQuery, Spec->InputSchema);
        }
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

        auto inferFromInput = [&] () {
            if (Spec->InputQuery) {
                table.TableUploadOptions.TableSchema = InputQuery->Query->GetTableSchema();
            } else {
                InferSchemaFromInput();
            }
        };

        switch (Spec->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    inferFromInput();
                } else {
                    validateOutputNotSorted();

                    if (!Spec->InputQuery) {
                        ValidateOutputSchemaCompatibility(true);
                    }
                }
                break;

            case ESchemaInferenceMode::FromInput:
                inferFromInput();
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

////////////////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateUnorderedMergeController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TUnorderedMergeOperationSpec>(operation->GetSpec());
    return New<TUnorderedMergeController>(config, spec, config->UnorderedMergeOperationOptions, host, operation);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT

