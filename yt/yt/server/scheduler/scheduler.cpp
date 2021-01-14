#include "scheduler.h"
#include "private.h"
#include "fair_share_strategy.h"
#include "helpers.h"
#include "job_prober_service.h"
#include "master_connector.h"
#include "node_shard.h"
#include "scheduler_strategy.h"
#include "controller_agent.h"
#include "operation_controller.h"
#include "bootstrap.h"
#include "operations_cleaner.h"
#include "controller_agent_tracker.h"
#include "scheduling_segment_manager.h"
#include "persistent_scheduler_state.h"

#include <yt/server/lib/scheduler/config.h>
#include <yt/server/lib/scheduler/scheduling_tag.h>
#include <yt/server/lib/scheduler/event_log.h>
#include <yt/server/lib/scheduler/helpers.h>

#include <yt/ytlib/scheduler/helpers.h>
#include <yt/ytlib/scheduler/job_resources.h>

#include <yt/client/security_client/acl.h>

#include <yt/ytlib/node_tracker_client/channel.h>

#include <yt/ytlib/table_client/schemaless_buffered_table_writer.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/client/object_client/helpers.h>

#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/unversioned_writer.h>
#include <yt/client/table_client/table_consumer.h>

#include <yt/client/api/transaction.h>

#include <yt/client/node_tracker_client/helpers.h>

#include <yt/ytlib/api/native/connection.h>

#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/controller_agent/controller_agent_service_proxy.h>

#include <yt/ytlib/job_tracker_client/proto/job_tracker_service.pb.h>

#include <yt/ytlib/node_tracker_client/helpers.h>

#include <yt/ytlib/security_client/helpers.h>

#include <yt/core/actions/cancelable_context.h>

#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/thread_affinity.h>
#include <yt/core/concurrency/thread_pool.h>
#include <yt/core/concurrency/throughput_throttler.h>

#include <yt/core/rpc/message.h>
#include <yt/core/rpc/response_keeper.h>

#include <yt/core/logging/fluent_log.h>

#include <yt/core/misc/lock_free.h>
#include <yt/core/misc/finally.h>
#include <yt/core/misc/numeric_helpers.h>
#include <yt/core/misc/sync_expiring_cache.h>

#include <yt/core/net/local_address.h>

#include <yt/core/profiling/timing.h>
#include <yt/core/profiling/profile_manager.h>

#include <yt/core/ytree/service_combiner.h>
#include <yt/core/ytree/virtual.h>
#include <yt/core/ytree/exception_helpers.h>
#include <yt/core/ytree/permission.h>

#include <yt/build/build.h>

#include <util/generic/size_literals.h>

namespace NYT::NScheduler {

using namespace NProfiling;
using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NRpc;
using namespace NNet;
using namespace NApi;
using namespace NObjectClient;
using namespace NHydra;
using namespace NJobTrackerClient;
using namespace NChunkClient;
using namespace NJobProberClient;
using namespace NNodeTrackerClient;
using namespace NTableClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NSecurityClient;
using namespace NEventLog;

using NNodeTrackerClient::TNodeId;
using NNodeTrackerClient::TNodeDescriptor;
using NNodeTrackerClient::TNodeDirectory;

using std::placeholders::_1;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;

////////////////////////////////////////////////////////////////////////////////

struct TPoolTreeKeysHolder
{
    TPoolTreeKeysHolder()
    {
        auto treeConfigTemplate = New<TFairShareStrategyTreeConfig>();
        auto treeConfigKeys = treeConfigTemplate->GetRegisteredKeys();

        auto poolConfigTemplate = New<TPoolConfig>();
        auto poolConfigKeys = poolConfigTemplate->GetRegisteredKeys();

        Keys.reserve(treeConfigKeys.size() + poolConfigKeys.size() + 2);
        Keys.insert(Keys.end(), treeConfigKeys.begin(), treeConfigKeys.end());
        Keys.insert(Keys.end(), poolConfigKeys.begin(), poolConfigKeys.end());
        Keys.insert(Keys.end(), DefaultTreeAttributeName);
        Keys.insert(Keys.end(), TreeConfigAttributeName);
    }

    std::vector<TString> Keys;
};

////////////////////////////////////////////////////////////////////////////////

class TScheduler::TImpl
    : public TRefCounted
    , public ISchedulerStrategyHost
    , public INodeShardHost
    , public IOperationsCleanerHost
    , public TEventLogHostBase
{
public:
    using TEventLogHostBase::LogEventFluently;

    TImpl(
        TSchedulerConfigPtr config,
        TBootstrap* bootstrap)
        : Config_(config)
        , InitialConfig_(Config_)
        , Bootstrap_(bootstrap)
        , SpecTemplate_(Config_->SpecTemplate)
        , MasterConnector_(std::make_unique<TMasterConnector>(Config_, Bootstrap_))
        , OrchidWorkerPool_(New<TThreadPool>(Config_->OrchidWorkerThreadCount, "OrchidWorker"))
        , FairShareUpdatePool_(New<TThreadPool>(Config_->FairShareUpdateThreadCount, "FSUpdatePool"))
    {
        YT_VERIFY(config);
        YT_VERIFY(bootstrap);
        VERIFY_INVOKER_THREAD_AFFINITY(GetControlInvoker(EControlQueue::Default), ControlThread);

        for (int index = 0; index < Config_->NodeShardCount; ++index) {
            NodeShards_.push_back(New<TNodeShard>(
                index,
                Config_,
                this,
                Bootstrap_));
            CancelableNodeShardInvokers_.push_back(GetNullInvoker());
        }

        HandleNodeIdChangesStrictly_ = Config_->HandleNodeIdChangesStrictly;

        OperationsCleaner_ = New<TOperationsCleaner>(Config_->OperationsCleaner, this, Bootstrap_);

        OperationsCleaner_->SubscribeOperationsArchived(BIND(&TImpl::OnOperationsArchived, MakeWeak(this)));

        ServiceAddress_ = BuildServiceAddress(
            GetLocalHostName(),
            Bootstrap_->GetConfig()->RpcPort);

        {
            std::vector<IInvokerPtr> feasibleInvokers;
            for (auto controlQueue : TEnumTraits<EControlQueue>::GetDomainValues()) {
                feasibleInvokers.push_back(Bootstrap_->GetControlInvoker(controlQueue));
            }

            Strategy_ = CreateFairShareStrategy(Config_, this, std::move(feasibleInvokers));
        }
    }

    void Initialize()
    {
        MasterConnector_->AddCommonWatcher(
            BIND(&TImpl::RequestConfig, Unretained(this)),
            BIND(&TImpl::HandleConfig, Unretained(this)),
            ESchedulerAlertType::UpdateConfig);

        MasterConnector_->AddCommonWatcher(
            BIND(&TImpl::RequestPoolTrees, Unretained(this)),
            BIND(&TImpl::HandlePoolTrees, Unretained(this)),
            ESchedulerAlertType::UpdatePools);

        MasterConnector_->SetCustomWatcher(
            EWatcherType::NodeAttributes,
            BIND(&TImpl::RequestNodesAttributes, Unretained(this)),
            BIND(&TImpl::HandleNodesAttributes, Unretained(this)),
            Config_->NodesAttributesUpdatePeriod);

        MasterConnector_->AddCommonWatcher(
            BIND(&TImpl::RequestOperationsEffectiveAcl, Unretained(this)),
            BIND(&TImpl::HandleOperationsEffectiveAcl, Unretained(this)));

        MasterConnector_->AddCommonWatcher(
            BIND(&TImpl::RequestOperationArchiveVersion, Unretained(this)),
            BIND(&TImpl::HandleOperationArchiveVersion, Unretained(this)));

        MasterConnector_->AddCommonWatcher(
            BIND(&TImpl::RequestClusterName, Unretained(this)),
            BIND(&TImpl::HandleClusterName, Unretained(this)));

        MasterConnector_->SubscribeMasterConnecting(BIND(
            &TImpl::OnMasterConnecting,
            Unretained(this)));
        MasterConnector_->SubscribeMasterHandshake(BIND(
            &TImpl::OnMasterHandshake,
            Unretained(this)));
        MasterConnector_->SubscribeMasterConnected(BIND(
            &TImpl::OnMasterConnected,
            Unretained(this)));
        MasterConnector_->SubscribeMasterDisconnected(BIND(
            &TImpl::OnMasterDisconnected,
            Unretained(this)));

        MasterConnector_->Start();

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(EControlQueue::SchedulerProfiling),
            BIND(&TImpl::OnProfiling, MakeWeak(this)),
            Config_->ProfilingUpdatePeriod);
        ProfilingExecutor_->Start();

        EventLogWriter_ = New<TEventLogWriter>(
            Config_->EventLog,
            GetMasterClient(),
            Bootstrap_->GetControlInvoker(EControlQueue::EventLog));
        ControlEventLogWriterConsumer_ = EventLogWriter_->CreateConsumer();
        FairShareEventLogWriterConsumer_ = EventLogWriter_->CreateConsumer();

        LogEventFluently(ELogEventType::SchedulerStarted)
            .Item("address").Value(ServiceAddress_);

        ClusterInfoLoggingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(EControlQueue::EventLog),
            BIND(&TImpl::OnClusterInfoLogging, MakeWeak(this)),
            Config_->ClusterInfoLoggingPeriod);
        ClusterInfoLoggingExecutor_->Start();

        NodesInfoLoggingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(EControlQueue::EventLog),
            BIND(&TImpl::OnNodesInfoLogging, MakeWeak(this)),
            Config_->NodesInfoLoggingPeriod);
        NodesInfoLoggingExecutor_->Start();

        UpdateExecNodeDescriptorsExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(EControlQueue::NodesPeriodicActivity),
            BIND(&TImpl::UpdateExecNodeDescriptors, MakeWeak(this)),
            Config_->ExecNodeDescriptorsUpdatePeriod);
        UpdateExecNodeDescriptorsExecutor_->Start();

        JobReporterWriteFailuresChecker_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(EControlQueue::CommonPeriodicActivity),
            BIND(&TImpl::CheckJobReporterIssues, MakeWeak(this)),
            Config_->JobReporterIssuesCheckPeriod);
        JobReporterWriteFailuresChecker_->Start();

        CachedExecNodeMemoryDistributionByTags_ = New<TSyncExpiringCache<TSchedulingTagFilter, TMemoryDistribution>>(
            BIND(&TImpl::CalculateMemoryDistribution, MakeStrong(this)),
            Config_->SchedulingTagFilterExpireTimeout,
            GetControlInvoker(EControlQueue::CommonPeriodicActivity));

        StrategyHungOperationsChecker_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(EControlQueue::OperationsPeriodicActivity),
            BIND(&TImpl::CheckHungOperations, MakeWeak(this)),
            Config_->OperationHangupCheckPeriod);
        StrategyHungOperationsChecker_->Start();

        OperationsDestroyerExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(EControlQueue::OperationsPeriodicActivity),
            BIND(&TImpl::PostOperationsToDestroy, MakeWeak(this)),
            Config_->OperationsDestroyPeriod);
        OperationsDestroyerExecutor_->Start();

        SchedulingSegmentsManagerExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(EControlQueue::CommonPeriodicActivity),
            BIND(&TImpl::ManageSchedulingSegments, MakeWeak(this)),
            Config_->SchedulingSegmentsManagePeriod);
        SchedulingSegmentsManagerExecutor_->Start();
    }

    const NApi::NNative::IClientPtr& GetMasterClient() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Bootstrap_->GetMasterClient();
    }

    IYPathServicePtr CreateOrchidService()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto staticOrchidProducer = BIND(&TImpl::BuildStaticOrchid, MakeStrong(this));
        auto staticOrchidService = IYPathService::FromProducer(staticOrchidProducer)
            ->Via(GetControlInvoker(EControlQueue::Orchid))
            ->Cached(
                Config_->StaticOrchidCacheUpdatePeriod,
                OrchidWorkerPool_->GetInvoker(),
                SchedulerProfiler.WithPrefix("/static_orchid"));
        StaticOrchidService_.Reset(dynamic_cast<ICachedYPathService*>(staticOrchidService.Get()));
        YT_VERIFY(StaticOrchidService_);

        auto lightStaticOrchidProducer = BIND(&TImpl::BuildLightStaticOrchid, MakeStrong(this));
        auto lightStaticOrchidService = IYPathService::FromProducer(lightStaticOrchidProducer)
            ->Via(GetControlInvoker(EControlQueue::Orchid));

        auto dynamicOrchidService = GetDynamicOrchidService()
            ->Via(GetControlInvoker(EControlQueue::Orchid));

        auto combinedOrchidService = New<TServiceCombiner>(
            std::vector<IYPathServicePtr>{
                staticOrchidService,
                std::move(lightStaticOrchidService),
                std::move(dynamicOrchidService)
            },
            Config_->OrchidKeysUpdatePeriod);
        CombinedOrchidService_.Reset(combinedOrchidService.Get());
        YT_VERIFY(CombinedOrchidService_);
        return combinedOrchidService;
    }

    TRefCountedExecNodeDescriptorMapPtr GetCachedExecNodeDescriptors()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto guard = ReaderGuard(ExecNodeDescriptorsLock_);
        return CachedExecNodeDescriptors_;
    }

    const TSchedulerConfigPtr& GetConfig() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Config_;
    }

    const std::vector<TNodeShardPtr>& GetNodeShards() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return NodeShards_;
    }

    const IInvokerPtr& GetCancelableNodeShardInvoker(int shardId) const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return CancelableNodeShardInvokers_[shardId];
    }

    bool IsConnected() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return MasterConnector_->GetState() == EMasterConnectorState::Connected;
    }

    void ValidateConnected()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        if (!IsConnected()) {
            THROW_ERROR_EXCEPTION(
                NRpc::EErrorCode::Unavailable,
                "Master is not connected");
        }
    }

    TMasterConnector* GetMasterConnector() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return MasterConnector_.get();
    }


    virtual void Disconnect(const TError& error) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        MasterConnector_->Disconnect(error);
    }

    virtual TInstant GetConnectionTime() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return MasterConnector_->GetConnectionTime();
    }

    TOperationPtr FindOperation(const TOperationIdOrAlias& idOrAlias) const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return Visit(idOrAlias.Payload,
            [&] (const TOperationId& id) -> TOperationPtr {
                auto it = IdToOperation_.find(id);
                return it == IdToOperation_.end() ? nullptr : it->second;
            },
            [&] (const TString& alias) -> TOperationPtr {
                auto it = OperationAliases_.find(alias);
                return it == OperationAliases_.end() ? nullptr : it->second.Operation;
            });
    }

    TOperationPtr GetOperation(const TOperationIdOrAlias& idOrAlias) const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto operation = FindOperation(idOrAlias);
        YT_VERIFY(operation);
        return operation;
    }

    TOperationPtr GetOperationOrThrow(const TOperationIdOrAlias& idOrAlias) const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto operation = FindOperation(idOrAlias);
        if (!operation) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::NoSuchOperation,
                "No such operation %v",
                idOrAlias);
        }
        return operation;
    }

    virtual TMemoryDistribution GetExecNodeMemoryDistribution(const TSchedulingTagFilter& filter) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return CachedExecNodeMemoryDistributionByTags_->Get(filter);
    }

    virtual void SetSchedulerAlert(ESchedulerAlertType alertType, const TError& alert) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!alert.IsOK()) {
            YT_LOG_WARNING(alert, "Setting scheduler alert (AlertType: %v)", alertType);
        } else {
            YT_LOG_DEBUG("Reset scheduler alert (AlertType: %v)", alertType);
        }

        MasterConnector_->SetSchedulerAlert(alertType, alert);
    }

    virtual TFuture<void> SetOperationAlert(
        TOperationId operationId,
        EOperationAlertType alertType,
        const TError& alert,
        std::optional<TDuration> timeout = std::nullopt) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND(&TImpl::DoSetOperationAlert, MakeStrong(this), operationId, alertType, alert, timeout)
            .AsyncVia(GetControlInvoker(EControlQueue::Operation))
            .Run();
    }

    virtual void ValidatePoolPermission(
        const TYPath& path,
        const TString& user,
        EPermission permission) const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_LOG_DEBUG("Validating pool permission (Permission: %v, User: %v, Pool: %v)",
            permission,
            user,
            path);

        const auto& client = GetMasterClient();
        auto result = WaitFor(client->CheckPermission(user, Config_->PoolTreesRoot + path, permission))
            .ValueOrThrow();
        if (result.Action == ESecurityAction::Deny) {
            THROW_ERROR_EXCEPTION(
                NSecurityClient::EErrorCode::AuthorizationError,
                "User %Qv has been denied access to pool %v",
                user,
                path.empty() ? RootPoolName : path)
                << result.ToError(user, permission);
        }

        YT_LOG_DEBUG("Pool permission successfully validated");
    }

    TFuture<void> ValidateOperationAccess(
        const TString& user,
        TOperationId operationId,
        EPermissionSet permissions) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto doValidateOperationAccess = BIND([=, this_ = MakeStrong(this)] {
            auto operation = GetOperationOrThrow(operationId);
            NScheduler::ValidateOperationAccess(
                user,
                operationId,
                TJobId(),
                permissions,
                operation->GetRuntimeParameters()->Acl,
                GetMasterClient(),
                Logger);
        });

        return doValidateOperationAccess
            .AsyncVia(GetControlInvoker(EControlQueue::Operation))
            .Run();
    }

    void DoValidateJobShellAccess(const TString& user, const TJobShellPtr& jobShell)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        TObjectServiceProxy proxy(Bootstrap_
            ->GetMasterClient()
            ->GetMasterChannelOrThrow(EMasterChannelKind::Cache, PrimaryMasterCellTag));
        auto connectionConfig = Bootstrap_
            ->GetMasterClient()
            ->GetNativeConnection()
            ->GetConfig();
        TMasterReadOptions readOptions;
        readOptions.ReadFrom = EMasterChannelKind::Cache;

        auto userClosure = GetSubjectClosure(
            user,
            proxy,
            connectionConfig,
            readOptions);

        auto allowedSubjects = jobShell->Owners;
        allowedSubjects.push_back(RootUserName);
        allowedSubjects.push_back(SuperusersGroupName);

        for (const auto& allowedSubject : allowedSubjects) {
            if (allowedSubject == user || userClosure.contains(allowedSubject)) {
                return;
            }
        }

        THROW_ERROR_EXCEPTION(
            NSecurityClient::EErrorCode::AuthorizationError,
            "User %Qv is not allowed to run job shell %Qv",
            user,
            jobShell->Name);
    }

    TFuture<void> ValidateJobShellAccess(
        const TString& user,
        const TJobShellPtr& jobShell)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND(&TImpl::DoValidateJobShellAccess, MakeStrong(this))
            .AsyncVia(GetControlInvoker(EControlQueue::Operation))
            .Run(user, jobShell);
    }

    TFuture<TParseOperationSpecResult> ParseSpec(TYsonString specString) const
    {
        return BIND(&NScheduler::ParseSpec, Passed(std::move(specString)), SpecTemplate_, /* operationId */ std::nullopt)
            .AsyncVia(TDispatcher::Get()->GetHeavyInvoker())
            .Run();
    }

    TFuture<TOperationPtr> StartOperation(
        EOperationType type,
        TTransactionId transactionId,
        TMutationId mutationId,
        const TString& user,
        TParseOperationSpecResult parseSpecResult)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (static_cast<int>(IdToOperation_.size()) >= Config_->MaxOperationCount) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::TooManyOperations,
                "Limit for the total number of concurrent operations %v has been reached",
                Config_->MaxOperationCount);
        }

        auto spec = parseSpecResult.Spec;
        auto secureVault = std::move(spec->SecureVault);

        auto baseAcl = GetOperationBaseAcl();
        if (spec->AddAuthenticatedUserToAcl) {
            baseAcl.Entries.emplace_back(
                ESecurityAction::Allow,
                std::vector<TString>{user},
                EPermissionSet(EPermission::Read | EPermission::Manage));
        }

        auto operationId = MakeRandomId(
            EObjectType::Operation,
            GetMasterClient()->GetNativeConnection()->GetPrimaryMasterCellTag());

        auto runtimeParameters = New<TOperationRuntimeParameters>();
        Strategy_->InitOperationRuntimeParameters(runtimeParameters, spec, baseAcl, user, type);

        auto operation = New<TOperation>(
            operationId,
            type,
            mutationId,
            transactionId,
            spec,
            std::move(parseSpecResult.CustomSpecPerTree),
            std::move(parseSpecResult.SpecString),
            secureVault,
            runtimeParameters,
            std::move(baseAcl),
            user,
            TInstant::Now(),
            MasterConnector_->GetCancelableControlInvoker(EControlQueue::Operation),
            spec->Alias,
            spec->ScheduleInSingleTree && Config_->EnableScheduleInSingleTree);

        IdToStartingOperation_.emplace(operationId, operation);

        if (!spec->Owners.empty()) {
            operation->SetAlert(
                EOperationAlertType::OwnersInSpecIgnored,
                TError("\"owners\" field in spec ignored as it was specified simultaneously with \"acl\""));
        }

        operation->SetStateAndEnqueueEvent(EOperationState::Starting);

        auto codicilGuard = operation->MakeCodicilGuard();

        YT_LOG_INFO("Starting operation (OperationType: %v, OperationId: %v, TransactionId: %v, User: %v)",
            type,
            operationId,
            transactionId,
            user);

        YT_LOG_INFO("Total resource limits (OperationId: %v, ResourceLimits: %v)",
            operationId,
            FormatResources(GetResourceLimits(EmptySchedulingTagFilter)));

        try {
            WaitFor(Strategy_->ValidateOperationStart(operation.Get()))
                .ThrowOnError();
        } catch (const std::exception& ex) {
            // It means that scheduler was disconnected during check.
            if (operation->GetStarted().IsSet()) {
                return operation->GetStarted();
            }
            auto wrappedError = TError("Operation has failed to start")
                << ex;
            operation->SetStarted(wrappedError);
            YT_VERIFY(IdToStartingOperation_.erase(operationId) == 1);
            THROW_ERROR(wrappedError);
        }

        if (operation->Spec()->TestingOperationOptions->DelayBeforeStart) {
            TDelayedExecutor::WaitForDuration(*operation->Spec()->TestingOperationOptions->DelayBeforeStart);
        }

        operation->GetCancelableControlInvoker()->Invoke(
            BIND(&TImpl::DoStartOperation, MakeStrong(this), operation));

        return operation->GetStarted();
    }

    TFuture<void> AbortOperation(
        const TOperationPtr& operation,
        const TError& error,
        const TString& user)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        if (operation->GetState() == EOperationState::None) {
            THROW_ERROR_EXCEPTION("Operation is not started yet");
        }

        WaitFor(ValidateOperationAccess(user, operation->GetId(), EPermissionSet(EPermission::Manage)))
            .ThrowOnError();


        if (operation->IsFinishingState() || operation->IsFinishedState()) {
            YT_LOG_INFO(error, "Operation is already shutting down (OperationId: %v, State: %v)",
                operation->GetId(),
                operation->GetState());
            return operation->GetFinished();
        }

        operation->GetCancelableControlInvoker()->Invoke(
            BIND(&TImpl::DoAbortOperation, MakeStrong(this), operation, error));

        return operation->GetFinished();
    }

    TFuture<void> SuspendOperation(
        const TOperationPtr& operation,
        const TString& user,
        bool abortRunningJobs)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        if (operation->GetState() == EOperationState::None) {
            THROW_ERROR_EXCEPTION("Operation is not started yet");
        }

        WaitFor(ValidateOperationAccess(user, operation->GetId(), EPermissionSet(EPermission::Manage)))
            .ThrowOnError();

        if (operation->IsFinishingState() || operation->IsFinishedState()) {
            return MakeFuture(TError(
                EErrorCode::InvalidOperationState,
                "Cannot suspend operation in %Qlv state",
                operation->GetState()));
        }

        DoSuspendOperation(
            operation,
            TError("Suspend operation by user request"),
            abortRunningJobs,
            /* setAlert */ false);

        return MasterConnector_->FlushOperationNode(operation);
    }

    TFuture<void> ResumeOperation(
        const TOperationPtr& operation,
        const TString& user)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        if (operation->GetState() == EOperationState::None) {
            THROW_ERROR_EXCEPTION("Operation is not started yet");
        }

        WaitFor(ValidateOperationAccess(user, operation->GetId(), EPermissionSet(EPermission::Manage)))
            .ThrowOnError();

        if (!operation->GetSuspended()) {
            return MakeFuture(TError(
                EErrorCode::InvalidOperationState,
                "Operation is in %Qlv state",
                operation->GetState()));
        }

        std::vector<TFuture<void>> resumeFutures;
        for (const auto& nodeShard : NodeShards_) {
            resumeFutures.push_back(BIND(&TNodeShard::ResumeOperationJobs, nodeShard)
                .AsyncVia(nodeShard->GetInvoker())
                .Run(operation->GetId()));
        }
        WaitFor(AllSucceeded(resumeFutures))
            .ThrowOnError();

        operation->SetSuspended(false);
        operation->ResetAlert(EOperationAlertType::OperationSuspended);

        YT_LOG_INFO("Operation resumed (OperationId: %v)",
            operation->GetId());

        return MasterConnector_->FlushOperationNode(operation);
    }

    TFuture<void> CompleteOperation(
        const TOperationPtr& operation,
        const TError& error,
        const TString& user)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        WaitFor(ValidateOperationAccess(user, operation->GetId(), EPermissionSet(EPermission::Manage)))
            .ThrowOnError();

        if (operation->IsFinishingState() || operation->IsFinishedState()) {
            YT_LOG_INFO(error, "Operation is already shutting down (OperationId: %v, State: %v)",
                operation->GetId(),
                operation->GetState());
            return operation->GetFinished();
        }

        if (operation->GetState() != EOperationState::Running) {
            return MakeFuture(TError(
                EErrorCode::InvalidOperationState,
                "Operation is in %Qlv state",
                operation->GetState()));
        }

        YT_LOG_INFO(error, "Completing operation (OperationId: %v, State: %v)",
            operation->GetId(),
            operation->GetState());

        operation->SetAlert(
            EOperationAlertType::OperationCompletedByUserRequest,
            TError("Operation completed by user request")
                << TErrorAttribute("user", user));

        const auto& controller = operation->GetController();
        auto completeError = WaitFor(controller->Complete());
        if (!completeError.IsOK()) {
            THROW_ERROR_EXCEPTION("Failed to complete operation %v", operation->GetId())
                << completeError;
        }

        return operation->GetFinished();
    }

    void OnOperationCompleted(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        operation->GetCancelableControlInvoker()->Invoke(
            BIND(&TImpl::DoCompleteOperation, MakeStrong(this), operation));
    }

    void OnOperationAborted(const TOperationPtr& operation, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        operation->GetCancelableControlInvoker()->Invoke(
            BIND(&TImpl::DoAbortOperation, MakeStrong(this), operation, error));
    }

    void OnOperationFailed(const TOperationPtr& operation, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        operation->GetCancelableControlInvoker()->Invoke(
            BIND(&TImpl::DoFailOperation, MakeStrong(this), operation, error));
    }

    void OnOperationSuspended(const TOperationPtr& operation, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        operation->GetCancelableControlInvoker()->Invoke(BIND(
            &TImpl::DoSuspendOperation,
            MakeStrong(this),
            operation,
            error,
            /* abortRunningJobs */ true,
            /* setAlert */ true));
    }

    void OnOperationAgentUnregistered(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        const auto& controller = operation->GetController();
        controller->RevokeAgent();

        Strategy_->DisableOperation(operation.Get());

        operation->Restart(TError("Agent unregistered"));
        operation->SetStateAndEnqueueEvent(EOperationState::Orphaned);

        for (const auto& nodeShard : NodeShards_) {
            nodeShard->GetInvoker()->Invoke(BIND(
                &TNodeShard::StartOperationRevival,
                nodeShard,
                operation->GetId()));
        }

        AddOperationToTransientQueue(operation);
    }

    void OnOperationBannedInTentativeTree(const TOperationPtr& operation, const TString& treeId, const std::vector<TJobId>& jobIds)
    {
        YT_LOG_INFO("Operation banned in tentative tree (OperationId: %v, TreeId: %v)",
            operation->GetId(),
            treeId);

        std::vector<std::vector<TJobId>> jobIdsByShardId(NodeShards_.size());
        for (auto jobId : jobIds) {
            auto shardId = GetNodeShardId(NodeIdFromJobId(jobId));
            jobIdsByShardId[shardId].emplace_back(jobId);
        }
        for (int shardId = 0; shardId < NodeShards_.size(); ++shardId) {
            if (jobIdsByShardId[shardId].empty()) {
                continue;
            }
            NodeShards_[shardId]->GetInvoker()->Invoke(
                BIND(&TNodeShard::AbortJobs,
                    NodeShards_[shardId],
                    jobIdsByShardId[shardId],
                    TError("Job was in banned tentative pool tree")));
        }

        LogEventFluently(ELogEventType::OperationBannedInTree)
            .Item("operation_id").Value(operation->GetId())
            .Item(EventLogPoolTreeKey).Value(treeId);

        GetControlInvoker(EControlQueue::Operation)->Invoke(
            BIND(&TImpl::UnregisterOperationFromTreeForBannedTree, MakeStrong(this), operation, treeId));
    }

    void UnregisterOperationFromTreeForBannedTree(const TOperationPtr& operation, const TString& treeId)
    {
        const auto& schedulingOptionsPerPoolTree = operation->GetRuntimeParameters()->SchedulingOptionsPerPoolTree;
        if (schedulingOptionsPerPoolTree.find(treeId) != schedulingOptionsPerPoolTree.end()) {
            UnregisterOperationFromTree(operation, treeId);
        } else {
            YT_LOG_INFO("Operation was already unregistered from tree (OperationId: %v, TreeId: %v)",
                operation->GetId(),
                treeId);
        }
    }

    void UnregisterOperationFromTree(const TOperationPtr& operation, const TString& treeId)
    {
        YT_LOG_INFO("Unregistering operation from tree (OperationId: %v, TreeId: %v)",
            operation->GetId(),
            treeId);

        Strategy_->UnregisterOperationFromTree(operation->GetId(), treeId);

        operation->EraseTrees({treeId});
    }

    void ValidateOperationRuntimeParametersUpdate(
        const TOperationPtr& operation,
        const TOperationRuntimeParametersUpdatePtr& update)
    {
        // TODO(renadeen): Remove this someday.
        if (!Config_->PoolChangeIsAllowed) {
            if (update->Pool) {
                THROW_ERROR_EXCEPTION("Pool updates temporary disabled");
            }
            for (const auto& [treeId, schedulingOptions] : update->SchedulingOptionsPerPoolTree) {
                if (schedulingOptions->Pool) {
                    THROW_ERROR_EXCEPTION("Pool updates temporary disabled");
                }
            }
        }

        // NB(eshcherbin): We don't want to allow operation pool changes during materialization or revival
        // because we rely on them being unchanged in |FinishOperationMaterialization|.
        auto state = operation->GetState();
        if (state == EOperationState::Materializing || state == EOperationState::RevivingJobs) {
            THROW_ERROR_EXCEPTION("Operation runtime parameters update is forbidden while "
                                  "operation is in materializing or reviving jobs state");
        }
    }

    void DoUpdateOperationParameters(
        TOperationPtr operation,
        const TString& user,
        INodePtr parameters)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        auto update = ConvertTo<TOperationRuntimeParametersUpdatePtr>(parameters);

        WaitFor(ValidateOperationAccess(user, operation->GetId(), update->GetRequiredPermissions()))
            .ThrowOnError();

        if (update->Acl.has_value()) {
            update->Acl->Entries.insert(
                update->Acl->Entries.end(),
                operation->BaseAcl().Entries.begin(),
                operation->BaseAcl().Entries.end());
        }

        // Perform asynchronous validation of the new runtime parameters.
        {
            ValidateOperationRuntimeParametersUpdate(operation, update);
            auto newParams = UpdateRuntimeParameters(operation->GetRuntimeParameters(), update);
            WaitFor(Strategy_->ValidateOperationRuntimeParameters(operation.Get(), newParams, /* validatePools */ update->ContainsPool()))
                .ThrowOnError();
        }

        // We recalculate params, since original runtime params may change during asynchronous validation.
        auto newParams = UpdateRuntimeParameters(operation->GetRuntimeParameters(), update);
        operation->SetRuntimeParameters(newParams);
        Strategy_->ApplyOperationRuntimeParameters(operation.Get());

        // Updating ACL and other attributes.
        WaitFor(MasterConnector_->FlushOperationNode(operation))
            .ThrowOnError();

        if (auto controller = operation->GetController()) {
            WaitFor(controller->UpdateRuntimeParameters(update))
                .ThrowOnError();
        }

        LogEventFluently(ELogEventType::RuntimeParametersInfo)
            .Item("runtime_params").Value(newParams);

        YT_LOG_INFO("Operation runtime parameters updated (OperationId: %v)",
            operation->GetId());
    }

    TFuture<void> UpdateOperationParameters(
        const TOperationPtr& operation,
        const TString& user,
        INodePtr parameters)
    {
        return BIND(&TImpl::DoUpdateOperationParameters, MakeStrong(this), operation, user, std::move(parameters))
            .AsyncVia(operation->GetCancelableControlInvoker())
            .Run();
    }

    TFuture<void> DumpInputContext(TJobId jobId, const TYPath& path, const TString& user)
    {
        const auto& nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::DumpJobInputContext, nodeShard, jobId, path, user)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    TFuture<TNodeDescriptor> GetJobNode(TJobId jobId)
    {
        const auto& nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::GetJobNode, nodeShard, jobId)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    TFuture<void> AbandonJob(TJobId jobId, const TString& user)
    {
        const auto& nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::AbandonJob, nodeShard, jobId, user)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    TFuture<void> AbortJob(TJobId jobId, std::optional<TDuration> interruptTimeout, const TString& user)
    {
        const auto& nodeShard = GetNodeShardByJobId(jobId);
        return
            BIND(
                &TNodeShard::AbortJobByUserRequest,
                nodeShard,
                jobId,
                interruptTimeout,
                user)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    void ProcessNodeHeartbeat(const TCtxNodeHeartbeatPtr& context)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto* request = &context->Request();
        auto nodeId = request->node_id();

        auto unregisterFuture = VoidFuture;
        if (HandleNodeIdChangesStrictly_) {
            auto guard = Guard(NodeAddressToNodeShardIdLock_);

            auto descriptor = FromProto<TNodeDescriptor>(request->node_descriptor());
            const auto& address = descriptor.GetDefaultAddress();
            auto it = NodeAddressToNodeShardId_.find(address);
            if (it != NodeAddressToNodeShardId_.end()) {
                int oldNodeId = it->second;
                if (nodeId != oldNodeId) {
                    auto nodeShard = GetNodeShard(oldNodeId);
                    unregisterFuture =
                        BIND(&TNodeShard::UnregisterAndRemoveNodeById, GetNodeShard(oldNodeId), oldNodeId)
                            .AsyncVia(nodeShard->GetInvoker())
                            .Run();
                }
            }
            NodeAddressToNodeShardId_[address] = nodeId;
        }

        unregisterFuture.Subscribe(BIND([=, this_ = MakeStrong(this)] (const TError& error) {
            if (!error.IsOK()) {
                context->Reply(error);
                return;
            }

            const auto& nodeShard = GetNodeShard(nodeId);
            nodeShard->GetInvoker()->Invoke(BIND(&TNodeShard::ProcessHeartbeat, nodeShard, context));
        }));
    }

    // ISchedulerStrategyHost implementation
    virtual TJobResources GetResourceLimits(const TSchedulingTagFilter& filter) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        TJobResources resourceLimits;
        for (const auto& nodeShard : NodeShards_) {
            resourceLimits += nodeShard->GetResourceLimits(filter);
        }

        {
            auto value = std::make_pair(GetCpuInstant(), resourceLimits);
            auto it = CachedResourceLimitsByTags_.find(filter);
            if (it == CachedResourceLimitsByTags_.end()) {
                CachedResourceLimitsByTags_.emplace(filter, std::move(value));
            } else {
                it->second = std::move(value);
            }
        }

        return resourceLimits;
    }

    virtual void MarkOperationAsRunningInStrategy(TOperationId operationId) override
    {
        auto operation = GetOperation(operationId);

        if (operation->IsRunningInStrategy()) {
            // Operation is already marked as schedulable by strategy.
            return;
        }

        auto codicilGuard = operation->MakeCodicilGuard();

        DoSetOperationAlert(operationId, EOperationAlertType::OperationPending, TError());

        operation->SetRunningInStrategy();

        TryStartOperationMaterialization(operation);
    }

    virtual void AbortOperation(TOperationId operationId, const TError& error) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto operation = GetOperation(operationId);

        DoAbortOperation(operation, error);
    }

    virtual void FlushOperationNode(TOperationId operationId) override
    {
        auto operation = GetOperation(operationId);

        Y_UNUSED(MasterConnector_->FlushOperationNode(operation));
    }

    void TryStartOperationMaterialization(const TOperationPtr& operation)
    {
        if (operation->GetState() != EOperationState::Pending || !operation->IsRunningInStrategy()) {
            // Operation can be in finishing or initializing state or can be pending by strategy.
            return;
        }

        YT_LOG_INFO("Materializing operation (OperationId: %v, RevivedFromSnapshot: %v)",
            operation->GetId(),
            operation->GetRevivedFromSnapshot());

        TFuture<TOperationControllerMaterializeResult> asyncMaterializeResult;
        std::vector<TFuture<void>> futures;
        if (operation->GetRevivedFromSnapshot()) {
            operation->SetStateAndEnqueueEvent(EOperationState::RevivingJobs);
            futures.push_back(RegisterJobsFromRevivedOperation(operation));
        } else {
            operation->SetStateAndEnqueueEvent(EOperationState::Materializing);
            asyncMaterializeResult = operation->GetController()->Materialize();
            futures.push_back(asyncMaterializeResult.AsVoid());

            futures.push_back(ResetOperationRevival(operation));
        }

        if (operation->IsScheduledInSingleTree()) {
            // NB(eshcherbin): We need to make sure that all necessary information is in fair share tree snapshots
            // before choosing the best single tree for this operation during |FinishOperationMaterialization| later.
            futures.push_back(Strategy_->GetFullFairShareUpdateFinished());
        }

        auto expectedState = operation->GetState();
        AllSucceeded(std::move(futures)).Subscribe(
            BIND([=, this_ = MakeStrong(this), asyncMaterializeResult = std::move(asyncMaterializeResult)] (const TError& error) {
                if (!error.IsOK()) {
                    return;
                }
                if (operation->GetState() != expectedState) { // EOperationState::RevivingJobs or EOperationState::Materializing
                    YT_LOG_INFO(
                        "Operation state changed during materialization, skip materialization postprocessing "
                        "(ActualState: %v, ExpectedState: %v)",
                        operation->GetState(),
                        expectedState);
                    return;
                }

                std::optional<TOperationControllerMaterializeResult> maybeMaterializeResult;
                if (asyncMaterializeResult) {
                    // Async materialize result is ready here as the combined future already has finished.
                    YT_VERIFY(asyncMaterializeResult.IsSet());

                    // asyncMaterializeResult contains no error, otherwise the |!error.IsOk()| check would trigger.
                    maybeMaterializeResult = asyncMaterializeResult.Get().Value();
                }

                FinishOperationMaterialization(operation, maybeMaterializeResult);
            })
            .Via(operation->GetCancelableControlInvoker()));
    }


    void FinishOperationMaterialization(
        const TOperationPtr& operation,
        std::optional<TOperationControllerMaterializeResult> maybeMaterializeResult)
    {
        bool shouldFlush = false;
        bool shouldSuspend = false;
        TJobResources neededResources;
        if (maybeMaterializeResult) {
            // Operation was materialized from scratch.
            shouldSuspend = maybeMaterializeResult->Suspend;
            neededResources = maybeMaterializeResult->InitialNeededResources;
            operation->SetInitialAggregatedMinNeededResources(maybeMaterializeResult->InitialAggregatedMinNeededResources);
            shouldFlush = true;
        } else {
            // Operation was revived from snapshot.
            // NB(eshcherbin): NeededResources was set during revive.
            neededResources = operation->GetController()->GetNeededResources();
        }

        if (operation->IsScheduledInSingleTree()) {
            auto chosenTree = Strategy_->ChooseBestSingleTreeForOperation(operation->GetId(), neededResources);

            std::vector<TString> treeIdsToUnregister;
            for (const auto& [treeId, treeRuntimeParameters] : operation->GetRuntimeParameters()->SchedulingOptionsPerPoolTree) {
                YT_VERIFY(!treeRuntimeParameters->Tentative);
                if (treeId != chosenTree) {
                    treeIdsToUnregister.emplace_back(treeId);
                }
            }

            // TODO(eshcherbin): Fix the outdated comment.
            // If any tree was erased, we should:
            // (1) Unregister operation from each tree.
            // (2) Remove each tree from operation's runtime parameters.
            // (3) Flush all these changes to master.
            if (!treeIdsToUnregister.empty()) {
                for (const auto& treeId : treeIdsToUnregister) {
                    UnregisterOperationFromTree(operation, treeId);
                }

                shouldFlush = true;
            }
        }

        if (shouldFlush) {
            // NB(eshcherbin): Persist info about erased trees and min needed resources to master. This flush is safe because nothing should
            // happen to |operation| until its state is set to EOperationState::Running. The only possible exception would be the case when
            // materialization fails and the operation is terminated, but we've already checked for any fail beforehand.
            // Result is ignored since failure causes scheduler disconnection.
            auto expectedState = operation->GetState();
            Y_UNUSED(WaitFor(MasterConnector_->FlushOperationNode(operation)));
            if (operation->GetState() != expectedState) {
                return;
            }
        }

        {
            auto error = Strategy_->InitOperationSchedulingSegment(operation->GetId());
            if (!error.IsOK()) {
                OnOperationFailed(operation, error);
                return;
            }
        }

        if (operation->Spec()->TestingOperationOptions->DelayAfterMaterialize) {
            TDelayedExecutor::WaitForDuration(*operation->Spec()->TestingOperationOptions->DelayAfterMaterialize);
        }
        operation->SetStateAndEnqueueEvent(EOperationState::Running);
        Strategy_->EnableOperation(operation.Get());

        if (shouldSuspend) {
            DoSuspendOperation(
                operation,
                TError("Operation suspended due to suspend_operation_after_materialization spec option"),
                /* abortRunningJobs */ false,
                /* setAlert */ false);
        }

        LogEventFluently(ELogEventType::OperationMaterialized)
            .Item("operation_id").Value(operation->GetId());
    }

    virtual std::vector<TNodeId> GetExecNodeIds(const TSchedulingTagFilter& filter) const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        std::vector<TNodeId> result;
        for (const auto& [nodeId, descriptor] : NodeIdToDescriptor_) {
            if (filter.CanSchedule(descriptor.Tags)) {
                result.push_back(nodeId);
            }
        }

        return result;
    }

    virtual TString GetExecNodeAddress(NNodeTrackerClient::TNodeId nodeId) const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return GetOrCrash(NodeIdToDescriptor_, nodeId).Address;
    }

    virtual IInvokerPtr GetControlInvoker(EControlQueue queue) const override
    {
        return Bootstrap_->GetControlInvoker(queue);
    }

    virtual IInvokerPtr GetFairShareLoggingInvoker() const override
    {
        return FairShareLoggingActionQueue_->GetInvoker();
    }

    virtual IInvokerPtr GetFairShareProfilingInvoker() const override
    {
        return FairShareProfilingActionQueue_->GetInvoker();
    }

    virtual IInvokerPtr GetFairShareUpdateInvoker() const override
    {
        return FairShareUpdatePool_->GetInvoker();
    }

    virtual IInvokerPtr GetOrchidWorkerInvoker() const override
    {
        return OrchidWorkerPool_->GetInvoker();
    }

    IYsonConsumer* GetControlEventLogConsumer()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return ControlEventLogWriterConsumer_.get();
    }

    IYsonConsumer* GetFairShareEventLogConsumer()
    {
        VERIFY_INVOKER_AFFINITY(GetFairShareLoggingInvoker());

        return FairShareEventLogWriterConsumer_.get();
    }

    virtual IYsonConsumer* GetEventLogConsumer() override
    {
        // By default, the control thread's consumer is used.
        return GetControlEventLogConsumer();
    }

    virtual const NLogging::TLogger* GetEventLogger() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return &SchedulerEventLogger;
    }

    virtual void LogResourceMetering(
        const TMeteringKey& key,
        const TMeteringStatistics& statistics,
        TInstant lastUpdateTime,
        TInstant now) override
    {
        if (!ClusterName_) {
            return;
        }

        NLogging::LogStructuredEventFluently(SchedulerResourceMeteringLogger, NLogging::ELogLevel::Info)
            .Item("schema").Value("yt.scheduler.pools.compute.v1")
            .Item("id").Value(Format("%v:%v:%v", key.TreeId, key.PoolId, (now - TInstant()).Seconds()))
            .DoIf(Config_->ResourceMetering->EnableNewAbcFormat, [&] (TFluentMap fluent) {
                fluent
                    .Item("abc_id").Value(key.AbcId);
            })
            .DoIf(!Config_->ResourceMetering->EnableNewAbcFormat, [&] (TFluentMap fluent) {
                fluent
                    .Item("abc_id").Value(ToString(key.AbcId))
                    .Item("cloud_id").Value(Config_->ResourceMetering->DefaultCloudId)
                    .Item("folder_id").Value(Config_->ResourceMetering->DefaultFolderId);
            })
            .Item("usage").BeginMap()
                .Item("quantity").Value((now - lastUpdateTime).MilliSeconds())
                .Item("unit").Value("milliseconds")
                .Item("start").Value(lastUpdateTime.Seconds())
                .Item("finish").Value(now.Seconds())
            .EndMap()
            .Item("tags").BeginMap()
                .Item("strong_guarantee_resources").Value(statistics.StrongGuaranteeResources())
                .Item("min_share_resources").Value(statistics.StrongGuaranteeResources())
                .Item("allocated_resources").Value(statistics.AllocatedResources())
                .Item("pool_tree").Value(key.TreeId)
                .Item("pool").Value(key.PoolId)
                .Item("cluster").Value(ClusterName_)
            .EndMap()
            .Item("version").Value("1")
            .Item("source_wt").Value((now - TInstant()).Seconds());
    }

    virtual int GetDefaultAbcId() const override
    {
        return Config_->ResourceMetering->DefaultAbcId;
    }

    // NB(eshcherbin): Separate method due to separate invoker.
    virtual TFluentLogEvent LogFairShareEventFluently(TInstant now) override
    {
        VERIFY_INVOKER_AFFINITY(GetFairShareLoggingInvoker());

        return LogEventFluently(
            ELogEventType::FairShareInfo,
            GetFairShareEventLogConsumer(),
            GetEventLogger(),
            now);
    }

    // INodeShardHost implementation
    virtual int GetNodeShardId(TNodeId nodeId) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return nodeId % static_cast<int>(NodeShards_.size());
    }

    virtual TFuture<void> RegisterOrUpdateNode(
        TNodeId nodeId,
        const TString& nodeAddress,
        const THashSet<TString>& tags) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND(&TImpl::DoRegisterOrUpdateNode, MakeStrong(this))
            .AsyncVia(GetControlInvoker(EControlQueue::NodeTracker))
            .Run(nodeId, nodeAddress, tags);
    }

    virtual void UnregisterNode(TNodeId nodeId, const TString& nodeAddress) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        GetControlInvoker(EControlQueue::NodeTracker)->Invoke(
            BIND([this, this_ = MakeStrong(this), nodeId, nodeAddress] {
                // NOTE: If node is unregistered from node shard before it becomes online
                // then its id can be missing in the map.
                auto it = NodeIdToDescriptor_.find(nodeId);
                if (it == NodeIdToDescriptor_.end()) {
                    YT_LOG_WARNING("Node is not registered at scheduler (Address: %v)", nodeAddress);
                } else {
                    NodeIdToDescriptor_.erase(it);
                    YT_LOG_INFO("Node unregistered from scheduler (Address: %v)", nodeAddress);
                }
                NodeIdsWithoutTree_.erase(nodeId);
            }));
    }

    void ProcessNodesWithoutPoolTreeAlert()
    {
        if (NodeIdsWithoutTree_.empty()) {
            SetSchedulerAlert(ESchedulerAlertType::NodesWithoutPoolTree, TError());
        } else {
            std::vector<TString> nodeAddresses;
            int nodeCount = 0;
            bool truncated = false;
            for (auto nodeId : NodeIdsWithoutTree_) {
                nodeCount++;
                if (nodeCount > MaxNodesWithoutPoolTreeToAlert) {
                    truncated = true;
                    break;
                }
                nodeAddresses.push_back(GetOrCrash(NodeIdToDescriptor_, nodeId).Address);
            }

            SetSchedulerAlert(
                ESchedulerAlertType::NodesWithoutPoolTree,
                TError("Found nodes that do not match any pool tree")
                    << TErrorAttribute("node_addresses", nodeAddresses)
                    << TErrorAttribute("truncated", truncated)
                    << TErrorAttribute("node_count", NodeIdsWithoutTree_.size()));
        }
    }

    void OnNodeChangedFairShareTree(
        TNodeId nodeId,
        std::optional<TString> treeId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto it = NodeIdToDescriptor_.find(nodeId);
        YT_VERIFY(it != NodeIdToDescriptor_.end());

        auto& currentDescriptor = it->second;
        YT_VERIFY(treeId != currentDescriptor.TreeId);

        YT_LOG_INFO("Node has changed pool tree (NodeId: %v, Address: %v, OldTreeId: %v, NewTreeId: %v)",
            nodeId,
            currentDescriptor.Address,
            currentDescriptor.TreeId,
            treeId);

        currentDescriptor.CancelableContext->Cancel(
            TError("Node has changed fair share tree")
                << TErrorAttribute("old_pool_tree", currentDescriptor.TreeId)
                << TErrorAttribute("new_pool_tree", treeId));

        currentDescriptor.CancelableContext = New<TCancelableContext>();
        currentDescriptor.TreeId = treeId;

        auto nodeShard = GetNodeShard(nodeId);
        BIND(&TNodeShard::AbortJobsAtNode, GetNodeShard(nodeId), nodeId, EAbortReason::NodeFairShareTreeChanged)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

    // Implementation for INodeShardHost interface.
    void DoRegisterOrUpdateNode(
        TNodeId nodeId,
        const TString& nodeAddress,
        const THashSet<TString>& tags)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto treeIds = Strategy_->GetNodeTreeIds(tags);

        std::optional<TString> treeId;
        if (treeIds.size() == 0) {
            NodeIdsWithoutTree_.insert(nodeId);
        } else if (treeIds.size() == 1) {
            NodeIdsWithoutTree_.erase(nodeId);
            treeId = treeIds[0];
        } else {
            THROW_ERROR_EXCEPTION("Node belongs to more than one fair-share tree")
                << TErrorAttribute("matched_pool_trees", treeIds);
        }

        auto it = NodeIdToDescriptor_.find(nodeId);
        if (it == NodeIdToDescriptor_.end()) {
            YT_VERIFY(NodeIdToDescriptor_.emplace(
                nodeId,
                TExecNodeSchedulerDescriptor{tags, nodeAddress, treeId, New<TCancelableContext>()}
            ).second);
            YT_LOG_INFO("Node is registered at scheduler (NodeId: %v, Address: %v, Tags: %v, TreeId: %v)",
                nodeId,
                nodeAddress,
                tags,
                treeId);
        } else {
            auto& currentDescriptor = it->second;
            if (treeId != currentDescriptor.TreeId) {
                OnNodeChangedFairShareTree(nodeId, treeId);
                currentDescriptor.CancelableContext->Cancel(
                    TError("Node has changed fair share tree")
                        << TErrorAttribute("old_pool_tree", currentDescriptor.TreeId)
                        << TErrorAttribute("new_pool_tree", treeId));
            }
            currentDescriptor.Tags = tags;
            currentDescriptor.Address = nodeAddress;
            YT_LOG_INFO("Node was updated at scheduler (NodeId: %v, Address: %v, Tags: %v, TreeId: %v)",
                nodeId,
                nodeAddress,
                tags,
                treeId);
        }

        ProcessNodesWithoutPoolTreeAlert();
    }

    virtual void UpdateNodesOnChangedTrees(const THashMap<TString, TSchedulingTagFilter>& treeIdToFilter) override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        for (auto& [nodeId, descriptor] : NodeIdToDescriptor_) {
            std::optional<TString> newTreeId;
            for (const auto& [treeId, filter] : treeIdToFilter) {
                if (filter.CanSchedule(descriptor.Tags)) {
                    YT_VERIFY(!newTreeId);
                    newTreeId = treeId;
                }
            }
            if (newTreeId) {
                NodeIdsWithoutTree_.erase(nodeId);
            } else {
                NodeIdsWithoutTree_.insert(nodeId);
            }
            if (newTreeId != descriptor.TreeId) {
                OnNodeChangedFairShareTree(nodeId, newTreeId);
            }
        }

        ProcessNodesWithoutPoolTreeAlert();
    }

    virtual const ISchedulerStrategyPtr& GetStrategy() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Strategy_;
    }

    const TOperationsCleanerPtr& GetOperationsCleaner() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return OperationsCleaner_;
    }

    TFuture<void> AttachJobContext(
        const NYTree::TYPath& path,
        TChunkId chunkId,
        TOperationId operationId,
        TJobId jobId,
        const TString& user) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return BIND(&TImpl::DoAttachJobContext, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetControlInvoker(EControlQueue::UserRequest))
            .Run(path, chunkId, operationId, jobId, user);
    }

    TJobProberServiceProxy CreateJobProberProxy(const TAddressWithNetwork& addressWithNetwork) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        const auto& channelFactory = GetMasterClient()->GetChannelFactory();
        auto channel = channelFactory->CreateChannel(addressWithNetwork);

        TJobProberServiceProxy proxy(std::move(channel));
        proxy.SetDefaultTimeout(Config_->JobProberRpcTimeout);
        return proxy;
    }

    virtual int GetOperationArchiveVersion() const final
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return OperationArchiveVersion_.load();
    }

    bool IsJobReporterEnabled() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Config_->EnableJobReporter;
    }

    TSerializableAccessControlList GetOperationBaseAcl() const
    {
        YT_VERIFY(OperationBaseAcl_.has_value());
        return *OperationBaseAcl_;
    }

    virtual TString FormatResources(const TJobResourcesWithQuota& resources) const override
    {
        auto mediumDirectory = Bootstrap_
            ->GetMasterClient()
            ->GetNativeConnection()
            ->GetMediumDirectory();
        return NScheduler::FormatResources(resources, mediumDirectory);
    }

    virtual TString FormatResourceUsage(
        const TJobResources& usage,
        const TJobResources& limits,
        const NNodeTrackerClient::NProto::TDiskResources& diskResources) const override
    {
        auto mediumDirectory = Bootstrap_
            ->GetMasterClient()
            ->GetNativeConnection()
            ->GetMediumDirectory();
        return NScheduler::FormatResourceUsage(usage, limits, diskResources, mediumDirectory);
    }

    virtual TString FormatHeartbeatResourceUsage(
        const TJobResources& usage,
        const TJobResources& limits,
        const NNodeTrackerClient::NProto::TDiskResources& diskResources) const override
    {
        THashMap<int, std::vector<i64>> mediumIndexToFreeResources;
        for (const auto& locationResources : diskResources.disk_location_resources()) {
            int mediumIndex = locationResources.medium_index();
            mediumIndexToFreeResources[mediumIndex].push_back(locationResources.limit() - locationResources.usage());
        }

        auto mediumDirectory = Bootstrap_
            ->GetMasterClient()
            ->GetNativeConnection()
            ->GetMediumDirectory();

        return Format("{%v, FreeDiskResources: %v}",
            NScheduler::FormatResourceUsage(usage, limits),
            MakeFormattableView(mediumIndexToFreeResources, [&mediumDirectory] (TStringBuilderBase* builder, const std::pair<int, std::vector<i64>>& pair) {
                int mediumIndex = pair.first;
                const auto& freeDiskSpace = pair.second;
                auto* mediumDescriptor = mediumDirectory->FindByIndex(mediumIndex);
                TStringBuf mediumName = mediumDescriptor
                    ? mediumDescriptor->Name
                    : AsStringBuf("unknown");
                builder->AppendFormat("%v: %v", mediumName, freeDiskSpace);
            }));
    }

    virtual void InvokeStoringStrategyState(TPersistentStrategyStatePtr strategyState) override
    {
        MasterConnector_->InvokeStoringStrategyState(std::move(strategyState));
    }

    virtual bool IsCoreProfilingCompatibilityEnabled() const override
    {
        return Bootstrap_->GetConfig()->SolomonExporter->EnableCoreProfilingCompatibility;
    }

    TFuture<TOperationId> FindOperationIdByJobId(TJobId jobId)
    {
        const auto& nodeShard = GetNodeShardByJobId(jobId);
        return BIND(&TNodeShard::FindOperationIdByJobId, nodeShard, jobId)
            .AsyncVia(nodeShard->GetInvoker())
            .Run();
    }

private:
    TSchedulerConfigPtr Config_;
    const TSchedulerConfigPtr InitialConfig_;
    ui64 ConfigRevision_ = 0;

    TBootstrap* const Bootstrap_;

    NYTree::INodePtr SpecTemplate_;

    const std::unique_ptr<TMasterConnector> MasterConnector_;
    std::atomic<bool> Connected_ = {false};

    YT_DECLARE_SPINLOCK(TReaderWriterSpinLock, MediumDirectoryLock_);
    NChunkClient::TMediumDirectoryPtr MediumDirectory_;

    TOperationsCleanerPtr OperationsCleaner_;

    const TThreadPoolPtr OrchidWorkerPool_;
    const TActionQueuePtr FairShareLoggingActionQueue_ = New<TActionQueue>("FSLogging");
    const TActionQueuePtr FairShareProfilingActionQueue_ = New<TActionQueue>("FSProfiling");
    const TThreadPoolPtr FairShareUpdatePool_;

    std::optional<TString> ClusterName_;

    ISchedulerStrategyPtr Strategy_;

    struct TOperationAlias
    {
        //! Id of an operation assigned to a given alias.
        TOperationId OperationId;
        //! Operation assigned to a given alias. May be nullptr if operation has already completed.
        //! (in this case we still remember the operation id, though).
        TOperationPtr Operation;
    };

    THashMap<TOperationId, TOperationPtr> IdToOperation_;
    THashMap<TString, TOperationAlias> OperationAliases_;
    THashMap<TOperationId, IYPathServicePtr> IdToOperationService_;

    THashMap<TOperationId, TOperationPtr> IdToStartingOperation_;

    YT_DECLARE_SPINLOCK(TReaderWriterSpinLock, ExecNodeDescriptorsLock_);
    TRefCountedExecNodeDescriptorMapPtr CachedExecNodeDescriptors_ = New<TRefCountedExecNodeDescriptorMap>();

    TIntrusivePtr<TSyncExpiringCache<TSchedulingTagFilter, TMemoryDistribution>> CachedExecNodeMemoryDistributionByTags_;

    TJobResourcesProfiler TotalResourceLimitsProfiler_;
    TJobResourcesProfiler TotalResourceUsageProfiler_;

    TPeriodicExecutorPtr ProfilingExecutor_;
    TPeriodicExecutorPtr ClusterInfoLoggingExecutor_;
    TPeriodicExecutorPtr NodesInfoLoggingExecutor_;
    TPeriodicExecutorPtr UpdateExecNodeDescriptorsExecutor_;
    TPeriodicExecutorPtr JobReporterWriteFailuresChecker_;
    TPeriodicExecutorPtr StrategyHungOperationsChecker_;
    TPeriodicExecutorPtr TransientOperationQueueScanPeriodExecutor_;
    TPeriodicExecutorPtr PendingByPoolOperationScanPeriodExecutor_;
    TPeriodicExecutorPtr OperationsDestroyerExecutor_;
    TPeriodicExecutorPtr SchedulingSegmentsManagerExecutor_;

    TString ServiceAddress_;

    std::vector<TNodeShardPtr> NodeShards_;
    std::vector<IInvokerPtr> CancelableNodeShardInvokers_;

    struct TExecNodeSchedulerDescriptor
    {
        THashSet<TString> Tags;
        TString Address;
        std::optional<TString> TreeId;
        TCancelableContextPtr CancelableContext;
    };

    struct TOperationProgress
    {
        NYson::TYsonString Progress;
        NYson::TYsonString BriefProgress;
        NYson::TYsonString Alerts;
    };

    THashMap<TNodeId, TExecNodeSchedulerDescriptor> NodeIdToDescriptor_;
    THashSet<TNodeId> NodeIdsWithoutTree_;

    // Special map to support node consistency between node shards YT-11381.
    std::atomic<bool> HandleNodeIdChangesStrictly_;
    YT_DECLARE_SPINLOCK(TAdaptiveLock, NodeAddressToNodeShardIdLock_);
    THashMap<TString, int> NodeAddressToNodeShardId_;

    THashMap<TSchedulingTagFilter, std::pair<TCpuInstant, TJobResources>> CachedResourceLimitsByTags_;

    IEventLogWriterPtr EventLogWriter_;
    std::unique_ptr<IYsonConsumer> ControlEventLogWriterConsumer_;
    std::unique_ptr<IYsonConsumer> FairShareEventLogWriterConsumer_;

    std::atomic<int> OperationArchiveVersion_ = {-1};

    TEnumIndexedVector<EOperationState, std::vector<TOperationPtr>> StateToTransientOperations_;
    TInstant OperationToAgentAssignmentFailureTime_;

    std::optional<NSecurityClient::TSerializableAccessControlList> OperationBaseAcl_;

    TIntrusivePtr<NYTree::ICachedYPathService> StaticOrchidService_;
    TIntrusivePtr<NYTree::TServiceCombiner> CombinedOrchidService_;

    std::vector<TOperationPtr> OperationsToDestroy_;

    TNodeSchedulingSegmentManager NodeSchedulingSegmentManager_;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

    void DoAttachJobContext(
        const NYTree::TYPath& path,
        TChunkId chunkId,
        TOperationId operationId,
        TJobId jobId,
        const TString& user)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        MasterConnector_->AttachJobContext(path, chunkId, operationId, jobId, user);
    }


    void DoSetOperationAlert(
        TOperationId operationId,
        EOperationAlertType alertType,
        const TError& alert,
        std::optional<TDuration> timeout = std::nullopt)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto operation = FindOperation(operationId);
        if (!operation) {
            return;
        }

        if (alert.IsOK()) {
            if (operation->HasAlert(alertType)) {
                operation->ResetAlert(alertType);
                YT_LOG_DEBUG("Operation alert reset (OperationId: %v, Type: %v)",
                    operationId,
                    alertType);
            }
        } else {
            operation->SetAlert(alertType, alert, timeout);
            YT_LOG_DEBUG(alert, "Operation alert set (OperationId: %v, Type: %v)",
                operationId,
                alertType);
        }
    }


    const TNodeShardPtr& GetNodeShard(TNodeId nodeId) const
    {
        return NodeShards_[GetNodeShardId(nodeId)];
    }

    const TNodeShardPtr& GetNodeShardByJobId(TJobId jobId) const
    {
        auto nodeId = NodeIdFromJobId(jobId);
        return GetNodeShard(nodeId);
    }


    int GetExecNodeCount() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        int execNodeCount = 0;
        for (const auto& nodeShard : NodeShards_) {
            execNodeCount += nodeShard->GetExecNodeCount();
        }
        return execNodeCount;
    }

    int GetTotalNodeCount() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        int totalNodeCount = 0;
        for (const auto& nodeShard : NodeShards_) {
            totalNodeCount += nodeShard->GetTotalNodeCount();
        }
        return totalNodeCount;
    }

    int GetActiveJobCount()
    {
        int activeJobCount = 0;
        for (const auto& nodeShard : NodeShards_) {
            activeJobCount += nodeShard->GetActiveJobCount();
        }
        return activeJobCount;
    }

    void OnProfiling()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        TotalResourceLimitsProfiler_.Update(GetResourceLimits(EmptySchedulingTagFilter));
        TotalResourceUsageProfiler_.Update(GetResourceUsage(EmptySchedulingTagFilter));
    }

    void OnClusterInfoLogging()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (IsConnected()) {
            LogEventFluently(ELogEventType::ClusterInfo)
                .Item("exec_node_count").Value(GetExecNodeCount())
                .Item("total_node_count").Value(GetTotalNodeCount())
                .Item("resource_limits").Value(GetResourceLimits(EmptySchedulingTagFilter))
                .Item("resource_usage").Value(GetResourceUsage(EmptySchedulingTagFilter));
        }
    }

    void OnNodesInfoLogging()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!IsConnected()) {
            return;
        }

        std::vector<TFuture<TYsonString>> nodeListFutures;
        for (const auto& nodeShard : NodeShards_) {
            nodeListFutures.push_back(
                BIND([nodeShard] () {
                    return BuildYsonStringFluently<EYsonType::MapFragment>()
                        .Do(BIND(&TNodeShard::BuildNodesYson, nodeShard))
                        .Finish();
                })
                .AsyncVia(nodeShard->GetInvoker())
                .Run());
        }

        auto nodeLists = WaitFor(AllSucceeded(nodeListFutures)).ValueOrThrow();

        LogEventFluently(ELogEventType::NodesInfo)
            .Item("nodes")
                .DoMapFor(nodeLists, [] (TFluentMap fluent, const auto& nodeList) {
                    fluent.Items(nodeList);
                });
    }


    void OnMasterConnecting()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // NB: We cannot be sure the previous incarnation did a proper cleanup due to possible
        // fiber cancelation.
        DoCleanup();

        // NB: Must start the keeper before registering operations.
        const auto& responseKeeper = Bootstrap_->GetResponseKeeper();
        responseKeeper->Start();

        OperationsCleaner_->Start();
    }

    void OnMasterHandshake(const TMasterHandshakeResult& result)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ValidateConfig();

        {
            YT_LOG_INFO("Connecting node shards");

            auto segmentsInitializationDeadline = TInstant::Now() + Config_->SchedulingSegmentsInitializationTimeout;
            NodeSchedulingSegmentManager_.SetNodeSegmentsInitializationDeadline(segmentsInitializationDeadline);

            TNodeShardMasterHandshakeResult nodeShardResult{
                .InitialSchedulingSegmentsState = result.SchedulingSegmentsState,
                .SchedulingSegmentInitializationDeadline = segmentsInitializationDeadline,
            };

            std::vector<TFuture<IInvokerPtr>> asyncInvokers;
            for (const auto& nodeShard : NodeShards_) {
                asyncInvokers.push_back(BIND(&TNodeShard::OnMasterConnected, nodeShard, nodeShardResult)
                    .AsyncVia(nodeShard->GetInvoker())
                    .Run());
            }

            auto invokerOrError = WaitFor(AllSucceeded(asyncInvokers));
            if (!invokerOrError.IsOK()) {
                THROW_ERROR_EXCEPTION("Error connecting node shards")
                    << invokerOrError;
            }

            const auto& invokers = invokerOrError.Value();
            for (size_t index = 0; index < NodeShards_.size(); ++index) {
                CancelableNodeShardInvokers_[index ] = invokers[index];
            }
        }

        {
            YT_LOG_INFO("Registering existing operations");

            for (const auto& operation : result.Operations) {
                if (operation->GetMutationId()) {
                    NScheduler::NProto::TRspStartOperation response;
                    ToProto(response.mutable_operation_id(), operation->GetId());
                    auto responseMessage = CreateResponseMessage(response);
                    auto responseKeeper = Bootstrap_->GetResponseKeeper();
                    responseKeeper->EndRequest(operation->GetMutationId(), responseMessage);
                }

                // NB: it is valid to reset state, since operation revival descriptor
                // has necessary information about state.
                operation->SetStateAndEnqueueEvent(EOperationState::Orphaned);

                if (operation->Alias()) {
                    RegisterOperationAlias(operation);
                }
                RegisterOperation(operation, /* jobsReady */ false);

                AddOperationToTransientQueue(operation);
            }
        }
    }

    void OnMasterConnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        TransientOperationQueueScanPeriodExecutor_ = New<TPeriodicExecutor>(
            MasterConnector_->GetCancelableControlInvoker(EControlQueue::OperationsPeriodicActivity),
            BIND(&TImpl::ScanTransientOperationQueue, MakeWeak(this)),
            Config_->TransientOperationQueueScanPeriod);
        TransientOperationQueueScanPeriodExecutor_->Start();

        PendingByPoolOperationScanPeriodExecutor_ = New<TPeriodicExecutor>(
            MasterConnector_->GetCancelableControlInvoker(EControlQueue::OperationsPeriodicActivity),
            BIND(&TImpl::ScanPendingOperations, MakeWeak(this)),
            Config_->PendingByPoolOperationScanPeriod);
        PendingByPoolOperationScanPeriodExecutor_->Start();

        Strategy_->OnMasterConnected();

        TotalResourceLimitsProfiler_.Init(SchedulerProfiler.WithPrefix("/total_resource_limits"));
        TotalResourceUsageProfiler_.Init(SchedulerProfiler.WithPrefix("/total_resource_usage"));
        NodeSchedulingSegmentManager_.SetProfilingEnabled(true);

        LogEventFluently(ELogEventType::MasterConnected)
            .Item("address").Value(ServiceAddress_);
    }

    void DoCleanup()
    {
        NodeIdToDescriptor_.clear();

        TotalResourceLimitsProfiler_.Reset();
        TotalResourceUsageProfiler_.Reset();
        NodeSchedulingSegmentManager_.SetProfilingEnabled(false);

        {
            auto error = TError(EErrorCode::MasterDisconnected, "Master disconnected");
            for (const auto& [operationId, operation] : IdToOperation_) {
                if (!operation->IsFinishedState()) {
                    // This awakes those waiting for start promise.
                    SetOperationFinalState(
                        operation,
                        EOperationState::Aborted,
                        error);
                }
                operation->Cancel(error);
            }
            for (const auto& [operationId, operation] : IdToStartingOperation_) {
                YT_VERIFY(!operation->IsFinishedState());
                SetOperationFinalState(
                    operation,
                    EOperationState::Aborted,
                    error);
                operation->Cancel(error);
            }
            OperationAliases_.clear();
            IdToOperation_.clear();
            IdToOperationService_.clear();
            IdToStartingOperation_.clear();
        }

        for (auto& queue : StateToTransientOperations_) {
            queue.clear();
        }

        const auto& responseKeeper = Bootstrap_->GetResponseKeeper();
        responseKeeper->Stop();

        if (TransientOperationQueueScanPeriodExecutor_) {
            TransientOperationQueueScanPeriodExecutor_->Stop();
            TransientOperationQueueScanPeriodExecutor_.Reset();
        }

        if (PendingByPoolOperationScanPeriodExecutor_) {
            PendingByPoolOperationScanPeriodExecutor_->Stop();
            PendingByPoolOperationScanPeriodExecutor_.Reset();
        }

        Strategy_->OnMasterDisconnected();
        OperationsCleaner_->Stop();
    }

    void OnMasterDisconnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LogEventFluently(ELogEventType::MasterDisconnected)
            .Item("address").Value(ServiceAddress_);

        if (Config_->TestingOptions->MasterDisconnectDelay) {
            Sleep(*Config_->TestingOptions->MasterDisconnectDelay);
        }

        DoCleanup();

        {
            YT_LOG_INFO("Started disconnecting node shards");

            std::vector<TFuture<void>> asyncResults;
            for (const auto& nodeShard : NodeShards_) {
                asyncResults.push_back(BIND(&TNodeShard::OnMasterDisconnected, nodeShard)
                    .AsyncVia(nodeShard->GetInvoker())
                    .Run());
            }

            // XXX(babenko): fiber switch is forbidden here; do we actually need to wait for these results?
            AllSucceeded(asyncResults)
                .Get();

            YT_LOG_INFO("Finished disconnecting node shards");
        }
    }


    void LogOperationFinished(
        const TOperationPtr& operation,
        ELogEventType logEventType,
        const TError& error,
        TYsonString progress,
        TYsonString alerts)
    {
        LogEventFluently(logEventType)
            .Do(BIND(&TImpl::BuildOperationInfoForEventLog, MakeStrong(this), operation))
            .Item("start_time").Value(operation->GetStartTime())
            .Item("finish_time").Value(operation->GetFinishTime())
            .Item("error").Value(error)
            .DoIf(progress.operator bool(), [&] (TFluentMap fluent) {
                fluent.Item("progress").Value(progress);
            })
            .DoIf(alerts.operator bool(), [&] (TFluentMap fluent) {
                fluent.Item("alerts").Value(alerts);
            });
    }


    void ValidateOperationState(const TOperationPtr& operation, EOperationState expectedState)
    {
        if (operation->GetState() != expectedState) {
            YT_LOG_INFO("Operation has unexpected state (OperationId: %v, State: %v, ExpectedState: %v)",
                operation->GetId(),
                operation->GetState(),
                expectedState);
            throw TFiberCanceledException();
        }
    }


    void RequestPoolTrees(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
    {
        static const TPoolTreeKeysHolder PoolTreeKeysHolder;

        YT_LOG_INFO("Requesting pool trees");

        auto req = TYPathProxy::Get(Config_->PoolTreesRoot);
        ToProto(req->mutable_attributes()->mutable_keys(), PoolTreeKeysHolder.Keys);
        batchReq->AddRequest(req, "get_pool_trees");

        if (!Strategy_->IsInitialized()) {
            YT_LOG_INFO("Requesting strategy state");
            batchReq->AddRequest(TYPathProxy::Get(StrategyStatePath), "get_strategy_state");
        }
    }

    void HandlePoolTrees(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_pool_trees");
        if (!rspOrError.IsOK()) {
            THROW_ERROR(rspOrError.Wrap(EErrorCode::WatcherHandlerFailed, "Error getting pool trees"));
        }

        const auto& rsp = rspOrError.Value();
        INodePtr poolTreesNode;
        try {
            poolTreesNode = ConvertToNode(TYsonString(rsp->value()));
        } catch (const std::exception& ex) {
            auto error = TError(EErrorCode::WatcherHandlerFailed, "Error parsing pool trees")
                << ex;
            THROW_ERROR(error);
        }

        TPersistentStrategyStatePtr strategyState;
        if (!Strategy_->IsInitialized()) {
            rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_strategy_state");
            if (!rspOrError.IsOK() && !rspOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
                THROW_ERROR(rspOrError.Wrap(EErrorCode::WatcherHandlerFailed, "Error fetching strategy state"));
            }

            strategyState = New<TPersistentStrategyState>();
            if (!rspOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
                auto value = rspOrError.ValueOrThrow()->value();
                try {
                    strategyState = ConvertTo<TPersistentStrategyStatePtr>(TYsonString(value));
                    YT_LOG_INFO("Successfully fetched strategy state");
                } catch (const std::exception& ex) {
                    YT_LOG_WARNING(
                        ex,
                        "Failed to deserialize strategy state; will drop it (Value: %Qv)",
                        ConvertToYsonString(value, EYsonFormat::Text));
                }
            }
        }

        Strategy_->UpdatePoolTrees(poolTreesNode, strategyState);
    }


    void RequestNodesAttributes(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
    {
        YT_LOG_INFO("Requesting exec nodes information");

        auto req = TYPathProxy::List(GetClusterNodesPath());
        ToProto(req->mutable_attributes()->mutable_keys(), std::vector<TString>{
            "id",
            "tags",
            "state",
            "io_weights",
            "scheduling_segment",
            "data_center"
        });
        batchReq->AddRequest(req, "get_nodes");
    }

    void HandleNodesAttributes(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspList>("get_nodes");
        if (!rspOrError.IsOK()) {
            YT_LOG_WARNING(rspOrError, "Error getting exec nodes information");
            return;
        }

        try {
            const auto& rsp = rspOrError.Value();
            auto nodesList = ConvertToNode(TYsonString(rsp->value()))->AsList();
            std::vector<std::vector<std::pair<TString, INodePtr>>> nodesForShard(NodeShards_.size());
            std::vector<std::vector<TString>> nodeAddressesForShard(NodeShards_.size());

            for (const auto& child : nodesList->GetChildren()) {
                auto address = child->GetValue<TString>();
                auto objectId = child->Attributes().Get<TObjectId>("id");
                auto nodeId = NodeIdFromObjectId(objectId);
                auto nodeShardId = GetNodeShardId(nodeId);
                nodeAddressesForShard[nodeShardId].push_back(address);
                nodesForShard[nodeShardId].emplace_back(address, child);
            }

            std::vector<TFuture<void>> removeFutures;
            for (int i = 0 ; i < NodeShards_.size(); ++i) {
                auto& nodeShard = NodeShards_[i];
                removeFutures.push_back(
                    BIND(&TNodeShard::RemoveMissingNodes, nodeShard)
                        .AsyncVia(nodeShard->GetInvoker())
                        .Run(std::move(nodeAddressesForShard[i])));
            }
            WaitFor(AllSucceeded(removeFutures))
                .ThrowOnError();

            std::vector<TFuture<std::vector<TError>>> handleFutures;
            for (int i = 0 ; i < NodeShards_.size(); ++i) {
                auto& nodeShard = NodeShards_[i];
                handleFutures.push_back(
                    BIND(&TNodeShard::HandleNodesAttributes, nodeShard)
                        .AsyncVia(nodeShard->GetInvoker())
                        .Run(std::move(nodesForShard[i])));
            }
            auto handleErrors = WaitFor(AllSucceeded(handleFutures))
                .ValueOrThrow();

            std::vector<TError> allErrors;
            for (auto& errors : handleErrors) {
                for (auto& error : errors) {
                    allErrors.emplace_back(std::move(error));
                }
            }

            if (allErrors.empty()) {
                SetSchedulerAlert(ESchedulerAlertType::UpdateNodesFailed, TError());
            } else {
                SetSchedulerAlert(ESchedulerAlertType::UpdateNodesFailed,
                    TError("Failed to update some nodes")
                        << allErrors);
            }

            YT_LOG_INFO("Exec nodes information updated");
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Error updating exec nodes information");
        }
    }

    void RequestOperationsEffectiveAcl(const TObjectServiceProxy::TReqExecuteBatchPtr& batchReq)
    {
        YT_LOG_INFO("Requesting operations effective acl");

        auto req = TYPathProxy::Get("//sys/operations/@effective_acl");
        batchReq->AddRequest(req, "get_operations_effective_acl");
    }

    void HandleOperationsEffectiveAcl(const TObjectServiceProxy::TRspExecuteBatchPtr& batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_operations_effective_acl");
        if (!rspOrError.IsOK()) {
            YT_LOG_WARNING(rspOrError, "Error getting operations effective ACL");
            return;
        }

        TSerializableAccessControlList operationsEffectiveAcl;
        try {
            const auto& rsp = rspOrError.Value();
            operationsEffectiveAcl = ConvertTo<TSerializableAccessControlList>(TYsonString(rsp->value()));
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Error parsing operations effective ACL");
            return;
        }

        OperationBaseAcl_.emplace();
        for (const auto& ace : operationsEffectiveAcl.Entries) {
            if (ace.Action == ESecurityAction::Allow && Any(ace.Permissions & EPermission::Write)) {
                OperationBaseAcl_->Entries.emplace_back(
                    ESecurityAction::Allow,
                    ace.Subjects,
                    EPermissionSet(EPermission::Read | EPermission::Manage));
            }
        }
    }

    void RequestConfig(const TObjectServiceProxy::TReqExecuteBatchPtr& batchReq)
    {
        YT_LOG_INFO("Requesting scheduler configuration");

        auto req = TYPathProxy::Get("//sys/scheduler/config");
        batchReq->AddRequest(req, "get_config");
    }

    void HandleConfig(const TObjectServiceProxy::TRspExecuteBatchPtr& batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_config");
        if (rspOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
            // No config in Cypress, just ignore.
            return;
        }
        if (!rspOrError.IsOK()) {
            THROW_ERROR(rspOrError.Wrap(EErrorCode::WatcherHandlerFailed, "Error getting scheduler configuration"));
        }

        auto newConfig = CloneYsonSerializable(InitialConfig_);
        try {
            const auto& rsp = rspOrError.Value();
            auto configFromCypress = ConvertToNode(TYsonString(rsp->value()));
            try {
                newConfig->Load(configFromCypress, /* validate */ true, /* setDefaults */ false);
            } catch (const std::exception& ex) {
                auto error = TError(EErrorCode::WatcherHandlerFailed, "Error updating scheduler configuration")
                    << ex;
                THROW_ERROR(error);
            }
        } catch (const std::exception& ex) {
            auto error = TError(EErrorCode::WatcherHandlerFailed, "Error parsing updated scheduler configuration")
                << ex;
            THROW_ERROR(error);
        }

        auto oldConfigNode = ConvertToNode(Config_);
        auto newConfigNode = ConvertToNode(newConfig);

        if (!AreNodesEqual(oldConfigNode, newConfigNode)) {
            YT_LOG_INFO("Scheduler configuration updated");

            Config_ = newConfig;
            ValidateConfig();

            HandleNodeIdChangesStrictly_ = Config_->HandleNodeIdChangesStrictly;

            SpecTemplate_ = CloneNode(Config_->SpecTemplate);

            for (const auto& nodeShard : NodeShards_) {
                nodeShard->GetInvoker()->Invoke(
                    BIND(&TNodeShard::UpdateConfig, nodeShard, Config_));
            }

            Strategy_->UpdateConfig(Config_);
            MasterConnector_->UpdateConfig(Config_);
            OperationsCleaner_->UpdateConfig(Config_->OperationsCleaner);
            CachedExecNodeMemoryDistributionByTags_->SetExpirationTimeout(Config_->SchedulingTagFilterExpireTimeout);

            ProfilingExecutor_->SetPeriod(Config_->ProfilingUpdatePeriod);
            ClusterInfoLoggingExecutor_->SetPeriod(Config_->ClusterInfoLoggingPeriod);
            NodesInfoLoggingExecutor_->SetPeriod(Config_->NodesInfoLoggingPeriod);
            UpdateExecNodeDescriptorsExecutor_->SetPeriod(Config_->ExecNodeDescriptorsUpdatePeriod);
            JobReporterWriteFailuresChecker_->SetPeriod(Config_->JobReporterIssuesCheckPeriod);
            StrategyHungOperationsChecker_->SetPeriod(Config_->OperationHangupCheckPeriod);
            OperationsDestroyerExecutor_->SetPeriod(Config_->OperationsDestroyPeriod);
            SchedulingSegmentsManagerExecutor_->SetPeriod(Config_->SchedulingSegmentsManagePeriod);
            if (TransientOperationQueueScanPeriodExecutor_) {
                TransientOperationQueueScanPeriodExecutor_->SetPeriod(Config_->TransientOperationQueueScanPeriod);
            }
            if (PendingByPoolOperationScanPeriodExecutor_) {
                PendingByPoolOperationScanPeriodExecutor_->SetPeriod(Config_->PendingByPoolOperationScanPeriod);
            }
            StaticOrchidService_->SetCachePeriod(Config_->StaticOrchidCacheUpdatePeriod);
            CombinedOrchidService_->SetUpdatePeriod(Config_->OrchidKeysUpdatePeriod);

            Bootstrap_->GetControllerAgentTracker()->UpdateConfig(Config_);

            EventLogWriter_->UpdateConfig(Config_->EventLog);
        }

        ++ConfigRevision_;
    }


    void RequestOperationArchiveVersion(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
    {
        YT_LOG_INFO("Requesting operation archive version");

        auto req = TYPathProxy::Get(GetOperationsArchiveVersionPath());
        batchReq->AddRequest(req, "get_operation_archive_version");
    }

    void HandleOperationArchiveVersion(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_operation_archive_version");
        if (!rspOrError.IsOK()) {
            YT_LOG_INFO(rspOrError, "Error getting operation archive version");
            return;
        }

        try {
            auto version = ConvertTo<int>(TYsonString(rspOrError.Value()->value()));
            OperationArchiveVersion_.store(version, std::memory_order_relaxed);
            OperationsCleaner_->SetArchiveVersion(version);
            SetSchedulerAlert(ESchedulerAlertType::UpdateArchiveVersion, TError());
        } catch (const std::exception& ex) {
            auto error = TError("Error parsing operation archive version")
                << ex;
            SetSchedulerAlert(ESchedulerAlertType::UpdateArchiveVersion, error);
        }
    }

    void RequestClusterName(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
    {
        YT_LOG_INFO("Requesting cluster name");

        auto req = TYPathProxy::Get(GetClusterNamePath());
        batchReq->AddRequest(req, "get_cluster_name");
    }

    void HandleClusterName(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_cluster_name");
        if (!rspOrError.IsOK()) {
            YT_LOG_INFO(rspOrError, "Error getting cluster name");
            return;
        }

        ClusterName_ = ConvertTo<TString>(TYsonString(rspOrError.Value()->value()));
    }

    void UpdateExecNodeDescriptors()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        std::vector<TFuture<TRefCountedExecNodeDescriptorMapPtr>> shardDescriptorsFutures;
        for (const auto& nodeShard : NodeShards_) {
            shardDescriptorsFutures.push_back(BIND(&TNodeShard::GetExecNodeDescriptors, nodeShard)
                .AsyncVia(nodeShard->GetInvoker())
                .Run());
        }

        auto shardDescriptors = WaitFor(AllSucceeded(shardDescriptorsFutures))
            .ValueOrThrow();

        auto result = New<TRefCountedExecNodeDescriptorMap>();
        for (const auto& descriptors : shardDescriptors) {
            for (const auto& pair : *descriptors) {
                YT_VERIFY(result->insert(pair).second);
            }
        }

        {
            auto guard = WriterGuard(ExecNodeDescriptorsLock_);
            std::swap(CachedExecNodeDescriptors_, result);
        }
    }

    void CheckJobReporterIssues()
    {
        int writeFailures = 0;
        int queueIsTooLargeNodeCount = 0;
        for (const auto& shard : NodeShards_) {
            writeFailures += shard->ExtractJobReporterWriteFailuresCount();
            queueIsTooLargeNodeCount += shard->GetJobReporterQueueIsTooLargeNodeCount();
        }

        std::vector<TError> errors;
        if (writeFailures > Config_->JobReporterWriteFailuresAlertThreshold) {
            auto error = TError("Too many job archive writes failed")
                << TErrorAttribute("aggregation_period", Config_->JobReporterIssuesCheckPeriod)
                << TErrorAttribute("threshold", Config_->JobReporterWriteFailuresAlertThreshold)
                << TErrorAttribute("write_failures", writeFailures);
            errors.push_back(error);
        }
        if (queueIsTooLargeNodeCount > Config_->JobReporterQueueIsTooLargeAlertThreshold) {
            auto error = TError("Too many nodes have large job archivation queues")
                << TErrorAttribute("threshold", Config_->JobReporterQueueIsTooLargeAlertThreshold)
                << TErrorAttribute("queue_is_too_large_node_count", queueIsTooLargeNodeCount);
            errors.push_back(error);
        }

        TError resultError;
        if (!errors.empty()) {
            resultError = TError("Job archivation issues detected")
                << errors;
        }

        SetSchedulerAlert(ESchedulerAlertType::JobsArchivation, resultError);
    }

    void CheckHungOperations()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        for (const auto& [operationId, error] : Strategy_->GetHungOperations()) {
            if (auto operation = FindOperation(operationId)) {
                OnOperationFailed(operation, error);
            }
        }
    }

    virtual TRefCountedExecNodeDescriptorMapPtr CalculateExecNodeDescriptors(const TSchedulingTagFilter& filter) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TRefCountedExecNodeDescriptorMapPtr descriptors;
        {
            auto guard = ReaderGuard(ExecNodeDescriptorsLock_);
            descriptors = CachedExecNodeDescriptors_;
        }

        if (filter.IsEmpty()) {
            return descriptors;
        }

        auto result = New<TRefCountedExecNodeDescriptorMap>();
        for (const auto& [nodeId, descriptor] : *descriptors) {
            if (filter.CanSchedule(descriptor.Tags)) {
                YT_VERIFY(result->emplace(descriptor.Id, descriptor).second);
            }
        }
        return result;
    }

    TMemoryDistribution CalculateMemoryDistribution(const TSchedulingTagFilter& filter) const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TMemoryDistribution result;

        {
            auto guard = ReaderGuard(ExecNodeDescriptorsLock_);

            for (const auto& [nodeId, descriptor] : *CachedExecNodeDescriptors_) {
                if (descriptor.Online && filter.CanSchedule(descriptor.Tags)) {
                    ++result[RoundUp<i64>(descriptor.ResourceLimits.GetMemory(), 1_GB)];
                }
            }
        }

        return result;
    }

    void DoStartOperation(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        {
            TForbidContextSwitchGuard contextSwitchGuard;

            ValidateOperationState(operation, EOperationState::Starting);

            bool aliasRegistered = false;
            try {

                if (operation->Alias()) {
                    RegisterOperationAlias(operation);
                    aliasRegistered = true;
                }

                // NB(babenko): now we only validate this on start but not during revival
                // NB(ignat): this validation must be just before operation registration below
                // to avoid violation of pool limits. See YT-10802.

                auto poolLimitViolations = Strategy_->GetPoolLimitViolations(operation.Get(), operation->GetRuntimeParameters());

                std::vector<TString> erasedTreeIds;
                for (const auto& [treeId, error] : poolLimitViolations) {
                    if (GetSchedulingOptionsPerPoolTree(operation.Get(), treeId)->Tentative) {
                        YT_LOG_INFO(
                            error,
                            "Tree is erased for operation since pool limits are violated (OperationId: %v)",
                            operation->GetId());
                        erasedTreeIds.push_back(treeId);
                        // No need to throw now.
                        continue;
                    }

                    THROW_ERROR error;
                }
                operation->EraseTrees(erasedTreeIds);
            } catch (const std::exception& ex) {
                if (aliasRegistered) {
                    auto it = OperationAliases_.find(*operation->Alias());
                    YT_VERIFY(it != OperationAliases_.end());
                    YT_VERIFY(it->second.Operation == operation);
                    OperationAliases_.erase(it);
                }

                YT_VERIFY(IdToStartingOperation_.erase(operation->GetId()) == 1);

                auto wrappedError = TError("Operation has failed to start")
                    << ex;
                operation->SetStarted(wrappedError);
                return;
            }

            YT_VERIFY(IdToStartingOperation_.erase(operation->GetId()) == 1);

            ValidateOperationState(operation, EOperationState::Starting);

            RegisterOperation(operation, /* jobsReady */ true);

            if (operation->GetRuntimeParameters()->SchedulingOptionsPerPoolTree.empty()) {
                operation->SetStarted(TError("No pool trees found for operation"));
                UnregisterOperation(operation);
                return;
            }
        }

        try {
            WaitFor(MasterConnector_->CreateOperationNode(operation))
                .ThrowOnError();
        } catch (const std::exception& ex) {
            auto wrappedError = TError("Failed to create Cypress node for operation %v",
                operation->GetId())
                << ex;
            operation->SetStarted(wrappedError);
            UnregisterOperation(operation);
            return;
        }

        ValidateOperationState(operation, EOperationState::Starting);

        operation->SetStateAndEnqueueEvent(EOperationState::WaitingForAgent);
        AddOperationToTransientQueue(operation);

        // NB: Once we've registered the operation in Cypress we're free to complete
        // StartOperation request. Preparation will happen in a non-blocking
        // fashion.
        operation->SetStarted(TError());
    }

    NYson::TYsonString BuildBriefSpec(const TOperationPtr& operation) const
    {
        auto briefSpec = BuildYsonStringFluently()
            .BeginMap()
                .Items(operation->ControllerAttributes().InitializeAttributes->BriefSpec)
            .EndMap();
        return briefSpec;
    }

    void DoInitializeOperation(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        auto operationId = operation->GetId();

        ValidateOperationState(operation, EOperationState::Initializing);

        try {
            RegisterAssignedOperation(operation);

            const auto& controller = operation->GetController();

            auto initializeResult = WaitFor(controller->Initialize(/* transactions */ std::nullopt))
                .ValueOrThrow();

            ValidateOperationState(operation, EOperationState::Initializing);

            operation->Transactions() = initializeResult.Transactions;
            operation->ControllerAttributes().InitializeAttributes = std::move(initializeResult.Attributes);
            operation->BriefSpecString() = BuildBriefSpec(operation);

            WaitFor(MasterConnector_->UpdateInitializedOperationNode(operation))
                .ThrowOnError();

            ValidateOperationState(operation, EOperationState::Initializing);
        } catch (const std::exception& ex) {
            auto wrappedError = TError("Operation has failed to initialize")
                << ex;
            OnOperationFailed(operation, wrappedError);
            return;
        }

        ValidateOperationState(operation, EOperationState::Initializing);

        LogEventFluently(ELogEventType::OperationStarted)
            .Do(std::bind(&TImpl::BuildOperationInfoForEventLog, MakeStrong(this), operation, _1))
            .Do(std::bind(&ISchedulerStrategy::BuildOperationInfoForEventLog, Strategy_, operation.Get(), _1));

        YT_LOG_INFO("Preparing operation (OperationId: %v)",
            operationId);

        operation->SetStateAndEnqueueEvent(EOperationState::Preparing);

        try {
            // Run async preparation.
            const auto& controller = operation->GetController();

            {
                auto result = WaitFor(controller->Prepare())
                    .ValueOrThrow();

                operation->ControllerAttributes().PrepareAttributes = std::move(result.Attributes);
            }

            ValidateOperationState(operation, EOperationState::Preparing);

            operation->SetStateAndEnqueueEvent(EOperationState::Pending);
        } catch (const std::exception& ex) {
            auto wrappedError = TError(EErrorCode::OperationFailedToPrepare, "Operation has failed to prepare")
                << ex;
            OnOperationFailed(operation, wrappedError);
            return;
        }

        YT_LOG_INFO("Operation prepared (OperationId: %v)",
            operationId);

        LogEventFluently(ELogEventType::OperationPrepared)
            .Item("operation_id").Value(operationId)
            .Item("unrecognized_spec").Value(operation->ControllerAttributes().InitializeAttributes->UnrecognizedSpec);

        TryStartOperationMaterialization(operation);
    }

    void DoReviveOperation(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        auto operationId = operation->GetId();

        ValidateOperationState(operation, EOperationState::ReviveInitializing);

        YT_LOG_INFO("Reviving operation (OperationId: %v)",
            operationId);

        try {
            RegisterAssignedOperation(operation);

            ValidateOperationState(operation, EOperationState::ReviveInitializing);

            const auto& controller = operation->GetController();

            {
                YT_VERIFY(operation->RevivalDescriptor());
                auto result = WaitFor(controller->Initialize(operation->Transactions()))
                    .ValueOrThrow();

                operation->Transactions() = std::move(result.Transactions);
                operation->ControllerAttributes().InitializeAttributes = std::move(result.Attributes);
                operation->BriefSpecString() = BuildBriefSpec(operation);
            }

            ValidateOperationState(operation, EOperationState::ReviveInitializing);

            WaitFor(MasterConnector_->UpdateInitializedOperationNode(operation))
                .ThrowOnError();

            ValidateOperationState(operation, EOperationState::ReviveInitializing);

            operation->SetStateAndEnqueueEvent(EOperationState::Reviving);

            {
                auto result = WaitFor(controller->Revive())
                    .ValueOrThrow();

                ValidateOperationState(operation, EOperationState::Reviving);

                operation->ControllerAttributes().PrepareAttributes = result.Attributes;
                operation->SetRevivedFromSnapshot(result.RevivedFromSnapshot);
                operation->RevivedJobs() = std::move(result.RevivedJobs);
                for (const auto& bannedTreeId : result.RevivedBannedTreeIds) {
                    // If operation is already erased from the tree, UnregisterOperationFromTree() will produce unnecessary log messages.
                    // However, I believe that this way the code is simpler and more concise.
                    // NB(eshcherbin): this procedure won't abort jobs that are running in banned tentative trees.
                    // So in case of an unfortunate scheduler failure, these jobs will continue running.
                    const auto& schedulingOptionsPerPoolTree = operation->GetRuntimeParameters()->SchedulingOptionsPerPoolTree;
                    if (schedulingOptionsPerPoolTree.find(bannedTreeId) != schedulingOptionsPerPoolTree.end()) {
                        UnregisterOperationFromTree(operation, bannedTreeId);
                    }
                }
            }

            YT_LOG_INFO("Operation has been revived (OperationId: %v)",
                operationId);

            operation->RevivalDescriptor().reset();
            operation->SetStateAndEnqueueEvent(EOperationState::Pending);

        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Operation has failed to revive (OperationId: %v)",
                operationId);
            auto wrappedError = TError("Operation has failed to revive")
                << ex;
            OnOperationFailed(operation, wrappedError);
        }

        TryStartOperationMaterialization(operation);
    }

    TFuture<void> ResetOperationRevival(const TOperationPtr& operation)
    {
        std::vector<TFuture<void>> asyncResults;
        for (int shardId = 0; shardId < NodeShards_.size(); ++shardId) {
            auto asyncResult = BIND(&TNodeShard::ResetOperationRevival, NodeShards_[shardId])
                .AsyncVia(NodeShards_[shardId]->GetInvoker())
                .Run(operation->GetId());
            asyncResults.emplace_back(std::move(asyncResult));
        }
        return AllSucceeded(asyncResults);
    }

    TFuture<void> RegisterJobsFromRevivedOperation(const TOperationPtr& operation)
    {
        auto jobs = std::move(operation->RevivedJobs());
        YT_LOG_INFO("Registering running jobs from the revived operation (OperationId: %v, JobCount: %v)",
            operation->GetId(),
            jobs.size());

        if (auto delay = operation->Spec()->TestingOperationOptions->DelayInsideRegisterJobsFromRevivedOperation) {
            TDelayedExecutor::WaitForDuration(*delay);
        }

        // First, unfreeze operation and register jobs in strategy. Do this synchronously as we are in the scheduler control thread.
        Strategy_->RegisterJobsFromRevivedOperation(operation->GetId(), jobs);

        // Second, register jobs on the corresponding node shards.
        std::vector<std::vector<TJobPtr>> jobsByShardId(NodeShards_.size());
        for (auto& job : jobs) {
            auto shardId = GetNodeShardId(NodeIdFromJobId(job->GetId()));
            jobsByShardId[shardId].push_back(std::move(job));
        }

        std::vector<TFuture<void>> asyncResults;
        for (int shardId = 0; shardId < NodeShards_.size(); ++shardId) {
            auto asyncResult = BIND(&TNodeShard::FinishOperationRevival, NodeShards_[shardId])
                .AsyncVia(NodeShards_[shardId]->GetInvoker())
                .Run(operation->GetId(), std::move(jobsByShardId[shardId]));
            asyncResults.emplace_back(std::move(asyncResult));
        }
        return AllSucceeded(asyncResults);
    }

    void BuildOperationOrchid(const TOperationPtr& operation, IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto agent = operation->FindAgent();

        BuildYsonFluently(consumer)
            .BeginMap()
                .Do(BIND(&NScheduler::BuildFullOperationAttributes, operation))
                .DoIf(static_cast<bool>(agent), [&] (TFluentMap fluent) {
                    fluent
                        .Item("agent_id").Value(agent->GetId());
                })
                .OptionalItem("alias", operation->Alias())
                .Item("progress").BeginMap()
                    .Do(BIND(&ISchedulerStrategy::BuildOperationProgress, Strategy_, operation->GetId()))
                .EndMap()
                .Item("brief_progress").BeginMap()
                    .Do(BIND(&ISchedulerStrategy::BuildBriefOperationProgress, Strategy_, operation->GetId()))
                .EndMap()
            .EndMap();
    }

    IYPathServicePtr CreateOperationOrchidService(const TOperationPtr& operation)
    {
        auto operationOrchidProducer = BIND(&TImpl::BuildOperationOrchid, MakeStrong(this), operation);
        return IYPathService::FromProducer(operationOrchidProducer)
            ->Via(GetControlInvoker(EControlQueue::Orchid));
    }

    void RegisterOperationAlias(const TOperationPtr& operation)
    {
        YT_VERIFY(operation->Alias());

        TOperationAlias alias{operation->GetId(), operation};
        auto it = OperationAliases_.find(*operation->Alias());
        if (it != OperationAliases_.end()) {
            if (it->second.Operation) {
                THROW_ERROR_EXCEPTION("Operation alias is already used by an operation")
                    << TErrorAttribute("operation_alias", operation->Alias())
                    << TErrorAttribute("operation_id", it->second.OperationId);
            }
            YT_LOG_DEBUG("Assigning an already existing alias to a new operation (Alias: %v, OldOperationId: %v, NewOperationId: %v)",
                *operation->Alias(),
                it->second.OperationId,
                operation->GetId());
            it->second = std::move(alias);
        } else {
            YT_LOG_DEBUG("Assigning a new alias to a new operation (Alias: %v, OperationId: %v)",
                *operation->Alias(),
                operation->GetId());
            OperationAliases_[*operation->Alias()] = std::move(alias);
        }
    }

    void RegisterOperation(const TOperationPtr& operation, bool jobsReady)
    {
        YT_VERIFY(operation->GetState() == EOperationState::Starting || operation->GetState() == EOperationState::Orphaned);
        YT_VERIFY(IdToOperation_.emplace(operation->GetId(), operation).second);

        const auto& agentTracker = Bootstrap_->GetControllerAgentTracker();
        auto controller = agentTracker->CreateController(operation);
        operation->SetController(controller);

        std::vector<TString> unknownTreeIds;
        Strategy_->RegisterOperation(operation.Get(), &unknownTreeIds);
        operation->EraseTrees(unknownTreeIds);
        YT_LOG_DEBUG_UNLESS(
            unknownTreeIds.empty(),
            "Operation has unknown pool trees after registration (OperationId: %v, TreeIds: %v)",
            operation->GetId(),
            unknownTreeIds);

        operation->PoolTreeControllerSettingsMap() = Strategy_->GetOperationPoolTreeControllerSettingsMap(operation->GetId());

        for (const auto& nodeShard : NodeShards_) {
            nodeShard->GetInvoker()->Invoke(BIND(
                &TNodeShard::RegisterOperation,
                nodeShard,
                operation->GetId(),
                operation->GetController(),
                jobsReady));
        }

        MasterConnector_->RegisterOperation(operation);

        auto service = CreateOperationOrchidService(operation);
        YT_VERIFY(IdToOperationService_.emplace(operation->GetId(), service).second);

        YT_LOG_DEBUG("Operation registered (OperationId: %v, OperationAlias: %v, JobsReady: %v)",
            operation->GetId(),
            operation->Alias(),
            jobsReady);
    }

    void RegisterAssignedOperation(const TOperationPtr& operation)
    {
        auto agent = operation->GetAgentOrCancelFiber();
        const auto& controller = operation->GetController();
        controller->AssignAgent(agent);

        const auto& agentTracker = Bootstrap_->GetControllerAgentTracker();
        WaitFor(agentTracker->RegisterOperationAtAgent(operation))
            .ThrowOnError();
    }

    void UnregisterOperation(const TOperationPtr& operation)
    {
        YT_VERIFY(IdToOperation_.erase(operation->GetId()) == 1);
        YT_VERIFY(IdToOperationService_.erase(operation->GetId()) == 1);
        if (operation->Alias()) {
            auto& alias = GetOrCrash(OperationAliases_, *operation->Alias());
            YT_LOG_DEBUG("Alias now corresponds to an unregistered operation (Alias: %v, OperationId: %v)",
                *operation->Alias(),
                operation->GetId());
            YT_VERIFY(alias.Operation == operation);
            alias.Operation = nullptr;
        }

        const auto& controller = operation->GetController();
        if (controller) {
            controller->RevokeAgent();
        }
        operation->SetController(nullptr);

        for (const auto& nodeShard : NodeShards_) {
            nodeShard->GetInvoker()->Invoke(BIND(
                &TNodeShard::UnregisterOperation,
                nodeShard,
                operation->GetId()));
        }

        Strategy_->UnregisterOperation(operation.Get());

        const auto& agentTracker = Bootstrap_->GetControllerAgentTracker();
        agentTracker->UnregisterOperationFromAgent(operation);

        MasterConnector_->UnregisterOperation(operation);

        YT_LOG_DEBUG("Operation unregistered (OperationId: %v)",
            operation->GetId());
    }

    void AbortOperationJobs(const TOperationPtr& operation, const TError& error, bool terminated)
    {
        std::vector<TFuture<void>> abortFutures;
        for (const auto& nodeShard : NodeShards_) {
            abortFutures.push_back(BIND(&TNodeShard::AbortOperationJobs, nodeShard)
                .AsyncVia(nodeShard->GetInvoker())
                .Run(operation->GetId(), error, terminated));
        }

        WaitFor(AllSucceeded(abortFutures))
            .ThrowOnError();

        YT_LOG_DEBUG("Requested node shards to abort all operation jobs (OperationId: %v)",
            operation->GetId());
    }

    void BuildOperationInfoForEventLog(const TOperationPtr& operation, TFluentMap fluent)
    {
        fluent
            .Item("operation_id").Value(operation->GetId())
            .Item("operation_type").Value(operation->GetType())
            .Item("spec").Value(operation->GetSpecString())
            .Item("authenticated_user").Value(operation->GetAuthenticatedUser());
    }

    void SetOperationFinalState(const TOperationPtr& operation, EOperationState state, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto truncatedError = error.Truncate();

        if (!operation->GetStarted().IsSet()) {
            operation->SetStarted(truncatedError);
        }
        operation->SetStateAndEnqueueEvent(state);
        operation->SetFinishTime(TInstant::Now());
        ToProto(operation->MutableResult().mutable_error(), truncatedError);
    }

    void FinishOperation(const TOperationPtr& operation)
    {
        if (!operation->GetFinished().IsSet()) {
            operation->SetFinished();
            UnregisterOperation(operation);
        }
        operation->Cancel(TError("Operation finished"));
        OperationsToDestroy_.push_back(operation);
    }

    void ProcessUnregisterOperationResult(
        const TOperationPtr& operation,
        const TOperationControllerUnregisterResult& result) const
    {
        if (!result.ResidualJobMetrics.empty()) {
            GetStrategy()->ApplyJobMetricsDelta({{operation->GetId(), result.ResidualJobMetrics}});
        }
    }

    void DoCompleteOperation(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (operation->IsFinishedState() || operation->IsFinishingState()) {
            // Operation is probably being aborted.
            return;
        }

        auto codicilGuard = operation->MakeCodicilGuard();

        auto operationId = operation->GetId();
        YT_LOG_INFO("Completing operation (OperationId: %v)",
            operationId);

        operation->SetStateAndEnqueueEvent(EOperationState::Completing);
        operation->SetSuspended(false);

        // The operation may still have running jobs (e.g. those started speculatively).
        AbortOperationJobs(operation, TError("Operation completed"), /* terminated */ true);

        TOperationProgress operationProgress;
        try {
            // First flush: ensure that all stderrs are attached and the
            // state is changed to Completing.
            {
                auto asyncResult = MasterConnector_->FlushOperationNode(operation);
                // Result is ignored since failure causes scheduler disconnection.
                Y_UNUSED(WaitFor(asyncResult));
                ValidateOperationState(operation, EOperationState::Completing);
            }

            // Should be called before commit in controller.
            operationProgress = WaitFor(BIND(&TImpl::RequestOperationProgress, MakeStrong(this), operation)
                .AsyncVia(operation->GetCancelableControlInvoker())
                .Run())
                .ValueOrThrow();

            ValidateOperationState(operation, EOperationState::Completing);

            {
                const auto& controller = operation->GetController();
                WaitFor(controller->Commit())
                    .ThrowOnError();

                ValidateOperationState(operation, EOperationState::Completing);

                if (Config_->TestingOptions->FinishOperationTransitionDelay) {
                    Sleep(*Config_->TestingOptions->FinishOperationTransitionDelay);
                }
            }

            YT_VERIFY(operation->GetState() == EOperationState::Completing);
            SetOperationFinalState(operation, EOperationState::Completed, TError());

            SubmitOperationToCleaner(operation, operationProgress);

            // Second flush: ensure that state is changed to Completed.
            {
                auto asyncResult = MasterConnector_->FlushOperationNode(operation);
                WaitFor(asyncResult)
                    .ThrowOnError();
                YT_VERIFY(operation->GetState() == EOperationState::Completed);
            }

            // Notify controller that it is going to be disposed.
            {
                const auto& controller = operation->GetController();
                auto resultOrError = WaitFor(controller->Unregister());
                if (resultOrError.IsOK()) {
                    ProcessUnregisterOperationResult(operation, resultOrError.Value());
                }
            }

            FinishOperation(operation);
        } catch (const std::exception& ex) {
            OnOperationFailed(operation, ex);
            return;
        }

        YT_LOG_INFO("Operation completed (OperationId: %v)",
             operationId);

        LogOperationFinished(
            operation,
            ELogEventType::OperationCompleted,
            TError(),
            operationProgress.Progress,
            operationProgress.Alerts);
    }

    void DoFailOperation(
        const TOperationPtr& operation,
        const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // NB: finishing state is ok, do not skip operation fail in this case.
        if (operation->IsFinishedState()) {
            // Operation is already terminated.
            return;
        }

        auto codicilGuard = operation->MakeCodicilGuard();

        YT_LOG_INFO(error, "Operation failed (OperationId: %v)",
             operation->GetId());

        TerminateOperation(
            operation,
            EOperationState::Failing,
            EOperationState::Failed,
            ELogEventType::OperationFailed,
            error);
    }

    void DoAbortOperation(const TOperationPtr& operation, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // NB: finishing state is ok, do not skip operation abort in this case.
        if (operation->IsFinishedState()) {
            // Operation is already terminated.
            return;
        }

        auto codicilGuard = operation->MakeCodicilGuard();

        YT_LOG_INFO(error, "Aborting operation (OperationId: %v, State: %v)",
            operation->GetId(),
            operation->GetState());

        if (operation->Spec()->TestingOperationOptions->DelayInsideAbort) {
            TDelayedExecutor::WaitForDuration(*operation->Spec()->TestingOperationOptions->DelayInsideAbort);
        }

        TerminateOperation(
            operation,
            EOperationState::Aborting,
            EOperationState::Aborted,
            ELogEventType::OperationAborted,
            error);
    }

    void DoSuspendOperation(
        const TOperationPtr& operation,
        const TError& error,
        bool abortRunningJobs,
        bool setAlert)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // NB: finishing state is ok, do not skip operation fail in this case.
        if (operation->IsFinishedState()) {
            // Operation is already terminated.
            return;
        }

        auto codicilGuard = operation->MakeCodicilGuard();

        operation->SetSuspended(true);

        if (abortRunningJobs) {
            AbortOperationJobs(operation, error, /* terminated */ false);
        }

        if (setAlert) {
            operation->SetAlert(EOperationAlertType::OperationSuspended, error);
        }

        YT_LOG_INFO(error, "Operation suspended (OperationId: %v)",
            operation->GetId());
    }

    TOperationProgress RequestOperationProgress(const TOperationPtr& operation) const
    {
        auto agent = operation->FindAgent();

        if (agent) {
            NControllerAgent::TControllerAgentServiceProxy proxy(agent->GetChannel());
            auto req = proxy.GetOperationInfo();
            req->SetTimeout(Config_->ControllerAgentTracker->LightRpcTimeout);
            ToProto(req->mutable_operation_id(), operation->GetId());
            auto rspOrError = WaitFor(req->Invoke());
            if (rspOrError.IsOK()) {
                auto rsp = rspOrError.Value();
                TOperationProgress result;
                // TODO(asaitgalin): Can we build map in controller instead of map fragment?
                result.Progress = BuildYsonStringFluently()
                    .BeginMap()
                        .Items(TYsonString(rsp->progress(), EYsonType::MapFragment))
                    .EndMap();
                result.BriefProgress = BuildYsonStringFluently()
                    .BeginMap()
                        .Items(TYsonString(rsp->brief_progress(), EYsonType::MapFragment))
                    .EndMap();
                result.Alerts = BuildYsonStringFluently()
                    .BeginMap()
                        .Items(TYsonString(rsp->alerts(), EYsonType::MapFragment))
                    .EndMap();
                return result;
            } else {
                YT_LOG_INFO(rspOrError, "Failed to get operation info from controller agent (OperationId: %v)",
                    operation->GetId());
            }
        }

        // If we failed to get progress from controller then we try to fetch it from Cypress.
        {
            auto attributesOrError = WaitFor(MasterConnector_->GetOperationNodeProgressAttributes(operation));
            if (attributesOrError.IsOK()) {
                auto attributes = ConvertToAttributes(attributesOrError.Value());

                TOperationProgress result;
                result.Progress = attributes->FindYson("progress");
                result.BriefProgress = attributes->FindYson("brief_progress");
                result.Alerts = attributes->FindYson("alerts");
                return result;
            } else {
                YT_LOG_INFO(attributesOrError, "Failed to get operation progress from Cypress (OperationId: %v)",
                    operation->GetId());
            }
        }

        return TOperationProgress();
    }

    void SubmitOperationToCleaner(
        const TOperationPtr& operation,
        const TOperationProgress& operationProgress) const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        TArchiveOperationRequest archivationReq;
        archivationReq.InitializeFromOperation(operation);
        archivationReq.Progress = operationProgress.Progress;
        archivationReq.BriefProgress = operationProgress.BriefProgress;
        archivationReq.Alerts = operationProgress.Alerts;

        OperationsCleaner_->SubmitForArchivation(std::move(archivationReq));
    }

    void TerminateOperation(
        const TOperationPtr& operation,
        EOperationState intermediateState,
        EOperationState finalState,
        ELogEventType logEventType,
        const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto initialState = operation->GetState();
        if (IsOperationFinished(initialState) ||
            initialState == EOperationState::Failing ||
            initialState == EOperationState::Aborting)
        {
            // Safe to call multiple times, just ignore it.
            return;
        }

        operation->SetStateAndEnqueueEvent(intermediateState);
        operation->SetSuspended(false);

        AbortOperationJobs(
            operation,
            TError("Operation terminated")
                << TErrorAttribute("state", initialState)
                << error,
            /* terminated */ true);

        // First flush: ensure that all stderrs are attached and the
        // state is changed to its intermediate value.
        {
            // Result is ignored since failure causes scheduler disconnection.
            Y_UNUSED(WaitFor(MasterConnector_->FlushOperationNode(operation)));
            if (operation->GetState() != intermediateState) {
                return;
            }
        }


        if (Config_->TestingOptions->FinishOperationTransitionDelay) {
            Sleep(*Config_->TestingOptions->FinishOperationTransitionDelay);
        }

        auto operationProgress = WaitFor(BIND(&TImpl::RequestOperationProgress, MakeStrong(this), operation)
            .AsyncVia(operation->GetCancelableControlInvoker())
            .Run())
            .ValueOrThrow();

        if (const auto& controller = operation->GetController()) {
            try {
                WaitFor(controller->Terminate(finalState))
                    .ThrowOnError();
            } catch (const std::exception& ex) {
                auto error = TError("Failed to abort controller of operation %v",
                    operation->GetId())
                    << ex;
                MasterConnector_->Disconnect(error);
                return;
            }
        }

        bool owningTransactions =
            initialState == EOperationState::WaitingForAgent ||
            initialState == EOperationState::Orphaned ||
            initialState == EOperationState::Initializing ||
            initialState == EOperationState::ReviveInitializing;
        if (owningTransactions && operation->Transactions()) {
            std::vector<TFuture<void>> asyncResults;
            THashSet<ITransactionPtr> abortedTransactions;
            auto scheduleAbort = [&] (const ITransactionPtr& transaction, TString transactionType) {
                if (abortedTransactions.contains(transaction)) {
                    return;
                }

                if (transaction) {
                    YT_LOG_DEBUG("Aborting transaction %v (Type: %v, OperationId: %v)",
                        transaction->GetId(),
                        transactionType,
                        operation->GetId());
                    YT_VERIFY(abortedTransactions.emplace(transaction).second);
                    asyncResults.push_back(transaction->Abort());
                } else {
                    YT_LOG_DEBUG("Transaction missed, skipping abort (Type: %v, OperationId: %v)",
                        transactionType,
                        operation->GetId());
                }
            };

            const auto& transactions = *operation->Transactions();
            scheduleAbort(transactions.AsyncTransaction, "Async");
            scheduleAbort(transactions.InputTransaction, "Input");
            scheduleAbort(transactions.OutputTransaction, "Output");
            scheduleAbort(transactions.DebugTransaction, "Debug");
            for (const auto& transaction : transactions.NestedInputTransactions) {
                scheduleAbort(transaction, "NestedInput");
            }

            try {
                WaitFor(AllSucceeded(asyncResults))
                    .ThrowOnError();
            } catch (const std::exception& ex) {
                YT_LOG_DEBUG(ex, "Failed to abort transactions of orphaned operation (OperationId: %v)",
                    operation->GetId());
            }
        } else {
            YT_LOG_DEBUG("Skipping transactions abort (OperationId: %v, InitialState: %v, HasTransaction: %v)",
                operation->GetId(),
                initialState,
                static_cast<bool>(operation->Transactions()));
        }

        SetOperationFinalState(operation, finalState, error);

        // Second flush: ensure that the state is changed to its final value.
        {
            // Result is ignored since failure causes scheduler disconnection.
            Y_UNUSED(WaitFor(MasterConnector_->FlushOperationNode(operation)));
            if (operation->GetState() != finalState) {
                return;
            }
        }

        SubmitOperationToCleaner(operation, operationProgress);

        if (const auto& controller = operation->GetController()) {
            // Notify controller that it is going to be disposed.
            auto resultOrError = WaitFor(controller->Unregister());
            if (resultOrError.IsOK()) {
                ProcessUnregisterOperationResult(operation, resultOrError.Value());
            }
        }

        LogOperationFinished(
            operation,
            logEventType,
            error,
            operationProgress.Progress,
            operationProgress.Alerts);

        FinishOperation(operation);
    }


    void CompleteOperationWithoutRevival(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        YT_LOG_INFO("Completing operation without revival (OperationId: %v)",
             operation->GetId());

        if (operation->RevivalDescriptor()->ShouldCommitOutputTransaction) {
            WaitFor(operation->Transactions()->OutputTransaction->Commit())
                .ThrowOnError();
            // We don't know whether debug transaction is committed.
            if (operation->Transactions()->DebugTransaction) {
                Y_UNUSED(operation->Transactions()->DebugTransaction->Commit());
            }
            for (auto transaction : {operation->Transactions()->InputTransaction, operation->Transactions()->AsyncTransaction}) {
                if (transaction) {
                    Y_UNUSED(transaction->Abort());
                }
            }
        }

        SetOperationFinalState(operation, EOperationState::Completed, TError());

        // Result is ignored since failure causes scheduler disconnection.
        Y_UNUSED(WaitFor(MasterConnector_->FlushOperationNode(operation)));

        auto result = WaitFor(BIND(&TImpl::RequestOperationProgress, MakeStrong(this), operation)
            .AsyncVia(operation->GetCancelableControlInvoker())
            .Run());
        auto progress = result.IsOK()
            ? result.Value().Progress
            : TYsonString();
        auto alerts = result.IsOK()
            ? result.Value().Alerts
            : TYsonString();

        LogOperationFinished(
            operation,
            ELogEventType::OperationCompleted,
            TError(),
            progress,
            alerts);

        FinishOperation(operation);
    }

    void AbortOperationWithoutRevival(const TOperationPtr& operation, const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto codicilGuard = operation->MakeCodicilGuard();

        YT_LOG_INFO(error, "Aborting operation without revival (OperationId: %v)",
             operation->GetId());

        THashSet<ITransactionPtr> abortedTransactions;
        auto abortTransaction = [&] (ITransactionPtr transaction, const TString& type) {
            if (abortedTransactions.contains(transaction)) {
                return;
            }

            if (transaction) {
                YT_LOG_DEBUG("Aborting transaction %v (Type: %v, OperationId: %v)", transaction->GetId(), type, operation->GetId());
                // Fire-and-forget.
                transaction->Abort();
                YT_VERIFY(abortedTransactions.emplace(transaction).second);
            } else {
                YT_LOG_DEBUG("Transaction is missing, skipping abort (Type: %v, OperationId: %v)", type, operation->GetId());
            }
        };

        const auto& transactions = *operation->Transactions();
        abortTransaction(transactions.InputTransaction, "Input");
        for (const auto& transaction : transactions.NestedInputTransactions) {
            abortTransaction(transaction, "NestedInput");
        }
        abortTransaction(transactions.OutputTransaction, "Output");
        abortTransaction(transactions.AsyncTransaction, "Async");
        abortTransaction(transactions.DebugTransaction, "Debug");

        SetOperationFinalState(operation, EOperationState::Aborted, error);

        // Result is ignored since failure causes scheduler disconnection.
        Y_UNUSED(WaitFor(MasterConnector_->FlushOperationNode(operation)));

        auto result = WaitFor(BIND(&TImpl::RequestOperationProgress, MakeStrong(this), operation)
            .AsyncVia(operation->GetCancelableControlInvoker())
            .Run());
        auto progress = result.IsOK()
            ? result.Value().Progress
            : TYsonString();
        auto alerts = result.IsOK()
            ? result.Value().Alerts
            : TYsonString();

        LogOperationFinished(
            operation,
            ELogEventType::OperationAborted,
            error,
            progress,
            alerts);

        FinishOperation(operation);
    }

    void RemoveExpiredResourceLimitsTags()
    {
        std::vector<TSchedulingTagFilter> toRemove;
        for (const auto& [filter, record] : CachedResourceLimitsByTags_) {
            if (record.first + DurationToCpuDuration(Config_->SchedulingTagFilterExpireTimeout) < GetCpuInstant()) {
                toRemove.push_back(filter);
            }
        }

        for (const auto& filter : toRemove) {
            YT_VERIFY(CachedResourceLimitsByTags_.erase(filter) == 1);
        }
    }

    TJobResources GetResourceUsage(const TSchedulingTagFilter& filter)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        TJobResources resourceUsage;
        for (const auto& nodeShard : NodeShards_) {
            resourceUsage += nodeShard->GetResourceUsage(filter);
        }

        return resourceUsage;
    }

    TYsonString BuildSuspiciousJobsYson()
    {
        TStringBuilder builder;
        for (const auto& [operationId, operation] : IdToOperation_) {
            builder.AppendString(operation->GetSuspiciousJobs().GetData());
        }
        return TYsonString(builder.Flush(), EYsonType::MapFragment);
    }

    void BuildStaticOrchid(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        RemoveExpiredResourceLimitsTags();

        BuildYsonFluently(consumer)
            .BeginMap()
                // COMPAT(babenko): deprecate cell in favor of cluster
                .Item("cell").BeginMap()
                    .Item("resource_limits").Value(GetResourceLimits(EmptySchedulingTagFilter))
                    .Item("resource_usage").Value(GetResourceUsage(EmptySchedulingTagFilter))
                    .Item("exec_node_count").Value(GetExecNodeCount())
                    .Item("total_node_count").Value(GetTotalNodeCount())
                    .Item("nodes_memory_distribution").Value(GetExecNodeMemoryDistribution(TSchedulingTagFilter()))
                    .Item("resource_limits_by_tags")
                        .DoMapFor(CachedResourceLimitsByTags_, [] (TFluentMap fluent, const auto& pair) {
                            const auto& [filter, record] = pair;
                            if (!filter.IsEmpty()) {
                                fluent.Item(filter.GetBooleanFormula().GetFormula()).Value(record.second);
                            }
                        })
                .EndMap()
                .Item("cluster").BeginMap()
                    .Item("resource_limits").Value(GetResourceLimits(EmptySchedulingTagFilter))
                    .Item("resource_usage").Value(GetResourceUsage(EmptySchedulingTagFilter))
                    .Item("exec_node_count").Value(GetExecNodeCount())
                    .Item("total_node_count").Value(GetTotalNodeCount())
                    .Item("nodes_memory_distribution").Value(GetExecNodeMemoryDistribution(TSchedulingTagFilter()))
                    .Item("resource_limits_by_tags")
                        .DoMapFor(CachedResourceLimitsByTags_, [] (TFluentMap fluent, const auto& pair) {
                            const auto& [filter, record] = pair;
                            if (!filter.IsEmpty()) {
                                fluent.Item(filter.GetBooleanFormula().GetFormula()).Value(record.second);
                            }
                        })
                    .Item("medium_directory").Value(
                        Bootstrap_
                        ->GetMasterClient()
                        ->GetNativeConnection()
                        ->GetMediumDirectory()
                    )
                .EndMap()
                .Item("suspicious_jobs").BeginMap()
                    .Items(BuildSuspiciousJobsYson())
                .EndMap()
                .Item("nodes").BeginMap()
                    .Do([=] (TFluentMap fluent) {
                        for (const auto& nodeShard : NodeShards_) {
                            auto asyncResult = WaitFor(
                                BIND(&TNodeShard::BuildNodesYson, nodeShard, fluent)
                                    .AsyncVia(nodeShard->GetInvoker())
                                    .Run());
                            asyncResult.ThrowOnError();
                        }
                    })
                .EndMap()
                .Do(std::bind(&ISchedulerStrategy::BuildOrchid, Strategy_, _1))
            .EndMap();
    }

    void BuildLightStaticOrchid(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        BuildYsonFluently(consumer)
            .BeginMap()
                // Deprecated.
                .Item("connected").Value(IsConnected())
                .Item("controller_agents").DoMapFor(Bootstrap_->GetControllerAgentTracker()->GetAgents(), [] (TFluentMap fluent, const auto& agent) {
                    fluent
                        .Item(agent->GetId()).BeginMap()
                            .Item("state").Value(agent->GetState())
                            .DoIf(agent->GetState() == EControllerAgentState::Registered, [&] (TFluentMap fluent) {
                                fluent.Item("incarnation_id").Value(agent->GetIncarnationId());
                            })
                            .Item("operation_ids").DoListFor(agent->Operations(), [] (TFluentList fluent, const auto& operation) {
                                fluent.Item().Value(operation->GetId());
                            })
                        .EndMap();
                })
                .Item("config").Value(Config_)
                .Item("config_revision").Value(ConfigRevision_)
                .Item("operations_cleaner").BeginMap()
                    .Do(std::bind(&TOperationsCleaner::BuildOrchid, OperationsCleaner_, _1))
                .EndMap()
                .Item("operation_base_acl").Value(OperationBaseAcl_)
                .Item("service").BeginMap()
                    // This information used by scheduler_uptime odin check and we want
                    // to receive all these fields by single request.
                    .Item("connected").Value(IsConnected())
                    .Item("last_connection_time").Value(GetConnectionTime())
                    .Item("build_version").Value(GetVersion())
                    .Item("hostname").Value(GetDefaultAddress(Bootstrap_->GetLocalAddresses()))
                .EndMap()
            .EndMap();
    }

    IYPathServicePtr GetDynamicOrchidService()
    {
        auto dynamicOrchidService = New<TCompositeMapService>();
        dynamicOrchidService->AddChild("operations", New<TOperationsService>(this));
        dynamicOrchidService->AddChild("jobs", New<TJobsService>(this));
        return dynamicOrchidService;
    }

    void ValidateConfig()
    {
        // First reset the alert.
        SetSchedulerAlert(ESchedulerAlertType::UnrecognizedConfigOptions, TError());

        if (!Config_->EnableUnrecognizedAlert) {
            return;
        }

        auto unrecognized = Config_->GetUnrecognizedRecursively();
        if (unrecognized && unrecognized->GetChildCount() > 0) {
            YT_LOG_WARNING("Scheduler config contains unrecognized options (Unrecognized: %v)",
                ConvertToYsonString(unrecognized, EYsonFormat::Text));
            SetSchedulerAlert(
                ESchedulerAlertType::UnrecognizedConfigOptions,
                TError("Scheduler config contains unrecognized options")
                    << TErrorAttribute("unrecognized", unrecognized));
        }
    }


    void AddOperationToTransientQueue(const TOperationPtr& operation)
    {
        StateToTransientOperations_[operation->GetState()].push_back(operation);

        if (TransientOperationQueueScanPeriodExecutor_) {
            TransientOperationQueueScanPeriodExecutor_->ScheduleOutOfBand();
        }

        YT_LOG_DEBUG("Operation added to transient queue (OperationId: %v, State: %v)",
            operation->GetId(),
            operation->GetState());
    }

    bool HandleWaitingForAgentOperation(const TOperationPtr& operation)
    {
        const auto& agentTracker = Bootstrap_->GetControllerAgentTracker();
        auto agent = agentTracker->PickAgentForOperation(operation);
        if (!agent) {
            YT_LOG_DEBUG("Failed to assign operation to agent; backing off (OperationId: %v)", operation->GetId());
            OperationToAgentAssignmentFailureTime_ = TInstant::Now();
            return false;
        }

        agentTracker->AssignOperationToAgent(operation, agent);

        THashMap<TString, TString> eventAttributes = {
            {"controller_agent_address", GetDefaultAddress(agent->GetAgentAddresses())},
        };

        if (operation->RevivalDescriptor()) {
            operation->SetStateAndEnqueueEvent(EOperationState::ReviveInitializing, eventAttributes);
            operation->GetCancelableControlInvoker()->Invoke(
                BIND(&TImpl::DoReviveOperation, MakeStrong(this), operation));
        } else {
            operation->SetStateAndEnqueueEvent(EOperationState::Initializing, eventAttributes);
            operation->GetCancelableControlInvoker()->Invoke(
                BIND(&TImpl::DoInitializeOperation, MakeStrong(this), operation));
        }

        return true;
    }

    void HandleOrphanedOperation(const TOperationPtr& operation)
    {
        auto operationId = operation->GetId();

        auto codicilGuard = operation->MakeCodicilGuard();

        YT_LOG_DEBUG("Handling orphaned operation (OperationId: %v)",
            operation->GetId());

        try {
            ValidateOperationState(operation, EOperationState::Orphaned);

            YT_VERIFY(operation->RevivalDescriptor());
            const auto& revivalDescriptor = *operation->RevivalDescriptor();

            if (revivalDescriptor.OperationCommitted) {
                CompleteOperationWithoutRevival(operation);
                return;
            }

            if (revivalDescriptor.OperationAborting) {
                AbortOperationWithoutRevival(
                    operation,
                    TError("Operation aborted since it was found in \"aborting\" state during scheduler revival"));
                return;
            }

            if (operation->GetRuntimeParameters()->SchedulingOptionsPerPoolTree.empty()) {
                AbortOperationWithoutRevival(
                    operation,
                    TError("Operation aborted since it has no active trees after revival"));
                return;
            }

            if (revivalDescriptor.UserTransactionAborted) {
                AbortOperationWithoutRevival(
                    operation,
                    GetUserTransactionAbortedError(operation->GetUserTransactionId()));
                return;
            }

            WaitFor(Strategy_->ValidateOperationStart(operation.Get()))
                .ThrowOnError();

            ValidateOperationState(operation, EOperationState::Orphaned);

            operation->SetStateAndEnqueueEvent(EOperationState::WaitingForAgent);
            AddOperationToTransientQueue(operation);
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Operation has failed to revive (OperationId: %v)",
                operationId);
            auto wrappedError = TError("Operation has failed to revive")
                << ex;
            OnOperationFailed(operation, wrappedError);
        }
    }

    void HandleOrphanedOperations()
    {
        auto& queuedOperations = StateToTransientOperations_[EOperationState::Orphaned];
        std::vector<TOperationPtr> operations;
        operations.reserve(queuedOperations.size());
        for (const auto& operation : queuedOperations) {
            if (operation->GetState() != EOperationState::Orphaned) {
                YT_LOG_DEBUG("Operation is no longer orphaned (OperationId: %v, State: %v)",
                    operation->GetId(),
                    operation->GetState());
                continue;
            }
            operations.push_back(operation);
        }
        queuedOperations.clear();

        if (operations.empty()) {
            return;
        }

        auto result = WaitFor(MasterConnector_->FetchOperationRevivalDescriptors(operations));
        if (!result.IsOK()) {
            YT_LOG_ERROR(result, "Error fetching revival descriptors");
            MasterConnector_->Disconnect(result);
            return;
        }

        for (const auto& operation : operations) {
            operation->GetCancelableControlInvoker()->Invoke(
                BIND(&TImpl::HandleOrphanedOperation, MakeStrong(this), operation));
        }
    }

    void ScanPendingOperations()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_LOG_DEBUG("Started scanning pending operations");

        Strategy_->ScanPendingOperations();

        YT_LOG_DEBUG("Finished scanning pending operations");
    }

    void ScanTransientOperationQueue()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_LOG_DEBUG("Started scanning transient operation queue");

        if (TInstant::Now() > OperationToAgentAssignmentFailureTime_ + Config_->OperationToAgentAssignmentBackoff) {
            int scannedOperationCount = 0;

            auto& queuedOperations = StateToTransientOperations_[EOperationState::WaitingForAgent];
            std::vector<TOperationPtr> newQueuedOperations;
            for (const auto& operation : queuedOperations) {
                if (operation->GetState() != EOperationState::WaitingForAgent) {
                    YT_LOG_DEBUG("Operation is no longer waiting for agent (OperationId: %v, State: %v)",
                        operation->GetId(),
                        operation->GetState());
                    continue;
                }
                ++scannedOperationCount;
                if (!HandleWaitingForAgentOperation(operation)) {
                    newQueuedOperations.push_back(operation);
                }
            }
            queuedOperations = std::move(newQueuedOperations);

            YT_LOG_DEBUG("Waiting for agent operations handled (OperationCount: %v)", scannedOperationCount);
        }

        HandleOrphanedOperations();

        YT_LOG_DEBUG("Finished scanning transient operation queue");
    }

    void OnOperationsArchived(const std::vector<TArchiveOperationRequest>& archivedOperationRequests)
    {
        for (const auto& request : archivedOperationRequests) {
            if (request.Alias) {
                // NB: some other operation could have already used this alias (and even be removed after they completed),
                // so we check if it is still assigned to an operation id we expect.
                auto it = OperationAliases_.find(*request.Alias);
                if (it == OperationAliases_.end()) {
                    // This case may happen due to reordering of removal requests inside operation cleaner
                    // (e.g. some of the removal requests may fail due to lock conflict).
                    YT_LOG_DEBUG("Operation alias has already been removed (Alias: %v, OperationId: %v)",
                        request.Alias,
                        request.Id);
                } else if (it->second.OperationId == request.Id) {
                    // We should have already dropped the pointer to the operation. Let's assert that.
                    YT_VERIFY(!it->second.Operation);
                    YT_LOG_DEBUG("Operation alias is still assigned to an operation, removing it (Alias: %v, OperationId: %v)",
                        request.Alias,
                        request.Id);
                    OperationAliases_.erase(it);
                } else {
                    YT_LOG_DEBUG("Operation alias was reused by another operation, doing nothing "
                        "(Alias: %v, OldOperationId: %v, NewOperationId: %v)",
                        request.Alias,
                        request.Id,
                        it->second.OperationId);
                }
            }
        }
    }

    void PostOperationsToDestroy()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        Y_UNUSED(WaitFor(BIND(&TImpl::TryDestroyOperations, MakeStrong(this), Passed(std::move(OperationsToDestroy_)))
            .AsyncVia(TDispatcher::Get()->GetHeavyInvoker())
            .Run()));
    }

    void TryDestroyOperations(std::vector<TOperationPtr>&& operations)
    {
        for (auto& operation : operations) {
            if (operation->GetRefCount() == 1) {
                YT_LOG_DEBUG("Destroying operation (OperationId: %v)", operation->GetId());
            } else {
                YT_LOG_DEBUG(
                    "Operation is still in use and will be destroyed later (OperationId: %v, ResidualRefCount: %v)",
                    operation->GetId(),
                    operation->GetRefCount() - 1);
            }
            operation.Reset();
        }
    }

    void ManageSchedulingSegments()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!IsConnected()) {
            return;
        }

        PersistOperationSchedulingSegmentDataCenters();

        ManageNodeSchedulingSegments();
    }

    // TODO(eshcherbin): Think about storing data center in runtime parameters only.
    // Current implementation has a lag between operation data center assignment and persisting this
    // decision by updating runtime parameters at the master. This lag is acceptable and is left for
    // implementation simplicity and code readability purposes.
    void PersistOperationSchedulingSegmentDataCenters()
    {
        auto updatesPerTree = Strategy_->GetOperationSchedulingSegmentDataCenterUpdates();

        {
            THashMap<TString, int> updateCountPerTree(updatesPerTree.size());
            for (const auto& [treeId, updates] : updatesPerTree) {
                updateCountPerTree.emplace(treeId, updates.size());
            }

            YT_LOG_DEBUG("Updating scheduling segment data centers in operations' runtime parameters (UpdateCountPerTree: %v)",
                updateCountPerTree);
        }

        for (const auto& [treeId, updates] : updatesPerTree) {
            for (const auto& [operationId, newDataCenter] : updates) {
                if (auto operation = FindOperation(operationId)) {
                    auto params = operation->GetRuntimeParameters();
                    GetOrCrash(params->SchedulingOptionsPerPoolTree, treeId)->SchedulingSegmentDataCenter = newDataCenter;
                    operation->SetRuntimeParameters(params);
                    Strategy_->ApplyOperationRuntimeParameters(operation.Get());
                }
            }
        }
    }

    void ManageNodeSchedulingSegments()
    {
        YT_LOG_DEBUG("Started managing node scheduling segments");

        TManageNodeSchedulingSegmentsContext context;
        context.Now = TInstant::Now();
        context.NodeShardHost = this;
        context.StrategySegmentsState = Strategy_->GetStrategySchedulingSegmentsState();
        context.ExecNodeDescriptors = GetCachedExecNodeDescriptors();
        for (const auto& [nodeId, _] : *context.ExecNodeDescriptors) {
            auto it = NodeIdToDescriptor_.find(nodeId);
            if (it == NodeIdToDescriptor_.end()) {
                continue;
            }

            const auto& descriptor = it->second;
            if (const auto& treeId = descriptor.TreeId) {
                context.NodeIdsPerTree[*treeId].emplace_back(nodeId);
            }
        }

        NodeSchedulingSegmentManager_.ManageNodeSegments(&context);

        int totalMovedNodeCount = 0;
        for (const auto& movedNodesInShard : context.MovedNodesPerNodeShard) {
            totalMovedNodeCount += movedNodesInShard.size();
        }

        if (totalMovedNodeCount > 0) {
            YT_LOG_DEBUG("Moving nodes to new scheduling segments (TotalMovedNodeCount: %v)",
                totalMovedNodeCount);

            std::vector<TFuture<void>> futures;
            for (int nodeShardId = 0; nodeShardId < NodeShards_.size(); ++nodeShardId) {
                const auto& nodeShard = NodeShards_[nodeShardId];
                const auto& movedNodesWithSegments = context.MovedNodesPerNodeShard[nodeShardId];

                futures.push_back(BIND(&TNodeShard::SetSchedulingSegmentsForNodes, nodeShard, movedNodesWithSegments)
                    .AsyncVia(nodeShard->GetInvoker())
                    .Run());
            }

            WaitFor(AllSet(futures))
                .ThrowOnError();

            // We want to update the descriptors after moving nodes between segments to send the most recent state to master.
            UpdateExecNodeDescriptors();
            context.ExecNodeDescriptors = GetCachedExecNodeDescriptors();
        }

        if (context.Now > NodeSchedulingSegmentManager_.GetNodeSegmentsInitializationDeadline()) {
            auto segmentsState = New<TPersistentSchedulingSegmentsState>();
            segmentsState->NodeStates = NodeSchedulingSegmentManager_.BuildPersistentNodeSegmentsState(&context);
            MasterConnector_->StoreSchedulingSegmentsStateAsync(std::move(segmentsState));
        }

        YT_LOG_DEBUG("Finished managing node scheduling segments");
    }

    class TOperationsService
        : public TVirtualMapBase
    {
    public:
        explicit TOperationsService(const TScheduler::TImpl* scheduler)
            : TVirtualMapBase(nullptr /* owningNode */)
            , Scheduler_(scheduler)
        { }

        virtual i64 GetSize() const override
        {
            return Scheduler_->IdToOperationService_.size() + Scheduler_->OperationAliases_.size();
        }

        virtual std::vector<TString> GetKeys(i64 limit) const override
        {
            std::vector<TString> keys;
            keys.reserve(limit);
            for (const auto& [operationId, operation] : Scheduler_->IdToOperation_) {
                if (static_cast<i64>(keys.size()) >= limit) {
                    break;
                }
                keys.emplace_back(ToString(operationId));
            }
            for (const auto& [aliasString, alias] : Scheduler_->OperationAliases_) {
                if (static_cast<i64>(keys.size()) >= limit) {
                    break;
                }
                keys.emplace_back(aliasString);
            }
            return keys;
        }

        virtual IYPathServicePtr FindItemService(TStringBuf key) const override
        {
            if (key.StartsWith(OperationAliasPrefix)) {
                // If operation is still registered, we will return the operation service.
                // If it has finished, but we still have an entry in alias -> operation id internal
                // mapping, we return a fictive map { operation_id = <operation_id> }. It is useful
                // for alias resolution when operation is not archived yet but already finished.
                auto it = Scheduler_->OperationAliases_.find(TString(key));
                if (it == Scheduler_->OperationAliases_.end()) {
                    return nullptr;
                } else {
                    auto jt = Scheduler_->IdToOperationService_.find(it->second.OperationId);
                    if (jt == Scheduler_->IdToOperationService_.end()) {
                        // The operation is unregistered, but we still return a fictive map.
                        return IYPathService::FromProducer(BIND([=] (IYsonConsumer* consumer) {
                            BuildYsonFluently(consumer)
                                .BeginMap()
                                    .Item("operation_id").Value(it->second.OperationId)
                                .EndMap();
                        }));
                    } else {
                        return jt->second;
                    }
                }
            } else {
                auto operationId = TOperationId::FromString(key);
                auto it = Scheduler_->IdToOperationService_.find(operationId);
                return it == Scheduler_->IdToOperationService_.end() ? nullptr : it->second;
            }
        }

    private:
        const TScheduler::TImpl* const Scheduler_;
    };

    class TJobsService
        : public TVirtualMapBase
    {
    public:
        explicit TJobsService(const TScheduler::TImpl* scheduler)
            : TVirtualMapBase(nullptr /* owningNode */)
            , Scheduler_(scheduler)
        { }

        virtual void GetSelf(
            TReqGet* request,
            TRspGet* response,
            const TCtxGetPtr& context) override
        {
            ThrowMethodNotSupported(context->GetMethod());
        }

        virtual void ListSelf(
            TReqList* request,
            TRspList* response,
            const TCtxListPtr& context) override
        {
            ThrowMethodNotSupported(context->GetMethod());
        }

        virtual i64 GetSize() const override
        {
            YT_ABORT();
        }

        virtual std::vector<TString> GetKeys(i64 limit) const override
        {
            YT_ABORT();
        }

        virtual IYPathServicePtr FindItemService(TStringBuf key) const override
        {
            auto jobId = TJobId::FromString(key);
            auto buildJobYsonCallback = BIND(&TJobsService::BuildControllerJobYson, MakeStrong(this), jobId);
            auto jobYPathService = IYPathService::FromProducer(buildJobYsonCallback)
                ->Via(Scheduler_->GetControlInvoker(EControlQueue::Orchid));
            return jobYPathService;
        }

    private:
        void BuildControllerJobYson(TJobId jobId, IYsonConsumer* consumer) const
        {
            const auto& nodeShard = Scheduler_->GetNodeShardByJobId(jobId);

            auto getOperationIdCallback = BIND(&TNodeShard::FindOperationIdByJobId, nodeShard, jobId)
                .AsyncVia(nodeShard->GetInvoker())
                .Run();
            auto operationId = WaitFor(getOperationIdCallback)
                .ValueOrThrow();

            if (!operationId) {
                THROW_ERROR_EXCEPTION("Job %v is missing", jobId);
            }

            auto operation = Scheduler_->GetOperationOrThrow(operationId);
            auto agent = operation->GetAgentOrThrow();

            NControllerAgent::TControllerAgentServiceProxy proxy(agent->GetChannel());
            auto req = proxy.GetJobInfo();
            req->SetTimeout(Scheduler_->Config_->ControllerAgentTracker->LightRpcTimeout);
            ToProto(req->mutable_operation_id(), operationId);
            ToProto(req->mutable_job_id(), jobId);
            auto rsp = WaitFor(req->Invoke())
                .ValueOrThrow();

            consumer->OnRaw(TYsonString(rsp->info()));
        }

        const TScheduler::TImpl* Scheduler_;
    };
};

////////////////////////////////////////////////////////////////////////////////

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

ISchedulerStrategyPtr TScheduler::GetStrategy() const
{
    return Impl_->GetStrategy();
}

const TOperationsCleanerPtr& TScheduler::GetOperationsCleaner() const
{
    return Impl_->GetOperationsCleaner();
}

IYPathServicePtr TScheduler::CreateOrchidService() const
{
    return Impl_->CreateOrchidService();
}

TRefCountedExecNodeDescriptorMapPtr TScheduler::GetCachedExecNodeDescriptors() const
{
    return Impl_->GetCachedExecNodeDescriptors();
}

const TSchedulerConfigPtr& TScheduler::GetConfig() const
{
    return Impl_->GetConfig();
}

int TScheduler::GetNodeShardId(TNodeId nodeId) const
{
    return Impl_->GetNodeShardId(nodeId);
}

const IInvokerPtr& TScheduler::GetCancelableNodeShardInvoker(int shardId) const
{
    return Impl_->GetCancelableNodeShardInvoker(shardId);
}

const std::vector<TNodeShardPtr>& TScheduler::GetNodeShards() const
{
    return Impl_->GetNodeShards();
}

bool TScheduler::IsConnected() const
{
    return Impl_->IsConnected();
}

void TScheduler::ValidateConnected()
{
    Impl_->ValidateConnected();
}

TMasterConnector* TScheduler::GetMasterConnector() const
{
    return Impl_->GetMasterConnector();
}

void TScheduler::Disconnect(const TError& error)
{
    Impl_->Disconnect(error);
}

TOperationPtr TScheduler::FindOperation(TOperationId id) const
{
    return Impl_->FindOperation(id);
}

TOperationPtr TScheduler::GetOperationOrThrow(const TOperationIdOrAlias& idOrAlias) const
{
    return Impl_->GetOperationOrThrow(idOrAlias);
}

TFuture<TParseOperationSpecResult> TScheduler::ParseSpec(TYsonString specString) const
{
    return Impl_->ParseSpec(std::move(specString));
}

TFuture<TOperationPtr> TScheduler::StartOperation(
    EOperationType type,
    TTransactionId transactionId,
    TMutationId mutationId,
    const TString& user,
    TParseOperationSpecResult parseSpecResult)
{
    return Impl_->StartOperation(
        type,
        transactionId,
        mutationId,
        user,
        std::move(parseSpecResult));
}

TFuture<void> TScheduler::AbortOperation(
    TOperationPtr operation,
    const TError& error,
    const TString& user)
{
    return Impl_->AbortOperation(operation, error, user);
}

TFuture<void> TScheduler::SuspendOperation(
    TOperationPtr operation,
    const TString& user,
    bool abortRunningJobs)
{
    return Impl_->SuspendOperation(operation, user, abortRunningJobs);
}

TFuture<void> TScheduler::ResumeOperation(
    TOperationPtr operation,
    const TString& user)
{
    return Impl_->ResumeOperation(operation, user);
}

TFuture<void> TScheduler::CompleteOperation(
    TOperationPtr operation,
    const TError& error,
    const TString& user)
{
    return Impl_->CompleteOperation(operation, error, user);
}

void TScheduler::OnOperationCompleted(const TOperationPtr& operation)
{
    Impl_->OnOperationCompleted(operation);
}

void TScheduler::OnOperationAborted(const TOperationPtr& operation, const TError& error)
{
    Impl_->OnOperationAborted(operation, error);
}

void TScheduler::OnOperationFailed(const TOperationPtr& operation, const TError& error)
{
    Impl_->OnOperationFailed(operation, error);
}

void TScheduler::OnOperationSuspended(const TOperationPtr& operation, const TError& error)
{
    Impl_->OnOperationSuspended(operation, error);
}

void TScheduler::OnOperationAgentUnregistered(const TOperationPtr& operation)
{
    Impl_->OnOperationAgentUnregistered(operation);
}

void TScheduler::OnOperationBannedInTentativeTree(const TOperationPtr& operation, const TString& treeId, const std::vector<TJobId>& jobIds)
{
    Impl_->OnOperationBannedInTentativeTree(operation, treeId, jobIds);
}

TFuture<void> TScheduler::UpdateOperationParameters(
    TOperationPtr operation,
    const TString& user,
    INodePtr parameters)
{
    return Impl_->UpdateOperationParameters(operation, user, parameters);
}

TFuture<void> TScheduler::DumpInputContext(TJobId jobId, const NYPath::TYPath& path, const TString& user)
{
    return Impl_->DumpInputContext(jobId, path, user);
}

TFuture<TNodeDescriptor> TScheduler::GetJobNode(TJobId jobId)
{
    return Impl_->GetJobNode(jobId);
}

TFuture<void> TScheduler::AbandonJob(TJobId jobId, const TString& user)
{
    return Impl_->AbandonJob(jobId, user);
}

TFuture<void> TScheduler::AbortJob(TJobId jobId, std::optional<TDuration> interruptTimeout, const TString& user)
{
    return Impl_->AbortJob(jobId, interruptTimeout, user);
}

void TScheduler::ProcessNodeHeartbeat(const TCtxNodeHeartbeatPtr& context)
{
    Impl_->ProcessNodeHeartbeat(context);
}

TSerializableAccessControlList TScheduler::GetOperationBaseAcl() const
{
    return Impl_->GetOperationBaseAcl();
}

int TScheduler::GetOperationArchiveVersion() const
{
    return Impl_->GetOperationArchiveVersion();
}

bool TScheduler::IsJobReporterEnabled() const
{
    return Impl_->IsJobReporterEnabled();
}

TString TScheduler::FormatResources(const TJobResourcesWithQuota& resources) const
{
    return Impl_->FormatResources(resources);
}

TString TScheduler::FormatResourceUsage(
    const TJobResources& usage,
    const TJobResources& limits,
    const NNodeTrackerClient::NProto::TDiskResources& diskResources) const
{
    return Impl_->FormatResourceUsage(
        usage,
        limits,
        diskResources);
}

TFuture<void> TScheduler::ValidateOperationAccess(
    const TString& user,
    TOperationId operationId,
    EPermissionSet permissions)
{
    return Impl_->ValidateOperationAccess(user, operationId, permissions);
}

TFuture<void> TScheduler::ValidateJobShellAccess(
    const TString& user,
    const TJobShellPtr& jobShell)
{
    return Impl_->ValidateJobShellAccess(user, jobShell);
}

TFuture<TOperationId> TScheduler::FindOperationIdByJobId(TJobId jobId)
{
    return Impl_->FindOperationIdByJobId(jobId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
