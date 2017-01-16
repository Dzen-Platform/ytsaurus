#include "schemaless_sorted_merging_reader.h"
#include "private.h"
#include "schemaless_chunk_reader.h"
#include "name_table.h"

#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/data_statistics.h>

#include <yt/core/misc/heap.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

// Reasonable default for max data size per one read call.
static const i64 MaxDataSizePerRead = 16 * 1024 * 1024;
static const i64 RowBufferSize = 10000;

static const auto& Logger = TableClientLogger;

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;

using NChunkClient::TDataSliceDescriptor;
using NYT::TRange;

////////////////////////////////////////////////////////////////////////////////

class TSchemalessSortedMergingReaderBase
    : public ISchemalessMultiChunkReader
{
public:
    TSchemalessSortedMergingReaderBase(
        int keyColumnCount);

    virtual TFuture<void> GetReadyEvent() override;

    virtual TDataStatistics GetDataStatistics() const override;

    virtual bool IsFetchingCompleted() const override;

    virtual std::vector<TChunkId> GetFailedChunkIds() const override;

    virtual TNameTablePtr GetNameTable() const override;

    virtual TKeyColumns GetKeyColumns() const override;

    virtual i64 GetTotalRowCount() const override;

    virtual i64 GetSessionRowIndex() const override;

    virtual i64 GetTableRowIndex() const override;

    virtual void Interrupt() override;

protected:
    struct TSession
    {
        TSession(const ISchemalessMultiChunkReaderPtr& reader, int rowCount)
            : Reader(reader)
        {
            Rows.reserve(rowCount);
        }

        ISchemalessMultiChunkReaderPtr Reader;
        std::vector<TUnversionedRow> Rows;
        int CurrentRowIndex = 0;
        int TableIndex = 0;
    };

    const int KeyColumnCount_;

    std::vector<TSession> SessionHolder_;
    std::vector<TSession*> SessionHeap_;

    i64 RowCount_ = 0;
    i64 RowIndex_ = 0;

    TFuture<void> ReadyEvent_;
    TPromise<void> CompletionError_ = NewPromise<void>();
    i64 TableRowIndex_ = 0;

    std::atomic<bool> Interrupting_ = {false};

    std::function<bool(const TSession* lhs, const TSession* rhs)> CompareSessions_;

    void DoOpen();
    TFuture<void> CombineCompletionError(TFuture<void> future);

};

////////////////////////////////////////////////////////////////////////////////

TSchemalessSortedMergingReaderBase::TSchemalessSortedMergingReaderBase(
    int keyColumnCount)
    : KeyColumnCount_(keyColumnCount)
{
    CompareSessions_ = [=] (const TSession* lhs, const TSession* rhs) -> bool {
        int result = CompareRows(
            lhs->Rows[lhs->CurrentRowIndex],
            rhs->Rows[rhs->CurrentRowIndex],
            KeyColumnCount_);
        if (result == 0) {
            result = lhs->TableIndex - rhs->TableIndex;
        }
        return result < 0;
    };
}

void TSchemalessSortedMergingReaderBase::DoOpen()
{
    auto getTableIndex = [] (TUnversionedRow row, TNameTablePtr nameTable) -> int {
        int tableIndexId;
        try {
            tableIndexId = nameTable->GetIdOrRegisterName(TableIndexColumnName);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Failed to add system column to name table for schemaless merging reader") 
                << ex;
        }

        for (const auto& value : row) {
            if (value.Id == tableIndexId) {
                YCHECK(value.Type == EValueType::Int64);
                return value.Data.Int64;
            }
        }

        return 0;
    };

    try {
        for (auto& session : SessionHolder_) {
            while (session.Reader->Read(&session.Rows)) {
                if (!session.Rows.empty()) {
                    session.TableIndex = getTableIndex(session.Rows.front(), session.Reader->GetNameTable());
                    SessionHeap_.push_back(&session);
                    break;
                }

                WaitFor(session.Reader->GetReadyEvent())
                    .ThrowOnError();
            }
        }

        if (!SessionHeap_.empty()) {
            MakeHeap(SessionHeap_.begin(), SessionHeap_.end(), CompareSessions_);
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Failed to open schemaless merging reader") << ex;
    }
}

TFuture<void> TSchemalessSortedMergingReaderBase::GetReadyEvent()
{
    return ReadyEvent_;
}

TDataStatistics TSchemalessSortedMergingReaderBase::GetDataStatistics() const
{
    TDataStatistics dataStatistics;

    for (const auto& session : SessionHolder_) {
        dataStatistics += session.Reader->GetDataStatistics();
    }
    dataStatistics.set_row_count(RowIndex_);

    return dataStatistics;
}

bool TSchemalessSortedMergingReaderBase::IsFetchingCompleted() const
{
    return std::all_of(
        SessionHolder_.begin(),
        SessionHolder_.end(),
        [] (const TSession& session) {
            return session.Reader->IsFetchingCompleted();
        });
}

std::vector<TChunkId> TSchemalessSortedMergingReaderBase::GetFailedChunkIds() const
{
    std::vector<TChunkId> result;
    for (const auto& session : SessionHolder_) {
        auto failedChunks = session.Reader->GetFailedChunkIds();
        result.insert(result.end(), failedChunks.begin(), failedChunks.end());
    }
    return result;
}

void TSchemalessSortedMergingReaderBase::Interrupt()
{
    Interrupting_ = true;
}

TNameTablePtr TSchemalessSortedMergingReaderBase::GetNameTable() const
{
    return SessionHolder_.front().Reader->GetNameTable();
}

TKeyColumns TSchemalessSortedMergingReaderBase::GetKeyColumns() const
{
    return SessionHolder_.front().Reader->GetKeyColumns();
}

i64 TSchemalessSortedMergingReaderBase::GetTotalRowCount() const
{
    return RowCount_;
}

i64 TSchemalessSortedMergingReaderBase::GetSessionRowIndex() const
{
    return RowIndex_;
}

i64 TSchemalessSortedMergingReaderBase::GetTableRowIndex() const
{
    return TableRowIndex_;
}

TFuture<void> TSchemalessSortedMergingReaderBase::CombineCompletionError(TFuture<void> future)
{
    auto promise = NewPromise<void>();
    promise.TrySetFrom(future);
    promise.TrySetFrom(CompletionError_.ToFuture());
    return promise.ToFuture();
}

////////////////////////////////////////////////////////////////////////////////

// ToDo(psushin): unittests.
class TSchemalessSortedMergingReader
    : public TSchemalessSortedMergingReaderBase
{
public:
    TSchemalessSortedMergingReader(
        const std::vector<ISchemalessMultiChunkReaderPtr>& readers,
        int keyColumnCount);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;

    virtual std::vector<TDataSliceDescriptor> GetUnreadDataSliceDescriptors(
        const TRange<TUnversionedRow>& unreadRows) const override;

private:
    TOwningKey LastKey_;
};

////////////////////////////////////////////////////////////////////////////////

TSchemalessSortedMergingReader::TSchemalessSortedMergingReader(
    const std::vector<ISchemalessMultiChunkReaderPtr>& readers,
    int keyColumnCount)
    : TSchemalessSortedMergingReaderBase(keyColumnCount)
{
    YCHECK(!readers.empty());
    int rowsPerSession = RowBufferSize / readers.size();

    YCHECK(rowsPerSession > 0);

    SessionHolder_.reserve(readers.size());
    SessionHeap_.reserve(readers.size());

    for (const auto& reader : readers) {
        SessionHolder_.emplace_back(reader, rowsPerSession);
        RowCount_ += reader->GetTotalRowCount();
    }

    LOG_INFO("Opening schemaless sorted merging reader (SessionCount: %v)",
        SessionHolder_.size());

    ReadyEvent_ = CombineCompletionError(BIND(
        &TSchemalessSortedMergingReader::DoOpen,
        MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
            .Run());
}

bool TSchemalessSortedMergingReader::Read(std::vector<TUnversionedRow>* rows)
{
    YCHECK(rows->capacity() > 0);

    rows->clear();

    if (!ReadyEvent_.IsSet() || !ReadyEvent_.Get().IsOK()) {
        return true;
    }

    if (SessionHeap_.empty()) {
        return false;
    }

    auto* session = SessionHeap_.front();
    if (session->CurrentRowIndex == session->Rows.size()) {
        session->CurrentRowIndex = 0;
        if (!session->Reader->Read(&session->Rows)) {
            YCHECK(session->Rows.empty());
            ExtractHeap(SessionHeap_.begin(), SessionHeap_.end(), CompareSessions_);
            SessionHeap_.pop_back();
        } else if (session->Rows.empty()) {
            ReadyEvent_ = CombineCompletionError(session->Reader->GetReadyEvent());
        } else {
            AdjustHeapFront(SessionHeap_.begin(), SessionHeap_.end(), CompareSessions_);
        }

        return true;
    }

    TableRowIndex_ = session->Reader->GetTableRowIndex() - session->Rows.size() + session->CurrentRowIndex;

    i64 dataWeight = 0;
    bool interrupting = Interrupting_;
    while (rows->size() < rows->capacity() && dataWeight < MaxDataSizePerRead) {
        const auto& row = session->Rows[session->CurrentRowIndex];
        if (interrupting && CompareRows(row, LastKey_, KeyColumnCount_) != 0) {
            LOG_DEBUG("Sorted merging reader interrupted (LastKey: %v, NextKey: %v)",
                LastKey_,
                GetKeyPrefix(row, KeyColumnCount_));
            ReadyEvent_ = VoidFuture;
            SessionHeap_.clear();
            return !rows->empty();
        }
        rows->push_back(row);
        dataWeight += GetDataWeight(rows->back());
        ++session->CurrentRowIndex;
        ++TableRowIndex_;
        ++RowIndex_;

        if (session->CurrentRowIndex == session->Rows.size()) {
            // Out of prefetched rows in this session.
            break;
        }

        AdjustHeapFront(SessionHeap_.begin(), SessionHeap_.end(), CompareSessions_);

        if (SessionHeap_.front() != session) {
            session = SessionHeap_.front();
            TableRowIndex_ = session->Reader->GetTableRowIndex() - session->Rows.size() + session->CurrentRowIndex;
        }
    }
    if (!rows->empty() && !interrupting) {
        LastKey_ = GetKeyPrefix(rows->back(), KeyColumnCount_);
    }
    return true;
}

std::vector<TDataSliceDescriptor> TSchemalessSortedMergingReader::GetUnreadDataSliceDescriptors(
    const TRange<TUnversionedRow>& unreadRows) const
{
    std::vector<TDataSliceDescriptor> result;
    if (unreadRows.Empty()) {
        for (const auto& session : SessionHolder_) {
            auto unreadDescriptors = session.Reader->GetUnreadDataSliceDescriptors(
                NYT::TRange<TUnversionedRow>(
                    session.Rows.data() + session.CurrentRowIndex,
                    session.Rows.data() + session.Rows.size()));
            std::move(unreadDescriptors.begin(), unreadDescriptors.end(), std::back_inserter(result));
        }
    } else {
        auto firstUnreadKey = GetKeyPrefix(unreadRows[0], KeyColumnCount_);
        for (const auto& session : SessionHolder_) {
            auto it = std::lower_bound(
                session.Rows.begin(),
                session.Rows.begin() + session.CurrentRowIndex,
                firstUnreadKey,
                [&] (const TUnversionedRow& row, const TOwningKey& key) -> bool {
                    return CompareRows(row, key, KeyColumnCount_) < 0;
                });
            auto firstUnreadRowIndex = std::distance(session.Rows.begin(), it);
            auto unreadDescriptors = session.Reader->GetUnreadDataSliceDescriptors(
                NYT::TRange<TUnversionedRow>(
                    session.Rows.data() + firstUnreadRowIndex,
                    session.Rows.data() + session.Rows.size()));
            std::move(unreadDescriptors.begin(), unreadDescriptors.end(), std::back_inserter(result));
        }
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

class TSchemalessJoiningReader
    : public TSchemalessSortedMergingReaderBase
{
public:
    TSchemalessJoiningReader(
        const std::vector<ISchemalessMultiChunkReaderPtr>& primaryReaders,
        int primaryKeyColumnCount,
        const std::vector<ISchemalessMultiChunkReaderPtr>& foreignReaders,
        int foreignKeyColumnCount,
        bool interruptAtKeyEdge);

    virtual bool Read(std::vector<TUnversionedRow>* rows) override;

    virtual std::vector<TDataSliceDescriptor> GetUnreadDataSliceDescriptors(
        const TRange<TUnversionedRow>& unreadRows) const override;

    virtual void Interrupt() override;

private:
    const int PrimaryKeyColumnCount_;
    bool InterruptAtKeyEdge_ = true;

    TSession* PrimarySession_;
    TOwningKey LastPrimaryKey_;
};

////////////////////////////////////////////////////////////////////////////////

TSchemalessJoiningReader::TSchemalessJoiningReader(
    const std::vector<ISchemalessMultiChunkReaderPtr>& primaryReaders,
    int primaryKeyColumnCount,
    const std::vector<ISchemalessMultiChunkReaderPtr>& foreignReaders,
    int foreignKeyColumnCount,
    bool interruptAtKeyEdge)
    : TSchemalessSortedMergingReaderBase(foreignKeyColumnCount)
    , PrimaryKeyColumnCount_(primaryKeyColumnCount)
    , InterruptAtKeyEdge_(interruptAtKeyEdge)
{
    YCHECK(!primaryReaders.empty() && !foreignReaders.empty());
    YCHECK(interruptAtKeyEdge || primaryReaders.size() == 1);

    ISchemalessMultiChunkReaderPtr mergingReader;
    if (primaryReaders.size() == 1) {
        mergingReader = primaryReaders[0];
    } else {
        mergingReader = New<TSchemalessSortedMergingReader>(primaryReaders, primaryKeyColumnCount);
    }

    int primaryRowsPerSession = std::max<int>(RowBufferSize / 2, 2);
    int foreignRowsPerSession = std::max<int>(primaryRowsPerSession / foreignReaders.size(), 2);

    SessionHolder_.reserve(foreignReaders.size() + 1);
    SessionHeap_.reserve(foreignReaders.size() + 1);

    SessionHolder_.emplace_back(mergingReader, primaryRowsPerSession);
    RowCount_ = mergingReader->GetTotalRowCount();
    for (const auto& reader : foreignReaders) {
        SessionHolder_.emplace_back(reader, foreignRowsPerSession);
        RowCount_ += reader->GetTotalRowCount();
    }
    PrimarySession_ = &SessionHolder_[0];

    LOG_INFO("Opening schemaless sorted joining reader (SessionCount: %v)",
        SessionHolder_.size());

    ReadyEvent_ = CombineCompletionError(BIND(
        &TSchemalessJoiningReader::DoOpen,
        MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
            .Run());
}

bool TSchemalessJoiningReader::Read(std::vector<TUnversionedRow>* rows)
{
    YCHECK(rows->capacity() > 0);

    rows->clear();

    if (!ReadyEvent_.IsSet() || !ReadyEvent_.Get().IsOK()) {
        return true;
    }

    bool interrupting = Interrupting_;
    if (interrupting && SessionHeap_.front() == PrimarySession_ && !InterruptAtKeyEdge_) {
        // Extract primary session from session heap.
        ExtractHeap(SessionHeap_.begin(), SessionHeap_.end(), CompareSessions_);
        SessionHeap_.pop_back();
    }

    if (SessionHeap_.empty()) {
        return false;
    }

    auto* session = SessionHeap_.front();
    if (session->CurrentRowIndex == session->Rows.size()) {
        session->CurrentRowIndex = 0;
        if (!session->Reader->Read(&session->Rows)) {
            YCHECK(session->Rows.empty());
            ExtractHeap(SessionHeap_.begin(), SessionHeap_.end(), CompareSessions_);
            SessionHeap_.pop_back();
        } else if (session->Rows.empty()) {
            ReadyEvent_ = CombineCompletionError(session->Reader->GetReadyEvent());
        } else {
            AdjustHeapFront(SessionHeap_.begin(), SessionHeap_.end(), CompareSessions_);
        }

        return true;
    }

    TableRowIndex_ = session->Reader->GetTableRowIndex() - session->Rows.size() + session->CurrentRowIndex;

    i64 dataWeight = 0;
    auto lastPrimaryRow = TKey(LastPrimaryKey_);
    auto nextPrimaryRow = TKey();
    while (rows->size() < rows->capacity() && dataWeight < MaxDataSizePerRead) {
        const auto& row = session->Rows[session->CurrentRowIndex];
        if (interrupting) {
            if (CompareRows(
                row,
                lastPrimaryRow,
                (session == PrimarySession_) ? PrimaryKeyColumnCount_ : KeyColumnCount_) > 0)
            {
                // Immediately stop reader on key change.
                SessionHeap_.clear();
                return !rows->empty();
            }
        }

        bool shouldJoinRow = false;
        // Skip rows that are not present in PrimarySession_.
        if (session == PrimarySession_) {
            lastPrimaryRow = row;
            nextPrimaryRow = TKey();
            shouldJoinRow = true;
        } else {
            if (!nextPrimaryRow && PrimarySession_->CurrentRowIndex < PrimarySession_->Rows.size()) {
                nextPrimaryRow = PrimarySession_->Rows[PrimarySession_->CurrentRowIndex];
            }
            if (
                (CompareRows(row, lastPrimaryRow, KeyColumnCount_) == 0) ||
                (CompareRows(row, nextPrimaryRow, KeyColumnCount_) == 0))
            {
                shouldJoinRow = true;
            }
        }
        if (shouldJoinRow) {
            rows->push_back(row);
            dataWeight += GetDataWeight(rows->back());
            ++RowIndex_;
        } else {
            --RowCount_;
        }
        ++session->CurrentRowIndex;
        ++TableRowIndex_;

        if (session->CurrentRowIndex == session->Rows.size()) {
            // Out of prefetched rows in this session.
            break;
        }

        AdjustHeapFront(SessionHeap_.begin(), SessionHeap_.end(), CompareSessions_);

        if (SessionHeap_.front() != session) {
            session = SessionHeap_.front();
            TableRowIndex_ = session->Reader->GetTableRowIndex() - session->Rows.size() + session->CurrentRowIndex;
        }
    }
    if (lastPrimaryRow) {
        LastPrimaryKey_ = GetKeyPrefix(lastPrimaryRow, PrimaryKeyColumnCount_);
    }
    return true;
}

std::vector<TDataSliceDescriptor> TSchemalessJoiningReader::GetUnreadDataSliceDescriptors(
    const TRange<TUnversionedRow>& unreadRows) const
{
    YCHECK(unreadRows.Empty());

    // Returns only UnreadDataSlices from primary readers.
    return PrimarySession_->Reader->GetUnreadDataSliceDescriptors(
        NYT::TRange<TUnversionedRow>(
            PrimarySession_->Rows.data() + PrimarySession_->CurrentRowIndex,
            PrimarySession_->Rows.size() - PrimarySession_->CurrentRowIndex));
}

void TSchemalessJoiningReader::Interrupt()
{
    TSchemalessSortedMergingReaderBase::Interrupt();

    // Return ready event to consumer if we don't wait for key edge.
    if (!InterruptAtKeyEdge_) {
        CompletionError_.TrySet();
    }
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkReaderPtr CreateSchemalessSortedMergingReader(
    const std::vector<ISchemalessMultiChunkReaderPtr>& readers,
    int keyColumnCount)
{
    YCHECK(!readers.empty());
    if (readers.size() == 1) {
        return readers[0];
    } else {
        return New<TSchemalessSortedMergingReader>(readers, keyColumnCount);
    }
}

ISchemalessMultiChunkReaderPtr CreateSchemalessSortedJoiningReader(
    const std::vector<ISchemalessMultiChunkReaderPtr>& primaryReaders,
    int primaryKeyColumnCount,
    const std::vector<ISchemalessMultiChunkReaderPtr>& foreignReaders,
    int foreignKeyColumnCount)
{
    YCHECK(!primaryReaders.empty());
    if (foreignReaders.empty()) {
        return New<TSchemalessSortedMergingReader>(
            primaryReaders,
            primaryKeyColumnCount);
    } else {
        return New<TSchemalessJoiningReader>(
            primaryReaders,
            primaryKeyColumnCount,
            foreignReaders,
            foreignKeyColumnCount,
            true);
    }
}

ISchemalessMultiChunkReaderPtr CreateSchemalessJoinReduceJoiningReader(
    const std::vector<ISchemalessMultiChunkReaderPtr>& primaryReaders,
    int primaryKeyColumnCount,
    const std::vector<ISchemalessMultiChunkReaderPtr>& foreignReaders,
    int foreignKeyColumnCount)
{
    YCHECK(primaryReaders.size() == 1);
    if (foreignReaders.empty()) {
        return primaryReaders[0];
    } else {
        return New<TSchemalessJoiningReader>(
            primaryReaders,
            primaryKeyColumnCount,
            foreignReaders,
            foreignKeyColumnCount,
            false);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
