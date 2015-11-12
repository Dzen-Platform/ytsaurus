#pragma once

#include "private.h"
#include "dynamic_memory_store_bits.h"
#include "dynamic_memory_store_comparer.h"
#include "store_detail.h"
#include "transaction.h"

#include <yt/ytlib/chunk_client/chunk_meta.pb.h>

#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/versioned_row.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/actions/signal.h>

#include <yt/core/misc/chunked_vector.h>
#include <yt/core/misc/property.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TRowBlockedException
    : public std::exception
{
public:
    TRowBlockedException(
        TDynamicMemoryStorePtr store,
        TDynamicRow row,
        ui32 lockMask,
        TTimestamp timestamp)
        : Store_(std::move(store))
        , Row_(row)
        , LockMask_(lockMask)
        , Timestamp_(timestamp)
    { }

    DEFINE_BYVAL_RO_PROPERTY(TDynamicMemoryStorePtr, Store);
    DEFINE_BYVAL_RO_PROPERTY(TDynamicRow, Row);
    DEFINE_BYVAL_RO_PROPERTY(ui32, LockMask);
    DEFINE_BYVAL_RO_PROPERTY(TTimestamp, Timestamp);

};

////////////////////////////////////////////////////////////////////////////////

class TDynamicMemoryStore
    : public TStoreBase
{
public:
    DEFINE_BYVAL_RW_PROPERTY(EStoreFlushState, FlushState);

public:
    TDynamicMemoryStore(
        TTabletManagerConfigPtr config,
        const TStoreId& id,
        TTablet* tablet);

    ~TDynamicMemoryStore();


    //! Sets the store state, as expected.
    //! Additionally, when the store transitions from |ActiveDynamic| to |PassiveDynamic|,
    //! its current revision is stored for future use in #CreateFlushReader.
    virtual void SetStoreState(EStoreState state);

    //! Returns the reader to be used during flush.
    NTableClient::IVersionedReaderPtr CreateFlushReader();

    //! Returns the reader to be used during store serialization.
    NTableClient::IVersionedReaderPtr CreateSnapshotReader();


    //! Returns the cached instance of row key comparer
    //! (obtained by calling TTablet::GetRowKeyComparer).
    const TDynamicRowKeyComparer& GetRowKeyComparer() const;

    int GetLockCount() const;
    int Lock();
    int Unlock();

    //! Checks if a given #row has any locks from #lockMask with prepared timestamp
    //! less that #timestamp. If so, raises |RowBlocked| signal and loops.
    void WaitOnBlockedRow(
        TDynamicRow row,
        ui32 lockMask,
        TTimestamp timestamp);

    //! Writes the row taking the needed locks.
    /*!
     *  Only applies to atomic transactions.
     *
     *  On lock failure, throws TErrorException explaining the cause.
     *  If a blocked row is encountered, throws TRowBlockedException.
     */
    TDynamicRow WriteRowAtomic(
        TTransaction* transaction,
        NTableClient::TUnversionedRow row,
        bool prelock,
        ui32 lockMask);

    //! Writes and immediately commits the row.
    /*!
     *  Only applies to non-atomic transactions. No locks are checked or taken.
     */
    TDynamicRow WriteRowNonAtomic(
        NTableClient::TUnversionedRow row,
        TTimestamp commitTimestamp);

    //! Deletes the row taking the needed locks.
    /*!
     *  Only applies to atomic transactions.
     *
     *  On lock failure, throws TErrorException explaining the cause.
     *  If a blocked row is encountered, throws TRowBlockedException.
     */
    TDynamicRow DeleteRowAtomic(
        TTransaction* transaction,
        TKey key,
        bool prelock);

    //! Deletes and immediately commits the row.
    /*!
     *  Only applies to non-atomic transactions. No locks are checked or taken.
     */
    TDynamicRow DeleteRowNonAtomic(
        TKey key,
        TTimestamp commitTimestamp);

    TDynamicRow MigrateRow(
        TTransaction* transaction,
        TDynamicRow row);

    void ConfirmRow(TTransaction* transaction, TDynamicRow row);
    void PrepareRow(TTransaction* transaction, TDynamicRow row);
    void CommitRow(TTransaction* transaction, TDynamicRow row);
    void AbortRow(TTransaction* transaction, TDynamicRow row);

    // The following functions are made public for unit-testing.
    TDynamicRow FindRow(NTableClient::TUnversionedRow key);
    std::vector<TDynamicRow> GetAllRows();
    TTimestamp TimestampFromRevision(ui32 revision);
    TTimestamp GetLastCommitTimestamp(TDynamicRow row, int lockIndex);

    int GetValueCount() const;
    int GetKeyCount() const;

    i64 GetPoolSize() const;
    i64 GetPoolCapacity() const;

    // IStore implementation.
    virtual EStoreType GetType() const override;

    virtual i64 GetUncompressedDataSize() const override;
    virtual i64 GetRowCount() const override;

    virtual TOwningKey GetMinKey() const override;
    virtual TOwningKey GetMaxKey() const override;

    virtual TTimestamp GetMinTimestamp() const override;
    virtual TTimestamp GetMaxTimestamp() const override;

    virtual NTableClient::IVersionedReaderPtr CreateReader(
        TOwningKey lowerKey,
        TOwningKey upperKey,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter) override;

    virtual NTableClient::IVersionedReaderPtr CreateReader(
        const TSharedRange<TKey>& keys,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter) override;

    virtual void CheckRowLocks(
        TUnversionedRow row,
        TTransaction* transaction,
        ui32 lockMask) override;

    virtual void Save(TSaveContext& context) const override;
    virtual void Load(TLoadContext& context) override;

    virtual TCallback<void(TSaveContext&)> AsyncSave() override;
    virtual void AsyncLoad(TLoadContext& context) override;

    virtual void BuildOrchidYson(NYson::IYsonConsumer* consumer) override;

    DEFINE_SIGNAL(void(TDynamicRow row, int lockIndex), RowBlocked)

private:
    class TReaderBase;
    class TRangeReader;
    class TLookupReader;

    const TTabletManagerConfigPtr Config_;

    ui32 FlushRevision_ = InvalidRevision;

    int StoreLockCount_ = 0;
    int StoreValueCount_ = 0;

    TDynamicRowKeyComparer RowKeyComparer_;

    NTableClient::TRowBufferPtr RowBuffer_;
    std::unique_ptr<TSkipList<TDynamicRow, TDynamicRowKeyComparer>> Rows_;

    TTimestamp MinTimestamp_ = NTransactionClient::MaxTimestamp;
    TTimestamp MaxTimestamp_ = NTransactionClient::MinTimestamp;

    static const size_t RevisionsPerChunk = 1ULL << 13;
    static const size_t MaxRevisionChunks = HardRevisionsPerDynamicMemoryStoreLimit / RevisionsPerChunk + 1;
    TChunkedVector<TTimestamp, RevisionsPerChunk> RevisionToTimestamp_;


    TDynamicRow AllocateRow();

    int GetBlockingLockIndex(
        TDynamicRow row,
        ui32 lockMask,
        TTimestamp timestamp);
    void ValidateRowNotBlocked(
        TDynamicRow row,
        ui32 lockMask,
        TTimestamp timestamp);

    void CheckRowLocks(
        TDynamicRow row,
        TTransaction* transaction,
        ui32 lockMask);
    void AcquireRowLocks(
        TDynamicRow row,
        TTransaction* transaction,
        bool prelock,
        ui32 lockMask,
        bool deleteFlag);

    TValueList PrepareFixedValue(TDynamicRow row, int index);
    void AddDeleteRevision(TDynamicRow row, ui32 revision);
    void AddWriteRevision(TLockDescriptor& lock, ui32 revision);
    void AddDeleteRevisionNonAtomic(TDynamicRow row, TTimestamp commitTimestamp, ui32 commitRevision);
    void AddWriteRevisionNonAtomic(TDynamicRow row, TTimestamp commitTimestamp, ui32 commitRevision);
    void SetKeys(TDynamicRow dstRow, TUnversionedValue* srcKeys);
    void SetKeys(TDynamicRow dstRow, TDynamicRow srcRow);

    struct TLoadScratchData
    {
        yhash_map<TTimestamp, ui32> TimestampToRevision;
        std::vector<std::vector<ui32>> WriteRevisions;
    };

    void LoadRow(TVersionedRow row, TLoadScratchData* scratchData);
    ui32 CaptureTimestamp(TTimestamp timestamp, TLoadScratchData* scratchData);
    ui32 CaptureVersionedValue(TDynamicValue* dst, const TVersionedValue& src, TLoadScratchData* scratchData);

    void CaptureUncommittedValue(TDynamicValue* dst, const TDynamicValue& src, int index);
    void CaptureUnversionedValue(TDynamicValue* dst, const TUnversionedValue& src);
    TDynamicValueData CaptureStringValue(TDynamicValueData src);
    TDynamicValueData CaptureStringValue(const TUnversionedValue& src);

    ui32 GetLatestRevision() const;
    ui32 RegisterRevision(TTimestamp timestamp);
    void UpdateTimestampRange(TTimestamp commitTimestamp);

    void OnMemoryUsageUpdated();

};

DEFINE_REFCOUNTED_TYPE(TDynamicMemoryStore)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
