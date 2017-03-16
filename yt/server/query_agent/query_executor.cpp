#include "query_executor.h"
#include "private.h"
#include "config.h"

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/server/data_node/chunk_block_manager.h>
#include <yt/server/data_node/chunk.h>
#include <yt/server/data_node/chunk_registry.h>
#include <yt/server/data_node/local_chunk_reader.h>
#include <yt/server/data_node/master_connector.h>

#include <yt/server/hydra/hydra_manager.h>

#include <yt/server/tablet_node/config.h>
#include <yt/server/tablet_node/security_manager.h>
#include <yt/server/tablet_node/slot_manager.h>
#include <yt/server/tablet_node/tablet.h>
#include <yt/server/tablet_node/tablet_manager.h>
#include <yt/server/tablet_node/tablet_reader.h>
#include <yt/server/tablet_node/tablet_slot.h>

#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/native_client.h>

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/chunk_spec.pb.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/replication_reader.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/query_client/callbacks.h>
#include <yt/ytlib/query_client/column_evaluator.h>
#include <yt/ytlib/query_client/coordinator.h>
#include <yt/ytlib/query_client/evaluator.h>
#include <yt/ytlib/query_client/functions_cache.h>
#include <yt/ytlib/query_client/helpers.h>
#include <yt/ytlib/query_client/query.h>
#include <yt/ytlib/query_client/query_helpers.h>
#include <yt/ytlib/query_client/private.h>
#include <yt/ytlib/query_client/query_statistics.h>
#include <yt/ytlib/query_client/executor.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/config.h>
#include <yt/ytlib/table_client/pipe.h>
#include <yt/ytlib/table_client/schemaful_chunk_reader.h>
#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/schemaful_writer.h>
#include <yt/ytlib/table_client/unordered_schemaful_reader.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/string.h>
#include <yt/core/misc/collection_helpers.h>

namespace NYT {
namespace NQueryAgent {

using namespace NYTree;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NQueryClient;
using namespace NTabletClient;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NNodeTrackerClient;
using namespace NTabletNode;
using namespace NDataNode;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

namespace {

TColumnFilter GetColumnFilter(const TTableSchema& desiredSchema, const TTableSchema& tabletSchema)
{
    // Infer column filter.
    TColumnFilter columnFilter;
    columnFilter.All = false;
    for (const auto& column : desiredSchema.Columns()) {
        const auto& tabletColumn = tabletSchema.GetColumnOrThrow(column.Name);
        if (tabletColumn.Type != column.Type) {
            THROW_ERROR_EXCEPTION("Mismatched type of column %Qv in schema: expected %Qlv, found %Qlv",
                column.Name,
                tabletColumn.Type,
                column.Type);
        }
        columnFilter.Indexes.push_back(tabletSchema.GetColumnIndex(tabletColumn));
    }

    return columnFilter;
}

struct TRangeFormatter
{
    void operator()(TStringBuilder* builder, const TRowRange& source) const
    {
        builder->AppendFormat("[%v .. %v]",
            source.first,
            source.second);
    }
};

struct TDataKeys
{
    //! Either a chunk id or tablet id.
    NObjectClient::TObjectId Id;
    TSharedRange<TRow> Keys;
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

struct TQuerySubexecutorBufferTag
{ };

////////////////////////////////////////////////////////////////////////////////

class TTabletSnapshotCache
{
public:
    explicit TTabletSnapshotCache(TSlotManagerPtr slotManager)
        : SlotManager_(std::move(slotManager))
    { }

    void RegisterTabletSnapshotOrThrow(
        const TTabletId& tabletId,
        const i64 mountRevision,
        const TTimestamp timestamp)
    {
        auto tabletSnapshot = SlotManager_->GetTabletSnapshotOrThrow(tabletId);

        tabletSnapshot->ValidateMountRevision(mountRevision);

        SlotManager_->ValidateTabletAccess(
            tabletSnapshot,
            NYTree::EPermission::Read,
            timestamp);

        Map_.insert(std::make_pair(tabletId, tabletSnapshot));
    }

    TTabletSnapshotPtr GetCachedTabletSnapshot(const TTabletId& tabletId)
    {
        auto it = Map_.find(tabletId);
        YCHECK(it != Map_.end());
        return it->second;
    }

private:
    const TSlotManagerPtr SlotManager_;
    yhash_map<TTabletId, TTabletSnapshotPtr> Map_;

};

////////////////////////////////////////////////////////////////////////////////

class TQueryExecution
    : public TIntrinsicRefCounted
{
public:
    TQueryExecution(
        TQueryAgentConfigPtr config,
        TFunctionImplCachePtr functionImplCache,
        TBootstrap* const bootstrap,
        const TEvaluatorPtr evaluator,
        TConstQueryPtr query,
        const TQueryOptions& options)
        : Config_(std::move(config))
        , FunctionImplCache_(std::move(functionImplCache))
        , Bootstrap_(bootstrap)
        , Evaluator_(std::move(evaluator))
        , Query_(std::move(query))
        , Options_(std::move(options))
        , Logger(MakeQueryLogger(Query_))
        , TabletSnapshots_(bootstrap->GetTabletSlotManager())
    { }

    TFuture<TQueryStatistics> Execute(
        TConstExternalCGInfoPtr externalCGInfo,
        std::vector<TDataRanges> dataSources,
        ISchemafulWriterPtr writer)
    {
        for (const auto& source : dataSources) {
            if (TypeFromId(source.Id) == EObjectType::Tablet) {
                TabletSnapshots_.RegisterTabletSnapshotOrThrow(
                    source.Id,
                    source.MountRevision,
                    Options_.Timestamp);
            } else {
                THROW_ERROR_EXCEPTION("Unsupported data split type %Qlv",
                    TypeFromId(source.Id));
            }
        }

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        auto maybeUser = securityManager->GetAuthenticatedUser();

        return BIND(&TQueryExecution::DoExecute, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetQueryPoolInvoker())
            .Run(
                std::move(externalCGInfo),
                std::move(dataSources),
                std::move(writer),
                maybeUser);
    }

private:
    const TQueryAgentConfigPtr Config_;
    const TFunctionImplCachePtr FunctionImplCache_;
    TBootstrap* const Bootstrap_;
    const TEvaluatorPtr Evaluator_;

    const TConstQueryPtr Query_;
    const TQueryOptions Options_;

    const NLogging::TLogger Logger;

    TTabletSnapshotCache TabletSnapshots_;

    typedef std::function<ISchemafulReaderPtr()> TSubreaderCreator;

    void LogSplits(const std::vector<TDataRanges>& splits)
    {
        if (Options_.VerboseLogging) {
            for (const auto& split : splits) {
                LOG_DEBUG("Ranges in split %v: %v",
                    split.Id,
                    MakeFormattableRange(split.Ranges, TRangeFormatter()));
            }
        }
    }

    TQueryStatistics DoCoordinateAndExecute(
        TConstExternalCGInfoPtr externalCGInfo,
        ISchemafulWriterPtr writer,
        const std::vector<TRefiner>& refiners,
        const std::vector<TSubreaderCreator>& subreaderCreators)
    {
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        auto maybeUser = securityManager->GetAuthenticatedUser();

        NApi::TClientOptions clientOptions;
        if (maybeUser) {
            clientOptions.User = maybeUser.Get();
        }

        auto client = Bootstrap_
            ->GetMasterClient()
            ->GetNativeConnection()
            ->CreateNativeClient(clientOptions);

        auto remoteExecutor = CreateQueryExecutor(
            client->GetNativeConnection(),
            client->GetHeavyChannelFactory(),
            FunctionImplCache_);

        auto functionGenerators = New<TFunctionProfilerMap>();
        auto aggregateGenerators = New<TAggregateProfilerMap>();
        MergeFrom(functionGenerators.Get(), *BuiltinFunctionCG);
        MergeFrom(aggregateGenerators.Get(), *BuiltinAggregateCG);
        FetchImplementations(
            functionGenerators,
            aggregateGenerators,
            externalCGInfo,
            FunctionImplCache_);

        return CoordinateAndExecute(
            Query_,
            writer,
            refiners,
            [&] (TConstQueryPtr subquery, int index) {
                auto mergingReader = subreaderCreators[index]();

                auto pipe = New<TSchemafulPipe>();

                LOG_DEBUG("Evaluating subquery (SubqueryId: %v)", subquery->Id);

                auto asyncSubqueryResults = std::make_shared<std::vector<TFuture<TQueryStatistics>>>();

                auto foreignExecuteCallback = [
                    asyncSubqueryResults,
                    externalCGInfo,
                    remoteExecutor,
                    this,
                    this_ = MakeStrong(this)
                ] (
                    const TQueryPtr& subquery,
                    TDataRanges dataRanges,
                    ISchemafulWriterPtr writer)
                {
                    LOG_DEBUG("Evaluating remote subquery (SubqueryId: %v)", subquery->Id);

                    auto remoteOptions = Options_;
                    remoteOptions.MaxSubqueries = 16;

                    auto asyncResult = remoteExecutor->Execute(
                        subquery,
                        externalCGInfo,
                        std::move(dataRanges),
                        writer,
                        remoteOptions);

                    asyncSubqueryResults->push_back(asyncResult);

                    return asyncResult;
                };

                auto asyncStatistics = BIND(&TEvaluator::RunWithExecutor, Evaluator_)
                    .AsyncVia(Bootstrap_->GetQueryPoolInvoker())
                    .Run(subquery,
                        mergingReader,
                        pipe->GetWriter(),
                        foreignExecuteCallback,
                        functionGenerators,
                        aggregateGenerators,
                        Options_.EnableCodeCache);

                asyncStatistics.Apply(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TQueryStatistics>& result) -> TErrorOr<TQueryStatistics>{
                    if (!result.IsOK()) {
                        pipe->Fail(result);
                        LOG_DEBUG(result, "Failed evaluating subquery (SubqueryId: %v)", subquery->Id);
                        return result;
                    } else {
                        TQueryStatistics statistics = result.Value();

                        for (const auto& asyncSubqueryResult : *asyncSubqueryResults) {
                            auto subqueryStatistics = WaitFor(asyncSubqueryResult)
                                .ValueOrThrow();

                            LOG_DEBUG("Remote subquery statistics %v", subqueryStatistics);
                            statistics += subqueryStatistics;
                        }

                        return statistics;
                    }
                }));

                return std::make_pair(pipe->GetReader(), asyncStatistics);
            },
            [&] (TConstQueryPtr topQuery, ISchemafulReaderPtr reader, ISchemafulWriterPtr writer) {
                LOG_DEBUG("Evaluating top query (TopQueryId: %v)", topQuery->Id);
                auto result = Evaluator_->Run(
                    topQuery,
                    std::move(reader),
                    std::move(writer),
                    functionGenerators,
                    aggregateGenerators,
                    Options_.EnableCodeCache);
                LOG_DEBUG("Finished evaluating top query (TopQueryId: %v)", topQuery->Id);
                return result;
            });
    }

    TQueryStatistics DoExecute(
        TConstExternalCGInfoPtr externalCGInfo,
        std::vector<TDataRanges> dataSources,
        ISchemafulWriterPtr writer,
        const TNullable<Stroka>& maybeUser)
    {
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager, maybeUser);

        LOG_DEBUG("Classifying data sources into ranges and lookup keys");

        std::vector<TDataRanges> rangesByTablet;

        auto rowBuffer = New<TRowBuffer>(TQuerySubexecutorBufferTag());

        auto keySize = Query_->OriginalSchema.GetKeyColumnCount();
        size_t rangesCount = 0;
        for (const auto& source : dataSources) {
            TRowRanges rowRanges;
            std::vector<TRow> keys;

            auto pushRanges = [&] () {
                if (!rowRanges.empty()) {
                    rangesCount += rowRanges.size();
                    TDataRanges item;
                    item.Id = source.Id;
                    item.Ranges = MakeSharedRange(std::move(rowRanges), source.Ranges.GetHolder());
                    item.LookupSupported = source.LookupSupported;
                    rangesByTablet.emplace_back(std::move(item));
                }
            };

            auto pushKeys = [&] () {
                if (!keys.empty()) {
                    TDataRanges item;
                    item.Id = source.Id;
                    item.Keys = MakeSharedRange(std::move(keys), source.Ranges.GetHolder());
                    item.LookupSupported = source.LookupSupported;
                    rangesByTablet.emplace_back(std::move(item));
                }
            };

            for (const auto& range : source.Ranges) {
                auto lowerBound = range.first;
                auto upperBound = range.second;

                if (source.LookupSupported &&
                    keySize == lowerBound.GetCount() &&
                    keySize + 1 == upperBound.GetCount() &&
                    upperBound[keySize].Type == EValueType::Max &&
                    CompareRows(lowerBound.Begin(), lowerBound.End(), upperBound.Begin(), upperBound.Begin() + keySize) == 0)
                {
                    pushRanges();
                    keys.push_back(lowerBound);
                } else {
                    pushKeys();
                    rowRanges.push_back(range);
                }
            }

            for (const auto& key : source.Keys) {
                auto rowSize = key.GetCount();
                if (source.LookupSupported &&
                    keySize == key.GetCount())
                {
                    pushRanges();
                    keys.push_back(key);
                } else {
                    auto lowerBound = key;

                    auto upperBound = rowBuffer->AllocateUnversioned(rowSize + 1);
                    for (int column = 0; column < rowSize; ++column) {
                        upperBound[column] = lowerBound[column];
                    }

                    upperBound[rowSize] = MakeUnversionedSentinelValue(EValueType::Max);
                    pushKeys();
                    rowRanges.emplace_back(lowerBound, upperBound);
                }
            }
            pushRanges();
            pushKeys();
        }

        LOG_DEBUG("Splitting %v ranges", rangesCount);

        auto splits = Split(std::move(rangesByTablet), rowBuffer);

        std::vector<TRefiner> refiners;
        std::vector<TSubreaderCreator> subreaderCreators;

        auto processSplitsRanges = [&] (int beginIndex, int endIndex) {
            if (beginIndex == endIndex) {
                return;
            }

            std::vector<TDataRanges> groupedSplit(splits.begin() + beginIndex, splits.begin() + endIndex);
            std::vector<TRowRange> keyRanges;
            for (const auto& dataRange : groupedSplit) {
                keyRanges.insert(keyRanges.end(), dataRange.Ranges.Begin(), dataRange.Ranges.End());
            }

            refiners.push_back([MOVE(keyRanges), inferRanges = Query_->InferRanges] (
                TConstExpressionPtr expr,
                const TKeyColumns& keyColumns)
            {
                if (inferRanges) {
                    return EliminatePredicate(keyRanges, expr, keyColumns);
                } else {
                    return expr;
                }
            });
            subreaderCreators.push_back([&, MOVE(groupedSplit)] () {
                size_t rangesCount = std::accumulate(
                    groupedSplit.begin(),
                    groupedSplit.end(),
                    0,
                    [] (size_t sum, const TDataRanges& element) {
                        return sum + element.Ranges.Size();
                    });
                LOG_DEBUG("Generating reader for %v splits from %v ranges",
                    groupedSplit.size(),
                    rangesCount);

                LogSplits(groupedSplit);

                auto bottomSplitReaderGenerator = [
                    MOVE(groupedSplit),
                    index = 0,
                    this,
                    this_ = MakeStrong(this)
                ] () mutable -> ISchemafulReaderPtr {
                    if (index == groupedSplit.size()) {
                        return nullptr;
                    }

                    const auto& group = groupedSplit[index++];
                    return GetMultipleRangesReader(group.Id, group.Ranges);
                };

                // TODO(lukyan): Use prefetching ordered reader after merge with branch 19.1
                return CreateUnorderedSchemafulReader(std::move(bottomSplitReaderGenerator), 1);
            });
        };

