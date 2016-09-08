#include "operation.h"
#include "exec_node.h"
#include "helpers.h"
#include "job.h"
#include "operation_controller.h"

#include <yt/ytlib/scheduler/helpers.h>
#include <yt/ytlib/scheduler/config.h>

namespace NYT {
namespace NScheduler {

using namespace NApi;
using namespace NJobTrackerClient;
using namespace NRpc;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////

TOperation::TOperation(
    const TOperationId& id,
    EOperationType type,
    const TMutationId& mutationId,
    ITransactionPtr userTransaction,
    IMapNodePtr spec,
    const Stroka& authenticatedUser,
    const std::vector<Stroka>& owners,
    TInstant startTime,
    EOperationState state,
    bool suspended)
    : Id_(id)
    , Type_(type)
    , MutationId_(mutationId)
    , State_(state)
    , Suspended_(suspended)
    , Activated_(false)
    , Prepared_(false)
    , UserTransaction_(userTransaction)
    , Spec_(spec)
    , AuthenticatedUser_(authenticatedUser)
    , Owners_(owners)
    , StartTime_(startTime)
    , StderrCount_(0)
    , JobNodeCount_(0)
    , CodicilData_(MakeOperationCodicilString(Id_))
{
    auto parsedSpec = ConvertTo<TOperationSpecBasePtr>(Spec_);
    MaxStderrCount_ = parsedSpec->MaxStderrCount;
    SchedulingTag_ = parsedSpec->SchedulingTag;
}

TFuture<TOperationPtr> TOperation::GetStarted()
{
    return StartedPromise_.ToFuture().Apply(BIND([this_ = MakeStrong(this)] () -> TOperationPtr {
        return this_;
    }));
}

void TOperation::SetStarted(const TError& error)
{
    StartedPromise_.Set(error);
}

TFuture<void> TOperation::GetFinished()
{
    return FinishedPromise_;
}

void TOperation::SetFinished()
{
    FinishedPromise_.Set();
}

bool TOperation::IsFinishedState() const
{
    return IsOperationFinished(State_);
}

bool TOperation::IsFinishingState() const
{
    return IsOperationFinishing(State_);
}

bool TOperation::IsSchedulable() const
{
    return State_ == EOperationState::Running && !Suspended_;
}

void TOperation::UpdateControllerTimeStatistics(const NYPath::TYPath& name, TDuration value)
{
    ControllerTimeStatistics_.AddSample(name, value.MicroSeconds());
}

void TOperation::UpdateControllerTimeStatistics(const TStatistics& statistics)
{
    ControllerTimeStatistics_.Update(statistics);
}

bool TOperation::HasControllerProgress() const
{
    return (State_ == EOperationState::Running || IsFinishedState()) &&
        Controller_ &&
        Controller_->HasProgress();
}

TCodicilGuard TOperation::MakeCodicilGuard() const
{
    return TCodicilGuard(CodicilData_);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

