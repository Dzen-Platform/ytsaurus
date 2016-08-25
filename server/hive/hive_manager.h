#pragma once

#include "public.h"

#include <yt/server/hive/hive_manager.pb.h>

#include <yt/server/hydra/entity_map.h>
#include <yt/server/hydra/public.h>

#include <yt/ytlib/hydra/public.h>

#include <yt/core/rpc/public.h>

#include <yt/core/ytree/public.h>


namespace NYT {
namespace NHive {

////////////////////////////////////////////////////////////////////////////////

//! Returns |true| if the current fiber currently handles a mutation
//! posted via Hive.
bool IsHiveMutation();

////////////////////////////////////////////////////////////////////////////////

// WinAPI is still with us :)
#undef PostMessage
#undef SendMessage

class THiveManager
    : public TRefCounted
{
public:
    THiveManager(
        THiveManagerConfigPtr config,
        TCellDirectoryPtr cellDirectory,
        const TCellId& selfCellId,
        IInvokerPtr automatonInvoker,
        NHydra::IHydraManagerPtr hydraManager,
        NHydra::TCompositeAutomatonPtr automaton);

    ~THiveManager();

    NRpc::IServicePtr GetRpcService();

    const TCellId& GetSelfCellId() const;

    TMailbox* CreateMailbox(const TCellId& cellId);
    TMailbox* GetOrCreateMailbox(const TCellId& cellId);
    TMailbox* GetMailboxOrThrow(const TCellId& cellId);
    void RemoveMailbox(const TCellId& cellId);

    //! Posts a message for delivery (either reliable or not).
    void PostMessage(
        TMailbox* mailbox,
        const NProto::TEncapsulatedMessage& message,
        bool reliable = true);
    void PostMessage(
        TMailbox* mailbox,
        const ::google::protobuf::MessageLite& message,
        bool reliable = true);

    //! When called at instant T, returns a future which gets set
    //! when all mutations enqueued at the remote side (represented by #mailbox)
    //! prior to T, are received and applied.
    TFuture<void> SyncWith(const TCellId& cellId);

    NYTree::IYPathServicePtr GetOrchidService();

    DECLARE_ENTITY_MAP_ACCESSORS(Mailbox, TMailbox);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(THiveManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHive
} // namespace NYT
