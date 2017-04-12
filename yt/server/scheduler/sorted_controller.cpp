#include "sorted_controller.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "chunk_pool.h"
#include "helpers.h"
#include "job_memory.h"
#include "sorted_chunk_pool.h"
#include "operation_controller_detail.h"

#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_scraper.h>
#include <yt/ytlib/chunk_client/input_chunk_slice.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/chunk_slice_fetcher.h>
#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/core/concurrency/periodic_yielder.h>

#include <yt/core/misc/numeric_helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NYPath;
using namespace NYson;
using namespace NJobProxy;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NScheduler::NProto;
using namespace NChunkClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NConcurrency;
using namespace NTableClient;

using NChunkClient::TReadRange;
using NChunkClient::TReadLimit;
using NTableClient::TKey;

////////////////////////////////////////////////////////////////////

static const NProfiling::TProfiler Profiler("/operations/merge");

////////////////////////////////////////////////////////////////////

// TODO(max42): support Config->MaxTotalSliceCount
// TODO(max42): reorder virtual methods in public section.

class TSortedControllerBase
    : public TOperationControllerBase
{
public:
    TSortedControllerBase(
        TSchedulerConfigPtr config,
        TSimpleOperationSpecBasePtr spec,
        TSimpleOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TOperationControllerBase(config, spec, options, host, operation)
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
        Persist(context, InputSliceDataSize_);
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

    class TSortedTask
        : public TTask
    {
    public:
        //! For persistence only.
        TSortedTask()
            : Controller_(nullptr)
        { }

        TSortedTask(TSortedControllerBase* controller)
            : TTask(controller)
            , Controller_(controller)
            , ChunkPool_(CreateSortedChunkPool(
                controller->GetSortedChunkPoolOptions(),
                controller->ChunkSliceFetcher_,
                controller->GetInputStreamDirectory()))
        { }

        virtual Stroka GetId() const override
        {
            return Format("Sorted");
        }

        virtual TTaskGroupPtr GetGroup() const override
        {
            return Controller_->SortedTaskGroup_;
        }

        virtual TDuration GetLocalityTimeout() const override
        {
            return Controller_->Spec_->LocalityTimeout;
        }

        virtual TExtendedJobResources GetNeededResources(TJobletPtr joblet) const override
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
        void BuildInputOutputJobSpec(TJobletPtr joblet, TJobSpec* jobSpec)
        {
            AddParallelInputSpec(jobSpec, joblet);
            AddFinalOutputSpecs(jobSpec, joblet);
        }

    private:
        DECLARE_DYNAMIC_PHOENIX_TYPE(TSortedTask, 0xf881be2a);

        TSortedControllerBase* Controller_;

        //! Initialized in descendandt tasks.
        std::unique_ptr<IChunkPool> ChunkPool_;

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

        virtual void OnJobCompleted(TJobletPtr joblet, const TCompletedJobSummary& jobSummary) override
        {
            TTask::OnJobCompleted(joblet, jobSummary);

            RegisterOutput(joblet, 0, jobSummary);
        }

        virtual void OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& jobSummary) override
        {
            TTask::OnJobAborted(joblet, jobSummary);
        }
    };

    typedef TIntrusivePtr<TSortedTask> TSortedTaskPtr;

    TTaskGroupPtr SortedTaskGroup_;

    TSortedTaskPtr SortedTask_;

    //! The (adjusted) key columns that define the sort order inside sorted chunk pool.
    std::vector<Stroka> PrimaryKeyColumns_;
    std::vector<Stroka> ForeignKeyColumns_;

    IChunkSliceFetcherPtr ChunkSliceFetcher_;

    IJobSizeConstraintsPtr JobSizeConstraints_;

    i64 InputSliceDataSize_;

    // Custom bits of preparation pipeline.

    TInputStreamDirectory GetInputStreamDirectory()
    {
        std::vector<TInputStreamDescriptor> inputStreams;
        inputStreams.reserve(InputTables.size());
        for (const auto& inputTable : InputTables) {
            inputStreams.emplace_back(inputTable.IsTeleportable, inputTable.IsPrimary(), inputTable.IsDynamic /* isVersioned */);
        }
        return TInputStreamDirectory(inputStreams);
    }

    virtual bool IsCompleted() const override
    {
        return SortedTask_->IsCompleted();
    }

    virtual void DoInitialize() override
    {
        TOperationControllerBase::DoInitialize();

        SortedTaskGroup_ = New<TTaskGroup>();
        SortedTaskGroup_->MinNeededResources.SetCpu(GetCpuLimit());

        RegisterTaskGroup(SortedTaskGroup_);
    }

    TSortedChunkPoolOptions GetSortedChunkPoolOptions()
    {
        TSortedChunkPoolOptions options;
        options.EnableKeyGuarantee = IsKeyGuaranteeEnabled();
        options.PrimaryPrefixLength = PrimaryKeyColumns_.size();
        options.ForeignPrefixLength = ForeignKeyColumns_.size();
        options.MaxTotalSliceCount = Config->MaxTotalSliceCount;
        options.MinTeleportChunkSize = MinTeleportChunkSize();
        options.JobSizeConstraints = JobSizeConstraints_;
        options.OperationId = OperationId;
        return options;
    }

    void CalculateSizes()
    {
        JobSizeConstraints_ = CreateSimpleJobSizeConstraints(
            Spec_,
            Options_,
            PrimaryInputDataSize + ForeignInputDataSize);

        InputSliceDataSize_ = JobSizeConstraints_->GetInputSliceDataSize();

        LOG_INFO(
            "Calculated operation parameters (JobCount: %v, MaxDataSizePerJob: %v, InputSliceDataSize: %v)",
            JobSizeConstraints_->GetJobCount(),
            JobSizeConstraints_->GetMaxDataSizePerJob(),
            InputSliceDataSize_);
    }

    TChunkStripePtr CreateChunkStripe(TInputDataSlicePtr dataSlice)
    {
        TChunkStripePtr chunkStripe = New<TChunkStripe>(InputTables[dataSlice->GetTableIndex()].IsForeign());
        chunkStripe->DataSlices.emplace_back(std::move(dataSlice));
        return chunkStripe;
    }

    void ProcessInputs()
    {
        PROFILE_TIMING ("/input_processing_time") {
            LOG_INFO("Processing inputs");

            TPeriodicYielder yielder(PrepareYieldPeriod);

            InitTeleportableInputTables();

            int primaryUnversionedSlices = 0;
            int primaryVersionedSlices = 0;
            int foreignSlices = 0;
            for (const auto& chunk : CollectPrimaryUnversionedChunks()) {
                const auto& slice = CreateUnversionedInputDataSlice(CreateInputChunkSlice(chunk));
                InferLimitsFromBoundaryKeys(slice, RowBuffer);
                RegisterInputStripe(CreateChunkStripe(slice), SortedTask_);
                ++primaryUnversionedSlices;
                yielder.TryYield();
            }
            for (const auto& slice : CollectPrimaryVersionedDataSlices(InputSliceDataSize_)) {
                RegisterInputStripe(CreateChunkStripe(slice), SortedTask_);
                ++primaryVersionedSlices;
                yielder.TryYield();
            }
            for (const auto& tableSlices : CollectForeignInputDataSlices(ForeignKeyColumns_.size())) {
                for (const auto& slice : tableSlices) {
                    RegisterInputStripe(CreateChunkStripe(slice), SortedTask_);
                    ++foreignSlices;
                    yielder.TryYield();
                }
            }

            LOG_INFO("Processed inputs (PrimaryUnversionedSlices: %v, PrimaryVersionedSlices: %v, ForeignSlices: %v)",
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

    // Progress reporting.

    virtual Stroka GetLoggingProgress() const override
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
            UnavailableInputChunkCount);
    }

    virtual TNullable<int> GetOutputTeleportTableIndex() const = 0;

    //! Initializes #JobIOConfig.
    void InitJobIOConfig()
    {
        JobIOConfig_ = CloneYsonSerializable(Spec_->JobIO);
        InitFinalOutputConfig(JobIOConfig_);
    }

    virtual bool IsKeyGuaranteeEnabled() = 0;

    virtual EJobType GetJobType() const = 0;

    virtual TCpuResource GetCpuLimit() const = 0;

    virtual void InitJobSpecTemplate() = 0;

    void InitTeleportableInputTables()
    {
        auto tableIndex = GetOutputTeleportTableIndex();
        if (tableIndex) {
            for (int index = 0; index < InputTables.size(); ++index) {
                if (!InputTables[index].IsDynamic) {
                    InputTables[index].IsTeleportable = ValidateTableSchemaCompatibility(
                        InputTables[index].Schema,
                        OutputTables[*tableIndex].TableUploadOptions.TableSchema,
                        false /* ignoreSortOrder */).IsOK();
                    if (GetJobType() == EJobType::SortedReduce) {
                        InputTables[index].IsTeleportable &= InputTables[index].Path.GetTeleport();
                    }
                }
            }
        }
    }

    virtual bool ShouldSlicePrimaryTableByKeys() const
    {
        return true;
    }

    virtual i64 MinTeleportChunkSize()  = 0;

    virtual void AdjustKeyColumns() = 0;

    virtual i64 GetUserJobMemoryReserve() const = 0;

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

        TScrapeChunksCallback scraperCallback;
        if (Spec_->UnavailableChunkStrategy == EUnavailableChunkAction::Wait) {
            scraperCallback = CreateScrapeChunksSessionCallback(
                Config->ChunkScraper,
                GetCancelableInvoker(),
                Host->GetChunkLocationThrottlerManager(),
                AuthenticatedInputMasterClient,
                InputNodeDirectory,
                Logger);
        }

        ChunkSliceFetcher_ = CreateChunkSliceFetcher(
            Config->Fetcher,
            InputSliceDataSize_,
            PrimaryKeyColumns_,
            ShouldSlicePrimaryTableByKeys(),
            InputNodeDirectory,
            GetCancelableInvoker(),
            scraperCallback,
            Host->GetMasterClient(),
            RowBuffer,
            Logger);

        InitTeleportableInputTables();

        SortedTask_ = New<TSortedTask>(this);

        ProcessInputs();

        SortedTask_->FinishInput();

        for (const auto& teleportChunk : SortedTask_->GetChunkPoolOutput()->GetTeleportChunks()) {
            // If teleport chunks were found, then teleport table index should be non-Null.
            RegisterOutput(teleportChunk, 0, *GetOutputTeleportTableIndex());
        }

        RegisterTask(SortedTask_);

        FinishPreparation();
    }

    virtual bool IsBoundaryKeysFetchEnabled() const override
    {
        return true;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TSortedControllerBase::TSortedTask);

////////////////////////////////////////////////////////////////////

class TSortedMergeController
    : public TSortedControllerBase
{
public:
    TSortedMergeController(
        TSchedulerConfigPtr config,
        TSortedMergeOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TSortedControllerBase(config, spec, config->SortedMergeOperationOptions, host, operation)
        , Spec_(spec)
    {
        RegisterJobProxyMemoryDigest(EJobType::SortedMerge, spec->JobProxyMemoryDigest);
    }

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

    virtual i64 MinTeleportChunkSize() override
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
        LOG_INFO("Spec key columns are %v", specKeyColumns);

        PrimaryKeyColumns_ = CheckInputTablesSorted(specKeyColumns);
        LOG_INFO("Adjusted key columns are %v", PrimaryKeyColumns_);
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

    virtual i64 GetUserJobMemoryReserve() const override
    {
        return 0;
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

        ToProto(schedulerJobSpecExt->mutable_data_source_directory(), MakeInputDataSources());
        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig_).GetData());

        ToProto(mergeJobSpecExt->mutable_key_columns(), PrimaryKeyColumns_);
    }

    virtual TNullable<int> GetOutputTeleportTableIndex() const override
    {
        return MakeNullable(0);
    }

    virtual void PrepareOutputTables() override
    {
        // Check that all input tables are sorted by the same key columns.
        TSortedControllerBase::PrepareOutputTables();

        auto& table = OutputTables[0];
        table.TableUploadOptions.LockMode = ELockMode::Exclusive;

        auto prepareOutputKeyColumns = [&] () {
            if (table.TableUploadOptions.TableSchema.IsSorted()) {
                if (table.TableUploadOptions.TableSchema.GetKeyColumns() != PrimaryKeyColumns_) {
                    THROW_ERROR_EXCEPTION("Merge key columns do not match output table schema in \"strong\" schema mode")
                            << TErrorAttribute("output_schema", table.TableUploadOptions.TableSchema)
                            << TErrorAttribute("merge_by", PrimaryKeyColumns_)
                            << TErrorAttribute("schema_inference_mode", Spec_->SchemaInferenceMode);
                }
            } else {
                table.TableUploadOptions.TableSchema =
                    table.TableUploadOptions.TableSchema.ToSorted(PrimaryKeyColumns_);
            }
        };

        switch (Spec_->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    InferSchemaFromInput(PrimaryKeyColumns_);
                } else {
                    prepareOutputKeyColumns();

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
                InferSchemaFromInput(PrimaryKeyColumns_);
                break;

            case ESchemaInferenceMode::FromOutput:
                if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    table.TableUploadOptions.TableSchema = TTableSchema::FromKeyColumns(PrimaryKeyColumns_);
                } else {
                    prepareOutputKeyColumns();
                }
                break;

            default:
                Y_UNREACHABLE();
        }
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TSortedMergeController, 0xf3b791ca);

    TSortedMergeOperationSpecPtr Spec_;
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TSortedMergeController);

IOperationControllerPtr CreateSortedMergeController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TSortedMergeOperationSpec>(operation->GetSpec());
    return New<TSortedMergeController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

class TSortedReduceControllerBase
    : public TSortedControllerBase
{
public:
    TSortedReduceControllerBase(
        TSchedulerConfigPtr config,
        TReduceOperationSpecBasePtr spec,
        TReduceOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TSortedControllerBase(config, spec, options, host, operation)
        , Spec_(spec)
        , Options_(options)
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

    virtual i64 GetUserJobMemoryReserve() const override
    {
        return ComputeUserJobMemoryReserve(GetJobType(), Spec_->Reducer);
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec_->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return Spec_->OutputTablePaths;
    }

    virtual TNullable<int> GetOutputTeleportTableIndex() const override
    {
        return OutputTeleportTableIndex_;
    }

    virtual i64 MinTeleportChunkSize() override
    {
        return 0;
    }

    virtual void CustomizeJoblet(TJobletPtr joblet) override
    {
        joblet->StartRowIndex = StartRowIndex_;
        StartRowIndex_ += joblet->InputStripeList->TotalRowCount;
    }

    virtual std::vector<TPathWithStage> GetFilePaths() const override
    {
        std::vector<TPathWithStage> result;
        for (const auto& path : Spec_->Reducer->FilePaths) {
            result.push_back(std::make_pair(path, EOperationStage::Reduce));
        }
        return result;
    }

    virtual void InitJobSpecTemplate() override
    {
        YCHECK(!PrimaryKeyColumns_.empty());

        JobSpecTemplate_.set_type(static_cast<int>(GetJobType()));
        auto* schedulerJobSpecExt = JobSpecTemplate_.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec_->JobIO)).GetData());

        ToProto(schedulerJobSpecExt->mutable_data_source_directory(), MakeInputDataSources());

        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig_).GetData());

        InitUserJobSpecTemplate(
            schedulerJobSpecExt->mutable_user_job_spec(),
            Spec_->Reducer,
            Files,
            Spec_->JobNodeAccount);

        auto* reduceJobSpecExt = JobSpecTemplate_.MutableExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        ToProto(reduceJobSpecExt->mutable_key_columns(), SortKeyColumns_);
        reduceJobSpecExt->set_reduce_key_column_count(PrimaryKeyColumns_.size());
        reduceJobSpecExt->set_join_key_column_count(ForeignKeyColumns_.size());
    }

    virtual void CustomizeJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
    {
        auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        InitUserJobSpec(
            schedulerJobSpecExt->mutable_user_job_spec(),
            joblet);
    }

    virtual void DoInitialize() override
    {
        TSortedControllerBase::DoInitialize();

        int teleportOutputCount = 0;
        for (int i = 0; i < static_cast<int>(OutputTables.size()); ++i) {
            if (OutputTables[i].Path.GetTeleport()) {
                ++teleportOutputCount;
                OutputTeleportTableIndex_ = i;
            }
        }

        if (teleportOutputCount > 1) {
            THROW_ERROR_EXCEPTION("Too many teleport output tables: maximum allowed 1, actual %v",
                                  teleportOutputCount);
        }

        ValidateUserFileCount(Spec_->Reducer, "reducer");
    }

    virtual void BuildBriefSpec(IYsonConsumer* consumer) const override
    {
        TSortedControllerBase::BuildBriefSpec(consumer);
        BuildYsonMapFluently(consumer)
            .Item("reducer").BeginMap()
            .Item("command").Value(TrimCommandForBriefSpec(Spec_->Reducer->Command))
            .EndMap();
    }

    virtual bool IsJobInterruptible() const override
    {
        return true;
    }

    virtual TJobSplitterConfigPtr GetJobSplitterConfig() const override
    {
        return IsJobInterruptible() && Config->EnableJobSplitting && Spec_->EnableJobSplitting
            ? Options_->JobSplitter
            : nullptr;
    }

    virtual bool IsInputDataSizeHistogramSupported() const override
    {
        return true;
    }

    virtual TNullable<TRichYPath> GetStderrTablePath() const override
    {
        return Spec_->StderrTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetStderrTableWriterConfig() const override
    {
        return Spec_->StderrTableWriterConfig;
    }

    virtual TNullable<TRichYPath> GetCoreTablePath() const override
    {
        return Spec_->CoreTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetCoreTableWriterConfig() const override
    {
        return Spec_->CoreTableWriterConfig;
    }

protected:
    std::vector<Stroka> SortKeyColumns_;

private:
    TReduceOperationSpecBasePtr Spec_;
    TReduceOperationOptionsPtr Options_;

    i64 StartRowIndex_ = 0;

    TNullable<int> OutputTeleportTableIndex_;

};

class TSortedReduceController
    : public TSortedReduceControllerBase
{
public:
    TSortedReduceController(
        TSchedulerConfigPtr config,
        TReduceOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TSortedReduceControllerBase(config, spec, config->ReduceOperationOptions, host, operation)
        , Spec_(spec)
    {
        RegisterJobProxyMemoryDigest(EJobType::SortedReduce, spec->JobProxyMemoryDigest);
        RegisterUserJobMemoryDigest(EJobType::SortedReduce, spec->Reducer->MemoryReserveFactor);
    }

    virtual bool ShouldSlicePrimaryTableByKeys() const override
    {
        return true;
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::SortedReduce;
    }

    virtual bool IsKeyGuaranteeEnabled() override
    {
        return true;
    }

    virtual void AdjustKeyColumns() override
    {
        auto specKeyColumns = Spec_->SortBy.empty() ? Spec_->ReduceBy : Spec_->SortBy;
        LOG_INFO("Spec key columns are %v", specKeyColumns);

        SortKeyColumns_ = CheckInputTablesSorted(specKeyColumns, &TInputTable::IsPrimary);

        if (SortKeyColumns_.size() < Spec_->ReduceBy.size() ||
            !CheckKeyColumnsCompatible(SortKeyColumns_, Spec_->ReduceBy)) {
            THROW_ERROR_EXCEPTION("Reduce key columns %v are not compatible with sort key columns %v",
                Spec_->ReduceBy,
                SortKeyColumns_);
        }

        PrimaryKeyColumns_ = Spec_->ReduceBy;
        ForeignKeyColumns_ = Spec_->JoinBy;
        if (!ForeignKeyColumns_.empty()) {
            LOG_INFO("Foreign key columns are %v", ForeignKeyColumns_);

            CheckInputTablesSorted(ForeignKeyColumns_, &TInputTable::IsForeign);

            if (Spec_->ReduceBy.size() < ForeignKeyColumns_.size() ||
                !CheckKeyColumnsCompatible(Spec_->ReduceBy, ForeignKeyColumns_))
            {
                THROW_ERROR_EXCEPTION("Join key columns %v are not compatible with reduce key columns %v",
                    ForeignKeyColumns_,
                    Spec_->ReduceBy);
            }
        }
    }

    virtual void DoInitialize() override
    {
        TSortedReduceControllerBase::DoInitialize();

        int foreignInputCount = 0;
        for (auto& table : InputTables) {
            if (table.Path.GetForeign()) {
                if (table.Path.GetTeleport()) {
                    THROW_ERROR_EXCEPTION("Foreign table can not be specified as teleport");
                }
                if (table.Path.GetRanges().size() > 1) {
                    THROW_ERROR_EXCEPTION("Reduce operation does not support foreign tables with multiple ranges");
                }
                ++foreignInputCount;
            }
        }

        if (foreignInputCount == InputTables.size()) {
            THROW_ERROR_EXCEPTION("At least one non-foreign input table is required");
        }

        if (foreignInputCount == 0 && !Spec_->JoinBy.empty()) {
            THROW_ERROR_EXCEPTION("At least one foreign input table is required");
        }

        if (foreignInputCount != 0 && Spec_->JoinBy.empty()) {
            THROW_ERROR_EXCEPTION("Join key columns are required");
        }
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TSortedReduceController, 0x761aad8e);

    TReduceOperationSpecPtr Spec_;

};

DEFINE_DYNAMIC_PHOENIX_TYPE(TSortedReduceController);

IOperationControllerPtr CreateSortedReduceController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TReduceOperationSpec>(operation->GetSpec());
    return New<TSortedReduceController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

class TJoinReduceController
    : public TSortedReduceControllerBase
{
public:
    TJoinReduceController(
        TSchedulerConfigPtr config,
        TJoinReduceOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TSortedReduceControllerBase(config, spec, config->JoinReduceOperationOptions, host, operation)
        , Spec_(spec)
    {
        RegisterJobProxyMemoryDigest(EJobType::JoinReduce, spec->JobProxyMemoryDigest);
        RegisterUserJobMemoryDigest(EJobType::JoinReduce, spec->Reducer->MemoryReserveFactor);
    }

    virtual bool ShouldSlicePrimaryTableByKeys() const override
    {
        return false;
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::JoinReduce;
    }

    virtual bool IsKeyGuaranteeEnabled() override
    {
        return false;
    }

    virtual void AdjustKeyColumns() override
    {
        LOG_INFO("Spec key columns are %v", Spec_->JoinBy);
        SortKeyColumns_ = ForeignKeyColumns_ = PrimaryKeyColumns_ = CheckInputTablesSorted(Spec_->JoinBy);
    }

    virtual void DoInitialize() override
    {
        TSortedReduceControllerBase::DoInitialize();

        if (InputTables.size() < 2) {
            THROW_ERROR_EXCEPTION("At least two input tables are required");
        }

        int primaryInputCount = 0;
        for (const auto& inputTable : InputTables) {
            if (!inputTable.Path.GetForeign()) {
                ++primaryInputCount;
            }
            if (inputTable.Path.GetTeleport()) {
                THROW_ERROR_EXCEPTION("Teleport tables are not supported in join-reduce");
            }
        }

        if (primaryInputCount != 1) {
            THROW_ERROR_EXCEPTION("You must specify exactly one non-foreign (primary) input table (%v specified)",
                                  primaryInputCount);
        }

        // For join reduce tables with multiple ranges are not supported.
        for (const auto& inputTable : InputTables) {
            auto& path = inputTable.Path;
            auto ranges = path.GetRanges();
            if (ranges.size() > 1) {
                THROW_ERROR_EXCEPTION("Join reduce operation does not support tables with multiple ranges");
            }
        }

        // Forbid teleport attribute for output tables.
        if (GetOutputTeleportTableIndex()) {
            THROW_ERROR_EXCEPTION("Teleport tables are not supported in join-reduce");
        }
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TJoinReduceController, 0x1120ca9f);

    TJoinReduceOperationSpecPtr Spec_;
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TJoinReduceController);

IOperationControllerPtr CreateJoinReduceController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TJoinReduceOperationSpec>(operation->GetSpec());
    return New<TJoinReduceController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
