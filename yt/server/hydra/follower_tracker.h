#pragma once

#include "private.h"

#include <core/concurrency/periodic_executor.h>
#include <core/concurrency/thread_affinity.h>

#include <core/logging/log.h>

#include <ytlib/hydra/hydra_service_proxy.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

class TFollowerTracker
    : public TRefCounted
{
public:
    TFollowerTracker(
        TDistributedHydraManagerConfigPtr config,
        NElection::TCellManagerPtr cellManager,
        TDecoratedAutomatonPtr decoratedAutomaton,
        TEpochContext* epochContext);

    void Start();

    void ResetFollower(TPeerId followerId);

    TFuture<void> GetActiveQuorum();

private:
    TDistributedHydraManagerConfigPtr Config_;
    NElection::TCellManagerPtr CellManager_;
    TDecoratedAutomatonPtr DecoratedAutomaton_;
    TEpochContext* EpochContext_;

    std::vector<EPeerState> PeerStates_;
    int ActivePeerCount_ = 0;
    TPromise<void> ActiveQuorumPromise_ = NewPromise<void>();

    NLogging::TLogger Logger;


    void SendPing(TPeerId followerId);
    void SchedulePing(TPeerId followerId);
    void OnPingResponse(TPeerId followerId, const THydraServiceProxy::TErrorOrRspPingFollowerPtr& rspOrError);
    
    void SetFollowerState(TPeerId followerId, EPeerState state);
    void OnPeerActivated();
    void OnPeerDeactivated();

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

};

DEFINE_REFCOUNTED_TYPE(TFollowerTracker)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
