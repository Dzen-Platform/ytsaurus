#pragma once

#include "public.h"

#include <yt/server/lib/hydra/public.h>

#include <yt/server/lib/security_server/public.h>

#include <yt/core/actions/public.h>

#include <yt/core/rpc/public.h>

namespace NYT::NHiveServer {

////////////////////////////////////////////////////////////////////////////////

class TTransactionSupervisor
    : public TRefCounted
{
public:
    TTransactionSupervisor(
        TTransactionSupervisorConfigPtr config,
        IInvokerPtr automatonInvoker,
        IInvokerPtr trackerInvoker,
        NHydra::IHydraManagerPtr hydraManager,
        NHydra::TCompositeAutomatonPtr automaton,
        NRpc::TResponseKeeperPtr responseKeeper,
        ITransactionManagerPtr transactionManager,
        NSecurityServer::ISecurityManagerPtr securityManager,
        TCellId selfCellId,
        NTransactionClient::ITimestampProviderPtr timestampProvider,
        const std::vector<ITransactionParticipantProviderPtr>& participantProviders);

    ~TTransactionSupervisor();

    std::vector<NRpc::IServicePtr> GetRpcServices();

    TFuture<void> CommitTransaction(
        TTransactionId transactionId,
        const TString& userName,
        const std::vector<NHydra::TCellId>& participantCellIds = std::vector<NHydra::TCellId>());

    TFuture<void> AbortTransaction(
        TTransactionId transactionId,
        bool force = false);

    void Decommission();
    bool IsDecommissioned() const;

private:
    class TImpl;
    using TImplPtr = TIntrusivePtr<TImpl>;
    const TImplPtr Impl_;

};

DEFINE_REFCOUNTED_TYPE(TTransactionSupervisor)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveServer
