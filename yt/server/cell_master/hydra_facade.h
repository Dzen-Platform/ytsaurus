#pragma once

#include "public.h"

#include <yt/server/hydra/mutation.h>

#include <yt/ytlib/election/public.h>

#include <yt/core/rpc/public.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

class TLeaderFallbackException
{ };

////////////////////////////////////////////////////////////////////////////////

class THydraFacade
    : public TRefCounted
{
public:
    THydraFacade(
        TCellMasterConfigPtr config,
        TBootstrap* bootstrap);
    ~THydraFacade();

    void Initialize();
    void LoadSnapshot(NHydra::ISnapshotReaderPtr reader, bool dump);

    TMasterAutomatonPtr GetAutomaton() const;
    NElection::IElectionManagerPtr GetElectionManager() const;
    NHydra::IHydraManagerPtr GetHydraManager() const;
    NRpc::TResponseKeeperPtr GetResponseKeeper() const;

    IInvokerPtr GetAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const;
    IInvokerPtr GetEpochAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const;
    IInvokerPtr GetGuardedAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const;

    //! Throws TLeaderFallbackException at followers.
    void RequireLeader() const;

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(THydraFacade)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
