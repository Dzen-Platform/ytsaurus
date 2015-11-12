#include "stdafx.h"
#include "lookup.h"
#include "tablet.h"
#include "store.h"
#include "tablet_slot.h"
#include "private.h"

#include <core/misc/object_pool.h>
#include <core/misc/protobuf_helpers.h>

#include <core/concurrency/scheduler.h>

#include <core/logging/log.h>

#include <ytlib/tablet_client/wire_protocol.h>
#include <ytlib/tablet_client/wire_protocol.pb.h>
#include <ytlib/table_client/row_merger.h>

#include <ytlib/table_client/versioned_reader.h>

////////////////////////////////////////////////////////////////////////////////

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletClient::NProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletNodeLogger;

////////////////////////////////////////////////////////////////////////////////

struct TLookupPoolTag { };

class TLookupSession
{
public:
    TLookupSession()
        : MemoryPool_(TLookupPoolTag())
    { }

    void Prepare(
        TTabletSnapshotPtr tabletSnapshot,
        TTimestamp timestamp,
        TWireProtocolReader* reader)
    {
        Clean();

        TabletSnapshot_ = std::move(tabletSnapshot);
        Timestamp_ = timestamp;
        KeyColumnCount_ = TabletSnapshot_->KeyColumns.size();
        SchemaColumnCount_ = TabletSnapshot_->Schema.Columns().size();

        TReqLookupRows req;
        reader->ReadMessage(&req);

        if (req.has_column_filter()) {
            ColumnFilter_.All = false;
            ColumnFilter_.Indexes = FromProto<int, SmallVector<int, TypicalColumnCount>>(req.column_filter().indexes());
        } else {
            ColumnFilter_.All = true;
        }

        ValidateColumnFilter(ColumnFilter_, SchemaColumnCount_);

        LookupKeys_ = reader->ReadUnversionedRowset();
    }

    TFutureHolder<void> Run(TWireProtocolWriter* writer)
    {
        try {
            DoRun(writer);
            return VoidFuture;
        } catch (const std::exception& ex) {
            return MakeFuture(TError(ex));
        }
    }

    const TSharedRange<TUnversionedRow>& GetLookupKeys() const
    {
        return LookupKeys_;
    }

    void Clean()
    {
        MemoryPool_.Clear();
        LookupKeys_ = TSharedRange<TUnversionedRow>();
        EdenSessions_.clear();
    }

private:
    class TReadSession
    {
    public:
        explicit TReadSession(IVersionedReaderPtr reader)
            : Reader_(std::move(reader))
        {
            Rows_.reserve(BufferCapacity);
        }

        TVersionedRow FetchRow()
        {
            ++RowIndex_;
            if (RowIndex_ >= Rows_.size()) {
                RowIndex_ = 0;
                while (true) {
                    YCHECK(Reader_->Read(&Rows_));
                    if (!Rows_.empty()) {
                        break;
                    }
                    WaitFor(Reader_->GetReadyEvent())
                        .ThrowOnError();
                }
            }
            return Rows_[RowIndex_];
        }

    private:
        static const int BufferCapacity = 1000;
        IVersionedReaderPtr Reader_;
        std::vector<TVersionedRow> Rows_;
        int RowIndex_ = -1;
    };

    TChunkedMemoryPool MemoryPool_;
    TSharedRange<TUnversionedRow> LookupKeys_;
    std::vector<TReadSession> EdenSessions_;
    std::vector<TReadSession> PartitionSessions_;

    TTabletSnapshotPtr TabletSnapshot_;
    TTimestamp Timestamp_;
    int KeyColumnCount_;
    int SchemaColumnCount_;
    TColumnFilter ColumnFilter_;