        auto processSplitKeys = [&] (int index) {
            const auto& tablePartId = splits[index].Id;
            auto& keys = splits[index].Keys;

            refiners.push_back([&, inferRanges = Query_->InferRanges] (
                TConstExpressionPtr expr, const
                TKeyColumns& keyColumns)
            {
                if (inferRanges) {
                    return EliminatePredicate(keys, expr, keyColumns);
                } else {
                    return expr;
                }
            });
            subreaderCreators.push_back([&, MOVE(keys)] () {
                return GetTabletReader(tablePartId, keys);
            });
        };

        int splitCount = splits.size();
        auto maxSubqueries = std::min({Options_.MaxSubqueries, Config_->MaxSubqueries, splitCount});
        int splitOffset = 0;
        int queryIndex = 1;
        int nextSplitOffset = queryIndex * splitCount / maxSubqueries;
        for (size_t splitIndex = 0; splitIndex < splitCount;) {
            if (splits[splitIndex].Keys) {
                processSplitsRanges(splitOffset, splitIndex);
                processSplitKeys(splitIndex);
                splitOffset = ++splitIndex;
            } else {
                ++splitIndex;
            }

            if (splitIndex == nextSplitOffset) {
                processSplitsRanges(splitOffset, nextSplitOffset);
                splitOffset = nextSplitOffset;
                ++queryIndex;
                nextSplitOffset = queryIndex * splitCount / maxSubqueries;
            }
        }

        YCHECK(splitOffset == splitCount);

        return DoCoordinateAndExecute(
            externalCGInfo,
            std::move(writer),
            refiners,
            subreaderCreators);
    }

    std::vector<TSharedRange<TRowRange>> SplitTablet(
        const std::vector<TPartitionSnapshotPtr>& partitions,
        TSharedRange<TRowRange> ranges,
        TRowBufferPtr rowBuffer)
    {
        auto verboseLogging = Options_.VerboseLogging;

        auto holder = MakeHolder(ranges.GetHolder(), rowBuffer);

        TRow lowerCapBound = rowBuffer->Capture(partitions.front()->PivotKey);
        TRow upperCapBound = rowBuffer->Capture(partitions.back()->NextPivotKey);

        struct TGroup
        {
            std::vector<TPartitionSnapshotPtr>::const_iterator PartitionIt;
            TSharedRange<TRowRange>::iterator BeginIt;
            TSharedRange<TRowRange>::iterator EndIt;
        };

        std::vector<TGroup> groupedByPartitions;

        auto appendGroup = [&] (const TGroup& group) {
            if (!groupedByPartitions.empty() && groupedByPartitions.back().PartitionIt == group.PartitionIt) {
                Y_ASSERT(groupedByPartitions.back().EndIt < group.EndIt);
                groupedByPartitions.back().EndIt = group.EndIt;
            } else {
                groupedByPartitions.push_back(group);
            }
        };

        for (auto rangesIt = begin(ranges); rangesIt != end(ranges);) {
            auto lowerBound = std::max(rangesIt->first, lowerCapBound);
            auto upperBound = std::min(rangesIt->second, upperCapBound);

            if (lowerBound >= upperBound) {
                ++rangesIt;
                continue;
            }

            // Run binary search to find the relevant partitions.
            auto startIt = std::upper_bound(
                partitions.begin(),
                partitions.end(),
                lowerBound,
                [] (TKey lhs, const TPartitionSnapshotPtr& rhs) {
                    return lhs < rhs->NextPivotKey;
                });
            YCHECK(startIt != partitions.end());

            auto nextPivotKey = (*startIt)->NextPivotKey.Get();

            if (upperBound < nextPivotKey) {
                auto rangesItEnd = std::upper_bound(
                    rangesIt,
                    end(ranges),
                    nextPivotKey,
                    [] (TKey key, const TRowRange& rowRange) {
                        return key < rowRange.second;
                    });

                appendGroup(TGroup{
                    startIt,
                    rangesIt,
                    rangesItEnd});
                rangesIt = rangesItEnd;
            } else {
                auto nextRangeIt = rangesIt;
                ++nextRangeIt;

                for (auto it = startIt; it != partitions.end() && (*it)->PivotKey < upperBound; ++it) {
                    appendGroup(TGroup{
                        it,
                        rangesIt,
                        nextRangeIt});
                }
                rangesIt = nextRangeIt;
            }
        }

        auto iterate = [&] (auto onRanges, auto onSamples) {
            for (const auto& group : groupedByPartitions) {
                // calculate touched sample count

                auto partitionIt = group.PartitionIt;
                const auto& sampleKeys = (*partitionIt)->SampleKeys->Keys;

                TRow pivot = rowBuffer->Capture((*partitionIt)->PivotKey);
                TRow nextPivot = rowBuffer->Capture((*partitionIt)->NextPivotKey);

                LOG_DEBUG_IF(verboseLogging, "Iterating over partition (%v .. %v): [%v .. %v]",
                    pivot,
                    nextPivot,
                    group.BeginIt - begin(ranges),
                    group.EndIt - begin(ranges));

                for (auto rangesIt = group.BeginIt; rangesIt != group.EndIt;) {
                    auto lowerBound = rangesIt == group.BeginIt
                        ? std::max(rangesIt->first, pivot)
                        : rangesIt->first;
                    auto upperBound = rangesIt + 1 == group.EndIt
                        ? std::min(rangesIt->second, nextPivot)
                        : rangesIt->second;

                    auto startSampleIt = std::upper_bound(sampleKeys.begin(), sampleKeys.end(), lowerBound);

                    auto nextPivotKey = startSampleIt == sampleKeys.end()
                        ? (*partitionIt)->NextPivotKey.Get()
                        : *startSampleIt;

                    if (upperBound < nextPivotKey) {
                        auto rangesItEnd = std::upper_bound(
                            rangesIt,
                            group.EndIt,
                            nextPivotKey,
                            [] (TKey key, const TRowRange& rowRange) {
                                return key < rowRange.second;
                            });
                        onRanges(rangesIt, rangesItEnd, pivot, nextPivot);
                        rangesIt = rangesItEnd;
                    } else {
                        auto endSampleIt = std::lower_bound(startSampleIt, sampleKeys.end(), upperBound);
                        onSamples(rangesIt, startSampleIt, endSampleIt, pivot, nextPivot);
                        ++rangesIt;
                    }
                }
            }
        };

        size_t totalSampleCount = 0;
        size_t totalBatchCount = 0;
        iterate(
            [&] (auto rangesIt, auto rangesItEnd, auto pivot, auto nextPivot) {
                ++totalBatchCount;
            },
            [&] (auto rangesIt, auto startSampleIt, auto endSampleIt, auto pivot, auto nextPivot) {
                ++totalBatchCount;
                totalSampleCount += std::distance(startSampleIt, endSampleIt);
            });

        size_t freeSlotCount = Config_->MaxSubsplitsPerTablet > totalBatchCount
            ? Config_->MaxSubsplitsPerTablet - totalBatchCount
            : 0;
        size_t cappedSampleCount = std::min(freeSlotCount, totalSampleCount);

        LOG_DEBUG_IF(verboseLogging, "Total sample count: %v", totalSampleCount);
        LOG_DEBUG_IF(verboseLogging, "Capped sample count: %v", cappedSampleCount);

        size_t sampleIndex = 0;
        size_t nextSampleCount;
        auto incrementSampleIndex = [&] {
            ++sampleIndex;
            nextSampleCount = cappedSampleCount != 0
                ? sampleIndex * totalSampleCount / cappedSampleCount
                : totalSampleCount;
        };

        incrementSampleIndex();

        size_t currentSampleCount = 0;

        std::vector<TSharedRange<TRowRange>> groupedSplits;
        std::vector<TRowRange> group;

        auto addGroup = [&] () {
            YCHECK(!group.empty());
            LOG_DEBUG_IF(verboseLogging, "(%v, %v) make batch [%v .. %v] from %v ranges",
                currentSampleCount,
                nextSampleCount,
                group.front().second,
                group.back().second,
                group.size());
            groupedSplits.push_back(MakeSharedRange(std::move(group), holder));
        };

        iterate(
            [&] (auto rangesIt, auto rangesItEnd, auto pivot, auto nextPivot) {
                for (auto it = rangesIt; it != rangesItEnd; ++it) {
                    auto lowerBound = it == rangesIt ? std::max(it->first, pivot) : it->first;
                    auto upperBound = it + 1 == rangesItEnd ? std::min(it->second, nextPivot) : it->second;

                    group.emplace_back(lowerBound, upperBound);
                }
                addGroup();
            },
            [&] (auto rangesIt, auto startSampleIt, auto endSampleIt, auto pivot, auto nextPivot) {
                TRow lowerBound = std::max(rangesIt->first, pivot);
                TRow upperBound = std::min(rangesIt->second, nextPivot);

                auto currentBound = lowerBound;

                size_t sampleCount = std::distance(startSampleIt, endSampleIt);
                size_t savedSampleCount = currentSampleCount;
                currentSampleCount += sampleCount;

                YCHECK(nextSampleCount >= savedSampleCount);

                auto it = startSampleIt;
                while (nextSampleCount < currentSampleCount) {
                    size_t step = nextSampleCount - savedSampleCount;
                    it += step;
                    savedSampleCount += step;

                    auto nextBound = rowBuffer->Capture(*it);
                    group.emplace_back(currentBound, nextBound);
                    currentBound = nextBound;

                    addGroup();
                    incrementSampleIndex();
                }

                group.emplace_back(currentBound, upperBound);

                addGroup();
            });

        return groupedSplits;
    }

    std::vector<TDataRanges> Split(std::vector<TDataRanges> rangesByTablet, TRowBufferPtr rowBuffer)
    {
        std::vector<TDataRanges> groupedSplits;

        for (auto& tablePartIdRange : rangesByTablet) {
            auto tablePartId = tablePartIdRange.Id;
            auto& ranges = tablePartIdRange.Ranges;

            auto tabletSnapshot = TabletSnapshots_.GetCachedTabletSnapshot(tablePartId);

            YCHECK(tablePartIdRange.Keys.Empty() != ranges.Empty());

            if (!tabletSnapshot->TableSchema.IsSorted() || ranges.Empty()) {
                groupedSplits.push_back(tablePartIdRange);
                continue;
            }

            YCHECK(std::is_sorted(
                ranges.Begin(),
                ranges.End(),
                [] (const TRowRange& lhs, const TRowRange& rhs) {
                    return lhs.first < rhs.first;
                }));

            const auto& partitions = tabletSnapshot->PartitionList;
            YCHECK(!partitions.empty());

            auto splits = SplitTablet(partitions, ranges, rowBuffer);

            for (const auto& split : splits) {
                TDataRanges dataRanges;
                dataRanges.Id = tablePartId;
                dataRanges.Ranges = split;
                dataRanges.LookupSupported = tablePartIdRange.LookupSupported;

                groupedSplits.push_back(std::move(dataRanges));
            }
        }

        for (const auto& split : groupedSplits) {
            YCHECK(std::is_sorted(
                split.Ranges.Begin(),
                split.Ranges.End(),
                [] (const TRowRange& lhs, const TRowRange& rhs) {
                    return lhs.second <= rhs.first;
                }));
        }

        YCHECK(std::is_sorted(
            groupedSplits.begin(),
            groupedSplits.end(),
            [] (const TDataRanges& lhs, const TDataRanges& rhs) {
                const auto& lhsValue = lhs.Ranges ? lhs.Ranges.Back().second : lhs.Keys.Back();
                const auto& rhsValue = rhs.Ranges ? rhs.Ranges.Front().first : rhs.Keys.Front();

                return lhsValue <= rhsValue;
            }));
        return groupedSplits;
    }

    ISchemafulReaderPtr GetMultipleRangesReader(
        const TObjectId& tabletId,
        const TSharedRange<TRowRange>& bounds)
    {
        auto tabletSnapshot = TabletSnapshots_.GetCachedTabletSnapshot(tabletId);
        auto columnFilter = GetColumnFilter(Query_->GetReadSchema(), tabletSnapshot->QuerySchema);

        if (!tabletSnapshot->TableSchema.IsSorted()) {
            auto bottomSplitReaderGenerator = [
                tabletSnapshot,
                columnFilter,
                MOVE(bounds),
                index = 0,
                this,
                this_ = MakeStrong(this)
            ] () mutable -> ISchemafulReaderPtr {
                if (index == bounds.Size()) {
                    return nullptr;
                }

                const auto& range = bounds[index++];

                TOwningKey lowerBound(range.first);
                TOwningKey upperBound(range.second);

                return CreateSchemafulOrderedTabletReader(
                    tabletSnapshot,
                    columnFilter,
                    lowerBound,
                    upperBound,
                    Options_.Timestamp,
                    Options_.WorkloadDescriptor);
            };

            return CreateUnorderedSchemafulReader(std::move(bottomSplitReaderGenerator), 1);
        } else {
            return CreateSchemafulSortedTabletReader(
                std::move(tabletSnapshot),
                columnFilter,
                bounds,
                Options_.Timestamp,
                Options_.WorkloadDescriptor);
        }
    }

    ISchemafulReaderPtr GetTabletReader(
        const TTabletId& tabletId,
        const TSharedRange<TRow>& keys)
    {
        auto tabletSnapshot = TabletSnapshots_.GetCachedTabletSnapshot(tabletId);
        auto columnFilter = GetColumnFilter(Query_->GetReadSchema(), tabletSnapshot->QuerySchema);

        return CreateSchemafulTabletReader(
            std::move(tabletSnapshot),
            columnFilter,
            keys,
            Options_.Timestamp,
            Options_.WorkloadDescriptor,
            Config_->MaxBottomReaderConcurrency);
    }

};

////////////////////////////////////////////////////////////////////////////////

class TQuerySubexecutor
    : public ISubexecutor
{
public:
    TQuerySubexecutor(
        TQueryAgentConfigPtr config,
        TBootstrap* bootstrap)
        : Config_(config)
        , FunctionImplCache_(CreateFunctionImplCache(
            config->FunctionImplCache,
            bootstrap->GetMasterClient()))
        , Bootstrap_(bootstrap)
        , Evaluator_(New<TEvaluator>(Config_))
        , ColumnEvaluatorCache_(Bootstrap_
            ->GetMasterClient()
            ->GetNativeConnection()
            ->GetColumnEvaluatorCache())
    { }

    // IExecutor implementation.
    virtual TFuture<TQueryStatistics> Execute(
        TConstQueryPtr query,
        TConstExternalCGInfoPtr externalCGInfo,
        std::vector<TDataRanges> dataSources,
        ISchemafulWriterPtr writer,
        const TQueryOptions& options) override
    {
        ValidateReadTimestamp(options.Timestamp);

        auto execution = New<TQueryExecution>(
            Config_,
            FunctionImplCache_,
            Bootstrap_,
            Evaluator_,
            std::move(query),
            options);

        return execution->Execute(
            std::move(externalCGInfo),
            std::move(dataSources),
            std::move(writer));
    }

private:
    const TQueryAgentConfigPtr Config_;
    const TFunctionImplCachePtr FunctionImplCache_;
    TBootstrap* const Bootstrap_;
    const TEvaluatorPtr Evaluator_;
    const TColumnEvaluatorCachePtr ColumnEvaluatorCache_;
};

ISubexecutorPtr CreateQuerySubexecutor(
    TQueryAgentConfigPtr config,
    TBootstrap* bootstrap)
{
    return New<TQuerySubexecutor>(config, bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryAgent
} // namespace NYT

