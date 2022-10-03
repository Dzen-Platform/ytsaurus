#pragma once

#include "public.h"

#include <yt/yt/server/lib/hive/proto/hive_manager.pb.h>

#include <yt/yt/server/lib/hydra_common/entity_map.h>
#include <yt/yt/server/lib/hydra_common/public.h>

#include <yt/yt/ytlib/hive/public.h>

#include <yt/yt/core/rpc/public.h>

#include <yt/yt/core/ytree/public.h>

namespace NYT::NHiveServer {

////////////////////////////////////////////////////////////////////////////////

//! Returns |true| if the current fiber currently handles a mutation
//! posted via Hive.
bool IsHiveMutation();

//! Returns the id of the cell that posted a mutation currently handed
//! by a current fiber or null id if that mutation is not a Hive one.
TCellId GetHiveMutationSenderId();

////////////////////////////////////////////////////////////////////////////////

struct TSerializedMessage
    : public TRefCounted
{
    TString Type;
    TString Data;
};

DEFINE_REFCOUNTED_TYPE(TSerializedMessage)

////////////////////////////////////////////////////////////////////////////////

/*!
 *  \note Thread affinity: single (unless noted otherwise)
 */
class THiveManager
    : public TRefCounted
{
public:
    THiveManager(
        THiveManagerConfigPtr config,
        NHiveClient::ICellDirectoryPtr cellDirectory,
        TCellId selfCellId,
        IInvokerPtr automatonInvoker,
        NHydra::IHydraManagerPtr hydraManager,
        NHydra::TCompositeAutomatonPtr automaton,
        NHydra::IUpstreamSynchronizerPtr upstreamSynchronizer,
        NRpc::IAuthenticatorPtr authenticator);

    ~THiveManager();

    /*!
     *  \note Thread affinity: any
     */
    NRpc::IServicePtr GetRpcService();

    /*!
     *  \note Thread affinity: any
     */
    NYTree::IYPathServicePtr GetOrchidService();

    /*!
     *  \note Thread affinity: any
     */
    TCellId GetSelfCellId() const;

    TMailbox* CreateMailbox(TCellId cellId, bool allowResurrection = false);
    TMailbox* FindMailbox(TCellId cellId);
    TMailbox* GetOrCreateMailbox(TCellId cellId);
    TMailbox* GetMailboxOrThrow(TCellId cellId);

    void RemoveMailbox(TMailbox* mailbox);

    //! Posts a message for delivery (either reliable or not).
    void PostMessage(
        TMailbox* mailbox,
        const TSerializedMessagePtr& message,
        bool reliable = true);
    void PostMessage(
        const TMailboxList& mailboxes,
        const TSerializedMessagePtr& message,
        bool reliable = true);
    void PostMessage(
        TMailbox* mailbox,
        const ::google::protobuf::MessageLite& message,
        bool reliable = true);
    void PostMessage(
        const TMailboxList& mailboxes,
        const ::google::protobuf::MessageLite& message,
        bool reliable = true);

    //! When called at instant T, returns a future which gets set
    //! when all mutations enqueued at the remote side (represented by #mailbox)
    //! prior to T, are received and applied.
    //! If #enableBatching is |true| then syncs are additionally batched.
    /*!
     *  \note Thread affinity: any
     */
    TFuture<void> SyncWith(TCellId cellId, bool enableBatching);

    DECLARE_ENTITY_MAP_ACCESSORS(Mailbox, TMailbox);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(THiveManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveServer
