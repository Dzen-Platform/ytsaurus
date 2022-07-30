#pragma once

#include "public.h"

#include "object_detail.h"

#include <yt/yt/ytlib/journal_client/journal_hunk_chunk_writer.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class THunkStore
    : public TObjectBase
    , public TRefCounted
{
public:
    // Transient state.
    DEFINE_BYVAL_RW_PROPERTY(EHunkStoreState, State, EHunkStoreState::Undefined);

    DEFINE_BYVAL_RW_PROPERTY(bool, MarkedSealable);

    DEFINE_BYVAL_RW_PROPERTY(TInstant, CreationTime);
    DEFINE_BYVAL_RW_PROPERTY(TInstant, LastWriteTime);

public:
    THunkStore(TStoreId storeId, THunkTablet* tablet);

    TFuture<std::vector<NJournalClient::TJournalHunkDescriptor>> WriteHunks(
        std::vector<TSharedRef> payloads);

    void Save(TSaveContext& context) const;
    void Load(TLoadContext& context);

    void Lock(TTabletId tabletId);
    void Unlock(TTabletId tabetId);
    bool IsLockedByTablet(TTabletId tabletId) const;

    void Lock(TTransactionId transactionId, EHunkStoreLockMode lockMode);
    void Unlock(TTransactionId transactionId, EHunkStoreLockMode lockMode);
    bool IsLocked() const;

    void SetWriter(NJournalClient::IJournalHunkChunkWriterPtr writer);
    const NJournalClient::IJournalHunkChunkWriterPtr GetWriter() const;
    bool IsReadyToWrite() const;

    void BuildOrchidYson(NYson::IYsonConsumer* consumer) const;

private:
    const THunkTablet* const Tablet_;

    const NLogging::TLogger Logger;

    THashMap<TTabletId, int> TabletIdToLockCount_;

    TTransactionId ExclusiveLockTransactionId_;
    THashSet<TTransactionId> SharedLockTransactionIds_;

    NJournalClient::IJournalHunkChunkWriterPtr Writer_;
    TFuture<void> WriterOpenedFuture_;

    bool IsMutationLoggingEnabled() const;
};

DEFINE_REFCOUNTED_TYPE(THunkStore)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
