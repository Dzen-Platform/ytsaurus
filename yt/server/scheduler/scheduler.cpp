#include "scheduler.h"
#include "private.h"
#include "event_log.h"
#include "fair_share_strategy.h"
#include "helpers.h"
#include "job_prober_service.h"
#include "job_resources.h"
#include "map_controller.h"
#include "master_connector.h"
#include "merge_controller.h"
#include "node_shard.h"
#include "operation_controller.h"
#include "remote_copy_controller.h"
#include "scheduler_strategy.h"
#include "snapshot_downloader.h"
#include "sort_controller.h"

#include <yt/server/exec_agent/public.h>

#include <yt/server/cell_scheduler/bootstrap.h>
#include <yt/server/cell_scheduler/config.h>

#include <yt/ytlib/job_prober_client/job_prober_service_proxy.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/ytlib/shell/config.h>

#include <yt/ytlib/node_tracker_client/channel.h>

#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_buffered_table_writer.h>
#include <yt/ytlib/table_client/schemaless_writer.h>
#include <yt/ytlib/table_client/table_consumer.h>

#include <yt/ytlib/api/transaction.h>
#include <yt/ytlib/api/native_connection.h>

#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/thread_affinity.h>
#include <yt/core/concurrency/thread_pool.h>

#include <yt/core/rpc/message.h>
#include <yt/core/rpc/response_keeper.h>

#include <yt/core/misc/lock_free.h>
#include <yt/core/misc/finally.h>

#include <yt/core/profiling/scoped_timer.h>
#include <yt/core/profiling/profile_manager.h>

#include <yt/core/ytree/service_combiner.h>
#include <yt/core/ytree/virtual.h>

namespace NYT {
namespace NScheduler {

using namespace NProfiling;
using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NRpc;
using namespace NApi;
using namespace NCellScheduler;
using namespace NObjectClient;
using namespace NHydra;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient;
using namespace NChunkClient;
using namespace NJobProberClient;
using namespace NNodeTrackerClient;
using namespace NTableClient;
using namespace NNodeTrackerServer;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NSecurityClient;
using namespace NShell;

using NNodeTrackerClient::TNodeId;
using NNodeTrackerClient::TNodeDescriptor;
using NNodeTrackerClient::TNodeDirectory;

////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;
static const auto& Profiler = SchedulerProfiler;
static const auto ProfilingPeriod = TDuration::Seconds(1);

////////////////////////////////////////////////////////////////////

class TScheduler::TImpl
    : public TRefCounted
    , public IOperationHost
    , public ISchedulerStrategyHost
    , public INodeShardHost
    , public TEventLogHostBase
{
public:
    using TEventLogHostBase::LogEventFluently;

    TImpl(
        TSchedulerConfigPtr config,
        TBootstrap* bootstrap)
        : Config_(config)
        , InitialConfig_(ConvertToNode(Config_))
        , Bootstrap_(bootstrap)
        , SnapshotIOQueue_(New<TActionQueue>("SnapshotIO"))
        , ControllerThreadPool_(New<TThreadPool>(Config_->ControllerThreadCount, "Controller"))
        , JobSpecBuilderThreadPool_(New<TThreadPool>(Config_->JobSpecBuilderThreadCount, "SpecBuilder"))
        , StatisticsAnalyzerThreadPool_(New<TThreadPool>(Config_->StatisticsAnalyzerThreadCount, "Statistics"))
        , MasterConnector_(new TMasterConnector(Config_, Bootstrap_))
        , TotalResourceLimitsProfiler_(Profiler.GetPathPrefix() + "/total_resource_limits")
        , TotalResourceUsageProfiler_(Profiler.GetPathPrefix() + "/total_resource_usage")
        , TotalCompletedJobTimeCounter_("/total_completed_job_time")
        , TotalFailedJobTimeCounter_("/total_failed_job_time")
        , TotalAbortedJobTimeCounter_("/total_aborted_job_time")
    {
        YCHECK(config);
        YCHECK(bootstrap);
        VERIFY_INVOKER_THREAD_AFFINITY(GetControlInvoker(), ControlThread);

        auto primaryMasterCellTag = GetMasterClient()->GetNativeConnection()->GetPrimaryMasterCellTag();
        for (int i = 0; i < Config_->NodeShardCount; ++i) {
            NodeShards_.push_back(New<TNodeShard>(i, primaryMasterCellTag, Config_, this, Bootstrap_));
        }

        auto localHostName = TAddressResolver::Get()->GetLocalHostName();
        int port = Bootstrap_->GetConfig()->RpcPort;
        ServiceAddress_ = BuildServiceAddress(localHostName, port);

        for (auto state : TEnumTraits<EJobState>::GetDomainValues()) {
            JobStateToTag_[state] = TProfileManager::Get()->RegisterTag("state", Format("%lv", state));
        }
        for (auto type : TEnumTraits<EJobType>::GetDomainValues()) {
            JobTypeToTag_[type] = TProfileManager::Get()->RegisterTag("type", Format("%lv", type));
        }
        for (auto reason : TEnumTraits<EAbortReason>::GetDomainValues()) {
            JobAbortReasonToTag_[reason] = TProfileManager::Get()->RegisterTag("reason", Format("%lv", reason));
        }
    }

    void Initialize()
    {
        InitStrategy();

        MasterConnector_->AddGlobalWatcherRequester(BIND(
            &TImpl::RequestPools,
            Unretained(this)));
        MasterConnector_->AddGlobalWatcherHandler(BIND(
            &TImpl::HandlePools,
            Unretained(this)));

        MasterConnector_->AddGlobalWatcherRequester(BIND(
            &TImpl::RequestNodesAttributes,
            Unretained(this)));
        MasterConnector_->AddGlobalWatcherHandler(BIND(
            &TImpl::HandleNodesAttributes,
            Unretained(this)));

        MasterConnector_->AddGlobalWatcherRequester(BIND(
            &TImpl::RequestConfig,
            Unretained(this)));
        MasterConnector_->AddGlobalWatcherHandler(BIND(
            &TImpl::HandleConfig,
            Unretained(this)));

        MasterConnector_->SubscribeMasterConnected(BIND(
            &TImpl::OnMasterConnected,
            Unretained(this)));
        MasterConnector_->SubscribeMasterDisconnected(BIND(
            &TImpl::OnMasterDisconnected,
            Unretained(this)));

        MasterConnector_->SubscribeUserTransactionAborted(BIND(
            &TImpl::OnUserTransactionAborted,
            Unretained(this)));
        MasterConnector_->SubscribeSchedulerTransactionAborted(BIND(
            &TImpl::OnSchedulerTransactionAborted,
            Unretained(this)));

        MasterConnector_->Start();

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TImpl::OnProfiling, MakeWeak(this)),
            ProfilingPeriod);
        ProfilingExecutor_->Start();

        auto nameTable = New<TNameTable>();
        auto options = New<TTableWriterOptions>();
        options->ValidateDuplicateIds = true;
        options->ValidateRowWeight = true;
        options->ValidateColumnCount = true;

        EventLogWriter_ = CreateSchemalessBufferedTableWriter(
            Config_->EventLog,
            options,
            GetMasterClient(),
            nameTable,
            Config_->EventLog->Path);

        // Open is always synchronous for buffered writer.
        YCHECK(EventLogWriter_->Open().IsSet());

        EventLogValueConsumer_.reset(new TWritingValueConsumer(EventLogWriter_, true));
        EventLogTableConsumer_.reset(new TTableConsumer(EventLogValueConsumer_.get()));

        LogEventFluently(ELogEventType::SchedulerStarted)
            .Item("address").Value(ServiceAddress_);

        LoggingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TImpl::OnLogging, MakeWeak(this)),
            Config_->ClusterInfoLoggingPeriod);
        LoggingExecutor_->Start();

        PendingEventLogRowsFlushExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TImpl::OnPendingEventLogRowsFlush, MakeWeak(this)),
            Config_->PendingEventLogRowsFlushPeriod);
        PendingEventLogRowsFlushExecutor_->Start();

        UpdateExecNodeDescriptorsExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TImpl::UpdateExecNodeDescriptors, MakeWeak(this)),
            Config_->UpdateExecNodeDescriptorsPeriod);
        UpdateExecNodeDescriptorsExecutor_->Start();
    }

    ISchedulerStrategyPtr GetStrategy()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Strategy_;
    }

    IYPathServicePtr GetOrchidService()
    {
        auto staticOrchidProducer = BIND(&TImpl::BuildStaticOrchid, MakeStrong(this));
        auto staticOrchidService = IYPathService::FromProducer(staticOrchidProducer)
            ->Via(GetControlInvoker())
            ->Cached(Config_->StaticOrchidCacheUpdatePeriod);

        auto dynamicOrchidService = GetDynamicOrchidService()
            ->Via(GetControlInvoker());

        return New<TServiceCombiner>(std::vector<IYPathServicePtr> {
            staticOrchidService,
            dynamicOrchidService
        });
    }

    std::vector<TOperationPtr> GetOperations()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        std::vector<TOperationPtr> operations;
        for (const auto& pair : IdToOperation_) {
            operations.push_back(pair.second);
        }
        return operations;
    }

    IInvokerPtr GetSnapshotIOInvoker()
    {
        return SnapshotIOQueue_->GetInvoker();
    }

    bool IsConnected()
    {
        return MasterConnector_->IsConnected();
    }

    void ValidateConnected()
    {
        if (!IsConnected()) {
            THROW_ERROR_EXCEPTION(GetMasterDisconnectedError());
        }
    }


    TOperationPtr FindOperation(const TOperationId& id)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto it = IdToOperation_.find(id);
        return it == IdToOperation_.end() ? nullptr : it->second;
    }

    TOperationPtr GetOperation(const TOperationId& id)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto operation = FindOperation(id);
        YCHECK(operation);
        return operation;
    }

    TOperationPtr GetOperationOrThrow(const TOperationId& id)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto operation = FindOperation(id);
        if (!operation) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::NoSuchOperation,
                "No such operation %v",
                id);
        }
        return operation;
    }


    virtual int GetExecNodeCount() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        int execNodeCount = 0;
        for (auto& nodeShard : NodeShards_) {
            execNodeCount += nodeShard->GetExecNodeCount();
        }
        return execNodeCount;
    }

    virtual int GetTotalNodeCount() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        int totalNodeCount = 0;
        for (auto& nodeShard : NodeShards_) {
            totalNodeCount += nodeShard->GetTotalNodeCount();
        }
        return totalNodeCount;
    }

    virtual std::vector<TExecNodeDescriptor> GetExecNodeDescriptors(const TNullable<Stroka>& tag) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        if (!tag) {
            TReaderGuard guard(ExecNodeDescriptorsLock_);

            return CachedExecNodeDescriptors_;
        }

        auto now = TInstant::Now();

        {
            TReaderGuard guard(ExecNodeDescriptorsByTagLock_);

            auto it = CachedExecNodeDescriptorsByTag_.find(*tag);
            if (it != CachedExecNodeDescriptorsByTag_.end() &&
                now <= it->second.first + Config_->UpdateExecNodeDescriptorsPeriod)
            {
                it->second.first = now;
                return it->second.second;
            }
        }

        std::vector<TExecNodeDescriptor> result;

        {
            TReaderGuard guard(ExecNodeDescriptorsLock_);

            for (const auto& descriptor : CachedExecNodeDescriptors_) {
                if (descriptor.CanSchedule(tag)) {
                    result.push_back(descriptor);
                }
            }
        }

        {
            TWriterGuard guard(ExecNodeDescriptorsByTagLock_);
            CachedExecNodeDescriptorsByTag_[*tag] = std::make_pair(now, result);
        }

        return result;
    }

    virtual void RegisterAlert(EAlertType alertType, const TError& alert) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_WARNING(alert, "Registering %v alert", alertType);

        GetMasterConnector()->RegisterAlert(alertType, alert);
    }

    virtual void UnregisterAlert(EAlertType alertType) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        GetMasterConnector()->UnregisterAlert(alertType);
    }

    virtual TFuture<void> CheckPoolPermission(
        const TYPath& path,
        const Stroka& user,
        EPermission permission) override
    {
        auto client = GetMasterClient();
        return client->CheckPermission(user, GetPoolsPath() + path, permission)
            .Apply(BIND([=] (const TCheckPermissionResult& result) {
                if (result.Action == ESecurityAction::Deny) {
                    THROW_ERROR_EXCEPTION(
                        NSecurityClient::EErrorCode::AuthorizationError,
                        "User %Qv has been denied access to pool %v",
                        user,
                        path)
                        << result.ToError(user, permission);
                }
            }));
    }


    void ValidateOperationPermission(
        const Stroka& user,
        const TOperationId& operationId,
        EPermission permission) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto path = GetOperationPath(operationId);

        auto client = GetMasterClient();
        auto asyncResult = client->CheckPermission(user, path, permission);
        auto resultOrError = WaitFor(asyncResult);
        if (!resultOrError.IsOK()) {
            THROW_ERROR_EXCEPTION("Error checking permission for operation %v",
                operationId)
                << resultOrError;
        }

        const auto& result = resultOrError.Value();
        if (result.Action == ESecurityAction::Deny) {
            THROW_ERROR_EXCEPTION(
                NSecurityClient::EErrorCode::AuthorizationError,
                "User %Qv has been denied access to operation %v",
                user,
                operationId);
        }
    }

    TFuture<TOperationPtr> StartOperation(
        EOperationType type,
        const TTransactionId& transactionId,
        const TMutationId& mutationId,
        IMapNodePtr spec,
        const Stroka& user)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (static_cast<int>(IdToOperation_.size()) >= Config_->MaxOperationCount) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::TooManyOperations,
                "Limit for the total number of concurrent operations %v has been reached",
                Config_->MaxOperationCount);
        }

        // Attach user transaction if any. Don't ping it.
        TTransactionAttachOptions userAttachOptions;
        userAttachOptions.Ping = false;
        userAttachOptions.PingAncestors = false;
        auto userTransaction = transactionId
            ? GetMasterClient()->AttachTransaction(transactionId, userAttachOptions)
            : nullptr;

        // Merge operation spec with template
        auto specTemplate = GetSpecTemplate(type, spec);
        if (specTemplate) {
            spec = UpdateNode(specTemplate, spec)->AsMap();
        }

        TOperationSpecBasePtr operationSpec;
        try {
            operationSpec = ConvertTo<TOperationSpecBasePtr>(spec);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing operation spec") << ex;
        }

        // Create operation object.
        auto operationId = MakeRandomId(
            EObjectType::Operation,
            GetMasterClient()->GetNativeConnection()->GetPrimaryMasterCellTag());
        auto operation = New<TOperation>(
            operationId,
            type,
            mutationId,
            userTransaction,
            spec,
            user,
            operationSpec->Owners,
            TInstant::Now());
        operation->SetState(EOperationState::Initializing);

        WaitFor(Strategy_->ValidateOperationStart(operation))
            .ThrowOnError();

        LOG_INFO("Starting operation (OperationType: %v, OperationId: %v, TransactionId: %v, User: %v)",
            type,
            operationId,
            transactionId,
            user);

        LOG_INFO("Total resource limits (OperationId: %v, ResourceLimits: %v)",
            operationId,
            FormatResources(GetTotalResourceLimits()));

        // Spawn a new fiber where all startup logic will work asynchronously.
        BIND(&TImpl::DoStartOperation, MakeStrong(this), operation)
            .AsyncVia(MasterConnector_->GetCancelableControlInvoker())
            .Run();

        return operation->GetStarted();
    }

    TFuture<void> AbortOperation(TOperationPtr operation, const TError& error, const Stroka& user)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        ValidateOperationPermission(user, operation->GetId(), EPermission::Write);

        if (operation->IsFinishingState() || operation->IsFinishedState()) {
            LOG_INFO(error, "Operation is already shuting down (OperationId: %v, State: %v)",
                operation->GetId(),
                operation->GetState());
            return operation->GetFinished();
        }

        LOG_INFO(error, "Aborting operation (OperationId: %v, State: %v)",
            operation->GetId(),
            operation->GetState());

        TerminateOperation(
            operation,
            EOperationState::Aborting,
            EOperationState::Aborted,
            ELogEventType::OperationAborted,
            error);

        return operation->GetFinished();
    }

    TFuture<void> SuspendOperation(TOperationPtr operation, const Stroka& user, bool abortRunningJobs)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        ValidateOperationPermission(user, operation->GetId(), EPermission::Write);

        if (operation->IsFinishingState() || operation->IsFinishedState()) {
            return MakeFuture(TError(
                EErrorCode::InvalidOperationState,
                "Cannot suspend operation in %Qlv state",
                operation->GetState()));
        }

        operation->SetSuspended(true);

        if (abortRunningJobs) {
            AbortOperationJobs(operation, TError("Suspend operation by user request"));
        }

        LOG_INFO("Operation suspended (OperationId: %v)",
            operation->GetId());

        return MasterConnector_->FlushOperationNode(operation);
    }

    TFuture<void> ResumeOperation(TOperationPtr operation, const Stroka& user)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        ValidateOperationPermission(user, operation->GetId(), EPermission::Write);

        if (!operation->GetSuspended()) {
            return MakeFuture(TError(
                EErrorCode::InvalidOperationState,
                "Operation is not suspended. Its state %Qlv",
                operation->GetState()));
        }

        operation->SetSuspended(false);

        LOG_INFO("Operation resumed (OperationId: %v)",
            operation->GetId());

        return MasterConnector_->FlushOperationNode(operation);
    }

    TFuture<void> CompleteOperation(TOperationPtr operation, const TError& error, const Stroka& user)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        ValidateOperationPermission(user, operation->GetId(), EPermission::Write);

        if (operation->IsFinishingState() || operation->IsFinishedState()) {
            LOG_INFO(error, "Operation is already shuting down (OperationId: %v, State: %v)",
                operation->GetId(),
                operation->GetState());
            return operation->GetFinished();
        }
        if (operation->GetState() != EOperationState::Running) {
            return MakeFuture(TError(
                EErrorCode::InvalidOperationState,
                "Operation is not running. Its state is %Qlv",
                operation->GetState()));
        }

        LOG_INFO(error, "Completing operation (OperationId: %v, State: %v)",
            operation->GetId(),
            operation->GetState());

        auto controller = operation->GetController();
        YCHECK(controller);
        controller->Complete();

        return operation->GetFinished();
    }

    TFuture<TYsonString> Strace(const TJobId& jobId, const Stroka& user)
    {
        auto nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::StraceJob, nodeShard, jobId, user)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    TFuture<void> DumpInputContext(const TJobId& jobId, const TYPath& path, const Stroka& user)
    {
        auto nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::DumpJobInputContext, nodeShard, jobId, path, user)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    TFuture<void> SignalJob(const TJobId& jobId, const Stroka& signalName, const Stroka& user)
    {
        auto nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::SignalJob, nodeShard, jobId, signalName, user)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    TFuture<void> AbandonJob(const TJobId& jobId, const Stroka& user)
    {
        auto nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::AbandonJob, nodeShard, jobId, user)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    TFuture<TYsonString> PollJobShell(const TJobId& jobId, const TYsonString& parameters, const Stroka& user)
    {
        auto nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::PollJobShell, nodeShard, jobId, parameters, user)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    TFuture<void> AbortJob(const TJobId& jobId, const Stroka& user)
    {
        auto nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::AbortJob, nodeShard, jobId, user)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    void ProcessHeartbeat(TCtxHeartbeatPtr context)
    {
        auto* request = &context->Request();
        auto nodeId = request->node_id();

        auto nodeShard = GetNodeShard(nodeId);
        auto operationsToLog = WaitFor(
            BIND(&TNodeShard::ProcessHeartbeat, nodeShard)
                .AsyncVia(nodeShard->GetInvoker())
                .Run(context))
            .ValueOrThrow();

        // NB: Do heavy logging after responding to heartbeat.
        for (const auto& operationId : operationsToLog) {
            auto operation = FindOperation(operationId);
            if (!operation) {
                continue;
            }
            LogOperationProgress(operation);
        }
    }


    // ISchedulerStrategyHost implementation
    virtual TMasterConnector* GetMasterConnector() override
    {
        return MasterConnector_.get();
    }

    virtual TJobResources GetTotalResourceLimits() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto totalResourceLimits = ZeroJobResources();
        for (auto& nodeShard : NodeShards_) {
            totalResourceLimits += nodeShard->GetTotalResourceLimits();
        }
        return totalResourceLimits;
    }

    TJobResources GetTotalResourceUsage()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto totalResourceUsage = ZeroJobResources();
        for (auto& nodeShard : NodeShards_) {
            totalResourceUsage += nodeShard->GetTotalResourceUsage();
        }
        return totalResourceUsage;
    }

    virtual TJobResources GetResourceLimits(const TNullable<Stroka>& tag) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto resourceLimits = ZeroJobResources();
        for (auto& nodeShard : NodeShards_) {
            resourceLimits += nodeShard->GetResourceLimits(tag);
        }
        return resourceLimits;
    }

    int GetActiveJobCount()
    {
        int activeJobCount = 0;
        for (auto& nodeShard : NodeShards_) {
             activeJobCount += nodeShard->GetActiveJobCount();
        }
        return activeJobCount;
    }

    virtual void ActivateOperation(const TOperationId& operationId) override
    {
        auto operation = GetOperation(operationId);

        auto codicilGuard = operation->MakeCodicilGuard();

        operation->SetActivated(true);
        if (operation->GetPrepared()) {
            MaterializeOperation(operation);
        }
    }

    void MaterializeOperation(TOperationPtr operation)
    {
        auto controller = operation->GetController();
        // TODO(ignat): avoid non-necessary async call here if operation is successfully revived.
        operation->SetState(EOperationState::Materializing);
        BIND(&IOperationController::Materialize, controller)
            .AsyncVia(controller->GetCancelableInvoker())
            .Run()
            .Subscribe(BIND([operation] (const TError& error) {
                if (error.IsOK()) {
                    if (operation->GetState() == EOperationState::Materializing) {
                        operation->SetState(EOperationState::Running);
                    }
                }
            })
            .Via(controller->GetCancelableControlInvoker()));
    }


    // IOperationHost implementation
    virtual NApi::INativeClientPtr GetMasterClient() override
    {
        return Bootstrap_->GetMasterClient();
    }

    virtual NHiveClient::TClusterDirectoryPtr GetClusterDirectory() override
    {
        return Bootstrap_->GetClusterDirectory();
    }

    virtual IInvokerPtr GetControlInvoker() override
    {
        return Bootstrap_->GetControlInvoker();
    }

    virtual IInvokerPtr CreateOperationControllerInvoker() override
    {
        return CreateSerializedInvoker(ControllerThreadPool_->GetInvoker());
    }

    virtual TThrottlerManagerPtr GetChunkLocationThrottlerManager() const override
    {
        return Bootstrap_->GetChunkLocationThrottlerManager();
    }

    virtual IYsonConsumer* GetEventLogConsumer() override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return EventLogTableConsumer_.get();
    }

    virtual void OnOperationCompleted(TOperationPtr operation) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        MasterConnector_->GetCancelableControlInvoker()->Invoke(
            BIND(&TImpl::DoCompleteOperation, MakeStrong(this), operation));
    }

    virtual void OnOperationFailed(TOperationPtr operation, const TError& error) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        MasterConnector_->GetCancelableControlInvoker()->Invoke(
            BIND(&TImpl::DoFailOperation, MakeStrong(this), operation, error));
    }

    virtual std::unique_ptr<IValueConsumer> CreateLogConsumer() override
    {
        return std::unique_ptr<IValueConsumer>(new TEventLogValueConsumer(this));
    }

    // INodeShardHost implementation
    int GetNodeShardId(TNodeId nodeId) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return nodeId % NodeShards_.size();
    }

    IInvokerPtr GetStatisticsAnalyzerInvoker() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return StatisticsAnalyzerThreadPool_->GetInvoker();
    }

    IInvokerPtr GetJobSpecBuilderInvoker() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return JobSpecBuilderThreadPool_->GetInvoker();
    }

    TFuture<void> UpdateOperationWithFinishedJob(
        const TOperationId& operationId,
        const TJobId& jobId,
        bool jobFailedOrAborted,
        NYson::TYsonString jobAttributes,
        const TChunkId& stderrChunkId,
        const TChunkId& failContextChunkId,
        TFuture<TYsonString> inputPathsFuture) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND(&TImpl::DoUpdateOperationWithFinishedJob, MakeStrong(this))
            .AsyncVia(MasterConnector_->GetCancelableControlInvoker())
            .Run(
                operationId,
                jobId,
                jobFailedOrAborted,
                jobAttributes,
                stderrChunkId,
                failContextChunkId,
                inputPathsFuture);
    }

    TFuture<void> AttachJobContext(
        const NYTree::TYPath& path,
        const NChunkClient::TChunkId& chunkId,
        const TOperationId& operationId,
        const TJobId& jobId) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND(&TImpl::DoAttachJobContext, MakeStrong(this))
            .AsyncVia(MasterConnector_->GetCancelableControlInvoker())
            .Run(path, chunkId, operationId, jobId);
    }

    TJobProberServiceProxy CreateJobProberProxy(const Stroka& address) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto factory = GetMasterClient()->GetLightChannelFactory();
        auto channel = factory->CreateChannel(address);

        TJobProberServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(Config_->JobProberRpcTimeout);
        return proxy;
    }

private:
    const TSchedulerConfigPtr Config_;
    const INodePtr InitialConfig_;
    TBootstrap* const Bootstrap_;

    TActionQueuePtr SnapshotIOQueue_;
    TThreadPoolPtr ControllerThreadPool_;
    TThreadPoolPtr JobSpecBuilderThreadPool_;
    TThreadPoolPtr StatisticsAnalyzerThreadPool_;

    std::unique_ptr<TMasterConnector> MasterConnector_;

    ISchedulerStrategyPtr Strategy_;

    TNodeDirectoryPtr NodeDirectory_ = New<TNodeDirectory>();

    typedef yhash_map<TOperationId, TOperationPtr> TOperationIdMap;
    TOperationIdMap IdToOperation_;

    typedef std::vector<TExecNodeDescriptor> TExecNodeDescriptors;
    NConcurrency::TReaderWriterSpinLock ExecNodeDescriptorsLock_;
    TExecNodeDescriptors CachedExecNodeDescriptors_;

    NConcurrency::TReaderWriterSpinLock ExecNodeDescriptorsByTagLock_;
    mutable yhash_map<Stroka, std::pair<TInstant, TExecNodeDescriptors>> CachedExecNodeDescriptorsByTag_;

    TProfiler TotalResourceLimitsProfiler_;
    TProfiler TotalResourceUsageProfiler_;

    TSimpleCounter TotalCompletedJobTimeCounter_;
    TSimpleCounter TotalFailedJobTimeCounter_;
    TSimpleCounter TotalAbortedJobTimeCounter_;

    TEnumIndexedVector<TTagId, EJobState> JobStateToTag_;
    TEnumIndexedVector<TTagId, EJobType> JobTypeToTag_;
    TEnumIndexedVector<TTagId, EAbortReason> JobAbortReasonToTag_;

    TPeriodicExecutorPtr ProfilingExecutor_;
    TPeriodicExecutorPtr LoggingExecutor_;
    TPeriodicExecutorPtr PendingEventLogRowsFlushExecutor_;
    TPeriodicExecutorPtr UpdateExecNodeDescriptorsExecutor_;

    Stroka ServiceAddress_;

    std::vector<TNodeShardPtr> NodeShards_;

    class TEventLogValueConsumer
        : public IValueConsumer
    {
    public:
        explicit TEventLogValueConsumer(TScheduler::TImpl* host)
            : Host_(host)
        { }

        virtual TNameTablePtr GetNameTable() const override
        {
            return Host_->EventLogWriter_->GetNameTable();
        }

        virtual bool GetAllowUnknownColumns() const override
        {
            return true;
        }

        virtual void OnBeginRow() override
        { }

        virtual void OnValue(const TUnversionedValue& value) override
        {
            Builder_.AddValue(value);
        }

        virtual void OnEndRow() override
        {
            Host_->PendingEventLogRows_.Enqueue(Builder_.FinishRow());
        }

    private:
        TScheduler::TImpl* const Host_;
        TUnversionedOwningRowBuilder Builder_;

    };

    ISchemalessWriterPtr EventLogWriter_;
    std::unique_ptr<IValueConsumer> EventLogValueConsumer_;
    std::unique_ptr<IYsonConsumer> EventLogTableConsumer_;
    TMultipleProducerSingleConsumerLockFreeStack<TUnversionedOwningRow> PendingEventLogRows_;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);


    TNodeShardPtr GetNodeShard(TNodeId nodeId)
    {
        return NodeShards_[GetNodeShardId(nodeId)];
    }

    TNodeShardPtr GetNodeShardByJobId(TJobId jobId)
    {
        auto nodeId = NodeIdFromJobId(jobId);
        return GetNodeShard(nodeId);
    }

    bool ShouldCreateJobNode(const TOperationPtr& operation, bool jobFailedOrAborted, bool hasStderr)
    {
        if (operation->GetJobNodeCount() >= Config_->MaxJobNodesPerOperation) {
            return false;
        }
        if (!jobFailedOrAborted) {
            return hasStderr && operation->GetStderrCount() < operation->GetMaxStderrCount();
        }
        return true;
    }

    void DoUpdateOperationWithFinishedJob(
        const TOperationId& operationId,
        const TJobId& jobId,
        bool jobFailedOrAborted,
        NYson::TYsonString jobAttributes,
        const TChunkId& stderrChunkId,
        const TChunkId& failContextChunkId,
        TFuture<TYsonString> inputPathsFuture)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto operation = FindOperation(operationId);
        if (!operation) {
            LOG_DEBUG("Dangling finished job found (JobId: %v, OperationId: %v)",
                jobId,
                operationId);
            return;
        }

        YCHECK(jobFailedOrAborted || !failContextChunkId);

        if (ShouldCreateJobNode(operation, jobFailedOrAborted, stderrChunkId != NullChunkId)) {
            TCreateJobNodeRequest request;
            request.OperationId = operationId;
            request.JobId = jobId;
            request.Attributes = jobAttributes;
            request.StderrChunkId = stderrChunkId;
            request.FailContextChunkId = failContextChunkId;
            request.InputPathsFuture = inputPathsFuture;

            MasterConnector_->CreateJobNode(request);

            if (stderrChunkId) {
                operation->SetStderrCount(operation->GetStderrCount() + 1);
            }
            operation->SetJobNodeCount(operation->GetJobNodeCount() + 1);
        } else {
            if (stderrChunkId) {
                ReleaseStderrChunk(operation, stderrChunkId);
            }
        }
    }

    void ReleaseStderrChunk(const TOperationPtr& operation, const TChunkId& chunkId)
    {
        auto cellTag = CellTagFromId(chunkId);
        auto channel = GetMasterClient()->GetMasterChannelOrThrow(NApi::EMasterChannelKind::Leader, cellTag);
        TChunkServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();
        auto req = batchReq->add_unstage_chunk_tree_subrequests();
        ToProto(req->mutable_chunk_tree_id(), chunkId);
        req->set_recursive(false);

        // Fire-and-forget.
        // The subscriber is only needed to log the outcome.
        batchReq->Invoke().Subscribe(
            BIND(&TImpl::OnStderrChunkReleased, MakeStrong(this)));
    }

    void OnStderrChunkReleased(const TChunkServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError)
    {
        // NB: We only look at the topmost error and ignore subresponses.
        if (!batchRspOrError.IsOK()) {
            LOG_WARNING(batchRspOrError, "Error releasing stderr chunk");
        }
    }

    void DoAttachJobContext(
        const NYTree::TYPath& path,
        const NChunkClient::TChunkId& chunkId,
        const TOperationId& operationId,
        const TJobId& jobId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        MasterConnector_->AttachJobContext(path, chunkId, operationId, jobId);
    }

    void OnProfiling()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        std::vector<TJobCounter> shardJobCounter(NodeShards_.size());
        std::vector<TAbortedJobCounter> shardAbortedJobCounter(NodeShards_.size());

        for (int i = 0; i < NodeShards_.size(); ++i) {
            auto& nodeShard = NodeShards_[i];
            shardJobCounter[i] = nodeShard->GetJobCounter();
            shardAbortedJobCounter[i] = nodeShard->GetAbortedJobCounter();
        }

        for (auto state : TEnumTraits<EJobState>::GetDomainValues()) {
            for (auto type : TEnumTraits<EJobType>::GetDomainValues()) {
                TTagIdList commonTags = {JobStateToTag_[state], JobTypeToTag_[type]};
                if (state == EJobState::Aborted) {
                    for (auto reason : TEnumTraits<EAbortReason>::GetDomainValues()) {
                        auto tags = commonTags;
                        tags.push_back(JobAbortReasonToTag_[reason]);
                        int counter = 0;
                        for (int i = 0; i < NodeShards_.size(); ++i) {
                            counter += shardAbortedJobCounter[i][reason][state][type];
                        }
                        Profiler.Enqueue("/job_count", counter, EMetricType::Counter, tags);
                    }
                } else {
                    int counter = 0;
                    for (int i = 0; i < NodeShards_.size(); ++i) {
                        counter += shardJobCounter[i][state][type];
                    }
                    Profiler.Enqueue("/job_count", counter, EMetricType::Counter, commonTags);
                }
            }
        }

        Profiler.Enqueue("/active_job_count", GetActiveJobCount(), EMetricType::Gauge);

        Profiler.Enqueue("/exec_node_count", GetExecNodeCount(), EMetricType::Gauge);
        Profiler.Enqueue("/total_node_count", GetTotalNodeCount(), EMetricType::Gauge);

        ProfileResources(TotalResourceLimitsProfiler_, GetTotalResourceLimits());
        ProfileResources(TotalResourceUsageProfiler_, GetTotalResourceUsage());

        {
            TJobTimeStatisticsDelta jobTimeStatisticsDelta;
            for (auto& nodeShard : NodeShards_) {
                jobTimeStatisticsDelta += nodeShard->GetJobTimeStatisticsDelta();
            }
            Profiler.Increment(TotalCompletedJobTimeCounter_, jobTimeStatisticsDelta.CompletedJobTimeDelta);
            Profiler.Increment(TotalFailedJobTimeCounter_, jobTimeStatisticsDelta.FailedJobTimeDelta);
            Profiler.Increment(TotalAbortedJobTimeCounter_, jobTimeStatisticsDelta.AbortedJobTimeDelta);
        }
    }

    void OnLogging()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (IsConnected()) {
            LogEventFluently(ELogEventType::ClusterInfo)
                .Item("exec_node_count").Value(GetExecNodeCount())
                .Item("total_node_count").Value(GetTotalNodeCount())
                .Item("resource_limits").Value(GetTotalResourceLimits())
                .Item("resource_usage").Value(GetTotalResourceUsage());
        }
    }


    void OnPendingEventLogRowsFlush()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (IsConnected()) {
            auto owningRows = PendingEventLogRows_.DequeueAll();
            std::vector<TUnversionedRow> rows(owningRows.begin(), owningRows.end());
            EventLogWriter_->Write(rows);
        }
    }


    void OnMasterConnected(const TMasterHandshakeResult& result)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto responseKeeper = Bootstrap_->GetResponseKeeper();
        responseKeeper->Start();

        LogEventFluently(ELogEventType::MasterConnected)
            .Item("address").Value(ServiceAddress_);

        for (const auto& operationReport : result.OperationReports) {
            const auto& operation = operationReport.Operation;
            if (operation->GetState() == EOperationState::Aborting) {
                AbortAbortingOperation(operation, operationReport.ControllerTransactions);
            } else {
                if (operationReport.UserTransactionAborted) {
                    OnUserTransactionAborted(operation);
                } else {
                    ReviveOperation(operation, operationReport.ControllerTransactions);
                }
            }
        }

        Strategy_->StartPeriodicActivity();
    }

    void OnMasterDisconnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Starting scheduler state cleanup");

        auto responseKeeper = Bootstrap_->GetResponseKeeper();
        responseKeeper->Stop();

        LogEventFluently(ELogEventType::MasterDisconnected)
            .Item("address").Value(ServiceAddress_);

        auto error = TError("Master disconnected");

        {
            std::vector<TFuture<void>> abortFutures;
            for (auto& nodeShard : NodeShards_) {
                abortFutures.push_back(BIND(&TNodeShard::AbortAllJobs, nodeShard)
                    .AsyncVia(nodeShard->GetInvoker())
                    .Run(error));
            }
            Combine(abortFutures)
                .Get()
                .ThrowOnError();
        }

        auto operations = IdToOperation_;
        for (const auto& pair : operations) {
            auto operation = pair.second;
            LOG_INFO("Forgetting operation (OperationId: %v)", operation->GetId());
            if (!operation->IsFinishedState()) {
                operation->GetController()->Abort();
                SetOperationFinalState(
                    operation,
                    EOperationState::Aborted,
                    error);
            }
            FinishOperation(operation);
        }
        YCHECK(IdToOperation_.empty());

        {
            std::vector<TFuture<void>> nodeShardFutures;
            for (auto& nodeShard : NodeShards_) {
                nodeShardFutures.push_back(BIND(&TNodeShard::OnMasterDisconnected, nodeShard)
                    .AsyncVia(nodeShard->GetInvoker())
                    .Run());
            }
            Combine(nodeShardFutures)
                .Get()
                .ThrowOnError();
        }

        Strategy_->ResetState();

        LOG_INFO("Finished scheduler state cleanup");
    }

    TError GetMasterDisconnectedError()
    {
        return TError(
            NRpc::EErrorCode::Unavailable,
            "Master is not connected");
    }

    void LogOperationFinished(TOperationPtr operation, ELogEventType logEventType, TError error)
    {
        LogEventFluently(logEventType)
            .Item("operation_id").Value(operation->GetId())
            .Item("operation_type").Value(operation->GetType())
            .Item("spec").Value(operation->GetSpec())
            .Item("authenticated_user").Value(operation->GetAuthenticatedUser())
            .Item("start_time").Value(operation->GetStartTime())
            .Item("finish_time").Value(operation->GetFinishTime())
            .Item("controller_time_statistics").Value(operation->ControllerTimeStatistics())
            .Item("error").Value(error);
    }

    void OnUserTransactionAborted(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        TerminateOperation(
            operation,
            EOperationState::Aborting,
            EOperationState::Aborted,
            ELogEventType::OperationAborted,
            TError("Operation transaction has expired or was aborted"));
    }

    void OnSchedulerTransactionAborted(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        TerminateOperation(
            operation,
            EOperationState::Failing,
            EOperationState::Failed,
            ELogEventType::OperationFailed,
            TError("Scheduler transaction has expired or was aborted"));
    }

    void RequestPools(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
    {
        LOG_INFO("Updating pools");

        auto req = TYPathProxy::Get(GetPoolsPath());
        static auto poolConfigTemplate = New<TPoolConfig>();
        static auto poolConfigKeys = poolConfigTemplate->GetRegisteredKeys();
        ToProto(req->mutable_attributes()->mutable_keys(), poolConfigKeys);
        batchReq->AddRequest(req, "get_pools");
    }

    void HandlePools(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_pools");
        if (!rspOrError.IsOK()) {
            LOG_ERROR(rspOrError, "Error getting pools configuration");
            return;
        }

        const auto& rsp = rspOrError.Value();
        INodePtr poolsNode;
        try {
            poolsNode = ConvertToNode(TYsonString(rsp->value()));
        } catch (const std::exception& ex) {
            auto error = TError("Error parsing pools configuration")
                << ex;
            RegisterAlert(EAlertType::UpdatePools, error);
            return;
        }

        Strategy_->UpdatePools(poolsNode);
    }

    void RequestNodesAttributes(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
    {
        LOG_INFO("Updating nodes information");

        auto req = TYPathProxy::List("//sys/nodes");
        std::vector<Stroka> attributeKeys{
            "id",
            "tags",
            "state",
            "io_weight"
        };
        ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
        batchReq->AddRequest(req, "get_nodes");
    }

    void HandleNodesAttributes(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspList>("get_nodes");
        if (!rspOrError.IsOK()) {
            LOG_ERROR(rspOrError, "Error updating nodes information");
            return;
        }

        try {
            const auto& rsp = rspOrError.Value();
            auto nodesList = ConvertToNode(TYsonString(rsp->value()))->AsList();
            std::vector<std::vector<std::pair<Stroka, INodePtr>>> nodesForShard(NodeShards_.size());
            std::vector<TFuture<void>> shardFutures;
            for (const auto& child : nodesList->GetChildren()) {
                auto address = child->GetValue<Stroka>();
                auto objectId = child->Attributes().Get<TObjectId>("id");
                auto nodeId = NodeIdFromObjectId(objectId);
                auto nodeShardId = GetNodeShardId(nodeId);
                nodesForShard[nodeShardId].emplace_back(address, child);
            }

            for (int i = 0 ; i < NodeShards_.size(); ++i) {
                auto& nodeShard = NodeShards_[i];
                shardFutures.push_back(
                    BIND(&TNodeShard::HandleNodesAttributes, nodeShard)
                        .AsyncVia(nodeShard->GetInvoker())
                        .Run(std::move(nodesForShard[i])));
            }
            WaitFor(Combine(shardFutures))
                .ThrowOnError();
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error updating nodes information");
        }

        LOG_INFO("Nodes information updated");
    }

    void RequestOperationRuntimeParams(
        TOperationPtr operation,
        TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
    {
        static auto runtimeParamsTemplate = New<TOperationRuntimeParams>();
        auto req = TYPathProxy::Get(GetOperationPath(operation->GetId()) + "/@");
        ToProto(req->mutable_attributes()->mutable_keys(), runtimeParamsTemplate->GetRegisteredKeys());
        batchReq->AddRequest(req, "get_runtime_params");
    }

    void HandleOperationRuntimeParams(
        TOperationPtr operation,
        TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_runtime_params");
        if (!rspOrError.IsOK()) {
            LOG_ERROR(rspOrError, "Error updating operation runtime parameters");
            return;
        }

        const auto& rsp = rspOrError.Value();
        auto attributesNode = ConvertToNode(TYsonString(rsp->value()));

        Strategy_->UpdateOperationRuntimeParams(operation, attributesNode);
    }

    void RequestConfig(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
    {
        LOG_INFO("Updating scheduler configuration");

        auto req = TYPathProxy::Get("//sys/scheduler/config");
        batchReq->AddRequest(req, "get_config");
    }

    void HandleConfig(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_config");
        if (rspOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
            // No config in Cypress, just ignore.
            UnregisterAlert(EAlertType::UpdateConfig);
            return;
        }
        if (!rspOrError.IsOK()) {
            LOG_ERROR(rspOrError, "Error getting scheduler configuration");
            return;
        }

        auto oldConfig = ConvertToNode(Config_);

        bool errorFound = false;
        try {
            const auto& rsp = rspOrError.Value();
            auto configFromCypress = ConvertToNode(TYsonString(rsp->value()));

            auto mergedConfig = UpdateNode(InitialConfig_, configFromCypress);
            try {
                Config_->Load(mergedConfig, /* validate */ true, /* setDefaults */ true);
            } catch (const std::exception& ex) {
                errorFound = true;
                auto error = TError("Error updating cell scheduler configuration")
                    << ex;
                RegisterAlert(EAlertType::UpdateConfig, error);
                Config_->Load(oldConfig, /* validate */ true, /* setDefaults */ true);
            }
        } catch (const std::exception& ex) {
            errorFound = true;
            auto error = TError("Error parsing updated scheduler configuration")
                << ex;
            RegisterAlert(EAlertType::UpdateConfig, error);
        }

        if (!errorFound) {
            UnregisterAlert(EAlertType::UpdateConfig);
        }

        auto newConfig = ConvertToNode(Config_);

        if (!AreNodesEqual(oldConfig, newConfig)) {
            LOG_INFO("Scheduler configuration updated");
            auto config = CloneYsonSerializable(Config_);
            for (const auto& operation : GetOperations()) {
                auto controller = operation->GetController();
                BIND(&IOperationController::UpdateConfig, controller, config)
                    .AsyncVia(controller->GetCancelableInvoker())
                    .Run();
            }

            for (auto& nodeShard : NodeShards_) {
                BIND(&TNodeShard::UpdateConfig, nodeShard, config)
                    .AsyncVia(nodeShard->GetInvoker())
                    .Run();
            }
        }
    }

    void UpdateExecNodeDescriptors()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        std::vector<TFuture<std::vector<TExecNodeDescriptor>>> shardDescriptorsFutures;
        for (auto& nodeShard : NodeShards_) {
            shardDescriptorsFutures.push_back(BIND(&TNodeShard::GetExecNodeDescriptors, nodeShard)
                .AsyncVia(nodeShard->GetInvoker())
                .Run());
        }

        auto shardDescriptors = WaitFor(Combine(shardDescriptorsFutures))
            .ValueOrThrow();

        std::vector<TExecNodeDescriptor> result;
        for (const auto& descriptors : shardDescriptors) {
            result.insert(result.end(), descriptors.begin(), descriptors.end());
        }

        {
            TWriterGuard guard(ExecNodeDescriptorsLock_);

            CachedExecNodeDescriptors_ = result;
        }
    }

    void DoStartOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        if (operation->GetState() != EOperationState::Initializing) {
            throw TFiberCanceledException();
        }

        bool registered = false;
        try {
            auto controller = CreateController(operation.Get());
            operation->SetController(controller);

            RegisterOperation(operation);
            registered = true;

            controller->Initialize();

            WaitFor(MasterConnector_->CreateOperationNode(operation))
                .ThrowOnError();

            if (operation->GetState() != EOperationState::Initializing) {
                throw TFiberCanceledException();
            }
        } catch (const std::exception& ex) {
            auto wrappedError = TError("Operation has failed to initialize")
                << ex;
            if (registered) {
                OnOperationFailed(operation, wrappedError);
            } else {
                operation->SetStarted(wrappedError);
            }
            THROW_ERROR(wrappedError);
        }

        LogEventFluently(ELogEventType::OperationStarted)
            .Item("operation_id").Value(operation->GetId())
            .Item("operation_type").Value(operation->GetType())
            .Item("spec").Value(operation->GetSpec());

        // NB: Once we've registered the operation in Cypress we're free to complete
        // StartOperation request. Preparation will happen in a separate fiber in a non-blocking
        // fashion.
        auto controller = operation->GetController();
        BIND(&TImpl::DoPrepareOperation, MakeStrong(this), operation)
            .AsyncVia(controller->GetCancelableControlInvoker())
            .Run();

        operation->SetStarted(TError());
    }

    void DoPrepareOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        if (operation->GetState() != EOperationState::Initializing) {
            throw TFiberCanceledException();
        }

        const auto& operationId = operation->GetId();

        try {
            // Run async preparation.
            LOG_INFO("Preparing operation (OperationId: %v)",
                operationId);

            operation->SetState(EOperationState::Preparing);

            auto controller = operation->GetController();
            auto asyncResult = BIND(&IOperationController::Prepare, controller)
                .AsyncVia(controller->GetCancelableInvoker())
                .Run();

            TScopedTimer timer;
            auto result = WaitFor(asyncResult);
            auto prepareDuration = timer.GetElapsed();
            operation->UpdateControllerTimeStatistics("/prepare", prepareDuration);

            THROW_ERROR_EXCEPTION_IF_FAILED(result);

            if (operation->GetState() != EOperationState::Preparing) {
                throw TFiberCanceledException();
            }
            operation->SetState(EOperationState::Pending);
            operation->SetPrepared(true);
            if (operation->GetActivated()) {
                MaterializeOperation(operation);
            }
        } catch (const std::exception& ex) {
            auto wrappedError = TError("Operation has failed to prepare")
                << ex;
            OnOperationFailed(operation, wrappedError);
            return;
        }

        LOG_INFO("Operation has been prepared (OperationId: %v)",
            operationId);

        LogEventFluently(ELogEventType::OperationPrepared)
            .Item("operation_id").Value(operationId);

        LogOperationProgress(operation);

        // From this moment on the controller is fully responsible for the
        // operation's fate. It will eventually call #OnOperationCompleted or
        // #OnOperationFailed to inform the scheduler about the outcome.
    }

    void ReviveOperation(const TOperationPtr& operation, const TControllerTransactionsPtr& controllerTransactions)
    {
        auto codicilGuard = operation->MakeCodicilGuard();

        operation->SetState(EOperationState::Reviving);

        const auto& operationId = operation->GetId();

        LOG_INFO("Reviving operation (OperationId: %v)",
            operationId);

        if (operation->GetMutationId()) {
            TRspStartOperation response;
            ToProto(response.mutable_operation_id(), operationId);
            auto responseMessage = CreateResponseMessage(response);
            auto responseKeeper = Bootstrap_->GetResponseKeeper();
            responseKeeper->EndRequest(operation->GetMutationId(), responseMessage);
        }

        // NB: The operation is being revived, hence it already
        // has a valid node associated with it.
        // If the revival fails, we still need to update the node
        // and unregister the operation from Master Connector.

        try {
            auto controller = CreateController(operation.Get());
            operation->SetController(controller);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Operation has failed to revive (OperationId: %v)",
                operationId);
            auto wrappedError = TError("Operation has failed to revive") << ex;
            SetOperationFinalState(operation, EOperationState::Failed, wrappedError);
            MasterConnector_->FlushOperationNode(operation);
            return;
        }

        RegisterOperation(operation);

        auto controller = operation->GetController();
        BIND(&TImpl::DoReviveOperation, MakeStrong(this), operation, controllerTransactions)
            .Via(controller->GetCancelableControlInvoker())
            .Run();
    }

    void DoReviveOperation(TOperationPtr operation, TControllerTransactionsPtr controllerTransactions)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        if (operation->GetState() != EOperationState::Reviving) {
            throw TFiberCanceledException();
        }

        try {
            auto controller = operation->GetController();

            controller->InitializeReviving(controllerTransactions);

            {
                auto error = WaitFor(MasterConnector_->ResetRevivingOperationNode(operation));
                THROW_ERROR_EXCEPTION_IF_FAILED(error);
            }

            {
                auto asyncResult = VoidFuture;
                asyncResult = BIND(&IOperationController::Revive, controller)
                    .AsyncVia(controller->GetCancelableInvoker())
                    .Run();
                auto error = WaitFor(asyncResult);
                THROW_ERROR_EXCEPTION_IF_FAILED(error);
            }

            if (operation->GetState() != EOperationState::Reviving) {
                throw TFiberCanceledException();
            }

            operation->SetState(EOperationState::Pending);
            operation->SetPrepared(true);
            if (operation->GetActivated()) {
                MaterializeOperation(operation);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Operation has failed to revive (OperationId: %v)",
                operation->GetId());
            auto wrappedError = TError("Operation has failed to revive") << ex;
            OnOperationFailed(operation, wrappedError);
            return;
        }

        LOG_INFO("Operation has been revived and is now running (OperationId: %v)",
            operation->GetId());
    }

    void RegisterOperation(TOperationPtr operation)
    {
        YCHECK(IdToOperation_.insert(std::make_pair(operation->GetId(), operation)).second);
        for (auto& nodeShard : NodeShards_) {
            BIND(&TNodeShard::RegisterOperation, nodeShard)
                .AsyncVia(nodeShard->GetInvoker())
                .Run(operation->GetId(), operation->GetController());
        }

        Strategy_->RegisterOperation(operation);

        GetMasterConnector()->AddOperationWatcherRequester(
            operation,
            BIND(&TImpl::RequestOperationRuntimeParams, Unretained(this), operation));
        GetMasterConnector()->AddOperationWatcherHandler(
            operation,
            BIND(&TImpl::HandleOperationRuntimeParams, Unretained(this), operation));

        LOG_DEBUG("Operation registered (OperationId: %v)",
            operation->GetId());
    }

    void AbortOperationJobs(TOperationPtr operation, const TError& error)
    {
        std::vector<TFuture<void>> abortFutures;
        for (auto& nodeShard : NodeShards_) {
            abortFutures.push_back(BIND(&TNodeShard::AbortOperationJobs, nodeShard)
                .AsyncVia(nodeShard->GetInvoker())
                .Run(operation->GetId(), error));
        }
        WaitFor(Combine(abortFutures))
            .ThrowOnError();
    }

    void UnregisterOperation(TOperationPtr operation)
    {
        YCHECK(IdToOperation_.erase(operation->GetId()) == 1);
        for (auto& nodeShard : NodeShards_) {
            BIND(&TNodeShard::UnregisterOperation, nodeShard, operation->GetId())
                .AsyncVia(nodeShard->GetInvoker())
                .Run();
        }

        Strategy_->UnregisterOperation(operation);

        LOG_DEBUG("Operation unregistered (OperationId: %v)",
            operation->GetId());
    }

    void LogOperationProgress(TOperationPtr operation)
    {
        if (operation->GetState() != EOperationState::Running)
            return;

        auto controller = operation->GetController();
        auto controllerLoggingProgress = WaitFor(
            BIND(&IOperationController::GetLoggingProgress, controller)
                .AsyncVia(controller->GetInvoker())
                .Run())
            .ValueOrThrow();

        if (!FindOperation(operation->GetId())) {
            return;
        }

        LOG_DEBUG("Progress: %v, %v (OperationId: %v)",
            controllerLoggingProgress,
            Strategy_->GetOperationLoggingProgress(operation->GetId()),
            operation->GetId());
    }

    void SetOperationFinalState(TOperationPtr operation, EOperationState state, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!operation->GetStarted().IsSet()) {
            operation->SetStarted(error);
        }
        operation->SetState(state);
        operation->SetFinishTime(TInstant::Now());
        ToProto(operation->Result().mutable_error(), error);
    }

    void FinishOperation(TOperationPtr operation)
    {
        if (!operation->GetFinished().IsSet()) {
            operation->SetFinished();
            operation->SetController(nullptr);
            operation->UpdateControllerTimeStatistics(
                Strategy_->GetOperationTimeStatistics(operation->GetId()));
            UnregisterOperation(operation);
        }
    }

    void InitStrategy()
    {
        Strategy_ = CreateFairShareStrategy(Config_, this);
    }

    IOperationControllerPtr CreateController(TOperation* operation)
    {
        auto config = CloneYsonSerializable(Config_);

        IOperationControllerPtr controller;
        switch (operation->GetType()) {
            case EOperationType::Map:
                controller = CreateMapController(config, this, operation);
                break;
            case EOperationType::Merge:
                controller = CreateMergeController(config, this, operation);
                break;
            case EOperationType::Erase:
                controller = CreateEraseController(config, this, operation);
                break;
            case EOperationType::Sort:
                controller = CreateSortController(config, this, operation);
                break;
            case EOperationType::Reduce:
                controller = CreateReduceController(config, this, operation);
                break;
            case EOperationType::JoinReduce:
                controller = CreateJoinReduceController(config, this, operation);
                break;
            case EOperationType::MapReduce:
                controller = CreateMapReduceController(config, this, operation);
                break;
            case EOperationType::RemoteCopy:
                controller = CreateRemoteCopyController(config, this, operation);
                break;
            default:
                Y_UNREACHABLE();
        }

        return CreateControllerWrapper(
            operation->GetId(),
            controller,
            ControllerThreadPool_->GetInvoker());
    }

    INodePtr GetSpecTemplate(EOperationType type, IMapNodePtr spec)
    {
        switch (type) {
            case EOperationType::Map:
                return Config_->MapOperationOptions->SpecTemplate;
            case EOperationType::Merge: {
                auto mergeSpec = ParseOperationSpec<TMergeOperationSpec>(spec);
                switch (mergeSpec->Mode) {
                    case EMergeMode::Unordered:
                        return Config_->UnorderedMergeOperationOptions->SpecTemplate;
                    case EMergeMode::Ordered:
                        return Config_->OrderedMergeOperationOptions->SpecTemplate;
                    case EMergeMode::Sorted:
                        return Config_->SortedMergeOperationOptions->SpecTemplate;
                    default:
                        Y_UNREACHABLE();
                }
            }
            case EOperationType::Erase:
                return Config_->EraseOperationOptions->SpecTemplate;
            case EOperationType::Sort:
                return Config_->SortOperationOptions->SpecTemplate;
            case EOperationType::Reduce:
                return Config_->ReduceOperationOptions->SpecTemplate;
            case EOperationType::JoinReduce:
                return Config_->JoinReduceOperationOptions->SpecTemplate;
            case EOperationType::MapReduce:
                return Config_->MapReduceOperationOptions->SpecTemplate;
            case EOperationType::RemoteCopy:
                return Config_->RemoteCopyOperationOptions->SpecTemplate;
            default:
                Y_UNREACHABLE();
        }
    }

    void DoCompleteOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        if (operation->IsFinishedState() || operation->IsFinishingState()) {
            // Operation is probably being aborted.
            return;
        }

        const auto& operationId = operation->GetId();

        LOG_INFO("Completing operation (OperationId: %v)",
            operationId);

        operation->SetState(EOperationState::Completing);

        // The operation may still have running jobs (e.g. those started speculatively).
        AbortOperationJobs(operation, TError("Operation completed"));

        try {
            // First flush: ensure that all stderrs are attached and the
            // state is changed to Completing.
            {
                auto asyncResult = MasterConnector_->FlushOperationNode(operation);
                WaitFor(asyncResult);
                if (operation->GetState() != EOperationState::Completing) {
                    throw TFiberCanceledException();
                }
            }

            {
                auto controller = operation->GetController();
                auto asyncResult = BIND(&IOperationController::Commit, controller)
                    .AsyncVia(controller->GetCancelableInvoker())
                    .Run();
                WaitFor(asyncResult)
                    .ThrowOnError();

                if (operation->GetState() != EOperationState::Completing) {
                    throw TFiberCanceledException();
                }
            }

            if (Config_->FinishOperationTransitionDelay) {
                Sleep(*Config_->FinishOperationTransitionDelay);
            }

            YCHECK(operation->GetState() == EOperationState::Completing);
            SetOperationFinalState(operation, EOperationState::Completed, TError());

            // Second flush: ensure that state is changed to Completed.
            {
                auto asyncResult = MasterConnector_->FlushOperationNode(operation);
                WaitFor(asyncResult);
                YCHECK(operation->GetState() == EOperationState::Completed);
            }

            FinishOperation(operation);
        } catch (const std::exception& ex) {
            OnOperationFailed(operation, ex);
            return;
        }

        LOG_INFO("Operation completed (OperationId: %v)",
             operationId);

        LogOperationFinished(operation, ELogEventType::OperationCompleted, TError());
    }

    void DoFailOperation(TOperationPtr operation, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        LOG_INFO(error, "Operation failed (OperationId: %v)",
             operation->GetId());

        TerminateOperation(
            operation,
            EOperationState::Failing,
            EOperationState::Failed,
            ELogEventType::OperationFailed,
            error);
    }

    void TerminateOperation(
        TOperationPtr operation,
        EOperationState intermediateState,
        EOperationState finalState,
        ELogEventType logEventType,
        const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto state = operation->GetState();
        if (IsOperationFinished(state) ||
            state == EOperationState::Failing ||
            state == EOperationState::Aborting)
        {
            // Safe to call multiple times, just ignore it.
            return;
        }

        operation->SetState(intermediateState);

        AbortOperationJobs(
            operation,
            TError("Operation terminated")
                << TErrorAttribute("state", state)
                << error);

        // First flush: ensure that all stderrs are attached and the
        // state is changed to its intermediate value.
        {
            auto asyncResult = MasterConnector_->FlushOperationNode(operation);
            WaitFor(asyncResult);
            if (operation->GetState() != intermediateState)
                return;
        }

        if (Config_->FinishOperationTransitionDelay) {
            Sleep(*Config_->FinishOperationTransitionDelay);
        }

        {
            auto controller = operation->GetController();
            if (controller) {
                controller->Abort();
            }
        }

        SetOperationFinalState(operation, finalState, error);

        // Second flush: ensure that the state is changed to its final value.
        {
            auto asyncResult = MasterConnector_->FlushOperationNode(operation);
            WaitFor(asyncResult);
            if (operation->GetState() != finalState)
                return;
        }

        FinishOperation(operation);

        LogOperationFinished(operation, logEventType, error);
    }

    void AbortAbortingOperation(TOperationPtr operation, TControllerTransactionsPtr controllerTransactions)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        LOG_INFO("Aborting operation (OperationId: %v)",
             operation->GetId());

        auto abortTransaction = [&] (ITransactionPtr transaction) {
            if (transaction) {
                // Fire-and-forget.
                transaction->Abort();
            }
        };

        abortTransaction(controllerTransactions->Sync);
        abortTransaction(controllerTransactions->Async);
        abortTransaction(controllerTransactions->Input);
        abortTransaction(controllerTransactions->Output);

        SetOperationFinalState(operation, EOperationState::Aborted, TError());

        WaitFor(MasterConnector_->FlushOperationNode(operation));

        LogOperationFinished(operation, ELogEventType::OperationCompleted, TError());
    }

    void BuildStaticOrchid(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("connected").Value(MasterConnector_->IsConnected())
                .Item("cell").BeginMap()
                    .Item("resource_limits").Value(GetTotalResourceLimits())
                    .Item("resource_usage").Value(GetTotalResourceUsage())
                    .Item("exec_node_count").Value(GetExecNodeCount())
                    .Item("total_node_count").Value(GetTotalNodeCount())
                .EndMap()
                .Item("suspicious_jobs").BeginMap()
                    .Do(BIND([=] (IYsonConsumer* consumer) {
                        for (auto nodeShard : NodeShards_) {
                            WaitFor(
                                BIND(&TNodeShard::BuildSuspiciousJobsYson, nodeShard, consumer)
                                    .AsyncVia(nodeShard->GetInvoker())
                                    .Run());
                        }
                    }))
                .EndMap()
                .Item("nodes").BeginMap()
                    .Do(BIND([=] (IYsonConsumer* consumer) {
                        for (auto nodeShard : NodeShards_) {
                            WaitFor(
                                BIND(&TNodeShard::BuildNodesYson, nodeShard, consumer)
                                    .AsyncVia(nodeShard->GetInvoker())
                                    .Run());
                        }
                    }))
                .EndMap()
                .Item("clusters").DoMapFor(GetClusterDirectory()->GetClusterNames(), [=] (TFluentMap fluent, const Stroka& clusterName) {
                    BuildClusterYson(clusterName, fluent);
                })
                .Item("config").Value(Config_)
                .DoIf(Strategy_.operator bool(), BIND(&ISchedulerStrategy::BuildOrchid, Strategy_))
            .EndMap();
    }

    void BuildClusterYson(const Stroka& clusterName, IYsonConsumer* consumer)
    {
        BuildYsonMapFluently(consumer)
            .Item(clusterName)
            .Value(GetClusterDirectory()->GetConnection(clusterName)->GetConfig());
    }

    void BuildOperationYson(TOperationPtr operation, IYsonConsumer* consumer) const
    {
        auto codicilGuard = operation->MakeCodicilGuard();

        auto controller = operation->GetController();

        bool hasControllerProgress = operation->HasControllerProgress();
        BuildYsonFluently(consumer)
            .BeginMap()
                // Include the complete list of attributes.
                .Do(BIND(&NScheduler::BuildInitializingOperationAttributes, operation))
                .DoIf(static_cast<bool>(controller), BIND(&IOperationController::BuildOperationAttributes, controller))
                .Item("progress").BeginMap()
                    .DoIf(hasControllerProgress, BIND([=] (IYsonConsumer* consumer) {
                        WaitFor(
                            BIND(&IOperationController::BuildProgress, controller)
                                .AsyncVia(controller->GetInvoker())
                                .Run(consumer));
                    }))
                    .Do(BIND(&ISchedulerStrategy::BuildOperationProgress, Strategy_, operation->GetId()))
                .EndMap()
                .Item("brief_progress").BeginMap()
                    .DoIf(hasControllerProgress, BIND([=] (IYsonConsumer* consumer) {
                        WaitFor(
                            BIND(&IOperationController::BuildBriefProgress, controller)
                                .AsyncVia(controller->GetInvoker())
                                .Run(consumer));
                    }))
                    .Do(BIND(&ISchedulerStrategy::BuildBriefOperationProgress, Strategy_, operation->GetId()))
                .EndMap()
                .Item("running_jobs").BeginAttributes()
                    .Item("opaque").Value("true")
                .EndAttributes()
                .BeginMap()
                .Do(BIND([=] (IYsonConsumer* consumer) {
                    for (auto& nodeShard : NodeShards_) {
                        WaitFor(
                            BIND(&TNodeShard::BuildOperationJobsYson, nodeShard)
                                .AsyncVia(nodeShard->GetInvoker())
                                .Run(operation->GetId(), consumer));
                    }
                    }))
                .EndMap()
                .Do(BIND([=] (IYsonConsumer* consumer) {
                    WaitFor(
                        BIND(&IOperationController::BuildMemoryDigestStatistics, controller)
                            .AsyncVia(controller->GetInvoker())
                            .Run(consumer));
                    }))
            .EndMap();
    }

    IYPathServicePtr GetDynamicOrchidService()
    {
        auto dynamicOrchidService = New<TCompositeMapService>();
        dynamicOrchidService->AddChild("operations", New<TOperationsService>(this));
        return dynamicOrchidService;
    }

    class TOperationsService
        : public TVirtualMapBase
    {
    public:
        TOperationsService(const TScheduler::TImpl* scheduler)
            : TVirtualMapBase(nullptr /* owningNode */)
            , Scheduler_(scheduler)
        { }

        virtual i64 GetSize() const override
        {
            return Scheduler_->IdToOperation_.size();
        }

        virtual std::vector<Stroka> GetKeys(i64 limit) const override
        {
            std::vector<Stroka> keys;
            keys.reserve(limit);
            for (const auto& pair : Scheduler_->IdToOperation_) {
                if (static_cast<i64>(keys.size()) >= limit) {
                    break;
                }
                keys.emplace_back(ToString(pair.first));
            }
            return keys;
        }

        virtual IYPathServicePtr FindItemService(const TStringBuf& key) const override
        {
            TOperationId operationId = TOperationId::FromString(key);
            auto iterator = Scheduler_->IdToOperation_.find(operationId);
            if (iterator == Scheduler_->IdToOperation_.end()) {
                return nullptr;
            }

            return IYPathService::FromProducer(
                BIND(&TScheduler::TImpl::BuildOperationYson, MakeStrong(Scheduler_), iterator->second));
        }

    private:
        const TScheduler::TImpl* const Scheduler_;
    };
};

