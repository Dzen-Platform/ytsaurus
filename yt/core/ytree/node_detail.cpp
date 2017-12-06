#include "node_detail.h"
//#include "convert.h"
//#include "tree_builder.h"
//#include "tree_visitor.h"
//#include "ypath_client.h"
//#include "ypath_detail.h"
//#include "ypath_service.h"

#include <yt/core/misc/singleton.h>

#include <yt/core/ypath/token.h>
#include <yt/core/ypath/tokenizer.h>

#include <yt/core/yson/tokenizer.h>
#include <yt/core/yson/async_writer.h>

namespace NYT {
namespace NYTree {

using namespace NRpc;
using namespace NYPath;
using namespace NYson;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

bool TNodeBase::DoInvoke(const IServiceContextPtr& context)
{
    DISPATCH_YPATH_SERVICE_METHOD(GetKey);
    DISPATCH_YPATH_SERVICE_METHOD(Get);
    DISPATCH_YPATH_SERVICE_METHOD(Set);
    DISPATCH_YPATH_SERVICE_METHOD(Remove);
    DISPATCH_YPATH_SERVICE_METHOD(List);
    DISPATCH_YPATH_SERVICE_METHOD(Exists);
    return TYPathServiceBase::DoInvoke(context);
}

void TNodeBase::GetSelf(
    TReqGet* request,
    TRspGet* response,
    const TCtxGetPtr& context)
{
    auto attributeKeys = request->has_attributes()
        ? MakeNullable(FromProto<std::vector<TString>>(request->attributes().keys()))
        : Null;

    // TODO(babenko): make use of limit
    auto limit = request->has_limit()
        ? MakeNullable(request->limit())
        : Null;

    context->SetRequestInfo("Limit: %v", limit);

    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);

    TAsyncYsonWriter writer;

    VisitTree(
        this,
        &writer,
        attributeKeys,
        false);

    writer.Finish().Subscribe(BIND([=] (const TErrorOr<TYsonString>& resultOrError) {
        if (resultOrError.IsOK()) {
            response->set_value(resultOrError.Value().GetData());
            context->Reply();
        } else {
            context->Reply(resultOrError);
        }
    }));
}

void TNodeBase::GetKeySelf(
    TReqGetKey* /*request*/,
    TRspGetKey* response,
    const TCtxGetKeyPtr& context)
{
    context->SetRequestInfo();

    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);

    auto parent = GetParent();
    if (!parent) {
        THROW_ERROR_EXCEPTION("Node has no parent");
    }

    TString key;
    switch (parent->GetType()) {
        case ENodeType::Map:
            key = parent->AsMap()->GetChildKey(this);
            break;

        case ENodeType::List:
            key = ToString(parent->AsList()->GetChildIndex(this));
            break;

        default:
            Y_UNREACHABLE();
    }

    context->SetResponseInfo("Key: %v", key);
    response->set_value(ConvertToYsonString(key).GetData());

    context->Reply();
}

void TNodeBase::RemoveSelf(
    TReqRemove* request,
    TRspRemove* /*response*/,
    const TCtxRemovePtr& context)
{
    context->SetRequestInfo();

    auto parent = GetParent();
    if (!parent) {
        ThrowCannotRemoveRoot();
    }

    ValidatePermission(
        EPermissionCheckScope::This | EPermissionCheckScope::Descendants,
        EPermission::Remove);
    ValidatePermission(
        EPermissionCheckScope::Parent,
        EPermission::Write);

    bool isComposite = (GetType() == ENodeType::Map || GetType() == ENodeType::List);
    if (!request->recursive() && isComposite && AsComposite()->GetChildCount() > 0) {
        THROW_ERROR_EXCEPTION("Cannot remove non-empty composite node");
    }

    parent->AsComposite()->RemoveChild(this);

    context->Reply();
}

IYPathService::TResolveResult TNodeBase::ResolveRecursive(
    const NYPath::TYPath& path,
    const IServiceContextPtr& context)
{
    if (context->GetMethod() == "Exists") {
        return TResolveResultHere{path};
    }

    ThrowCannotHaveChildren(this);
    Y_UNREACHABLE();
}

TYPath TNodeBase::GetPath() const
{
    SmallVector<TString, 64> tokens;
    IConstNodePtr current(this);
    while (true) {
        auto parent = current->GetParent();
        if (!parent) {
            break;
        }
        TString token;
        switch (parent->GetType()) {
            case ENodeType::List: {
                auto index = parent->AsList()->GetChildIndex(current);
                token = ToYPathLiteral(index);
                break;
            }
            case ENodeType::Map: {
                auto key = parent->AsMap()->GetChildKey(current);
                token = ToYPathLiteral(key);
                break;
            }
            default:
                Y_UNREACHABLE();
        }
        tokens.emplace_back(std::move(token));
        current = parent;
    }

    TStringBuilder builder;
    for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
        builder.AppendChar('/');
        builder.AppendString(*it);
    }
    return builder.Flush();
}

////////////////////////////////////////////////////////////////////////////////

void TCompositeNodeMixin::SetRecursive(
    const TYPath& path,
    TReqSet* request,
    TRspSet* /*response*/,
    const TCtxSetPtr& context)
{
    context->SetRequestInfo();

    ValidatePermission(EPermissionCheckScope::This, EPermission::Write);

    auto factory = CreateFactory();
    auto child = ConvertToNode(TYsonString(request->value()), factory.get());
    SetChild(factory.get(), "/" + path, child, request->recursive());
    factory->Commit();

    context->Reply();
}

void TCompositeNodeMixin::RemoveRecursive(
    const TYPath& path,
    TSupportsRemove::TReqRemove* request,
    TSupportsRemove::TRspRemove* /*response*/,
    const TSupportsRemove::TCtxRemovePtr& context)
{
    context->SetRequestInfo();

    NYPath::TTokenizer tokenizer(path);
    if (tokenizer.Advance() == NYPath::ETokenType::Asterisk) {
        tokenizer.Advance();
        tokenizer.Expect(NYPath::ETokenType::EndOfStream);

        ValidatePermission(EPermissionCheckScope::This, EPermission::Write);
        ValidatePermission(EPermissionCheckScope::Descendants, EPermission::Remove);
        Clear();

        context->Reply();
    } else if (request->force()) {
        // There is no child node under the given path, so there is nothing to remove.
        context->Reply();
    } else {
        ThrowNoSuchChildKey(this, tokenizer.GetLiteralValue());
    }
}

int TCompositeNodeMixin::GetMaxChildCount() const
{
    return std::numeric_limits<int>::max();
}

////////////////////////////////////////////////////////////////////////////////

IYPathService::TResolveResult TMapNodeMixin::ResolveRecursive(
    const TYPath& path,
    const IServiceContextPtr& context)
{
    const auto& method = context->GetMethod();

    NYPath::TTokenizer tokenizer(path);
    switch (tokenizer.Advance()) {
        case NYPath::ETokenType::Asterisk: {
            if (method != "Remove") {
                THROW_ERROR_EXCEPTION("\"*\" is only allowed for Remove method");
            }

            tokenizer.Advance();
            tokenizer.Expect(NYPath::ETokenType::EndOfStream);

            return IYPathService::TResolveResultHere{"/" + path};
        }

        case NYPath::ETokenType::Literal: {
            auto key = tokenizer.GetLiteralValue();
            if (key.Empty()) {
                THROW_ERROR_EXCEPTION("Child key cannot be empty");
            }

            auto suffix = TYPath(tokenizer.GetSuffix());

            auto child = FindChild(key);
            if (!child) {
                if (method == "Exists" ||
                    method == "Create" ||
                    method == "Copy" ||
                    method == "Remove" ||
                    method == "Set")
                {
                    return IYPathService::TResolveResultHere{"/" + path};
                } else {
                    ThrowNoSuchChildKey(this, key);
                }
            }

            return IYPathService::TResolveResultThere{std::move(child), std::move(suffix)};
        }

        default:
            tokenizer.ThrowUnexpected();
            Y_UNREACHABLE();
    }
}

void TMapNodeMixin::ListSelf(
    TReqList* request,
    TRspList* response,
    const TCtxListPtr& context)
{
    ValidatePermission(EPermissionCheckScope::This, EPermission::Read);

    auto attributeKeys = request->has_attributes()
        ? MakeNullable(FromProto<std::vector<TString>>(request->attributes().keys()))
        : Null;

    auto limit = request->has_limit()
        ? MakeNullable(request->limit())
        : Null;

    context->SetRequestInfo("Limit: %v", limit);

    TAsyncYsonWriter writer;

    auto children = GetChildren();
    if (limit && children.size() > *limit) {
        writer.OnBeginAttributes();
        writer.OnKeyedItem("incomplete");
        writer.OnBooleanScalar(true);
        writer.OnEndAttributes();
    }

    i64 counter = 0;

    writer.OnBeginList();
    for (const auto& pair : children) {
        const auto& key = pair.first;
        const auto& node = pair.second;
        writer.OnListItem();
        node->WriteAttributes(&writer, attributeKeys, false);
        writer.OnStringScalar(key);
        if (limit && ++counter >= *limit) {
            break;
        }
    }
    writer.OnEndList();

    writer.Finish().Subscribe(BIND([=] (const TErrorOr<TYsonString>& resultOrError) {
        if (resultOrError.IsOK()) {
            response->set_value(resultOrError.Value().GetData());
            context->Reply();
        } else {
            context->Reply(resultOrError);
        }
    }));
}

void TMapNodeMixin::SetChild(
    INodeFactory* factory,
    const TYPath& path,
    INodePtr child,
    bool recursive)
{
    NYPath::TTokenizer tokenizer(path);
    if (tokenizer.Advance() == NYPath::ETokenType::EndOfStream) {
        tokenizer.ThrowUnexpected();
    }

    IMapNodePtr rootNode = AsMap();
    INodePtr rootChild;
    TString rootKey;

    auto currentNode = rootNode;
    try {
        while (tokenizer.GetType() != NYPath::ETokenType::EndOfStream) {
            tokenizer.Skip(NYPath::ETokenType::Ampersand);
            tokenizer.Expect(NYPath::ETokenType::Slash);

            tokenizer.Advance();
            tokenizer.Expect(NYPath::ETokenType::Literal);
            auto key = tokenizer.GetLiteralValue();

            int maxKeyLength = GetMaxKeyLength();
            if (key.length() > maxKeyLength) {
                THROW_ERROR_EXCEPTION(
                    NYTree::EErrorCode::MaxKeyLengthViolation,
                    "Map node %v is not allowed to contain items with keys longer than %v symbols",
                    GetPath(),
                    maxKeyLength);
            }

            tokenizer.Advance();

            bool lastStep = (tokenizer.GetType() == NYPath::ETokenType::EndOfStream);
            if (!recursive && !lastStep) {
                THROW_ERROR_EXCEPTION("%v has no child %Qv; consider using \"recursive\" option to force its creation",
                    currentNode->GetPath(),
                    key);
            }

            int maxChildCount = GetMaxChildCount();
            if (currentNode->GetChildCount() >= maxChildCount) {
                THROW_ERROR_EXCEPTION(
                    NYTree::EErrorCode::MaxChildCountViolation,
                    "Map node %v is not allowed to contain more than %v items",
                    GetPath(),
                    maxChildCount);
            }

            auto newChild = lastStep ? child : factory->CreateMap();
            if (currentNode != rootNode) {
                YCHECK(currentNode->AddChild(newChild, key));
            } else {
                rootChild = newChild;
                rootKey = key;
            }

            if (!lastStep) {
                currentNode = newChild->AsMap();
            }
        }
    } catch (const TErrorException& ex) {
        if (recursive) {
            THROW_ERROR_EXCEPTION("Failed to set node recursively")
                << ex.Error();
        } else {
            throw;
        }
    }

    YCHECK(rootKey);
    rootNode->AddChild(rootChild, rootKey);
}

int TMapNodeMixin::GetMaxKeyLength() const
{
    return std::numeric_limits<int>::max();
}

////////////////////////////////////////////////////////////////////////////////

IYPathService::TResolveResult TListNodeMixin::ResolveRecursive(
    const TYPath& path,
    const IServiceContextPtr& context)
{
    NYPath::TTokenizer tokenizer(path);
    switch (tokenizer.Advance()) {
        case NYPath::ETokenType::Asterisk: {
            tokenizer.Advance();
            tokenizer.Expect(NYPath::ETokenType::EndOfStream);

            return IYPathService::TResolveResultHere{"/" + path};
        }

        case NYPath::ETokenType::Literal: {
            const auto& token = tokenizer.GetToken();
            if (token == ListBeginToken ||
                token == ListEndToken)
            {
                tokenizer.Advance();
                tokenizer.Expect(NYPath::ETokenType::EndOfStream);

                return IYPathService::TResolveResultHere{"/" + path};
            } else if (token.StartsWith(ListBeforeToken) ||
                       token.StartsWith(ListAfterToken))
            {
                auto indexToken = ExtractListIndex(token);
                int index = ParseListIndex(indexToken);
                AdjustChildIndex(index);

                tokenizer.Advance();
                tokenizer.Expect(NYPath::ETokenType::EndOfStream);

                return IYPathService::TResolveResultHere{"/" + path};
            } else {
                int index = ParseListIndex(token);
                int adjustedIndex = AdjustChildIndex(index);
                auto child = FindChild(adjustedIndex);
                const auto& method = context->GetMethod();
                if (!child && method == "Exists") {
                    return IYPathService::TResolveResultHere{"/" + path};
                }

                return IYPathService::TResolveResultThere{std::move(child), TYPath(tokenizer.GetSuffix())};
            }
        }

        default:
            tokenizer.ThrowUnexpected();
            Y_UNREACHABLE();
    }
}

void TListNodeMixin::SetChild(
    INodeFactory* /*factory*/,
    const TYPath& path,
    INodePtr child,
    bool recursive)
{
    if (recursive) {
        THROW_ERROR_EXCEPTION("List node %v does not support \"recursive\" option",
            GetPath());
    }

    int beforeIndex = -1;

    NYPath::TTokenizer tokenizer(path);

    tokenizer.Advance();
    tokenizer.Skip(NYPath::ETokenType::Ampersand);
    tokenizer.Expect(NYPath::ETokenType::Slash);

    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::Literal);

    const auto& token = tokenizer.GetToken();
    if (token.StartsWith(ListBeginToken)) {
        beforeIndex = 0;
    } else if (token.StartsWith(ListEndToken)) {
        beforeIndex = GetChildCount();
    } else if (token.StartsWith(ListBeforeToken) || token.StartsWith(ListAfterToken)) {
        auto indexToken = ExtractListIndex(token);
        int index = ParseListIndex(indexToken);
        beforeIndex = AdjustChildIndex(index);
        if (token.StartsWith(ListAfterToken)) {
            ++beforeIndex;
        }
    } else {
        tokenizer.ThrowUnexpected();
    }

    tokenizer.Advance();
    tokenizer.Expect(NYPath::ETokenType::EndOfStream);

    int maxChildCount = GetMaxChildCount();
    if (GetChildCount() >= maxChildCount) {
        THROW_ERROR_EXCEPTION(
            NYTree::EErrorCode::MaxChildCountViolation,
            "List node %v is not allowed to contain more than %v items",
            GetPath(),
            maxChildCount);
    }

    AddChild(child, beforeIndex);
}

////////////////////////////////////////////////////////////////////////////////

IYPathServicePtr TNonexistingService::Get()
{
    return RefCountedSingleton<TNonexistingService>();
}

bool TNonexistingService::DoInvoke(const IServiceContextPtr& context)
{
    DISPATCH_YPATH_SERVICE_METHOD(Exists);
    return TYPathServiceBase::DoInvoke(context);
}

IYPathService::TResolveResult TNonexistingService::Resolve(
    const TYPath& path,
    const IServiceContextPtr& /*context*/)
{
    return TResolveResultHere{path};
}

void TNonexistingService::ExistsSelf(
    TReqExists* /*request*/,
    TRspExists* /*response*/,
    const TCtxExistsPtr& context)
{
    ExistsAny(context);
}

void TNonexistingService::ExistsRecursive(
    const TYPath& /*path*/,
    TReqExists* /*request*/,
    TRspExists* /*response*/,
    const TCtxExistsPtr& context)
{
    ExistsAny(context);
}

void TNonexistingService::ExistsAttribute(
    const TYPath& /*path*/,
    TReqExists* /*request*/,
    TRspExists* /*response*/,
    const TCtxExistsPtr& context)
{
    ExistsAny(context);
}

void TNonexistingService::ExistsAny(const TCtxExistsPtr& context)
{
    context->SetRequestInfo();
    Reply(context, false);
}

////////////////////////////////////////////////////////////////////////////////

TTransactionalNodeFactoryBase::~TTransactionalNodeFactoryBase()
{
    YCHECK(State_ == EState::Committed || State_ == EState::RolledBack);
}

void TTransactionalNodeFactoryBase::Commit() noexcept
{
    YCHECK(State_ == EState::Active);
    State_ = EState::Committed;
}

void TTransactionalNodeFactoryBase::Rollback() noexcept
{
    YCHECK(State_ == EState::Active);
    State_ = EState::RolledBack;
}

void TTransactionalNodeFactoryBase::RollbackIfNeeded()
{
    if (State_ == EState::Active) {
        Rollback();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

