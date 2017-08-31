#include "lookup.h"
#include "private.h"
#include "store.h"
#include "tablet.h"
#include "tablet_slot.h"
#include "tablet_profiling.h"

#include <yt/server/tablet_node/config.h>

#include <yt/ytlib/table_client/config.h>
#include <yt/ytlib/table_client/row_merger.h>
#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/versioned_reader.h>

#include <yt/ytlib/tablet_client/wire_protocol.h>
#include <yt/ytlib/tablet_client/wire_protocol.pb.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profile_manager.h>
#include <yt/core/profiling/profiler.h>
#include <yt/core/profiling/scoped_timer.h>

#include <yt/core/misc/nullable.h>
#include <yt/core/misc/protobuf_helpers.h>
#include <yt/core/misc/small_vector.h>
#include <yt/core/misc/variant.h>
#include <yt/core/misc/tls_cache.h>

////////////////////////////////////////////////////////////////////////////////

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTabletClient::NProto;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletNodeLogger;
static const size_t RowBufferCapacity = 1000;

////////////////////////////////////////////////////////////////////////////////

struct TLookupCounters
{
    TLookupCounters(const TTagIdList& list)
        : RowCount("/lookup/row_count", list)
        , DataWeight("/lookup/data_weight", list)
        , CpuTime("/lookup/cpu_time", list)
    { }

    TSimpleCounter RowCount;
    TSimpleCounter DataWeight;
    TSimpleCounter CpuTime;
};

using TLookupProfilerTrait = TTabletProfilerTrait<TLookupCounters>;

////////////////////////////////////////////////////////////////////////////////

struct TLookupSessionBufferTag
{ };

class TLookupSession
{
public:
    TLookupSession(
        TTabletSnapshotPtr tabletSnapshot,
        TTimestamp timestamp,
        const TString& user,
        bool produceAllVersions,
        const TColumnFilter& columnFilter,
        const TWorkloadDescriptor& workloadDescriptor,
        TSharedRange<TUnversionedRow> lookupKeys)
        : TabletSnapshot_(std::move(tabletSnapshot))
        , Timestamp_(timestamp)
        , ProduceAllVersions_(produceAllVersions)
        , ColumnFilter_(columnFilter)
        , WorkloadDescriptor_(workloadDescriptor)
        , LookupKeys_(std::move(lookupKeys))
    {
        if (TabletSnapshot_->IsProfilingEnabled()) {
            Tags_ = GetUserProfilerTags(user, TabletSnapshot_->ProfilerTags);
        }
    }

    void Run(
        const std::function<void(TVersionedRow)>& onPartialRow,
        const std::function<std::pair<bool, size_t>()>& onRow)
    {
        LOG_DEBUG("Tablet lookup started (TabletId: %v, CellId: %v, KeyCount: %v)",
            TabletSnapshot_->TabletId,
            TabletSnapshot_->CellId,
            LookupKeys_.Size());

        TCpuTimer timer;

        CreateReadSessions(&EdenSessions_, TabletSnapshot_->GetEdenStores(), LookupKeys_);

        auto currentIt = LookupKeys_.Begin();
        while (currentIt != LookupKeys_.End()) {
            auto nextPartitionIt = std::upper_bound(
                TabletSnapshot_->PartitionList.begin(),
                TabletSnapshot_->PartitionList.end(),
                *currentIt,
                [] (TKey lhs, const TPartitionSnapshotPtr& rhs) {
                    return lhs < rhs->PivotKey;
                });
            YCHECK(nextPartitionIt != TabletSnapshot_->PartitionList.begin());
            auto nextIt = nextPartitionIt == TabletSnapshot_->PartitionList.end()
                ? LookupKeys_.End()
                : std::lower_bound(currentIt, LookupKeys_.End(), (*nextPartitionIt)->PivotKey);

            LookupInPartition(
                *(nextPartitionIt - 1),
                LookupKeys_.Slice(currentIt, nextIt),
                onPartialRow,
                onRow);

            currentIt = nextIt;
        }

        auto cpuTime = timer.GetCpuValue();

        if (IsProfilingEnabled()) {
            auto& counters = GetLocallyGloballyCachedValue<TLookupProfilerTrait>(Tags_);
            TabletNodeProfiler.Increment(counters.RowCount, FoundRowCount_);
            TabletNodeProfiler.Increment(counters.DataWeight, FoundDataWeight_);
            TabletNodeProfiler.Increment(counters.CpuTime, cpuTime);
        }

        LOG_DEBUG("Tablet lookup completed (TabletId: %v, CellId: %v, FoundRowCount: %v, FoundDataWeight: %v, CpuTime: %v)",
            TabletSnapshot_->TabletId,
            TabletSnapshot_->CellId,
            FoundRowCount_,
            FoundDataWeight_,
            ValueToDuration(cpuTime));
    }

private:
    class TReadSession
    {
    public:
        explicit TReadSession(IVersionedReaderPtr reader)
            : Reader_(std::move(reader))
        {
            Rows_.reserve(RowBufferCapacity);
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
        const IVersionedReaderPtr Reader_;

        std::vector<TVersionedRow> Rows_;

        int RowIndex_ = -1;

    };

    const TTabletSnapshotPtr TabletSnapshot_;
    const TTimestamp Timestamp_;
    const bool ProduceAllVersions_;
    const TColumnFilter& ColumnFilter_;
    const TWorkloadDescriptor& WorkloadDescriptor_;
    const TSharedRange<TUnversionedRow> LookupKeys_;

    static const int TypicalSessionCount = 16;
    using TReadSessionList = SmallVector<TReadSession, TypicalSessionCount>;
    TReadSessionList EdenSessions_;
    TReadSessionList PartitionSessions_;

    int FoundRowCount_ = 0;
    size_t FoundDataWeight_ = 0;

    TTagIdList Tags_;

    bool IsProfilingEnabled() const
    {
        return !Tags_.empty();
    }

    void CreateReadSessions(
        TReadSessionList* sessions,
        const std::vector<ISortedStorePtr>& stores,
        const TSharedRange<TKey>& keys)
    {
        sessions->clear();

        // NB: Will remain empty for in-memory tables.
        std::vector<TFuture<void>> asyncFutures;
        for (const auto& store : stores) {
            auto reader = store->CreateReader(
                TabletSnapshot_,
                keys,
                Timestamp_,
                ProduceAllVersions_,
                ColumnFilter_,
                WorkloadDescriptor_);
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
        const TPartitionSnapshotPtr& partitionSnapshot,
        const TSharedRange<TKey>& keys,
        const std::function<void(TVersionedRow)>& onPartialRow,
        const std::function<std::pair<bool, size_t>()>& onRow)
    {
        if (keys.Empty() || !partitionSnapshot) {
            return;
        }

        CreateReadSessions(&PartitionSessions_, partitionSnapshot->Stores, keys);

        auto processSessions = [&] (TReadSessionList& sessions) {
            for (auto& session : sessions) {
                onPartialRow(session.FetchRow());
            }
        };

        for (int index = 0; index < keys.Size(); ++index) {
            processSessions(PartitionSessions_);
            processSessions(EdenSessions_);

            auto statistics = onRow();
            FoundRowCount_ += statistics.first;
            FoundDataWeight_ += statistics.second;
        }
    }
};

static NTableClient::TColumnFilter DecodeColumnFilter(
    std::unique_ptr<NTabletClient::NProto::TColumnFilter> protoColumnFilter,
    int columnCount)
{
    NTableClient::TColumnFilter columnFilter;
    if (!protoColumnFilter) {
        columnFilter.All = true;
    } else {
        columnFilter.All = false;
        columnFilter.Indexes = FromProto<SmallVector<int, TypicalColumnCount>>(protoColumnFilter->indexes());
    }
    ValidateColumnFilter(columnFilter, columnCount);
    return columnFilter;
}

void LookupRows(
    TTabletSnapshotPtr tabletSnapshot,
    TTimestamp timestamp,
    const TString& user,
    const TWorkloadDescriptor& workloadDescriptor,
    TWireProtocolReader* reader,
    TWireProtocolWriter* writer)
{
    TReqLookupRows req;
    reader->ReadMessage(&req);

    auto columnFilter = DecodeColumnFilter(
        std::unique_ptr<NTabletClient::NProto::TColumnFilter>(req.release_column_filter()),
        tabletSnapshot->PhysicalSchema.GetColumnCount());
    auto schemaData = TWireProtocolReader::GetSchemaData(tabletSnapshot->PhysicalSchema.ToKeys());
    auto lookupKeys = reader->ReadSchemafulRowset(schemaData, false);

    TSchemafulRowMerger merger(
        New<TRowBuffer>(TLookupSessionBufferTag()),
        tabletSnapshot->PhysicalSchema.Columns().size(),
        tabletSnapshot->PhysicalSchema.GetKeyColumnCount(),
        columnFilter,
        tabletSnapshot->ColumnEvaluator);

    TLookupSession session(
        std::move(tabletSnapshot),
        timestamp,
        user,
        false,
        columnFilter,
        workloadDescriptor,
        std::move(lookupKeys));

    session.Run(
        [&] (TVersionedRow partialRow) { merger.AddPartialRow(partialRow); },
        [&] {
            auto mergedRow = merger.BuildMergedRow();
            writer->WriteSchemafulRow(mergedRow);
            return std::make_pair(static_cast<bool>(mergedRow),GetDataWeight(mergedRow));
        });
}

void VersionedLookupRows(
    TTabletSnapshotPtr tabletSnapshot,
    TTimestamp timestamp,
    const TString& user,
    const TWorkloadDescriptor& workloadDescriptor,
    TRetentionConfigPtr retentionConfig,
    TWireProtocolReader* reader,
    TWireProtocolWriter* writer)
{
    TReqVersionedLookupRows req;
    reader->ReadMessage(&req);

    auto columnFilter = DecodeColumnFilter(
        std::unique_ptr<NTabletClient::NProto::TColumnFilter>(req.release_column_filter()),
        tabletSnapshot->PhysicalSchema.GetColumnCount());
    auto schemaData = TWireProtocolReader::GetSchemaData(tabletSnapshot->PhysicalSchema.ToKeys());
    auto lookupKeys = reader->ReadSchemafulRowset(schemaData, false);

    TVersionedRowMerger merger(
        New<TRowBuffer>(TLookupSessionBufferTag()),
        tabletSnapshot->PhysicalSchema.GetColumnCount(),
        tabletSnapshot->PhysicalSchema.GetKeyColumnCount(),
        columnFilter,
        std::move(retentionConfig),
        timestamp,
        MinTimestamp,
        tabletSnapshot->ColumnEvaluator,
        true);

    TLookupSession session(
        std::move(tabletSnapshot),
        timestamp,
        user,
        true,
        columnFilter,
        workloadDescriptor,
        std::move(lookupKeys));

    session.Run(
        [&] (TVersionedRow partialRow) { merger.AddPartialRow(partialRow); },
        [&] {
            auto mergedRow = merger.BuildMergedRow();
            writer->WriteVersionedRow(mergedRow);
            return std::make_pair(static_cast<bool>(mergedRow), GetDataWeight(mergedRow));
        });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
