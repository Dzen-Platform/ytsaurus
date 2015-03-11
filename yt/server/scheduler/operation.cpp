#include "stdafx.h"
#include "operation.h"
#include "job.h"
#include "exec_node.h"
#include "operation_controller.h"

#include <ytlib/scheduler/helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////

TOperation::TOperation(
    const TOperationId& id,
    EOperationType type,
    const NRpc::TMutationId& mutationId,
    TTransactionPtr userTransaction,
    NYTree::IMapNodePtr spec,
    const Stroka& authenticatedUser,
    TInstant startTime,
    EOperationState state,
    bool suspended)
    : Id_(id)
    , Type_(type)
    , MutationId_(mutationId)
    , State_(state)
    , Suspended_(suspended)
    , UserTransaction_(userTransaction)
    , Spec_(spec)
    , AuthenticatedUser_(authenticatedUser)
    , StartTime_(startTime)
    , StderrCount_(0)
    , MaxStderrCount_(0)
    , CleanStart_(false)
    , StartedPromise(NewPromise<void>())
    , FinishedPromise(NewPromise<void>())
{ }

TFuture<TOperationPtr> TOperation::GetStarted()
{
    return StartedPromise.ToFuture().Apply(BIND([this_ = MakeStrong(this)] () -> TOperationPtr {
        return this_;
    }));
}

void TOperation::SetStarted(const TError& error)
{
    StartedPromise.Set(error);
}

TFuture<void> TOperation::GetFinished()
{
    return FinishedPromise;
}

void TOperation::SetFinished()
{
    FinishedPromise.Set();
}

bool TOperation::IsFinishedState() const
{
    return IsOperationFinished(State_);
}

bool TOperation::IsFinishingState() const
{
    return IsOperationFinishing(State_);
}

void TOperation::UpdateStatistics(const TStatistics& statistics, EJobFinalState state)
{
    Statistics[state].Merge(statistics);
}

void TOperation::BuildStatistics(NYson::IYsonConsumer* consumer) const
{
    NYTree::BuildYsonFluently(consumer)
        .DoMapFor(TEnumTraits<EJobFinalState>::GetDomainValues(), [&] (NYTree::TFluentMap fluent, EJobFinalState state) {
            fluent
                .Item(Format("%lv_jobs", state))
                .Value(Statistics[state]);
        });
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

