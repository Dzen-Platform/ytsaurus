#pragma once

#include "public.h"
#include "object_detail.h"
#include "dynamic_store_bits.h"

#include <yt/yt/server/lib/transaction_supervisor/transaction_detail.h>

#include <yt/yt/ytlib/transaction_client/public.h>

#include <yt/yt/core/actions/future.h>

#include <yt/yt/core/concurrency/public.h>

#include <yt/yt/core/rpc/authentication_identity.h>

#include <yt/yt/core/misc/persistent_queue.h>
#include <yt/yt/core/misc/property.h>
#include <yt/yt/core/misc/ref_tracked.h>
#include <yt/yt/core/misc/ring_queue.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

struct TTransactionWriteRecord
{
    TTransactionWriteRecord() = default;
    TTransactionWriteRecord(
        TTabletId tabletId,
        TSharedRef data,
        int rowCount,
        size_t byteSize,
        const TSyncReplicaIdList& syncReplicaIds);

    TTabletId TabletId;
    TSharedRef Data;
    int RowCount = 0;
    size_t DataWeight = 0;
    TSyncReplicaIdList SyncReplicaIds;

    void Save(TSaveContext& context) const;
    void Load(TLoadContext& context);

    i64 GetByteSize() const;
};

constexpr size_t TransactionWriteLogChunkSize = 256;
using TTransactionWriteLog = TPersistentQueue<TTransactionWriteRecord, TransactionWriteLogChunkSize>;
using TTransactionWriteLogSnapshot = TPersistentQueueSnapshot<TTransactionWriteRecord, TransactionWriteLogChunkSize>;

i64 GetWriteLogRowCount(const TTransactionWriteLog& writeLog);

////////////////////////////////////////////////////////////////////////////////

class TTransaction
    : public NTransactionSupervisor::TTransactionBase<TObjectBase>
    , public TRefTracked<TTransaction>
{
public:
    DEFINE_BYVAL_RW_PROPERTY(bool, Transient);
    DEFINE_BYVAL_RW_PROPERTY(bool, Foreign);
    DEFINE_BYVAL_RW_PROPERTY(bool, HasLease);
    DEFINE_BYVAL_RW_PROPERTY(TDuration, Timeout);

    DEFINE_BYVAL_RW_PROPERTY(TTimestamp, StartTimestamp, NullTimestamp);
    DEFINE_BYVAL_RW_PROPERTY(TTimestamp, PrepareTimestamp, NullTimestamp);
    DEFINE_BYVAL_RW_PROPERTY(TTimestamp, CommitTimestamp, NullTimestamp);
    DEFINE_BYVAL_RW_PROPERTY(NHydra::TRevision, PrepareRevision, NHydra::NullRevision);
    DEFINE_BYVAL_RW_PROPERTY(NObjectClient::TCellTag, CommitTimestampClusterTag, NObjectClient::InvalidCellTag)

    DEFINE_BYREF_RW_PROPERTY(TRingQueue<TSortedDynamicRowRef>, PrelockedRows);
    DEFINE_BYREF_RW_PROPERTY(std::vector<TSortedDynamicRowRef>, LockedRows);

    DEFINE_BYREF_RW_PROPERTY(std::vector<TTablet*>, LockedTablets);

    DEFINE_BYREF_RW_PROPERTY(THashSet<TTabletId>, TabletsToUpdateReplicationProgress);
    DEFINE_BYVAL_RW_PROPERTY(bool, SerializationForced);

    DEFINE_BYREF_RW_PROPERTY(TTransactionWriteLog, ImmediateLockedWriteLog);
    DEFINE_BYREF_RW_PROPERTY(TTransactionWriteLog, ImmediateLocklessWriteLog);
    DEFINE_BYREF_RW_PROPERTY(TTransactionWriteLog, DelayedLocklessWriteLog);

    DEFINE_BYREF_RW_PROPERTY(TTransactionSignature, PersistentPrepareSignature, InitialTransactionSignature);
    DEFINE_BYREF_RW_PROPERTY(TTransactionSignature, TransientPrepareSignature, InitialTransactionSignature);
    DEFINE_BYVAL_RW_PROPERTY(TTransactionGeneration, PersistentGeneration, InitialTransactionGeneration);
    DEFINE_BYVAL_RW_PROPERTY(TTransactionGeneration, TransientGeneration, InitialTransactionGeneration);

    DEFINE_BYREF_RW_PROPERTY(TTransactionSignature, CommitSignature, InitialTransactionSignature);

    DEFINE_BYREF_RW_PROPERTY(NTransactionSupervisor::TTransactionCommitOptions, CommitOptions);

    DEFINE_BYVAL_RW_PROPERTY(bool, RowsPrepared, false);
    DEFINE_BYREF_RW_PROPERTY(NRpc::TAuthenticationIdentity, AuthenticationIdentity);

public:
    explicit TTransaction(TTransactionId id);

    void Save(TSaveContext& context) const;
    void Load(TLoadContext& context);

    TCallback<void(TSaveContext&)> AsyncSave();
    void AsyncLoad(TLoadContext& context);

    TFuture<void> GetFinished() const;
    void SetFinished();
    void ResetFinished();

    TTimestamp GetPersistentPrepareTimestamp() const;

    TInstant GetStartTime() const;

    bool IsSerializationNeeded() const;

    NObjectClient::TCellTag GetCellTag() const;

private:
    TPromise<void> Finished_ = NewPromise<void>();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
