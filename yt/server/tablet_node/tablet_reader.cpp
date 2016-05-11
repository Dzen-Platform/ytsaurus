#include "tablet_reader.h"
#include "private.h"
#include "config.h"
#include "partition.h"
#include "store.h"
#include "tablet.h"
#include "tablet_slot.h"

#include <yt/ytlib/table_client/overlapping_reader.h>
#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/row_merger.h>
#include <yt/ytlib/table_client/schemaful_concatencaing_reader.h>
#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/unordered_schemaful_reader.h>
#include <yt/ytlib/table_client/versioned_reader.h>
#include <yt/ytlib/table_client/versioned_row.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/chunked_memory_pool.h>
#include <yt/core/misc/heap.h>

#include <yt/core/misc/error.h>
#include <yt/core/misc/range.h>

namespace NYT {
namespace NTabletNode {

using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

struct TTabletReaderPoolTag { };

static const auto& Logger = TabletNodeLogger;

////////////////////////////////////////////////////////////////////////////////

struct TStoreRangeFormatter
{
    void operator()(TStringBuilder* builder, const ISortedStorePtr& store) const
    {
        builder->AppendFormat("<%v:%v>",
            store->GetMinKey(),
            store->GetMaxKey());
    }
};

////////////////////////////////////////////////////////////////////////////////

namespace {

ISchemafulReaderPtr CreateSchemafulSortedTabletReader(
    TTabletSnapshotPtr tabletSnapshot,
    const TColumnFilter& columnFilter,
    TOwningKey lowerBound,
    TOwningKey upperBound,
    TTimestamp timestamp,
    const TWorkloadDescriptor& workloadDescriptor)
{
    std::vector<ISortedStorePtr> stores;

    // Pick stores which intersect [lowerBound, upperBound) (excluding upperBound).
    auto takePartition = [&] (const TPartitionSnapshotPtr& partitionSnapshot) {
        for (const auto& store : partitionSnapshot->Stores) {
            if (store->GetMinKey() < upperBound && store->GetMaxKey() >= lowerBound) {
                stores.push_back(store);
            }
        }
    };

    takePartition(tabletSnapshot->Eden);

    auto range = tabletSnapshot->GetIntersectingPartitions(lowerBound, upperBound);
    for (auto it = range.first; it != range.second; ++it) {
        takePartition(*it);
    }

    LOG_DEBUG("Creating schemaful sorted tablet reader (TabletId: %v, CellId: %v, Timestamp: %v, "
        "LowerBound: %v, UpperBound: %v, WorkloadDescriptor: %v, StoreIds: %v, StoreRanges: %v)",
        tabletSnapshot->TabletId,
        tabletSnapshot->CellId,
        timestamp,
        lowerBound,
        upperBound,
        workloadDescriptor,
        MakeFormattableRange(stores, TStoreIdFormatter()),
        MakeFormattableRange(stores, TStoreRangeFormatter()));

    if (stores.size() > tabletSnapshot->Config->MaxReadFanIn) {
        THROW_ERROR_EXCEPTION("Read fan-in limit exceeded; please wait until your data is merged")
            << TErrorAttribute("tablet_id", tabletSnapshot->TabletId)
            << TErrorAttribute("fan_in", stores.size())
            << TErrorAttribute("fan_in_limit", tabletSnapshot->Config->MaxReadFanIn);
    }

    auto rowMerger = New<TSchemafulRowMerger>(
        New<TRowBuffer>(TRefCountedTypeTag<TTabletReaderPoolTag>()),
        tabletSnapshot->QuerySchema.Columns().size(),
        tabletSnapshot->QuerySchema.GetKeyColumnCount(),
        columnFilter,
        tabletSnapshot->ColumnEvaluator);

    std::vector<TOwningKey> boundaries;
    boundaries.reserve(stores.size());
    for (const auto& store : stores) {
        boundaries.push_back(store->GetMinKey());
    }

    return CreateSchemafulOverlappingRangeReader(
        std::move(boundaries),
        std::move(rowMerger),
        [=, stores = std::move(stores)] (int index) {
            YASSERT(index < stores.size());
            return stores[index]->CreateReader(
                tabletSnapshot,
                lowerBound,
                upperBound,
                timestamp,
                columnFilter,
                workloadDescriptor);
        },
        [keyComparer = tabletSnapshot->RowKeyComparer] (
            const TUnversionedValue* lhsBegin,
            const TUnversionedValue* lhsEnd,
            const TUnversionedValue* rhsBegin,
            const TUnversionedValue* rhsEnd) {
            return keyComparer(lhsBegin, lhsEnd, rhsBegin, rhsEnd);
        });
}

ISchemafulReaderPtr CreateSchemafulOrderedTabletReader(
    TTabletSnapshotPtr tabletSnapshot,
    const TColumnFilter& columnFilter,
    TOwningKey lowerBound,
    TOwningKey upperBound,
    TTimestamp /*timestamp*/,
    const TWorkloadDescriptor& workloadDescriptor)
{
    YCHECK(lowerBound.GetCount() >= 1 && lowerBound.GetCount() <= 2);
    YCHECK(upperBound.GetCount() >= 1 && upperBound.GetCount() <= 2);

    auto valueToInt = [] (const TUnversionedValue& value) {
        switch (value.Type) {
            case EValueType::Int64:
                return value.Data.Int64;
            case EValueType::Min:
                return std::numeric_limits<i64>::min();
            case EValueType::Max:
                return std::numeric_limits<i64>::max();
            default:
                YUNREACHABLE();
        }
    };

    int tabletIndex = 0;
    i64 lowerRowIndex = 0;
    i64 upperRowIndex = std::numeric_limits<i64>::max();
    if (lowerBound < upperBound) {
        YCHECK(lowerBound[0].Type == EValueType::Int64);
        tabletIndex = static_cast<int>(lowerBound[0].Data.Int64);

        YCHECK(upperBound[0].Type == EValueType::Int64 ||
               upperBound[0].Type == EValueType::Max);
        YCHECK(upperBound[0].Type != EValueType::Int64 ||
               lowerBound[0].Data.Int64 == upperBound[0].Data.Int64 ||
               lowerBound[0].Data.Int64 + 1 == upperBound[0].Data.Int64);

        if (lowerBound.GetCount() == 2) {
            lowerRowIndex = valueToInt(lowerBound[1]);
        }

        if (upperBound.GetCount() == 2) {
            upperRowIndex = valueToInt(upperBound[1]);
        }
    }

    std::vector<ISchemafulReaderPtr> readers;
    if (lowerRowIndex < upperRowIndex && !tabletSnapshot->StoreList.empty()) {
        auto lowerIt = std::upper_bound(
            tabletSnapshot->StoreList.begin(),
            tabletSnapshot->StoreList.end(),
            lowerRowIndex,
            [] (i64 lhs, const IOrderedStorePtr& rhs) {
                return lhs < rhs->GetStartingRowIndex();
            }) - 1;
        auto it = lowerIt;
        while (it != tabletSnapshot->StoreList.end()) {
            const auto& store = *it;
            if (store->GetStartingRowIndex() >= upperRowIndex) {
                break;
            }
            readers.emplace_back(store->CreateReader(
                tabletSnapshot,
                tabletIndex,
                lowerRowIndex,
                upperRowIndex,
                columnFilter,
                workloadDescriptor));
            ++it;
        }
    }

    return CreateSchemafulConcatencatingReader(readers);
}

} // namespace

ISchemafulReaderPtr CreateSchemafulTabletReader(
    TTabletSnapshotPtr tabletSnapshot,
    const TColumnFilter& columnFilter,
    TOwningKey lowerBound,
    TOwningKey upperBound,
    TTimestamp timestamp,
    const TWorkloadDescriptor& workloadDescriptor)
{
    if (tabletSnapshot->TableSchema.IsSorted()) {
        return CreateSchemafulSortedTabletReader(
            std::move(tabletSnapshot),
            columnFilter,
            std::move(lowerBound),
            std::move(upperBound),
            timestamp,
            workloadDescriptor);
    } else {
        return CreateSchemafulOrderedTabletReader(
            std::move(tabletSnapshot),
            columnFilter,
            std::move(lowerBound),
            std::move(upperBound),
            timestamp,
            workloadDescriptor);
    }
}

////////////////////////////////////////////////////////////////////////////////

namespace {

ISchemafulReaderPtr CreateSchemafulPartitionReader(
    TTabletSnapshotPtr tabletSnapshot,
    const TColumnFilter& columnFilter,
    TPartitionSnapshotPtr paritionSnapshot,
    const TSharedRange<TKey>& keys,
    TTimestamp timestamp,
    const TWorkloadDescriptor& workloadDescriptor,
    TRowBufferPtr rowBuffer)
{
    auto minKey = *keys.Begin();
    auto maxKey = *(keys.End() - 1);
    std::vector<ISortedStorePtr> stores;

    // Pick stores which intersect [minKey, maxKey] (including maxKey).
    auto takePartition = [&] (const TPartitionSnapshotPtr& partitionSnapshot) {
        YASSERT(partitionSnapshot);
        for (const auto& store : partitionSnapshot->Stores) {
            if (store->GetMinKey() <= maxKey && store->GetMaxKey() >= minKey) {
                stores.push_back(store);
            }
        }
    };

    takePartition(tabletSnapshot->Eden);
    takePartition(paritionSnapshot);

    LOG_DEBUG("Creating schemaful tablet reader (TabletId: %v, CellId: %v, Timestamp: %v, WorkloadDescriptor: %v, "
        "StoreIds: %v, StoreRanges: %v)",
        tabletSnapshot->TabletId,
        tabletSnapshot->CellId,
        timestamp,
        workloadDescriptor,
        MakeFormattableRange(stores, TStoreIdFormatter()),
        MakeFormattableRange(stores, TStoreRangeFormatter()));

    auto rowMerger = New<TSchemafulRowMerger>(
        rowBuffer
            ? std::move(rowBuffer)
            : New<TRowBuffer>(TRefCountedTypeTag<TTabletReaderPoolTag>()),
        tabletSnapshot->QuerySchema.Columns().size(),
        tabletSnapshot->QuerySchema.GetKeyColumnCount(),
        columnFilter,
        tabletSnapshot->ColumnEvaluator);

    return CreateSchemafulOverlappingLookupReader(
        std::move(rowMerger),
        [=, stores = std::move(stores), index = 0] () mutable -> IVersionedReaderPtr {
            if (index < stores.size()) {
                return stores[index++]->CreateReader(
                    tabletSnapshot,
                    keys,
                    timestamp,
                    columnFilter,
                    workloadDescriptor);
            } else {
                return nullptr;
            }
        });
}

} // namespace

ISchemafulReaderPtr CreateSchemafulTabletReader(
    TTabletSnapshotPtr tabletSnapshot,
    const TColumnFilter& columnFilter,
    const TSharedRange<TKey>& keys,
    TTimestamp timestamp,
    const TWorkloadDescriptor& workloadDescriptor,
    int concurrency,
    TRowBufferPtr rowBuffer)
{
    YCHECK(!rowBuffer || concurrency == 1);

    if (!tabletSnapshot->TableSchema.IsSorted()) {
        THROW_ERROR_EXCEPTION("Table %v is not sorted",
            tabletSnapshot->TableId);
    }

    std::vector<TPartitionSnapshotPtr> partitions;
    std::vector<TSharedRange<TKey>> partitionedKeys;
    auto currentIt = keys.Begin();
    while (currentIt != keys.End()) {
        auto nextPartitionIt = std::upper_bound(
            tabletSnapshot->PartitionList.begin(),
            tabletSnapshot->PartitionList.end(),
            *currentIt,
            [] (TKey lhs, const TPartitionSnapshotPtr& rhs) {
                return lhs < rhs->PivotKey;
            });
        YCHECK(nextPartitionIt != tabletSnapshot->PartitionList.begin());
        auto nextIt = nextPartitionIt == tabletSnapshot->PartitionList.end()
            ? keys.End()
            : std::lower_bound(currentIt, keys.End(), (*nextPartitionIt)->PivotKey);
        partitions.push_back(*(nextPartitionIt - 1));
        partitionedKeys.push_back(keys.Slice(currentIt, nextIt));
        currentIt = nextIt;
    }

    auto readerFactory = [
        =,
        tabletSnapshot = std::move(tabletSnapshot),
        columnFilter = std::move(columnFilter),
        partitions = std::move(partitions),
        partitionedKeys = std::move(partitionedKeys),
        rowBuffer = std::move(rowBuffer),
        index = 0
    ] () mutable -> ISchemafulReaderPtr {
        if (index < partitionedKeys.size()) {
            auto reader = CreateSchemafulPartitionReader(
                tabletSnapshot,
                columnFilter,
                partitions[index],
                partitionedKeys[index],
                timestamp,
                workloadDescriptor,
                rowBuffer);
            ++index;
            return reader;
        } else {
            return nullptr;
        }
    };

    return CreateUnorderedSchemafulReader(std::move(readerFactory), concurrency);
}

////////////////////////////////////////////////////////////////////////////////

IVersionedReaderPtr CreateVersionedTabletReader(
    IInvokerPtr poolInvoker,
    TTabletSnapshotPtr tabletSnapshot,
    std::vector<ISortedStorePtr> stores,
    TOwningKey lowerBound,
    TOwningKey upperBound,
    TTimestamp currentTimestamp,
    TTimestamp majorTimestamp,
    const TWorkloadDescriptor& workloadDescriptor)
{
    if (!tabletSnapshot->TableSchema.IsSorted()) {
        THROW_ERROR_EXCEPTION("Table %v is not sorted",
            tabletSnapshot->TableId);
    }

    LOG_DEBUG(
        "Creating versioned tablet reader (TabletId: %v, CellId: %v, LowerBound: %v, UpperBound: %v, "
        "CurrentTimestamp: %v, MajorTimestamp: %v, WorkloadDescriptor: %v, StoreIds: %v, StoreRanges: %v)",
        tabletSnapshot->TabletId,
        tabletSnapshot->CellId,
        lowerBound,
        upperBound,
        currentTimestamp,
        majorTimestamp,
        workloadDescriptor,
        MakeFormattableRange(stores, TStoreIdFormatter()),
        MakeFormattableRange(stores, TStoreRangeFormatter()));

    auto rowMerger = New<TVersionedRowMerger>(
        New<TRowBuffer>(TRefCountedTypeTag<TTabletReaderPoolTag>()),
        tabletSnapshot->QuerySchema.GetKeyColumnCount(),
        tabletSnapshot->Config,
        currentTimestamp,
        majorTimestamp,
        tabletSnapshot->ColumnEvaluator);

    std::vector<TOwningKey> boundaries;
    boundaries.reserve(stores.size());
    for (const auto& store : stores) {
        boundaries.push_back(store->GetMinKey());
    }

    return CreateVersionedOverlappingRangeReader(
        std::move(boundaries),
        std::move(rowMerger),
        [=, stores = std::move(stores)] (int index) {
            YASSERT(index < stores.size());
            return stores[index]->CreateReader(
                tabletSnapshot,
                lowerBound,
                upperBound,
                AllCommittedTimestamp,
                TColumnFilter(),
                workloadDescriptor);
        },
        [keyComparer = tabletSnapshot->RowKeyComparer] (
            const TUnversionedValue* lhsBegin,
            const TUnversionedValue* lhsEnd,
            const TUnversionedValue* rhsBegin,
            const TUnversionedValue* rhsEnd)
        {
            return keyComparer(lhsBegin, lhsEnd, rhsBegin, rhsEnd);
        });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT

