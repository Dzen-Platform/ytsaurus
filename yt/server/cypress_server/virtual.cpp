#include "virtual.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>

#include <yt/server/hydra/hydra_manager.h>

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>
#include <yt/server/cell_master/multicell_manager.h>
#include <yt/server/cell_master/config.h>

#include <yt/ytlib/hive/cell_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/core/ypath/tokenizer.h>
#include <yt/core/ypath/token.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/ypath_proxy.h>

#include <yt/core/yson/writer.h>
#include <yt/core/yson/async_writer.h>
#include <yt/core/yson/attribute_consumer.h>

#include <yt/core/rpc/dispatcher.h>

#include <yt/core/concurrency/scheduler.h>

namespace NYT {
namespace NCypressServer {

using namespace NRpc;
using namespace NYTree;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NCellMaster;
using namespace NTransactionServer;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NCypressClient;
using namespace NConcurrency;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

TVirtualMulticellMapBase::TVirtualMulticellMapBase(
    NCellMaster::TBootstrap* bootstrap,
    INodePtr owningNode)
    : Bootstrap_(bootstrap)
    , OwningNode_(owningNode)
{ }

bool TVirtualMulticellMapBase::DoInvoke(IServiceContextPtr context)
{
    DISPATCH_YPATH_SERVICE_METHOD(Get);
    DISPATCH_YPATH_SERVICE_METHOD(List);
    DISPATCH_YPATH_SERVICE_METHOD(Exists);
    DISPATCH_YPATH_SERVICE_METHOD(Enumerate);
    return TSupportsAttributes::DoInvoke(context);
}

IYPathService::TResolveResult TVirtualMulticellMapBase::ResolveRecursive(
    const TYPath& path,
    IServiceContextPtr context)
{
    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::Literal);

    TObjectId objectId;
    const auto& objectIdString = tokenizer.GetLiteralValue();
    if (!TObjectId::FromString(objectIdString, &objectId)) {
        THROW_ERROR_EXCEPTION("Error parsing object id %v",
            objectIdString);
    }

    auto objectManager = Bootstrap_->GetObjectManager();
    IYPathServicePtr proxy;
    if (Bootstrap_->IsPrimaryMaster() && CellTagFromId(objectId) != Bootstrap_->GetCellTag()) {
        proxy = objectManager->CreateRemoteProxy(objectId);
    } else {
        auto* object = objectManager->FindObject(objectId);
        if (IsObjectAlive(object) && IsValid(object)) {
            proxy = objectManager->GetProxy(object, nullptr);
        }
    }

    if (!proxy) {
        if (context->GetMethod() == "Exists") {
            return TResolveResult::Here(path);
        }
        THROW_ERROR_EXCEPTION(
            NYTree::EErrorCode::ResolveError,
            "No such child %Qv",
            objectId);
    }

    return TResolveResult::There(proxy, tokenizer.GetSuffix());
}

void TVirtualMulticellMapBase::GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context)
{
    YASSERT(!NYson::TTokenizer(GetRequestYPath(context)).ParseNext());

    auto attributeFilter = request->has_attribute_filter()
        ? FromProto<TAttributeFilter>(request->attribute_filter())
        : TAttributeFilter::None;

    i64 limit = request->has_limit()
        ? request->limit()
        : DefaultVirtualChildLimit;

    context->SetRequestInfo("AttributeFilterMode: %v, Limit: %v",
        attributeFilter.Mode,
        limit);

    // NB: Must deal with owning node's attributes here due to thread affinity issues.
    auto asyncOwningNodeAttributes = GetOwningNodeAttributes(attributeFilter);

    FetchItems(limit, attributeFilter)
        .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TFetchItemsSessionPtr>& sessionOrError) {
            if (!sessionOrError.IsOK()) {
                context->Reply(TError(sessionOrError));
                return;
            }

            auto owningNodeAttributesOrError = WaitFor(asyncOwningNodeAttributes);
            if (!owningNodeAttributesOrError.IsOK()) {
                context->Reply(owningNodeAttributesOrError);
                return;
            }

            const auto& owningNodeAttributes = owningNodeAttributesOrError.Value();
            const auto& session = sessionOrError.Value();

            TStringStream stream;
            TYsonWriter writer(&stream, EYsonFormat::Binary, EYsonType::Node, true);

            {
                TAsyncYsonConsumerAdapter asyncAdapter(&writer);
                TAttributeFragmentConsumer attributesConsumer(&asyncAdapter);
                attributesConsumer.OnRaw(owningNodeAttributes);
                if (session->Incomplete) {
                    attributesConsumer.OnKeyedItem("incomplete");
                    attributesConsumer.OnBooleanScalar(true);
                }
            }

            writer.OnBeginMap();
            for (const auto& item : session->Items) {
                writer.OnKeyedItem(item.Key);
                if (!item.Attributes.Data().empty()) {
                    writer.OnBeginAttributes();
                    writer.OnRaw(item.Attributes);
                    writer.OnEndAttributes();
                }
                writer.OnEntity();
            }
            writer.OnEndMap();

            const auto& str = stream.Str();
            response->set_value(str);

            context->SetRequestInfo("Count: %v, Limit: %v, ByteSize: %v",
                session->Items.size(),
                limit,
                str.length());
            context->Reply();
        }).Via(NRpc::TDispatcher::Get()->GetInvoker()));
}

void TVirtualMulticellMapBase::ListSelf(TReqList* request, TRspList* response, TCtxListPtr context)
{
    auto attributeFilter = request->has_attribute_filter()
        ? FromProto<TAttributeFilter>(request->attribute_filter())
        : TAttributeFilter::None;

    i64 limit = request->has_limit()
        ? request->limit()
        : DefaultVirtualChildLimit;

    context->SetRequestInfo("AttributeFilterMode: %v",
        attributeFilter.Mode,
        limit);

    FetchItems(limit, attributeFilter)
        .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TFetchItemsSessionPtr>& sessionOrError) {
            if (!sessionOrError.IsOK()) {
                context->Reply(TError(sessionOrError));
                return;
            }

            const auto& session = sessionOrError.Value();

            TStringStream stream;
            TYsonWriter writer(&stream, EYsonFormat::Binary, EYsonType::Node, true);

            {
                TAsyncYsonConsumerAdapter asyncAdapter(&writer);
                TAttributeFragmentConsumer attributesConsumer(&asyncAdapter);
                if (session->Incomplete) {
                    attributesConsumer.OnKeyedItem("incomplete");
                    attributesConsumer.OnBooleanScalar(true);
                }
            }

            writer.OnBeginList();
            for (const auto& item : session->Items) {
                writer.OnListItem();
                if (!item.Attributes.Data().empty()) {
                    writer.OnBeginAttributes();
                    writer.OnRaw(item.Attributes);
                    writer.OnEndAttributes();
                }
                writer.OnStringScalar(item.Key);
            }
            writer.OnEndList();

            const auto& str = stream.Str();
            response->set_value(str);

            context->SetRequestInfo("Count: %v, Limit: %v, ByteSize: %v",
                session->Items.size(),
                limit,
                str.length());
            context->Reply();
        }).Via(NRpc::TDispatcher::Get()->GetInvoker()));
}

void TVirtualMulticellMapBase::ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors)
{
    descriptors->push_back(TAttributeDescriptor("count")
        .SetOpaque(true));
    descriptors->push_back(TAttributeDescriptor("multicell_count")
        .SetOpaque(true));
}

bool TVirtualMulticellMapBase::GetBuiltinAttribute(const Stroka& /*key*/, IYsonConsumer* /*consumer*/)
{
    return false;
}