////////////////////////////////////////////////////////////////////

TScheduler::TScheduler(
    TSchedulerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TScheduler::~TScheduler() = default;

void TScheduler::Initialize()
{
    Impl_->Initialize();
}

ISchedulerStrategyPtr TScheduler::GetStrategy()
{
    return Impl_->GetStrategy();
}

IYPathServicePtr TScheduler::GetOrchidService()
{
    return Impl_->GetOrchidService();
}

std::vector<TOperationPtr> TScheduler::GetOperations()
{
    return Impl_->GetOperations();
}

IInvokerPtr TScheduler::GetSnapshotIOInvoker()
{
    return Impl_->GetSnapshotIOInvoker();
}

bool TScheduler::IsConnected()
{
    return Impl_->IsConnected();
}

void TScheduler::ValidateConnected()
{
    Impl_->ValidateConnected();
}

TOperationPtr TScheduler::FindOperation(const TOperationId& id)
{
    return Impl_->FindOperation(id);
}

TOperationPtr TScheduler::GetOperationOrThrow(const TOperationId& id)
{
    return Impl_->GetOperationOrThrow(id);
}

TFuture<TOperationPtr> TScheduler::StartOperation(
    EOperationType type,
    const TTransactionId& transactionId,
    const TMutationId& mutationId,
    IMapNodePtr spec,
    const Stroka& user)
{
    return Impl_->StartOperation(
        type,
        transactionId,
        mutationId,
        spec,
        user);
}

TFuture<void> TScheduler::AbortOperation(
    TOperationPtr operation,
    const TError& error,
    const Stroka& user)
{
    return Impl_->AbortOperation(operation, error, user);
}

TFuture<void> TScheduler::SuspendOperation(
    TOperationPtr operation,
    const Stroka& user,
    bool abortRunningJobs)
{
    return Impl_->SuspendOperation(operation, user, abortRunningJobs);
}

TFuture<void> TScheduler::ResumeOperation(
    TOperationPtr operation,
    const Stroka& user)
{
    return Impl_->ResumeOperation(operation, user);
}

TFuture<void> TScheduler::CompleteOperation(
    TOperationPtr operation,
    const TError& error,
    const Stroka& user)
{
    return Impl_->CompleteOperation(operation, error, user);
}

TFuture<void> TScheduler::DumpInputContext(const TJobId& jobId, const NYPath::TYPath& path, const Stroka& user)
{
    return Impl_->DumpInputContext(jobId, path, user);
}

TFuture<TYsonString> TScheduler::Strace(const TJobId& jobId, const Stroka& user)
{
    return Impl_->Strace(jobId, user);
}

TFuture<void> TScheduler::SignalJob(const TJobId& jobId, const Stroka& signalName, const Stroka& user)
{
    return Impl_->SignalJob(jobId, signalName, user);
}

TFuture<void> TScheduler::AbandonJob(const TJobId& jobId, const Stroka& user)
{
    return Impl_->AbandonJob(jobId, user);
}

TFuture<TYsonString> TScheduler::PollJobShell(const TJobId& jobId, const TYsonString& parameters, const Stroka& user)
{
    return Impl_->PollJobShell(jobId, parameters, user);
}

TFuture<void> TScheduler::AbortJob(const TJobId& jobId, const Stroka& user)
{
    return Impl_->AbortJob(jobId, user);
}

void TScheduler::ProcessHeartbeat(TCtxHeartbeatPtr context)
{
    Impl_->ProcessHeartbeat(context);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
