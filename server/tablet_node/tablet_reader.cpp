#include "tablet_reader.h"
#include "private.h"
#include "config.h"
#include "partition.h"
#include "store.h"
#include "tablet.h"
#include "tablet_slot.h"

#include <yt/ytlib/table_client/row_merger.h>
#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/versioned_reader.h>
#include <yt/ytlib/table_client/versioned_row.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/chunked_memory_pool.h>
#include <yt/core/misc/common.h>
#include <yt/core/misc/heap.h>
#include <yt/core/misc/small_vector.h>

#include <atomic>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NTableClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletNodeLogger;
static const size_t MaxRowsPerRead = 1024;
static const int TypicalStoresPerSession = 64;

////////////////////////////////////////////////////////////////////////////////
// NB(lukyan): Do not use delegating constructors. It leads to incorrect generated code in Visual C++ and memory corruption.

struct TTabletReaderPoolTag { };

namespace {
struct TSession
{
    IVersionedReaderPtr Reader;
    std::vector<TVersionedRow> Rows;
    std::vector<TVersionedRow>::iterator CurrentRow;
};

typedef SmallVector<TSession, TypicalStoresPerSession> TSessions;
typedef SmallVector<TSession*, TypicalStoresPerSession> TSessionPtrs;

TColumnFilter GetColumnFilter(const TTableSchema& schema, const TTableSchema& tabletSchema)
{
    // Infer column filter.
    TColumnFilter columnFilter;
    columnFilter.All = false;
    for (const auto& column : schema.Columns()) {
        const auto& tabletColumn = tabletSchema.GetColumnOrThrow(column.Name);
        if (tabletColumn.Type != column.Type) {
            THROW_ERROR_EXCEPTION("Invalid type of schema column %Qv: expected %Qlv, actual %Qlv",
                column.Name,
                tabletColumn.Type,
                column.Type);
        }
        columnFilter.Indexes.push_back(tabletSchema.GetColumnIndex(tabletColumn));
    }

    return columnFilter;
}

void TakePartition(
    std::vector<IStorePtr>* stores,
    const TPartitionSnapshotPtr& partitionSnapshot,
    TKey minKey,
    TKey maxKey)
{
    for (const auto& store : partitionSnapshot->Stores) {
        if (store->GetMinKey() <= maxKey && store->GetMaxKey() >= minKey) {
            stores->push_back(store);
        }
    }
}

void TakeLockedStores(
    std::vector<IStorePtr>* stores,
    const TTabletSnapshotPtr& tabletSnapshot)
{
    for (const auto& weakStore : tabletSnapshot->LockedStores) {
        auto store = weakStore.Lock();
        if (store) {
            stores->push_back(store);
        }
    }
}

} // namespace

