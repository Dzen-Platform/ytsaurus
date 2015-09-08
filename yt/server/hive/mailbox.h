#pragma once

#include "public.h"

#include <core/misc/property.h>
#include <core/misc/ref_tracked.h>

#include <core/rpc/public.h>

#include <core/tracing/public.h>

#include <ytlib/hydra/hydra_manager.pb.h>

#include <server/hydra/entity_map.h>

#include <server/hive/hive_manager.pb.h>

namespace NYT {
namespace NHive {

////////////////////////////////////////////////////////////////////////////////

class TMailbox
    : public NHydra::TEntityBase
    , public TRefTracked<TMailbox>
{
public:
    // Persistent state.
    DEFINE_BYVAL_RO_PROPERTY(TCellId, CellId);

    DEFINE_BYVAL_RW_PROPERTY(int, FirstOutcomingMessageId);
    DEFINE_BYVAL_RW_PROPERTY(int, LastIncomingMessageId);
    DEFINE_BYVAL_RW_PROPERTY(bool, PostMessagesInFlight)

    DEFINE_BYREF_RW_PROPERTY(std::vector<NProto::TEncapsulatedMessage>, OutcomingMessages);
    
    typedef std::map<int, NProto::TEncapsulatedMessage> TIncomingMessageMap;
    DEFINE_BYREF_RW_PROPERTY(TIncomingMessageMap, IncomingMessages);

    // Transient state.
    DEFINE_BYVAL_RW_PROPERTY(bool, Connected);

    struct TSyncRequest
    {
        int MessageId;
        TPromise<void> Promise;
    };

    typedef std::map<int, TSyncRequest> TSyncRequestMap;
    DEFINE_BYREF_RW_PROPERTY(TSyncRequestMap, SyncRequests);

public:
    explicit TMailbox(const TCellId& cellId);

    void Save(NHydra::TSaveContext& context) const;
    void Load(NHydra::TLoadContext& context);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NHive
} // namespace NYT
