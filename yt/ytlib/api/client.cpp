#include "stdafx.h"
#include "client.h"
#include "transaction.h"
#include "connection.h"
#include "file_reader.h"
#include "file_writer.h"
#include "journal_reader.h"
#include "journal_writer.h"
#include "rowset.h"
#include "config.h"
#include "box.h"
#include "private.h"

#include <core/profiling/scoped_timer.h>

#include <core/concurrency/scheduler.h>

#include <core/ytree/attribute_helpers.h>
#include <core/ytree/ypath_proxy.h>

#include <core/rpc/helpers.h>
#include <core/rpc/scoped_channel.h>

#include <core/compression/helpers.h>

#include <ytlib/transaction_client/transaction_manager.h>
#include <ytlib/transaction_client/timestamp_provider.h>

#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/object_client/master_ypath_proxy.h>
#include <ytlib/object_client/helpers.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>
#include <ytlib/cypress_client/rpc_helpers.h>

#include <ytlib/tablet_client/wire_protocol.h>
#include <ytlib/tablet_client/table_mount_cache.h>
#include <ytlib/tablet_client/tablet_service_proxy.h>
#include <ytlib/tablet_client/wire_protocol.pb.h>

#include <ytlib/security_client/group_ypath_proxy.h>

#include <ytlib/driver/dispatcher.h>

#include <ytlib/hive/config.h>
#include <ytlib/hive/cell_directory.h>

#include <ytlib/new_table_client/schemaful_writer.h>
#include <ytlib/new_table_client/name_table.h>

#include <ytlib/query_client/plan_fragment.h>
#include <ytlib/query_client/plan_helpers.h>
#include <ytlib/query_client/coordinator.h>
#include <ytlib/query_client/helpers.h>
#include <ytlib/query_client/query_statistics.h>
#include <ytlib/query_client/query_service_proxy.h>
#include <ytlib/query_client/query_statistics.h>
#include <ytlib/query_client/evaluator.h>
#include <ytlib/query_client/column_evaluator.h>
#include <ytlib/query_client/private.h> // XXX(sandello): refactor BuildLogger

#include <ytlib/chunk_client/chunk_replica.h>
#include <ytlib/chunk_client/read_limit.h>

#include <ytlib/scheduler/scheduler_service_proxy.h>
#include <ytlib/scheduler/job_prober_service_proxy.h>

// TODO(babenko): refactor this
#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/new_table_client/chunk_meta_extensions.h>
#include <ytlib/new_table_client/schemaful_reader.h>
#include <ytlib/new_table_client/table_ypath_proxy.h>
#include <ytlib/new_table_client/row_merger.h>
#include <ytlib/new_table_client/row_base.h>

namespace NYT {
namespace NApi {

using namespace NConcurrency;
using namespace NYPath;
using namespace NYTree;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NRpc;
using namespace NVersionedTableClient;
using namespace NVersionedTableClient::NProto;
using namespace NTabletClient;
using namespace NTabletClient::NProto;
using namespace NSecurityClient;
using namespace NQueryClient;
using namespace NChunkClient;
using namespace NScheduler;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TQueryHelper)
DECLARE_REFCOUNTED_CLASS(TClient)
DECLARE_REFCOUNTED_CLASS(TTransaction)

////////////////////////////////////////////////////////////////////////////////

namespace {

TNameTableToSchemaIdMapping BuildColumnIdMapping(
    const TTableMountInfoPtr& tableInfo,
    const TNameTablePtr& nameTable)
{
    for (const auto& name : tableInfo->KeyColumns) {
        if (!nameTable->FindId(name) && !tableInfo->Schema.GetColumnOrThrow(name).Expression) {
            THROW_ERROR_EXCEPTION("Missing key column %Qv in name table",
                name);
        }
    }

    TNameTableToSchemaIdMapping mapping;
    mapping.resize(nameTable->GetSize());
    for (int nameTableId = 0; nameTableId < nameTable->GetSize(); ++nameTableId) {
        const auto& name = nameTable->GetName(nameTableId);
        int schemaId = tableInfo->Schema.GetColumnIndexOrThrow(name);
        mapping[nameTableId] = schemaId;
    }
    return mapping;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

TError TCheckPermissionResult::ToError(const Stroka& user, NYTree::EPermission permission) const
{
    switch (Action) {
        case NSecurityClient::ESecurityAction::Allow:
            return TError();

        case NSecurityClient::ESecurityAction::Deny: {
            TError error;
            if (ObjectName && SubjectName) {
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied: %Qlv permission is denied for %Qv by ACE at %v",
                    permission,
                    *SubjectName,
                    *ObjectName);
            } else {
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied: %Qlv permission is not allowed by any matching ACE",
                    permission);
            }
            error.Attributes().Set("user", user);
            error.Attributes().Set("permission", permission);
            if (ObjectId != NullObjectId) {
                error.Attributes().Set("denied_by", ObjectId);
            }
            if (SubjectId != NullObjectId) {
                error.Attributes().Set("denied_for", SubjectId);
            }
            return error;
        }

        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

class TQueryResponseReader
    : public ISchemafulReader
{
public:
    explicit TQueryResponseReader(TFuture<TQueryServiceProxy::TRspExecutePtr> asyncResponse)
        : AsyncResponse_(std::move(asyncResponse))
    {
        QueryResult_.OnCanceled(BIND([this, this_ = MakeStrong(this)] () {
            AsyncResponse_.Cancel();
            {
                TGuard<TSpinLock> guard(SpinLock_);
                QueryResult_.Reset();
            }
        }));
    }

    virtual TFuture<void> Open(const TTableSchema& schema) override
    {
        return AsyncResponse_.Apply(BIND(
            &TQueryResponseReader::OnResponse,
            MakeStrong(this),
            schema));
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        return RowsetReader_->Read(rows);
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return RowsetReader_->GetReadyEvent();
    }

    TFuture<TQueryStatistics> GetQueryResult() const
    {
        return QueryResult_.ToFuture();
    }

private:
    TFuture<TQueryServiceProxy::TRspExecutePtr> AsyncResponse_;

    std::unique_ptr<TWireProtocolReader> ProtocolReader_;
    ISchemafulReaderPtr RowsetReader_;

    TSpinLock SpinLock_;
    TPromise<TQueryStatistics> QueryResult_ = NewPromise<TQueryStatistics>();


    void OnResponse(
        const TTableSchema& schema,
        const TQueryServiceProxy::TErrorOrRspExecutePtr& responseOrError)
    {
        if (!responseOrError.IsOK()) {
            QueryResult_.Set(responseOrError);
            THROW_ERROR responseOrError;
        }
        const auto& response = responseOrError.Value();

        {
            TGuard<TSpinLock> guard(SpinLock_);
            QueryResult_.Set(FromProto(response->query_statistics()));
        }

        YCHECK(!ProtocolReader_);
        auto data  = NCompression::DecompressWithEnvelope(response->Attachments());
        ProtocolReader_ = std::make_unique<TWireProtocolReader>(data);

        YCHECK(!RowsetReader_);
        RowsetReader_ = ProtocolReader_->CreateSchemafulRowsetReader();

        auto openResult = RowsetReader_->Open(schema);
        YCHECK(openResult.IsSet()); // this reader is sync
        openResult
            .Get()
            .ThrowOnError();
    }
};

DEFINE_REFCOUNTED_TYPE(TQueryResponseReader)

////////////////////////////////////////////////////////////////////////////////

class TQueryHelper
    : public IExecutor
    , public IPrepareCallbacks
{
public:
    TQueryHelper(
        IConnectionPtr connection,
        IChannelPtr masterChannel,
        IChannelFactoryPtr nodeChannelFactory,
        IFunctionRegistryPtr functionRegistry)
        : Connection_(std::move(connection))
        , MasterChannel_(std::move(masterChannel))
        , NodeChannelFactory_(std::move(nodeChannelFactory))
        , FunctionRegistry_(std::move(functionRegistry))
    { }

    // IPrepareCallbacks implementation.

    virtual TFuture<TDataSplit> GetInitialSplit(
        const TYPath& path,
        TTimestamp timestamp) override
    {
        return BIND(&TQueryHelper::DoGetInitialSplit, MakeStrong(this))
            .AsyncVia(NDriver::TDispatcher::Get()->GetLightInvoker())
            .Run(path, timestamp);
    }

    // IExecutor implementation.

    virtual TFuture<TQueryStatistics> Execute(
        TPlanFragmentPtr fragment,
        ISchemafulWriterPtr writer) override
    {
        TRACE_CHILD("QueryClient", "Execute") {

            auto execute = fragment->Ordered
                ? &TQueryHelper::DoExecuteOrdered
                : &TQueryHelper::DoExecute;

            return BIND(execute, MakeStrong(this))
                .AsyncVia(NDriver::TDispatcher::Get()->GetHeavyInvoker())
                .Run(fragment, std::move(writer));
        }
    }

private:
    const IConnectionPtr Connection_;
    const IChannelPtr MasterChannel_;
    const IChannelFactoryPtr NodeChannelFactory_;
    const IFunctionRegistryPtr FunctionRegistry_;


    TDataSplit DoGetInitialSplit(
        const TYPath& path,
        TTimestamp timestamp)
    {
        auto tableMountCache = Connection_->GetTableMountCache();
        auto info = WaitFor(tableMountCache->GetTableInfo(path))
            .ValueOrThrow();

        TDataSplit result;
        SetObjectId(&result, info->TableId);
        SetTableSchema(&result, info->Schema);
        SetKeyColumns(&result, info->KeyColumns);
        SetTimestamp(&result, timestamp);
        return result;
    }


    std::vector<std::pair<TDataSource, Stroka>> Split(
        TGuid objectId,
        const std::vector<TRowRange>& ranges,
        TRowBufferPtr rowBuffer,
        const NLogging::TLogger& Logger,
        bool verboseLogging)
    {
        std::vector<std::pair<TDataSource, Stroka>> result;

        if (TypeFromId(objectId) == EObjectType::Table) {
            result = SplitTableFurther(objectId, ranges, std::move(rowBuffer));
            LOG_DEBUG_IF(verboseLogging, "Got %v sources for input %v",
                result.size(),
                objectId);
        }

        return result;
    }

    std::vector<std::pair<TDataSource, Stroka>> SplitTableFurther(
        TGuid tableId,
        const std::vector<TRowRange>& ranges,
        TRowBufferPtr rowBuffer)
    {
        auto tableMountCache = Connection_->GetTableMountCache();
        auto tableInfo = WaitFor(tableMountCache->GetTableInfo(FromObjectId(tableId)))
            .ValueOrThrow();
        return tableInfo->Sorted
            ? SplitSortedTableFurther(tableId, ranges, std::move(rowBuffer))
            : SplitUnsortedTableFurther(tableId, ranges, std::move(rowBuffer), std::move(tableInfo));
    }

    std::vector<std::pair<TDataSource, Stroka>> SplitSortedTableFurther(
        TGuid tableId,
        const std::vector<TRowRange>& ranges,
        TRowBufferPtr rowBuffer)
    {
        // TODO(babenko): refactor and optimize
        TObjectServiceProxy proxy(MasterChannel_);

        auto req = TTableYPathProxy::Fetch(FromObjectId(tableId));
        ToProto(req->mutable_ranges(), std::vector<TReadRange>({TReadRange()}));
        req->set_fetch_all_meta_extensions(true);

        auto rsp = WaitFor(proxy.Execute(req))
            .ValueOrThrow();

        auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
        nodeDirectory->MergeFrom(rsp->node_directory());

        auto chunkSpecs = FromProto<NChunkClient::NProto::TChunkSpec>(rsp->chunks());

        std::vector<std::pair<TDataSource, Stroka>> result;

        const auto& networkName = Connection_->GetConfig()->NetworkName;

        for (auto& chunkSpec : chunkSpecs) {
            auto chunkKeyColumns = FindProtoExtension<TKeyColumnsExt>(chunkSpec.chunk_meta().extensions());
            auto chunkSchema = FindProtoExtension<TTableSchemaExt>(chunkSpec.chunk_meta().extensions());

            // TODO(sandello): One day we should validate consistency.
            // Now we just check we do _not_ have any of these.
            YCHECK(!chunkKeyColumns);
            YCHECK(!chunkSchema);

            TOwningKey chunkLowerBound, chunkUpperBound;
            if (TryGetBoundaryKeys(chunkSpec.chunk_meta(), &chunkLowerBound, &chunkUpperBound)) {
                chunkUpperBound = GetKeySuccessor(chunkUpperBound.Get());
                SetLowerBound(&chunkSpec, chunkLowerBound);
                SetUpperBound(&chunkSpec, chunkUpperBound);
            }

            auto replicas = FromProto<TChunkReplica, TChunkReplicaList>(chunkSpec.replicas());
            if (replicas.empty()) {
                auto objectId = GetObjectIdFromDataSplit(chunkSpec);
                THROW_ERROR_EXCEPTION("No alive replicas for chunk %v",
                    objectId);
            }
            auto replica = replicas[RandomNumber(replicas.size())];

            auto keyRange = GetBothBoundsFromDataSplit(chunkSpec);

            auto dataSource = TDataSource{
                GetObjectIdFromDataSplit(chunkSpec),
                TRowRange(rowBuffer->Capture(keyRange.first.Get()), rowBuffer->Capture(keyRange.second.Get()))};

            const auto& descriptor = nodeDirectory->GetDescriptor(replica);
            const auto& address = descriptor.GetAddressOrThrow(networkName);
            result.emplace_back(dataSource, address);
        }

        return result;
    }

    std::vector<std::pair<TDataSource, Stroka>> SplitUnsortedTableFurther(
        TGuid tableId,
        const std::vector<TRowRange>& ranges,
        TRowBufferPtr rowBuffer,
        TTableMountInfoPtr tableInfo)
    {
        if (tableInfo->Tablets.empty()) {
            THROW_ERROR_EXCEPTION("Table %v is neither sorted nor has tablets",
                tableId);
        }

        auto cellDirectory = Connection_->GetCellDirectory();

        std::vector<std::pair<TDataSource, Stroka>> subsources;
        for (const auto& range : ranges) {
            auto lowerBound = range.first;
            auto upperBound = range.second;

            // Run binary search to find the relevant tablets.
            auto startIt = std::upper_bound(
                tableInfo->Tablets.begin(),
                tableInfo->Tablets.end(),
                lowerBound,
                [] (const TRow& key, const TTabletInfoPtr& tabletInfo) {
                    return key < tabletInfo->PivotKey.Get();
                }) - 1;

        
            for (auto it = startIt; it != tableInfo->Tablets.end(); ++it) {
                const auto& tabletInfo = *it;
                if (upperBound <= tabletInfo->PivotKey)
                    break;

                if (tabletInfo->State != ETabletState::Mounted) {
                    // TODO(babenko): learn to work with unmounted tablets
                    THROW_ERROR_EXCEPTION("Tablet %v is not mounted",
                        tabletInfo->TabletId);
                }

                TDataSource subsource;
                subsource.Id = tabletInfo->TabletId;

                auto pivotKey = tabletInfo->PivotKey;
                auto nextPivotKey = (it + 1 == tableInfo->Tablets.end()) ? MaxKey() : (*(it + 1))->PivotKey;

                subsource.Range.first = rowBuffer->Capture(std::max(lowerBound, pivotKey.Get()));
                subsource.Range.second = rowBuffer->Capture(std::min(upperBound, nextPivotKey.Get()));

                auto addresses = cellDirectory->GetAddressesOrThrow(tabletInfo->CellId);
                if (addresses.empty()) {
                    THROW_ERROR_EXCEPTION("No alive replicas for tablet %v",
                        tabletInfo->TabletId);
                }

                const auto& address = addresses[RandomNumber(addresses.size())];
                subsources.emplace_back(std::move(subsource), address);
            }
        }

        return subsources;
    }


    TQueryStatistics DoCoordinateAndExecute(
        TPlanFragmentPtr fragment,
        ISchemafulWriterPtr writer,
        int subrangesCount,
        bool isOrdered,
        std::function<std::pair<TDataSources, Stroka>(int)> getSubsources)
    {
        auto Logger = BuildLogger(fragment->Query);

        std::vector<TRefiner> refiners(subrangesCount, [] (
            TConstExpressionPtr expr,
            const TTableSchema& schema,
            const TKeyColumns& keyColumns) {
                return expr;
            });

        return CoordinateAndExecute(
            fragment,
            writer,
            refiners,
            isOrdered,
            [&] (TConstQueryPtr subquery, int index) {
                auto subfragment = New<TPlanFragment>(fragment->Source);
                subfragment->Timestamp = fragment->Timestamp;
                subfragment->ForeignDataId = fragment->ForeignDataId;
                subfragment->Query = subquery;
                subfragment->RangeExpansionLimit = fragment->RangeExpansionLimit,
                subfragment->VerboseLogging = fragment->VerboseLogging;
                subfragment->Ordered = fragment->Ordered;

                Stroka address;
                std::tie(subfragment->DataSources, address) = getSubsources(index);
                
                LOG_DEBUG("Delegating subquery (SubqueryId: %v, Address: %v)",
                    subquery->Id,
                    address);

                return Delegate(subfragment, address);
            },
            [&] (TConstQueryPtr topQuery, ISchemafulReaderPtr reader, ISchemafulWriterPtr writer) {
                LOG_DEBUG("Evaluating top query (TopQueryId: %v)", topQuery->Id);
                auto evaluator = Connection_->GetQueryEvaluator();
                return evaluator->Run(topQuery, std::move(reader), std::move(writer), FunctionRegistry_);
            });
    }

    TQueryStatistics DoExecute(
        TPlanFragmentPtr fragment,
        ISchemafulWriterPtr writer)
    {
        auto Logger = BuildLogger(fragment->Query);

        const auto& dataSources = fragment->DataSources;

        auto rowBuffer = New<TRowBuffer>();
        auto prunedRanges = GetPrunedRanges(
            fragment->Query,
            dataSources,
            rowBuffer,
            Connection_->GetColumnEvaluatorCache(),
            FunctionRegistry_,
            fragment->RangeExpansionLimit,
            fragment->VerboseLogging);

        LOG_DEBUG("Splitting pruned splits");

        std::vector<std::pair<TDataSource, Stroka>> allSplits;
        for (int index = 0; index < dataSources.size(); ++index) {
            auto id = dataSources[index].Id;
            const auto& ranges = prunedRanges[index];
            auto splits = Split(id, ranges, rowBuffer, Logger, fragment->VerboseLogging);
            allSplits.insert(allSplits.begin(), splits.begin(), splits.end());
        }

        yhash_map<Stroka, TDataSources> groupsByAddress;
        for (const auto& split : allSplits) {
            const auto& address = split.second;
            groupsByAddress[address].push_back(split.first);
        }

        std::vector<std::pair<TDataSources, Stroka>> groupedSplits;
        for (const auto& group : groupsByAddress) {
            if (group.second.empty()) {
                continue;
            }
            groupedSplits.emplace_back(group.second, group.first);
        }

        LOG_DEBUG("Regrouped %v splits into %v groups",
            allSplits.size(),
            groupsByAddress.size());

        return DoCoordinateAndExecute(fragment, writer, groupedSplits.size(), false, [&] (int index) {
            return groupedSplits[index];
        });
    }

    TQueryStatistics DoExecuteOrdered(
        TPlanFragmentPtr fragment,
        ISchemafulWriterPtr writer)
    {
        auto Logger = BuildLogger(fragment->Query);

        const auto& dataSources = fragment->DataSources;

        auto rowBuffer = New<TRowBuffer>();
        auto prunedRanges = GetPrunedRanges(
            fragment->Query,
            dataSources,
            rowBuffer,
            Connection_->GetColumnEvaluatorCache(),
            FunctionRegistry_,
            fragment->RangeExpansionLimit,
            fragment->VerboseLogging);

        LOG_DEBUG("Splitting pruned splits");

        std::vector<std::pair<TDataSource, Stroka>> allSplits;

        for (int index = 0; index < dataSources.size(); ++index) {
            const auto& id = dataSources[index].Id;
            const auto& ranges = prunedRanges[index];

            auto splits = Split(id, ranges, rowBuffer, Logger, fragment->VerboseLogging);
            std::move(splits.begin(), splits.end(), std::back_inserter(allSplits));
        }

        LOG_DEBUG("Sorting %v splits", allSplits.size());

        std::sort(
            allSplits.begin(),
            allSplits.end(),
            [] (const std::pair<TDataSource, Stroka>& lhs, const std::pair<TDataSource, Stroka>& rhs) {
                return lhs.first.Range.first < rhs.first.Range.first;
            });

        return DoCoordinateAndExecute(fragment, writer, allSplits.size(), true, [&] (int index) {
            const auto& split = allSplits[index];
            const auto& address = split.second;
            LOG_DEBUG("Delegating to tablet %v at %v",
                split.first.Id,
                address);
            return std::make_pair(TDataSources(1, split.first), address);
        });
    }

   std::pair<ISchemafulReaderPtr, TFuture<TQueryStatistics>> Delegate(
        TPlanFragmentPtr fragment,
        const Stroka& address)
    {
        auto Logger = BuildLogger(fragment->Query);

        TRACE_CHILD("QueryClient", "Delegate") {
            auto channel = NodeChannelFactory_->CreateChannel(address);
            auto config = Connection_->GetConfig();

            TQueryServiceProxy proxy(channel);
            proxy.SetDefaultTimeout(config->QueryTimeout);

            auto req = proxy.Execute();

            TDuration serializationTime;
            {
                NProfiling::TAggregatingTimingGuard timingGuard(&serializationTime);
                ToProto(req->mutable_plan_fragment(), fragment);
                req->set_response_codec(static_cast<int>(config->QueryResponseCodec));
            }

            TRACE_ANNOTATION("serialization_time", serializationTime);
            TRACE_ANNOTATION("request_size", req->ByteSize());

            auto resultReader = New<TQueryResponseReader>(req->Invoke());
            return std::make_pair(resultReader, resultReader->GetQueryResult());
        }
    }

};

DEFINE_REFCOUNTED_TYPE(TQueryHelper)

////////////////////////////////////////////////////////////////////////////////

class TClient
    : public IClient
{
public:
    TClient(
        IConnectionPtr connection,
        const TClientOptions& options)
        : Connection_(std::move(connection))
        , Options_(options)
        , Invoker_(NDriver::TDispatcher::Get()->GetLightInvoker())
        , FunctionRegistry_(Connection_->GetFunctionRegistry())
    {
        for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
            MasterChannels_[kind] = Connection_->GetMasterChannel(kind);
        }
        SchedulerChannel_ = Connection_->GetSchedulerChannel();
        NodeChannelFactory_ = Connection_->GetNodeChannelFactory();

        for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
            MasterChannels_[kind] = CreateAuthenticatedChannel(MasterChannels_[kind], options.User);
        }
        SchedulerChannel_ = CreateAuthenticatedChannel(SchedulerChannel_, options.User);
        NodeChannelFactory_ = CreateAuthenticatedChannelFactory(NodeChannelFactory_, options.User);

        for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
            MasterChannels_[kind] = CreateScopedChannel(MasterChannels_[kind]);
        }
        SchedulerChannel_ = CreateScopedChannel(SchedulerChannel_);

        for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
            ObjectProxies_[kind].reset(new TObjectServiceProxy(MasterChannels_[kind]));
        }
        SchedulerProxy_.reset(new TSchedulerServiceProxy(SchedulerChannel_));
        JobProberProxy_.reset(new TJobProberServiceProxy(SchedulerChannel_));

        TransactionManager_ = New<TTransactionManager>(
            Connection_->GetConfig()->TransactionManager,
            Connection_->GetConfig()->Master->CellTag,
            Connection_->GetConfig()->Master->CellId,
            GetMasterChannel(EMasterChannelKind::Leader),
            Connection_->GetTimestampProvider(),
            Connection_->GetCellDirectory());

        QueryHelper_ = New<TQueryHelper>(
            Connection_,
            GetMasterChannel(EMasterChannelKind::LeaderOrFollower),
            NodeChannelFactory_,
            FunctionRegistry_);

        Logger.AddTag("Client: %p", this);
    }


    virtual IConnectionPtr GetConnection() override
    {
        return Connection_;
    }

    virtual IChannelPtr GetMasterChannel(EMasterChannelKind kind) override
    {
        return MasterChannels_[kind];
    }

    virtual IChannelPtr GetSchedulerChannel() override
    {
        return SchedulerChannel_;
    }

    virtual IChannelFactoryPtr GetNodeChannelFactory() override
    {
        return NodeChannelFactory_;
    }

    virtual TTransactionManagerPtr GetTransactionManager() override
    {
        return TransactionManager_;
    }

    virtual NQueryClient::IExecutorPtr GetQueryExecutor() override
    {
        return QueryHelper_;
    }

    virtual TFuture<void> Terminate() override
    {
        TransactionManager_->AbortAll();

        auto error = TError("Client terminated");
        std::vector<TFuture<void>> asyncResults;
        for (auto kind : TEnumTraits<EMasterChannelKind>::GetDomainValues()) {
            asyncResults.push_back(MasterChannels_[kind]->Terminate(error));
        }
        asyncResults.push_back(SchedulerChannel_->Terminate(error));
        return Combine(asyncResults);
    }


    virtual TFuture<ITransactionPtr> StartTransaction(
        ETransactionType type,
        const TTransactionStartOptions& options) override;

#define DROP_BRACES(...) __VA_ARGS__
#define IMPLEMENT_METHOD(returnType, method, signature, args) \
    virtual TFuture<returnType> method signature override \
    { \
        return Execute( \
            #method, \
            options, \
            BIND( \
                &TClient::Do ## method, \
                MakeStrong(this), \
                DROP_BRACES args)); \
    }

    IMPLEMENT_METHOD(IRowsetPtr, LookupRows, (
        const TYPath& path,
        TNameTablePtr nameTable,
        const std::vector<NVersionedTableClient::TKey>& keys,
        const TLookupRowsOptions& options),
        (path, nameTable, keys, options))

    virtual TFuture<IRowsetPtr> LookupRow(
        const TYPath& path,
        TNameTablePtr nameTable,
        NVersionedTableClient::TKey key,
        const TLookupRowsOptions& options) override
    {
        return LookupRows(
            path,
            std::move(nameTable),
            std::vector<NVersionedTableClient::TKey>(1, key),
            options);
    }

    IMPLEMENT_METHOD(TQueryStatistics, SelectRows, (
        const Stroka& query,
        ISchemafulWriterPtr writer,
        const TSelectRowsOptions& options),
        (query, writer, options))

    virtual TFuture<std::pair<IRowsetPtr, TQueryStatistics>> SelectRows(
        const Stroka& query,
        const TSelectRowsOptions& options) override
    {
        auto result = NewPromise<std::pair<IRowsetPtr, TQueryStatistics>>();

        ISchemafulWriterPtr writer;
        TFuture<IRowsetPtr> rowset;
        std::tie(writer, rowset) = CreateSchemafulRowsetWriter();

        SelectRows(query, writer, options).Subscribe(BIND([=] (const TErrorOr<TQueryStatistics>& error) mutable {
            if (!error.IsOK()) {
                // It's uncommon to have the promise set here but let's be sloppy about it.
                result.Set(TError(error));
            } else {
                result.Set(std::make_pair(rowset.Get().Value(), error.Value()));
            }
        }));

        return result;
    }


    IMPLEMENT_METHOD(void, MountTable, (
        const TYPath& path,
        const TMountTableOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, UnmountTable, (
        const TYPath& path,
        const TUnmountTableOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, RemountTable, (
        const TYPath& path,
        const TRemountTableOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, ReshardTable, (
        const TYPath& path,
        const std::vector<NVersionedTableClient::TKey>& pivotKeys,
        const TReshardTableOptions& options),
        (path, pivotKeys, options))


    IMPLEMENT_METHOD(TYsonString, GetNode, (
        const TYPath& path,
        const TGetNodeOptions& options),
        (path, options))
    IMPLEMENT_METHOD(void, SetNode, (
        const TYPath& path,
        const TYsonString& value,
        const TSetNodeOptions& options),
        (path, value, options))
    IMPLEMENT_METHOD(void, RemoveNode, (
        const TYPath& path,
        const TRemoveNodeOptions& options),
        (path, options))
    IMPLEMENT_METHOD(TYsonString, ListNode, (
        const TYPath& path,
        const TListNodeOptions& options),
        (path, options))
    IMPLEMENT_METHOD(TNodeId, CreateNode, (
        const TYPath& path,
        EObjectType type,
        const TCreateNodeOptions& options),
        (path, type, options))
    IMPLEMENT_METHOD(TLockId, LockNode, (
        const TYPath& path,
        NCypressClient::ELockMode mode,
        const TLockNodeOptions& options),
        (path, mode, options))
    IMPLEMENT_METHOD(TNodeId, CopyNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TCopyNodeOptions& options),
        (srcPath, dstPath, options))
    IMPLEMENT_METHOD(TNodeId, MoveNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TMoveNodeOptions& options),
        (srcPath, dstPath, options))
    IMPLEMENT_METHOD(TNodeId, LinkNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TLinkNodeOptions& options),
        (srcPath, dstPath, options))
    IMPLEMENT_METHOD(bool, NodeExists, (
        const TYPath& path,
        const TNodeExistsOptions& options),
        (path, options))


    IMPLEMENT_METHOD(TObjectId, CreateObject, (
        EObjectType type,
        const TCreateObjectOptions& options),
        (type, options))


    virtual IFileReaderPtr CreateFileReader(
        const TYPath& path,
        const TFileReaderOptions& options) override
    {
        return NApi::CreateFileReader(this, path, options);
    }

    virtual IFileWriterPtr CreateFileWriter(
        const TYPath& path,
        const TFileWriterOptions& options) override
    {
        return NApi::CreateFileWriter(this, path, options);
    }


    virtual IJournalReaderPtr CreateJournalReader(
        const TYPath& path,
        const TJournalReaderOptions& options) override
    {
        return NApi::CreateJournalReader(this, path, options);
    }

    virtual IJournalWriterPtr CreateJournalWriter(
        const TYPath& path,
        const TJournalWriterOptions& options) override
    {
        return NApi::CreateJournalWriter(this, path, options);
    }


    IMPLEMENT_METHOD(void, AddMember, (
        const Stroka& group,
        const Stroka& member,
        const TAddMemberOptions& options),
        (group, member, options))
    IMPLEMENT_METHOD(void, RemoveMember, (
        const Stroka& group,
        const Stroka& member,
        const TRemoveMemberOptions& options),
        (group, member, options))
    IMPLEMENT_METHOD(TCheckPermissionResult, CheckPermission, (
        const Stroka& user,
        const TYPath& path,
        EPermission permission,
        const TCheckPermissionOptions& options),
        (user, path, permission, options))

    IMPLEMENT_METHOD(TOperationId, StartOperation, (
        EOperationType type,
        const TYsonString& spec,
        const TStartOperationOptions& options),
        (type, spec, options))
    IMPLEMENT_METHOD(void, AbortOperation, (
        const TOperationId& operationId,
        const TAbortOperationOptions& options),
        (operationId, options))
    IMPLEMENT_METHOD(void, SuspendOperation, (
        const TOperationId& operationId,
        const TSuspendOperationOptions& options),
        (operationId, options))
    IMPLEMENT_METHOD(void, ResumeOperation, (
        const TOperationId& operationId,
        const TResumeOperationOptions& options),
        (operationId, options))

    IMPLEMENT_METHOD(void, DumpJobContext, (
        const TJobId& jobId,
        const TYPath& path,
        const TDumpJobContextOptions& options),
        (jobId, path, options))
    IMPLEMENT_METHOD(TYsonString, StraceJob, (
        const TJobId& jobId,
        const TStraceJobOptions& options),
        (jobId, options))

#undef DROP_BRACES
#undef IMPLEMENT_METHOD

    IChannelPtr GetTabletChannel(const TTabletCellId& cellId)
    {
        const auto& cellDirectory = Connection_->GetCellDirectory();
        auto channel = cellDirectory->GetChannelOrThrow(cellId);
        return CreateAuthenticatedChannel(std::move(channel), Options_.User);
    }

private:
    friend class TTransaction;

    const IConnectionPtr Connection_;
    const TClientOptions Options_;

    const IInvokerPtr Invoker_;

    const IFunctionRegistryPtr FunctionRegistry_;

    TEnumIndexedVector<IChannelPtr, EMasterChannelKind> MasterChannels_;
    IChannelPtr SchedulerChannel_;
    IChannelFactoryPtr NodeChannelFactory_;
    TTransactionManagerPtr TransactionManager_;
    TQueryHelperPtr QueryHelper_;
    TEnumIndexedVector<std::unique_ptr<TObjectServiceProxy>, EMasterChannelKind> ObjectProxies_;
    std::unique_ptr<TSchedulerServiceProxy> SchedulerProxy_;
    std::unique_ptr<TJobProberServiceProxy> JobProberProxy_;

    NLogging::TLogger Logger = ApiLogger;


    template <class T>
    TFuture<T> Execute(
        const Stroka& commandName,
        const TTimeoutOptions& options,
        TCallback<T()> callback)
    {
        return
            BIND([=, this_ = MakeStrong(this)] () {
                try {
                    LOG_DEBUG("Command started (Command: %v)", commandName);
                    TBox<T> result(callback);
                    LOG_DEBUG("Command completed (Command: %v)", commandName);
                    return result.Unwrap();
                } catch (const std::exception& ex) {
                    LOG_DEBUG(ex, "Command failed (Command: %v)", commandName);
                    throw;
                }
            })
            .AsyncVia(Invoker_)
            .Run()
            .WithTimeout(options.Timeout);
    }


    TTableMountInfoPtr SyncGetTableInfo(const TYPath& path)
    {
        const auto& tableMountCache = Connection_->GetTableMountCache();
        return WaitFor(tableMountCache->GetTableInfo(path))
            .ValueOrThrow();
    }

    static TTabletInfoPtr SyncGetTabletInfo(
        TTableMountInfoPtr tableInfo,
        NVersionedTableClient::TKey key)
    {
        auto tabletInfo = tableInfo->GetTablet(key);
        if (tabletInfo->State != ETabletState::Mounted) {
            THROW_ERROR_EXCEPTION("Tablet %v of table %v is in %Qlv state",
                tabletInfo->TabletId,
                tableInfo->Path,
                tabletInfo->State);
        }
        return tabletInfo;
    }


    static void GenerateMutationId(IClientRequestPtr request, TMutatingOptions& options)
    {
        if (options.MutationId == NullMutationId) {
            options.MutationId = NRpc::GenerateMutationId();
        }
        SetMutationId(request, options.MutationId, options.Retry);
        ++options.MutationId.Parts32[1];
    }


    TTransactionId GetTransactionId(const TTransactionalOptions& options, bool allowNullTransaction)
    {
        auto transaction = GetTransaction(options, allowNullTransaction, true);
        return transaction ? transaction->GetId() : NullTransactionId;
    }

    NTransactionClient::TTransactionPtr GetTransaction(
        const TTransactionalOptions& options,
        bool allowNullTransaction,
        bool pingTransaction)
    {
        if (options.TransactionId == NullTransactionId) {
            if (!allowNullTransaction) {
                THROW_ERROR_EXCEPTION("A valid master transaction is required");
            }
            return nullptr;
        }

        if (TypeFromId(options.TransactionId) != EObjectType::Transaction) {
            THROW_ERROR_EXCEPTION("A valid master transaction is required");
        }

        TTransactionAttachOptions attachOptions;
        attachOptions.Ping = pingTransaction;
        attachOptions.PingAncestors = options.PingAncestors;
        return TransactionManager_->Attach(options.TransactionId, attachOptions);
    }

    void SetTransactionId(
        IClientRequestPtr request,
        const TTransactionalOptions& options,
        bool allowNullTransaction)
    {
        NCypressClient::SetTransactionId(request, GetTransactionId(options, allowNullTransaction));
    }


    void SetPrerequisites(
        IClientRequestPtr request,
        const TPrerequisiteOptions& options)
    {
        if (options.PrerequisiteTransactionIds.empty())
            return;

        auto* prerequisitesExt = request->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
        for (const auto& id : options.PrerequisiteTransactionIds) {
            auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
            ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
        }
    }


    static void SetSuppressAccessTracking(
        IClientRequestPtr request,
        const TSuppressableAccessTrackingOptions& commandOptions)
    {
        if (commandOptions.SuppressAccessTracking) {
            NCypressClient::SetSuppressAccessTracking(request, true);
        }
        if (commandOptions.SuppressModificationTracking) {
            NCypressClient::SetSuppressModificationTracking(request, true);
        }
    }


    class TTabletLookupSession
        : public TIntrinsicRefCounted
    {
    public:
        TTabletLookupSession(
            TClient* owner,
            TTabletInfoPtr tabletInfo,
            const TLookupRowsOptions& options,
            const TNameTableToSchemaIdMapping& idMapping)
            : Config_(owner->Connection_->GetConfig())
            , TabletId_(tabletInfo->TabletId)
            , Options_(options)
            , IdMapping_(idMapping)
        { }

        void AddKey(int index, NVersionedTableClient::TKey key)
        {
            if (Batches_.empty() || Batches_.back()->Indexes.size() >= Config_->MaxRowsPerReadRequest) {
                Batches_.emplace_back(new TBatch());
            }

            auto& batch = Batches_.back();
            batch->Indexes.push_back(index);
            batch->Keys.push_back(key);
        }

        TFuture<void> Invoke(IChannelPtr channel)
        {
            // Do all the heavy lifting here.
            for (auto& batch : Batches_) {
                TReqLookupRows req;
                if (!Options_.ColumnFilter.All) {
                    ToProto(req.mutable_column_filter()->mutable_indexes(), Options_.ColumnFilter.Indexes);
                }

                TWireProtocolWriter writer;
                writer.WriteCommand(EWireProtocolCommand::LookupRows);
                writer.WriteMessage(req);
                writer.WriteUnversionedRowset(batch->Keys, &IdMapping_);

                batch->RequestData = NCompression::CompressWithEnvelope(
                    writer.Flush(),
                    Config_->LookupRequestCodec);
            }

            InvokeChannel_ = channel;
            InvokeNextBatch();
            return InvokePromise_;
        }

        void ParseResponse(
            std::vector<TUnversionedRow>* resultRows,
            std::vector<std::unique_ptr<TWireProtocolReader>>* readers)
        {
            for (const auto& batch : Batches_) {
                auto data = NCompression::DecompressWithEnvelope(batch->Response->Attachments());
                auto reader = std::make_unique<TWireProtocolReader>(data);
                for (int index = 0; index < batch->Keys.size(); ++index) {
                    auto row = reader->ReadUnversionedRow();
                    (*resultRows)[batch->Indexes[index]] = row;
                }
                readers->push_back(std::move(reader));
            }
        }

    private:
        TConnectionConfigPtr Config_;
        TTabletId TabletId_;
        TLookupRowsOptions Options_;
        TNameTableToSchemaIdMapping IdMapping_;

        struct TBatch
        {
            std::vector<int> Indexes;
            std::vector<NVersionedTableClient::TKey> Keys;
            std::vector<TSharedRef> RequestData;
            TTabletServiceProxy::TRspReadPtr Response;
        };

        std::vector<std::unique_ptr<TBatch>> Batches_;

        IChannelPtr InvokeChannel_;
        int InvokeBatchIndex_ = 0;
        TPromise<void> InvokePromise_ = NewPromise<void>();


        void InvokeNextBatch()
        {
            if (InvokeBatchIndex_ >= Batches_.size()) {
                InvokePromise_.Set(TError());
                return;
            }

            const auto& batch = Batches_[InvokeBatchIndex_];

            TTabletServiceProxy proxy(InvokeChannel_);
            proxy.SetDefaultTimeout(Config_->LookupTimeout);
            proxy.SetDefaultRequestAck(false);

            auto req = proxy.Read();
            ToProto(req->mutable_tablet_id(), TabletId_);
            req->set_timestamp(Options_.Timestamp);
            req->set_response_codec(static_cast<int>(Config_->LookupResponseCodec));
            req->Attachments() = std::move(batch->RequestData);

            req->Invoke().Subscribe(
                BIND(&TTabletLookupSession::OnResponse, MakeStrong(this)));
        }

        void OnResponse(const TTabletServiceProxy::TErrorOrRspReadPtr& rspOrError)
        {
            if (rspOrError.IsOK()) {
                Batches_[InvokeBatchIndex_]->Response = rspOrError.Value();
                ++InvokeBatchIndex_;
                InvokeNextBatch();
            } else {
                InvokePromise_.Set(rspOrError);
            }
        }

    };

    typedef TIntrusivePtr<TTabletLookupSession> TLookupTabletSessionPtr;

    IRowsetPtr DoLookupRows(
        const TYPath& path,
        TNameTablePtr nameTable,
        const std::vector<NVersionedTableClient::TKey>& keys,
        const TLookupRowsOptions& options)
    {
        auto tableInfo = SyncGetTableInfo(path);

        int schemaColumnCount = static_cast<int>(tableInfo->Schema.Columns().size());
        int keyColumnCount = static_cast<int>(tableInfo->KeyColumns.size());

        ValidateColumnFilter(options.ColumnFilter, schemaColumnCount);

        auto resultSchema = tableInfo->Schema.Filter(options.ColumnFilter);
        auto idMapping = BuildColumnIdMapping(tableInfo, nameTable);

        // Server-side is specifically optimized for handling long runs of keys
        // from the same partition. Let's sort the keys to facilitate this.
        std::vector<std::pair<NVersionedTableClient::TKey, int>> sortedKeys;
        sortedKeys.reserve(keys.size());

        auto rowBuffer = New<TRowBuffer>();

        if (tableInfo->NeedKeyEvaluation) {
            auto evaluatorCache = Connection_->GetColumnEvaluatorCache();
            auto evaluator = evaluatorCache->Find(tableInfo->Schema, keyColumnCount);

            for (int index = 0; index < keys.size(); ++index) {
                ValidateClientKey(keys[index], keyColumnCount, tableInfo->Schema);
                evaluator->EvaluateKeys(keys[index], rowBuffer);
                sortedKeys.push_back(std::make_pair(keys[index], index));
            }
        } else {
            for (int index = 0; index < static_cast<int>(keys.size()); ++index) {
                ValidateClientKey(keys[index], keyColumnCount, tableInfo->Schema);
                sortedKeys.push_back(std::make_pair(keys[index], index));
            }
        }
        std::sort(sortedKeys.begin(), sortedKeys.end());

        yhash_map<TTabletInfoPtr, TLookupTabletSessionPtr> tabletToSession;

        for (const auto& pair : sortedKeys) {
            int index = pair.second;
            auto key = pair.first;
            auto tabletInfo = SyncGetTabletInfo(tableInfo, key);
            auto it = tabletToSession.find(tabletInfo);
            if (it == tabletToSession.end()) {
                it = tabletToSession.insert(std::make_pair(
                    tabletInfo,
                    New<TTabletLookupSession>(this, tabletInfo, options, idMapping))).first;
            }
            const auto& session = it->second;
            session->AddKey(index, key);
        }

        std::vector<TFuture<void>> asyncResults;
        for (const auto& pair : tabletToSession) {
            const auto& tabletInfo = pair.first;
            const auto& session = pair.second;
            auto channel = GetTabletChannel(tabletInfo->CellId);
            asyncResults.push_back(session->Invoke(std::move(channel)));
        }

        WaitFor(Combine(asyncResults))
            .ThrowOnError();

        std::vector<TUnversionedRow> resultRows;
        resultRows.resize(keys.size());

        std::vector<std::unique_ptr<TWireProtocolReader>> readers;

        for (const auto& pair : tabletToSession) {
            const auto& session = pair.second;
            session->ParseResponse(&resultRows, &readers);
        }

        if (!options.KeepMissingRows) {
            resultRows.erase(
                std::remove_if(
                    resultRows.begin(),
                    resultRows.end(),
                    [] (TUnversionedRow row) {
                        return !static_cast<bool>(row);
                    }),
                resultRows.end());
        }

        return CreateRowset(
            std::move(readers),
            resultSchema,
            std::move(resultRows));
    }

    TQueryStatistics DoSelectRows(
        const Stroka& query,
        ISchemafulWriterPtr writer,
        const TSelectRowsOptions& options)
    {
        auto inputRowLimit = options.InputRowLimit.Get(Connection_->GetConfig()->DefaultInputRowLimit);
        auto outputRowLimit = options.OutputRowLimit.Get(Connection_->GetConfig()->DefaultOutputRowLimit);
        auto fragment = PreparePlanFragment(
            QueryHelper_.Get(),
            query,
            FunctionRegistry_.Get(),
            inputRowLimit,
            outputRowLimit,
            options.Timestamp);
        fragment->RangeExpansionLimit = options.RangeExpansionLimit;
        fragment->VerboseLogging = options.VerboseLogging;
        auto statistics = WaitFor(QueryHelper_->Execute(fragment, writer))
            .ValueOrThrow();
        if (options.FailOnIncompleteResult) {
            if (statistics.IncompleteInput) {
                THROW_ERROR_EXCEPTION("Query terminated prematurely due to excessive input; consider rewriting your query or changing input limit")
                    << TErrorAttribute("input_row_limit", inputRowLimit);
            }
            if (statistics.IncompleteOutput) {
                THROW_ERROR_EXCEPTION("Query terminated prematurely due to excessive output; consider rewriting your query or changing output limit")
                    << TErrorAttribute("output_row_limit", outputRowLimit);
            }
        }
        return statistics;
    }


    void DoMountTable(
        const TYPath& path,
        const TMountTableOptions& options)
    {
        auto req = TTableYPathProxy::Mount(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_last_tablet_index(*options.LastTabletIndex);
        }
        if (options.CellId != NullTabletCellId) {
            ToProto(req->mutable_cell_id(), options.CellId);
        }
        req->set_estimated_uncompressed_size(options.EstimatedUncompressedSize);
        req->set_estimated_compressed_size(options.EstimatedCompressedSize);

        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoUnmountTable(
        const TYPath& path,
        const TUnmountTableOptions& options)
    {
        auto req = TTableYPathProxy::Unmount(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_last_tablet_index(*options.LastTabletIndex);
        }
        req->set_force(options.Force);

        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoRemountTable(
        const TYPath& path,
        const TRemountTableOptions& options)
    {
        auto req = TTableYPathProxy::Remount(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_first_tablet_index(*options.LastTabletIndex);
        }

        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoReshardTable(
        const TYPath& path,
        const std::vector<NVersionedTableClient::TKey>& pivotKeys,
        const TReshardTableOptions& options)
    {
        auto req = TTableYPathProxy::Reshard(path);
        if (options.FirstTabletIndex) {
            req->set_first_tablet_index(*options.FirstTabletIndex);
        }
        if (options.LastTabletIndex) {
            req->set_last_tablet_index(*options.LastTabletIndex);
        }
        ToProto(req->mutable_pivot_keys(), pivotKeys);

        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }


    TYsonString DoGetNode(
        const TYPath& path,
        const TGetNodeOptions& options)
    {
        auto req = TYPathProxy::Get(path);
        SetTransactionId(req, options, true);
        SetSuppressAccessTracking(req, options);

        ToProto(req->mutable_attribute_filter(), options.AttributeFilter);
        if (options.MaxSize) {
            req->set_max_size(*options.MaxSize);
        }
        req->set_ignore_opaque(options.IgnoreOpaque);
        if (options.Options) {
            ToProto(req->mutable_options(), *options.Options);
        }

        const auto& proxy = ObjectProxies_[EMasterChannelKind::LeaderOrFollower];
        auto rsp = WaitFor(proxy->Execute(req))
            .ValueOrThrow();

        return TYsonString(rsp->value());
    }

    void DoSetNode(
        const TYPath& path,
        const TYsonString& value,
        TSetNodeOptions options)
    {
        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TYPathProxy::Set(path);
        SetTransactionId(req, options, true);
        GenerateMutationId(req, options);
        req->set_value(value.Data());
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        batchRsp->GetResponse<TYPathProxy::TRspSet>(0)
            .ThrowOnError();
    }

    void DoRemoveNode(
        const TYPath& path,
        TRemoveNodeOptions options)
    {
        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TYPathProxy::Remove(path);
        SetTransactionId(req, options, true);
        GenerateMutationId(req, options);
        req->set_recursive(options.Recursive);
        req->set_force(options.Force);
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        batchRsp->GetResponse<TYPathProxy::TRspRemove>(0)
            .ThrowOnError();
    }

    TYsonString DoListNode(
        const TYPath& path,
        const TListNodeOptions& options)
    {
        auto req = TYPathProxy::List(path);
        SetTransactionId(req, options, true);
        SetSuppressAccessTracking(req, options);

        ToProto(req->mutable_attribute_filter(), options.AttributeFilter);
        if (options.MaxSize) {
            req->set_max_size(*options.MaxSize);
        }

        const auto& proxy = ObjectProxies_[EMasterChannelKind::LeaderOrFollower];
        auto rsp = WaitFor(proxy->Execute(req))
            .ValueOrThrow();
        return TYsonString(rsp->keys());
    }

    TNodeId DoCreateNode(
        const TYPath& path,
        EObjectType type,
        TCreateNodeOptions options)
    {
        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Create(path);
        SetTransactionId(req, options, true);
        GenerateMutationId(req, options);
        req->set_type(static_cast<int>(type));
        req->set_recursive(options.Recursive);
        req->set_ignore_existing(options.IgnoreExisting);
        if (options.Attributes) {
            ToProto(req->mutable_node_attributes(), *options.Attributes);
        }
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCreate>(0)
            .ValueOrThrow();
        return FromProto<TNodeId>(rsp->node_id());
    }

    TLockId DoLockNode(
        const TYPath& path,
        NCypressClient::ELockMode mode,
        TLockNodeOptions options)
    {
        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Lock(path);
        SetTransactionId(req, options, false);
        GenerateMutationId(req, options);
        req->set_mode(static_cast<int>(mode));
        req->set_waitable(options.Waitable);
        if (options.ChildKey) {
            req->set_child_key(*options.ChildKey);
        }
        if (options.AttributeKey) {
            req->set_attribute_key(*options.AttributeKey);
        }
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspLock>(0)
            .ValueOrThrow();
        return FromProto<TLockId>(rsp->lock_id());
    }

    TNodeId DoCopyNode(
        const TYPath& srcPath,
        const TYPath& dstPath,
        TCopyNodeOptions options)
    {
        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Copy(dstPath);
        SetTransactionId(req, options, true);
        GenerateMutationId(req, options);
        req->set_source_path(srcPath);
        req->set_preserve_account(options.PreserveAccount);
        req->set_recursive(options.Recursive);
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCopy>(0)
            .ValueOrThrow();
        return FromProto<TNodeId>(rsp->object_id());
    }

    TNodeId DoMoveNode(
        const TYPath& srcPath,
        const TYPath& dstPath,
        TMoveNodeOptions options)
    {
        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Copy(dstPath);
        SetTransactionId(req, options, true);
        GenerateMutationId(req, options);
        req->set_source_path(srcPath);
        req->set_preserve_account(options.PreserveAccount);
        req->set_remove_source(true);
        req->set_recursive(options.Recursive);
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCopy>(0)
            .ValueOrThrow();
        return FromProto<TNodeId>(rsp->object_id());
    }

    TNodeId DoLinkNode(
        const TYPath& srcPath,
        const TYPath& dstPath,
        TLinkNodeOptions options)
    {
        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TCypressYPathProxy::Create(dstPath);
        req->set_type(static_cast<int>(EObjectType::Link));
        req->set_recursive(options.Recursive);
        req->set_ignore_existing(options.IgnoreExisting);
        SetTransactionId(req, options, true);
        GenerateMutationId(req, options);
        auto attributes = options.Attributes ? ConvertToAttributes(options.Attributes.get()) : CreateEphemeralAttributes();
        attributes->Set("target_path", srcPath);
        ToProto(req->mutable_node_attributes(), *attributes);
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCreate>(0)
            .ValueOrThrow();
        return FromProto<TNodeId>(rsp->node_id());
    }

    bool DoNodeExists(
        const TYPath& path,
        const TNodeExistsOptions& options)
    {
        auto req = TYPathProxy::Exists(path);
        SetTransactionId(req, options, true);

        const auto& proxy = ObjectProxies_[EMasterChannelKind::LeaderOrFollower];
        auto rsp = WaitFor(proxy->Execute(req))
            .ValueOrThrow();
        return rsp->value();
    }


    TObjectId DoCreateObject(
        EObjectType type,
        TCreateObjectOptions options)
    {
        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        auto batchReq = proxy->ExecuteBatch();
        SetPrerequisites(batchReq, options);

        auto req = TMasterYPathProxy::CreateObjects();
        GenerateMutationId(req, options);
        if (options.TransactionId != NullTransactionId) {
            ToProto(req->mutable_transaction_id(), options.TransactionId);
        }
        req->set_type(static_cast<int>(type));
        if (options.Attributes) {
            ToProto(req->mutable_object_attributes(), *options.Attributes);
        }
        batchReq->AddRequest(req);

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();
        auto rsp = batchRsp->GetResponse<TMasterYPathProxy::TRspCreateObjects>(0)
            .ValueOrThrow();
        return FromProto<TObjectId>(rsp->object_ids(0));
    }


    static Stroka GetGroupPath(const Stroka& name)
    {
        return "//sys/groups/" + ToYPathLiteral(name);
    }

    void DoAddMember(
        const Stroka& group,
        const Stroka& member,
        TAddMemberOptions options)
    {
        auto req = TGroupYPathProxy::AddMember(GetGroupPath(group));
        req->set_name(member);
        GenerateMutationId(req, options);

        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    void DoRemoveMember(
        const Stroka& group,
        const Stroka& member,
        TRemoveMemberOptions options)
    {
        auto req = TGroupYPathProxy::RemoveMember(GetGroupPath(group));
        req->set_name(member);
        GenerateMutationId(req, options);

        const auto& proxy = ObjectProxies_[EMasterChannelKind::Leader];
        WaitFor(proxy->Execute(req))
            .ThrowOnError();
    }

    TCheckPermissionResult DoCheckPermission(
        const Stroka& user,
        const TYPath& path,
        EPermission permission,
        const TCheckPermissionOptions& options)
    {
        auto req = TObjectYPathProxy::CheckPermission(path);
        req->set_user(user);
        req->set_permission(static_cast<int>(permission));
        SetTransactionId(req, options, true);

        const auto& proxy = ObjectProxies_[EMasterChannelKind::LeaderOrFollower];
        auto rsp = WaitFor(proxy->Execute(req))
            .ValueOrThrow();

        TCheckPermissionResult result;
        result.Action = ESecurityAction(rsp->action());
        result.ObjectId = rsp->has_object_id() ? FromProto<TObjectId>(rsp->object_id()) : NullObjectId;
        result.ObjectName = rsp->has_object_name() ? MakeNullable(rsp->object_name()) : Null;
        result.SubjectId = rsp->has_subject_id() ? FromProto<TSubjectId>(rsp->subject_id()) : NullObjectId;
        result.SubjectName = rsp->has_subject_name() ? MakeNullable(rsp->subject_name()) : Null;
        return result;
    }


    TOperationId DoStartOperation(
        EOperationType type,
        const TYsonString& spec,
        TStartOperationOptions options)
    {
        auto req = SchedulerProxy_->StartOperation();
        SetTransactionId(req, options, true);
        GenerateMutationId(req, options);
        req->set_type(static_cast<int>(type));
        req->set_spec(spec.Data());

        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();

        return FromProto<TOperationId>(rsp->operation_id());
    }

    void DoAbortOperation(
        const TOperationId& operationId,
        const TAbortOperationOptions& /*options*/)
    {
        auto req = SchedulerProxy_->AbortOperation();
        ToProto(req->mutable_operation_id(), operationId);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    void DoSuspendOperation(
        const TOperationId& operationId,
        const TSuspendOperationOptions& /*options*/)
    {
        auto req = SchedulerProxy_->SuspendOperation();
        ToProto(req->mutable_operation_id(), operationId);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    void DoResumeOperation(
        const TOperationId& operationId,
        const TResumeOperationOptions& /*options*/)
    {
        auto req = SchedulerProxy_->ResumeOperation();
        ToProto(req->mutable_operation_id(), operationId);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }


    void DoDumpJobContext(
        const TJobId& jobId,
        const TYPath& path,
        const TDumpJobContextOptions& /*options*/)
    {
        auto req = JobProberProxy_->DumpInputContext();
        ToProto(req->mutable_job_id(), jobId);
        ToProto(req->mutable_path(), path);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    TYsonString DoStraceJob(
        const TJobId& jobId,
        const TStraceJobOptions& /*options*/)
    {
        auto req = JobProberProxy_->Strace();
        ToProto(req->mutable_job_id(), jobId);

        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();

        return TYsonString(FromProto<Stroka>(rsp->trace()));
    }
};

DEFINE_REFCOUNTED_TYPE(TClient)

IClientPtr CreateClient(IConnectionPtr connection, const TClientOptions& options)
{
    return New<TClient>(std::move(connection), options);
}

////////////////////////////////////////////////////////////////////////////////

class TTransaction
    : public ITransaction
{
public:
    TTransaction(
        TClientPtr client,
        NTransactionClient::TTransactionPtr transaction)
        : Client_(std::move(client))
        , Transaction_(std::move(transaction))
        , Logger(Client_->Logger)
    {
        Logger.AddTag("TransactionId: %v", GetId());
    }


    virtual IConnectionPtr GetConnection() override
    {
        return Client_->GetConnection();
    }

    virtual IClientPtr GetClient() const override
    {
        return Client_;
    }

    virtual NTransactionClient::ETransactionType GetType() const override
    {
        return Transaction_->GetType();
    }

    virtual const TTransactionId& GetId() const override
    {
        return Transaction_->GetId();
    }

    virtual TTimestamp GetStartTimestamp() const override
    {
        return Transaction_->GetStartTimestamp();
    }

    virtual EAtomicity GetAtomicity() const override
    {
        return Transaction_->GetAtomicity();
    }

    virtual EDurability GetDurability() const override
    {
        return Transaction_->GetDurability();
    }


    virtual TFuture<void> Commit(const TTransactionCommitOptions& options) override
    {
        return BIND(&TTransaction::DoCommit, MakeStrong(this))
            .AsyncVia(Client_->Invoker_)
            .Run(options);
    }

    virtual TFuture<void> Abort(const TTransactionAbortOptions& options) override
    {
        return Transaction_->Abort(options);
    }


    virtual TFuture<ITransactionPtr> StartTransaction(
        ETransactionType type,
        const TTransactionStartOptions& options) override
    {
        auto adjustedOptions = options;
        adjustedOptions.ParentId = GetId();
        return Client_->StartTransaction(
            type,
            adjustedOptions);
    }


    virtual void WriteRow(
        const TYPath& path,
        TNameTablePtr nameTable,
        TUnversionedRow row,
        const TWriteRowsOptions& options) override
    {
        WriteRows(
            path,
            std::move(nameTable),
            std::vector<TUnversionedRow>(1, row),
            options);
    }

    virtual void WriteRows(
        const TYPath& path,
        TNameTablePtr nameTable,
        std::vector<TUnversionedRow> rows,
        const TWriteRowsOptions& options) override
    {
        Requests_.push_back(std::unique_ptr<TRequestBase>(new TWriteRequest(
            this,
            path,
            std::move(nameTable),
            std::move(rows),
            options)));
        LOG_DEBUG("Row writes buffered (RowCount: %v)",
            rows.size());
    }


    virtual void DeleteRow(
        const TYPath& path,
        TNameTablePtr nameTable,
        NVersionedTableClient::TKey key,
        const TDeleteRowsOptions& options) override
    {
        DeleteRows(
            path,
            std::move(nameTable),
            std::vector<NVersionedTableClient::TKey>(1, key),
            options);
    }

    virtual void DeleteRows(
        const TYPath& path,
        TNameTablePtr nameTable,
        std::vector<NVersionedTableClient::TKey> keys,
        const TDeleteRowsOptions& options) override
    {
        Requests_.push_back(std::unique_ptr<TRequestBase>(new TDeleteRequest(
            this,
            path,
            std::move(nameTable),
            std::move(keys),
            options)));
        LOG_DEBUG("Row deletes buffered (RowCount: %v)",
            keys.size());
    }


#define DELEGATE_TRANSACTIONAL_METHOD(returnType, method, signature, args) \
    virtual returnType method signature override \
    { \
        auto& originalOptions = options; \
        { \
            auto options = originalOptions; \
            options.TransactionId = GetId(); \
            return Client_->method args; \
        } \
    }

#define DELEGATE_TIMESTAMPED_METHOD(returnType, method, signature, args) \
    virtual returnType method signature override \
    { \
        auto& originalOptions = options; \
        { \
            auto options = originalOptions; \
            options.Timestamp = GetReadTimestamp(); \
            return Client_->method args; \
        } \
    }

    DELEGATE_TIMESTAMPED_METHOD(TFuture<IRowsetPtr>, LookupRow, (
        const TYPath& path,
        TNameTablePtr nameTable,
        NVersionedTableClient::TKey key,
        const TLookupRowsOptions& options),
        (path, nameTable, key, options))
    DELEGATE_TIMESTAMPED_METHOD(TFuture<IRowsetPtr>, LookupRows, (
        const TYPath& path,
        TNameTablePtr nameTable,
        const std::vector<NVersionedTableClient::TKey>& keys,
        const TLookupRowsOptions& options),
        (path, nameTable, keys, options))


    DELEGATE_TIMESTAMPED_METHOD(TFuture<NQueryClient::TQueryStatistics>, SelectRows, (
        const Stroka& query,
        ISchemafulWriterPtr writer,
        const TSelectRowsOptions& options),
        (query, writer, options))
    typedef std::pair<IRowsetPtr, NQueryClient::TQueryStatistics> TSelectRowsResult;
    DELEGATE_TIMESTAMPED_METHOD(TFuture<TSelectRowsResult>, SelectRows, (
        const Stroka& query,
        const TSelectRowsOptions& options),
        (query, options))


    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TYsonString>, GetNode, (
        const TYPath& path,
        const TGetNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, SetNode, (
        const TYPath& path,
        const TYsonString& value,
        const TSetNodeOptions& options),
        (path, value, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<void>, RemoveNode, (
        const TYPath& path,
        const TRemoveNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TYsonString>, ListNode, (
        const TYPath& path,
        const TListNodeOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, CreateNode, (
        const TYPath& path,
        EObjectType type,
        const TCreateNodeOptions& options),
        (path, type, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TLockId>, LockNode, (
        const TYPath& path,
        NCypressClient::ELockMode mode,
        const TLockNodeOptions& options),
        (path, mode, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, CopyNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TCopyNodeOptions& options),
        (srcPath, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, MoveNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TMoveNodeOptions& options),
        (srcPath, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TNodeId>, LinkNode, (
        const TYPath& srcPath,
        const TYPath& dstPath,
        const TLinkNodeOptions& options),
        (srcPath, dstPath, options))
    DELEGATE_TRANSACTIONAL_METHOD(TFuture<bool>, NodeExists, (
        const TYPath& path,
        const TNodeExistsOptions& options),
        (path, options))


    DELEGATE_TRANSACTIONAL_METHOD(TFuture<TObjectId>, CreateObject, (
        EObjectType type,
        const TCreateObjectOptions& options),
        (type, options))


    DELEGATE_TRANSACTIONAL_METHOD(IFileReaderPtr, CreateFileReader, (
        const TYPath& path,
        const TFileReaderOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(IFileWriterPtr, CreateFileWriter, (
        const TYPath& path,
        const TFileWriterOptions& options),
        (path, options))


    DELEGATE_TRANSACTIONAL_METHOD(IJournalReaderPtr, CreateJournalReader, (
        const TYPath& path,
        const TJournalReaderOptions& options),
        (path, options))
    DELEGATE_TRANSACTIONAL_METHOD(IJournalWriterPtr, CreateJournalWriter, (
        const TYPath& path,
        const TJournalWriterOptions& options),
        (path, options))

#undef DELEGATE_TRANSACTIONAL_METHOD
#undef DELEGATE_TIMESTAMPED_METHOD

    TRowBufferPtr GetRowBuffer() const
    {
        return RowBuffer_;
    }

private:
    const TClientPtr Client_;
    const NTransactionClient::TTransactionPtr Transaction_;

    TRowBufferPtr RowBuffer_ = New<TRowBuffer>();

    NLogging::TLogger Logger;


    class TRequestBase
    {
    public:
        void Run()
        {
            DoPrepare();
            DoRun();
        }

    protected:
        explicit TRequestBase(
            TTransaction* transaction,
            const TYPath& path,
            TNameTablePtr nameTable)
            : Transaction_(transaction)
            , Path_(path)
            , NameTable_(std::move(nameTable))
        { }

        TTransaction* const Transaction_;
        const TYPath Path_;
        const TNameTablePtr NameTable_;

        TTableMountInfoPtr TableInfo_;


        void DoPrepare()
        {
            TableInfo_ = Transaction_->Client_->SyncGetTableInfo(Path_);
        }

        virtual void DoRun() = 0;

    };

    class TModifyRequest
        : public TRequestBase
    {
    protected:
        using TRowValidator = std::function<void(TUnversionedRow, int, const TNameTableToSchemaIdMapping&, const TTableSchema&)>;

        TModifyRequest(
            TTransaction* transaction,
            const TYPath& path,
            TNameTablePtr nameTable)
            : TRequestBase(transaction, path, std::move(nameTable))
        { }

        void WriteRequests(
            const std::vector<TUnversionedRow>& rows,
            EWireProtocolCommand command,
            int columnCount,
            TRowValidator validateRow)
        {
            const auto& idMapping = Transaction_->GetColumnIdMapping(TableInfo_, NameTable_);
            int keyColumnCount = TableInfo_->KeyColumns.size();

            auto writeRequest = [&] (const TUnversionedRow row) {
                auto tabletInfo = Transaction_->Client_->SyncGetTabletInfo(TableInfo_, row);
                auto* session = Transaction_->GetTabletSession(tabletInfo, TableInfo_);
                session->SubmitRow(command, row, &idMapping);
            };

            if (TableInfo_->NeedKeyEvaluation) {
                const auto& rowBuffer = Transaction_->GetRowBuffer();
                auto evaluatorCache = Transaction_->GetConnection()->GetColumnEvaluatorCache();
                auto evaluator = evaluatorCache->Find(TableInfo_->Schema, keyColumnCount);

                for (auto row : rows) {
                    validateRow(row, keyColumnCount, idMapping, TableInfo_->Schema);
                    evaluator->EvaluateKeys(row, rowBuffer);
                    writeRequest(row);
                    rowBuffer->Clear();
                }
            } else {
                for (auto row : rows) {
                    validateRow(row, keyColumnCount, idMapping, TableInfo_->Schema);
                    writeRequest(row);
                }
            }
        }
    };

    class TWriteRequest
        : public TModifyRequest
    {
    public:
        TWriteRequest(
            TTransaction* transaction,
            const TYPath& path,
            TNameTablePtr nameTable,
            std::vector<TUnversionedRow> rows,
            const TWriteRowsOptions& options)
            : TModifyRequest(transaction, path, std::move(nameTable))
            , Rows_(std::move(rows))
            , Options_(options)
        { }

    private:
        const std::vector<TUnversionedRow> Rows_;
        const TWriteRowsOptions Options_;

        virtual void DoRun() override
        {
            WriteRequests(
                Rows_,
                EWireProtocolCommand::WriteRow,
                TableInfo_->Schema.Columns().size(),
                ValidateClientDataRow);
        }
    };

    class TDeleteRequest
        : public TModifyRequest
    {
    public:
        TDeleteRequest(
            TTransaction* transaction,
            const TYPath& path,
            TNameTablePtr nameTable,
            std::vector<NVersionedTableClient::TKey> keys,
            const TDeleteRowsOptions& options)
            : TModifyRequest(transaction, path, std::move(nameTable))
            , Keys_(std::move(keys))
            , Options_(options)
        { }

    private:
        const std::vector<TUnversionedRow> Keys_;
        const TDeleteRowsOptions Options_;

        virtual void DoRun() override
        {
            WriteRequests(
                Keys_,
                EWireProtocolCommand::DeleteRow,
                TableInfo_->KeyColumns.size(),
                [ ](
                    TUnversionedRow row,
                    int keyColumnCount,
                    const TNameTableToSchemaIdMapping& idMapping,
                    const TTableSchema& schema) {
                    ValidateClientKey(row, keyColumnCount, schema);
                });
        }
    };

    std::vector<std::unique_ptr<TRequestBase>> Requests_;

    class TTabletCommitSession
        : public TIntrinsicRefCounted
    {
    public:
        TTabletCommitSession(
            TTransactionPtr owner,
            TTabletInfoPtr tabletInfo,
            int keyColumnCount,
            int schemaColumnCount)
            : TransactionId_(owner->Transaction_->GetId())
            , TabletId_(tabletInfo->TabletId)
            , Config_(owner->Client_->Connection_->GetConfig())
            , Durability_(owner->Transaction_->GetDurability())
            , KeyColumnCount_(keyColumnCount)
            , SchemaColumnCount_(schemaColumnCount)
            , Logger(owner->Logger)
        {
            Logger.AddTag("TabletId: %v", TabletId_);
        }

        TWireProtocolWriter* GetWriter()
        {
            if (Batches_.empty() || Batches_.back()->RowCount >= Config_->MaxRowsPerWriteRequest) {
                Batches_.emplace_back(new TBatch());
            }
            auto& batch = Batches_.back();
            ++batch->RowCount;
            return &batch->Writer;
        }

        void SubmitRow(
            EWireProtocolCommand command,
            TUnversionedRow row,
            const TNameTableToSchemaIdMapping* idMapping)
        {
            SubmittedRows_.push_back(TSubmittedRow{
                command,
                row,
                idMapping,
                static_cast<int>(SubmittedRows_.size())});
        }

        TFuture<void> Invoke(IChannelPtr channel)
        {
            try {
                std::sort(
                    SubmittedRows_.begin(),
                    SubmittedRows_.end(),
                    [=] (const TSubmittedRow& lhs, const TSubmittedRow& rhs) {
                        int res = CompareRows(lhs.Row, rhs.Row, KeyColumnCount_);
                        return res != 0 ? res < 0 : lhs.SequentialId < rhs.SequentialId;
                    });
            } catch (const std::exception& ex) {
                // NB: CompareRows may throw on composite values.
                return MakeFuture(TError(ex));
            }

            std::vector<TSubmittedRow> mergedRows;
            mergedRows.reserve(SubmittedRows_.size());
            auto merger = TUnversionedRowMerger(
                RowBuffer_->GetPool(),
                SchemaColumnCount_,
                KeyColumnCount_,
                NVersionedTableClient::TColumnFilter());

            auto addPartialRow = [&] (const TSubmittedRow& submittedRow) {
                switch (submittedRow.Command) {
                    case EWireProtocolCommand::DeleteRow:
                        merger.DeletePartialRow(submittedRow.Row);
                        break;

                    case EWireProtocolCommand::WriteRow:
                        merger.AddPartialRow(submittedRow.Row);
                        break;

                    default:
                        YUNREACHABLE();
                }
            };

            int index = 0;
            while (index < SubmittedRows_.size()) {
                if (index < SubmittedRows_.size() - 1 &&
                    CompareRows(SubmittedRows_[index].Row, SubmittedRows_[index + 1].Row, KeyColumnCount_) == 0)
                {
                    addPartialRow(SubmittedRows_[index]);
                    while (index < SubmittedRows_.size() - 1 &&
                        CompareRows(SubmittedRows_[index].Row, SubmittedRows_[index + 1].Row, KeyColumnCount_) == 0)
                    {
                        ++index;
                        addPartialRow(SubmittedRows_[index]);
                    }
                    SubmittedRows_[index].Row = merger.BuildMergedRow();
                }
                mergedRows.push_back(SubmittedRows_[index]);
                ++index;
            }

            SubmittedRows_ = std::move(mergedRows);

            for (const auto& submittedRow : SubmittedRows_) {
                WriteRow(submittedRow);
            }

            // Do all the heavy lifting here.
            YCHECK(!Batches_.empty());
            for (auto& batch : Batches_) {
                batch->RequestData = NCompression::CompressWithEnvelope(
                    batch->Writer.Flush(),
                    Config_->WriteRequestCodec);;
            }

            merger.Reset();

            InvokeChannel_ = channel;
            InvokeNextBatch();
            return InvokePromise_;
        }

    private:
        const TTransactionId TransactionId_;
        const TTabletId TabletId_;
        const TConnectionConfigPtr Config_;
        const EDurability Durability_;
        const int KeyColumnCount_;
        const int SchemaColumnCount_;

        TRowBufferPtr RowBuffer_ = New<TRowBuffer>();

        NLogging::TLogger Logger;

        struct TBatch
        {
            TWireProtocolWriter Writer;
            std::vector<TSharedRef> RequestData;
            int RowCount = 0;
        };

        std::vector<std::unique_ptr<TBatch>> Batches_;

        struct TSubmittedRow
        {
            EWireProtocolCommand Command;
            TUnversionedRow Row;
            const TNameTableToSchemaIdMapping* IdMapping;
            int SequentialId;
        };

        std::vector<TSubmittedRow> SubmittedRows_;

        IChannelPtr InvokeChannel_;
        int InvokeBatchIndex_ = 0;
        TPromise<void> InvokePromise_ = NewPromise<void>();

        void WriteRow(const TSubmittedRow& submittedRow)
        {
            if (Batches_.empty() || Batches_.back()->RowCount >= Config_->MaxRowsPerWriteRequest) {
                Batches_.emplace_back(new TBatch());
            }
            auto& batch = Batches_.back();
            ++batch->RowCount;
            auto& writer = batch->Writer;
            writer.WriteCommand(submittedRow.Command);

            switch (submittedRow.Command) {
                case EWireProtocolCommand::DeleteRow: {
                    auto req = TReqDeleteRow();
                    writer.WriteMessage(req);
                    break;
                }

                case EWireProtocolCommand::WriteRow: {
                    auto req = TReqWriteRow();
                    writer.WriteMessage(req);
                    break;
                }

                default:
                    YUNREACHABLE();
            }

            writer.WriteUnversionedRow(submittedRow.Row, submittedRow.IdMapping);
        }

        void InvokeNextBatch()
        {
            if (InvokeBatchIndex_ >= Batches_.size()) {
                InvokePromise_.Set(TError());
                return;
            }

            const auto& batch = Batches_[InvokeBatchIndex_];

            LOG_DEBUG("Sending batch (BatchIndex: %v, BatchCount: %v, RowCount: %v)",
                InvokeBatchIndex_,
                batch->RowCount);

            TTabletServiceProxy proxy(InvokeChannel_);
            proxy.SetDefaultTimeout(Config_->WriteTimeout);
            proxy.SetDefaultRequestAck(false);

            auto req = proxy.Write();
            ToProto(req->mutable_transaction_id(), TransactionId_);
            ToProto(req->mutable_tablet_id(), TabletId_);
            req->set_durability(static_cast<int>(Durability_));
            req->Attachments() = std::move(batch->RequestData);

            req->Invoke().Subscribe(
                BIND(&TTabletCommitSession::OnResponse, MakeStrong(this)));
        }

        void OnResponse(const TTabletServiceProxy::TErrorOrRspWritePtr& rspOrError)
        {
            if (rspOrError.IsOK()) {
                LOG_DEBUG("Batch sent successfully");
                ++InvokeBatchIndex_;
                InvokeNextBatch();
            } else {
                LOG_DEBUG(rspOrError, "Error sending batch");
                InvokePromise_.Set(rspOrError);
            }
        }

    };

    typedef TIntrusivePtr<TTabletCommitSession> TTabletSessionPtr;

    yhash_map<TTabletInfoPtr, TTabletSessionPtr> TabletToSession_;

    std::vector<TFuture<void>> AsyncTransactionStartResults_;

    // Maps ids from name table to schema, for each involved name table.
    yhash_map<TNameTablePtr, TNameTableToSchemaIdMapping> NameTableToIdMapping_;


    const TNameTableToSchemaIdMapping& GetColumnIdMapping(const TTableMountInfoPtr& tableInfo, const TNameTablePtr& nameTable)
    {
        auto it = NameTableToIdMapping_.find(nameTable);
        if (it == NameTableToIdMapping_.end()) {
            auto mapping = BuildColumnIdMapping(tableInfo, nameTable);
            it = NameTableToIdMapping_.insert(std::make_pair(nameTable, std::move(mapping))).first;
        }
        return it->second;
    }

    TTabletCommitSession* GetTabletSession(const TTabletInfoPtr& tabletInfo, const TTableMountInfoPtr& tableInfo)
    {
        auto it = TabletToSession_.find(tabletInfo);
        if (it == TabletToSession_.end()) {
            AsyncTransactionStartResults_.push_back(Transaction_->AddTabletParticipant(tabletInfo->CellId));
            it = TabletToSession_.insert(std::make_pair(
                tabletInfo,
                New<TTabletCommitSession>(
                    this,
                    tabletInfo,
                    tableInfo->KeyColumns.size(),
                    tableInfo->Schema.Columns().size())
                )).first;
        }
        return it->second.Get();
    }

    void DoCommit(const TTransactionCommitOptions& options)
    {
        try {
            for (const auto& request : Requests_) {
                request->Run();
            }

            WaitFor(Combine(AsyncTransactionStartResults_))
                .ThrowOnError();

            std::vector<TFuture<void>> asyncResults;
            for (const auto& pair : TabletToSession_) {
                const auto& tabletInfo = pair.first;
                const auto& session = pair.second;
                auto channel = Client_->GetTabletChannel(tabletInfo->CellId);
                asyncResults.push_back(session->Invoke(std::move(channel)));
            }

            WaitFor(Combine(asyncResults))
                .ThrowOnError();
        } catch (const std::exception& ex) {
            // Fire and forget.
            Transaction_->Abort();
            throw;
        }

        WaitFor(Transaction_->Commit(options))
            .ThrowOnError();
    }

    TTimestamp GetReadTimestamp() const
    {
        switch (Transaction_->GetAtomicity()) {
            case EAtomicity::Full:
                return GetStartTimestamp();
            case EAtomicity::None:
                // NB: Start timestamp is approximate.
                return SyncLastCommittedTimestamp;
            default:
                YUNREACHABLE();
        }
    }

};

DEFINE_REFCOUNTED_TYPE(TTransaction)

TFuture<ITransactionPtr> TClient::StartTransaction(
    ETransactionType type,
    const TTransactionStartOptions& options)
{
    return TransactionManager_->Start(type, options).Apply(
        BIND([=, this_ = MakeStrong(this)] (NTransactionClient::TTransactionPtr transaction) -> ITransactionPtr {
            return New<TTransaction>(this_, transaction);
        }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

