#include "unordered_controller.h"

#include "auto_merge_task.h"
#include "job_info.h"
#include "job_memory.h"
#include "helpers.h"
#include "operation_controller_detail.h"
#include "task.h"

#include <yt/server/controller_agent/chunk_list_pool.h>
#include <yt/server/controller_agent/helpers.h>
#include <yt/server/controller_agent/job_size_constraints.h>
#include <yt/server/controller_agent/operation.h>
#include <yt/server/controller_agent/config.h>

#include <yt/server/lib/legacy_chunk_pools/unordered_chunk_pool.h>
#include <yt/server/lib/legacy_chunk_pools/chunk_pool.h>

#include <yt/client/api/transaction.h>

#include <yt/ytlib/chunk_client/input_chunk_slice.h>
#include <yt/ytlib/chunk_client/input_data_slice.h>

#include <yt/ytlib/job_tracker_client/statistics.h>

#include <yt/ytlib/table_client/config.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/ytlib/query_client/query.h>

#include <yt/core/concurrency/periodic_yielder.h>

#include <yt/core/misc/numeric_helpers.h>

#include <util/generic/cast.h>

namespace NYT::NControllerAgent::NLegacyControllers {

using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NJobProxy;
using namespace NChunkClient;
using namespace NLegacyChunkPools;
using namespace NChunkClient::NProto;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient;
using namespace NJobTrackerClient::NProto;
using namespace NTableClient;
using namespace NConcurrency;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////////////////

static const NProfiling::TProfiler Profiler("/operations/unordered");

////////////////////////////////////////////////////////////////////////////////

class TUnorderedControllerBase
    : public TOperationControllerBase
{
public:
    class TUnorderedTaskBase
        : public TTask
    {
    public:
        //! For persistence only.
        TUnorderedTaskBase()
            : Controller_(nullptr)
        { }

        TUnorderedTaskBase(TUnorderedControllerBase* controller, std::vector<TEdgeDescriptor> edgeDescriptors)
            : TTask(controller, std::move(edgeDescriptors))
            , Controller_(controller)
        {
            auto options = Controller_->GetUnorderedChunkPoolOptions();
            options.Name = GetTitle();

            ChunkPool_ = CreateUnorderedChunkPool(std::move(options), Controller_->GetInputStreamDirectory());
            ChunkPool_->SubscribeChunkTeleported(BIND(&TUnorderedTaskBase::OnChunkTeleported, MakeWeak(this)));
        }

        virtual TTaskGroupPtr GetGroup() const override
        {
            return Controller_->UnorderedTaskGroup;
        }

        virtual TDuration GetLocalityTimeout() const override
        {
            return Controller_->IsLocalityEnabled()
                ? Controller_->Spec->LocalityTimeout
                : TDuration::Zero();
        }

        virtual TExtendedJobResources GetNeededResources(const TJobletPtr& joblet) const override
        {
            auto result = Controller_->GetUnorderedOperationResources(
                joblet->InputStripeList->GetStatistics());
            AddFootprintAndUserJobResources(result);
            return result;
        }

        virtual IChunkPoolInputPtr GetChunkPoolInput() const override
        {
            return ChunkPool_;
        }

        virtual IChunkPoolOutputPtr GetChunkPoolOutput() const override
        {
            return ChunkPool_;
        }

        virtual void Persist(const TPersistenceContext& context) override
        {
            TTask::Persist(context);

            using NYT::Persist;
            Persist(context, Controller_);
            Persist(context, ChunkPool_);
            Persist(context, TotalOutputRowCount_);

            ChunkPool_->SubscribeChunkTeleported(BIND(&TUnorderedTaskBase::OnChunkTeleported, MakeWeak(this)));
        }

        virtual TUserJobSpecPtr GetUserJobSpec() const override
        {
            return Controller_->GetUserJobSpec();
        }

        virtual EJobType GetJobType() const override
        {
            return Controller_->GetJobType();
        }

        virtual TJobFinishedResult OnJobCompleted(TJobletPtr joblet, TCompletedJobSummary& jobSummary) override
        {
            auto result = TTask::OnJobCompleted(joblet, jobSummary);
            TotalOutputRowCount_ += GetTotalOutputDataStatistics(*jobSummary.Statistics).row_count();

            RegisterOutput(&jobSummary.Result, joblet->ChunkListIds, joblet);

            return result;
        }

        i64 GetTotalOutputRowCount() const
        {
            return TotalOutputRowCount_;
        }

    private:
        TUnorderedControllerBase* Controller_;

        IChunkPoolPtr ChunkPool_;

        i64 TotalOutputRowCount_ = 0;

        virtual TExtendedJobResources GetMinNeededResourcesHeavy() const override
        {
            auto result = Controller_->GetUnorderedOperationResources(ChunkPool_->GetApproximateStripeStatistics());
            AddFootprintAndUserJobResources(result);
            return result;
        }

        virtual void BuildJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
        {
            jobSpec->CopyFrom(Controller_->JobSpecTemplate);
            AddSequentialInputSpec(jobSpec, joblet);
            AddOutputTableSpecs(jobSpec, joblet);
        }

        virtual void OnChunkTeleported(TInputChunkPtr teleportChunk, std::any tag) override
        {
            TTask::OnChunkTeleported(teleportChunk, tag);

            YT_VERIFY(GetJobType() == EJobType::UnorderedMerge);
            Controller_->RegisterTeleportChunk(std::move(teleportChunk), /*key=*/0, /*tableIndex=*/0);
        }
    };

    INHERIT_DYNAMIC_PHOENIX_TYPE(TUnorderedTaskBase, TUnorderedTask, 0x8ab75ee8);
    INHERIT_DYNAMIC_PHOENIX_TYPE_TEMPLATED(TAutoMergeableOutputMixin, TAutoMergeableUnorderedTask, 0x9a9bcee4, TUnorderedTaskBase);

    typedef TIntrusivePtr<TUnorderedTaskBase> TUnorderedTaskPtr;

    TUnorderedControllerBase(
        TUnorderedOperationSpecBasePtr spec,
        TControllerAgentConfigPtr config,
        TSimpleOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TOperationControllerBase(
            spec,
            config,
            options,
            host,
            operation)
        , Spec(spec)
        , Options(options)
    { }

    // Persistence.
    virtual void Persist(const TPersistenceContext& context) override
    {
        TOperationControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, Spec);
        Persist(context, JobIOConfig);
        Persist(context, JobSpecTemplate);
        Persist(context, JobSizeConstraints_);
        Persist(context, UnorderedTask_);
        Persist(context, UnorderedTaskGroup);
    }

protected:
    TUnorderedOperationSpecBasePtr Spec;
    TSimpleOperationOptionsPtr Options;

    //! Customized job IO config.
    TJobIOConfigPtr JobIOConfig;

    //! The template for starting new jobs.
    TJobSpec JobSpecTemplate;

    IJobSizeConstraintsPtr JobSizeConstraints_;

    TUnorderedTaskPtr UnorderedTask_;
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
        // Unordered task may be null, if all chunks were teleported.
        return TOperationControllerBase::IsCompleted() && (!UnorderedTask_ || UnorderedTask_->IsCompleted());
    }

    void InitTeleportableInputTables()
    {
        if (GetJobType() == EJobType::UnorderedMerge && !Spec->InputQuery) {
            for (int index = 0; index < InputTables_.size(); ++index) {
                if (!InputTables_[index]->Dynamic &&
                    !InputTables_[index]->Path.GetColumns() &&
                    InputTables_[index]->ColumnRenameDescriptors.empty() &&
                    OutputTables_[0]->TableUploadOptions.SchemaModification == ETableSchemaModification::None)
                {
                    InputTables_[index]->Teleportable = CheckTableSchemaCompatibility(
                        *InputTables_[index]->Schema,
                        *OutputTables_[0]->TableUploadOptions.TableSchema,
                        false /* ignoreSortOrder */).first == ESchemaCompatibility::FullyCompatible;
                }
            }
        }
    }

    virtual i64 GetMinTeleportChunkSize() const = 0;

    void InitJobSizeConstraints()
    {
        Spec->Sampling->MaxTotalSliceCount = Spec->Sampling->MaxTotalSliceCount.value_or(Config->MaxTotalSliceCount);

        switch (OperationType) {
            case EOperationType::Merge:
                JobSizeConstraints_ = CreateMergeJobSizeConstraints(
                    Spec,
                    Options,
                    Logger,
                    TotalEstimatedInputChunkCount,
                    PrimaryInputDataWeight,
                    DataWeightRatio,
                    InputCompressionRatio);
                break;

            case EOperationType::Map:
                JobSizeConstraints_ = CreateUserJobSizeConstraints(
                    Spec,
                    Options,
                    Logger,
                    GetOutputTablePaths().size(),
                    DataWeightRatio,
                    TotalEstimatedInputChunkCount,
                    PrimaryInputDataWeight,
                    TotalEstimatedInputRowCount);
                break;

            default:
                YT_ABORT();
        }

        YT_LOG_INFO(
            "Calculated operation parameters (JobCount: %v, DataWeightPerJob: %v, MaxDataWeightPerJob: %v, "
            "InputSliceDataWeight: %v, InputSliceRowCount: %v, IsExplicitJobCount: %v)",
            JobSizeConstraints_->GetJobCount(),
            JobSizeConstraints_->GetDataWeightPerJob(),
            JobSizeConstraints_->GetMaxDataWeightPerJob(),
            JobSizeConstraints_->GetInputSliceDataWeight(),
            JobSizeConstraints_->GetInputSliceRowCount(),
            JobSizeConstraints_->IsExplicitJobCount());
    }

    virtual TUnorderedChunkPoolOptions GetUnorderedChunkPoolOptions() const
    {
        TUnorderedChunkPoolOptions options;
        options.MinTeleportChunkSize = GetMinTeleportChunkSize();
        options.MinTeleportChunkDataWeight = options.MinTeleportChunkSize;
        options.OperationId = OperationId;
        options.JobSizeConstraints = JobSizeConstraints_;
        options.SliceErasureChunksByParts = Spec->SliceErasureChunksByParts;

        return options;
    }

    void ProcessInputs()
    {
        PROFILE_TIMING ("/input_processing_time") {
            YT_LOG_INFO("Processing inputs");

            TPeriodicYielder yielder(PrepareYieldPeriod);

            InitTeleportableInputTables();

            int unversionedSlices = 0;
            int versionedSlices = 0;
            for (auto& chunk : CollectPrimaryUnversionedChunks()) {
                const auto& slice = CreateUnversionedInputDataSlice(CreateInputChunkSlice(chunk));
                UnorderedTask_->AddInput(New<TChunkStripe>(std::move(slice)));
                ++unversionedSlices;
                yielder.TryYield();
            }
            for (auto& slice : CollectPrimaryVersionedDataSlices(JobSizeConstraints_->GetInputSliceDataWeight())) {
                UnorderedTask_->AddInput(New<TChunkStripe>(std::move(slice)));
                ++versionedSlices;
                yielder.TryYield();
            }

            YT_LOG_INFO("Processed inputs (UnversionedSlices: %v, VersionedSlices: %v)",
                unversionedSlices,
                versionedSlices);
        }
    }

    virtual void CustomPrepare() override
    {
        InitTeleportableInputTables();

        InitJobSizeConstraints();

        auto autoMergeEnabled = TryInitAutoMerge(JobSizeConstraints_->GetJobCount(), DataWeightRatio);

        if (autoMergeEnabled) {
            UnorderedTask_ = New<TAutoMergeableUnorderedTask>(this, GetAutoMergeEdgeDescriptors());
        } else {
            UnorderedTask_ = New<TUnorderedTask>(this, GetStandardEdgeDescriptors());
        }

        RegisterTask(UnorderedTask_);

        ProcessInputs();

        FinishTaskInput(UnorderedTask_);
        if (AutoMergeTask_) {
            AutoMergeTask_->FinishInput(UnorderedTask_->GetVertexDescriptor());
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

    virtual void OnChunksReleased(int chunkCount) override
    {
        TOperationControllerBase::OnChunksReleased(chunkCount);

        if (const auto& autoMergeDirector = GetAutoMergeDirector()) {
            autoMergeDirector->OnMergeJobFinished(chunkCount /* unregisteredIntermediateChunkCount */);
        }
    }

    virtual EIntermediateChunkUnstageMode GetIntermediateChunkUnstageMode() const override
    {
        auto mapperSpec = GetUserJobSpec();
        // We could get here only if this is an unordered map and auto-merge is enabled.
        YT_VERIFY(mapperSpec);
        YT_VERIFY(Spec->AutoMerge->Mode != EAutoMergeMode::Disabled);

        if (Spec->AutoMerge->Mode != EAutoMergeMode::Relaxed && mapperSpec->Deterministic) {
            return EIntermediateChunkUnstageMode::OnJobCompleted;
        } else {
            return EIntermediateChunkUnstageMode::OnSnapshotCompleted;
        }
    }

    // Unsorted helpers.
    virtual EJobType GetJobType() const = 0;

    virtual TUserJobSpecPtr GetUserJobSpec() const
    {
        return nullptr;
    }

    virtual TCpuResource GetCpuLimit() const
    {
        return 1;
    }

    void InitJobIOConfig()
    {
        JobIOConfig = CloneYsonSerializable(Spec->JobIO);
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

        SetDataSourceDirectory(schedulerJobSpecExt, BuildDataSourceDirectoryFromInputTables(InputTables_));

        if (Spec->InputQuery) {
            WriteInputQueryToJobSpec(schedulerJobSpecExt);
        }

        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig).GetData());
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TUnorderedControllerBase::TUnorderedTask);
DEFINE_DYNAMIC_PHOENIX_TYPE(TUnorderedControllerBase::TAutoMergeableUnorderedTask);

////////////////////////////////////////////////////////////////////////////////

class TMapController
    : public TUnorderedControllerBase
{
public:
    TMapController(
        TMapOperationSpecPtr spec,
        TControllerAgentConfigPtr config,
        TMapOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TUnorderedControllerBase(
            spec,
            config,
            options,
            host,
            operation)
        , Spec(spec)
        , Options(options)
    { }

    virtual void BuildBriefSpec(TFluentMap fluent) const override
    {
        TUnorderedControllerBase::BuildBriefSpec(fluent);
        fluent
            .Item("mapper").BeginMap()
                .Item("command").Value(TrimCommandForBriefSpec(Spec->Mapper->Command))
            .EndMap();
    }

    // Persistence.
    virtual void Persist(const TPersistenceContext& context) override
    {
        TUnorderedControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, Spec);
        Persist(context, StartRowIndex);
    }

protected:
    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        return AsStringBuf("data_weight_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::Map};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TMapController, 0xbac5fd83);

    TMapOperationSpecPtr Spec;
    TMapOperationOptionsPtr Options;

    i64 StartRowIndex = 0;

    // Custom bits of preparation pipeline.

    virtual EJobType GetJobType() const override
    {
        return EJobType::Map;
    }

    virtual TUnorderedChunkPoolOptions GetUnorderedChunkPoolOptions() const override
    {
        auto options = TUnorderedControllerBase::GetUnorderedChunkPoolOptions();
        if (Config->EnableMapJobSizeAdjustment) {
            options.JobSizeAdjusterConfig = Options->JobSizeAdjuster;
        }

        return options;
    }

    virtual TJobSplitterConfigPtr GetJobSplitterConfig() const override
    {
        return
            IsJobInterruptible() &&
            Config->EnableJobSplitting &&
            Spec->EnableJobSplitting &&
            InputTables_.size() <= Options->JobSplitter->MaxInputTableCount
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

    virtual std::optional<TRichYPath> GetStderrTablePath() const override
    {
        return Spec->StderrTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetStderrTableWriterConfig() const override
    {
        return Spec->StderrTableWriter;
    }

    virtual std::optional<TRichYPath> GetCoreTablePath() const override
    {
        return Spec->CoreTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetCoreTableWriterConfig() const override
    {
        return Spec->CoreTableWriter;
    }

    virtual bool GetEnableCudaGpuCoreDump() const override
    {
        return Spec->EnableCudaGpuCoreDump;
    }

    virtual std::vector<TUserJobSpecPtr> GetUserJobSpecs() const override
    {
        return {Spec->Mapper};
    }

    virtual void DoInitialize() override
    {
        TUnorderedControllerBase::DoInitialize();

        ValidateUserFileCount(Spec->Mapper, "mapper");
    }

    virtual ELegacyLivePreviewMode GetLegacyOutputLivePreviewMode() const override
    {
        return ToLegacyLivePreviewMode(Spec->EnableLegacyLivePreview);
    }

    // Unsorted helpers.
    virtual TCpuResource GetCpuLimit() const override
    {
        return TCpuResource(Spec->Mapper->CpuLimit);
    }

    virtual void InitJobSpecTemplate() override
    {
        TUnorderedControllerBase::InitJobSpecTemplate();
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        InitUserJobSpecTemplate(
            schedulerJobSpecExt->mutable_user_job_spec(),
            Spec->Mapper,
            UserJobFiles_[Spec->Mapper],
            Spec->JobNodeAccount);
    }

    virtual void CustomizeJoblet(const TJobletPtr& joblet) override
    {
        joblet->StartRowIndex = StartRowIndex;
        StartRowIndex += joblet->InputStripeList->TotalRowCount;
    }

    virtual i64 GetMinTeleportChunkSize() const override
    {
        return std::numeric_limits<i64>::max();
    }

    virtual bool IsInputDataSizeHistogramSupported() const override
    {
        return true;
    }

    virtual bool IsJobInterruptible() const override
    {
        // We don't let jobs to be interrupted if MaxOutputTablesTimesJobCount is too much overdrafted.
        auto totalJobCount = GetDataFlowGraph()->GetTotalJobCounter()->GetTotal();
        return
            !JobSizeConstraints_->IsExplicitJobCount() &&
            2 * Options->MaxOutputTablesTimesJobsCount > totalJobCount * GetOutputTablePaths().size() &&
            2 * Options->MaxJobCount > totalJobCount &&
            TOperationControllerBase::IsJobInterruptible();
    }

    virtual TYsonSerializablePtr GetTypedSpec() const override
    {
        return Spec;
    }

    virtual TError GetAutoMergeError() const override
    {
        return TError();
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TMapController);

////////////////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateUnorderedMapController(
    TControllerAgentConfigPtr config,
    IOperationControllerHostPtr host,
    TOperation* operation)
{
    auto options = config->MapOperationOptions;
    auto spec = ParseOperationSpec<TMapOperationSpec>(UpdateSpec(options->SpecTemplate, operation->GetSpec()));
    return New<TMapController>(spec, config, options, host, operation);
}

////////////////////////////////////////////////////////////////////////////////

class TUnorderedMergeController
    : public TUnorderedControllerBase
{
public:
    TUnorderedMergeController(
        TUnorderedMergeOperationSpecPtr spec,
        TControllerAgentConfigPtr config,
        TUnorderedMergeOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TUnorderedControllerBase(
            spec,
            config,
            options,
            host,
            operation)
        , Spec(spec)
    { }

protected:
    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        return AsStringBuf("data_weight_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::UnorderedMerge};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TUnorderedMergeController, 0x9a17a410);

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
        return !Spec->InputQuery &&
            !Spec->Sampling->SamplingRate &&
            !Spec->JobIO->TableReader->SamplingRate;
    }

    virtual i64 GetMinTeleportChunkSize() const override
    {
        if (Spec->ForceTransform) {
            return std::numeric_limits<i64>::max();
        }
        if (!Spec->CombineChunks) {
            return 0;
        }
        return Spec->JobIO->TableWriter->DesiredChunkSize;
    }

    virtual void PrepareInputQuery() override
    {
        if (Spec->InputQuery) {
            ParseInputQuery(*Spec->InputQuery, Spec->InputSchema);
        }
    }

    virtual void PrepareOutputTables() override
    {
        auto& table = OutputTables_[0];

        ValidateSchemaInferenceMode(Spec->SchemaInferenceMode);

        auto validateOutputNotSorted = [&] () {
            if (table->TableUploadOptions.TableSchema->IsSorted()) {
                THROW_ERROR_EXCEPTION("Cannot perform unordered merge into a sorted table in a \"strong\" schema mode")
                    << TErrorAttribute("schema", *table->TableUploadOptions.TableSchema);
            }
        };

        auto inferFromInput = [&] () {
            if (Spec->InputQuery) {
                table->TableUploadOptions.TableSchema = InputQuery->Query->GetTableSchema();
            } else {
                InferSchemaFromInput();
            }
        };

        switch (Spec->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table->TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
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
                YT_ABORT();
        }
    }

    virtual TYsonSerializablePtr GetTypedSpec() const override
    {
        return Spec;
    }

    virtual bool IsJobInterruptible() const override
    {
        return false;
    }

    virtual void OnOperationCompleted(bool interrupted) override
    {
        if (!interrupted) {
            auto isNontrivialInput = InputHasReadLimits() || InputHasVersionedTables();
            if (!isNontrivialInput && IsRowCountPreserved() && Spec->ForceTransform) {
                YT_LOG_ERROR_IF(TotalEstimatedInputRowCount != UnorderedTask_->GetTotalOutputRowCount(),
                    "Input/output row count mismatch in unordered merge operation (TotalEstimatedInputRowCount: %v, TotalOutputRowCount: %v)",
                     TotalEstimatedInputRowCount,
                     UnorderedTask_->GetTotalOutputRowCount());
                YT_VERIFY(TotalEstimatedInputRowCount == UnorderedTask_->GetTotalOutputRowCount());
            }
        }

        TUnorderedControllerBase::OnOperationCompleted(interrupted);
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TUnorderedMergeController);

////////////////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateUnorderedMergeController(
    TControllerAgentConfigPtr config,
    IOperationControllerHostPtr host,
    TOperation* operation)
{
    auto options = config->UnorderedMergeOperationOptions;
    auto spec = ParseOperationSpec<TUnorderedMergeOperationSpec>(UpdateSpec(options->SpecTemplate, operation->GetSpec()));
    return New<TUnorderedMergeController>(spec, config, options, host, operation);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent::NLegacyControllers