    void CreateReadSessions(
        std::vector<TReadSession>* sessions,
        const TPartitionSnapshotPtr partitionSnapshot,
        const TSharedRange<TKey>& keys)
    {
        sessions->clear();
        if (!partitionSnapshot) {
            return;
        }

        std::vector<TFuture<void>> asyncFutures;
        for (const auto& store : partitionSnapshot->Stores) {
            auto reader = store->CreateReader(keys, Timestamp_, ColumnFilter_);
            auto future = reader->Open();
            auto maybeError = future.TryGet();
            if (maybeError) {
                maybeError->ThrowOnError();
            } else {
                asyncFutures.emplace_back(std::move(future));
            }
            sessions->emplace_back(std::move(reader));
        }

        if (!asyncFutures.empty()) {
            WaitFor(Combine(asyncFutures))
                .ThrowOnError();
        }
    }

    void LookupInPartition(
        const TPartitionSnapshotPtr partitionSnapshot,
        const TSharedRange<TKey>& keys,
        TWireProtocolWriter* writer)
    {
        if (keys.Empty()) {
            return;
        }

        CreateReadSessions(&PartitionSessions_, partitionSnapshot, keys);

        TSchemafulRowMerger merger(
            &MemoryPool_,
            SchemaColumnCount_,
            KeyColumnCount_,
            ColumnFilter_);

        auto processSessions = [&] (std::vector<TReadSession>& sessions) {
            for (auto& session : sessions) {
                merger.AddPartialRow(session.FetchRow());
            }
        };

        for (int index = 0; index < keys.Size(); ++index) {
            processSessions(PartitionSessions_);
            processSessions(EdenSessions_);

            auto mergedRow = merger.BuildMergedRow();
            writer->WriteUnversionedRow(mergedRow);
        }
    }

    void DoRun(TWireProtocolWriter* writer)
    {
        CreateReadSessions(&EdenSessions_, TabletSnapshot_->Eden, LookupKeys_);

        TPartitionSnapshotPtr currentPartitionSnapshot;
        int currentPartitionStartOffset = 0;
        for (int index = 0; index < LookupKeys_.Size(); ++index) {
            YASSERT(index == 0 || LookupKeys_[index] >= LookupKeys_[index - 1]);
            auto key = LookupKeys_[index];
            ValidateServerKey(key, KeyColumnCount_, TabletSnapshot_->Schema);
            auto partitionSnapshot = TabletSnapshot_->FindContainingPartition(key);
            if (partitionSnapshot != currentPartitionSnapshot) {
                LookupInPartition(
                    currentPartitionSnapshot,
                    LookupKeys_.Slice(currentPartitionStartOffset, index),
                    writer);
                currentPartitionSnapshot = std::move(partitionSnapshot);
                currentPartitionStartOffset = index;
            }
        }

        LookupInPartition(
            currentPartitionSnapshot,
            LookupKeys_.Slice(currentPartitionStartOffset, LookupKeys_.Size()),
            writer);
    }
};

} // namespace NTabletNode
} // namespace NYT

namespace NYT {

template <>
struct TPooledObjectTraits<NTabletNode::TLookupSession, void>
    : public TPooledObjectTraitsBase
{
    static void Clean(NTabletNode::TLookupSession* executor)
    {
        executor->Clean();
    }
};

} // namespace NYT

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

void LookupRows(
    TTabletSnapshotPtr tabletSnapshot,
    TTimestamp timestamp,
    TWireProtocolReader* reader,
    TWireProtocolWriter* writer)
{
    auto session = ObjectPool<TLookupSession>().Allocate();
    session->Prepare(tabletSnapshot, timestamp, reader);
    LOG_DEBUG("Performing tablet lookup (TabletId: %v, CellId: %v, KeyCount: %v, Session: %v)",
        tabletSnapshot->TabletId,
        tabletSnapshot->Slot ? tabletSnapshot->Slot->GetCellId() : NullCellId,
        session->GetLookupKeys().Size(),
        session.get());

    auto resultHolder = session->Run(writer);
    return WaitFor(resultHolder.Get())
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
