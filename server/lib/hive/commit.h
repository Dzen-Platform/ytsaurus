#pragma once

#include "public.h"

#include <yt/server/lib/hydra/entity_map.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/client/hive/timestamp_map.h>

#include <yt/core/actions/future.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/ref_tracked.h>

#include <yt/core/rpc/public.h>
#include <yt/client/api/public.h>

namespace NYT::NHiveServer {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ECommitState,
    ((Start)                     (0))
    ((Prepare)                   (1))
    ((GeneratingCommitTimestamps)(2)) // transient only
    ((Commit)                    (3))
    ((Aborting)                  (4)) // transient only
    ((Abort)                     (5))
    ((Finishing)                 (6)) // transient only
);

class TCommit
    : public NHydra::TEntityBase
    , public TRefTracked<TCommit>
{
public:
    DEFINE_BYVAL_RO_PROPERTY(TTransactionId, TransactionId);
    DEFINE_BYVAL_RO_PROPERTY(NRpc::TMutationId, MutationId);
    DEFINE_BYREF_RO_PROPERTY(std::vector<TCellId>, ParticipantCellIds);
    DEFINE_BYVAL_RO_PROPERTY(bool, Distributed);
    DEFINE_BYVAL_RO_PROPERTY(bool, GeneratePrepareTimestamp);
    DEFINE_BYVAL_RO_PROPERTY(bool, InheritCommitTimestamp);
    DEFINE_BYVAL_RO_PROPERTY(NApi::ETransactionCoordinatorCommitMode, CoordinatorCommitMode);
    DEFINE_BYVAL_RW_PROPERTY(bool, Persistent);
    DEFINE_BYREF_RW_PROPERTY(NHiveClient::TTimestampMap, CommitTimestamps);
    DEFINE_BYVAL_RW_PROPERTY(ECommitState, TransientState, ECommitState::Start);
    DEFINE_BYVAL_RW_PROPERTY(ECommitState, PersistentState, ECommitState::Start);
    DEFINE_BYREF_RW_PROPERTY(THashSet<TCellId>, RespondedCellIds);
    DEFINE_BYVAL_RW_PROPERTY(TString, UserName);

public:
    explicit TCommit(TTransactionId transactionId);
    TCommit(
        TTransactionId transactionId,
        NRpc::TMutationId mutationId,
        const std::vector<TCellId>& participantCellIds,
        bool distributed,
        bool generatePrepareTimestamp,
        bool inheritCommitTimestamp,
        NApi::ETransactionCoordinatorCommitMode coordinatorCommitMode,
        const TString& userName);

    TFuture<TSharedRefArray> GetAsyncResponseMessage();
    void SetResponseMessage(TSharedRefArray message);

    void Save(NHydra::TSaveContext& context) const;
    void Load(NHydra::TLoadContext& context);

private:
    TPromise<TSharedRefArray> ResponseMessagePromise_ = NewPromise<TSharedRefArray>();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHiveServer
