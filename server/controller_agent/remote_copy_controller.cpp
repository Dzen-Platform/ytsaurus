#include "remote_copy_controller.h"
#include "private.h"
#include "chunk_pool.h"
#include "helpers.h"
#include "job_memory.h"
#include "operation_controller_detail.h"

#include <yt/ytlib/api/config.h>
#include <yt/ytlib/api/native_connection.h>

#include <yt/ytlib/chunk_client/data_slice_descriptor.h>
#include <yt/ytlib/chunk_client/input_chunk_slice.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/hive/cluster_directory.h>

#include <yt/ytlib/node_tracker_client/node_directory_builder.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/transaction_client/helpers.h>

#include <yt/ytlib/table_client/config.h>

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/core/ytree/helpers.h>

namespace NYT {
namespace NControllerAgent {

using namespace NYson;
using namespace NYTree;
using namespace NYPath;
using namespace NChunkServer;
using namespace NJobProxy;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NApi;
using namespace NConcurrency;
using namespace NTableClient;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////

static const NProfiling::TProfiler Profiler("/operations/remote_copy");

////////////////////////////////////////////////////////////////////

class TRemoteCopyController
    : public TOperationControllerBase
{
public:
    TRemoteCopyController(
        TSchedulerConfigPtr config,
        TRemoteCopyOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TOperationControllerBase(config, spec, config->RemoteCopyOperationOptions, host, operation)
        , Spec_(spec)
        , Options_(config->RemoteCopyOperationOptions)
    {
        RegisterJobProxyMemoryDigest(EJobType::RemoteCopy, spec->JobProxyMemoryDigest);
    }

    virtual void BuildBriefSpec(IYsonConsumer* consumer) const override
    {
        TOperationControllerBase::BuildBriefSpec(consumer);
        BuildYsonMapFluently(consumer)
            .Item("cluster_name").Value(Spec_->ClusterName)
            .Item("network_name").Value(Spec_->NetworkName);
    }

    // Persistence.

    virtual void Persist(const TPersistenceContext& context) override
    {
        TOperationControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, RemoteCopyTaskGroup_);
        Persist(context, JobIOConfig_);
        Persist(context, JobSpecTemplate_);
        Persist<TAttributeDictionaryRefSerializer>(context, InputTableAttributes_);
    }

protected:
    virtual TStringBuf GetDataSizeParameterNameForJob(EJobType jobType) const override
    {
        Y_UNREACHABLE();
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TRemoteCopyController, 0xbac5ad82);

    TRemoteCopyOperationSpecPtr Spec_;
    TRemoteCopyOperationOptionsPtr Options_;

    class TRemoteCopyTask
        : public TTask
    {
    public:
        //! For persistence only.
        TRemoteCopyTask()
        { }

        TRemoteCopyTask(TRemoteCopyController* controller, int index)
            : TTask(controller)
            , Controller_(controller)
            , ChunkPool_(CreateAtomicChunkPool())
            , Index_(index)
        { }

        virtual Stroka GetId() const override
        {
            return "RemoteCopy";
        }

        virtual TTaskGroupPtr GetGroup() const override
        {
            return Controller_->RemoteCopyTaskGroup_;
        }

        virtual TDuration GetLocalityTimeout() const override
        {
            return TDuration::Zero();
        }

        virtual bool HasInputLocality() const override
        {
            return false;
        }

        virtual TExtendedJobResources GetNeededResources(TJobletPtr joblet) const override
        {
            return GetRemoteCopyResources(
                joblet->InputStripeList->GetStatistics());
        }

        virtual IChunkPoolInput* GetChunkPoolInput() const override
        {
            return ChunkPool_.get();
        }

        virtual IChunkPoolOutput* GetChunkPoolOutput() const override
        {
            return ChunkPool_.get();
        }

        virtual EJobType GetJobType() const override
        {
            return EJobType::RemoteCopy;
        }

        virtual void Persist(const TPersistenceContext& context) override
        {
            TTask::Persist(context);

            using NYT::Persist;
            Persist(context, Controller_);
            Persist(context, ChunkPool_);
            Persist(context, Index_);
        }

    private:
        DECLARE_DYNAMIC_PHOENIX_TYPE(TRemoteCopyTask, 0x83b0dfe3);

        TRemoteCopyController* Controller_ = nullptr;

        std::unique_ptr<IChunkPool> ChunkPool_;

        int Index_;

        virtual TExtendedJobResources GetMinNeededResourcesHeavy() const override
        {
            return GetRemoteCopyResources(
                ChunkPool_->GetApproximateStripeStatistics());
        }

        TExtendedJobResources GetRemoteCopyResources(const TChunkStripeStatisticsVector& statistics) const
        {
            TExtendedJobResources result;
            result.SetUserSlots(1);
            result.SetCpu(0);
            result.SetJobProxyMemory(GetMemoryResources(statistics));
            AddFootprintAndUserJobResources(result);
            return result;
        }

        i64 GetMemoryResources(const TChunkStripeStatisticsVector& statistics) const
        {
            i64 result = 0;

            // Replication writer
            result += Controller_->Spec_->JobIO->TableWriter->SendWindowSize +
                Controller_->Spec_->JobIO->TableWriter->GroupSize;

            // Max block size
            i64 maxBlockSize = 0;
            for (const auto& stat : statistics) {
                 maxBlockSize = std::max(maxBlockSize, stat.MaxBlockSize);
            }
            result += maxBlockSize;

            return result;
        }

        virtual void BuildJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
        {
            jobSpec->CopyFrom(Controller_->JobSpecTemplate_);
            AddSequentialInputSpec(jobSpec, joblet);
            AddFinalOutputSpecs(jobSpec, joblet);
        }

        virtual void OnJobCompleted(TJobletPtr joblet, const TCompletedJobSummary& jobSummary) override
        {
            TTask::OnJobCompleted(joblet, jobSummary);
            RegisterOutput(joblet, Index_, jobSummary);
        }

        virtual void OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& jobSummary) override
        {
            TTask::OnJobAborted(joblet, jobSummary);
        }
    };

    TTaskGroupPtr RemoteCopyTaskGroup_;

    TJobIOConfigPtr JobIOConfig_;
    TJobSpec JobSpecTemplate_;

    std::unique_ptr<IAttributeDictionary> InputTableAttributes_;

    virtual bool ShouldVerifySortedOutput() const override
    {
        return false;
    }

    // Custom bits of preparation pipeline.
    void InitializeTransactions() override
    {
        StartAsyncSchedulerTransaction();
        StartInputTransaction(NullTransactionId);
        StartOutputTransaction(UserTransactionId);
        StartDebugOutputTransaction();
    }

    virtual void DoInitialize() override
    {
        TOperationControllerBase::DoInitialize();

        RemoteCopyTaskGroup_ = New<TTaskGroup>();
        RegisterTaskGroup(RemoteCopyTaskGroup_);
    }

    virtual void InitializeConnections() override
    {
        TClientOptions options;
        options.User = AuthenticatedUser;

        if (Spec_->ClusterConnection) {
            auto connection = CreateNativeConnection(*Spec_->ClusterConnection);
            AuthenticatedInputMasterClient = connection->CreateNativeClient(options);
        } else {
            AuthenticatedInputMasterClient = Host
                ->GetClusterDirectory()
                ->GetConnectionOrThrow(*Spec_->ClusterName)
                ->CreateNativeClient(options);
        }
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec_->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return std::vector<TRichYPath>(1, Spec_->OutputTablePath);
    }

    virtual std::vector<TPathWithStage> GetFilePaths() const override
    {
        return std::vector<TPathWithStage>();
    }

    virtual void PrepareOutputTables() override
    {
        auto& table = OutputTables[0];

        switch (Spec_->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    InferSchemaFromInputOrdered();
                    break;
                }
                // We intentionally fall into next clause.

            case ESchemaInferenceMode::FromOutput:
                ValidateOutputSchemaOrdered();

                // Since remote copy doesn't unpack blocks and validate schema, we must ensure
                // that schemas are identical.
                for (const auto& inputTable : InputTables) {
                    if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Strong &&
                        inputTable.Schema.ToCanonical() != table.TableUploadOptions.TableSchema.ToCanonical())
                    {
                        THROW_ERROR_EXCEPTION("Cannot make remote copy into table with \"strong\" schema since "
                            "input table schema differs from output table schema")
                            << TErrorAttribute("input_table_schema", inputTable.Schema)
                            << TErrorAttribute("output_table_schema", table.TableUploadOptions.TableSchema);
                    }
                }
                break;

            case ESchemaInferenceMode::FromInput:
                InferSchemaFromInputOrdered();
                break;

            default:
                Y_UNREACHABLE();
        }
    }

    virtual void CustomPrepare() override
    {
        TOperationControllerBase::CustomPrepare();

        LOG_INFO("Processing inputs");

        if (InputHasDynamicTables()) {
            THROW_ERROR_EXCEPTION("Remote copy operation does not support dynamic tables");
        }

        std::vector<TChunkStripePtr> stripes;
        for (const auto& chunkSpec : CollectPrimaryUnversionedChunks()) {
            if (chunkSpec->LowerLimit() && !IsTrivial(*chunkSpec->LowerLimit()) ||
                chunkSpec->UpperLimit() && !IsTrivial(*chunkSpec->UpperLimit()))
            {
                THROW_ERROR_EXCEPTION("Remote copy operation does not support non-trivial table limits");
            }
            stripes.push_back(New<TChunkStripe>(CreateUnversionedInputDataSlice(CreateInputChunkSlice(chunkSpec))));
        }

        auto jobSizeConstraints = CreateSimpleJobSizeConstraints(Spec_, Options_, TotalEstimatedInputDataSize);

        if (stripes.size() > Spec_->MaxChunkCountPerJob * jobSizeConstraints->GetJobCount()) {
            THROW_ERROR_EXCEPTION("Too many chunks per job: actual %v, limit %v; "
                "please merge input tables before starting Remote Copy",
                stripes.size() / jobSizeConstraints->GetJobCount(),
                Spec_->MaxChunkCountPerJob);
        }

        if (Spec_->CopyAttributes) {
            if (InputTables.size() > 1) {
                THROW_ERROR_EXCEPTION("Attributes can be copied only in case of one input table");
            }

            const auto& path = Spec_->InputTablePaths[0].GetPath();

            auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
            TObjectServiceProxy proxy(channel);

            auto req = TObjectYPathProxy::Get(path + "/@");
            SetTransactionId(req, InputTransaction->GetId());

            auto rspOrError = WaitFor(proxy.Execute(req));
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting attributes of input table %v",
                path);

            const auto& rsp = rspOrError.Value();
            InputTableAttributes_ = ConvertToAttributes(TYsonString(rsp->value()));
        }

        BuildTasks(stripes);

        LOG_INFO("Inputs processed");

        InitJobIOConfig();
        InitJobSpecTemplate();
    }

    virtual void CustomCommit() override
    {
        TOperationControllerBase::CustomCommit();

        if (Spec_->CopyAttributes) {
            const auto& path = Spec_->OutputTablePath.GetPath();

            auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
            TObjectServiceProxy proxy(channel);

            auto userAttributeKeys = InputTableAttributes_->Get<std::vector<Stroka>>("user_attribute_keys");
            auto attributeKeys = Spec_->AttributeKeys.Get(userAttributeKeys);

            auto batchReq = proxy.ExecuteBatch();
            for (const auto& key : attributeKeys) {
                auto req = TYPathProxy::Set(path + "/@" + key);
                req->set_value(InputTableAttributes_->GetYson(key).GetData());
                SetTransactionId(req, OutputTransaction->GetId());
                batchReq->AddRequest(req);
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error setting attributes for output table %v",
                path);
        }
    }

    void BuildTasks(const std::vector<TChunkStripePtr>& stripes)
    {
        auto addTask = [this] (const std::vector<TChunkStripePtr>& stripes, int index) {
            auto task = New<TRemoteCopyTask>(this, index);
            task->Initialize();
            task->AddInput(stripes);
            task->FinishInput();
            RegisterTask(task);
        };

        i64 currentDataSize = 0;
        i64 dataSizePerJob = Spec_->DataSizePerJob.Get(Options_->DataSizePerJob);
        std::vector<TChunkStripePtr> currentStripes;
        for (auto stripe : stripes) {
            currentStripes.push_back(stripe);
            currentDataSize += stripe->GetStatistics().DataSize;
            if (currentDataSize >= dataSizePerJob || currentStripes.size() == Options_->MaxDataSlicesPerJob) {
                addTask(currentStripes, Tasks.size());
                currentStripes.clear();
                currentDataSize = 0;
            }
        }
        if (!currentStripes.empty()) {
            addTask(currentStripes, Tasks.size());
        }
    }

    virtual void CustomizeJoblet(TJobletPtr joblet) override
    { }

    virtual bool IsOutputLivePreviewSupported() const override
    {
        return false;
    }

    virtual bool IsParityReplicasFetchEnabled() const override
    {
        return true;
    }

    virtual bool IsCompleted() const override
    {
        return Tasks.size() == JobCounter.GetCompletedTotal();
    }

    // Progress reporting.

    virtual Stroka GetLoggingProgress() const override
    {
        return Format(
            "Jobs = {T: %v, R: %v, C: %v, P: %v, F: %v, A: %v}, "
            "UnavailableInputChunks: %v",
            JobCounter.GetTotal(),
            JobCounter.GetRunning(),
            JobCounter.GetCompletedTotal(),
            GetPendingJobCount(),
            JobCounter.GetFailed(),
            JobCounter.GetAbortedTotal(),
            UnavailableInputChunkCount);
    }


    // Unsorted helpers.

    void InitJobIOConfig()
    {
        JobIOConfig_ = CloneYsonSerializable(Spec_->JobIO);
        InitFinalOutputConfig(JobIOConfig_);
    }

    void InitJobSpecTemplate()
    {
        JobSpecTemplate_.set_type(static_cast<int>(EJobType::RemoteCopy));
        auto* schedulerJobSpecExt = JobSpecTemplate_.MutableExtension(
            TSchedulerJobSpecExt::scheduler_job_spec_ext);

        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig_).GetData());
        schedulerJobSpecExt->set_table_reader_options("");

        ToProto(schedulerJobSpecExt->mutable_data_source_directory(), MakeInputDataSources());

        const auto& clusterDirectory = Host->GetClusterDirectory();
        TNativeConnectionConfigPtr connectionConfig;
        if (Spec_->ClusterConnection) {
            connectionConfig = *Spec_->ClusterConnection;
        } else {
            auto connection = clusterDirectory->GetConnectionOrThrow(*Spec_->ClusterName);
            connectionConfig = CloneYsonSerializable(connection->GetConfig());
        }
        if (Spec_->NetworkName) {
            connectionConfig->Networks = {*Spec_->NetworkName};
        }

        auto* remoteCopyJobSpecExt = JobSpecTemplate_.MutableExtension(TRemoteCopyJobSpecExt::remote_copy_job_spec_ext);
        remoteCopyJobSpecExt->set_connection_config(ConvertToYsonString(connectionConfig).GetData());
    }

};

DEFINE_DYNAMIC_PHOENIX_TYPE(TRemoteCopyController);
DEFINE_DYNAMIC_PHOENIX_TYPE(TRemoteCopyController::TRemoteCopyTask);

IOperationControllerPtr CreateRemoteCopyController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TRemoteCopyOperationSpec>(operation->GetSpec());
    return New<TRemoteCopyController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT


