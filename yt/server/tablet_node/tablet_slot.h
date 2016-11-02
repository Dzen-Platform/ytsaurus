#pragma once

#include "public.h"

#include <yt/server/cell_node/public.h>

#include <yt/server/hydra/public.h>

#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/public.h>

#include <yt/ytlib/hydra/public.h>

#include <yt/ytlib/tablet_client/heartbeat.pb.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/actions/public.h>

#include <yt/core/rpc/public.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

//! An instance of Hydra managing a number of tablets.
class TTabletSlot
    : public TRefCounted
{
public:
    TTabletSlot(
        int slotIndex,
        TTabletNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap);
    
    ~TTabletSlot();

    int GetIndex() const;
    const NHydra::TCellId& GetCellId() const;
    NHydra::EPeerState GetControlState() const;
    NHydra::EPeerState GetAutomatonState() const;
    NHydra::TPeerId GetPeerId() const;
    const NHiveClient::TCellDescriptor& GetCellDescriptor() const;
    NTransactionClient::TTransactionId GetPrerequisiteTransactionId() const;

    NHydra::IHydraManagerPtr GetHydraManager() const;
    NRpc::TResponseKeeperPtr GetResponseKeeper() const;
    TTabletAutomatonPtr GetAutomaton() const;

    // These methods are thread-safe.
    // They may return null invoker (see #GetNullInvoker) if the invoker of the requested type is not available.
    IInvokerPtr GetAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const;
    IInvokerPtr GetEpochAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const;
    IInvokerPtr GetGuardedAutomatonInvoker(EAutomatonThreadQueue queue = EAutomatonThreadQueue::Default) const;

    NHiveServer::THiveManagerPtr GetHiveManager() const;
    NHiveServer::TMailbox* GetMasterMailbox();

    TTransactionManagerPtr GetTransactionManager() const;
    NHiveServer::TTransactionSupervisorPtr GetTransactionSupervisor() const;

    TTabletManagerPtr GetTabletManager() const;

    NObjectClient::TObjectId GenerateId(NObjectClient::EObjectType type);

    void Initialize(const NTabletClient::NProto::TCreateTabletSlotInfo& createInfo);
    bool CanConfigure() const;
    void Configure(const NTabletClient::NProto::TConfigureTabletSlotInfo& configureInfo);
    TFuture<void> Finalize();

    NYTree::IYPathServicePtr GetOrchidService();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

    class TElectionManager;
    using TElectionManagerPtr = TIntrusivePtr<TElectionManager>;

};

DEFINE_REFCOUNTED_TYPE(TTabletSlot)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