template <class TMerger>
class TTabletReaderBase
    : public virtual TRefCounted
{
protected:
    TTabletReaderBase(
        TTabletPerformanceCountersPtr performanceCounters,
        const TDynamicRowKeyComparer& keyComparer)
        : KeyComparer_(keyComparer)
        , Merger_(KeyComparer_)
        , PerformanceCounters_(std::move(performanceCounters))
    { }
 
    template <class TRow, class TRowMerger>
    bool DoRead(std::vector<TRow>* rows, TRowMerger* rowMerger)
    {
        YCHECK(Opened_);
        YCHECK(!Refilling_);

        rows->clear();
        rowMerger->Reset();

        if (!ExhaustedSessions_.empty()) {
            // Prevent proceeding to the merge phase in presence of exhausted sessions.
            // Request refill and signal the user that he must wait.
            if (RefillExhaustedSessions()) {
                return true;
            }
        }

        // Refill sessions with newly arrived rows requested in RefillExhaustedSessions above.
        for (auto* session : RefillingSessions_) {
            RefillSession(session);
        }
        RefillingSessions_.clear();

        // Check for the end-of-rowset.
        if (!Merger_.HasActiveSessions()) {
            return false;
        }

        // Must stop once an exhausted session appears.
        while (ExhaustedSessions_.empty()) {
            // Fetch rows from all sessions with a matching key and merge them.
            Merger_.FetchMatchingRows(&ExhaustedSessions_, rowMerger);

            // Save merged row.
            auto mergedRow = rowMerger->BuildMergedRow();
            if (mergedRow) {
                rows->push_back(mergedRow);
            }
        }

        PerformanceCounters_->MergedRowReadCount += rows->size();

        return true;
    }

    bool RefillSession(TSession* session)
    {
        auto& rows = session->Rows;
        bool hasMoreRows = session->Reader->Read(&rows);

        if (rows.empty()) {
            return !hasMoreRows;
        }

        int rowCount = rows.size();
        PerformanceCounters_->UnmergedRowReadCount += rowCount;

        session->CurrentRow = rows.begin();

        Merger_.AddSessionToActive(session);
        return true;
    }

    bool RefillExhaustedSessions()
    {
        YCHECK(RefillingSessions_.empty());

        std::vector<TFuture<void>> asyncResults;
        for (auto* session : ExhaustedSessions_) {
            // Try to refill the session right away.
            if (!RefillSession(session)) {
                // No data at the moment, must wait.
                asyncResults.push_back(session->Reader->GetReadyEvent());
                RefillingSessions_.push_back(session);
            }
        }
        ExhaustedSessions_.clear();

        if (asyncResults.empty()) {
            return false;
        }

        Refilling_ = true;
        ReadyEvent_ = Combine(asyncResults).Apply(BIND([=, this_ = MakeStrong(this)] (const TError& error) {
            Refilling_ = false;
            error.ThrowOnError();
        }));

        return true;
    }

    void AddReader(IVersionedReaderPtr reader)
    {
        if (reader) {
            Sessions_.push_back(TSession());
            auto& session = Sessions_.back();
            session.Reader = std::move(reader);
            session.Rows.reserve(MaxRowsPerRead);
        }
    }

    // TODO(lukyan): Remove it after removeing method Open in IVersionedReader
    void DoOpen()
    {
        Merger_.Init(Sessions_.size());

        // Open readers.
        std::vector<TFuture<void>> asyncResults;
        for (const auto& session : Sessions_) {
            auto asyncResult = session.Reader->Open();
            auto maybeResult = asyncResult.TryGet();
            if (maybeResult) {
                maybeResult->ThrowOnError();
            } else {
                asyncResults.push_back(asyncResult);
            }
        }

        WaitFor(Combine(asyncResults))
            .ThrowOnError();

        // Mark all sessions as exhausted.
        for (auto& session : Sessions_) {
            ExhaustedSessions_.push_back(&session);
        }

        Opened_ = true;
    }

protected:
    TFuture<void> ReadyEvent_ = VoidFuture;
    const TDynamicRowKeyComparer KeyComparer_;

private:    
    TMerger Merger_;

    const TTabletPerformanceCountersPtr PerformanceCounters_;

    TSessions Sessions_;

    SmallVector<TSession*, TypicalStoresPerSession> ExhaustedSessions_;
    SmallVector<TSession*, TypicalStoresPerSession> RefillingSessions_;

    std::atomic<bool> Opened_ = {false};
    std::atomic<bool> Refilling_ = {false};

};

class TSessionComparer
{
public:
    explicit TSessionComparer(const TDynamicRowKeyComparer& keyComparer)
        : KeyComparer_(keyComparer)
    { }

    bool operator()(const TSession* lhsSession, const TSession* rhsSession) const
    {
        auto lhsRow = *lhsSession->CurrentRow;
        auto rhsRow = *rhsSession->CurrentRow;
        return KeyComparer_(
            lhsRow.BeginKeys(), lhsRow.EndKeys(),
            rhsRow.BeginKeys(), rhsRow.EndKeys()) < 0;
    }

protected:
    const TDynamicRowKeyComparer& KeyComparer_;
};

class THeapMerger
    : private TSessionComparer
{
public:
    explicit THeapMerger(const TDynamicRowKeyComparer& keyComparer)
        : TSessionComparer(keyComparer)
    { }

    void Init(int maxSessionCount)
    {
        ActiveSessions_.reserve(maxSessionCount);
        ActiveSessionsBegin_ = ActiveSessionsEnd_ = ActiveSessions_.begin();
    }

    bool HasActiveSessions() const
    {
        return ActiveSessionsBegin_ != ActiveSessionsEnd_;
    }

    void AddSessionToActive(TSession* session)
    {
        *ActiveSessionsEnd_++ = session;
        AdjustHeapBack(ActiveSessionsBegin_, ActiveSessionsEnd_, GetSessionComparer());
    }

    template <class TRowMerger>
    void FetchMatchingRows(TSessionPtrs* exhausted, TRowMerger* rowMerger)
    {
        const TUnversionedValue* currentKeyBegin = nullptr;
        const TUnversionedValue* currentKeyEnd = nullptr;
        // Advance current rows in sessions.
        // Check for exhausted sessions.
        while (ActiveSessionsBegin_ != ActiveSessionsEnd_) {
            auto* session = *ActiveSessionsBegin_;
            auto partialRow = *session->CurrentRow;

            if (currentKeyBegin) {
                if (KeyComparer_(
                        partialRow.BeginKeys(), partialRow.EndKeys(),
                        currentKeyBegin, currentKeyEnd) != 0)
                    break;
            } else {
                currentKeyBegin = partialRow.BeginKeys();
                currentKeyEnd = partialRow.EndKeys();
            }

            rowMerger->AddPartialRow(partialRow);

            if (++session->CurrentRow == session->Rows.end()) {
                exhausted->push_back(session);
                ExtractHeap(ActiveSessionsBegin_, ActiveSessionsEnd_, GetSessionComparer());
                --ActiveSessionsEnd_;
            } else {
                #ifndef NDEBUG
                Y_ASSERT(KeyComparer_(
                    partialRow.BeginKeys(), partialRow.EndKeys(),
                    session->CurrentRow->BeginKeys(), session->CurrentRow->EndKeys()) < 0);
                #endif
                AdjustHeapFront(ActiveSessionsBegin_, ActiveSessionsEnd_, GetSessionComparer());
            }
        }
    }

private:
    const TSessionComparer& GetSessionComparer() const
    {
        return *this;
    }

    TSessionPtrs ActiveSessions_;
    TSessionPtrs::iterator ActiveSessionsBegin_;
    TSessionPtrs::iterator ActiveSessionsEnd_;

};

class TSimpleMerger
{
public:
    explicit TSimpleMerger(const TDynamicRowKeyComparer& keyComparer)
    { }

    void Init(int maxSessionCount)
    {
        ActiveSessions_.reserve(maxSessionCount);
        ActiveSessionsBegin_ = ActiveSessionsEnd_ = ActiveSessions_.begin();
    }

    bool HasActiveSessions() const
    {
        return ActiveSessionsBegin_ != ActiveSessionsEnd_;
    }

    void AddSessionToActive(TSession* session)
    {
        *ActiveSessionsEnd_++ = session;
    }

    template <class TRowMerger>
    void FetchMatchingRows(TSessionPtrs* exhausted, TRowMerger* rowMerger)
    {
        auto it = ActiveSessionsBegin_;
        while (it != ActiveSessionsEnd_) {
            auto* session = *it;
            auto partialRow = *session->CurrentRow;
            rowMerger->AddPartialRow(partialRow);

            if (++session->CurrentRow == session->Rows.end()) {
                exhausted->push_back(session);
                --ActiveSessionsEnd_;
                std::swap(*it, *ActiveSessionsEnd_);
            } else {
                ++it;
            }
        }
    }

private:
    TSessionPtrs ActiveSessions_;
    TSessionPtrs::iterator ActiveSessionsBegin_;
    TSessionPtrs::iterator ActiveSessionsEnd_;

};

////////////////////////////////////////////////////////////////////////////////

class TTabletRangeReader
    : public ISchemafulReader
    , protected TTabletReaderBase<THeapMerger>
{
public:
    TTabletRangeReader(
        TTabletPerformanceCountersPtr performanceCounters,
        const TDynamicRowKeyComparer& keyComparer)
        : TBase(performanceCounters, keyComparer)
        , Pool_(TTabletReaderPoolTag())
    { }

    static ISchemafulReaderPtr Create(
        TTabletSnapshotPtr tabletSnapshot,
        const TTableSchema& schema,
        TOwningKey lowerBound,
        TOwningKey upperBound,
        TTimestamp timestamp,
        const TWorkloadDescriptor& workloadDescriptor)
    {
        // Select stores.
        std::vector<IStorePtr> stores;

        TakePartition(&stores, tabletSnapshot->Eden, lowerBound.Get(), upperBound.Get());

        auto range = tabletSnapshot->GetIntersectingPartitions(lowerBound, upperBound);
        for (auto it = range.first; it != range.second; ++it) {
            TakePartition(&stores, *it, lowerBound.Get(), upperBound.Get());
        }

        TakeLockedStores(&stores, tabletSnapshot);

        LOG_DEBUG("Creating schemaful tablet reader (TabletId: %v, CellId: %v, Timestamp: %v, StoreIds: [%v], WorkloadDescriptor: %v)",
            tabletSnapshot->TabletId,
            tabletSnapshot->Slot->GetCellId(),
            timestamp,
            JoinToString(stores, TStoreIdFormatter()),
            workloadDescriptor);

        if (stores.size() > tabletSnapshot->Config->MaxReadFanIn) {
            THROW_ERROR_EXCEPTION("Read fan-in limit exceeded; please wait until your data is merged")
                << TErrorAttribute("tablet_id", tabletSnapshot->TabletId)
                << TErrorAttribute("fan_in", stores.size())
                << TErrorAttribute("fan_in_limit", tabletSnapshot->Config->MaxReadFanIn);
        }

        auto columnFilter = GetColumnFilter(schema, tabletSnapshot->Schema);

        auto result = New<TTabletRangeReader>(
            tabletSnapshot->PerformanceCounters,
            tabletSnapshot->RowKeyComparer);

        // Create readers.
        for (const auto& store : stores) {
            result->AddReader(store->CreateReader(
                lowerBound,
                upperBound,
                timestamp,
                columnFilter,
                workloadDescriptor));
        }

        result->RowMerger_ = std::make_unique<TSchemafulRowMerger>(
            &result->Pool_,
            tabletSnapshot->Schema.Columns().size(),
            tabletSnapshot->KeyColumns.size(),
            columnFilter);

        result->DoOpen();

        return result;
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        return DoRead(rows, RowMerger_.get());
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return ReadyEvent_;
    }

private:
    typedef TTabletReaderBase<THeapMerger> TBase;

    TChunkedMemoryPool Pool_;
    std::unique_ptr<TSchemafulRowMerger> RowMerger_;

};

class TTabletKeysReader
    : public ISchemafulReader
    , protected TTabletReaderBase<TSimpleMerger>
{
public:
    TTabletKeysReader(
        TTabletPerformanceCountersPtr performanceCounters,
        const TDynamicRowKeyComparer& keyComparer)
        : TBase(performanceCounters, keyComparer)
        , Pool_(TTabletReaderPoolTag())
    { }

    static ISchemafulReaderPtr Create(
        TTabletSnapshotPtr tabletSnapshot,
        const TTableSchema& schema,
        TSharedRange <TKey> keys,
        TTimestamp timestamp,
        std::vector<IStorePtr> stores,
        const TWorkloadDescriptor& workloadDescriptor)
    {
        LOG_DEBUG("Creating schemaful tablet reader (TabletId: %v, CellId: %v, Timestamp: %v, StoreIds: [%v], WorkloadDescriptor: %v)",
            tabletSnapshot->TabletId,
            tabletSnapshot->Slot->GetCellId(),
            timestamp,
            JoinToString(stores, TStoreIdFormatter()),
            workloadDescriptor);

        if (stores.size() > tabletSnapshot->Config->MaxReadFanIn) {
            THROW_ERROR_EXCEPTION("Read fan-in limit exceeded; please wait until your data is merged")
                << TErrorAttribute("tablet_id", tabletSnapshot->TabletId)
                << TErrorAttribute("fan_in", stores.size())
                << TErrorAttribute("fan_in_limit", tabletSnapshot->Config->MaxReadFanIn);
        }

        auto columnFilter = GetColumnFilter(schema, tabletSnapshot->Schema);

        auto result = New<TTabletKeysReader>(
            tabletSnapshot->PerformanceCounters,
            tabletSnapshot->RowKeyComparer);

        // Create readers.
        for (const auto& store : stores) {
            result->AddReader(store->CreateReader(
                keys,
                timestamp,
                columnFilter,
                workloadDescriptor));
        }

        result->RowMerger_ = std::make_unique<TSchemafulRowMerger>(
            &result->Pool_,
            tabletSnapshot->Schema.Columns().size(),
            tabletSnapshot->KeyColumns.size(),
            columnFilter);

        result->DoOpen();

        return result;
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        return DoRead(rows, RowMerger_.get());
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return ReadyEvent_;
    }

private:
    typedef TTabletReaderBase<TSimpleMerger> TBase;

    TChunkedMemoryPool Pool_;
    std::unique_ptr<TSchemafulRowMerger> RowMerger_;

};

ISchemafulReaderPtr CreateSchemafulTabletReader(
    TTabletSnapshotPtr tabletSnapshot,
    const TTableSchema& schema,
    TOwningKey lowerBound,
    TOwningKey upperBound,
    TTimestamp timestamp,
    const TWorkloadDescriptor& workloadDescriptor)
{
    return TTabletRangeReader::Create(
        std::move(tabletSnapshot),
        schema,
        std::move(lowerBound),
        std::move(upperBound),
        timestamp,
        workloadDescriptor);
}

ISchemafulReaderPtr CreateSchemafulTabletReader(
    TTabletSnapshotPtr tabletSnapshot,
    const TTableSchema& schema,
    const TSharedRange<TKey>& keys,
    TTimestamp timestamp,
    const TWorkloadDescriptor& workloadDescriptor)
{
    TKey minKey;
    TKey maxKey;

    for (const auto& key : keys) {
        minKey = !minKey || key < minKey ? key : minKey;
        maxKey = !maxKey || key > maxKey ? key : maxKey;
    }

    // Select stores.
    std::vector<IStorePtr> stores;

    TakePartition(&stores, tabletSnapshot->Eden, minKey, maxKey);

    std::vector<TPartitionSnapshotPtr> snapshots;
    for (auto key : keys) {
        snapshots.push_back(tabletSnapshot->FindContainingPartition(key));
    }

    std::sort(snapshots.begin(), snapshots.end());
    snapshots.erase(std::unique(snapshots.begin(), snapshots.end()), snapshots.end());

    for (const auto& snapshot : snapshots) {
        TakePartition(&stores, snapshot, minKey, maxKey);
    }

    return TTabletKeysReader::Create(
        std::move(tabletSnapshot),
        schema,
        std::move(keys),
        timestamp,
        std::move(stores),
        workloadDescriptor);
}

ISchemafulReaderPtr CreateSchemafulTabletReader(
    TTabletSnapshotPtr tabletSnapshot,
    const TTableSchema& schema,
    TPartitionSnapshotPtr paritionSnapshot,
    const TSharedRange<TKey>& keys,
    TTimestamp timestamp,
    const TWorkloadDescriptor& workloadDescriptor)
{
    YCHECK(keys.Size() > 0);

    TKey minKey = keys[0];
    TKey maxKey = keys[keys.Size() - 1];

    // Select stores.
    std::vector<IStorePtr> stores;

    TakePartition(&stores, tabletSnapshot->Eden, minKey, maxKey);
    TakePartition(&stores, paritionSnapshot, minKey, maxKey);
    TakeLockedStores(&stores, tabletSnapshot);

    return TTabletKeysReader::Create(
        std::move(tabletSnapshot),
        schema,
        std::move(keys),
        timestamp,
        std::move(stores),
        workloadDescriptor);
}

////////////////////////////////////////////////////////////////////////////////

class TVersionedTabletReader
    : public IVersionedReader
    , public TTabletReaderBase<THeapMerger>
{
public:
    TVersionedTabletReader(
        IInvokerPtr poolInvoker,
        TTabletSnapshotPtr tabletSnapshot,
        std::vector<IStorePtr> stores,
        TOwningKey lowerBound,
        TOwningKey upperBound,
        TTimestamp currentTimestamp,
        TTimestamp majorTimestamp,
        const TWorkloadDescriptor& workloadDescriptor)
         : TTabletReaderBase(
            tabletSnapshot->PerformanceCounters,
            tabletSnapshot->RowKeyComparer)
        , PoolInvoker_(std::move(poolInvoker))
        , TabletSnapshot_(std::move(tabletSnapshot))
        , Stores_(std::move(stores))
        , RowMerger_(
            &Pool_,
            TabletSnapshot_->KeyColumns.size(),
            TabletSnapshot_->Config,
            currentTimestamp,
            majorTimestamp)
        , LowerBound_(std::move(lowerBound))
        , UpperBound_(std::move(upperBound))
        , WorkloadDescriptor_(workloadDescriptor)
    { }

    virtual TFuture<void> Open() override
    {
        return BIND(&TVersionedTabletReader::DoOpen, MakeStrong(this))
            .AsyncVia(PoolInvoker_)
            .Run();
    }

    virtual bool Read(std::vector<TVersionedRow>* rows) override
    {
        bool result = DoRead(rows, &RowMerger_);
        #ifndef NDEBUG
        for (int index = 0; index < static_cast<int>(rows->size()) - 1; ++index) {
            auto lhs = (*rows)[index];
            auto rhs = (*rows)[index + 1];
            Y_ASSERT(KeyComparer_(
                lhs.BeginKeys(), lhs.EndKeys(),
                rhs.BeginKeys(), rhs.EndKeys()) < 0);
        }
        #endif
        return result;
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return ReadyEvent_;
    }

private:
    typedef TTabletReaderBase<THeapMerger> TBase;

    const IInvokerPtr PoolInvoker_;
    const TTabletSnapshotPtr TabletSnapshot_;

    const std::vector<IStorePtr> Stores_;
    TChunkedMemoryPool Pool_;
    TVersionedRowMerger RowMerger_;
    const TOwningKey LowerBound_;
    const TOwningKey UpperBound_;
    const TColumnFilter ColumnFilter_;
    const TWorkloadDescriptor WorkloadDescriptor_;
    

    void DoOpen()
    {
        LOG_DEBUG(
            "Creating versioned tablet reader (TabletId: %v, CellId: %v, LowerBound: {%v}, UpperBound: {%v}, "
            "CurrentTimestamp: %v, MajorTimestamp: %v, StoreIds: [%v])",
            TabletSnapshot_->TabletId,
            TabletSnapshot_->Slot->GetCellId(),
            LowerBound_,
            UpperBound_,
            RowMerger_.GetCurrentTimestamp(),
            RowMerger_.GetMajorTimestamp(),
            JoinToString(Stores_, TStoreIdFormatter()));

        for (const auto& store : Stores_) {
            this->AddReader(store->CreateReader(
                LowerBound_,
                UpperBound_,
                AllCommittedTimestamp,
                ColumnFilter_,
                WorkloadDescriptor_));
        }

        TBase::DoOpen();
    }

};

IVersionedReaderPtr CreateVersionedTabletReader(
    IInvokerPtr poolInvoker,
    TTabletSnapshotPtr tabletSnapshot,
    std::vector<IStorePtr> stores,
    TOwningKey lowerBound,
    TOwningKey upperBound,
    TTimestamp currentTimestamp,
    TTimestamp majorTimestamp,
    const TWorkloadDescriptor& workloadDescriptor)
{
    return New<TVersionedTabletReader>(
        std::move(poolInvoker),
        std::move(tabletSnapshot),
        std::move(stores),
        std::move(lowerBound),
        std::move(upperBound),
        currentTimestamp,
        majorTimestamp,
        workloadDescriptor);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT

