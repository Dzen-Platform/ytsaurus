#include "persistent_queue.h"
#include "client.h"
#include "transaction.h"
#include "config.h"
#include "private.h"

#include <yt/ytlib/table_client/unversioned_row.h>
#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/row_buffer.h>

#include <yt/ytlib/query_client/query_statistics.h>

#include <yt/core/ytree/helpers.h>

#include <yt/core/concurrency/thread_affinity.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/delayed_executor.h>

namespace NYT {
namespace NApi {

using namespace NYPath;
using namespace NYTree;
using namespace NConcurrency;
using namespace NTableClient;
using namespace NObjectClient;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

struct TPersistentQueuePollerBufferTag
{ };

namespace {

DEFINE_ENUM(ERowState,
    ((Consumed)              (0))
    ((ConsumedAndTrimmed)    (1))
);

struct TStateTableRow
{
    int TabletIndex;
    i64 RowIndex;
    ERowState State;
};

struct TStateTable
{
    static const Stroka TabletIndexColumnName;
    static const Stroka RowIndexColumnName;
    static const Stroka StateColumnName;
};

const Stroka TStateTable::TabletIndexColumnName("tablet_index");
const Stroka TStateTable::RowIndexColumnName("row_index");
const Stroka TStateTable::StateColumnName("state");

std::vector<int> PrepareTabletIndexes(std::vector<int> tabletIndexes)
{
    std::sort(tabletIndexes.begin(), tabletIndexes.end());
    tabletIndexes.erase(std::unique(tabletIndexes.begin(), tabletIndexes.end()), tabletIndexes.end());
    return tabletIndexes;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TPersistentQueuePoller::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TPersistentQueuePollerConfigPtr config,
        IClientPtr client,
        const TYPath& dataTablePath,
        const TYPath& stateTablePath,
        const std::vector<int>& tabletIndexes)
        : Config_(std::move(config))
        , Client_(std::move(client))
        , DataTablePath_(dataTablePath)
        , StateTablePath_(stateTablePath)
        , TabletIndexes_(PrepareTabletIndexes(tabletIndexes))
        , PollerId_(TGuid::Create())
        , Logger(NLogging::TLogger(ApiLogger)
            .AddTag("PollerId: %v", PollerId_))
        , Invoker_(Client_->GetConnection()->GetHeavyInvoker())
    {
        YCHECK(Config_);

        RecreateState(false);

        LOG_INFO("Persistent queue poller initialized (DataTablePath: %v, StateTablePath: %v, TabletIndexes: %v)",
            DataTablePath_,
            StateTablePath_,
            TabletIndexes_);

        for (int tabletIndex : TabletIndexes_) {
            auto executor = New<TPeriodicExecutor>(
                Invoker_,
                BIND(&TImpl::FetchTablet, MakeWeak(this), tabletIndex),
                Config_->DataPollPeriod);
            PollExecutors_.push_back(executor);
            executor->Start();
        }

        {
            TrimExecutor_ = New<TPeriodicExecutor>(
                Invoker_,
                BIND(&TImpl::TrimState, MakeWeak(this)),
                Config_->DataPollPeriod);
            TrimExecutor_->Start();
        }
    }

    TFuture<IPersistentQueueRowsetPtr> Poll()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto promise = NewPromise<IPersistentQueueRowsetPtr>();
        auto state = GetState();
        TGuard<TSpinLock> guard(state->SpinLock);
        state->Promises.push_back(promise);
        TryFulfillPromises(state, &guard);
        return promise;
    }

private:
    struct TBatch
    {
        IRowsetPtr Rowset;
        int RowCount;
        i64 DataWeight;
        int TabletIndex;
        i64 RowsetStartRowIndex;
        i64 BeginRowIndex;
        i64 EndRowIndex;
    };

    struct TTablet
    {
        yhash_set<i64> ConsumedRowIndexes;
        i64 MaxConsumedRowIndex = std::numeric_limits<i64>::min();
        i64 FetchRowIndex = std::numeric_limits<i64>::max();
    };

    struct TState
        : public TIntrinsicRefCounted
    {
        TSpinLock SpinLock;
        std::deque<TPromise<IPersistentQueueRowsetPtr>> Promises;
        std::deque<TBatch> Batches;
        int BatchesRowCount = 0;
        i64 BatchesDataWeight = 0;
        yhash_map<int, TTablet> TabletMap;
        std::atomic<bool> Failed = {false};
    };

    using TStatePtr = TIntrusivePtr<TState>;

    class TPolledRowset
        : public IPersistentQueueRowset
    {
    public:
        TPolledRowset(
            TIntrusivePtr<TImpl> owner,
            TStatePtr state,
            TBatch batch)
            : Owner_(std::move(owner))
            , State_(std::move(state))
            , Batch_(std::move(batch))
            , Rows_(
                Batch_.Rowset->GetRows().begin() + Batch_.BeginRowIndex - Batch_.RowsetStartRowIndex,
                Batch_.Rowset->GetRows().begin() + Batch_.EndRowIndex - Batch_.RowsetStartRowIndex)
        { }

        ~TPolledRowset()
        {
            if (!Committed_) {
                Owner_->ReclaimBatch(State_, std::move(Batch_));
            }
        }

        virtual const TTableSchema& GetSchema() const override
        {
            return Batch_.Rowset->GetSchema();
        }

        virtual const NTableClient::TNameTablePtr& GetNameTable() const override
        {
            return Batch_.Rowset->GetNameTable();
        }

        virtual const std::vector<TUnversionedRow>& GetRows() const override
        {
            return Rows_;
        }

        virtual TFuture<void> Confirm(const ITransactionPtr& transaction) override
        {
            transaction->SubscribeCommitted(BIND(&TPolledRowset::OnCommitted, MakeStrong(this)));
            return Owner_->ConfirmBatch(State_, Batch_, transaction);
        }

    private:
        const TIntrusivePtr<TImpl> Owner_;
        const TStatePtr State_;
        const TBatch Batch_;
        const std::vector<TUnversionedRow> Rows_;

        bool Committed_ = false;


        void OnCommitted()
        {
            Owner_->OnBatchCommitted(Batch_);
            Committed_ = true;
        }
    };


    const TPersistentQueuePollerConfigPtr Config_;
    const IClientPtr Client_;
    const NYPath::TYPath DataTablePath_;
    const NYPath::TYPath StateTablePath_;
    const std::vector<int> TabletIndexes_;

    const TGuid PollerId_;
    const NLogging::TLogger Logger;
    const IInvokerPtr Invoker_;

    TSpinLock SpinLock_;
    TStatePtr State_;

    std::vector<TPeriodicExecutorPtr> PollExecutors_;
    TPeriodicExecutorPtr TrimExecutor_;


    TStatePtr GetState()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        return State_;
    }


    std::vector<TStateTableRow> ReadStateTable()
    {
        // TODO(babenko): escaping
        auto query = Format(
            "[%v], [%v], [%v] from [%v] where [%v] in (%v)",
            TStateTable::TabletIndexColumnName,
            TStateTable::RowIndexColumnName,
            TStateTable::StateColumnName,
            StateTablePath_,
            TStateTable::TabletIndexColumnName,
            JoinToString(TabletIndexes_));
        auto result = WaitFor(Client_->SelectRows(query))
            .ValueOrThrow();
        const auto& rowset = result.first;

        auto tabletIndexColumnId = rowset->GetNameTable()->GetId(TStateTable::TabletIndexColumnName);
        auto rowIndexColumnId = rowset->GetNameTable()->GetId(TStateTable::RowIndexColumnName);
        auto stateColumnId = rowset->GetNameTable()->GetId(TStateTable::StateColumnName);

        std::vector<TStateTableRow> rows;

        for (auto row : rowset->GetRows()) {
            TStateTableRow stateRow;

            YASSERT(row[tabletIndexColumnId].Type == EValueType::Int64);
            stateRow.TabletIndex = static_cast<int>(row[tabletIndexColumnId].Data.Int64);

            YASSERT(row[rowIndexColumnId].Type == EValueType::Int64);
            stateRow.RowIndex = row[rowIndexColumnId].Data.Int64;

            YASSERT(row[rowIndexColumnId].Type == EValueType::Int64);
            stateRow.State = ERowState(row[stateColumnId].Data.Int64);

            rows.push_back(stateRow);
        }

        return rows;
    }


    void DoLoadState(const TStatePtr& state)
    {
        LOG_INFO("Loading queue poller state for initialization");

        auto stateRows = ReadStateTable();

        TGuard<TSpinLock> guard(state->SpinLock);

        for (auto& pair : state->TabletMap) {
            auto& tablet = pair.second;
            tablet.FetchRowIndex = 0;
        }

        for (const auto& row : stateRows) {
            auto tabletIt = state->TabletMap.find(row.TabletIndex);
            YCHECK(tabletIt != state->TabletMap.end());
            auto& tablet = tabletIt->second;

            tablet.ConsumedRowIndexes.insert(row.RowIndex);
            tablet.MaxConsumedRowIndex = std::max(tablet.MaxConsumedRowIndex, row.RowIndex);

            if (row.State == ERowState::ConsumedAndTrimmed) {
                tablet.FetchRowIndex = row.RowIndex;
            }
        }

        for (auto& pair : state->TabletMap) {
            auto& tablet = pair.second;
            while (tablet.ConsumedRowIndexes.find(tablet.FetchRowIndex) != tablet.ConsumedRowIndexes.end()) {
                YCHECK(tablet.ConsumedRowIndexes.erase(tablet.FetchRowIndex) == 1);
                ++tablet.FetchRowIndex;
            }
        }

        for (const auto& pair : state->TabletMap) {
            int tabletIndex = pair.first;
            const auto& tablet = pair.second;
            LOG_DEBUG("Tablet state collected (TabletIndex: %v, ConsumedRowIndexes: %v, FetchRowIndex: %v)",
                tabletIndex,
                tablet.ConsumedRowIndexes,
                tablet.FetchRowIndex);
        }

        LOG_INFO("Queue poller state loaded");
    }

    void LoadState(const TStatePtr& state)
    {
        try {
            DoLoadState(state);
        } catch (const std::exception& ex) {
            OnStateFailed(state);
            LOG_ERROR(ex, "Error loading queue poller state");
        }
    }

    void RecreateState(bool backoff)
    {
        auto state = New<TState>();
        for (int tabletIndex : TabletIndexes_) {
            YCHECK(state->TabletMap.insert(std::make_pair(tabletIndex, TTablet())).second);
        }

        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (State_) {
                state->Promises = std::move(State_->Promises);
            }
            State_ = state;
        }

        TDelayedExecutor::Submit(
            BIND(&TImpl::LoadState, MakeStrong(this), state),
            backoff ? Config_->BackoffTime : TDuration::Zero());
    }


    void DoFetchTablet(int tabletIndex)
    {
        auto state = GetState();
        if (state->Failed) {
            return;
        }

        auto tabletIt = state->TabletMap.find(tabletIndex);
        YCHECK(tabletIt != state->TabletMap.end());
        auto& tablet = tabletIt->second;

        int rowLimit = Config_->MaxRowsPerFetch;
        {
            TGuard<TSpinLock> guard(state->SpinLock);
            if (state->BatchesDataWeight > Config_->MaxPrefetchDataWeight) {
                return;
            }
            rowLimit = std::min(rowLimit, Config_->MaxPrefetchRowCount - state->BatchesRowCount);
        }

        if (rowLimit <= 0) {
            return;
        }

        LOG_DEBUG("Started fetching data (TabletIndex: %v, FetchRowIndex: %v, RowLimit: %v)",
            tabletIndex,
            tablet.FetchRowIndex,
            rowLimit);

        // TODO(babenko): escaping
        auto query = Format(
            "* from [%v] where [%v] = %v and [%v] >= %v order by [%v] limit %v",
            DataTablePath_,
            TabletIndexColumnName,
            tabletIndex,
            RowIndexColumnName,
            tablet.FetchRowIndex,
            RowIndexColumnName,
            rowLimit);
        auto result = WaitFor(Client_->SelectRows(query))
            .ValueOrThrow();
        const auto& rowset = result.first;
        const auto& rows = rowset->GetRows();

        LOG_DEBUG("Finished fetching data (TabletIndex: %v, RowCount: %v)",
            tabletIndex,
            rows.size());

        if (rows.empty()) {
            return;
        }

        auto rowIndexColumnId = rowset->GetNameTable()->GetId(RowIndexColumnName);

        std::vector<TBatch> batches;
        i64 currentRowIndex = tablet.FetchRowIndex;
        i64 batchBeginRowIndex = -1;


        auto beginBatch = [&] () {
            YCHECK(batchBeginRowIndex < 0);
            batchBeginRowIndex = currentRowIndex;
        };

        auto endBatch = [&] () {
            if (batchBeginRowIndex < 0) {
                return;
            }

            i64 batchEndRowIndex = currentRowIndex;
            YCHECK(batchBeginRowIndex < batchEndRowIndex);

            TBatch batch;
            batch.TabletIndex = tabletIndex;
            batch.Rowset = rowset;
            batch.RowCount = static_cast<int>(batchEndRowIndex - batchBeginRowIndex);
            batch.RowsetStartRowIndex = tablet.FetchRowIndex;
            batch.BeginRowIndex = batchBeginRowIndex;
            batch.EndRowIndex = batchEndRowIndex;
            batch.DataWeight = 0;
            for (i64 index = batchBeginRowIndex; index < batchEndRowIndex; ++index) {
                batch.DataWeight += GetDataWeight(rows[index - tablet.FetchRowIndex]);
            }
            batches.emplace_back(std::move(batch));

            LOG_DEBUG("Rows fetched (TabletIndex: %v, RowIndexes: %v-%v, DataWeight: %v)",
                tabletIndex,
                batchBeginRowIndex,
                batchEndRowIndex - 1,
                batch.DataWeight);

            batchBeginRowIndex = -1;
        };

        for (auto row : rows) {
            YASSERT(row[rowIndexColumnId].Type == EValueType::Int64);
            auto queryRowIndex = row[rowIndexColumnId].Data.Int64;
            if (queryRowIndex != currentRowIndex) {
                OnStateFailed(state);
                THROW_ERROR_EXCEPTION("Fetched row index mismatch: expected %v, got %v",
                    currentRowIndex,
                    queryRowIndex);
            }

            if (tablet.ConsumedRowIndexes.find(currentRowIndex) == tablet.ConsumedRowIndexes.end()) {
                if (batchBeginRowIndex >= 0 && currentRowIndex - batchBeginRowIndex >= Config_->MaxRowsPerPoll) {
                    endBatch();
                }
                if (batchBeginRowIndex < 0) {
                    beginBatch();
                }
            } else {
                endBatch();
            }

            ++currentRowIndex;
        }

        endBatch();

        {
            TGuard<TSpinLock> guard(state->SpinLock);

            for (const auto& batch : batches) {
                state->Batches.push_back(batch);
                state->BatchesRowCount += batch.RowCount;
                state->BatchesDataWeight += batch.DataWeight;
            }

            tablet.FetchRowIndex += rows.size();
            if (tablet.FetchRowIndex > tablet.MaxConsumedRowIndex) {
                // No need to keep them anymore.
                tablet.ConsumedRowIndexes.clear();
            }

            TryFulfillPromises(state, &guard);
        }
    }

    void FetchTablet(int tabletIndex)
    {
        try {
            DoFetchTablet(tabletIndex);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error fetching queue data (TabletIndex: %v)",
                tabletIndex);
        }
    }


    void TryFulfillPromises(const TStatePtr& state, TGuard<TSpinLock>* guard)
    {
        if (state->Failed) {
            return;
        }

        std::vector<std::tuple<TBatch, TPromise<IPersistentQueueRowsetPtr>>> toFulfill;
        while (!state->Batches.empty() && !state->Promises.empty()) {
            const auto& batch = state->Batches.front();
            const auto& promise = state->Promises.front();
            toFulfill.push_back(std::make_tuple(batch, promise));
            state->Batches.pop_front();
            state->Promises.pop_front();
            state->BatchesRowCount -= batch.RowCount;
            state->BatchesDataWeight -= batch.DataWeight;
        }

        guard->Release();

        for (auto& tuple : toFulfill) {
            const auto& rowset = std::get<0>(tuple);
            auto& promise = std::get<1>(tuple);
            LOG_DEBUG("Rows offered (TabletIndex: %v, RowIndexes: %v-%v)",
                rowset.TabletIndex,
                rowset.BeginRowIndex,
                rowset.EndRowIndex - 1);
            promise.Set(New<TPolledRowset>(this, state, rowset));
        }
    }

    void ReclaimBatch(
        const TStatePtr& state,
        TBatch batch)
    {
        TGuard<TSpinLock> guard(state->SpinLock);

        if (State_ != state) {
            return;
        }

        State_->BatchesRowCount += batch.RowCount;
        State_->BatchesDataWeight += batch.DataWeight;
        State_->Batches.emplace_back(std::move(batch));

        LOG_DEBUG("Rows reclaimed (TabletIndex: %v RowIndexes: %v-%v)",
            batch.TabletIndex,
            batch.BeginRowIndex,
            batch.EndRowIndex - 1);

        TryFulfillPromises(state, &guard);
    }


    TFuture<void> ConfirmBatch(
        const TStatePtr& state,
        const TBatch& batch,
        const ITransactionPtr& transaction)
    {
        return BIND(&TImpl::DoConfirmBatch, MakeStrong(this))
            .AsyncVia(Invoker_)
            .Run(state, batch, transaction);
    }

    void DoConfirmBatch(
        const TStatePtr& state,
        const TBatch& batch,
        const ITransactionPtr& transaction)
    {
        try {
            // Check that none of the dequeued rows were consumed in another transaction.
            {
                // TODO(babenko): escaping
                auto query = Format("[%v] from [%v] where [%v] = %v and [%v] between %v and %v",
                    TStateTable::RowIndexColumnName,
                    StateTablePath_,
                    TStateTable::TabletIndexColumnName,
                    batch.TabletIndex,
                    TStateTable::RowIndexColumnName,
                    batch.BeginRowIndex,
                    batch.EndRowIndex - 1);
                auto result = WaitFor(transaction->SelectRows(query))
                    .ValueOrThrow();
                const auto& rowset = result.first;
                if (!rowset->GetRows().empty()) {
                    std::vector<i64> rowIndexes;
                    auto rowIndexColumnId = rowset->GetNameTable()->GetId(TStateTable::RowIndexColumnName);
                    for (auto row : rowset->GetRows()) {
                        const auto& value = row[rowIndexColumnId];
                        YASSERT(value.Type == EValueType::Int64);
                        rowIndexes.push_back(value.Data.Int64);
                    }
                    OnStateFailed(state);
                    THROW_ERROR_EXCEPTION("Some of the offered rows were already consumed")
                        << TErrorAttribute("consumed_row_indexes", rowIndexes);
                }
            }

            // Check that none of the dequeued rows were trimmed.
            {
                // TODO(babenko): escaping
                auto query = Format("[%v] from [%v] where [%v] = %v and [%v] = %v order by [%v] limit 1",
                    TStateTable::RowIndexColumnName,
                    StateTablePath_,
                    TStateTable::TabletIndexColumnName,
                    batch.TabletIndex,
                    TStateTable::StateColumnName,
                    static_cast<int>(ERowState::ConsumedAndTrimmed),
                    TStateTable::RowIndexColumnName);
                auto result = WaitFor(transaction->SelectRows(query))
                    .ValueOrThrow();
                const auto& rowset = result.first;
                if (!rowset->GetRows().empty()) {
                    YCHECK(rowset->GetRows().size() == 1);
                    auto row = rowset->GetRows()[0];

                    auto rowIndexColumnId = rowset->GetNameTable()->GetId(TStateTable::RowIndexColumnName);

                    YASSERT(row[rowIndexColumnId].Type == EValueType::Int64);
                    auto rowIndex = row[rowIndexColumnId].Data.Int64;

                    if (rowIndex >= batch.BeginRowIndex) {
                        OnStateFailed(state);
                        THROW_ERROR_EXCEPTION("Some of the offered rows were already trimmed")
                            << TErrorAttribute("trimmed_row_index", rowIndex);
                    }
                }
            }

            // Mark rows as consumed in state table.
            {
                auto nameTable = New<TNameTable>();
                auto tabletIndexColumnId = nameTable->RegisterName(TStateTable::TabletIndexColumnName);
                auto rowIndexColumnId = nameTable->RegisterName(TStateTable::RowIndexColumnName);
                auto stateColumnId = nameTable->RegisterName(TStateTable::StateColumnName);

                auto rowBuffer = New<TRowBuffer>(TPersistentQueuePollerBufferTag());
                std::vector<TUnversionedRow> rows;
                for (i64 rowIndex = batch.BeginRowIndex; rowIndex < batch.EndRowIndex; ++rowIndex) {
                    auto row = rowBuffer->Allocate(3);
                    row[0] = MakeUnversionedInt64Value(batch.TabletIndex, tabletIndexColumnId);
                    row[1] = MakeUnversionedInt64Value(rowIndex, rowIndexColumnId);
                    row[2] = MakeUnversionedInt64Value(static_cast<int>(ERowState::Consumed), stateColumnId);
                    rows.push_back(row);
                }
                transaction->WriteRows(
                    StateTablePath_,
                    std::move(nameTable),
                    MakeSharedRange(std::move(rows), std::move(rowBuffer)));
            }

            LOG_DEBUG("Rows processing confirmed (TabletIndex: %v RowIndexes: %v-%v, TransactionId: %v)",
                batch.TabletIndex,
                batch.BeginRowIndex,
                batch.EndRowIndex - 1,
                transaction->GetId());
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error confirming persistent queue rows",
                batch.TabletIndex,
                batch.BeginRowIndex,
                batch.EndRowIndex - 1)
                << TErrorAttribute("poller_id", PollerId_)
                << TErrorAttribute("transaction_id", transaction->GetId())
                << TErrorAttribute("tablet_index", batch.TabletIndex)
                << TErrorAttribute("begin_row_index", batch.BeginRowIndex)
                << TErrorAttribute("end_row_index", batch.EndRowIndex)
                << TErrorAttribute("data_table_path", DataTablePath_)
                << TErrorAttribute("state_table_path", StateTablePath_)
                << ex;
        }
    }


    void OnBatchCommitted(const TBatch& batch)
    {
        LOG_DEBUG("Rows processing committed (TabletIndex: %v RowIndexes: %v-%v)",
            batch.TabletIndex,
            batch.BeginRowIndex,
            batch.EndRowIndex - 1);
    }


    void DoTrimState()
    {
        // NB: Not actually needed, just for a backoff.
        auto state = GetState();
        if (state->Failed) {
            return;
        }

        LOG_DEBUG("Starting state trim transaction");

        auto transaction = WaitFor(Client_->StartTransaction(ETransactionType::Tablet))
            .ValueOrThrow();

        LOG_DEBUG("State trim transaction started (TransactionId: %v)",
            transaction->GetId());

        LOG_DEBUG("Loading queue poller state for trim");

        auto stateRows = ReadStateTable();

        LOG_DEBUG("Queue poller state loaded");

        struct TTabletStatistics
        {
            i64 LastTrimmedRowIndex = -1;
            yhash_set<i64> ConsumedRowIndexes;
        };

        yhash_map<int, TTabletStatistics> tabletStatisticsMap;

        for (const auto& row : stateRows) {
            auto& tablet = tabletStatisticsMap[row.TabletIndex];
            if (row.State == ERowState::ConsumedAndTrimmed) {
                tablet.LastTrimmedRowIndex = std::max(tablet.LastTrimmedRowIndex, row.RowIndex);
            }
            YCHECK(tablet.ConsumedRowIndexes.insert(row.RowIndex).second);
        }

        {
            auto nameTable = New<TNameTable>();
            auto tabletIndexColumnId = nameTable->RegisterName(TStateTable::TabletIndexColumnName);
            auto rowIndexColumnId = nameTable->RegisterName(TStateTable::RowIndexColumnName);
            auto stateColumnId = nameTable->RegisterName(TStateTable::StateColumnName);

            for (const auto& pair : tabletStatisticsMap) {
                int tabletIndex = pair.first;
                const auto& statistics = pair.second;

                i64 trimRowIndex = statistics.LastTrimmedRowIndex;
                while (statistics.ConsumedRowIndexes.find(trimRowIndex + 1) != statistics.ConsumedRowIndexes.end()) {
                    ++trimRowIndex;
                }

                if (trimRowIndex > statistics.LastTrimmedRowIndex) {
                    auto rowBuffer = New<TRowBuffer>(TPersistentQueuePollerBufferTag());

                    std::vector<TUnversionedRow> deleteKeys;
                    for (i64 rowIndex = statistics.LastTrimmedRowIndex; rowIndex < trimRowIndex; ++rowIndex) {
                        auto key = rowBuffer->Allocate(2);
                        key[0] = MakeUnversionedInt64Value(tabletIndex, tabletIndexColumnId);
                        key[1] = MakeUnversionedInt64Value(rowIndex, rowIndexColumnId);
                        deleteKeys.push_back(key);
                    }
                    transaction->DeleteRows(
                        StateTablePath_,
                        nameTable,
                        MakeSharedRange(std::move(deleteKeys), rowBuffer));

                    std::vector<TUnversionedRow> writeRows;
                    {
                        auto row = rowBuffer->Allocate(3);
                        row[0] = MakeUnversionedInt64Value(tabletIndex, tabletIndexColumnId);
                        row[1] = MakeUnversionedInt64Value(trimRowIndex, rowIndexColumnId);
                        row[2] = MakeUnversionedInt64Value(static_cast<int>(ERowState::ConsumedAndTrimmed), stateColumnId);
                        writeRows.push_back(row);
                    }
                    transaction->WriteRows(
                        StateTablePath_,
                        nameTable,
                        MakeSharedRange(std::move(writeRows), rowBuffer));

                    LOG_DEBUG("Tablet state trim scheduled (TabletIndex: %v, TrimRowIndex: %v)",
                        tabletIndex,
                        trimRowIndex);
                }
            }
        }

        LOG_DEBUG("Committing state trim transaction");

        WaitFor(transaction->Commit())
            .ThrowOnError();

        LOG_DEBUG("State trim transaction committed");
    }

    void TrimState()
    {
        try {
            DoTrimState();
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error trimming queue poller state");
        }
    }


    void OnStateFailed(const TStatePtr& state)
    {
        bool expected = false;
        if (!state->Failed.compare_exchange_strong(expected, true)) {
            RecreateState(true);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TPersistentQueuePoller::TPersistentQueuePoller(
    TPersistentQueuePollerConfigPtr config,
    IClientPtr client,
    const TYPath& dataTablePath,
    const TYPath& stateTablePath,
    const std::vector<int>& tabletIndexes)
    : Impl_(New<TImpl>(
        std::move(config),
        std::move(client),
        dataTablePath,
        stateTablePath,
        tabletIndexes))
{ }

TFuture<IPersistentQueueRowsetPtr> TPersistentQueuePoller::Poll()
{
    return Impl_->Poll();
}

////////////////////////////////////////////////////////////////////////////////

TFuture<void> CreatePersistentQueueStateTable(
    IClientPtr client,
    const TYPath& path)
{
    TTableSchema schema({
        TColumnSchema(TStateTable::TabletIndexColumnName, EValueType::Int64)
            .SetSortOrder(ESortOrder::Ascending),
        TColumnSchema(TStateTable::RowIndexColumnName, EValueType::Int64)
            .SetSortOrder(ESortOrder::Ascending),
        TColumnSchema(TStateTable::StateColumnName, EValueType::Int64)
    });

    auto attributes = CreateEphemeralAttributes();
    attributes->Set("dynamic", true);
    attributes->Set("schema", schema);

    TCreateNodeOptions options;
    options.Attributes = std::move(attributes);
    return client->CreateNode(path, EObjectType::Table, options).As<void>();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

