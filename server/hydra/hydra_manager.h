#pragma once

#include "public.h"

#include <yt/core/actions/future.h>
#include <yt/core/actions/signal.h>

#include <yt/core/misc/error.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

struct IHydraManager
    : public virtual TRefCounted
{
    //! Activates the instance.
    /*!
     *  \note Thread affinity: ControlThread
     */
    virtual void Initialize() = 0;

    //! Deactivates the instance. The resulting future is set
    //! when the instance is fully stopped, e.g. the automaton thread
    //! will not receive any callbacks.
    /*!
     *  \note Thread affinity: ControlThread
     */
    virtual TFuture<void> Finalize() = 0;

    //! Returns the state as seen in the control thread.
    /*!
     *  \note Thread affinity: ControlThread
     */
    virtual EPeerState GetControlState() const = 0;

    //! Returns the state as seen in the automaton thread.
    /*!
     *  \note Thread affinity: AutomatonThread
     */
    virtual EPeerState GetAutomatonState() const = 0;

    //! Returns a wrapper invoker used for accessing the automaton.
    /*!
     *  \note Thread affinity: any
     */
    virtual IInvokerPtr CreateGuardedAutomatonInvoker(IInvokerPtr underlyingInvoker) = 0;

    //! Returns |true| if the peer is a leader ready to carry out distributed commits.
    /*!
     *  This check also ensures that the leader has acquired and is still holding the lease.
     *
     *  \note Thread affinity: any
     */
    virtual bool IsActiveLeader() const = 0;

    //! Returns |true| if the peer is a follower ready to serve reads.
    /*!
     *  Any follower still can lag arbitrarily behind the leader.
     *  One should use #SyncWithLeader to workaround stale reads.
     *
     *  \note Thread affinity: any
     */
    virtual bool IsActiveFollower() const = 0;

    //! Returns the cancelable context for the current epoch, as viewed by the Control Thread.
    /*!
     *  \note Thread affinity: ControlThread
     */
    virtual TCancelableContextPtr GetControlCancelableContext() const = 0;

    //! Returns the cancelable context for the current epoch, as viewed by the Automaton Thread.
    /*!
     *  \note Thread affinity: AutomatonThread
     */
    virtual TCancelableContextPtr GetAutomatonCancelableContext() const = 0;

    //! Returns the leading peer id, as viewed by the Automaton Thread.
    /*!
     *  \note Thread affinity: AutomatonThread
     */
    virtual NElection::TPeerId GetAutomatonLeaderId() const = 0;

    //! When called at the leader returns a preset future.
    //! When called at a follower at instant T returns a future that gets set
    //! when the committed version at this follower is equal to or larger than
    //! the committed version at the leader at T.
    /*!
     *  \note Thread affinity: AutomatonThread
     */
    virtual TFuture<void> SyncWithLeader() = 0;

    //! Commits a mutation.
    /*!
     *  If the automaton is in read-only state then #EErrorCode::ReadOnly is returned.
     *  If the peer is not an active leader then #EErrorCode::InvalidState is returned.
     *
     *  \note Thread affinity: AutomatonThread
     */
    virtual TFuture<TMutationResponse> CommitMutation(const TMutationRequest& request) = 0;

    //! Returns |true| if read-only mode is active.
    /*!
     *  \note Thread affinity: any
     */
    virtual bool GetReadOnly() const = 0;

    //! Toggles read-only mode.
    /*!
     *  \note Thread affinity: any
     */
    virtual void SetReadOnly(bool value) = 0;

    //! Starts a distributed snapshot build operation.
    //! Once finished, returns the snapshot id.
    /*!
     *  \note Thread affinity: AutomatonThread
     */
    virtual TFuture<int> BuildSnapshot() = 0;

    //! Produces monitoring info.
    /*!
     *  \note Thread affinity: any
     */
    virtual NYTree::TYsonProducer GetMonitoringProducer() = 0;

    //! Raised within the automaton thread when the peer has started leading
    //! and enters recovery.
    DECLARE_INTERFACE_SIGNAL(void(), StartLeading);
    //! Raised within the automaton thread when the leader recovery is complete.
    //! The leader may now serve read requests.
    DECLARE_INTERFACE_SIGNAL(void(), LeaderRecoveryComplete);
    //! Raised within the automaton thread when an active quorum is established.
    //! The leader may now serve read-write requests.
    DECLARE_INTERFACE_SIGNAL(void(), LeaderActive);
    //! Raised within the automaton thread when the peer has stopped leading.
    DECLARE_INTERFACE_SIGNAL(void(), StopLeading);

    //! Raised within the automaton thread when the peer has started following
    //! and enters recovery.
    DECLARE_INTERFACE_SIGNAL(void(), StartFollowing);
    //! Raised within the automaton thread when the follower recovery is complete.
    //! The follower may now serve read requests.
    DECLARE_INTERFACE_SIGNAL(void(), FollowerRecoveryComplete);
    //! Raised within the automaton thread when the peer has stopped following.
    DECLARE_INTERFACE_SIGNAL(void(), StopFollowing);

    //! Raised during periodic leader lease checks.
    //! The subscriber must start an appropriate check and return a future
    //! summarizing its outcome.
    DECLARE_INTERFACE_SIGNAL(TFuture<void>(), LeaderLeaseCheck);


    // Extension methods.
    bool IsLeader() const;
    bool IsFollower() const;
    bool IsRecovery() const;

};

DEFINE_REFCOUNTED_TYPE(IHydraManager)

///////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
