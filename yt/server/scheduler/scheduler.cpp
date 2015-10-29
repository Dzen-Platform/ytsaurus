#include "stdafx.h"
#include "scheduler.h"
#include "scheduler_strategy.h"
#include "fair_share_strategy.h"
#include "operation_controller.h"
#include "map_controller.h"
#include "merge_controller.h"
#include "remote_copy_controller.h"
#include "sort_controller.h"
#include "helpers.h"
#include "master_connector.h"
#include "job_resources.h"
#include "private.h"
#include "snapshot_downloader.h"
#include "event_log.h"

#include <core/concurrency/thread_affinity.h>
#include <core/concurrency/periodic_executor.h>

#include <core/misc/finally.h>

#include <core/profiling/scoped_timer.h>

#include <core/rpc/message.h>
#include <core/rpc/response_keeper.h>

#include <ytlib/job_prober_client/job_prober_service_proxy.h>

#include <ytlib/object_client/master_ypath_proxy.h>
#include <ytlib/object_client/helpers.h>

#include <ytlib/chunk_client/private.h>

#include <ytlib/scheduler/helpers.h>

#include <ytlib/table_client/name_table.h>
#include <ytlib/table_client/schemaless_writer.h>
#include <ytlib/table_client/schemaless_buffered_table_writer.h>
#include <ytlib/table_client/table_consumer.h>

#include <server/cell_scheduler/config.h>
#include <server/cell_scheduler/bootstrap.h>

namespace NYT {
namespace NScheduler {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NRpc;
using namespace NTransactionClient;
using namespace NCypressClient;
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
        , MasterConnector_(new TMasterConnector(Config_, Bootstrap_))
        , TotalResourceLimitsProfiler_(Profiler.GetPathPrefix() + "/total_resource_limits")
        , TotalResourceUsageProfiler_(Profiler.GetPathPrefix() + "/total_resource_usage")
        , TotalCompletedJobTimeCounter_("/total_completed_job_time")
        , TotalFailedJobTimeCounter_("/total_failed_job_time")
        , TotalAbortedJobTimeCounter_("/total_aborted_job_time")
        , TotalResourceLimits_(ZeroNodeResources())
        , TotalResourceUsage_(ZeroNodeResources())
    {
        YCHECK(config);
        YCHECK(bootstrap);
        VERIFY_INVOKER_THREAD_AFFINITY(GetControlInvoker(), ControlThread);
        VERIFY_INVOKER_THREAD_AFFINITY(GetSnapshotIOInvoker(), SnapshotIOThread);

        auto localHostName = TAddressResolver::Get()->GetLocalHostName();
        int port = Bootstrap_->GetConfig()->RpcPort;
        ServiceAddress_ = BuildServiceAddress(localHostName, port);
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
        EventLogWriter_ = CreateSchemalessBufferedTableWriter(
            Config_->EventLog,
            New<TRemoteWriterOptions>(),
            Bootstrap_->GetMasterClient(),
            nameTable,
            Config_->EventLog->Path);

        // Open is always synchronous for buffered writer.
        YCHECK(EventLogWriter_->Open().IsSet());

        auto valueConsumer = New<TWritingValueConsumer>(EventLogWriter_, true);
        EventLogConsumer_.reset(new TTableConsumer(valueConsumer));

        LogEventFluently(ELogEventType::SchedulerStarted)
            .Item("address").Value(ServiceAddress_);

        LoggingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TImpl::OnLogging, MakeWeak(this)),
            Config_->ClusterInfoLoggingPeriod);
        LoggingExecutor_->Start();
    }

    ISchedulerStrategy* GetStrategy()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Strategy_.get();
    }

    IYPathServicePtr GetOrchidService()
    {
        auto producer = BIND(&TImpl::BuildOrchid, MakeStrong(this));
        return IYPathService::FromProducer(producer);
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

        return GetExecNodes().size();
    }

    virtual std::vector<TExecNodePtr> GetExecNodes() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TReaderGuard guard(AddressToNodeLock_);

        std::vector<TExecNodePtr> result;
        for (const auto& pair : AddressToNode_) {
            const auto& node = pair.second;
            if (node->GetMasterState() == ENodeState::Online) {
                result.push_back(node);
            }
        }
        return result;
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
            THROW_ERROR_EXCEPTION("Limit for the number of concurrent operations %v has been reached",
                Config_->MaxOperationCount);
        }

        // Attach user transaction if any. Don't ping it.
        auto transactionManager = GetMasterClient()->GetTransactionManager();
        TTransactionAttachOptions userAttachOptions;
        userAttachOptions.Ping = false;
        userAttachOptions.PingAncestors = false;
        auto userTransaction = transactionId
            ? transactionManager->Attach(transactionId, userAttachOptions)
            : nullptr;

        // Merge operation spec with template
        auto specTemplate = GetSpecTemplate(type, spec);
        if (specTemplate) {
            spec = NYTree::UpdateNode(specTemplate, spec)->AsMap();
        }

        // Create operation object.
        auto operationId = MakeRandomId(
            EObjectType::Operation,
            GetMasterClient()->GetConnection()->GetPrimaryMasterCellTag());
        auto operation = New<TOperation>(
            operationId,
            type,
            mutationId,
            userTransaction,
            spec,
            user,
            TInstant::Now());
        operation->SetCleanStart(true);
        operation->SetState(EOperationState::Initializing);

        LOG_INFO("Starting operation (OperationType: %v, OperationId: %v, TransactionId: %v, User: %v)",
            type,
            operationId,
            transactionId,
            user);

        LOG_INFO("Total resource limits (OperationId: %v, ResourceLimits: {%v})",
            operationId,
            FormatResources(GetTotalResourceLimits()));

        // Spawn a new fiber where all startup logic will work asynchronously.
        BIND(&TImpl::DoStartOperation, MakeStrong(this), operation)
            .AsyncVia(MasterConnector_->GetCancelableControlInvoker())
            .Run();

        return operation->GetStarted();
    }

    TFuture<void> AbortOperation(TOperationPtr operation, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (operation->IsFinishingState()) {
            LOG_INFO(error, "Operation is already finishing (OperationId: %v, State: %v)",
                operation->GetId(),
                operation->GetState());
            return operation->GetFinished();
        }

        if (operation->IsFinishedState()) {
            LOG_INFO(error, "Operation is already finished (OperationId: %v, State: %v)",
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

    TFuture<void> SuspendOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (operation->IsFinishingState() || operation->IsFinishedState()) {
            return MakeFuture(TError(
                EErrorCode::InvalidOperationState,
                "Cannot suspend operation in %Qlv state",
                operation->GetState()));
        }

        operation->SetSuspended(true);

        LOG_INFO("Operation suspended (OperationId: %v)",
            operation->GetId());

        return MasterConnector_->FlushOperationNode(operation);
    }

    TFuture<void> ResumeOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

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

    TFuture<TYsonString> Strace(const TJobId& jobId)
    {
        return BIND(&TImpl::DoStrace, MakeStrong(this), jobId)
            .AsyncVia(MasterConnector_->GetCancelableControlInvoker())
            .Run();
    }

    TYsonString DoStrace(const TJobId& jobId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto job = GetJobOrThrow(jobId);

        auto proxy = CreateJobProberProxy(job);

        auto req = proxy.Strace();
        ToProto(req->mutable_job_id(), jobId);

        auto rspOrError = WaitFor(req->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error stracing processes of job %v",
            jobId);

        auto& res = rspOrError.Value();

        return TYsonString(FromProto<Stroka>(res->trace()));
    }

    TFuture<void> DumpInputContext(const TJobId& jobId, const TYPath& path)
    {
        return BIND(&TImpl::DoDumpInputContext, MakeStrong(this), jobId, path)
            .AsyncVia(MasterConnector_->GetCancelableControlInvoker())
            .Run();
    }

    void DoDumpInputContext(const TJobId& jobId, const TYPath& path)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto job = GetJobOrThrow(jobId);

        auto proxy = CreateJobProberProxy(job);

        auto req = proxy.DumpInputContext();
        ToProto(req->mutable_job_id(), jobId);

        auto rspOrError = WaitFor(req->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(
            rspOrError,
            "Error saving input context for job %v into %v",
            jobId,
            path);

        const auto& res = rspOrError.Value();
        auto chunkIds = FromProto<TGuid>(res->chunk_id());
        YCHECK(chunkIds.size() == 1);
        MasterConnector_->AttachJobContext(path, chunkIds.front(), job);

        LOG_INFO("Input context saved (JobId: %v, Path: %v)",
            jobId,
            path);
    }

    TJobProberServiceProxy CreateJobProberProxy(TJobPtr job)
    {
        const auto& address = job->GetNode()->GetInterconnectAddress();
        auto channel = NChunkClient::LightNodeChannelFactory->CreateChannel(address);

        TJobProberServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(Bootstrap_->GetConfig()->Scheduler->JobProberRpcTimeout);
        return proxy;
    }

    void ProcessHeartbeatJobs(
        TExecNodePtr node,
        NJobTrackerClient::NProto::TReqHeartbeat* request,
        NJobTrackerClient::NProto::TRspHeartbeat* response,
        std::vector<TJobPtr>* runningJobs,
        bool* hasWaitingJobs,
        yhash_set<TOperationPtr>* operationsToLog)
    {
        auto now = TInstant::Now();

        bool forceJobsLogging = false;
        auto& lastJobsLogTime = node->LastJobsLogTime();
        if (!lastJobsLogTime || now > lastJobsLogTime.Get() + Config_->JobsLoggingPeriod) {
            forceJobsLogging = true;
            lastJobsLogTime = now;
        }

        auto missingJobs = node->Jobs();

        for (auto& jobStatus : *request->mutable_jobs()) {
            auto jobType = EJobType(jobStatus.job_type());
            // Skip jobs that are not issued by the scheduler.
            if (jobType <= EJobType::SchedulerFirst || jobType >= EJobType::SchedulerLast) {
                continue;
            }

            auto job = ProcessJobHeartbeat(
                node,
                request,
                response,
                &jobStatus,
                forceJobsLogging);
            if (job) {
                YCHECK(missingJobs.erase(job) == 1);
                switch (job->GetState()) {
                    case EJobState::Completed:
                    case EJobState::Failed:
                    case EJobState::Aborted:
                        operationsToLog->insert(job->GetOperation());
                        break;
                    case EJobState::Running:
                        runningJobs->push_back(job);
                        break;
                    case EJobState::Waiting:
                        *hasWaitingJobs = true;
                        break;
                    default:
                        break;
                }
            }
        }

        // Check for missing jobs.
        for (auto job : missingJobs) {
            LOG_ERROR("Job is missing (Address: %v, JobId: %v, OperationId: %v)",
                node->GetDefaultAddress(),
                job->GetId(),
                job->GetOperationId());
            AbortJob(job, TError("Job vanished"));
            UnregisterJob(job);
        }
    }

    TFuture<void> ProcessScheduledJobs(
        ISchedulingContext* schedulingContext,
        NJobTrackerClient::NProto::TRspHeartbeat* response,
        yhash_set<TOperationPtr>* operationsToLog)
    {
        std::vector<TFuture<void>> asyncResults;

        auto specBuilderInvoker = NRpc::TDispatcher::Get()->GetInvoker();
        for (auto job : schedulingContext->StartedJobs()) {
            auto operation = FindOperation(job->GetOperationId());
            if (!operation || operation->GetState() != EOperationState::Running) {
                LOG_INFO("Dangling started job found (JobId: %v, OperationId: %v)",
                    job->GetId(),
                    job->GetOperationId());
                continue;
            }

            RegisterJob(job);

            LogEventFluently(ELogEventType::JobStarted)
                .Item("job_id").Value(job->GetId())
                .Item("operation_id").Value(job->GetOperationId())
                .Item("resource_limits").Value(job->ResourceLimits())
                .Item("node_address").Value(job->GetNode()->GetDefaultAddress())
                .Item("job_type").Value(job->GetType());

            auto* startInfo = response->add_jobs_to_start();
            ToProto(startInfo->mutable_job_id(), job->GetId());
            *startInfo->mutable_resource_limits() = job->ResourceUsage();

            // Build spec asynchronously.
            asyncResults.push_back(
                BIND(job->GetSpecBuilder(), startInfo->mutable_spec())
                    .AsyncVia(specBuilderInvoker)
                    .Run());

            // Release to avoid circular references.
            job->SetSpecBuilder(TJobSpecBuilder());
            operationsToLog->insert(operation);
        }

        for (auto job : schedulingContext->PreemptedJobs()) {
            if (!FindOperation(job->GetOperationId())) {
                LOG_INFO("Dangling preempted job found (JobId: %v, OperationId: %v)",
                    job->GetId(),
                    job->GetOperationId());
                continue;
            }
            PreemptJob(job);
            ToProto(response->add_jobs_to_abort(), job->GetId());
        }

        return Combine(asyncResults);
    }

    void ProcessHeartbeat(TCtxHeartbeatPtr context)
    {
        auto* request = &context->Request();
        auto* response = &context->Response();

        auto nodeId = request->node_id();
        auto descriptor = FromProto<TNodeDescriptor>(request->node_descriptor());
        auto node = GetOrRegisterNode(nodeId, descriptor);
        if (node->GetMasterState() != ENodeState::Online) {
            // NB: Resource limits should be updated even if node is offline
            // to avoid getting incorrect total limits when node becomes online.
            // XXX(ignat): Should we consider resource usage here?
            node->ResourceLimits() = context->Request().resource_limits();
            THROW_ERROR_EXCEPTION("Node is not online");
        }

        // We should process only one heartbeat at a time from the same node.
        if (node->GetHasOngoingHeartbeat()) {
            THROW_ERROR_EXCEPTION("Node has ongoing heartbeat");
        }

        yhash_set<TOperationPtr> operationsToLog;
        TFuture<void> scheduleJobsAsyncResult = VoidFuture;

        {
            node->SetHasOngoingHeartbeat(true);
            TFinallyGuard heartbeatGuard([&] {
                node->SetHasOngoingHeartbeat(false);
            });

            TLeaseManager::RenewLease(node->GetLease());

            auto oldResourceLimits = node->ResourceLimits();
            auto oldResourceUsage = node->ResourceUsage();

            node->ResourceLimits() = request->resource_limits();
            node->ResourceUsage() = request->resource_usage();

            // Update total resource limits _before_ processing the heartbeat to
            // maintain exact values of total resource limits.
            TotalResourceLimits_ -= oldResourceLimits;
            TotalResourceLimits_ += node->ResourceLimits();

            for (const auto& tag : node->SchedulingTags()) {
                auto& resources = SchedulingTagResources_[tag];
                resources -= oldResourceLimits;
                resources += node->ResourceLimits();
            }

            // NB: No exception must leave this try/catch block.
            try {
                std::vector<TJobPtr> runningJobs;
                bool hasWaitingJobs = false;
                PROFILE_TIMING ("/analysis_time") {
                    ProcessHeartbeatJobs(
                        node,
                        request,
                        response,
                        &runningJobs,
                        &hasWaitingJobs,
                        &operationsToLog);
                }

                if (hasWaitingJobs) {
                    LOG_INFO("Waiting jobs found, suppressing new jobs scheduling");
                } else {
                    auto schedulingContext = CreateSchedulingContext(
                        Config_,
                        node,
                        runningJobs,
                        Bootstrap_->GetMasterClient()->GetConnection()->GetPrimaryMasterCellTag());

                    PROFILE_TIMING ("/schedule_time") {
                        Strategy_->ScheduleJobs(schedulingContext.get());
                    }

                    scheduleJobsAsyncResult = ProcessScheduledJobs(
                        schedulingContext.get(),
                        response,
                        &operationsToLog);
                }
            } catch (const std::exception& ex) {
                LOG_FATAL(ex, "Failed to process heartbeat");
            }

            // Update total resource usage _after_ processing the heartbeat to avoid
            // "unsaturated CPU" phenomenon.
            TotalResourceUsage_ -= oldResourceUsage;
            TotalResourceUsage_ += node->ResourceUsage();
        }

        context->ReplyFrom(scheduleJobsAsyncResult);

        // NB: Do heavy logging after responding to heartbeat.
        for (auto operation : operationsToLog) {
            if (!FindOperation(operation->GetId())) {
                continue;
            }

            LogOperationProgress(operation);
        }
    }


    // ISchedulerStrategyHost implementation
    DEFINE_SIGNAL(void(TOperationPtr), OperationRegistered);
    DEFINE_SIGNAL(void(TOperationPtr), OperationUnregistered);
    DEFINE_SIGNAL(void(TOperationPtr, INodePtr update), OperationRuntimeParamsUpdated);

    DEFINE_SIGNAL(void(TJobPtr job), JobFinished);
    DEFINE_SIGNAL(void(TJobPtr, const TNodeResources& resourcesDelta), JobUpdated);

    DEFINE_SIGNAL(void(INodePtr pools), PoolsUpdated);


    virtual TMasterConnector* GetMasterConnector() override
    {
        return MasterConnector_.get();
    }

    virtual TNodeResources GetTotalResourceLimits() override
    {
        return TotalResourceLimits_;
    }

    virtual TNodeResources GetResourceLimits(const TNullable<Stroka>& schedulingTag) override
    {
        if (!schedulingTag || SchedulingTagResources_.find(*schedulingTag) == SchedulingTagResources_.end()) {
            return TotalResourceLimits_;
        } else {
            return SchedulingTagResources_[*schedulingTag];
        }
    }


    // IOperationHost implementation
    virtual NApi::IClientPtr GetMasterClient() override
    {
        return Bootstrap_->GetMasterClient();
    }

    virtual NHive::TClusterDirectoryPtr GetClusterDirectory() override
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

        return EventLogConsumer_.get();
    }

    virtual void OnOperationCompleted(TOperationPtr operation) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        GetControlInvoker()->Invoke(BIND(&TImpl::DoCompleteOperation, MakeStrong(this), operation));
    }

    virtual void OnOperationFailed(TOperationPtr operation, const TError& error) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        GetControlInvoker()->Invoke(BIND(&TImpl::DoFailOperation, MakeStrong(this), operation, error));
    }

private:
    const TSchedulerConfigPtr Config_;
    const INodePtr InitialConfig_;
    TBootstrap* const Bootstrap_;

    TActionQueuePtr SnapshotIOQueue_;
    TThreadPoolPtr ControllerThreadPool_;

    std::unique_ptr<TMasterConnector> MasterConnector_;

    std::unique_ptr<ISchedulerStrategy> Strategy_;

    NConcurrency::TReaderWriterSpinLock AddressToNodeLock_;
    typedef yhash_map<Stroka, TExecNodePtr> TExecNodeMap;
    TExecNodeMap AddressToNode_;

    TNodeDirectoryPtr NodeDirectory_ = New<TNodeDirectory>();

    typedef yhash_map<TOperationId, TOperationPtr> TOperationIdMap;
    TOperationIdMap IdToOperation_;

    typedef yhash_map<TJobId, TJobPtr> TJobMap;
    TJobMap IdToJob_;

    NProfiling::TProfiler TotalResourceLimitsProfiler_;
    NProfiling::TProfiler TotalResourceUsageProfiler_;

    NProfiling::TAggregateCounter TotalCompletedJobTimeCounter_;
    NProfiling::TAggregateCounter TotalFailedJobTimeCounter_;
    NProfiling::TAggregateCounter TotalAbortedJobTimeCounter_;

    TEnumIndexedVector<int, EJobType> JobTypeCounters_;
    TPeriodicExecutorPtr ProfilingExecutor_;

    TNodeResources TotalResourceLimits_;
    TNodeResources TotalResourceUsage_;

    TPeriodicExecutorPtr LoggingExecutor_;

    Stroka ServiceAddress_;

    ISchemalessWriterPtr EventLogWriter_;
    std::unique_ptr<IYsonConsumer> EventLogConsumer_;

    yhash_map<Stroka, TNodeResources> SchedulingTagResources_;


    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(SnapshotIOThread);


    void OnProfiling()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        for (auto jobType : TEnumTraits<EJobType>::GetDomainValues()) {
            if (jobType > EJobType::SchedulerFirst && jobType < EJobType::SchedulerLast) {
                Profiler.Enqueue("/job_count/" + FormatEnum(jobType), JobTypeCounters_[jobType]);
            }
        }

        Profiler.Enqueue("/job_count/total", IdToJob_.size());
        Profiler.Enqueue("/operation_count", IdToOperation_.size());
        Profiler.Enqueue("/node_count", GetExecNodeCount());

        ProfileResources(TotalResourceLimitsProfiler_, TotalResourceLimits_);
        ProfileResources(TotalResourceUsageProfiler_, TotalResourceUsage_);
    }


    void OnLogging()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (IsConnected()) {
            LogEventFluently(ELogEventType::ClusterInfo)
                .Item("node_count").Value(GetExecNodeCount())
                .Item("resource_limits").Value(TotalResourceLimits_)
                .Item("resource_usage").Value(TotalResourceUsage_);
        }
    }


    void OnMasterConnected(const TMasterHandshakeResult& result)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto responseKeeper = Bootstrap_->GetResponseKeeper();
        responseKeeper->Start();

        LogEventFluently(ELogEventType::MasterConnected)
            .Item("address").Value(ServiceAddress_);

        AbortAbortingOperations(result.AbortingOperations);
        ReviveOperations(result.RevivingOperations);
    }

    void OnMasterDisconnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto responseKeeper = Bootstrap_->GetResponseKeeper();
        responseKeeper->Stop();

        LogEventFluently(ELogEventType::MasterDisconnected)
            .Item("address").Value(ServiceAddress_);

        auto operations = IdToOperation_;
        for (const auto& pair : operations) {
            auto operation = pair.second;
            if (!operation->IsFinishedState()) {
                operation->GetController()->Abort();
                SetOperationFinalState(
                    operation,
                    EOperationState::Aborted,
                    TError("Master disconnected"));
            }
            FinishOperation(operation);
        }
        YCHECK(IdToOperation_.empty());

        {
            TReaderGuard guard(AddressToNodeLock_);
            for (const auto& pair : AddressToNode_) {
                auto node = pair.second;
                node->Jobs().clear();
            }
        }

        IdToJob_.clear();

        std::fill(JobTypeCounters_.begin(), JobTypeCounters_.end(), 0);
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

        auto req = TYPathProxy::Get("//sys/pools");
        static auto poolConfigTemplate = New<TPoolConfig>();
        static auto poolConfigKeys = poolConfigTemplate->GetRegisteredKeys();
        TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly, poolConfigKeys);
        ToProto(req->mutable_attribute_filter(), attributeFilter);
        batchReq->AddRequest(req, "get_pools");
    }

    void HandlePools(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_pools");
        if (!rspOrError.IsOK()) {
            LOG_ERROR(rspOrError, "Error getting pools configuration");
            return;
        }

        try {
            const auto& rsp = rspOrError.Value();
            auto poolsNode = ConvertToNode(TYsonString(rsp->value()));
            PoolsUpdated_.Fire(poolsNode);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error parsing pools configuration");
        }
    }

    void RequestNodesAttributes(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
    {
        LOG_INFO("Updating nodes information");

        auto req = TYPathProxy::Get("//sys/nodes");
        TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly, {"scheduling_tags", "state"});
        ToProto(req->mutable_attribute_filter(), attributeFilter);
        batchReq->AddRequest(req, "get_nodes");
    }

    void HandleNodesAttributes(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_nodes");
        if (!rspOrError.IsOK()) {
            LOG_ERROR(rspOrError, "Error updating nodes information");
            return;
        }

        try {
            TReaderGuard guard(AddressToNodeLock_);

            const auto& rsp = rspOrError.Value();
            auto nodesMap = ConvertToNode(TYsonString(rsp->value()))->AsMap();
            for (const auto& child : nodesMap->GetChildren()) {
                auto address = child.first;
                auto node = child.second;
                auto newState = node->Attributes().Get<ENodeState>("state");

                if (AddressToNode_.find(address) == AddressToNode_.end()) {
                    if (newState == ENodeState::Online) {
                        LOG_WARNING("Node %v is not registered in scheduler but online at master", address);
                    }
                    continue;
                }

                auto execNode = AddressToNode_[address];
                auto oldState = execNode->GetMasterState();

                auto schedulingTags = node->Attributes().Find<std::vector<Stroka>>("scheduling_tags");
                if (schedulingTags) {
                    UpdateSchedulingTags(execNode, *schedulingTags);
                }

                if (oldState != newState) {
                    if (oldState == ENodeState::Online && newState != ENodeState::Online) {
                        SubtractNodeResources(execNode);
                        AbortJobsAtNode(execNode);
                    }
                    if (oldState != ENodeState::Online && newState == ENodeState::Online) {
                        AddNodeResources(execNode);
                    }
                }

                execNode->SetMasterState(newState);

                if (oldState != newState) {
                    LOG_INFO("Node %lv (Address: %v)", newState, address);
                }
            }
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
        auto req = TYPathProxy::Get(GetOperationPath(operation->GetId()));
        TAttributeFilter attributeFilter(
            EAttributeFilterMode::MatchingOnly,
            runtimeParamsTemplate->GetRegisteredKeys());
        ToProto(req->mutable_attribute_filter(), attributeFilter);
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
        auto operationNode = ConvertToNode(TYsonString(rsp->value()));
        auto attributesNode = ConvertToNode(operationNode->Attributes());

        OperationRuntimeParamsUpdated_.Fire(operation, attributesNode);
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
            return;
        }
        if (!rspOrError.IsOK()) {
            LOG_ERROR(rspOrError, "Error getting scheduler configuration");
            return;
        }

        auto oldConfig = ConvertToNode(Config_);

        try {
            const auto& rsp = rspOrError.Value();
            auto configFromCypress = ConvertToNode(TYsonString(rsp->value()));

            try {
                Config_->Load(InitialConfig_, /* validate */ true, /* setDefaults */ true);
                Config_->Load(configFromCypress, /* validate */ true, /* setDefaults */ false);
            } catch (const std::exception& ex) {
                LOG_ERROR(ex, "Error updating cell scheduler configuration");
                Config_->Load(oldConfig, /* validate */ true, /* setDefaults */ true);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error parsing updated scheduler configuration");
        }

        auto newConfig = ConvertToNode(Config_);

        if (!NYTree::AreNodesEqual(oldConfig, newConfig)) {
            LOG_INFO("Scheduler configuration updated");
        }
    }


    void DoStartOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

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
            controller->Essentiate();

            auto error = WaitFor(MasterConnector_->CreateOperationNode(operation));
            THROW_ERROR_EXCEPTION_IF_FAILED(error);

            if (operation->GetState() != EOperationState::Initializing)
                throw TFiberCanceledException();
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

            NProfiling::TScopedTimer timer;
            auto result = WaitFor(asyncResult);
            auto prepareDuration = timer.GetElapsed();
            operation->UpdateControllerTimeStatistics("/prepare", prepareDuration);

            THROW_ERROR_EXCEPTION_IF_FAILED(result);
        } catch (const std::exception& ex) {
            auto wrappedError = TError("Operation has failed to prepare")
                << ex;
            OnOperationFailed(operation, wrappedError);
            return;
        }

        if (operation->GetState() != EOperationState::Preparing) {
            throw TFiberCanceledException();
        }

        if (operation->GetQueued()) {
            operation->SetState(EOperationState::Pending);
        } else {
            operation->SetState(EOperationState::Running);
        }

        LOG_INFO("Operation has been prepared and is now running (OperationId: %v)",
            operationId);

        LogEventFluently(ELogEventType::OperationPrepared)
            .Item("operation_id").Value(operationId);

        LogOperationProgress(operation);

        // From this moment on the controller is fully responsible for the
        // operation's fate. It will eventually call #OnOperationCompleted or
        // #OnOperationFailed to inform the scheduler about the outcome.
    }

    void AbortAbortingOperations(const std::vector<TOperationPtr>& operations)
    {
        for (auto operation : operations) {
            AbortAbortingOperation(operation);
        }
    }

    void ReviveOperations(const std::vector<TOperationPtr>& operations)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YCHECK(IdToOperation_.empty());
        for (auto operation : operations) {
            ReviveOperation(operation);
        }
    }

    void ReviveOperation(TOperationPtr operation)
    {
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
        BIND(&TImpl::DoReviveOperation, MakeStrong(this), operation)
            .AsyncVia(controller->GetCancelableControlInvoker())
            .Run();
    }

    void DoReviveOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (operation->GetState() != EOperationState::Reviving) {
            throw TFiberCanceledException();
        }

        try {
            auto controller = operation->GetController();

            if (!operation->Snapshot()) {
                operation->SetCleanStart(true);
                controller->Initialize();
            }

            controller->Essentiate();

            {
                auto error = WaitFor(MasterConnector_->ResetRevivingOperationNode(operation));
                THROW_ERROR_EXCEPTION_IF_FAILED(error);
            }

            {
                auto asyncResult = VoidFuture;
                if (operation->Snapshot()) {
                    asyncResult = BIND(&IOperationController::Revive, controller)
                        .AsyncVia(controller->GetCancelableInvoker())
                        .Run();
                } else {
                    asyncResult = BIND(&IOperationController::Prepare, controller)
                        .AsyncVia(controller->GetCancelableInvoker())
                        .Run();
                }
                auto error = WaitFor(asyncResult);
                THROW_ERROR_EXCEPTION_IF_FAILED(error);
            }

            if (operation->GetState() != EOperationState::Reviving)
                throw TFiberCanceledException();
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Operation has failed to revive (OperationId: %v)",
                operation->GetId());
            auto wrappedError = TError("Operation has failed to revive") << ex;
            OnOperationFailed(operation, wrappedError);
            return;
        }

        // Discard the snapshot, if any.
        operation->Snapshot().Reset();

        if (operation->GetQueued()) {
            operation->SetState(EOperationState::Pending);
        } else {
            operation->SetState(EOperationState::Running);
        }

        LOG_INFO("Operation has been revived and is now running (OperationId: %v)",
            operation->GetId());
    }


    TExecNodePtr GetOrRegisterNode(TNodeId nodeId, const TNodeDescriptor& descriptor)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        TReaderGuard guard(AddressToNodeLock_);
        auto it = AddressToNode_.find(descriptor.GetDefaultAddress());
        if (it == AddressToNode_.end()) {
            // NB: RegisterNode takes exclusive access on AddressToNodeLock_.
            guard.Release();
            return RegisterNode(nodeId, descriptor);
        }

        // Update the current descriptor, just in case.
        auto node = it->second;
        node->Descriptor() = descriptor;
        return node;
    }

    TExecNodePtr RegisterNode(TNodeId nodeId, const TNodeDescriptor& descriptor)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto node = New<TExecNode>(nodeId, descriptor);
        const auto& address = node->GetDefaultAddress();

        auto lease = TLeaseManager::CreateLease(
            Config_->NodeHeartbeatTimeout,
            BIND(&TImpl::UnregisterNode, MakeWeak(this), node)
                .Via(GetControlInvoker()));

        node->SetLease(lease);

        {
            TWriterGuard guard(AddressToNodeLock_);
            YCHECK(AddressToNode_.insert(std::make_pair(address, node)).second);
        }

        LOG_INFO("Node registered (Address: %v)", address);

        return node;
    }

    void AbortJobsAtNode(TExecNodePtr node)
    {
        // Make a copy, the collection will be modified.
        auto jobs = node->Jobs();
        const auto& address = node->GetDefaultAddress();
        for (auto job : jobs) {
            LOG_INFO("Aborting job on an offline node %v (JobId: %v, OperationId: %v)",
                address,
                job->GetId(),
                job->GetOperationId());
            AbortJob(job, TError("Node offline"));
            UnregisterJob(job);
        }
    }

    void UpdateSchedulingTags(TExecNodePtr node, const std::vector<Stroka>& schedulingTags)
    {
        yhash_set<Stroka> tags;
        for (const auto& tag : schedulingTags) {
            tags.insert(tag);
            if (SchedulingTagResources_.find(tag) == SchedulingTagResources_.end()) {
                SchedulingTagResources_.insert(std::make_pair(tag, TNodeResources()));
            }
        }

        if (node->GetMasterState() == ENodeState::Online) {
            auto oldTags = node->SchedulingTags();
            for (const auto& oldTag : oldTags) {
                if (tags.find(oldTag) == tags.end()) {
                    SchedulingTagResources_[oldTag] -= node->ResourceLimits();
                }
            }

            for (const auto& tag : tags) {
                if (oldTags.find(tag) == oldTags.end()) {
                    SchedulingTagResources_[tag] += node->ResourceLimits();
                }
            }
        }

        node->SchedulingTags() = tags;
    }

    void SubtractNodeResources(TExecNodePtr node)
    {
        TotalResourceLimits_ -= node->ResourceLimits();
        TotalResourceUsage_ -= node->ResourceUsage();

        for (const auto& tag : node->SchedulingTags()) {
            SchedulingTagResources_[tag] -= node->ResourceLimits();
        }
    }

    void AddNodeResources(TExecNodePtr node)
    {
        TotalResourceLimits_ += node->ResourceLimits();
        TotalResourceUsage_ += node->ResourceUsage();

        for (const auto& tag : node->SchedulingTags()) {
            SchedulingTagResources_[tag] += node->ResourceLimits();
        }
    }

    void UnregisterNode(TExecNodePtr node)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Node unregistered (Address: %v)", node->GetDefaultAddress());

        if (node->GetMasterState() == ENodeState::Online) {
            SubtractNodeResources(node);
        }

        AbortJobsAtNode(node);

        {
            TWriterGuard guard(AddressToNodeLock_);
            YCHECK(AddressToNode_.erase(node->GetDefaultAddress()) == 1);
        }
    }


    void RegisterOperation(TOperationPtr operation)
    {
        YCHECK(IdToOperation_.insert(std::make_pair(operation->GetId(), operation)).second);

        OperationRegistered_.Fire(operation);

        GetMasterConnector()->AddOperationWatcherRequester(
            operation,
            BIND(&TImpl::RequestOperationRuntimeParams, Unretained(this), operation));
        GetMasterConnector()->AddOperationWatcherHandler(
            operation,
            BIND(&TImpl::HandleOperationRuntimeParams, Unretained(this), operation));

        LOG_DEBUG("Operation registered (OperationId: %v)",
            operation->GetId());
    }

    void AbortOperationJobs(TOperationPtr operation)
    {
        auto jobs = operation->Jobs();
        for (auto job : jobs) {
            AbortJob(job, TError("Operation has state %Qlv", operation->GetState()));
            UnregisterJob(job);
        }

        YCHECK(operation->Jobs().empty());
    }

    void UnregisterOperation(TOperationPtr operation)
    {
        YCHECK(IdToOperation_.erase(operation->GetId()) == 1);

        OperationUnregistered_.Fire(operation);

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
                .Run());

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


    void CommitSchedulerTransactions(TOperationPtr operation)
    {
        YCHECK(operation->GetState() == EOperationState::Completing);

        LOG_INFO("Committing scheduler transactions (OperationId: %v)",
            operation->GetId());

        auto commitTransaction = [&] (TTransactionPtr transaction) {
            if (!transaction) {
                return;
            }
            auto result = WaitFor(transaction->Commit());
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Operation has failed to commit");
            if (operation->GetState() != EOperationState::Completing) {
                throw TFiberCanceledException();
            }
        };

        commitTransaction(operation->GetInputTransaction());
        commitTransaction(operation->GetOutputTransaction());
        commitTransaction(operation->GetSyncSchedulerTransaction());

        LOG_INFO("Scheduler transactions committed (OperationId: %v)",
            operation->GetId());

        // NB: Never commit async transaction since it's used for writing Live Preview tables.
        operation->GetAsyncSchedulerTransaction()->Abort();
    }

    void AbortSchedulerTransactions(TOperationPtr operation)
    {
        auto abortTransaction = [&] (TTransactionPtr transaction) {
            if (transaction) {
                // Fire-and-forget.
                transaction->Abort();
            }
        };

        abortTransaction(operation->GetInputTransaction());
        abortTransaction(operation->GetOutputTransaction());
        abortTransaction(operation->GetSyncSchedulerTransaction());
        abortTransaction(operation->GetAsyncSchedulerTransaction());
    }

    void FinishOperation(TOperationPtr operation)
    {
        if (!operation->GetFinished().IsSet()) {
            operation->SetFinished();
            operation->SetController(nullptr);
            UnregisterOperation(operation);
        }
    }


    void RegisterJob(TJobPtr job)
    {
        auto operation = FindOperation(job->GetOperationId());
        YCHECK(operation);

        auto node = job->GetNode();

        ++JobTypeCounters_[job->GetType()];

        YCHECK(IdToJob_.insert(std::make_pair(job->GetId(), job)).second);
        YCHECK(operation->Jobs().insert(job).second);
        YCHECK(node->Jobs().insert(job).second);

        LOG_DEBUG("Job registered (JobId: %v, JobType: %v, OperationId: %v)",
            job->GetId(),
            job->GetType(),
            operation->GetId());
    }

    void UnregisterJob(TJobPtr job)
    {
        auto operation = FindOperation(job->GetOperationId());
        YCHECK(operation);

        auto node = job->GetNode();

        --JobTypeCounters_[job->GetType()];

        YCHECK(IdToJob_.erase(job->GetId()) == 1);
        YCHECK(operation->Jobs().erase(job) == 1);
        YCHECK(node->Jobs().erase(job) == 1);

        JobFinished_.Fire(job);

        LOG_DEBUG("Job unregistered (JobId: %v, OperationId: %v)",
            job->GetId(),
            operation->GetId());
    }

    TJobPtr FindJob(const TJobId& jobId)
    {
        auto it = IdToJob_.find(jobId);
        return it == IdToJob_.end() ? nullptr : it->second;
    }

    TJobPtr GetJobOrThrow(const TJobId& jobId)
    {
        auto job = FindJob(jobId);
        if (!job) {
            THROW_ERROR_EXCEPTION("No such job %v", jobId);
        }
        return job;
    }

    void AbortJob(TJobPtr job, const TError& error)
    {
        // This method must be safe to call for any job.
        if (job->GetState() != EJobState::Running &&
            job->GetState() != EJobState::Waiting)
            return;

        job->SetState(EJobState::Aborted);
        TJobResult result;
        ToProto(result.mutable_error(), error);
        result.mutable_statistics();

        job->SetResult(std::move(result));
        OnJobFinished(job);

        auto operation = FindOperation(job->GetOperationId());
        YCHECK(operation);

        if (operation->GetState() == EOperationState::Running) {
            LogFinishedJobFluently(ELogEventType::JobAborted, job)
                .Item("reason").Value(GetAbortReason(job->Result()));
            auto controller = operation->GetController();
            BIND(&IOperationController::OnJobAborted, controller, TAbortedJobSummary(job))
                .AsyncVia(controller->GetCancelableInvoker())
                .Run();
        }
    }

    void PreemptJob(TJobPtr job)
    {
        YCHECK(FindOperation(job->GetOperationId()));

        LOG_DEBUG("Job preempted (JobId: %v, OperationId: %v)",
            job->GetId(),
            job->GetOperationId());

        TError error("Job preempted");
        error.Attributes().Set("abort_reason", EAbortReason::Preemption);
        AbortJob(job, error);
    }


    void OnJobRunning(TJobPtr job, const TJobStatus& status)
    {
        auto operation = FindOperation(job->GetOperationId());
        YCHECK(operation);

        if (operation->GetState() == EOperationState::Running) {
            auto controller = operation->GetController();
            BIND(&IOperationController::OnJobRunning, controller, job->GetId(), status)
                .AsyncVia(controller->GetCancelableInvoker())
                .Run();
        }
    }

    void OnJobWaiting(TJobPtr)
    {
        // Do nothing.
    }

    void OnJobCompleted(TJobPtr job, TJobResult* result)
    {
        if (job->GetState() == EJobState::Running ||
            job->GetState() == EJobState::Waiting)
        {
            job->SetState(EJobState::Completed);
            job->SetResult(std::move(*result));

            OnJobFinished(job);

            auto operation = FindOperation(job->GetOperationId());
            YCHECK(operation);

            if (operation->GetState() == EOperationState::Running) {
                LogFinishedJobFluently(ELogEventType::JobCompleted, job);
                auto controller = operation->GetController();
                BIND(&IOperationController::OnJobCompleted, controller, TCompletedJobSummary(job))
                    .AsyncVia(controller->GetCancelableInvoker())
                    .Run();
            }

            ProcessFinishedJobResult(job);
        }

        UnregisterJob(job);
    }

    TFluentLogEvent LogFinishedJobFluently(ELogEventType eventType, TJobPtr job)
    {
        return LogEventFluently(eventType)
            .Item("job_id").Value(job->GetId())
            .Item("operation_id").Value(job->GetOperationId())
            .Item("start_time").Value(job->GetStartTime())
            .Item("finish_time").Value(job->GetFinishTime())
            .Item("resource_limits").Value(job->ResourceLimits())
            .Item("statistics").Value(job->Statistics())
            .Item("node_address").Value(job->GetNode()->GetDefaultAddress())
            .Item("job_type").Value(job->GetType());
    }

    void OnJobFailed(TJobPtr job, TJobResult* result)
    {
        if (job->GetState() == EJobState::Running ||
            job->GetState() == EJobState::Waiting)
        {
            job->SetState(EJobState::Failed);
            job->SetResult(std::move(*result));

            OnJobFinished(job);

            auto operation = FindOperation(job->GetOperationId());
            YCHECK(operation);

            if (operation->GetState() == EOperationState::Running) {
                auto error = FromProto<TError>(job->Result()->error());
                LogFinishedJobFluently(ELogEventType::JobFailed, job)
                    .Item("error").Value(error);
                auto controller = operation->GetController();
                BIND(&IOperationController::OnJobFailed, controller, TFailedJobSummary(job))
                    .AsyncVia(controller->GetCancelableInvoker())
                    .Run();
            }

            ProcessFinishedJobResult(job);
        }

        UnregisterJob(job);
    }

    void OnJobAborted(TJobPtr job, TJobResult* result)
    {
        // Only update the result for the first time.
        // Typically the scheduler decides to abort the job on its own.
        // In this case we should ignore the result returned from the node
        // and avoid notifying the controller twice.
        if (job->GetState() == EJobState::Running ||
            job->GetState() == EJobState::Waiting)
        {
            job->SetState(EJobState::Aborted);
            job->SetResult(std::move(*result));

            OnJobFinished(job);

            auto operation = FindOperation(job->GetOperationId());
            YCHECK(operation);

            if (operation->GetState() == EOperationState::Running) {
                LogFinishedJobFluently(ELogEventType::JobAborted, job)
                    .Item("reason").Value(GetAbortReason(job->Result()));
                auto controller = operation->GetController();
                BIND(&IOperationController::OnJobAborted, controller, TAbortedJobSummary(job))
                    .AsyncVia(controller->GetCancelableInvoker())
                    .Run();
            }
        }

        UnregisterJob(job);
    }

    void OnJobFinished(TJobPtr job)
    {
        job->FinalizeJob(TInstant::Now());
        auto duration = job->GetDuration();

        switch (job->GetState()) {
            case EJobState::Completed:
                Profiler.Increment(TotalCompletedJobTimeCounter_, duration.MicroSeconds());
                break;
            case EJobState::Failed:
                Profiler.Increment(TotalFailedJobTimeCounter_, duration.MicroSeconds());
                break;
            case EJobState::Aborted:
                Profiler.Increment(TotalAbortedJobTimeCounter_, duration.MicroSeconds());
                break;
            default:
                YUNREACHABLE();
        }
    }

    void ProcessFinishedJobResult(TJobPtr job)
    {
        auto jobFailed = job->GetState() == EJobState::Failed;
        const auto& schedulerResultExt = job->Result()->GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

        auto stderrChunkId = schedulerResultExt.has_stderr_chunk_id()
            ? FromProto<TChunkId>(schedulerResultExt.stderr_chunk_id())
            : NullChunkId;

        auto failContextChunkId = schedulerResultExt.has_fail_context_chunk_id()
            ? FromProto<TChunkId>(schedulerResultExt.fail_context_chunk_id())
            : NullChunkId;

        auto operation = FindOperation(job->GetOperationId());
        YCHECK(operation);

        if (jobFailed) {
            if (stderrChunkId) {
                operation->SetStderrCount(operation->GetStderrCount() + 1);
            }
            MasterConnector_->CreateJobNode(job, stderrChunkId, failContextChunkId);
            return;
        }

        YCHECK(!failContextChunkId);
        if (!stderrChunkId) {
            // Do not create job node.
            return;
        }

        // Job has not failed, but has stderr.
        if (operation->GetStderrCount() < operation->GetMaxStderrCount()) {
            MasterConnector_->CreateJobNode(job, stderrChunkId, failContextChunkId);
            operation->SetStderrCount(operation->GetStderrCount() + 1);
        } else {
            ReleaseStderrChunk(job, stderrChunkId);
        }
    }

    void ReleaseStderrChunk(TJobPtr job, const TChunkId& chunkId)
    {
        auto transaction = job->GetOperation()->GetAsyncSchedulerTransaction();
        if (!transaction)
            return;

        auto channel = GetMasterClient()->GetMasterChannelOrThrow(NApi::EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto req = TMasterYPathProxy::UnstageObject();
        ToProto(req->mutable_object_id(), chunkId);
        req->set_recursive(false);

        // Fire-and-forget.
        // The subscriber is only needed to log the outcome.
        proxy.Execute(req).Subscribe(
            BIND(&TImpl::OnStderrChunkReleased, MakeStrong(this)));
    }

    void OnStderrChunkReleased(const TMasterYPathProxy::TErrorOrRspUnstageObjectPtr& rspOrError)
    {
        if (!rspOrError.IsOK()) {
            LOG_WARNING(rspOrError, "Error releasing stderr chunk");
        }
    }

    void InitStrategy()
    {
        Strategy_ = CreateFairShareStrategy(Config_, this);
    }

    IOperationControllerPtr CreateController(TOperation* operation)
    {
        switch (operation->GetType()) {
            case EOperationType::Map:
                return CreateMapController(Config_, this, operation);
            case EOperationType::Merge:
                return CreateMergeController(Config_, this, operation);
            case EOperationType::Erase:
                return CreateEraseController(Config_, this, operation);
            case EOperationType::Sort:
                return CreateSortController(Config_, this, operation);
            case EOperationType::Reduce:
                return CreateReduceController(Config_, this, operation);
            case EOperationType::JoinReduce:
                return CreateJoinReduceController(Config_, this, operation);
            case EOperationType::MapReduce:
                return CreateMapReduceController(Config_, this, operation);
            case EOperationType::RemoteCopy:
                return CreateRemoteCopyController(Config_, this, operation);
            default:
                YUNREACHABLE();
        }
    }

    INodePtr GetSpecTemplate(EOperationType type, IMapNodePtr spec)
    {
        switch (type) {
            case EOperationType::Map:
                return Config_->MapOperationOptions->SpecTemplate;
            case EOperationType::Merge: {
                auto mergeSpec = ParseOperationSpec<TMergeOperationSpec>(spec);
                switch (mergeSpec->Mode) {
                    case EMergeMode::Unordered: {
                        return Config_->UnorderedMergeOperationOptions->SpecTemplate;
                    }
                    case EMergeMode::Ordered: {
                        return Config_->OrderedMergeOperationOptions->SpecTemplate;
                    }
                    case EMergeMode::Sorted: {
                        return Config_->SortedMergeOperationOptions->SpecTemplate;
                    }
                    default:
                        YUNREACHABLE();
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
                YUNREACHABLE();
        }
    }


    void DoCompleteOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (operation->IsFinishedState() || operation->IsFinishingState()) {
            // Operation is probably being aborted.
            return;
        }

        const auto& operationId = operation->GetId();

        LOG_INFO("Completing operation (OperationId: %v)",
            operationId);

        operation->SetState(EOperationState::Completing);

        // The operation may still have running jobs (e.g. those started speculatively).
        AbortOperationJobs(operation);

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

            CommitSchedulerTransactions(operation);

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

        AbortOperationJobs(operation);

        // First flush: ensure that all stderrs are attached and the
        // state is changed to its intermediate value.
        {
            auto asyncResult = MasterConnector_->FlushOperationNode(operation);
            WaitFor(asyncResult);
            if (operation->GetState() != intermediateState)
                return;
        }

        SetOperationFinalState(operation, finalState, error);

        AbortSchedulerTransactions(operation);

        // Second flush: ensure that the state is changed to its final value.
        {
            auto asyncResult = MasterConnector_->FlushOperationNode(operation);
            WaitFor(asyncResult);
            if (operation->GetState() != finalState)
                return;
        }

        auto controller = operation->GetController();
        if (controller) {
            controller->Abort();
        }

        LogOperationFinished(operation, logEventType, error);

        FinishOperation(operation);
    }

    void AbortAbortingOperation(TOperationPtr operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Aborting operation (OperationId: %v)",
             operation->GetId());

        YCHECK(operation->GetState() == EOperationState::Aborting);

        AbortSchedulerTransactions(operation);
        SetOperationFinalState(operation, EOperationState::Aborted, TError());

        WaitFor(MasterConnector_->FlushOperationNode(operation));

        LogOperationFinished(operation, ELogEventType::OperationCompleted, TError());
    }

    void BuildOrchid(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("connected").Value(MasterConnector_->IsConnected())
                .Item("cell").BeginMap()
                    .Item("resource_limits").Value(TotalResourceLimits_)
                    .Item("resource_usage").Value(TotalResourceUsage_)
                .EndMap()
                .Item("operations").DoMapFor(GetOperations(), [=] (TFluentMap fluent, const TOperationPtr& operation) {
                    if (FindOperation(operation->GetId())) {
                        BuildOperationYson(operation, fluent);
                    }
                })
                .Item("nodes").DoMapFor(GetExecNodes(), [=] (TFluentMap fluent, const TExecNodePtr& node) {
                    BuildNodeYson(node, fluent);
                })
                .Item("clusters").DoMapFor(GetClusterDirectory()->GetClusterNames(), [=] (TFluentMap fluent, const Stroka& clusterName) {
                    BuildClusterYson(clusterName, fluent);
                })
                .Item("config").Value(Config_)
                .DoIf(Strategy_ != nullptr, BIND(&ISchedulerStrategy::BuildOrchid, Strategy_.get()))
            .EndMap();
    }

    void BuildClusterYson(const Stroka& clusterName, IYsonConsumer* consumer)
    {
        BuildYsonMapFluently(consumer)
            .Item(clusterName)
            .Value(GetClusterDirectory()->GetConnection(clusterName)->GetConfig());
    }

    void BuildOperationYson(TOperationPtr operation, IYsonConsumer* consumer)
    {
        auto controller = operation->GetController();
        auto state = operation->GetState();
        bool hasProgress = (state == EOperationState::Running) || IsOperationFinished(state);
        BuildYsonMapFluently(consumer)
            .Item(ToString(operation->GetId())).BeginMap()
                // Include the complete list of attributes.
                .Do(BIND(&NScheduler::BuildInitializingOperationAttributes, operation))
                .Item("progress").BeginMap()
                    .DoIf(hasProgress, BIND([=] (IYsonConsumer* consumer) {
                        WaitFor(
                            BIND(&IOperationController::BuildProgress, controller)
                                .AsyncVia(controller->GetInvoker())
                                .Run(consumer));
                    }))
                    .Do(BIND(&ISchedulerStrategy::BuildOperationProgress, Strategy_.get(), operation->GetId()))
                .EndMap()
                .Item("brief_progress").BeginMap()
                    .DoIf(hasProgress, BIND([=] (IYsonConsumer* consumer) {
                        WaitFor(
                            BIND(&IOperationController::BuildBriefProgress, controller)
                                .AsyncVia(controller->GetInvoker())
                                .Run(consumer));
                    }))
                    .Do(BIND(&ISchedulerStrategy::BuildBriefOperationProgress, Strategy_.get(), operation->GetId()))
                .EndMap()
                .Item("running_jobs").BeginAttributes()
                    .Item("opaque").Value("true")
                .EndAttributes()
                .DoMapFor(operation->Jobs(), [=] (TFluentMap fluent, TJobPtr job) {
                    BuildJobYson(job, fluent);
                })
            .EndMap();
    }

    void BuildJobYson(TJobPtr job, IYsonConsumer* consumer)
    {
        BuildYsonMapFluently(consumer)
            .Item(ToString(job->GetId())).BeginMap()
                .Do([=] (TFluentMap fluent) {
                    BuildJobAttributes(job, fluent);
                })
            .EndMap();
    }

    void BuildNodeYson(TExecNodePtr node, IYsonConsumer* consumer)
    {
        BuildYsonMapFluently(consumer)
            .Item(node->GetDefaultAddress()).BeginMap()
                .Do([=] (TFluentMap fluent) {
                    BuildExecNodeAttributes(node, fluent);
                })
            .EndMap();
    }


    TJobPtr ProcessJobHeartbeat(
        TExecNodePtr node,
        NJobTrackerClient::NProto::TReqHeartbeat* request,
        NJobTrackerClient::NProto::TRspHeartbeat* response,
        TJobStatus* jobStatus,
        bool forceJobsLogging)
    {
        auto jobId = FromProto<TJobId>(jobStatus->job_id());
        auto state = EJobState(jobStatus->state());
        const auto& jobAddress = node->GetDefaultAddress();

        NLogging::TLogger Logger(SchedulerLogger);
        Logger.AddTag("Address: %v, JobId: %v",
            jobAddress,
            jobId);

        auto job = FindJob(jobId);
        if (!job) {
            switch (state) {
                case EJobState::Completed:
                    LOG_INFO("Unknown job has completed, removal scheduled");
                    ToProto(response->add_jobs_to_remove(), jobId);
                    break;

                case EJobState::Failed:
                    LOG_INFO("Unknown job has failed, removal scheduled");
                    ToProto(response->add_jobs_to_remove(), jobId);
                    break;

                case EJobState::Aborted:
                    LOG_INFO("Job aborted, removal scheduled");
                    ToProto(response->add_jobs_to_remove(), jobId);
                    break;

                case EJobState::Running:
                    LOG_INFO("Unknown job is running, abort scheduled");
                    ToProto(response->add_jobs_to_abort(), jobId);
                    break;

                case EJobState::Waiting:
                    LOG_INFO("Unknown job is waiting, abort scheduled");
                    ToProto(response->add_jobs_to_abort(), jobId);
                    break;

                case EJobState::Aborting:
                    LOG_DEBUG("Job is aborting");
                    break;

                default:
                    YUNREACHABLE();
            }
            return nullptr;
        }

        auto operation = FindOperation(job->GetOperationId());
        YCHECK(operation);

        Logger.AddTag("JobType: %v, State: %v, OperationId: %v",
            job->GetType(),
            state,
            operation->GetId());

        // Check if the job is running on a proper node.
        const auto& expectedAddress = job->GetNode()->GetDefaultAddress();
        if (jobAddress != expectedAddress) {
            // Job has moved from one node to another. No idea how this could happen.
            if (state == EJobState::Aborting) {
                // Do nothing, job is already terminating.
            } else if (state == EJobState::Completed || state == EJobState::Failed || state == EJobState::Aborted) {
                ToProto(response->add_jobs_to_remove(), jobId);
                LOG_WARNING("Job status report was expected from %v, removal scheduled",
                    expectedAddress);
            } else {
                ToProto(response->add_jobs_to_abort(), jobId);
                LOG_WARNING("Job status report was expected from %v, abort scheduled",
                    expectedAddress);
            }
            return nullptr;
        }

        bool shouldLogJob = (state != job->GetState()) || forceJobsLogging;
        switch (state) {
            case EJobState::Completed: {
                LOG_INFO("Job completed, removal scheduled");
                OnJobCompleted(job, jobStatus->mutable_result());
                ToProto(response->add_jobs_to_remove(), jobId);
                break;
            }

            case EJobState::Failed: {
                auto error = FromProto<TError>(jobStatus->result().error());
                LOG_INFO(error, "Job failed, removal scheduled");
                OnJobFailed(job, jobStatus->mutable_result());
                ToProto(response->add_jobs_to_remove(), jobId);
                break;
            }

            case EJobState::Aborted: {
                auto error = FromProto<TError>(jobStatus->result().error());
                LOG_INFO(error, "Job aborted, removal scheduled");
                OnJobAborted(job, jobStatus->mutable_result());
                ToProto(response->add_jobs_to_remove(), jobId);
                break;
            }

            case EJobState::Running:
            case EJobState::Waiting:
                if (job->GetState() == EJobState::Aborted) {
                    LOG_INFO("Aborting job");
                    ToProto(response->add_jobs_to_abort(), jobId);
                } else {
                    switch (state) {
                        case EJobState::Running: {
                            LOG_DEBUG_IF(shouldLogJob, "Job is running");
                            job->SetState(state);
                            job->SetProgress(jobStatus->progress());
                            OnJobRunning(job, *jobStatus);
                            auto delta = jobStatus->resource_usage() - job->ResourceUsage();
                            JobUpdated_.Fire(job, delta);
                            job->ResourceUsage() = jobStatus->resource_usage();
                            break;
                        }

                        case EJobState::Waiting:
                            LOG_DEBUG_IF(shouldLogJob, "Job is waiting");
                            OnJobWaiting(job);
                            break;

                        default:
                            YUNREACHABLE();
                    }
                }
                break;

            case EJobState::Aborting:
                LOG_DEBUG("Job is aborting");
                break;

            default:
                YUNREACHABLE();
        }

        return job;
    }

};

////////////////////////////////////////////////////////////////////

TScheduler::TScheduler(
    TSchedulerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TScheduler::~TScheduler()
{ }

void TScheduler::Initialize()
{
    Impl_->Initialize();
}

ISchedulerStrategy* TScheduler::GetStrategy()
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

std::vector<TExecNodePtr> TScheduler::GetExecNodes()
{
    return Impl_->GetExecNodes();
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
    const TError& error)
{
    return Impl_->AbortOperation(operation, error);
}

TFuture<void> TScheduler::SuspendOperation(TOperationPtr operation)
{
    return Impl_->SuspendOperation(operation);
}

TFuture<void> TScheduler::ResumeOperation(TOperationPtr operation)
{
    return Impl_->ResumeOperation(operation);
}

TFuture<TYsonString> TScheduler::Strace(const TJobId& jobId)
{
    return Impl_->Strace(jobId);
}

TFuture<void> TScheduler::DumpInputContext(const TJobId& jobId, const TYPath& path)
{
    return Impl_->DumpInputContext(jobId, path);
}

void TScheduler::ProcessHeartbeat(TCtxHeartbeatPtr context)
{
    Impl_->ProcessHeartbeat(context);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
