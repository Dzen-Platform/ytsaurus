#include "virtual.h"
#include "ephemeral_attribute_owner.h"
#include "fluent.h"
#include "node_detail.h"
#include "ypath_client.h"
#include "ypath_detail.h"

#include <yt/core/misc/common.h>

#include <yt/core/ypath/tokenizer.h>

#include <yt/core/yson/tokenizer.h>
#include <yt/core/yson/writer.h>

namespace NYT {
namespace NYTree {

using namespace NRpc;
using namespace NYson;
using namespace NYPath;

////////////////////////////////////////////////////////////////////////////////

static const size_t DefaultMaxSize = 1000;

////////////////////////////////////////////////////////////////////////////////

bool TVirtualMapBase::DoInvoke(IServiceContextPtr context)
{
    DISPATCH_YPATH_SERVICE_METHOD(Get);
    DISPATCH_YPATH_SERVICE_METHOD(List);
    DISPATCH_YPATH_SERVICE_METHOD(Exists);
    return TSupportsAttributes::DoInvoke(context);
}

IYPathService::TResolveResult TVirtualMapBase::ResolveRecursive(
    const TYPath& path,
    IServiceContextPtr context)
{
    Y_UNUSED(context);

    NYPath::TTokenizer tokenizer(path);
    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::Literal);
    auto key = tokenizer.GetLiteralValue();
    auto service = FindItemService(key);
    if (!service) {
        if (context->GetMethod() == "Exists") {
            return TResolveResult::Here(path);
        }
        // TODO(babenko): improve diagnostics
        THROW_ERROR_EXCEPTION("Node has no child with key %Qv",
            ToYPathLiteral(key));
    }

    return TResolveResult::There(service, tokenizer.GetSuffix());
}

void TVirtualMapBase::GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context)
{
    YASSERT(!NYson::TTokenizer(GetRequestYPath(context)).ParseNext());

    auto attributeFilter =
        request->has_attribute_filter()
        ? NYT::FromProto<TAttributeFilter>(request->attribute_filter())
        : TAttributeFilter::None;

    int maxSize = request->has_max_size() ? request->max_size() : DefaultMaxSize;

    context->SetRequestInfo("Limit: %v", maxSize);

    auto keys = GetKeys(maxSize);
    size_t size = GetSize();

    TStringStream stream;
    TYsonWriter writer(&stream, EYsonFormat::Binary);

    if (keys.size() != size) {
        writer.OnBeginAttributes();
        writer.OnKeyedItem("incomplete");
        writer.OnStringScalar("true");
        writer.OnEndAttributes();
    }

    writer.OnBeginMap();
    for (const auto& key : keys) {
        auto service = FindItemService(key);
        if (service) {
            writer.OnKeyedItem(key);
            service->SerializeAttributes(&writer, attributeFilter, false);
            writer.OnEntity();
        }
    }
    writer.OnEndMap();

    response->set_value(stream.Str());
    context->Reply();
}

void TVirtualMapBase::ListSelf(TReqList* request, TRspList* response, TCtxListPtr context)
{
    auto attributeFilter =
        request->has_attribute_filter()
        ? NYT::FromProto<TAttributeFilter>(request->attribute_filter())
        : TAttributeFilter::None;

    int maxSize = request->has_max_size() ? request->max_size() : DefaultMaxSize;

    context->SetRequestInfo("Limit: %v", maxSize);

    auto keys = GetKeys(maxSize);
    size_t size = GetSize();

    TStringStream stream;
    TYsonWriter writer(&stream, EYsonFormat::Binary);
    BuildYsonFluently(&writer);

    if (keys.size() != size) {
        writer.OnBeginAttributes();
        writer.OnKeyedItem("incomplete");
        writer.OnStringScalar("true");
        writer.OnEndAttributes();
    }

    writer.OnBeginList();
    for (const auto& key : keys) {
        auto service = FindItemService(key);
        if (service) {
            writer.OnListItem();
            service->SerializeAttributes(&writer, attributeFilter, false);
            writer.OnStringScalar(key);
        }
    }
    writer.OnEndList();

    response->set_keys(stream.Str());
    context->Reply();
}

void TVirtualMapBase::ListSystemAttributes(std::vector<TAttributeInfo>* attributes)
{
    attributes->push_back("count");
}

bool TVirtualMapBase::GetBuiltinAttribute(const Stroka& key, IYsonConsumer* consumer)
{
    if (key == "count") {
        BuildYsonFluently(consumer)
            .Value(static_cast<i64>(GetSize()));
        return true;
    }

    return false;
}

TFuture<void> TVirtualMapBase::GetBuiltinAttributeAsync(const Stroka& key, IYsonConsumer* consumer)
{
    Y_UNUSED(key);
    Y_UNUSED(consumer);
    return Null;
}

ISystemAttributeProvider* TVirtualMapBase::GetBuiltinAttributeProvider()
{
    return this;
}

bool TVirtualMapBase::SetBuiltinAttribute(const Stroka& key, const TYsonString& value)
{
    Y_UNUSED(key);
    Y_UNUSED(value);
    return false;
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualEntityNode
    : public TNodeBase
    , public TSupportsAttributes
    , public IEntityNode
    , public TEphemeralAttributeOwner
{
    YTREE_NODE_TYPE_OVERRIDES(Entity)

public:
    explicit TVirtualEntityNode(IYPathServicePtr underlyingService)
        : UnderlyingService(underlyingService)
    { }

    virtual INodeFactoryPtr CreateFactory() const override
    {
        YASSERT(Parent);
        return Parent->CreateFactory();
    }

    virtual INodeResolverPtr GetResolver() const override
    {
        YASSERT(Parent);
        return Parent->GetResolver();
    }

    virtual ICompositeNodePtr GetParent() const override
    {
        return Parent;
    }

    virtual void SetParent(ICompositeNodePtr parent) override
    {
        Parent = parent.Get();
    }

    virtual TResolveResult Resolve(
        const TYPath& path,
        IServiceContextPtr context) override
    {
        Y_UNUSED(context);

        // TODO(babenko): handle ugly face
        return TResolveResult::There(UnderlyingService, path);
    }

    virtual void SerializeAttributes(
        IYsonConsumer* /*consumer*/,
        const TAttributeFilter& /*filter*/,
        bool /*sortKeys*/)
    { }

private:
    IYPathServicePtr UnderlyingService;
    ICompositeNode* Parent;

    // TSupportsAttributes members

    virtual IAttributeDictionary* GetCustomAttributes() override
    {
        return MutableAttributes();
    }
};

INodePtr CreateVirtualNode(IYPathServicePtr service)
{
    return New<TVirtualEntityNode>(service);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
