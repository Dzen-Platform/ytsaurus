#include "stdafx.h"
#include "tablet_reader.h"
#include "tablet.h"
#include "tablet_slot.h"
#include "partition.h"
#include "store.h"
#include "row_merger.h"
#include "config.h"
#include "private.h"

#include <core/misc/chunked_memory_pool.h>
#include <core/misc/small_vector.h>
#include <core/misc/heap.h>

#include <core/concurrency/scheduler.h>

#include <ytlib/new_table_client/versioned_row.h>
#include <ytlib/new_table_client/schemaful_reader.h>
#include <ytlib/new_table_client/versioned_reader.h>

#include <atomic>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NVersionedTableClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletNodeLogger;
static const size_t MaxRowsPerRead = 1024;
static const int TypicalStoresPerSession = 64;

////////////////////////////////////////////////////////////////////////////////

struct TTabletReaderPoolTag { };

class TTabletReaderBase
    : public virtual TRefCounted
{
public:
    TTabletReaderBase(
        IInvokerPtr poolInvoker,
        TTabletSnapshotPtr tabletSnapshot,
        TOwningKey lowerBound,
        TOwningKey upperBound,
        TTimestamp timestamp)
        : PoolInvoker_(std::move(poolInvoker))
        , TabletSnapshot_(std::move(tabletSnapshot))
        , PerformanceCounters_(TabletSnapshot_->PerformanceCounters)
        , LowerBound_(std::move(lowerBound))
        , UpperBound_(std::move(upperBound))
        , Timestamp_(timestamp)
        , Pool_(TTabletReaderPoolTag())
    { }

    TTabletReaderBase(
        IInvokerPtr poolInvoker,
        TTabletSnapshotPtr tabletSnapshot,
        const std::vector<TKey>& keys,
        TTimestamp timestamp)
        : PoolInvoker_(std::move(poolInvoker))
        , TabletSnapshot_(std::move(tabletSnapshot))
        , PerformanceCounters_(TabletSnapshot_->PerformanceCounters)
        , Keys_(keys)
        , Timestamp_(timestamp)
        , Pool_(TTabletReaderPoolTag())
    { }

protected:
    const IInvokerPtr PoolInvoker_;
    const TTabletSnapshotPtr TabletSnapshot_;
    const TTabletPerformanceCountersPtr PerformanceCounters_;
    const TOwningKey LowerBound_ = EmptyKey();
    const TOwningKey UpperBound_ = EmptyKey();
    const std::vector<TKey> Keys_;
    const TTimestamp Timestamp_;

    TChunkedMemoryPool Pool_;

    struct TSession
    {
        IVersionedReaderPtr Reader;
        std::vector<TVersionedRow> Rows;
        std::vector<TVersionedRow>::iterator CurrentRow;
    };

    SmallVector<TSession, TypicalStoresPerSession> Sessions_;

    typedef SmallVector<TSession*, TypicalStoresPerSession> TSessionHeap;
    TSessionHeap SessionHeap_;
    TSessionHeap::iterator SessionHeapBegin_;
    TSessionHeap::iterator SessionHeapEnd_;

    SmallVector<TSession*, TypicalStoresPerSession> ExhaustedSessions_;
    SmallVector<TSession*, TypicalStoresPerSession> RefillingSessions_;

    TFuture<void> ReadyEvent_ = VoidFuture;

    std::atomic<bool> Opened_ = {false};
    std::atomic<bool> Refilling_ = {false};


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
            RefillExhaustedSessions();
            YCHECK(ExhaustedSessions_.empty()); // must be cleared in RefillSessions
            return true;
        }

        // Refill sessions with newly arrived rows requested in RefillExhaustedSessions above.
        for (auto* session : RefillingSessions_) {
            RefillSession(session);
        }
        RefillingSessions_.clear();

        // Check for the end-of-rowset.
        if (SessionHeapBegin_ == SessionHeapEnd_) {
            return false;
        }

        // Must stop once an exhausted session appears.
        while (ExhaustedSessions_.empty()) {
            const TUnversionedValue* currentKeyBegin = nullptr;
            const TUnversionedValue* currentKeyEnd = nullptr;

            // Fetch rows from all sessions with a matching key and merge them.
            // Advance current rows in sessions.
            // Check for exhausted sessions.
            while (SessionHeapBegin_ != SessionHeapEnd_) {
                auto* session = *SessionHeapBegin_;
                auto partialRow = *session->CurrentRow;

                if (currentKeyBegin) {
                    if (TabletSnapshot_->RowKeyComparer(
                            partialRow.BeginKeys(),
                            partialRow.EndKeys(),
                            currentKeyBegin,
                            currentKeyEnd) != 0)
                        break;
                } else {
                    currentKeyBegin = partialRow.BeginKeys();
                    currentKeyEnd = partialRow.EndKeys();
                }

                rowMerger->AddPartialRow(partialRow);

                if (++session->CurrentRow == session->Rows.end()) {
                    ExhaustedSessions_.push_back(session);
                    ExtractHeap(SessionHeapBegin_, SessionHeapEnd_,  GetSessionComparer());
                    --SessionHeapEnd_;
                } else {
                    AdjustHeapFront(SessionHeapBegin_, SessionHeapEnd_,  GetSessionComparer());
                }
            }

            // Save merged row.
            auto mergedRow = rowMerger->BuildMergedRow();
            if (mergedRow) {
                rows->push_back(mergedRow);
            }
        }

        PerformanceCounters_->MergedRowReadCount += rows->size();

        return true;
    }

    void DoOpen(
        const TColumnFilter& columnFilter,
        const std::vector<IStorePtr>& stores)
    {
        // Create readers.
        auto addReader = [&] (IVersionedReaderPtr reader) {
            if (reader) {
                Sessions_.push_back(TSession());
                auto& session = Sessions_.back();
                session.Reader = std::move(reader);
                session.Rows.reserve(MaxRowsPerRead);
            }
        };

        for (const auto& store : stores) {
            addReader(store->CreateReader(
                LowerBound_,
                UpperBound_,
                Timestamp_,
                columnFilter));

            addReader(store->CreateReader(
                Keys_,
                Timestamp_,
                columnFilter));
        }

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

        // Construct an empty heap.
        SessionHeap_.reserve(Sessions_.size());
        SessionHeapBegin_ = SessionHeapEnd_ = SessionHeap_.begin();

        // Mark all sessions as exhausted.
        for (auto& session : Sessions_) {
            ExhaustedSessions_.push_back(&session);
        }

        Opened_ = true;
    }


    std::function<bool(const TSession*, const TSession*)> GetSessionComparer()
    {
        return [&] (const TSession* lhsSession, const TSession* rhsSession) {
            auto lhsRow = *lhsSession->CurrentRow;
            auto rhsRow = *rhsSession->CurrentRow;
            return TabletSnapshot_->RowKeyComparer(
                lhsRow.BeginKeys(),
                lhsRow.EndKeys(),
                rhsRow.BeginKeys(),
                rhsRow.EndKeys()) < 0;
        };
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

        #ifndef NDEBUG
        for (int index = 0; index < rowCount - 1; ++index) {
            auto lhs = rows[index];
            auto rhs = rows[index + 1];
            YASSERT(TabletSnapshot_->RowKeyComparer(
                lhs.BeginKeys(), lhs.EndKeys(),
                rhs.BeginKeys(), rhs.EndKeys()) < 0);
        }
        #endif

        session->CurrentRow = rows.begin();
        *SessionHeapEnd_++ = session;
        AdjustHeapBack(SessionHeapBegin_, SessionHeapEnd_, GetSessionComparer());
        return true;
    }

    void RefillExhaustedSessions()
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
            ReadyEvent_ = VoidFuture;
            return;
        }

        Refilling_ = true;
        ReadyEvent_ = Combine(asyncResults).Apply(BIND([=, this_ = MakeStrong(this)] (const TError& error) {
            Refilling_ = false;
            error.ThrowOnError();
        }));
    }

};

////////////////////////////////////////////////////////////////////////////////

class TSchemafulTabletReader
    : public TTabletReaderBase
    , public ISchemafulReader
{
public:
    TSchemafulTabletReader(
        IInvokerPtr poolInvoker,
        TTabletSnapshotPtr tabletSnapshot,
        TOwningKey lowerBound,
        TOwningKey upperBound,
        TTimestamp timestamp)
        : TTabletReaderBase(
            std::move(poolInvoker),
            std::move(tabletSnapshot),
            std::move(lowerBound),
            std::move(upperBound),
            timestamp)
    { }

    TSchemafulTabletReader(
        IInvokerPtr poolInvoker,
        TTabletSnapshotPtr tabletSnapshot,
        const std::vector<TKey>& keys,
        TTimestamp timestamp)
        : TTabletReaderBase(
            std::move(poolInvoker),
            std::move(tabletSnapshot),
            keys,
            timestamp)
    { }

    virtual TFuture<void> Open(const TTableSchema& schema) override
    {
        return BIND(&TSchemafulTabletReader::DoOpen, MakeStrong(this))
            .AsyncVia(PoolInvoker_)
            .Run(schema);
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        return TTabletReaderBase::DoRead(rows, RowMerger_.get());
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return ReadyEvent_;
    }

private:
    std::unique_ptr<TUnversionedRowMerger> RowMerger_;


    void DoOpen(const TTableSchema& schema)
    {
        const auto& tabletSchema = TabletSnapshot_->Schema;

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

        // Initialize merger.
        RowMerger_ = std::make_unique<TUnversionedRowMerger>(
            &Pool_,
            TabletSnapshot_->Schema.Columns().size(),
            TabletSnapshot_->KeyColumns.size(),
            columnFilter);

        // Select stores.
        std::vector<IStorePtr> stores;
        auto takePartition = [&] (const TPartitionSnapshotPtr& partitionSnapshot) {
            stores.insert(
                stores.end(),
                partitionSnapshot->Stores.begin(),
                partitionSnapshot->Stores.end());
        };

        takePartition(TabletSnapshot_->Eden);

        auto range = TabletSnapshot_->GetIntersectingPartitions(LowerBound_, UpperBound_);
        for (auto it = range.first; it != range.second; ++it) {
            takePartition(*it);
        }

        LOG_DEBUG("Creating schemaful tablet reader (TabletId: %v, CellId: %v, LowerBound: {%v}, UpperBound: {%v}, Timestamp: %v, StoreIds: [%v])",
            TabletSnapshot_->TabletId,
            TabletSnapshot_->Slot->GetCellId(),
            LowerBound_,
            UpperBound_,
            Timestamp_,
            JoinToString(stores, TStoreIdFormatter()));

        if (stores.size() > TabletSnapshot_->Config->MaxReadFanIn) {
            THROW_ERROR_EXCEPTION("Read fan-in limit exceeded; please wait until your data is merged")
                << TErrorAttribute("tablet_id", TabletSnapshot_->TabletId)
                << TErrorAttribute("fan_in", stores.size())
                << TErrorAttribute("fan_in_limit", TabletSnapshot_->Config->MaxReadFanIn);
        }

        TTabletReaderBase::DoOpen(columnFilter, stores);
    }
};

ISchemafulReaderPtr CreateSchemafulTabletReader(
    IInvokerPtr poolInvoker,
    TTabletSnapshotPtr tabletSnapshot,
    TOwningKey lowerBound,
    TOwningKey upperBound,
    TTimestamp timestamp)
{
    return New<TSchemafulTabletReader>(
        std::move(poolInvoker),
        std::move(tabletSnapshot),
        std::move(lowerBound),
        std::move(upperBound),
        timestamp);
}

ISchemafulReaderPtr CreateSchemafulTabletReader(
    IInvokerPtr poolInvoker,
    TTabletSnapshotPtr tabletSnapshot,
    const std::vector<TKey>& keys,
    TTimestamp timestamp)
{
    return New<TSchemafulTabletReader>(
        std::move(poolInvoker),
        std::move(tabletSnapshot),
        keys,
        timestamp);
}

////////////////////////////////////////////////////////////////////////////////

class TVersionedTabletReader
    : public TTabletReaderBase
    , public IVersionedReader
{
public:
    TVersionedTabletReader(
        IInvokerPtr poolInvoker,
        TTabletSnapshotPtr tabletSnapshot,
        std::vector<IStorePtr> stores,
        TOwningKey lowerBound,
        TOwningKey upperBound,
        TTimestamp currentTimestamp,
        TTimestamp majorTimestamp)
        : TTabletReaderBase(
            std::move(poolInvoker),
            std::move(tabletSnapshot),
            std::move(lowerBound),
            std::move(upperBound),
            AsyncAllCommittedTimestamp)
        , Stores_(std::move(stores))
        , CurrentTimestamp_(currentTimestamp)
        , MajorTimestamp_(majorTimestamp)
        , RowMerger_(
            &Pool_,
            TabletSnapshot_->KeyColumns.size(),
            TabletSnapshot_->Config,
            CurrentTimestamp_,
            MajorTimestamp_)
    { }

    virtual TFuture<void> Open() override
    {
        return BIND(&TVersionedTabletReader::DoOpen, MakeStrong(this))
            .AsyncVia(PoolInvoker_)
            .Run();
    }

    virtual bool Read(std::vector<TVersionedRow>* rows) override
    {
        bool result = TTabletReaderBase::DoRead(rows, &RowMerger_);
        #ifndef NDEBUG
        for (int index = 0; index < static_cast<int>(rows->size()) - 1; ++index) {
            auto lhs = (*rows)[index];
            auto rhs = (*rows)[index + 1];
            YASSERT(TabletSnapshot_->RowKeyComparer(
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
    const std::vector<IStorePtr> Stores_;
    const TTimestamp CurrentTimestamp_;
    const TTimestamp MajorTimestamp_;

    TVersionedRowMerger RowMerger_;


    void DoOpen()
    {
        LOG_DEBUG("Creating versioned tablet reader (TabletId: %v, CellId: %v, LowerBound: {%v}, UpperBound: {%v}, Timestamp: %v, StoreIds: [%v])",
            TabletSnapshot_->TabletId,
            TabletSnapshot_->Slot->GetCellId(),
            LowerBound_,
            UpperBound_,
            Timestamp_,
            JoinToString(Stores_, TStoreIdFormatter()));

        TTabletReaderBase::DoOpen(TColumnFilter(), Stores_);
    }

};

IVersionedReaderPtr CreateVersionedTabletReader(
    IInvokerPtr poolInvoker,
    TTabletSnapshotPtr tabletSnapshot,
    std::vector<IStorePtr> stores,
    TOwningKey lowerBound,
    TOwningKey upperBound,
    TTimestamp currentTimestamp,
    TTimestamp majorTimestamp)
{
    return New<TVersionedTabletReader>(
        std::move(poolInvoker),
        std::move(tabletSnapshot),
        std::move(stores),
        std::move(lowerBound),
        std::move(upperBound),
        currentTimestamp,
        majorTimestamp);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT

