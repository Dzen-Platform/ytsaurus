#include "control_thread.h"
#include "operation_controller.h"
#include "node_shard.h"

#include <yt/server/scheduler/fair_share_implementations.h>
#include <yt/server/scheduler/fair_share_strategy.h>
#include <yt/server/scheduler/fair_share_tree.h>

#include <yt/client/security_client/acl.h>

#include <yt/core/concurrency/public.h>
#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/thread_pool.h>

#include <yt/core/yson/public.h>


namespace NYT::NSchedulerSimulator {

using namespace NScheduler;
using namespace NLogging;
using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;

using std::placeholders::_1;

static const auto& Logger = SchedulerSimulatorLogger;

////////////////////////////////////////////////////////////////////////////////

TControlThreadEvent::TControlThreadEvent(EControlThreadEventType type, TInstant time)
    : Type(type)
    , Time(time)
    , OperationId()
{ }

TControlThreadEvent TControlThreadEvent::OperationStarted(TInstant time, TOperationId id)
{
    TControlThreadEvent event(EControlThreadEventType::OperationStarted, time);
    event.OperationId = id;
    return event;
}

TControlThreadEvent TControlThreadEvent::FairShareUpdateAndLog(TInstant time)
{
    return TControlThreadEvent(EControlThreadEventType::FairShareUpdateAndLog, time);
}

TControlThreadEvent TControlThreadEvent::LogNodes(TInstant time)
{
    return TControlThreadEvent(EControlThreadEventType::LogNodes, time);
}

bool operator<(const TControlThreadEvent& lhs, const TControlThreadEvent& rhs)
{
    if (lhs.Time != rhs.Time) {
        return lhs.Time < rhs.Time;
    }
    return lhs.Type < rhs.Type;
}

////////////////////////////////////////////////////////////////////////////////

TSimulatorControlThread::TSimulatorControlThread(
    const std::vector<TExecNodePtr>* execNodes,
    IOutputStream* eventLogOutputStream,
    IOperationStatisticsOutput* operationStatisticsOutput,
    const TSchedulerSimulatorConfigPtr& config,
    const NScheduler::TSchedulerConfigPtr& schedulerConfig,
    const std::vector<TOperationDescription>& operations,
    TInstant earliestTime)
    : Initialized_(false)
    , FairShareUpdateAndLogPeriod_(schedulerConfig->FairShareUpdatePeriod)
    , NodesInfoLoggingPeriod_(schedulerConfig->NodesInfoLoggingPeriod)
    , Config_(config)
    , ExecNodes_(execNodes)
    , ActionQueue_(New<TActionQueue>(Format("ControlThread")))
    , StrategyHost_(execNodes, eventLogOutputStream, config->RemoteEventLog)
    , SchedulerStrategy_(
        Config_->UseClassicScheduler
        ? CreateFairShareStrategy<TClassicFairShareImpl>(schedulerConfig, &StrategyHost_, {ActionQueue_->GetInvoker()})
        : CreateFairShareStrategy<TVectorFairShareImpl>(schedulerConfig, &StrategyHost_, {ActionQueue_->GetInvoker()})
    )
    , SchedulerStrategyForNodeShards_(SchedulerStrategy_, StrategyHost_, ActionQueue_->GetInvoker())
    , NodeShardEventQueue_(
        *execNodes,
        config->HeartbeatPeriod,
        earliestTime,
        config->NodeShardCount,
        /* maxAllowedOutrunning */ FairShareUpdateAndLogPeriod_ + FairShareUpdateAndLogPeriod_)
    , NodeShardThreadPool_(New<TThreadPool>(config->ThreadCount, "NodeShardPool"))
    , OperationStatistics_(operations)
    , JobAndOperationCounter_(operations.size())
    , Logger(TLogger(NSchedulerSimulator::Logger).AddTag("ControlThread"))
{
    for (const auto& operation : operations) {
        InsertControlThreadEvent(TControlThreadEvent::OperationStarted(operation.StartTime, operation.Id));
    }
    InsertControlThreadEvent(TControlThreadEvent::FairShareUpdateAndLog(earliestTime));
    InsertControlThreadEvent(TControlThreadEvent::LogNodes(earliestTime + TDuration::MilliSeconds(123)));

    for (int shardId = 0; shardId < config->NodeShardCount; ++shardId) {
        auto nodeShard = New<TSimulatorNodeShard>(
            NodeShardThreadPool_->GetInvoker(),
            &StrategyHost_,
            &NodeShardEventQueue_,
            &SchedulerStrategyForNodeShards_,
            &OperationStatistics_,
            operationStatisticsOutput,
            &RunningOperationsMap_,
            &JobAndOperationCounter_,
            Config_,
            schedulerConfig,
            earliestTime,
            shardId);

        NodeShards_.push_back(nodeShard);
    }
}

void TSimulatorControlThread::Initialize(const NYTree::INodePtr& poolTreesNode)
{
    YT_VERIFY(!Initialized_.load());

    WaitFor(BIND(&ISchedulerStrategy::UpdatePoolTrees, SchedulerStrategy_, poolTreesNode, New<TPersistentStrategyState>())
        .AsyncVia(ActionQueue_->GetInvoker())
        .Run())
        .ThrowOnError();

    for (const auto& execNode : *ExecNodes_) {
        const auto& nodeShard = NodeShards_[GetNodeShardId(execNode->GetId(), NodeShards_.size())];
        WaitFor(
            BIND(&TSimulatorNodeShard::RegisterNode, nodeShard, execNode)
                .AsyncVia(nodeShard->GetInvoker())
                .Run())
            .ThrowOnError();
    }

    Initialized_.store(true);
}

bool TSimulatorControlThread::IsInitialized() const
{
    return Initialized_.load();
}

TFuture<void> TSimulatorControlThread::AsyncRun()
{
    YT_VERIFY(Initialized_.load());
    return BIND(&TSimulatorControlThread::Run, MakeStrong(this))
        .AsyncVia(ActionQueue_->GetInvoker())
        .Run();
}

void TSimulatorControlThread::Run()
{
    YT_LOG_INFO("Simulation started (ThreadCount %v, NodeShardCount: %v)",
        Config_->ThreadCount,
        Config_->NodeShardCount);

    std::vector<TFuture<void>> asyncWorkerResults;
    for (const auto& nodeShard : NodeShards_) {
        asyncWorkerResults.emplace_back(nodeShard->AsyncRun());
    }

    int iter = 0;
    while (JobAndOperationCounter_.HasUnfinishedOperations()) {
        iter += 1;
        if (iter % Config_->CyclesPerFlush == 0) {
            YT_LOG_INFO(
                "Simulated %v cycles (FinishedOperations: %v, RunningOperation: %v, "
                "TotalOperations: %v, RunningJobs: %v)",
                iter,
                JobAndOperationCounter_.GetFinishedOperationCount(),
                JobAndOperationCounter_.GetStartedOperationCount(),
                JobAndOperationCounter_.GetTotalOperationCount(),
                JobAndOperationCounter_.GetRunningJobCount());

            RunningOperationsMap_.ApplyRead([this] (const auto& pair) {
                const auto& operation = pair.second;
                YT_LOG_INFO("%v, (OperationId: %v)",
                    operation->GetController()->GetLoggingProgress(),
                    operation->GetId());
            });
        }

        RunOnce();
        Yield();
    }

    WaitFor(AllSucceeded(asyncWorkerResults))
        .ThrowOnError();

    SchedulerStrategy_->OnMasterDisconnected();
    StrategyHost_.CloseEventLogger();

    YT_LOG_INFO("Simulation finished");
}


void TSimulatorControlThread::RunOnce()
{
    auto event = PopControlThreadEvent();

    switch (event.Type) {
        case EControlThreadEventType::OperationStarted: {
            OnOperationStarted(event);
            break;
        }

        case EControlThreadEventType::FairShareUpdateAndLog: {
            OnFairShareUpdateAndLog(event);
            break;
        }

        case EControlThreadEventType::LogNodes: {
            OnLogNodes(event);
            break;
        }
    }
}

void TSimulatorControlThread::OnOperationStarted(const TControlThreadEvent& event)
{
    const auto& description = OperationStatistics_.GetOperationDescription(event.OperationId);

    auto runtimeParameters = New<TOperationRuntimeParameters>();
    SchedulerStrategy_->InitOperationRuntimeParameters(
        runtimeParameters,
        NYTree::ConvertTo<TOperationSpecBasePtr>(description.Spec),
        NSecurityClient::TSerializableAccessControlList(),
        description.AuthenticatedUser,
        description.Type);
    auto operation = New<NSchedulerSimulator::TOperation>(description, runtimeParameters);

    auto operationController = CreateSimulatorOperationController(operation.Get(), &description,
        Config_->ScheduleJobDelay);
    operation->SetController(operationController);

    RunningOperationsMap_.Insert(operation->GetId(), operation);
    OperationStatistics_.OnOperationStarted(operation->GetId());
    YT_LOG_INFO("Operation started (VirtualTimestamp: %v, OperationId: %v)", event.Time, operation->GetId());

    // Notify scheduler.
    std::vector<TString> unknownTreeIds;
    SchedulerStrategy_->RegisterOperation(operation.Get(), &unknownTreeIds);
    YT_VERIFY(unknownTreeIds.empty());
    StrategyHost_.LogEventFluently(ELogEventType::OperationStarted)
        .Item("operation_id").Value(operation->GetId())
        .Item("operation_type").Value(operation->GetType())
        .Item("spec").Value(operation->GetSpecString())
        .Item("authenticated_user").Value(operation->GetAuthenticatedUser())
        .Do(std::bind(&ISchedulerStrategy::BuildOperationInfoForEventLog, SchedulerStrategy_, operation.Get(), _1));
    SchedulerStrategy_->EnableOperation(operation.Get());

    JobAndOperationCounter_.OnOperationStarted();
}

void TSimulatorControlThread::OnFairShareUpdateAndLog(const TControlThreadEvent& event)
{
    auto updateTime = event.Time;

    YT_LOG_INFO("Started waiting for struggling node shards (VirtualTimestamp: %v)", event.Time);
    NodeShardEventQueue_.WaitForStrugglingNodeShards(updateTime);
    YT_LOG_INFO("Finished waiting for struggling node shards (VirtualTimestamp: %v)", event.Time);

    SchedulerStrategy_->OnFairShareUpdateAt(updateTime);
    SchedulerStrategy_->OnFairShareProfilingAt(updateTime);
    if (Config_->EnableFullEventLog) {
        SchedulerStrategy_->OnFairShareLoggingAt(updateTime);
    } else {
        SchedulerStrategy_->OnFairShareEssentialLoggingAt(updateTime);
    }

    NodeShardEventQueue_.UpdateControlThreadTime(updateTime);
    InsertControlThreadEvent(TControlThreadEvent::FairShareUpdateAndLog(event.Time + FairShareUpdateAndLogPeriod_));
}

void TSimulatorControlThread::OnLogNodes(const TControlThreadEvent& event)
{
    YT_LOG_INFO("Started logging nodes info (VirtualTimestamp: %v)", event.Time);

    std::vector<TFuture<TYsonString>> nodeListFutures;
    for (const auto& nodeShard : NodeShards_) {
        nodeListFutures.push_back(
            BIND([nodeShard] () {
                return BuildYsonStringFluently<EYsonType::MapFragment>()
                    .Do(BIND(&TSimulatorNodeShard::BuildNodesYson, nodeShard))
                    .Finish();
            })
            .AsyncVia(nodeShard->GetInvoker())
            .Run());
    }

    auto nodeLists = WaitFor(AllSucceeded(nodeListFutures))
        .ValueOrThrow();

    StrategyHost_.LogEventFluently(ELogEventType::NodesInfo, event.Time)
        .Item("nodes")
        .DoMapFor(nodeLists, [](TFluentMap fluent, const auto& nodeList) {
            fluent.Items(nodeList);
        });

    InsertControlThreadEvent(TControlThreadEvent::LogNodes(event.Time + NodesInfoLoggingPeriod_));
    YT_LOG_INFO("Finished logging nodes info (VirtualTimestamp: %v)", event.Time);
}

void TSimulatorControlThread::InsertControlThreadEvent(TControlThreadEvent event)
{
    ControlThreadEvents_.insert(event);
}

TControlThreadEvent TSimulatorControlThread::PopControlThreadEvent()
{
    YT_VERIFY(!ControlThreadEvents_.empty());
    auto beginIt = ControlThreadEvents_.begin();
    auto event = *beginIt;
    ControlThreadEvents_.erase(beginIt);
    return event;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSchedulerSimulator

