#pragma once

#include "public.h"

#include <yt/ytlib/hydra/public.h>

#include <yt/ytlib/scheduler/scheduler_service.pb.h>

#include <yt/ytlib/job_tracker_client/statistics.h>

#include <yt/ytlib/api/public.h>

#include <yt/core/actions/future.h>

#include <yt/core/misc/enum.h>
#include <yt/core/misc/error.h>
#include <yt/core/misc/property.h>
#include <yt/core/misc/ref.h>
#include <yt/core/misc/crash_handler.h>

#include <yt/core/ytree/node.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

struct TOperationEvent
{
    TInstant Time;
    EOperationState State;
};

void Serialize(const TOperationEvent& schema, NYson::IYsonConsumer* consumer);
void Deserialize(TOperationEvent& event, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

class TOperation
    : public TIntrinsicRefCounted
{
public:
    using TAlertsArray = TEnumIndexedVector<TError, EOperationAlertType>;

    DEFINE_BYVAL_RO_PROPERTY(TOperationId, Id);

    DEFINE_BYVAL_RO_PROPERTY(EOperationType, Type);

    DEFINE_BYVAL_RO_PROPERTY(NRpc::TMutationId, MutationId);

    DEFINE_BYVAL_RO_PROPERTY(EOperationState, State);
    DEFINE_BYVAL_RW_PROPERTY(bool, Suspended);

    // By default, all new operations are not activated.
    // When operation passes admission control and scheduler decides
    // that it's ready to start jobs, it is marked as active.
    DEFINE_BYVAL_RW_PROPERTY(bool, Activated);

    DEFINE_BYVAL_RW_PROPERTY(bool, Prepared);

    //! User-supplied transaction where the operation resides.
    DEFINE_BYVAL_RO_PROPERTY(NTransactionClient::TTransactionId, UserTransactionId);

    DEFINE_BYVAL_RO_PROPERTY(NYTree::IMapNodePtr, Spec);

    // A YSON map that is stored under ACL in Cypress.
    // NB: It should not be present in operation spec as it may contain
    // sensitive information.
    DEFINE_BYVAL_RW_PROPERTY(NYTree::IMapNodePtr, SecureVault);

    DEFINE_BYVAL_RO_PROPERTY(Stroka, AuthenticatedUser);
    DEFINE_BYVAL_RO_PROPERTY(std::vector<Stroka>, Owners);

    DEFINE_BYVAL_RO_PROPERTY(TInstant, StartTime);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, FinishTime);

    //! List of events that happened to operation.
    DEFINE_BYVAL_RO_PROPERTY(std::vector<TOperationEvent>, Events);

    //! List of operation alerts.
    DEFINE_BYREF_RW_PROPERTY(TAlertsArray, Alerts);

    //! Controller that owns the operation.
    DEFINE_BYVAL_RW_PROPERTY(IOperationControllerPtr, Controller);

    //! Operation result, becomes set when the operation finishes.
    DEFINE_BYREF_RW_PROPERTY(NProto::TOperationResult, Result);

    //! Stores statistics about operation preparation and schedule job timings.
    DEFINE_BYREF_RW_PROPERTY(NJobTrackerClient::TStatistics, ControllerTimeStatistics);

    //! Last time the progress was logged.
    DEFINE_BYVAL_RW_PROPERTY(TInstant, LastLogProgressTime);

    //! Gets set when the operation is started.
    TFuture<TOperationPtr> GetStarted();

    //! Set operation start result.
    void SetStarted(const TError& error);

    //! Gets set when the operation is finished.
    TFuture<void> GetFinished();

    //! Marks the operation as finished.
    void SetFinished();

    //! Delegates to #NYT::NScheduler::IsOperationFinished.
    bool IsFinishedState() const;

    //! Delegates to #NYT::NScheduler::IsOperationFinishing.
    bool IsFinishingState() const;

    //! Checks whether current operation state allows starting new jobs.
    bool IsSchedulable() const;

    //! Adds new sample to controller time statistics.
    void UpdateControllerTimeStatistics(const NYPath::TYPath& name, TDuration value);
    void UpdateControllerTimeStatistics(const NJobTrackerClient::TStatistics& statistics);

    //! Returns |true| if operation controller progress can be built.
    bool HasControllerProgress() const;

    //! Returns |true| if operation controller job splitter info can be built.
    bool HasControllerJobSplitterInfo() const;

    //! Returns the codicil guard holding the operation id.
    TCodicilGuard MakeCodicilGuard() const;

    //! Sets operation state and adds corresponding event.
    void SetState(EOperationState state);

    //! Returns a cancelable control invoker corresponding to this operation.
    const IInvokerPtr& GetCancelableControlInvoker();

    //! Cancels the context of the invoker returned by #GetCancelableControlInvoker.
    void Cancel();

    TOperation(
        const TOperationId& operationId,
        EOperationType type,
        const NRpc::TMutationId& mutationId,
        NTransactionClient::TTransactionId userTransactionId,
        NYTree::IMapNodePtr spec,
        const Stroka& authenticatedUser,
        const std::vector<Stroka>& owners,
        TInstant startTime,
        IInvokerPtr controlInvoker,
        EOperationState state = EOperationState::None,
        bool suspended = false,
        const std::vector<TOperationEvent>& events = {});

private:
    const Stroka CodicilData_;
    const TCancelableContextPtr CancelableContext_;
    const IInvokerPtr CancelableInvoker_;

    TPromise<void> StartedPromise_ = NewPromise<void>();
    TPromise<void> FinishedPromise_ = NewPromise<void>();

};

DEFINE_REFCOUNTED_TYPE(TOperation)

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
