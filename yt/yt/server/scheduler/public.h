#pragma once

#include <yt/server/lib/scheduler/public.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/actions/callback.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TOperationControllerData)

DECLARE_REFCOUNTED_CLASS(TOperation)
DECLARE_REFCOUNTED_CLASS(TJob)

using TJobList = std::list<TJobPtr>;

DECLARE_REFCOUNTED_CLASS(TNodeShard)
DECLARE_REFCOUNTED_CLASS(TExecNode)
DECLARE_REFCOUNTED_CLASS(TControllerAgent)

DECLARE_REFCOUNTED_CLASS(TOperationsCleaner)

DECLARE_REFCOUNTED_CLASS(TScheduler)
DECLARE_REFCOUNTED_CLASS(TControllerAgentTracker)

struct IEventLogHost;

DECLARE_REFCOUNTED_STRUCT(ISchedulerStrategy)
struct ISchedulerStrategyHost;
struct IOperationStrategyHost;

DECLARE_REFCOUNTED_STRUCT(ISchedulingContext)
DECLARE_REFCOUNTED_STRUCT(IOperationControllerStrategyHost)
DECLARE_REFCOUNTED_STRUCT(IOperationController)

struct TOperationControllerInitializeResult;
struct TOperationControllerPrepareResult;
struct TOperationRevivalDescriptor;

class TMasterConnector;
class TBootstrap;

DECLARE_REFCOUNTED_CLASS(TPersistentStrategyState)
DECLARE_REFCOUNTED_CLASS(TPersistentTreeState)
DECLARE_REFCOUNTED_CLASS(TPersistentPoolState)

namespace NClassicScheduler {

// TODO(mrkastep) Move to private.h
DECLARE_REFCOUNTED_CLASS(TFairShareStrategyOperationState)

} // namespace NClassicScheduler

// TODO(mrkastep) Move to private.h
DECLARE_REFCOUNTED_CLASS(TFairShareStrategyOperationState)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
