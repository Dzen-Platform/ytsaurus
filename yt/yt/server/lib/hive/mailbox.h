#pragma once

#include "public.h"

#include <yt/yt/server/lib/hive/proto/hive_manager.pb.h>

#include <yt/yt/server/lib/hydra/entity_map.h>

#include <yt/yt/ytlib/hive/proto/hive_service.pb.h>

#include <yt/yt/ytlib/hydra/proto/hydra_manager.pb.h>

#include <yt/yt/core/misc/property.h>
#include <yt/yt/core/misc/ref_tracked.h>

#include <yt/yt/core/concurrency/public.h>

#include <yt/yt/core/rpc/public.h>

#include <yt/yt/core/tracing/public.h>

namespace NYT::NHiveServer {

////////////////////////////////////////////////////////////////////////////////

class TMailbox
    : public NHydra::TEntityBase
    , public TRefTracked<TMailbox>
{
public:
    // Persistent state.
    DEFINE_BYVAL_RO_PROPERTY(TCellId, CellId);

    //! The id of the first message in |OutcomingMessages|.
    DEFINE_BYVAL_RW_PROPERTY(TMessageId, FirstOutcomingMessageId);

    struct TOutcomingMessage
    {
        TSerializedMessagePtr SerializedMessage;
        NTracing::TTraceContextPtr TraceContext;

        void Save(TStreamSaveContext& context) const;
        void Load(TStreamLoadContext& context);
    };

    //! Messages enqueued for the destination cell, ordered by id.
    DEFINE_BYREF_RW_PROPERTY(std::vector<TOutcomingMessage>, OutcomingMessages);

    //! The id of the next incoming message to be handled by Hydra.
    DEFINE_BYVAL_RW_PROPERTY(TMessageId, NextPersistentIncomingMessageId);

    // Transient state.
    DEFINE_BYVAL_RO_PROPERTY(TMailboxRuntimeDataPtr, RuntimeData);
    DEFINE_BYVAL_RW_PROPERTY(bool, Connected);
    DEFINE_BYVAL_RW_PROPERTY(bool, AcknowledgeInProgress);
    DEFINE_BYVAL_RW_PROPERTY(bool, PostInProgress);
    DEFINE_BYVAL_RW_PROPERTY(TMessageId, NextTransientIncomingMessageId);

    //! The id of the first message for which |PostMessages| request to the destination
    //! cell is still in progress. If no request is in progress then this is
    //! just the id of the first message to be sent.
    DEFINE_BYVAL_RW_PROPERTY(TMessageId, FirstInFlightOutcomingMessageId);
    //! The number of messages in the above request.
    //! If this value is zero then there is no in-flight request.
    DEFINE_BYVAL_RW_PROPERTY(int, InFlightOutcomingMessageCount);

    DEFINE_BYREF_RW_PROPERTY(NConcurrency::TDelayedExecutorCookie, IdlePostCookie);

    using TSyncRequestMap = std::map<TMessageId, TPromise<void>>;
    DEFINE_BYREF_RW_PROPERTY(TSyncRequestMap, SyncRequests);

    DEFINE_BYVAL_RW_PROPERTY(NRpc::IChannelPtr, CachedChannel);
    DEFINE_BYVAL_RW_PROPERTY(NProfiling::TCpuInstant, CachedChannelDeadline);

    DEFINE_BYVAL_RW_PROPERTY(NConcurrency::TDelayedExecutorCookie, PostBatchingCookie);

public:
    explicit TMailbox(TCellId cellId);

    void Save(NHydra::TSaveContext& context) const;
    void Load(NHydra::TLoadContext& context);

    void UpdateLastOutcomingMessageId();
};

////////////////////////////////////////////////////////////////////////////////

struct TMailboxRuntimeData
    : public TRefCounted
{
    std::atomic<i64> LastOutcomingMessageId = -1;
};

DEFINE_REFCOUNTED_TYPE(TMailboxRuntimeData)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveServer
