#include "ordered_controller.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "helpers.h"
#include "job_info.h"
#include "job_size_constraints.h"
#include "operation_controller_detail.h"
#include "operation.h"
#include "config.h"

#include <yt/server/controller_agent/job_memory.h>

#include <yt/server/lib/chunk_pools/chunk_pool.h>
#include <yt/server/lib/chunk_pools/ordered_chunk_pool.h>

#include <yt/client/api/config.h>
#include <yt/client/api/transaction.h>

#include <yt/ytlib/api/native/connection.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_scraper.h>
#include <yt/ytlib/chunk_client/input_chunk_slice.h>
#include <yt/ytlib/chunk_client/input_data_slice.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/hive/cluster_directory.h>

#include <yt/ytlib/query_client/query.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/schema.h>
#include <yt/ytlib/table_client/table_upload_options.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/client/table_client/unversioned_row.h>

#include <yt/core/concurrency/periodic_yielder.h>

#include <yt/core/misc/numeric_helpers.h>

namespace NYT::NControllerAgent {

using namespace NApi;
using namespace NYTree;
using namespace NYPath;
using namespace NYson;
using namespace NJobProxy;
using namespace NChunkClient;
using namespace NChunkPools;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NScheduler::NProto;
using namespace NChunkClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NConcurrency;
using namespace NTableClient;
using namespace NScheduler;

using NYT::FromProto;
using NYT::ToProto;

using NChunkClient::TReadRange;
using NChunkClient::TReadLimit;

////////////////////////////////////////////////////////////////////////////////

static const NProfiling::TProfiler Profiler("/operations/merge");

////////////////////////////////////////////////////////////////////////////////

class TOrderedControllerBase
    : public TOperationControllerBase
{
public:
    TOrderedControllerBase(
        TSimpleOperationSpecBasePtr spec,
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
        , Spec_(spec)
        , Options_(options)
    { }

    // Persistence.

    virtual void Persist(const TPersistenceContext& context) override
    {
        TOperationControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, Spec_);
        Persist(context, Options_);
        Persist(context, JobIOConfig_);
        Persist(context, JobSpecTemplate_);
        Persist(context, JobSizeConstraints_);
        Persist(context, InputSliceDataWeight_);
        Persist(context, OrderedTaskGroup_);
        Persist(context, OrderedTask_);
        Persist(context, OrderedOutputRequired_);
        Persist(context, IsExplicitJobCount_);
    }

protected:
    TSimpleOperationSpecBasePtr Spec_;
    TSimpleOperationOptionsPtr Options_;

    //! Customized job IO config.
    TJobIOConfigPtr JobIOConfig_;

    //! The template for starting new jobs.
    TJobSpec JobSpecTemplate_;

    class TOrderedTask
        : public TTask
    {
    public:
        //! For persistence only.
        TOrderedTask()
            : Controller_(nullptr)
        { }

        TOrderedTask(TOrderedControllerBase* controller)
            : TTask(controller)
            , Controller_(controller)
        {
            auto options = controller->GetOrderedChunkPoolOptions();
            options.Task = GetTitle();
            ChunkPool_ = CreateOrderedChunkPool(options, controller->GetInputStreamDirectory());
        }

        virtual IChunkPoolInput* GetChunkPoolInput() const override
        {
            return ChunkPool_.get();
        }

        virtual IChunkPoolOutput* GetChunkPoolOutput() const override
        {
            return ChunkPool_.get();
        }

        virtual void Persist(const TPersistenceContext& context) override
        {
            TTask::Persist(context);

            using NYT::Persist;
            Persist(context, Controller_);
            Persist(context, ChunkPool_);
        }

    private:
        DECLARE_DYNAMIC_PHOENIX_TYPE(TOrderedTask, 0xaba78384);

        TOrderedControllerBase* Controller_;

        std::unique_ptr<IChunkPool> ChunkPool_;

        virtual TTaskGroupPtr GetGroup() const override
        {
            return Controller_->OrderedTaskGroup_;
        }

        virtual TDuration GetLocalityTimeout() const override
        {
            return Controller_->IsLocalityEnabled()
                ? Controller_->Spec_->LocalityTimeout
                : TDuration::Zero();
        }

        virtual TExtendedJobResources GetNeededResources(const TJobletPtr& joblet) const override
        {
            return GetMergeResources(joblet->InputStripeList->GetStatistics());
        }

        void BuildInputOutputJobSpec(TJobletPtr joblet, TJobSpec* jobSpec)
        {
            AddParallelInputSpec(jobSpec, joblet);
            AddOutputTableSpecs(jobSpec, joblet);
        }

        virtual TExtendedJobResources GetMinNeededResourcesHeavy() const override
        {
            return GetMergeResources(ChunkPool_->GetApproximateStripeStatistics());
        }

        TExtendedJobResources GetMergeResources(
            const TChunkStripeStatisticsVector& statistics) const
        {
            TExtendedJobResources result;
            result.SetUserSlots(1);
            result.SetCpu(Controller_->GetCpuLimit());
            result.SetJobProxyMemory(Controller_->GetFinalIOMemorySize(Controller_->Spec_->JobIO, statistics));
            AddFootprintAndUserJobResources(result);
            return result;
        }

        virtual EJobType GetJobType() const override
        {
            return Controller_->GetJobType();
        }

        virtual TUserJobSpecPtr GetUserJobSpec() const override
        {
            return Controller_->GetUserJobSpec();
        }

        virtual void BuildJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
        {
            jobSpec->CopyFrom(Controller_->JobSpecTemplate_);
            BuildInputOutputJobSpec(joblet, jobSpec);
        }

        virtual TJobFinishedResult OnJobCompleted(TJobletPtr joblet, TCompletedJobSummary& jobSummary) override
        {
            auto result = TTask::OnJobCompleted(joblet, jobSummary);

            TChunkStripeKey key = 0;
            if (Controller_->OrderedOutputRequired_) {
                key = TOutputOrder::TEntry(joblet->OutputCookie);
            }

            RegisterOutput(&jobSummary.Result, joblet->ChunkListIds, joblet, key);

            return result;
        }

        virtual TJobFinishedResult OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& jobSummary) override
        {
            return TTask::OnJobAborted(joblet, jobSummary);
        }
    };

    typedef TIntrusivePtr<TOrderedTask> TOrderedTaskPtr;

    TTaskGroupPtr OrderedTaskGroup_;

    TOrderedTaskPtr OrderedTask_;

    IJobSizeConstraintsPtr JobSizeConstraints_;

    i64 InputSliceDataWeight_;

    bool OrderedOutputRequired_ = false;

    bool IsExplicitJobCount_ = false;

    virtual EJobType GetJobType() const = 0;

    virtual void InitJobSpecTemplate() = 0;

    virtual bool IsTeleportationSupported() const = 0;

    virtual i64 GetMinTeleportChunkSize() = 0;

    virtual void ValidateInputDataSlice(const TInputDataSlicePtr& dataSlice)
    { }

    virtual TCpuResource GetCpuLimit() const
    {
        return 1;
    }

    virtual TUserJobSpecPtr GetUserJobSpec() const
    {
        return nullptr;
    }

    virtual bool IsCompleted() const override
    {
        return OrderedTask_->IsCompleted();
    }

    virtual void DoInitialize() override
    {
        TOperationControllerBase::DoInitialize();

        OrderedTaskGroup_ = New<TTaskGroup>();
        OrderedTaskGroup_->MinNeededResources.SetCpu(GetCpuLimit());

        RegisterTaskGroup(OrderedTaskGroup_);
    }

    void CalculateSizes()
    {
        Spec_->Sampling->MaxTotalSliceCount = Spec_->Sampling->MaxTotalSliceCount.value_or(Config->MaxTotalSliceCount);

        switch (OperationType) {
            case EOperationType::Merge:
            case EOperationType::Erase:
            case EOperationType::RemoteCopy:
                JobSizeConstraints_ = CreateMergeJobSizeConstraints(
                    Spec_,
                    Options_,
                    Logger,
                    TotalEstimatedInputChunkCount,
                    PrimaryInputDataWeight,
                    DataWeightRatio,
                    InputCompressionRatio);
                break;

            case EOperationType::Map:
                JobSizeConstraints_ = CreateUserJobSizeConstraints(
                    Spec_,
                    Options_,
                    Logger,
                    OutputTables_.size(),
                    DataWeightRatio,
                    TotalEstimatedInputChunkCount,
                    PrimaryInputDataWeight);
                break;

            default:
                YT_ABORT();
        }

        IsExplicitJobCount_ = JobSizeConstraints_->IsExplicitJobCount();

        InputSliceDataWeight_ = JobSizeConstraints_->GetInputSliceDataWeight();

        YT_LOG_INFO("Calculated operation parameters (JobCount: %v, MaxDataWeightPerJob: %v, InputSliceDataWeight: %v)",
            JobSizeConstraints_->GetJobCount(),
            JobSizeConstraints_->GetMaxDataWeightPerJob(),
            InputSliceDataWeight_);
    }

    // XXX(max42): this helper seems redundant.
    TChunkStripePtr CreateChunkStripe(TInputDataSlicePtr dataSlice)
    {
        TChunkStripePtr chunkStripe = New<TChunkStripe>(false /* foreign */);
        chunkStripe->DataSlices.emplace_back(std::move(dataSlice));
        return chunkStripe;
    }

    void ProcessInputs()
    {
        PROFILE_TIMING ("/input_processing_time") {
            YT_LOG_INFO("Processing inputs");

            TPeriodicYielder yielder(PrepareYieldPeriod);

            InitTeleportableInputTables();

            int sliceCount = 0;
            for (auto& slice : CollectPrimaryInputDataSlices(InputSliceDataWeight_)) {
                ValidateInputDataSlice(slice);
                OrderedTask_->AddInput(CreateChunkStripe(std::move(slice)));
                ++sliceCount;
                yielder.TryYield();
            }

            YT_LOG_INFO("Processed inputs (Slices: %v)", sliceCount);
        }
    }

    void FinishPreparation()
    {
        InitJobIOConfig();
        InitJobSpecTemplate();
    }

    //! Initializes #JobIOConfig.
    void InitJobIOConfig()
    {
        JobIOConfig_ = CloneYsonSerializable(Spec_->JobIO);
    }

    void InitTeleportableInputTables()
    {
        if (IsTeleportationSupported()) {
            for (int index = 0; index < InputTables_.size(); ++index) {
                if (!InputTables_[index]->Dynamic &&
                    !InputTables_[index]->Path.GetColumns() &&
                    InputTables_[index]->ColumnRenameDescriptors.empty() &&
                    OutputTables_[0]->TableUploadOptions.SchemaModification == ETableSchemaModification::None)
                {
                    InputTables_[index]->Teleportable = ValidateTableSchemaCompatibility(
                        InputTables_[index]->Schema,
                        OutputTables_[0]->TableUploadOptions.TableSchema,
                        false /* ignoreSortOrder */).IsOK();
                }
            }
        }
    }

    virtual TOutputOrderPtr GetOutputOrder() const override
    {
        return OrderedTask_->GetChunkPoolOutput()->GetOutputOrder();
    }

    virtual void CustomPrepare() override
    {
        // NB: Base member is not called intentionally.

        CalculateSizes();

        InitTeleportableInputTables();

        if (!ShouldVerifySortedOutput()) {
            OrderedOutputRequired_ = true;
        }

        for (const auto& table : OutputTables_) {
            if (!table->TableUploadOptions.TableSchema.IsSorted()) {
                OrderedOutputRequired_ = true;
            }
        }

        OrderedTask_ = New<TOrderedTask>(this);
        RegisterTask(OrderedTask_);

        ProcessInputs();

        FinishTaskInput(OrderedTask_);

        for (const auto& teleportChunk : OrderedTask_->GetChunkPoolOutput()->GetTeleportChunks()) {
            if (OrderedOutputRequired_) {
                RegisterTeleportChunk(
                    teleportChunk,
                    TOutputOrder::TEntry(teleportChunk) /* key */,
                    0 /* tableIndex */);
            } else {
                RegisterTeleportChunk(
                    teleportChunk,
                    0 /* key */,
                    0 /* tableIndex */);
            }
        }

        FinishPreparation();
    }

    TOrderedChunkPoolOptions GetOrderedChunkPoolOptions()
    {
        TOrderedChunkPoolOptions chunkPoolOptions;
        chunkPoolOptions.MaxTotalSliceCount = Config->MaxTotalSliceCount;
        chunkPoolOptions.EnablePeriodicYielder = true;
        chunkPoolOptions.MinTeleportChunkSize = GetMinTeleportChunkSize();
        chunkPoolOptions.JobSizeConstraints = JobSizeConstraints_;
        chunkPoolOptions.OperationId = OperationId;
        chunkPoolOptions.KeepOutputOrder = OrderedOutputRequired_;
        chunkPoolOptions.ShouldSliceByRowIndices = GetJobType() != EJobType::RemoteCopy;
        return chunkPoolOptions;
    }

    virtual TJobSplitterConfigPtr GetJobSplitterConfig() const override
    {
        return
            IsJobInterruptible() &&
            Config->EnableJobSplitting &&
            Spec_->EnableJobSplitting &&
            InputTables_.size() <= Options_->JobSplitter->MaxInputTableCount
            ? Options_->JobSplitter
            : nullptr;
    }

    virtual bool IsJobInterruptible() const override
    {
        // We don't let jobs to be interrupted if MaxOutputTablesTimesJobCount is too much overdrafted.
        auto totalJobCount = GetDataFlowGraph()->GetTotalJobCounter()->GetTotal();
        return !IsExplicitJobCount_ &&
               2 * Options_->MaxOutputTablesTimesJobsCount > totalJobCount * GetOutputTablePaths().size() &&
               2 * Options_->MaxJobCount > totalJobCount &&
               TOperationControllerBase::IsJobInterruptible();
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TOrderedControllerBase::TOrderedTask);

////////////////////////////////////////////////////////////////////////////////

class TOrderedMergeController
    : public TOrderedControllerBase
{
public:
    TOrderedMergeController(
        TOrderedMergeOperationSpecPtr spec,
        TControllerAgentConfigPtr config,
        TSimpleOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TOrderedControllerBase(
            spec,
            config,
            options,
            host,
            operation)
        , Spec_(spec)
    { }

    virtual void Persist(const TPersistenceContext& context) override
    {
        TOrderedControllerBase::Persist(context);

        using NYT::Persist;

        Persist(context, Spec_);
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TOrderedMergeController, 0xe7098bca);

    TOrderedMergeOperationSpecPtr Spec_;

    virtual bool IsRowCountPreserved() const override
    {
        return !Spec_->InputQuery;
    }

    virtual i64 GetMinTeleportChunkSize() override
    {
        if (Spec_->ForceTransform || Spec_->InputQuery) {
            return std::numeric_limits<i64>::max();
        }
        if (!Spec_->CombineChunks) {
            return 0;
        }
        return Spec_->JobIO
            ->TableWriter
            ->DesiredChunkSize;
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::OrderedMerge;
    }

    virtual void PrepareInputQuery() override
    {
        if (Spec_->InputQuery) {
            ParseInputQuery(*Spec_->InputQuery, Spec_->InputSchema);
        }
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec_->InputTablePaths;
    }

    virtual bool IsBoundaryKeysFetchEnabled() const override
    {
        // Required for chunk teleporting in case of sorted output.
        return OutputTables_[0]->TableUploadOptions.TableSchema.IsSorted();
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return {Spec_->OutputTablePath};
    }

    virtual void InitJobSpecTemplate() override
    {
        JobSpecTemplate_.set_type(static_cast<int>(EJobType::OrderedMerge));
        auto* schedulerJobSpecExt = JobSpecTemplate_.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec_->JobIO)).GetData());

        if (Spec_->InputQuery) {
            WriteInputQueryToJobSpec(schedulerJobSpecExt);
        }

        SetDataSourceDirectory(schedulerJobSpecExt, BuildDataSourceDirectoryFromInputTables(InputTables_));
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig_).GetData());
    }

    virtual bool IsTeleportationSupported() const override
    {
        return true;
    }

    virtual void PrepareOutputTables() override
    {
        auto& table = OutputTables_[0];

        auto inferFromInput = [&] () {
            if (Spec_->InputQuery) {
                table->TableUploadOptions.TableSchema = InputQuery->Query->GetTableSchema();
            } else {
                InferSchemaFromInputOrdered();
            }
        };

        switch (Spec_->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table->TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    inferFromInput();
                } else {
                    ValidateOutputSchemaOrdered();
                    if (!Spec_->InputQuery) {
                        ValidateOutputSchemaCompatibility(false);
                    }
                }
                break;

            case ESchemaInferenceMode::FromInput:
                inferFromInput();
                break;

            case ESchemaInferenceMode::FromOutput:
                break;

            default:
                YT_ABORT();
        }
    }

    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        return AsStringBuf("data_weight_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::OrderedMerge};
    }

    virtual TYsonSerializablePtr GetTypedSpec() const override
    {
        return Spec_;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TOrderedMergeController);

IOperationControllerPtr CreateOrderedMergeController(
    TControllerAgentConfigPtr config,
    IOperationControllerHostPtr host,
    TOperation* operation)
{
    auto options = config->OrderedMergeOperationOptions;
    auto spec = ParseOperationSpec<TOrderedMergeOperationSpec>(UpdateSpec(options->SpecTemplate, operation->GetSpec()));
    return New<TOrderedMergeController>(spec, config, options, host, operation);
}

////////////////////////////////////////////////////////////////////////////////

class TOrderedMapController
    : public TOrderedControllerBase
{
public:
    TOrderedMapController(
        TMapOperationSpecPtr spec,
        TControllerAgentConfigPtr config,
        TMapOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TOrderedControllerBase(
            spec,
            config,
            options,
            host,
            operation)
        , Spec_(spec)
        , Options_(options)
    { }

    virtual void Persist(const TPersistenceContext& context) override
    {
        TOrderedControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, Spec_);
        Persist(context, Options_);
        Persist(context, StartRowIndex_);
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TOrderedMapController, 0x3be901ca);

    i64 StartRowIndex_ = 0;

    TMapOperationSpecPtr Spec_;
    TMapOperationOptionsPtr Options_;

    virtual bool IsRowCountPreserved() const override
    {
        return false;
    }

    virtual TUserJobSpecPtr GetUserJobSpec() const
    {
        return Spec_->Mapper;
    }

    virtual i64 GetMinTeleportChunkSize() override
    {
        return std::numeric_limits<i64>::max();
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::OrderedMap;
    }

    virtual TCpuResource GetCpuLimit() const override
    {
        return Spec_->Mapper->CpuLimit;
    }

    virtual void BuildBriefSpec(TFluentMap fluent) const override
    {
        TOperationControllerBase::BuildBriefSpec(fluent);
        fluent
            .Item("mapper").BeginMap()
                .Item("command").Value(TrimCommandForBriefSpec(Spec_->Mapper->Command))
            .EndMap();
    }

    virtual void CustomizeJoblet(const TJobletPtr& joblet) override
    {
        joblet->StartRowIndex = StartRowIndex_;
        StartRowIndex_ += joblet->InputStripeList->TotalRowCount;
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec_->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return Spec_->OutputTablePaths;
    }

    virtual std::optional<TRichYPath> GetStderrTablePath() const override
    {
        return Spec_->StderrTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetStderrTableWriterConfig() const override
    {
        return Spec_->StderrTableWriter;
    }

    virtual std::optional<TRichYPath> GetCoreTablePath() const override
    {
        return Spec_->CoreTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetCoreTableWriterConfig() const override
    {
        return Spec_->CoreTableWriter;
    }

    virtual void InitJobSpecTemplate() override
    {
        JobSpecTemplate_.set_type(static_cast<int>(EJobType::OrderedMap));
        auto* schedulerJobSpecExt = JobSpecTemplate_.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec_->JobIO)).GetData());

        SetDataSourceDirectory(schedulerJobSpecExt, BuildDataSourceDirectoryFromInputTables(InputTables_));

        if (Spec_->InputQuery) {
            WriteInputQueryToJobSpec(schedulerJobSpecExt);
        }

        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig_).GetData());

        InitUserJobSpecTemplate(
            schedulerJobSpecExt->mutable_user_job_spec(),
            Spec_->Mapper,
            UserJobFiles_[Spec_->Mapper],
            Spec_->JobNodeAccount);
    }

    virtual bool IsTeleportationSupported() const override
    {
        return false;
    }

    virtual void PrepareInputQuery() override
    {
        if (Spec_->InputQuery) {
            ParseInputQuery(*Spec_->InputQuery, Spec_->InputSchema);
        }
    }

    virtual ELegacyLivePreviewMode GetLegacyOutputLivePreviewMode() const override
    {
        return ToLegacyLivePreviewMode(Spec_->EnableLegacyLivePreview);
    }

    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        return AsStringBuf("data_weight_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::OrderedMap};
    }

    virtual std::vector<TUserJobSpecPtr> GetUserJobSpecs() const override
    {
        return {Spec_->Mapper};
    }

    virtual void DoInitialize() override
    {
        TOrderedControllerBase::DoInitialize();

        ValidateUserFileCount(Spec_->Mapper, "mapper");
    }

    virtual TYsonSerializablePtr GetTypedSpec() const override
    {
        return Spec_;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TOrderedMapController);

IOperationControllerPtr CreateOrderedMapController(
    TControllerAgentConfigPtr config,
    IOperationControllerHostPtr host,
    TOperation* operation)
{
    auto options = config->MapOperationOptions;
    auto spec = ParseOperationSpec<TMapOperationSpec>(UpdateSpec(options->SpecTemplate, operation->GetSpec()));
    return New<TOrderedMapController>(spec, config, options, host, operation);
}

////////////////////////////////////////////////////////////////////////////////

class TEraseController
    : public TOrderedControllerBase
{
public:
    TEraseController(
        TEraseOperationSpecPtr spec,
        TControllerAgentConfigPtr config,
        TSimpleOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TOrderedControllerBase(
            spec,
            config,
            options,
            host,
            operation)
        , Spec_(spec)
    { }

    virtual void Persist(const TPersistenceContext& context) override
    {
        TOrderedControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, Spec_);
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TEraseController, 0xfbb39ac0);

    TEraseOperationSpecPtr Spec_;

    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        YT_ABORT();
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {};
    }

    bool IsTeleportationSupported() const override
    {
        return true;
    }

    virtual void BuildBriefSpec(TFluentMap fluent) const override
    {
        TOrderedControllerBase::BuildBriefSpec(fluent);
        fluent
            // In addition to "input_table_paths" and "output_table_paths".
            // Quite messy, only needed for consistency with the regular spec.
            .Item("table_path").Value(Spec_->TablePath);
    }

    virtual bool IsRowCountPreserved() const override
    {
        return false;
    }

    virtual i64 GetMinTeleportChunkSize() override
    {
        if (!Spec_->CombineChunks) {
            return 0;
        }
        return Spec_->JobIO
            ->TableWriter
            ->DesiredChunkSize;
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return {Spec_->TablePath};
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return {Spec_->TablePath};
    }

    virtual void DoInitialize() override
    {
        TOrderedControllerBase::DoInitialize();

        auto& path = InputTables_[0]->Path;
        auto ranges = path.GetRanges();
        if (ranges.size() > 1) {
            THROW_ERROR_EXCEPTION("Erase operation does not support tables with multiple ranges");
        }
        if (path.GetColumns()) {
            THROW_ERROR_EXCEPTION("Erase operation does not support column filtering");
        }

        if (ranges.size() == 1) {
            std::vector<TReadRange> complementaryRanges;
            const auto& range = ranges[0];
            if (!range.LowerLimit().IsTrivial()) {
                complementaryRanges.push_back(TReadRange(TReadLimit(), range.LowerLimit()));
            }
            if (!range.UpperLimit().IsTrivial()) {
                complementaryRanges.push_back(TReadRange(range.UpperLimit(), TReadLimit()));
            }
            path.SetRanges(complementaryRanges);
        } else {
            path.SetRanges(std::vector<TReadRange>());
        }
    }

    virtual bool IsBoundaryKeysFetchEnabled() const override
    {
        // Required for chunk teleporting in case of sorted output.
        return OutputTables_[0]->TableUploadOptions.TableSchema.IsSorted();
    }

    virtual void PrepareOutputTables() override
    {
        auto& table = OutputTables_[0];
        table->TableUploadOptions.UpdateMode = EUpdateMode::Overwrite;
        table->TableUploadOptions.LockMode = ELockMode::Exclusive;

        // Erase output MUST be sorted.
        if (Spec_->SchemaInferenceMode != ESchemaInferenceMode::FromOutput) {
            table->TableWriterOptions->ExplodeOnValidationError = true;
        }

        switch (Spec_->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table->TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    InferSchemaFromInputOrdered();
                } else {
                    if (InputTables_[0]->SchemaMode == ETableSchemaMode::Strong) {
                        ValidateTableSchemaCompatibility(
                            InputTables_[0]->Schema,
                            table->TableUploadOptions.TableSchema,
                            /* ignoreSortOrder */ false)
                            .ThrowOnError();
                    }
                }
                break;

            case ESchemaInferenceMode::FromInput:
                InferSchemaFromInputOrdered();
                break;

            case ESchemaInferenceMode::FromOutput:
                break;

            default:
                YT_ABORT();
        }
    }

    virtual void InitJobSpecTemplate() override
    {
        JobSpecTemplate_.set_type(static_cast<int>(EJobType::OrderedMerge));
        auto* schedulerJobSpecExt = JobSpecTemplate_.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec_->JobIO)).GetData());

        SetDataSourceDirectory(schedulerJobSpecExt, BuildDataSourceDirectoryFromInputTables(InputTables_));

        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig_).GetData());

        auto* jobSpecExt = JobSpecTemplate_.MutableExtension(TMergeJobSpecExt::merge_job_spec_ext);
        const auto& table = OutputTables_[0];
        if (table->TableUploadOptions.TableSchema.IsSorted()) {
            ToProto(jobSpecExt->mutable_key_columns(), table->TableUploadOptions.TableSchema.GetKeyColumns());
        }
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::OrderedMerge;
    }

    virtual TYsonSerializablePtr GetTypedSpec() const override
    {
        return Spec_;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TEraseController);

IOperationControllerPtr CreateEraseController(
    TControllerAgentConfigPtr config,
    IOperationControllerHostPtr host,
    TOperation* operation)
{
    auto options = config->EraseOperationOptions;
    auto spec = ParseOperationSpec<TEraseOperationSpec>(UpdateSpec(options->SpecTemplate, operation->GetSpec()));
    return New<TEraseController>(spec, config, options, host, operation);
}

////////////////////////////////////////////////////////////////////////////////

class TRemoteCopyController
    : public TOrderedControllerBase
{
public:
    TRemoteCopyController(
        TRemoteCopyOperationSpecPtr spec,
        TControllerAgentConfigPtr config,
        TRemoteCopyOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TOrderedControllerBase(
            spec,
            config,
            options,
            host,
            operation)
        , Spec_(spec)
        , Options_(options)
    { }

    virtual void Persist(const TPersistenceContext& context) override
    {
        TOrderedControllerBase::Persist(context);
        using NYT::Persist;

        Persist(context, Spec_);
        Persist(context, Options_);
        Persist<TAttributeDictionaryRefSerializer>(context, InputTableAttributes_);
    }

    virtual IJobSplitter* GetJobSplitter() override
    {
        return nullptr;
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TRemoteCopyController, 0xaa8829a9);

    TRemoteCopyOperationSpecPtr Spec_;
    TRemoteCopyOperationOptionsPtr Options_;

    std::unique_ptr<IAttributeDictionary> InputTableAttributes_;

    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        YT_ABORT();
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {};
    }

    virtual bool ShouldVerifySortedOutput() const override
    {
        return false;
    }

    virtual void BuildBriefSpec(TFluentMap fluent) const override
    {
        TOperationControllerBase::BuildBriefSpec(fluent);
        fluent
            .Item("cluster_name").Value(Spec_->ClusterName)
            .Item("network_name").Value(Spec_->NetworkName);
    }

    // Custom bits of preparation pipeline.
    virtual TTransactionId GetInputTransactionParentId() override
    {
        return {};
    }

    virtual void InitializeClients() override
    {
        TOperationControllerBase::InitializeClients();

        TClientOptions options;
        options.PinnedUser = AuthenticatedUser;
        InputClient = GetRemoteConnection()->CreateNativeClient(options);
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec_->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return {Spec_->OutputTablePath};
    }

    virtual void PrepareOutputTables() override
    {
        auto& table = OutputTables_[0];

        switch (Spec_->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table->TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    InferSchemaFromInputOrdered();
                    break;
                }
                // We intentionally fall into next clause.

            case ESchemaInferenceMode::FromOutput:
                ValidateOutputSchemaOrdered();

                // Since remote copy doesn't unpack blocks and validate schema, we must ensure
                // that schemas are identical.
                for (const auto& inputTable : InputTables_) {
                    if (table->TableUploadOptions.SchemaMode == ETableSchemaMode::Strong &&
                        inputTable->Schema.ToCanonical() != table->TableUploadOptions.TableSchema.ToCanonical())
                    {
                        THROW_ERROR_EXCEPTION("Cannot make remote copy into table with \"strong\" schema since "
                            "input table schema differs from output table schema")
                            << TErrorAttribute("input_table_schema", inputTable->Schema)
                            << TErrorAttribute("output_table_schema", table->TableUploadOptions.TableSchema);
                    }
                }
                break;

            case ESchemaInferenceMode::FromInput:
                InferSchemaFromInputOrdered();
                break;
        }
    }

    virtual void ValidateInputDataSlice(const TInputDataSlicePtr& dataSlice) override
    {
        if (!dataSlice->IsTrivial()) {
            THROW_ERROR_EXCEPTION("Remote copy operation supports only unversioned tables");
        }
        const auto& chunk = dataSlice->GetSingleUnversionedChunkOrThrow();
        if ((chunk->LowerLimit() && !IsTrivial(*chunk->LowerLimit())) ||
            (chunk->UpperLimit() && !IsTrivial(*chunk->UpperLimit())))
        {
            THROW_ERROR_EXCEPTION("Remote copy operation does not support non-trivial table limits");
        }
    }

    virtual void CustomPrepare() override
    {
        if (Spec_->CopyAttributes) {
            if (InputTables_.size() != 1) {
                THROW_ERROR_EXCEPTION("Attributes can be copied only in case of one input table");
            }

            const auto& table = InputTables_[0];

            auto channel = InputClient->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
            TObjectServiceProxy proxy(channel);

            auto req = TObjectYPathProxy::Get(table->GetObjectIdPath() + "/@");
            SetTransactionId(req, *table->TransactionId);

            auto rspOrError = WaitFor(proxy.Execute(req));
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting attributes of input table %v",
                table->GetPath());

            const auto& rsp = rspOrError.Value();
            InputTableAttributes_ = ConvertToAttributes(TYsonString(rsp->value()));
        }

        TOrderedControllerBase::CustomPrepare();
    }

    virtual void CustomCommit() override
    {
        TOperationControllerBase::CustomCommit();

        if (Spec_->CopyAttributes) {
            const auto& path = Spec_->OutputTablePath.GetPath();

            auto channel = OutputClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
            TObjectServiceProxy proxy(channel);

            auto userAttributeKeys = InputTableAttributes_->Get<std::vector<TString>>("user_attribute_keys");
            auto attributeKeys = Spec_->AttributeKeys.value_or(userAttributeKeys);

            auto batchReq = proxy.ExecuteBatch();
            for (const auto& key : attributeKeys) {
                auto req = TYPathProxy::Set(path + "/@" + key);
                req->set_value(InputTableAttributes_->GetYson(key).GetData());
                SetTransactionId(req, OutputCompletionTransaction->GetId());
                batchReq->AddRequest(req);
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error setting attributes for output table %v",
                path);
        }
    }

    void InitJobSpecTemplate()
    {
        JobSpecTemplate_.set_type(static_cast<int>(EJobType::RemoteCopy));
        auto* schedulerJobSpecExt = JobSpecTemplate_.MutableExtension(
            TSchedulerJobSpecExt::scheduler_job_spec_ext);

        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig_).GetData());
        schedulerJobSpecExt->set_table_reader_options("");
        SetDataSourceDirectory(schedulerJobSpecExt, BuildDataSourceDirectoryFromInputTables(InputTables_));

        auto connectionConfig = CloneYsonSerializable(GetRemoteConnectionConfig());
        if (Spec_->NetworkName) {
            connectionConfig->Networks = {*Spec_->NetworkName};
        }

        auto* remoteCopyJobSpecExt = JobSpecTemplate_.MutableExtension(TRemoteCopyJobSpecExt::remote_copy_job_spec_ext);
        remoteCopyJobSpecExt->set_connection_config(ConvertToYsonString(connectionConfig).GetData());
        remoteCopyJobSpecExt->set_concurrency(Spec_->Concurrency);
        remoteCopyJobSpecExt->set_block_buffer_size(Spec_->BlockBufferSize);
    }

    NNative::IConnectionPtr GetRemoteConnection() const
    {
        if (Spec_->ClusterConnection) {
            return NApi::NNative::CreateConnection(*Spec_->ClusterConnection);
        } else if (Spec_->ClusterName) {
            auto connection = Host
                ->GetClient()
                ->GetNativeConnection()
                ->GetClusterDirectory()
                ->GetConnectionOrThrow(*Spec_->ClusterName);

            auto* nativeConnection = dynamic_cast<NNative::IConnection*>(connection.Get());
            if (!nativeConnection) {
                THROW_ERROR_EXCEPTION("No native connection could be established with cluster %Qv",
                    *Spec_->ClusterName);
            }

            return nativeConnection;
        } else {
            THROW_ERROR_EXCEPTION("No remote cluster is specified");
        }
    }

    NNative::TConnectionConfigPtr GetRemoteConnectionConfig() const
    {
        if (Spec_->ClusterConnection) {
            return *Spec_->ClusterConnection;
        } else if (Spec_->ClusterName) {
            return GetRemoteConnection()->GetConfig();
        } else {
            THROW_ERROR_EXCEPTION("No remote cluster is specified");
        }
    }

    virtual bool CheckParityReplicas() const override
    {
        return true;
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::RemoteCopy;
    }

    virtual bool IsTeleportationSupported() const override
    {
        return false;
    }

    virtual i64 GetMinTeleportChunkSize() override
    {
        return std::numeric_limits<i64>::max();
    }

    virtual TYsonSerializablePtr GetTypedSpec() const override
    {
        return Spec_;
    }

    virtual bool IsJobInterruptible() const override
    {
        return false;
    }

    virtual TCpuResource GetCpuLimit() const
    {
        return Options_->CpuLimit;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TRemoteCopyController);

IOperationControllerPtr CreateRemoteCopyController(
    TControllerAgentConfigPtr config,
    IOperationControllerHostPtr host,
    TOperation* operation)
{
    auto options = config->RemoteCopyOperationOptions;
    auto spec = ParseOperationSpec<TRemoteCopyOperationSpec>(UpdateSpec(options->SpecTemplate, operation->GetSpec()));
    return New<TRemoteCopyController>(spec, config, options, host, operation);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
