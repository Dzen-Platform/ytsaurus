#include "sorted_controller.h"

#include "auto_merge_task.h"
#include "chunk_list_pool.h"
#include "helpers.h"
#include "job_info.h"
#include "job_size_constraints.h"
#include "operation_controller_detail.h"
#include "task.h"
#include "operation.h"
#include "config.h"

#include <yt/server/controller_agent/job_memory.h>

#include <yt/server/lib/chunk_pools/chunk_pool.h>
#include <yt/server/lib/chunk_pools/sorted_chunk_pool.h>

#include <yt/client/api/transaction.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_scraper.h>
#include <yt/ytlib/chunk_client/input_chunk_slice.h>
#include <yt/ytlib/chunk_client/input_data_slice.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/chunk_slice_fetcher.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/client/table_client/unversioned_row.h>

#include <yt/core/concurrency/periodic_yielder.h>

#include <yt/core/misc/numeric_helpers.h>

namespace NYT::NControllerAgent {

using namespace NYTree;
using namespace NYPath;
using namespace NYson;
using namespace NJobProxy;
using namespace NChunkClient;
using namespace NChunkPools;
using namespace NObjectClient;
using namespace NCypressClient;
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
using NTableClient::TKey;

////////////////////////////////////////////////////////////////////////////////

static const NProfiling::TProfiler Profiler("/operations/merge");

////////////////////////////////////////////////////////////////////////////////

// TODO(max42): support Config->MaxTotalSliceCount
// TODO(max42): reorder virtual methods in public section.

class TSortedControllerBase
    : public TOperationControllerBase
{
public:
    TSortedControllerBase(
        TSimpleOperationSpecBasePtr spec,
        TControllerAgentConfigPtr config,
        TSimpleOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TOperationControllerBase(
            spec,
            std::move(config),
            options,
            std::move(host),
            operation)
        , Spec_(std::move(spec))
        , Options_(std::move(options))
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
        Persist(context, SortedTaskGroup_);
        Persist(context, SortedTask_);
        Persist(context, PrimaryKeyColumns_);
        Persist(context, ForeignKeyColumns_);
    }

protected:
    TSimpleOperationSpecBasePtr Spec_;
    TSimpleOperationOptionsPtr Options_;

    //! Customized job IO config.
    TJobIOConfigPtr JobIOConfig_;

    //! The template for starting new jobs.
    TJobSpec JobSpecTemplate_;

    class TSortedTaskBase
        : public TTask
    {
    public:
        //! For persistence only.
        TSortedTaskBase()
            : Controller_(nullptr)
        { }

        TSortedTaskBase(TSortedControllerBase* controller, std::vector<TEdgeDescriptor> edgeDescriptors)
            : TTask(controller, std::move(edgeDescriptors))
            , Controller_(controller)
        {
            auto options = controller->GetSortedChunkPoolOptions();
            options.Task = GetTitle();
            ChunkPool_ = CreateSortedChunkPool(
                options,
                controller->CreateChunkSliceFetcherFactory(),
                controller->GetInputStreamDirectory());

        }

        virtual TTaskGroupPtr GetGroup() const override
        {
            return Controller_->SortedTaskGroup_;
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

    protected:
        TSortedControllerBase* Controller_;

        //! Initialized in descendandt tasks.
        std::unique_ptr<IChunkPool> ChunkPool_;

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

            RegisterOutput(&jobSummary.Result, joblet->ChunkListIds, joblet);

            return result;
        }

        virtual TJobFinishedResult OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& jobSummary) override
        {
            return TTask::OnJobAborted(joblet, jobSummary);
        }
    };

    INHERIT_DYNAMIC_PHOENIX_TYPE(TSortedTaskBase, TSortedTask, 0xbbe534a7);
    INHERIT_DYNAMIC_PHOENIX_TYPE_TEMPLATED(TAutoMergeableOutputMixin, TAutoMergeableSortedTask, 0x1233fa99, TSortedTaskBase);

    typedef TIntrusivePtr<TSortedTaskBase> TSortedTaskPtr;

    TTaskGroupPtr SortedTaskGroup_;

    TSortedTaskPtr SortedTask_;

    //! The (adjusted) key columns that define the sort order inside sorted chunk pool.
    std::vector<TString> PrimaryKeyColumns_;
    std::vector<TString> ForeignKeyColumns_;

    // XXX(max42): this field is effectively transient, do not persist it.
    IJobSizeConstraintsPtr JobSizeConstraints_;

    i64 InputSliceDataWeight_;

    IFetcherChunkScraperPtr FetcherChunkScraper_;

    // Custom bits of preparation pipeline.

    virtual bool IsCompleted() const override
    {
        return TOperationControllerBase::IsCompleted() && (!SortedTask_ || SortedTask_->IsCompleted());
    }

    virtual i64 GetUnavailableInputChunkCount() const override
    {
        if (FetcherChunkScraper_ && State == EControllerState::Preparing) {
            return FetcherChunkScraper_->GetUnavailableChunkCount();
        }

        return TOperationControllerBase::GetUnavailableInputChunkCount();
    };

    virtual void DoInitialize() override
    {
        TOperationControllerBase::DoInitialize();

        SortedTaskGroup_ = New<TTaskGroup>();
        SortedTaskGroup_->MinNeededResources.SetCpu(GetCpuLimit());

        RegisterTaskGroup(SortedTaskGroup_);
    }

    void CalculateSizes()
    {
        Spec_->Sampling->MaxTotalSliceCount = Spec_->Sampling->MaxTotalSliceCount.value_or(Config->MaxTotalSliceCount);

        switch (OperationType) {
            case EOperationType::Merge:
                JobSizeConstraints_ = CreateMergeJobSizeConstraints(
                    Spec_,
                    Options_,
                    Logger,
                    TotalEstimatedInputChunkCount,
                    PrimaryInputDataWeight,
                    DataWeightRatio,
                    InputCompressionRatio,
                    InputTables_.size(),
                    GetPrimaryInputTableCount());
                break;
            default:
                JobSizeConstraints_ = CreateUserJobSizeConstraints(
                    Spec_,
                    Options_,
                    Logger,
                    OutputTables_.size(),
                    DataWeightRatio,
                    TotalEstimatedInputChunkCount,
                    PrimaryInputDataWeight,
                    std::numeric_limits<i64>::max() /* InputRowCount */, // It is not important in sorted operations.
                    GetForeignInputDataWeight(),
                    InputTables_.size(),
                    GetPrimaryInputTableCount(),
                    /*sortedOperation=*/true);
                break;
        }

        InputSliceDataWeight_ = JobSizeConstraints_->GetInputSliceDataWeight();

        YT_LOG_INFO(
            "Calculated operation parameters (JobCount: %v, MaxDataWeightPerJob: %v, InputSliceDataWeight: %v)",
            JobSizeConstraints_->GetJobCount(),
            JobSizeConstraints_->GetMaxDataWeightPerJob(),
            InputSliceDataWeight_);
    }

    void CheckInputTableKeyColumnTypes(
        const TKeyColumns& keyColumns,
        std::function<bool(const TInputTablePtr& table)> inputTableFilter = [] (const TInputTablePtr& /* table */) { return true; })
    {
        YT_VERIFY(!InputTables_.empty());

        for (const auto& columnName : keyColumns) {
            const TColumnSchema* referenceColumn = nullptr;
            TInputTablePtr referenceTable;
            for (const auto& table : InputTables_) {
                if (!inputTableFilter(table)) {
                    continue;
                }
                const auto& column = table->Schema.GetColumnOrThrow(columnName);
                if (!column.SimplifiedLogicalType()) {
                    THROW_ERROR_EXCEPTION("Key column %Qv cannot have complex type %Qv",
                        columnName,
                        *column.LogicalType());
                }
                if (*column.SimplifiedLogicalType() == ESimpleLogicalValueType::Any) {
                    continue;
                }
                if (referenceColumn) {
                    YT_VERIFY(referenceColumn->SimplifiedLogicalType());
                    if (GetPhysicalType(*referenceColumn->SimplifiedLogicalType()) != GetPhysicalType(*column.SimplifiedLogicalType())) {
                        THROW_ERROR_EXCEPTION("Key columns have different types in input tables")
                            << TErrorAttribute("column_name", columnName)
                            << TErrorAttribute("input_table_1", referenceTable->GetPath())
                            << TErrorAttribute("type_1", ToString(*referenceColumn->LogicalType()))
                            << TErrorAttribute("input_table_2", table->GetPath())
                            << TErrorAttribute("type_2", ToString(*column.LogicalType()));
                    }
                } else {
                    referenceColumn = &column;
                    referenceTable = table;
                }
            }
        }
    }

    TChunkStripePtr CreateChunkStripe(TInputDataSlicePtr dataSlice)
    {
        auto chunkStripe = New<TChunkStripe>(InputTables_[dataSlice->GetTableIndex()]->IsForeign());
        chunkStripe->DataSlices.emplace_back(std::move(dataSlice));
        return chunkStripe;
    }

    void ProcessInputs()
    {
        PROFILE_TIMING ("/input_processing_time") {
            YT_LOG_INFO("Processing inputs");

            TPeriodicYielder yielder(PrepareYieldPeriod);

            InitTeleportableInputTables();

            int primaryUnversionedSlices = 0;
            int primaryVersionedSlices = 0;
            int foreignSlices = 0;
            for (const auto& chunk : CollectPrimaryUnversionedChunks()) {
                const auto& slice = CreateUnversionedInputDataSlice(CreateInputChunkSlice(chunk));
                InferLimitsFromBoundaryKeys(slice, RowBuffer);
                SortedTask_->AddInput(CreateChunkStripe(slice));
                ++primaryUnversionedSlices;
                yielder.TryYield();
            }
            for (const auto& slice : CollectPrimaryVersionedDataSlices(InputSliceDataWeight_)) {
                SortedTask_->AddInput(CreateChunkStripe(slice));
                ++primaryVersionedSlices;
                yielder.TryYield();
            }
            for (const auto& tableSlices : CollectForeignInputDataSlices(ForeignKeyColumns_.size())) {
                for (const auto& slice : tableSlices) {
                    SortedTask_->AddInput(CreateChunkStripe(slice));
                    ++foreignSlices;
                    yielder.TryYield();
                }
            }

            YT_LOG_INFO("Processed inputs (PrimaryUnversionedSlices: %v, PrimaryVersionedSlices: %v, ForeignSlices: %v)",
                primaryUnversionedSlices,
                primaryVersionedSlices,
                foreignSlices);
        }
    }

    void FinishPreparation()
    {
        InitJobIOConfig();
        InitJobSpecTemplate();
    }

    virtual std::optional<int> GetOutputTeleportTableIndex() const = 0;

    //! Initializes #JobIOConfig.
    void InitJobIOConfig()
    {
        JobIOConfig_ = CloneYsonSerializable(Spec_->JobIO);
    }

    virtual bool IsKeyGuaranteeEnabled() = 0;

    virtual EJobType GetJobType() const = 0;

    virtual TCpuResource GetCpuLimit() const = 0;

    virtual void InitJobSpecTemplate() = 0;

    void InitTeleportableInputTables()
    {
        auto tableIndex = GetOutputTeleportTableIndex();
        if (tableIndex) {
            for (const auto& inputTable : InputTables_) {
                if (!inputTable->Dynamic &&
                    !inputTable->Path.GetColumns() &&
                    inputTable->ColumnRenameDescriptors.empty() &&
                    OutputTables_[*tableIndex]->TableUploadOptions.SchemaModification == ETableSchemaModification::None)
                {
                    inputTable->Teleportable = ValidateTableSchemaCompatibility(
                        inputTable->Schema,
                        OutputTables_[*tableIndex]->TableUploadOptions.TableSchema,
                        false /* ignoreSortOrder */).IsOK();
                    if (GetJobType() == EJobType::SortedReduce) {
                        inputTable->Teleportable &= inputTable->Path.GetTeleport();
                    }
                }
            }
        }
    }

    virtual bool ShouldSlicePrimaryTableByKeys() const
    {
        return true;
    }

    virtual i64 GetMinTeleportChunkSize()  = 0;

    virtual void AdjustKeyColumns() = 0;

    virtual i64 GetForeignInputDataWeight() const = 0;

    virtual void PrepareOutputTables() override
    {
        // NB: we need to do this after locking input tables but before preparing ouput tables.
        AdjustKeyColumns();
    }

    virtual TUserJobSpecPtr GetUserJobSpec() const = 0;

    virtual void CustomPrepare() override
    {
        // NB: Base member is not called intentionally.
        // TODO(max42): But why?

        CalculateSizes();

        InitTeleportableInputTables();

        bool autoMergeNeeded = false;
        if (GetOperationType() != EOperationType::Merge) {
            autoMergeNeeded = TryInitAutoMerge(JobSizeConstraints_->GetJobCount(), DataWeightRatio);
        }

        if (autoMergeNeeded) {
            SortedTask_ = New<TAutoMergeableSortedTask>(this, GetAutoMergeEdgeDescriptors());
        } else {
            SortedTask_ = New<TSortedTask>(this, GetStandardEdgeDescriptors());
        }
        RegisterTask(SortedTask_);

        ProcessInputs();

        FinishTaskInput(SortedTask_);
        for (int index = 0; index < AutoMergeTasks.size(); ++index) {
            if (AutoMergeTasks[index]) {
                AutoMergeTasks[index]->FinishInput(SortedTask_->GetVertexDescriptor());
            }
        }

        for (const auto& teleportChunk : SortedTask_->GetChunkPoolOutput()->GetTeleportChunks()) {
            // If teleport chunks were found, then teleport table index should be non-null.
            RegisterTeleportChunk(teleportChunk, 0, *GetOutputTeleportTableIndex());
        }

        FinishPreparation();
    }

    virtual bool IsBoundaryKeysFetchEnabled() const override
    {
        return true;
    }

    class TChunkSliceFetcherFactory
        : public IChunkSliceFetcherFactory
    {
    public:
        //! Used only for persistence.
        TChunkSliceFetcherFactory() = default;

        TChunkSliceFetcherFactory(TSortedControllerBase* controller)
            : Controller_(controller)
        { }

        virtual IChunkSliceFetcherPtr CreateChunkSliceFetcher() override
        {
            return Controller_->CreateChunkSliceFetcher();
        }

        virtual void Persist(const TPersistenceContext& context) override
        {
            using NYT::Persist;

            Persist(context, Controller_);
        }

    private:
        DECLARE_DYNAMIC_PHOENIX_TYPE(TChunkSliceFetcherFactory, 0x23cad49e);

        TSortedControllerBase* Controller_;
    };

    virtual IChunkSliceFetcherFactoryPtr CreateChunkSliceFetcherFactory()
    {
        return New<TChunkSliceFetcherFactory>(this /* controller */);
    }

    virtual TSortedChunkPoolOptions GetSortedChunkPoolOptions()
    {
        TSortedChunkPoolOptions chunkPoolOptions;
        TSortedJobOptions jobOptions;
        jobOptions.EnableKeyGuarantee = IsKeyGuaranteeEnabled();
        jobOptions.PrimaryPrefixLength = PrimaryKeyColumns_.size();
        jobOptions.ForeignPrefixLength = ForeignKeyColumns_.size();
        jobOptions.ShouldSlicePrimaryTableByKeys = ShouldSlicePrimaryTableByKeys();
        jobOptions.MaxTotalSliceCount = Config->MaxTotalSliceCount;
        jobOptions.EnablePeriodicYielder = true;

        if (Spec_->NightlyOptions) {
            auto logDetails = Spec_->NightlyOptions->FindChild("log_details");
            if (logDetails && logDetails->GetType() == ENodeType::Boolean) {
                jobOptions.LogDetails = logDetails->AsBoolean()->GetValue();
            }
        }

        chunkPoolOptions.SortedJobOptions = jobOptions;
        chunkPoolOptions.MinTeleportChunkSize = GetMinTeleportChunkSize();
        chunkPoolOptions.JobSizeConstraints = JobSizeConstraints_;
        chunkPoolOptions.OperationId = OperationId;
        return chunkPoolOptions;
    }


    virtual bool IsJobInterruptible() const override
    {
        auto totalJobCount = GetDataFlowGraph()->GetTotalJobCounter()->GetTotal();
        return
            2 * Options_->MaxOutputTablesTimesJobsCount > totalJobCount * GetOutputTablePaths().size() &&
            2 * Options_->MaxJobCount > totalJobCount &&
            TOperationControllerBase::IsJobInterruptible();
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


    virtual void OnChunksReleased(int chunkCount) override
    {
        TOperationControllerBase::OnChunksReleased(chunkCount);

        if (const auto& autoMergeDirector = GetAutoMergeDirector()) {
            autoMergeDirector->OnMergeJobFinished(chunkCount /* unregisteredIntermediateChunkCount */);
        }
    }

    virtual EIntermediateChunkUnstageMode GetIntermediateChunkUnstageMode() const override
    {
        auto reducerSpec = GetUserJobSpec();
        // We could get here only if this is a sorted reduce and auto-merge is enabled.
        YT_VERIFY(reducerSpec);
        YT_VERIFY(Spec_->AutoMerge->Mode != EAutoMergeMode::Disabled);

        if (Spec_->AutoMerge->Mode != EAutoMergeMode::Relaxed && reducerSpec->Deterministic) {
            return EIntermediateChunkUnstageMode::OnJobCompleted;
        } else {
            return EIntermediateChunkUnstageMode::OnSnapshotCompleted;
        }
    }

private:
    IChunkSliceFetcherPtr CreateChunkSliceFetcher()
    {
        FetcherChunkScraper_ = CreateFetcherChunkScraper();

        auto fetcher = NTableClient::CreateChunkSliceFetcher(
            Config->Fetcher,
            InputSliceDataWeight_,
            InputNodeDirectory_,
            GetCancelableInvoker(),
            FetcherChunkScraper_,
            Host->GetClient(),
            RowBuffer,
            Logger);
        fetcher->SetCancelableContext(GetCancelableContext());
        return fetcher;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TSortedControllerBase::TSortedTask);
DEFINE_DYNAMIC_PHOENIX_TYPE(TSortedControllerBase::TAutoMergeableSortedTask);
DEFINE_DYNAMIC_PHOENIX_TYPE(TSortedControllerBase::TChunkSliceFetcherFactory);

////////////////////////////////////////////////////////////////////////////////

class TSortedMergeController
    : public TSortedControllerBase
{
public:
    TSortedMergeController(
        TSortedMergeOperationSpecPtr spec,
        TControllerAgentConfigPtr config,
        TSortedMergeOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TSortedControllerBase(
            spec,
            config,
            options,
            host,
            operation)
        , Spec_(spec)
    { }

    virtual bool ShouldSlicePrimaryTableByKeys() const override
    {
        return true;
    }

    virtual bool IsRowCountPreserved() const override
    {
        return true;
    }

    virtual TUserJobSpecPtr GetUserJobSpec() const
    {
        return nullptr;
    }

    virtual i64 GetMinTeleportChunkSize() override
    {
        if (Spec_->ForceTransform) {
            return std::numeric_limits<i64>::max();
        }
        if (!Spec_->CombineChunks) {
            return 0;
        }
        return Spec_->JobIO
            ->TableWriter
            ->DesiredChunkSize;
    }

    virtual void AdjustKeyColumns() override
    {
        const auto& specKeyColumns = Spec_->MergeBy;
        YT_LOG_INFO("Spec key columns are %v", specKeyColumns);

        PrimaryKeyColumns_ = CheckInputTablesSorted(specKeyColumns);
        YT_LOG_INFO("Adjusted key columns are %v", PrimaryKeyColumns_);
    }

    virtual bool IsKeyGuaranteeEnabled() override
    {
        return false;
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::SortedMerge;
    }

    virtual TCpuResource GetCpuLimit() const override
    {
        return 1;
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec_->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return {Spec_->OutputTablePath};
    }

    virtual void InitJobSpecTemplate() override
    {
        JobSpecTemplate_.set_type(static_cast<int>(EJobType::SortedMerge));
        auto* schedulerJobSpecExt = JobSpecTemplate_.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        auto* mergeJobSpecExt = JobSpecTemplate_.MutableExtension(TMergeJobSpecExt::merge_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec_->JobIO)).GetData());

        SetDataSourceDirectory(schedulerJobSpecExt, BuildDataSourceDirectoryFromInputTables(InputTables_));
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig_).GetData());

        ToProto(mergeJobSpecExt->mutable_key_columns(), PrimaryKeyColumns_);
    }

    virtual std::optional<int> GetOutputTeleportTableIndex() const override
    {
        return std::make_optional(0);
    }

    virtual void PrepareOutputTables() override
    {
        // Check that all input tables are sorted by the same key columns.
        TSortedControllerBase::PrepareOutputTables();

        auto& table = OutputTables_[0];
        if (!table->Dynamic) {
            table->TableUploadOptions.LockMode = ELockMode::Exclusive;
        }

        auto prepareOutputKeyColumns = [&] () {
            if (table->TableUploadOptions.TableSchema.IsSorted()) {
                if (table->TableUploadOptions.TableSchema.GetKeyColumns() != PrimaryKeyColumns_) {
                    THROW_ERROR_EXCEPTION("Merge key columns do not match output table schema in \"strong\" schema mode")
                        << TErrorAttribute("output_schema", table->TableUploadOptions.TableSchema)
                        << TErrorAttribute("merge_by", PrimaryKeyColumns_)
                        << TErrorAttribute("schema_inference_mode", Spec_->SchemaInferenceMode);
                }
            } else {
                table->TableUploadOptions.TableSchema =
                    table->TableUploadOptions.TableSchema.ToSorted(PrimaryKeyColumns_);
            }
        };

        switch (Spec_->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table->TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    InferSchemaFromInput(PrimaryKeyColumns_);
                } else {
                    prepareOutputKeyColumns();
                    ValidateOutputSchemaCompatibility(true);
                }
                break;

            case ESchemaInferenceMode::FromInput:
                InferSchemaFromInput(PrimaryKeyColumns_);
                break;

            case ESchemaInferenceMode::FromOutput:
                if (table->TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    table->TableUploadOptions.TableSchema = TTableSchema::FromKeyColumns(PrimaryKeyColumns_);
                } else {
                    prepareOutputKeyColumns();
                }
                break;

            default:
                YT_ABORT();
        }
    }

    virtual i64 GetForeignInputDataWeight() const override
    {
        return 0;
    }

protected:
    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        return AsStringBuf("data_weight_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::SortedMerge};
    }

    virtual TYsonSerializablePtr GetTypedSpec() const override
    {
        return Spec_;
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TSortedMergeController, 0xf3b791ca);

    TSortedMergeOperationSpecPtr Spec_;
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TSortedMergeController);

IOperationControllerPtr CreateSortedMergeController(
    TControllerAgentConfigPtr config,
    IOperationControllerHostPtr host,
    TOperation* operation)
{
    auto options = config->SortedMergeOperationOptions;
    auto spec = ParseOperationSpec<TSortedMergeOperationSpec>(UpdateSpec(options->SpecTemplate, operation->GetSpec()));
    return New<TSortedMergeController>(spec, config, options, host, operation);
}

////////////////////////////////////////////////////////////////////////////////

class TReduceController
    : public TSortedControllerBase
{
public:
    TReduceController(
        TReduceOperationSpecPtr spec,
        TControllerAgentConfigPtr config,
        TReduceOperationOptionsPtr options,
        IOperationControllerHostPtr host,
        TOperation* operation)
        : TSortedControllerBase(
            spec,
            std::move(config),
            options,
            std::move(host),
            operation)
        , Spec_(std::move(spec))
        , Options_(std::move(options))
    { }

    virtual bool IsRowCountPreserved() const override
    {
        return false;
    }

    virtual bool AreForeignTablesSupported() const override
    {
        return true;
    }

    virtual TCpuResource GetCpuLimit() const override
    {
        return Spec_->Reducer->CpuLimit;
    }

    virtual TUserJobSpecPtr GetUserJobSpec() const
    {
        return Spec_->Reducer;
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec_->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return Spec_->OutputTablePaths;
    }

    virtual std::optional<int> GetOutputTeleportTableIndex() const override
    {
        return OutputTeleportTableIndex_;
    }

    virtual i64 GetMinTeleportChunkSize() override
    {
        return 0;
    }

    virtual void CustomizeJoblet(const TJobletPtr& joblet) override
    {
        joblet->StartRowIndex = StartRowIndex_;
        StartRowIndex_ += joblet->InputStripeList->TotalRowCount;
    }

    virtual std::vector<TUserJobSpecPtr> GetUserJobSpecs() const override
    {
        return {Spec_->Reducer};
    }

    virtual void InitJobSpecTemplate() override
    {
        YT_VERIFY(!PrimaryKeyColumns_.empty());

        JobSpecTemplate_.set_type(static_cast<int>(GetJobType()));
        auto* schedulerJobSpecExt = JobSpecTemplate_.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec_->JobIO)).GetData());

        SetDataSourceDirectory(schedulerJobSpecExt, BuildDataSourceDirectoryFromInputTables(InputTables_));

        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig_).GetData());

        InitUserJobSpecTemplate(
            schedulerJobSpecExt->mutable_user_job_spec(),
            Spec_->Reducer,
            UserJobFiles_[Spec_->Reducer],
            Spec_->JobNodeAccount);

        auto* reduceJobSpecExt = JobSpecTemplate_.MutableExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        ToProto(reduceJobSpecExt->mutable_key_columns(), SortKeyColumns_);
        reduceJobSpecExt->set_reduce_key_column_count(PrimaryKeyColumns_.size());
        reduceJobSpecExt->set_join_key_column_count(ForeignKeyColumns_.size());
    }

    virtual void DoInitialize() override
    {
        TSortedControllerBase::DoInitialize();

        int teleportOutputCount = 0;
        for (int i = 0; i < static_cast<int>(OutputTables_.size()); ++i) {
            if (OutputTables_[i]->Path.GetTeleport()) {
                ++teleportOutputCount;
                OutputTeleportTableIndex_ = i;
            }
        }

        if (teleportOutputCount > 1) {
            THROW_ERROR_EXCEPTION("Too many teleport output tables: maximum allowed 1, actual %v",
                teleportOutputCount);
        }

        ValidateUserFileCount(Spec_->Reducer, "reducer");

        int foreignInputCount = 0;
        for (auto& table : InputTables_) {
            if (table->Path.GetForeign()) {
                if (table->Path.GetTeleport()) {
                    THROW_ERROR_EXCEPTION("Foreign table can not be specified as teleport")
                        << TErrorAttribute("path", table->Path);
                }
                if (table->Path.GetRanges().size() > 1) {
                    THROW_ERROR_EXCEPTION("Reduce operation does not support foreign tables with multiple ranges");
                }
                ++foreignInputCount;
            }
        }

        if (foreignInputCount == InputTables_.size()) {
            THROW_ERROR_EXCEPTION("At least one non-foreign input table is required");
        }

        if (foreignInputCount == 0 && !Spec_->JoinBy.empty()) {
            THROW_ERROR_EXCEPTION("At least one foreign input table is required when join_by is specified");
        }

        if (foreignInputCount != 0 && Spec_->JoinBy.empty()) {
            THROW_ERROR_EXCEPTION("It is required to specify join_by when using foreign tables");
        }

        if (!Spec_->PivotKeys.empty()) {
            if (!*Spec_->EnableKeyGuarantee) {
                THROW_ERROR_EXCEPTION("Pivot keys are not supported in disabled key guarantee mode.");
            }

            TKey previousKey;
            for (const auto& key : Spec_->PivotKeys) {
                if (key < previousKey) {
                    THROW_ERROR_EXCEPTION("Pivot keys should be sorted")
                        << TErrorAttribute("previous", previousKey)
                        << TErrorAttribute("current", key);
                }
                previousKey = key;

                if (key.GetCount() > Spec_->ReduceBy.size()) {
                    THROW_ERROR_EXCEPTION("Pivot key cannot be longer than reduce key column count")
                        << TErrorAttribute("key", key)
                        << TErrorAttribute("reduce_by", Spec_->ReduceBy);
                }
            }
            for (const auto& table : InputTables_) {
                if (table->Path.GetTeleport()) {
                    THROW_ERROR_EXCEPTION("Chunk teleportation is not supported when pivot keys are specified");
                }
            }
        }
    }

    virtual void BuildBriefSpec(TFluentMap fluent) const override
    {
        TSortedControllerBase::BuildBriefSpec(fluent);
        fluent
            .Item("reducer").BeginMap()
                .Item("command").Value(TrimCommandForBriefSpec(Spec_->Reducer->Command))
            .EndMap();
    }

    virtual bool IsInputDataSizeHistogramSupported() const override
    {
        return true;
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

    virtual bool GetEnableCudaGpuCoreDump() const override
    {
        return Spec_->EnableCudaGpuCoreDump;
    }

    virtual ELegacyLivePreviewMode GetLegacyOutputLivePreviewMode() const override
    {
        return ToLegacyLivePreviewMode(Spec_->EnableLegacyLivePreview);
    }

    virtual i64 GetForeignInputDataWeight() const override
    {
        return Spec_->ConsiderOnlyPrimarySize ? 0 : ForeignInputDataWeight;
    }

    virtual TYsonSerializablePtr GetTypedSpec() const override
    {
        return Spec_;
    }

    virtual bool ShouldSlicePrimaryTableByKeys() const override
    {
        return *Spec_->EnableKeyGuarantee;
    }

    virtual EJobType GetJobType() const override
    {
        return *Spec_->EnableKeyGuarantee ? EJobType::SortedReduce : EJobType::JoinReduce;
    }

    virtual bool IsKeyGuaranteeEnabled() override
    {
        return *Spec_->EnableKeyGuarantee;
    }

    virtual void AdjustKeyColumns() override
    {
        YT_LOG_INFO("Adjusting key columns (EnableKeyGuarantee: %v, ReduceBy: %v, SortBy: %v, JoinBy: %v)",
            *Spec_->EnableKeyGuarantee,
            Spec_->ReduceBy,
            Spec_->SortBy,
            Spec_->JoinBy);

        if (*Spec_->EnableKeyGuarantee) {
            auto specKeyColumns = Spec_->SortBy.empty() ? Spec_->ReduceBy : Spec_->SortBy;
            SortKeyColumns_ = CheckInputTablesSorted(specKeyColumns, &TInputTable::IsPrimary);

            if (!CheckKeyColumnsCompatible(SortKeyColumns_, Spec_->ReduceBy)) {
                THROW_ERROR_EXCEPTION("Reduce key columns are not compatible with sort key columns")
                    << TErrorAttribute("reduce_by", Spec_->ReduceBy)
                    << TErrorAttribute("sort_by", SortKeyColumns_);
            }

            if (Spec_->ReduceBy.empty()) {
                THROW_ERROR_EXCEPTION("Reduce by can not be empty when key guarantee is enabled")
                    << TErrorAttribute("operation_type", OperationType);
            }

            PrimaryKeyColumns_ = Spec_->ReduceBy;
            ForeignKeyColumns_ = Spec_->JoinBy;
            if (!ForeignKeyColumns_.empty()) {
                CheckInputTablesSorted(ForeignKeyColumns_, &TInputTable::IsForeign);
                if (!CheckKeyColumnsCompatible(PrimaryKeyColumns_, ForeignKeyColumns_)) {
                    THROW_ERROR_EXCEPTION("Join key columns are not compatible with reduce key columns")
                        << TErrorAttribute("join_by", ForeignKeyColumns_)
                        << TErrorAttribute("reduce_by", PrimaryKeyColumns_);
                }
            }
        } else {
            if (!Spec_->ReduceBy.empty() && !Spec_->JoinBy.empty()) {
                THROW_ERROR_EXCEPTION("Specifying both reduce and join key columns is not supported in disabled key guarantee mode");
            }
            if (Spec_->ReduceBy.empty() && Spec_->JoinBy.empty()) {
                THROW_ERROR_EXCEPTION("At least one of reduce_by or join_by is required for this operation");
            }
            PrimaryKeyColumns_ = CheckInputTablesSorted(!Spec_->ReduceBy.empty() ? Spec_->ReduceBy : Spec_->JoinBy);
            if (PrimaryKeyColumns_.empty()) {
                THROW_ERROR_EXCEPTION("At least one of reduce_by and join_by should be specified when key guarantee is disabled")
                    << TErrorAttribute("operation_type", OperationType);
            }
            SortKeyColumns_ = ForeignKeyColumns_ = PrimaryKeyColumns_;

            if (!Spec_->SortBy.empty()) {
                if (!CheckKeyColumnsCompatible(Spec_->SortBy, Spec_->JoinBy)) {
                    THROW_ERROR_EXCEPTION("Join key columns are not compatible with sort key columns")
                        << TErrorAttribute("join_by", Spec_->JoinBy)
                        << TErrorAttribute("sort_by", Spec_->SortBy);
                }
                SortKeyColumns_ = Spec_->SortBy;
            }
        }
        if (Spec_->ValidateKeyColumnTypes) {
            CheckInputTableKeyColumnTypes(ForeignKeyColumns_);
            CheckInputTableKeyColumnTypes(PrimaryKeyColumns_, [] (const auto& table) {
                return table->IsPrimary();
            });
        }
        YT_LOG_INFO("Key columns adjusted (PrimaryKeyColumns: %v, ForeignKeyColumns: %v, SortKeyColumns: %v)",
            PrimaryKeyColumns_,
            ForeignKeyColumns_,
            SortKeyColumns_);
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TReduceController, 0x4fc44a45);

    TReduceOperationSpecPtr Spec_;
    TReduceOperationOptionsPtr Options_;

    i64 StartRowIndex_ = 0;

    std::vector<TString> SortKeyColumns_;

    std::optional<int> OutputTeleportTableIndex_;

    virtual TStringBuf GetDataWeightParameterNameForJob(EJobType jobType) const override
    {
        return AsStringBuf("data_weight_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {GetJobType()};
    }

    virtual bool IsJobInterruptible() const override
    {
        return Spec_->PivotKeys.empty() && TSortedControllerBase::IsJobInterruptible();
    }

    virtual IChunkSliceFetcherFactoryPtr CreateChunkSliceFetcherFactory() override
    {
        if (Spec_->PivotKeys.empty()) {
            return TSortedControllerBase::CreateChunkSliceFetcherFactory();
        } else {
            return nullptr;
        }
    }

    virtual TSortedChunkPoolOptions GetSortedChunkPoolOptions() override
    {
        auto options = TSortedControllerBase::GetSortedChunkPoolOptions();
        options.SortedJobOptions.PivotKeys = std::vector<TKey>(Spec_->PivotKeys.begin(), Spec_->PivotKeys.end());
        return options;
    }

    virtual TError GetAutoMergeError() const override
    {
        return TError();
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TReduceController);

IOperationControllerPtr CreateReduceController(
    TControllerAgentConfigPtr config,
    IOperationControllerHostPtr host,
    TOperation* operation,
    bool isJoinReduce)
{
    auto options = isJoinReduce ? config->JoinReduceOperationOptions : config->ReduceOperationOptions;
    auto mergedSpec = UpdateSpec(options->SpecTemplate, operation->GetSpec());
    auto spec = ParseOperationSpec<TReduceOperationSpec>(mergedSpec);
    if (!spec->EnableKeyGuarantee) {
        spec->EnableKeyGuarantee = !isJoinReduce;
    }
    return New<TReduceController>(std::move(spec), std::move(config), std::move(options), std::move(host), operation);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