TFuture<TYsonString> TVirtualMulticellMapBase::GetBuiltinAttributeAsync(const Stroka& key)
{
    if (key == "count") {
        return FetchSizes().Apply(BIND([] (const std::vector<std::pair<TCellTag, i64>>& multicellSizes) {
            i64 result = 0;
            for (const auto& pair : multicellSizes) {
                result += pair.second;
            }
            return ConvertToYsonString(result);
        }));
    }

    if (key == "multicell_count") {
        return FetchSizes().Apply(BIND([] (const std::vector<std::pair<TCellTag, i64>>& multicellSizes) {
            return BuildYsonStringFluently().DoMapFor(multicellSizes, [] (TFluentMap fluent, const std::pair<TCellTag, i64>& pair) {
                fluent.Item(ToString(pair.first)).Value(pair.second);
            });
        }));
    }

    return Null;
}

ISystemAttributeProvider* TVirtualMulticellMapBase::GetBuiltinAttributeProvider()
{
    return this;
}

bool TVirtualMulticellMapBase::SetBuiltinAttribute(const Stroka& /*key*/, const TYsonString& /*value*/)
{
    return false;
}

TFuture<void> TVirtualMulticellMapBase::SetBuiltinAttributeAsync(const Stroka& /*key*/, const TYsonString& /*value*/)
{
    return Null;
}

bool TVirtualMulticellMapBase::RemoveBuiltinAttribute(const Stroka& /*key*/)
{
    return false;
}

TFuture<std::vector<std::pair<TCellTag, i64>>> TVirtualMulticellMapBase::FetchSizes()
{
    std::vector<TFuture<std::pair<TCellTag, i64>>> asyncResults{
        MakeFuture(std::make_pair(Bootstrap_->GetCellTag(), GetSize()))
    };

    if (Bootstrap_->IsPrimaryMaster()) {
        auto multicellManager = Bootstrap_->GetMulticellManager();
        for (auto cellTag : multicellManager->GetRegisteredMasterCellTags()) {
            auto channel = multicellManager->FindMasterChannel(cellTag, NHydra::EPeerKind::LeaderOrFollower);
            if (!channel)
                continue;

            TObjectServiceProxy proxy(channel);
            auto batchReq = proxy.ExecuteBatch();
            batchReq->SetSuppressUpstreamSync(true);

            auto path = GetWellKnownPath();
            auto req = TYPathProxy::Get(path + "/@count");
            batchReq->AddRequest(req, "get_count");

            auto asyncResult = batchReq->Invoke()
                .Apply(BIND([=, this_ = MakeStrong(this)] (const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError) {
                    auto cumulativeError = GetCumulativeError(batchRspOrError);
                    if (!cumulativeError.IsOK()) {
                        THROW_ERROR_EXCEPTION("Error fetching size of virtual map %v from cell %v",
                            path,
                            cellTag)
                            << cumulativeError;
                    }

                    const auto& batchRsp = batchRspOrError.Value();

                    auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_count");
                    const auto& rsp = rspOrError.Value();
                    return std::make_pair(cellTag, ConvertTo<i64>(TYsonString(rsp->value())));
                }));

            asyncResults.push_back(asyncResult);
        }
    }

    return Combine(asyncResults);
}

TFuture<TVirtualMulticellMapBase::TFetchItemsSessionPtr> TVirtualMulticellMapBase::FetchItems(
    i64 limit,
    const TAttributeFilter& attributeFilter)
{
    auto multicellManager = Bootstrap_->GetMulticellManager();

    auto session = New<TFetchItemsSession>();
    session->Limit = limit;
    session->AttributeFilter = attributeFilter;
    session->CellTags = multicellManager->GetRegisteredMasterCellTags();

    auto promise = NewPromise<TFetchItemsSessionPtr>();
    FetchItemsFromAnywhere(session, promise);

    return promise.ToFuture();
}

void TVirtualMulticellMapBase::FetchItemsFromAnywhere(
    TVirtualMulticellMapBase::TFetchItemsSessionPtr session,
    TPromise<TFetchItemsSessionPtr> promise)
{
    if (promise.IsSet())
        return;

    if (session->CellTagIndex >= (int) session->CellTags.size() ||
        session->Items.size() >= session->Limit)
    {
        promise.Set(session);
    } else if (session->CellTagIndex < 0) {
        FetchItemsFromLocal(session, promise);
    } else {
        FetchItemsFromRemote(session, promise);
    }
}

void TVirtualMulticellMapBase::FetchItemsFromLocal(
    TVirtualMulticellMapBase::TFetchItemsSessionPtr session,
    TPromise<TFetchItemsSessionPtr> promise)
{
    auto keys = GetKeys(session->Limit);
    session->Incomplete |= (keys.size() == session->Limit);

    auto objectManager = Bootstrap_->GetObjectManager();

    std::vector<TFuture<TYsonString>> asyncAttributes;
    for (const auto& key : keys) {
        auto* object = objectManager->FindObject(key);
        if (IsObjectAlive(object)) {
            TFetchItem item;
            item.Key = ToString(key);
            if (session->AttributeFilter.Mode != EAttributeFilterMode::None) {
                TAsyncYsonWriter writer(EYsonType::MapFragment);
                auto proxy = objectManager->GetProxy(object, nullptr);
                proxy->WriteAttributesFragment(&writer, session->AttributeFilter, false);
                asyncAttributes.emplace_back(writer.Finish());
            } else {
                static const auto EmptyFragment = MakeFuture(TYsonString(Stroka(), EYsonType::MapFragment));
                asyncAttributes.push_back(EmptyFragment);
            }
            session->Items.push_back(item);
        }
    }

    Combine(asyncAttributes)
        .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<std::vector<TYsonString>>& errorOrAttributes) mutable {
            if (!errorOrAttributes.IsOK()) {
                promise.Set(errorOrAttributes);
                return;
            }

            const auto& attributes = errorOrAttributes.Value();
            YCHECK(session->Items.size() == attributes.size());
            for (int index = 0; index < session->Items.size(); ++index) {
                session->Items[index].Attributes = attributes[index];
            }

            // Proceed to remotes.
            session->CellTagIndex = 0;
            FetchItemsFromAnywhere(session, promise);
        }));
}

void TVirtualMulticellMapBase::FetchItemsFromRemote(
    TVirtualMulticellMapBase::TFetchItemsSessionPtr session,
    TPromise<TFetchItemsSessionPtr> promise)
{
    auto cellTag = session->CellTags[session->CellTagIndex++];
    auto multicellManager = Bootstrap_->GetMulticellManager();
    auto channel = multicellManager->FindMasterChannel(cellTag, NHydra::EPeerKind::LeaderOrFollower);
    if (!channel) {
        FetchItemsFromAnywhere(session, promise);
        return;
    }

    TObjectServiceProxy proxy(channel);
    auto batchReq = proxy.ExecuteBatch();
    batchReq->SetSuppressUpstreamSync(true);

    auto path = GetWellKnownPath();
    auto req = TCypressYPathProxy::Enumerate(path);
    req->set_limit(session->Limit - session->Items.size());
    ToProto(req->mutable_attribute_filter(), session->AttributeFilter);
    batchReq->AddRequest(req, "enumerate");

    batchReq->Invoke()
        .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError) mutable {
            auto cumulativeError = GetCumulativeError(batchRspOrError);
            if (!cumulativeError.IsOK()) {
                promise.Set(TError("Error fetching content of virtual map %v from cell %v",
                    path,
                    cellTag)
                    << cumulativeError);
                return;
            }

            const auto& batchRsp = batchRspOrError.Value();

            auto rspOrError = batchRsp->GetResponse<TCypressYPathProxy::TRspEnumerate>("enumerate");
            const auto& rsp = rspOrError.Value();

            session->Incomplete |= rsp->incomplete();
            for (const auto& protoItem : rsp->items()) {
                TFetchItem item;
                item.Key = protoItem.key();
                if (protoItem.has_attributes()) {
                    item.Attributes = TYsonString(protoItem.attributes(), EYsonType::MapFragment);
                }
                session->Items.push_back(item);
            }

            // Proceed to the next remote.
            FetchItemsFromAnywhere(session, promise);
        }).Via(NRpc::TDispatcher::Get()->GetInvoker()));
}

TFuture<TYsonString> TVirtualMulticellMapBase::GetOwningNodeAttributes(const TAttributeFilter& attributeFilter)
{
    TAsyncYsonWriter writer(EYsonType::MapFragment);
    if (OwningNode_) {
        OwningNode_->WriteAttributesFragment(&writer, attributeFilter, false);
    }
    return writer.Finish();
}

DEFINE_YPATH_SERVICE_METHOD(TVirtualMulticellMapBase, Enumerate)
{
    // XXX(babenko): remove this after updating multicell cluster
    if (NHydra::HasMutationContext()) {
        context->Reply();
        return;
    }

    auto attributeFilter = request->has_attribute_filter()
        ? FromProto<TAttributeFilter>(request->attribute_filter())
        : TAttributeFilter::None;

    i64 limit = request->limit();

    context->SetRequestInfo("AttributeFilterMode: %v, Limit: %v",
        attributeFilter.Mode,
        limit);

    auto keys = GetKeys(limit);

    auto objectManager = Bootstrap_->GetObjectManager();

    std::vector<TFuture<TYsonString>> asyncValues;
    for (const auto& key : keys) {
        auto* object = objectManager->FindObject(key);
        if (IsObjectAlive(object)) {
            auto* protoItem = response->add_items();
            protoItem->set_key(ToString(key));
            if (attributeFilter.Mode != EAttributeFilterMode::None) {
                TAsyncYsonWriter writer(EYsonType::MapFragment);
                auto proxy = objectManager->GetProxy(object, nullptr);
                proxy->WriteAttributesFragment(&writer, attributeFilter, false);
                asyncValues.push_back(writer.Finish());
            }
        }
    }

    response->set_incomplete(response->items_size() == limit);

    Combine(asyncValues)
        .Subscribe(BIND([=] (const TErrorOr<std::vector<TYsonString>>& valuesOrError) {
            if (!valuesOrError.IsOK()) {
                context->Reply(valuesOrError);
                return;
            }

            const auto& values = valuesOrError.Value();
            YCHECK(response->items_size() == values.size());
            for (int index = 0; index < response->items_size(); ++index) {
                const auto& value = values[index];
                if (!value.Data().empty()) {
                    response->mutable_items(index)->set_attributes(value.Data());
                }
            }

            context->SetResponseInfo("Count: %v, Incomplete: %v",
                response->items_size(),
                response->incomplete());
            context->Reply();
        }));
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualNode
    : public TCypressNodeBase
{
public:
    explicit TVirtualNode(const TVersionedNodeId& id)
        : TCypressNodeBase(id)
    { }

    virtual ENodeType GetNodeType() const override
    {
        return ENodeType::Entity;
    }

};

////////////////////////////////////////////////////////////////////////////////

class TVirtualNodeProxy
    : public TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, IEntityNode, TVirtualNode>
{
public:
    TVirtualNodeProxy(
        INodeTypeHandlerPtr typeHandler,
        TBootstrap* bootstrap,
        TTransaction* transaction,
        TVirtualNode* trunkNode,
        EVirtualNodeOptions options,
        TYPathServiceProducer producer)
        : TBase(
            typeHandler,
            bootstrap,
            transaction,
            trunkNode)
        , Options_(options)
        , Producer_(producer)
    { }

private:
    typedef TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, IEntityNode, TVirtualNode> TBase;

    const EVirtualNodeOptions Options_;
    const TYPathServiceProducer Producer_;


    virtual TResolveResult ResolveSelf(const TYPath& path, IServiceContextPtr context) override
    {
        auto service = GetService();
        const auto& method = context->GetMethod();
        if ((Options_ & EVirtualNodeOptions::RedirectSelf) != EVirtualNodeOptions::None &&
            method != "Remove" &&
            method != "GetBasicAttributes")
        {
            return TResolveResult::There(service, path);
        } else {
            return TBase::ResolveSelf(path, context);
        }
    }

    virtual TResolveResult ResolveRecursive(const TYPath& path, IServiceContextPtr context) override
    {
        auto service = GetService();
        NYPath::TTokenizer tokenizer(path);
        switch (tokenizer.Advance()) {
            case NYPath::ETokenType::EndOfStream:
            case NYPath::ETokenType::Slash:
                return TResolveResult::There(service, path);
            default:
                return TResolveResult::There(service, "/" + path);
        }
    }


    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override
    {
        auto service = GetService();
        auto* provider = GetTargetBuiltinAttributeProvider(service);
        if (provider) {
            provider->ListSystemAttributes(descriptors);
        }

        TBase::ListSystemAttributes(descriptors);
    }

    virtual bool GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        auto service = GetService();
        auto* provider = GetTargetBuiltinAttributeProvider(service);
        if (provider && provider->GetBuiltinAttribute(key, consumer)) {
            return true;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    virtual TFuture<TYsonString> GetBuiltinAttributeAsync(const Stroka& key) override
    {
        auto service = GetService();
        auto* provider = GetTargetBuiltinAttributeProvider(service);
        if (provider) {
            auto result = provider->GetBuiltinAttributeAsync(key);
            if (result) {
                return result;
            }
        }

        return TBase::GetBuiltinAttributeAsync(key);
    }

    virtual bool SetBuiltinAttribute(const Stroka& key, const TYsonString& value) override
    {
        auto service = GetService();
        auto* provider = GetTargetBuiltinAttributeProvider(service);
        if (provider && provider->SetBuiltinAttribute(key, value)) {
            return true;
        }

        return TBase::SetBuiltinAttribute(key, value);
    }

    virtual TFuture<void> SetBuiltinAttributeAsync(const Stroka& key, const TYsonString& value) override
    {
        auto service = GetService();
        auto* provider = GetTargetBuiltinAttributeProvider(service);
        if (provider) {
            auto result = provider->SetBuiltinAttributeAsync(key, value);
            if (result) {
                return result;
            }
        }

        return TBase::SetBuiltinAttributeAsync(key, value);
    }

    virtual bool IsLeaderReadRequired() const override
    {
        return Any(Options_ & EVirtualNodeOptions::RequireLeader);
    }


    static ISystemAttributeProvider* GetTargetBuiltinAttributeProvider(IYPathServicePtr service)
    {
        return dynamic_cast<ISystemAttributeProvider*>(service.Get());
    }

    IYPathServicePtr GetService()
    {
        return Producer_.Run(this);
    }

};

////////////////////////////////////////////////////////////////////////////////

class TVirtualNodeTypeHandler
    : public TCypressNodeTypeHandlerBase<TVirtualNode>
{
public:
    TVirtualNodeTypeHandler(
        TBootstrap* bootstrap,
        TYPathServiceProducer producer,
        EObjectType objectType,
        EVirtualNodeOptions options)
        : TCypressNodeTypeHandlerBase<TVirtualNode>(bootstrap)
        , Producer_(producer)
        , ObjectType_(objectType)
        , Options_(options)
    { }

    virtual EObjectType GetObjectType() override
    {
        return ObjectType_;
    }

    virtual ENodeType GetNodeType() override
    {
        return ENodeType::Entity;
    }

private:
    const TYPathServiceProducer Producer_;
    const EObjectType ObjectType_;
    const EVirtualNodeOptions Options_;


    virtual ICypressNodeProxyPtr DoGetProxy(
        TVirtualNode* trunkNode,
        TTransaction* transaction) override
    {
        return New<TVirtualNodeProxy>(
            this,
            Bootstrap_,
            transaction,
            trunkNode,
            Options_,
            Producer_);
    }

};

INodeTypeHandlerPtr CreateVirtualTypeHandler(
    TBootstrap* bootstrap,
    EObjectType objectType,
    TYPathServiceProducer producer,
    EVirtualNodeOptions options)
{
    return New<TVirtualNodeTypeHandler>(
        bootstrap,
        producer,
        objectType,
        options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
