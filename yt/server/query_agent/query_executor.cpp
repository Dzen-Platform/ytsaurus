#include "stdafx.h"

#include "query_executor.h"
#include "config.h"
#include "private.h"

#include <core/misc/string.h>

#include <core/concurrency/scheduler.h>

#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/replication_reader.h>
#include <ytlib/chunk_client/chunk_spec.pb.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <ytlib/object_client/helpers.h>

#include <ytlib/table_client/chunk_meta_extensions.h>

#include <ytlib/new_table_client/config.h>
#include <ytlib/new_table_client/schemaful_reader.h>
#include <ytlib/new_table_client/schemaful_chunk_reader.h>
#include <ytlib/new_table_client/schemaful_writer.h>
#include <ytlib/new_table_client/unordered_schemaful_reader.h>
#include <ytlib/new_table_client/pipe.h>
#include <ytlib/new_table_client/chunk_meta_extensions.h>

#include <ytlib/query_client/callbacks.h>
#include <ytlib/query_client/evaluator.h>
#include <ytlib/query_client/plan_fragment.h>
#include <ytlib/query_client/plan_helpers.h>
#include <ytlib/query_client/coordinator.h>
#include <ytlib/query_client/private.h>
#include <ytlib/query_client/helpers.h>
#include <ytlib/query_client/query_statistics.h>

#include <ytlib/tablet_client/public.h>

#include <ytlib/api/client.h>

#include <server/data_node/block_store.h>

#include <server/tablet_node/tablet_slot_manager.h>
#include <server/tablet_node/tablet_manager.h>
#include <server/tablet_node/tablet_slot.h>
#include <server/tablet_node/tablet.h>
#include <server/tablet_node/tablet_reader.h>
#include <server/tablet_node/config.h>

#include <server/hydra/hydra_manager.h>

#include <server/data_node/local_chunk_reader.h>
#include <server/data_node/chunk_registry.h>

#include <server/cell_node/bootstrap.h>
#include <server/cell_node/config.h>

namespace NYT {
namespace NQueryAgent {

using namespace NConcurrency;
using namespace NObjectClient;
using namespace NQueryClient;
using namespace NChunkClient;
using namespace NTabletClient;
using namespace NVersionedTableClient;
using namespace NVersionedTableClient::NProto;
using namespace NNodeTrackerClient;
using namespace NTabletNode;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NDataNode;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = QueryAgentLogger;

////////////////////////////////////////////////////////////////////////////////

class TLazySchemafulReader
    : public ISchemafulReader
{
public:
    explicit TLazySchemafulReader(TFuture<ISchemafulReaderPtr> futureUnderlyingReader)
        : FutureUnderlyingReader_(std::move(futureUnderlyingReader))
    { }

    virtual TFuture<void> Open(const TTableSchema& schema) override
    {
        return FutureUnderlyingReader_.Apply(
            BIND(&TLazySchemafulReader::DoOpen, MakeStrong(this), schema));
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        YASSERT(UnderlyingReader_);
        return UnderlyingReader_->Read(rows);
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        YASSERT(UnderlyingReader_);
        return UnderlyingReader_->GetReadyEvent();
    }

private:
    TFuture<ISchemafulReaderPtr> FutureUnderlyingReader_;

    ISchemafulReaderPtr UnderlyingReader_;


    TFuture<void> DoOpen(const TTableSchema& schema, const TErrorOr<ISchemafulReaderPtr>& readerOrError)
    {
        if (!readerOrError.IsOK()) {
            return MakeFuture(TError(readerOrError));
        }

        YCHECK(!UnderlyingReader_);
        UnderlyingReader_ = readerOrError.Value();

        return UnderlyingReader_->Open(schema);
    }

};

////////////////////////////////////////////////////////////////////////////////

class TRemoteExecutor
    : public IExecutor
{
public:
    explicit TRemoteExecutor(TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
    { }

    TFuture<TQueryStatistics> Execute(
        const TPlanFragmentPtr& fragment,
        ISchemafulWriterPtr writer)
    {
        return Bootstrap_->GetMasterClient()->GetQueryExecutor()->Execute(fragment, std::move(writer));
    }

private:
    TBootstrap* Bootstrap_;
};

class TQueryExecutor
    : public IExecutor
{
public:
    explicit TQueryExecutor(
        TQueryAgentConfigPtr config,
        TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , Evaluator_(New<TEvaluator>(Config_))
        , RemoteExecutor_(New<TRemoteExecutor>(bootstrap))
    { }

    // IExecutor implementation.
    virtual TFuture<TQueryStatistics> Execute(
        const TPlanFragmentPtr& fragment,
        ISchemafulWriterPtr writer) override
    {
        return BIND(&TQueryExecutor::DoExecute, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetQueryPoolInvoker())
            .Run(fragment, std::move(writer));
    }

private:
    TQueryAgentConfigPtr Config_;
    TBootstrap* Bootstrap_;

    TEvaluatorPtr Evaluator_;
    IExecutorPtr RemoteExecutor_;

    TQueryStatistics DoExecute(
        const TPlanFragmentPtr& fragment,
        ISchemafulWriterPtr writer)
    {
        auto nodeDirectory = fragment->NodeDirectory;
        auto Logger = BuildLogger(fragment->Query);
        auto splits = Split(fragment->DataSplits, nodeDirectory, Logger);
        int splitCount = splits.size();
        int splitOffset = 0;
        TGroupedDataSplits groupedSplits;

        for (int queryIndex = 1; queryIndex <= Config_->MaxSubqueries; ++queryIndex) {
            int nextSplitOffset = queryIndex * splitCount / Config_->MaxSubqueries;
            if (splitOffset != nextSplitOffset) {
                groupedSplits.emplace_back(splits.begin() + splitOffset, splits.begin() + nextSplitOffset);
                splitOffset = nextSplitOffset;
            }
        }

        auto ranges = GetRanges(groupedSplits);

        return CoordinateAndExecute(
            fragment,
            writer,
            false,
            ranges,
            [&] (const TConstQueryPtr& subquery, size_t index) {
                std::vector<ISchemafulReaderPtr> bottomSplitReaders;
                for (const auto& dataSplit : groupedSplits[index]) {
                    bottomSplitReaders.push_back(GetReader(dataSplit, nodeDirectory));
                }
                auto mergingReader = CreateUnorderedSchemafulReader(bottomSplitReaders);

                auto pipe = New<TSchemafulPipe>();

                auto asyncStatistics = BIND(&TEvaluator::RunWithExecutor, Evaluator_)
                    .AsyncVia(Bootstrap_->GetBoundedConcurrencyQueryPoolInvoker())
                    .Run(subquery, mergingReader, pipe->GetWriter(), [&] (const TQueryPtr& subquery, ISchemafulWriterPtr writer) -> TQueryStatistics {
                        auto planFragment = New<TPlanFragment>();

                        planFragment->NodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
                        planFragment->Query = subquery;
                        planFragment->DataSplits.push_back(fragment->ForeignDataSplit);

                        auto subqueryResult = RemoteExecutor_->Execute(planFragment, writer);

                        return WaitFor(subqueryResult)
                            .ValueOrThrow();
                    });

                asyncStatistics.Subscribe(BIND([pipe] (const TErrorOr<TQueryStatistics>& result) {
                    if (!result.IsOK()) {
                        pipe->Fail(result);
                    }
                }));

                return std::make_pair(pipe->GetReader(), asyncStatistics);
            },
            [&] (const TConstQueryPtr& topQuery, ISchemafulReaderPtr reader, ISchemafulWriterPtr writer) {
                auto asyncQueryStatisticsOrError = BIND(&TEvaluator::Run, Evaluator_)
                    .AsyncVia(Bootstrap_->GetBoundedConcurrencyQueryPoolInvoker())
                    .Run(topQuery, std::move(reader), std::move(writer));

                return WaitFor(asyncQueryStatisticsOrError)
                    .ValueOrThrow();
            });
    }

    TDataSplits Split(
        const TDataSplits& splits,
        TNodeDirectoryPtr nodeDirectory,
        const NLogging::TLogger& Logger)
    {
        std::map<TGuid, TDataSplits> splitsByTablet;

        TDataSplits allSplits;
        for (const auto& split : splits) {
            auto objectId = GetObjectIdFromDataSplit(split);
            auto type = TypeFromId(objectId);

            if (type == EObjectType::Tablet) {
                splitsByTablet[objectId].push_back(split);
            } else {
                allSplits.push_back(split);
                continue;
            }
        }

        for (auto& tabletIdSplit : splitsByTablet) {
            auto tabletId = tabletIdSplit.first;
            auto& splits = tabletIdSplit.second;

            YCHECK(!splits.empty());

            auto keyColumns = GetKeyColumnsFromDataSplit(splits.front());
            auto schema = GetTableSchemaFromDataSplit(splits.front());
            auto timestamp = GetTimestampFromDataSplit(splits.front());

            std::sort(splits.begin(), splits.end(), [] (const TDataSplit& lhs, const TDataSplit& rhs) {
                return GetLowerBoundFromDataSplit(lhs) < GetLowerBoundFromDataSplit(rhs);
            });

            auto tabletSlotManager = Bootstrap_->GetTabletSlotManager();
            auto tabletSnapshot = tabletSlotManager->GetTabletSnapshotOrThrow(tabletId);

            int lastIndex = 0;
            std::vector<std::pair<TOwningKey, TOwningKey>> resultRanges;
            for (int index = 1; index < splits.size(); ++index) {
                auto lowerBound = GetLowerBoundFromDataSplit(splits[index]);
                auto upperBound = GetUpperBoundFromDataSplit(splits[index - 1]);

                int totalSampleCount, partitionCount;
                std::tie(totalSampleCount, partitionCount) = GetBoundSampleKeys(tabletSnapshot, upperBound, lowerBound);

                if (totalSampleCount != 0 || partitionCount != 0) {
                    resultRanges.emplace_back(GetLowerBoundFromDataSplit(splits[lastIndex]), upperBound);
                    lastIndex = index;
                }
            }

            resultRanges.emplace_back(GetLowerBoundFromDataSplit(splits[lastIndex]), GetUpperBoundFromDataSplit(splits.back()));

            int totalSampleCount = 0;
            int totalPartitionCount = 0;
            for (const auto& range : resultRanges) {
                int sampleCount, partitionCount;
                std::tie(sampleCount, partitionCount) = GetBoundSampleKeys(tabletSnapshot, range.first, range.second);
                totalSampleCount += sampleCount;
                totalPartitionCount += partitionCount;
            }

            int freeSlotCount = std::max(0, Config_->MaxSubsplitsPerTablet - totalPartitionCount);
            int cappedSampleCount = std::min(freeSlotCount, totalSampleCount);

            int nextSampleIndex = 1;
            int currentSampleCount = 1;
            for (const auto& range : resultRanges) {
                auto splitKeys = BuildSplitKeys(
                    tabletSnapshot,
                    range.first,
                    range.second,
                    nextSampleIndex,
                    currentSampleCount,
                    totalSampleCount,
                    cappedSampleCount);

                for (int splitKeyIndex = 0; splitKeyIndex < splitKeys.size(); ++splitKeyIndex) {
                    const auto& thisKey = splitKeys[splitKeyIndex];
                    const auto& nextKey = (splitKeyIndex == splitKeys.size() - 1)
                        ? MaxKey()
                        : splitKeys[splitKeyIndex + 1];
                    TDataSplit subsplit;
                    SetObjectId(&subsplit, tabletId);
                    SetKeyColumns(&subsplit, keyColumns);
                    SetTableSchema(&subsplit, schema);
                    SetLowerBound(&subsplit, std::max(range.first, thisKey));
                    SetUpperBound(&subsplit, std::min(range.second, nextKey));
                    SetTimestamp(&subsplit, timestamp);
                    allSplits.push_back(std::move(subsplit));
                }
            }
        }

        return allSplits;
    }

    std::pair<int, int> GetBoundSampleKeys(
        TTabletSnapshotPtr tabletSnapshot,
        const TOwningKey& lowerBound,
        const TOwningKey& upperBound)
    {
        auto findStartSample = [&] (const std::vector<TOwningKey>& sampleKeys) {
            return std::upper_bound(
                sampleKeys.begin(),
                sampleKeys.end(),
                lowerBound);
        };
        auto findEndSample = [&] (const std::vector<TOwningKey>& sampleKeys) {
            return std::lower_bound(
                sampleKeys.begin(),
                sampleKeys.end(),
                upperBound);
        };

        // Run binary search to find the relevant partitions.
        const auto& partitions = tabletSnapshot->Partitions;
        YCHECK(lowerBound >= partitions[0]->PivotKey);
        auto startPartitionIt = std::upper_bound(
            partitions.begin(),
            partitions.end(),
            lowerBound,
            [] (const TOwningKey& lhs, const TPartitionSnapshotPtr& rhs) {
                return lhs < rhs->PivotKey;
            }) - 1;
        auto endPartitionIt = std::lower_bound(
            startPartitionIt,
            partitions.end(),
            upperBound,
            [] (const TPartitionSnapshotPtr& lhs, const TOwningKey& rhs) {
                return lhs->PivotKey < rhs;
            });
        int partitionCount = std::distance(startPartitionIt, endPartitionIt);

        int totalSampleCount = 0;
        for (auto partitionIt = startPartitionIt; partitionIt != endPartitionIt; ++partitionIt) {
            const auto& partition = *partitionIt;
            const auto& sampleKeys = partition->SampleKeys->Keys;
            auto startSampleIt = partitionIt == startPartitionIt && !sampleKeys.empty()
                ? findStartSample(sampleKeys)
                : sampleKeys.begin();
            auto endSampleIt = partitionIt + 1 == endPartitionIt
                ? findEndSample(sampleKeys)
                : sampleKeys.end();

            totalSampleCount += std::distance(startSampleIt, endSampleIt);
        }

        return std::make_pair(totalSampleCount, partitionCount);
    }

    std::vector<TOwningKey> BuildSplitKeys(
        TTabletSnapshotPtr tabletSnapshot,
        const TOwningKey& lowerBound,
        const TOwningKey& upperBound,
        int& nextSampleIndex,
        int& currentSampleCount,
        int totalSampleCount,
        int cappedSampleCount)
    {
        auto findStartSample = [&] (const std::vector<TOwningKey>& sampleKeys) {
            return std::upper_bound(
                sampleKeys.begin(),
                sampleKeys.end(),
                lowerBound);
        };
        auto findEndSample = [&] (const std::vector<TOwningKey>& sampleKeys) {
            return std::lower_bound(
                sampleKeys.begin(),
                sampleKeys.end(),
                upperBound);
        };

        // Run binary search to find the relevant partitions.
        const auto& partitions = tabletSnapshot->Partitions;
        YCHECK(lowerBound >= partitions[0]->PivotKey);
        auto startPartitionIt = std::upper_bound(
            partitions.begin(),
            partitions.end(),
            lowerBound,
            [] (const TOwningKey& lhs, const TPartitionSnapshotPtr& rhs) {
                return lhs < rhs->PivotKey;
            }) - 1;
        auto endPartitionIt = std::lower_bound(
            startPartitionIt,
            partitions.end(),
            upperBound,
            [] (const TPartitionSnapshotPtr& lhs, const TOwningKey& rhs) {
                return lhs->PivotKey < rhs;
            });
        int partitionCount = std::distance(startPartitionIt, endPartitionIt);

        int nextSampleCount = cappedSampleCount != 0
            ? nextSampleIndex * totalSampleCount / cappedSampleCount
            : 0;

        // Fill results with pivotKeys and up to cappedSampleCount sampleKeys.
        std::vector<TOwningKey> result;
        result.reserve(partitionCount + cappedSampleCount);
        for (auto partitionIt = startPartitionIt; partitionIt != endPartitionIt; ++partitionIt) {
            const auto& partition = *partitionIt;
            const auto& sampleKeys = partition->SampleKeys->Keys;
            auto startSampleIt = partitionIt == startPartitionIt && !sampleKeys.empty()
                ? findStartSample(sampleKeys)
                : sampleKeys.begin();
            auto endSampleIt = partitionIt == endPartitionIt - 1
                ? findEndSample(sampleKeys)
                : sampleKeys.end();

            result.push_back(partition->PivotKey);

            if (cappedSampleCount == 0) {
                continue;
            }

            for (auto sampleIt = startSampleIt; sampleIt < endSampleIt;) {
                if (currentSampleCount == nextSampleCount) {
                    ++nextSampleIndex;
                    nextSampleCount = nextSampleIndex * totalSampleCount / cappedSampleCount;
                    result.push_back(*sampleIt);
                }
                int samplesLeft = static_cast<int>(std::distance(sampleIt, endSampleIt));
                int step = std::min(samplesLeft, nextSampleCount - currentSampleCount);
                YCHECK(step > 0);
                sampleIt += step;
                currentSampleCount += step;
            }
        }
        return result;
    }

    ISchemafulReaderPtr GetReader(
        const TDataSplit& split,
        TNodeDirectoryPtr nodeDirectory)
    {
        auto objectId = FromProto<TObjectId>(split.chunk_id());
        switch (TypeFromId(objectId)) {
            case EObjectType::Chunk:
            case EObjectType::ErasureChunk:
                return GetChunkReader(split, std::move(nodeDirectory));

            case EObjectType::Tablet:
                return GetTabletReader(split, std::move(nodeDirectory));

            default:
                THROW_ERROR_EXCEPTION("Unsupported data split type %Qlv",
                    TypeFromId(objectId));
        }
    }

    ISchemafulReaderPtr GetChunkReader(
        const TDataSplit& split,
        TNodeDirectoryPtr nodeDirectory)
    {
        auto futureReader = BIND(&TQueryExecutor::GetChunkReaderControl, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetControlInvoker())
            .Run(split, std::move(nodeDirectory));
        return New<TLazySchemafulReader>(std::move(futureReader));
    }

    ISchemafulReaderPtr GetChunkReaderControl(
        const TDataSplit& split,
        TNodeDirectoryPtr nodeDirectory)
    {
        auto chunkId = FromProto<TChunkId>(split.chunk_id());
        auto lowerBound = FromProto<TReadLimit>(split.lower_limit());
        auto upperBound = FromProto<TReadLimit>(split.upper_limit());
        auto timestamp = GetTimestampFromDataSplit(split);

        auto chunkRegistry = Bootstrap_->GetChunkRegistry();
        auto chunk = chunkRegistry->FindChunk(chunkId);

        NChunkClient::IChunkReaderPtr chunkReader;
        if (chunk) {
            LOG_DEBUG("Creating local reader for chunk split (ChunkId: %v, LowerBound: {%v}, UpperBound: {%v}, Timestamp: %v)",
                chunkId,
                lowerBound,
                upperBound,
                timestamp);

            chunkReader = CreateLocalChunkReader(
                Bootstrap_,
                Bootstrap_->GetConfig()->TabletNode->ChunkReader,
                chunk);
        } else {
            LOG_DEBUG("Creating remote reader for chunk split (ChunkId: %v, LowerBound: {%v}, UpperBound: {%v}, Timestamp: %v)",
                chunkId,
                lowerBound,
                upperBound,
                timestamp);

            // TODO(babenko): seed replicas?
            // TODO(babenko): throttler?
            chunkReader = CreateReplicationReader(
                Bootstrap_->GetConfig()->TabletNode->ChunkReader,
                Bootstrap_->GetBlockStore()->GetCompressedBlockCache(),
                Bootstrap_->GetMasterClient()->GetMasterChannel(NApi::EMasterChannelKind::LeaderOrFollower),
                nodeDirectory,
                Bootstrap_->GetLocalDescriptor(),
                chunkId);
        }

        return CreateSchemafulChunkReader(
            Bootstrap_->GetConfig()->TabletNode->ChunkReader,
            std::move(chunkReader),
            Bootstrap_->GetUncompressedBlockCache(),
            split.chunk_meta(),
            lowerBound,
            upperBound,
            timestamp);
    }


    ISchemafulReaderPtr GetTabletReader(
        const TDataSplit& split,
        TNodeDirectoryPtr nodeDirectory)
    {
        try {
            auto tabletId = FromProto<TTabletId>(split.chunk_id());

            auto tabletSlotManager = Bootstrap_->GetTabletSlotManager();
            auto tabletSnapshot = tabletSlotManager->GetTabletSnapshotOrThrow(tabletId);

            auto lowerBound = GetLowerBoundFromDataSplit(split);
            auto upperBound = GetUpperBoundFromDataSplit(split);
            auto timestamp = GetTimestampFromDataSplit(split);

            return CreateSchemafulTabletReader(
                Bootstrap_->GetQueryPoolInvoker(),
                std::move(tabletSnapshot),
                std::move(lowerBound),
                std::move(upperBound),
                timestamp);
        } catch (const std::exception& ex) {
            auto futureReader = MakeFuture(TErrorOr<ISchemafulReaderPtr>(ex));
            return New<TLazySchemafulReader>(futureReader);
        }
    }

};

IExecutorPtr CreateQueryExecutor(
    TQueryAgentConfigPtr config,
    TBootstrap* bootstrap)
{
    return New<TQueryExecutor>(config, bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryAgent
} // namespace NYT

