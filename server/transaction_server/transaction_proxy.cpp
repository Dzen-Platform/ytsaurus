#include "transaction_proxy.h"
#include "transaction_manager.h"
#include "transaction.h"

#include <yt/server/chunk_server/chunk_manager.h>

#include <yt/server/cypress_server/node.h>

#include <yt/server/security_server/account.h>

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/multicell_manager.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/fluent.h>

namespace NYT {
namespace NTransactionServer {

using namespace NYTree;
using namespace NYson;
using namespace NObjectServer;
using namespace NCypressServer;
using namespace NSecurityServer;
using namespace NHydra;
using namespace NObjectClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TTransactionProxy
    : public TNonversionedObjectProxyBase<TTransaction>
{
public:
    TTransactionProxy(
        NCellMaster::TBootstrap* bootstrap,
        TObjectTypeMetadata* metadata,
        TTransaction* transaction)
        : TBase(bootstrap, metadata, transaction)
    { }

private:
    typedef TNonversionedObjectProxyBase<TTransaction> TBase;

    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override
    {
        TBase::ListSystemAttributes(descriptors);

        const auto* transaction = GetThisImpl();

        descriptors->push_back("state");
        descriptors->push_back("secondary_cell_tags");
        descriptors->push_back(TAttributeDescriptor("timeout")
            .SetPresent(transaction->GetTimeout().HasValue())
            .SetReplicated(true));
        descriptors->push_back(TAttributeDescriptor("last_ping_time")
            .SetPresent(transaction->GetTimeout().HasValue()));
        descriptors->push_back(TAttributeDescriptor("title")
            .SetPresent(transaction->GetTitle().HasValue()));
        descriptors->push_back("accounting_enabled");
        descriptors->push_back(TAttributeDescriptor("parent_id")
            .SetReplicated(true));
        descriptors->push_back("start_time");
        descriptors->push_back(TAttributeDescriptor("nested_transaction_ids")
            .SetOpaque(true));
        descriptors->push_back(TAttributeDescriptor("staged_object_ids")
            .SetOpaque(true));
        descriptors->push_back(TAttributeDescriptor("exported_objects")
            .SetOpaque(true));
        descriptors->push_back("exported_object_count");
        descriptors->push_back(TAttributeDescriptor("imported_object_ids")
            .SetOpaque(true));
        descriptors->push_back("imported_object_count");
        descriptors->push_back(TAttributeDescriptor("staged_node_ids")
            .SetOpaque(true));
        descriptors->push_back(TAttributeDescriptor("branched_node_ids")
            .SetOpaque(true));
        descriptors->push_back(TAttributeDescriptor("locked_node_ids")
            .SetOpaque(true));
        descriptors->push_back(TAttributeDescriptor("lock_ids")
            .SetOpaque(true));
        descriptors->push_back("resource_usage");
        descriptors->push_back("multicell_resource_usage");
    }

    virtual bool GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        const auto* transaction = GetThisImpl();

        if (key == "state") {
            BuildYsonFluently(consumer)
                .Value(transaction->GetState());
            return true;
        }

        if (key == "secondary_cell_tags") {
            BuildYsonFluently(consumer)
                .Value(transaction->SecondaryCellTags());
            return true;
        }

        if (key == "timeout" && transaction->GetTimeout()) {
            BuildYsonFluently(consumer)
                .Value(*transaction->GetTimeout());
            return true;
        }

        if (key == "title" && transaction->GetTitle()) {
            BuildYsonFluently(consumer)
                .Value(*transaction->GetTitle());
            return true;
        }

        if (key == "accounting_enabled") {
            BuildYsonFluently(consumer)
                .Value(transaction->GetAccountingEnabled());
            return true;
        }

        if (key == "parent_id") {
            BuildYsonFluently(consumer)
                .Value(GetObjectId(transaction->GetParent()));
            return true;
        }

        if (key == "start_time") {
            BuildYsonFluently(consumer)
                .Value(transaction->GetStartTime());
            return true;
        }

        if (key == "nested_transaction_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(transaction->NestedTransactions(), [=] (TFluentList fluent, TTransaction* nestedTransaction) {
                    fluent.Item().Value(nestedTransaction->GetId());
                });
            return true;
        }

        if (key == "staged_node_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(transaction->StagedNodes(), [=] (TFluentList fluent, const TCypressNodeBase* node) {
                    fluent.Item().Value(node->GetId());
                });
            return true;
        }

        if (key == "branched_node_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(transaction->BranchedNodes(), [=] (TFluentList fluent, const TCypressNodeBase* node) {
                    fluent.Item().Value(node->GetId());
                });
            return true;
        }

        if (key == "locked_node_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(transaction->LockedNodes(), [=] (TFluentList fluent, const TCypressNodeBase* node) {
                    fluent.Item().Value(node->GetId());
                });
            return true;
        }

        if (key == "lock_ids") {
            BuildYsonFluently(consumer)
                .DoListFor(transaction->Locks(), [=] (TFluentList fluent, const TLock* lock) {
                    fluent.Item().Value(lock->GetId());
                });
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual TFuture<TYsonString> GetBuiltinAttributeAsync(const Stroka& key) override
    {
        const auto* transaction = GetThisImpl();
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        if (key == "last_ping_time") {
            RequireLeader();
            return Bootstrap_
                ->GetTransactionManager()
                ->GetLastPingTime(transaction)
                .Apply(BIND([] (TInstant value) {
                    return ConvertToYsonString(value);
                }));
        }

        if (key == "resource_usage") {
            return GetAggregatedResourceUsageMap().Apply(BIND([=] (const TAccountResourcesMap& usageMap) {
                return BuildYsonStringFluently()
                    .DoMapFor(usageMap, [=] (TFluentMap fluent, const TAccountResourcesMap::value_type& nameAndUsage) {
                        fluent
                            .Item(nameAndUsage.first)
                            .Value(New<TSerializableClusterResources>(chunkManager, nameAndUsage.second));
                    });
            }).AsyncVia(GetCurrentInvoker()));
        }

        if (key == "multicell_resource_usage") {
            return GetMulticellResourceUsageMap().Apply(BIND([=] (const TMulticellAccountResourcesMap& multicellUsageMap) {
                return BuildYsonStringFluently()
                    .DoMapFor(multicellUsageMap, [=] (TFluentMap fluent, const TMulticellAccountResourcesMap::value_type& cellTagAndUsageMap) {
                        fluent
                            .Item(ToString(cellTagAndUsageMap.first))
                            .DoMapFor(cellTagAndUsageMap.second, [=] (TFluentMap fluent, const TAccountResourcesMap::value_type& nameAndUsage) {
                                fluent
                                    .Item(nameAndUsage.first)
                                    .Value(New<TSerializableClusterResources>(chunkManager, nameAndUsage.second));
                            });
                    });
            }).AsyncVia(GetCurrentInvoker()));
        }

        if (key == "staged_object_ids") {
            return FetchMergeableAttribute(
                key,
                BIND([=, this_ = MakeStrong(this)] {
                    return BuildYsonStringFluently().DoListFor(transaction->StagedObjects(), [] (TFluentList fluent, const TObjectBase* object) {
                        fluent.Item().Value(object->GetId());
                    });
                }));
        }

        if (key == "imported_object_count") {
            return FetchSummableAttribute(
                key,
                BIND([=, this_ = MakeStrong(this)] {
                    return ConvertToYsonString(transaction->ImportedObjects().size());
                }));
        }

        if (key == "imported_object_ids") {
            return FetchMergeableAttribute(
                key,
                BIND([=, this_ = MakeStrong(this)] {
                    return BuildYsonStringFluently().DoListFor(transaction->ImportedObjects(), [] (TFluentList fluent, const TObjectBase* object) {
                        fluent.Item().Value(object->GetId());
                    });
                }));
        }

        if (key == "exported_object_count") {
            return FetchSummableAttribute(
                key,
                BIND([=, this_ = MakeStrong(this)] {
                    return ConvertToYsonString(transaction->ExportedObjects().size());
                }));
        }

        if (key == "exported_objects") {
            return FetchMergeableAttribute(
                key,
                BIND([=, this_ = MakeStrong(this)] {
                    return BuildYsonStringFluently().DoListFor(transaction->ExportedObjects(), [] (TFluentList fluent, const TTransaction::TExportEntry& entry) {
                        fluent
                            .Item().BeginMap()
                                .Item("id").Value(entry.Object->GetId())
                                .Item("destination_cell_tag").Value(entry.DestinationCellTag)
                            .EndMap();
                    });
                }));
        }

        return Null;
    }

    // Account name -> cluster resources.
    using TAccountResourcesMap = yhash<Stroka, NSecurityServer::TClusterResources>;
    // Cell tag -> account name -> cluster resources.
    using TMulticellAccountResourcesMap = yhash<TCellTag, TAccountResourcesMap>;

    TFuture<TMulticellAccountResourcesMap> GetMulticellResourceUsageMap()
    {
        std::vector<TFuture<std::pair<TCellTag, TAccountResourcesMap>>> asyncResults;
        asyncResults.push_back(GetLocalResourcesMap(Bootstrap_->GetCellTag()));
        if (Bootstrap_->IsPrimaryMaster()) {
            for (auto cellTag : Bootstrap_->GetSecondaryCellTags()) {
                asyncResults.push_back(GetRemoteResourcesMap(cellTag));
            }
        }

        return Combine(asyncResults).Apply(BIND([] (const std::vector<std::pair<TCellTag, TAccountResourcesMap>>& results) {
            TMulticellAccountResourcesMap multicellMap;
            for (const auto& pair : results) {
                YCHECK(multicellMap.insert(pair).second);
            }
            return multicellMap;
        }));
    }

    TFuture<TAccountResourcesMap> GetAggregatedResourceUsageMap()
    {
        return GetMulticellResourceUsageMap().Apply(BIND([] (const TMulticellAccountResourcesMap& multicellMap) {
            TAccountResourcesMap aggregatedMap;
            for (const auto& cellTagAndUsageMap : multicellMap) {
                for (const auto& nameAndUsage : cellTagAndUsageMap.second) {
                    aggregatedMap[nameAndUsage.first] += nameAndUsage.second;
                }
            }
            return aggregatedMap;
        }));
    }

    TFuture<std::pair<TCellTag, TAccountResourcesMap>> GetLocalResourcesMap(TCellTag cellTag)
    {
        const auto* transaction = GetThisImpl();
        TAccountResourcesMap result;
        for (const auto& pair : transaction->AccountResourceUsage()) {
            YCHECK(result.insert(std::make_pair(pair.first->GetName(), pair.second)).second);
        }
        return MakeFuture(std::make_pair(cellTag, result));
    }

    TFuture<std::pair<TCellTag, TAccountResourcesMap>> GetRemoteResourcesMap(TCellTag cellTag)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto channel = multicellManager->GetMasterChannelOrThrow(
            cellTag,
            EPeerKind::LeaderOrFollower);

        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        auto transactionId = GetId();
        auto req = TYPathProxy::Get(FromObjectId(transactionId) + "/@resource_usage");
        batchReq->AddRequest(req);

        return batchReq->Invoke()
            .Apply(BIND([=, this_ = MakeStrong(this)] (const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError) {
                auto cumulativeError = GetCumulativeError(batchRspOrError);
                if (cumulativeError.FindMatching(NYTree::EErrorCode::ResolveError)) {
                    return std::make_pair(cellTag, TAccountResourcesMap());
                }

                THROW_ERROR_EXCEPTION_IF_FAILED(cumulativeError, "Error fetching resource usage of transaction %v from cell %v",
                    transactionId,
                    cellTag);

                const auto& batchRsp = batchRspOrError.Value();
                auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>(0);
                const auto& rsp = rspOrError.Value();
                return std::make_pair(cellTag, DeserializeAccountResourcesMap(TYsonString(rsp->value())));
            }).AsyncVia(GetCurrentInvoker()));
    }

    TAccountResourcesMap DeserializeAccountResourcesMap(const TYsonString& value)
    {
        TAccountResourcesMap result;
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto serializableAccountResources = ConvertTo<yhash<Stroka, TSerializableClusterResourcesPtr>>(value);
        for (const auto& pair : serializableAccountResources) {
            result.insert(std::make_pair(pair.first, pair.second->ToClusterResources(chunkManager)));
        }
        return result;
    }


    template <class TSession>
    TFuture<void> FetchCombinedAttributeFromRemote(
        const TIntrusivePtr<TSession>& session,
        const Stroka& attributeKey,
        TCellTag cellTag,
        const TCallback<void(const TIntrusivePtr<TSession>& session, const TYsonString& yson)>& accumulator)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        auto channel = multicellManager->FindMasterChannel(cellTag, NHydra::EPeerKind::Follower);
        if (!channel) {
            return VoidFuture;
        }

        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        auto transactionId = Object_->GetId();
        auto req = TYPathProxy::Get(FromObjectId(transactionId) + "/@" + attributeKey);
        batchReq->AddRequest(req);

        return batchReq->Invoke()
            .Apply(BIND([=, this_ = MakeStrong(this)] (const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError) {
                auto cumulativeError = GetCumulativeError(batchRspOrError);
                if (cumulativeError.FindMatching(NYTree::EErrorCode::ResolveError)) {
                    return;
                }

                THROW_ERROR_EXCEPTION_IF_FAILED(cumulativeError, "Error fetching attribute %Qv of transaction %v from cell %v",
                    attributeKey,
                    transactionId,
                    cellTag);

                const auto& batchRsp = batchRspOrError.Value();
                auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>(0);
                const auto& rsp = rspOrError.Value();
                accumulator(session, TYsonString(rsp->value()));
            }).AsyncVia(GetCurrentInvoker()));
    }

    template <class TSession>
    TFuture<TYsonString> FetchCombinedAttribute(
        const Stroka& attributeKey,
        const TCallback<TYsonString()>& localFetcher,
        const TCallback<void(const TIntrusivePtr<TSession>& session, const TYsonString& yson)>& accumulator,
        const TCallback<TYsonString(const TIntrusivePtr<TSession>& session)>& finalizer)
    {
        auto invoker = CreateSerializedInvoker(NRpc::TDispatcher::Get()->GetHeavyInvoker());

        auto session = New<TSession>();
        accumulator(session, localFetcher());

        std::vector<TFuture<void>> asyncResults;
        if (Bootstrap_->IsPrimaryMaster()) {
            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            for (auto cellTag : multicellManager->GetRegisteredMasterCellTags()) {
                asyncResults.push_back(FetchCombinedAttributeFromRemote(session, attributeKey, cellTag, accumulator));
            }
        }

        return Combine(asyncResults).Apply(BIND([=] {
            return finalizer(session);
        }));
    }


    TFuture<TYsonString> FetchMergeableAttribute(
        const Stroka& attributeKey,
        const TCallback<TYsonString()>& localFetcher)
    {
        struct TSession
            : public TIntrinsicRefCounted
        {
            yhash<Stroka, TYsonString> Map;
        };

        using TSessionPtr = TIntrusivePtr<TSession>;

        return FetchCombinedAttribute<TSession>(
            attributeKey,
            BIND([=, this_ = MakeStrong(this)] () {
                return BuildYsonStringFluently()
                    .BeginMap()
                        .Item(ToString(Bootstrap_->GetCellTag())).Value(localFetcher())
                    .EndMap();
            }),
            BIND([] (const TSessionPtr& session, const TYsonString& yson) {
                auto map = ConvertTo<yhash<Stroka, INodePtr>>(yson);
                for (const auto& pair : map) {
                    session->Map.emplace(pair.first, ConvertToYsonString(pair.second));
                }
            }),
            BIND([] (const TSessionPtr& session) {
                return BuildYsonStringFluently()
                    .DoMapFor(session->Map, [&] (TFluentMap fluent, const std::pair<const Stroka&, TYsonString>& pair) {
                        fluent.Item(pair.first).Value(pair.second);
                    });
            }));
    }

    TFuture<TYsonString> FetchSummableAttribute(
        const Stroka& attributeKey,
        const TCallback<TYsonString()>& localFetcher)
    {
        struct TSession
            : public TIntrinsicRefCounted
        {
            i64 Value = 0;
        };

        using TSessionPtr = TIntrusivePtr<TSession>;

        return FetchCombinedAttribute<TSession>(
            attributeKey,
            std::move(localFetcher),
            BIND([] (const TSessionPtr& session, const TYsonString& yson) {
                session->Value += ConvertTo<i64>(yson);
            }),
            BIND([] (const TSessionPtr& session) {
                return ConvertToYsonString(session->Value);
            }));

    }
};

IObjectProxyPtr CreateTransactionProxy(
    NCellMaster::TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTransaction* transaction)
{
    return New<TTransactionProxy>(bootstrap, metadata, transaction);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTransactionServer
} // namespace NYT

