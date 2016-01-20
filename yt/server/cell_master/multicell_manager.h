#pragma once

#include "public.h"

#include <yt/core/misc/ref.h>
#include <yt/core/misc/variant.h>

#include <yt/core/actions/signal.h>

#include <yt/core/rpc/public.h>

#include <yt/ytlib/election/public.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/ytlib/hydra/public.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

//! Provides a convenient helper for constructing cross-cell messages of various
//! kinds. Note that ctors are intentionally implicit.
struct TCrossCellMessage
{
    template <class TRequest, class TResponse>
    TCrossCellMessage(TIntrusivePtr<NYTree::TTypedYPathRequest<TRequest, TResponse>> request)
        : Payload(TClientMessage{std::move(request)})
    { }

    TCrossCellMessage(const ::google::protobuf::MessageLite& message)
        : Payload(TProtoMessage{&message})
    { }

    TCrossCellMessage(const NObjectClient::TObjectId& objectId, NRpc::IServiceContextPtr context)
        : Payload(TServiceMessage{objectId, std::move(context)})
    { }

    struct TClientMessage
    {
        NRpc::IClientRequestPtr Request;
    };

    struct TProtoMessage
    {
        const ::google::protobuf::MessageLite* Message;
    };

    struct TServiceMessage
    {
        NObjectClient::TObjectId ObjectId;
        NRpc::IServiceContextPtr Context;
    };

    TVariant<
        TClientMessage,
        TProtoMessage,
        TServiceMessage
    > Payload;
};

////////////////////////////////////////////////////////////////////////////////

class TMulticellManager
    : public TRefCounted
{
public:
    TMulticellManager(
        TMulticellManagerConfigPtr config,
        TBootstrap* bootstrap);
    ~TMulticellManager();

    void PostToMaster(
        const TCrossCellMessage& message,
        NObjectClient::TCellTag cellTag,
        bool reliable = true);
    void PostToMasters(
        const TCrossCellMessage& message,
        const NObjectClient::TCellTagList& cellTags,
        bool reliable = true);
    void PostToSecondaryMasters(
        const TCrossCellMessage& message,
        bool reliable = true);

    //! Returns |true| if there is a registered master cell with a given cell tag.
    bool IsRegisteredMasterCell(NObjectClient::TCellTag cellTag);

    //! Returns the list of cell tags for all registered master cells (other than the local one),
    //! in a stable order.
    /*!
     *  For secondary masters, the primary master is always the first element.
     */
    const NObjectClient::TCellTagList& GetRegisteredMasterCellTags();

    //! Returns a stable index of a given (registered) master cell (other than the local one).
    int GetRegisteredMasterCellIndex(NObjectClient::TCellTag cellTag);

    //! Picks a random (but deterministically chosen) secondary master cell for
    //! a new chunk owner node. Cells with less-than-average number of chunks are preferred.
    //! If no secondary cells are registered then #InvalidCellTag is returned.
    NObjectClient::TCellTag PickSecondaryMasterCell();

    //! Computes the total cluster statistics by summing counters for all cells (including primary).
    NProto::TCellStatistics ComputeClusterStatistics();

    //! Returns the channel to be used for communicating with another master.
    //! This channel has a properly configured timeout.
    //! Throws on error.
    NRpc::IChannelPtr GetMasterChannelOrThrow(NObjectClient::TCellTag cellTag, NHydra::EPeerKind peerKind);

    //! Same as #GetMasterChannelOrThrow but returns |nullptr| if no channel is currently known.
    NRpc::IChannelPtr FindMasterChannel(NObjectClient::TCellTag cellTag, NHydra::EPeerKind peerKind);

    DECLARE_SIGNAL(void(NObjectClient::TCellTag), ValidateSecondaryMasterRegistration);
    DECLARE_SIGNAL(void(NObjectClient::TCellTag), ReplicateKeysToSecondaryMaster);
    DECLARE_SIGNAL(void(NObjectClient::TCellTag), ReplicateValuesToSecondaryMaster);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TMulticellManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
