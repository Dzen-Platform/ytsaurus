#include "raw_batch_request.h"

#include "raw_requests.h"
#include "rpc_parameters_serialization.h"

#include <mapreduce/yt/common/finally_guard.h>
#include <mapreduce/yt/common/helpers.h>
#include <mapreduce/yt/interface/logging/log.h>

#include <mapreduce/yt/interface/errors.h>
#include <mapreduce/yt/interface/serialize.h>

#include <mapreduce/yt/node/node.h>

#include <mapreduce/yt/http/retry_request.h>

#include <util/generic/guid.h>
#include <util/string/builder.h>

#include <exception>

namespace NYT {
namespace NDetail {

using NThreading::TFuture;
using NThreading::TPromise;
using NThreading::NewPromise;

////////////////////////////////////////////////////////////////////

static TString RequestInfo(const TNode& request)
{
    return TStringBuilder()
        << request["command"].AsString() << ' ' << NodeToYsonString(request["parameters"]);
}

static void EnsureNothing(const TMaybe<TNode>& node)
{
    Y_ENSURE(!node, "Internal error: expected to have no response, but got response of type " << node->GetType());
}

static void EnsureSomething(const TMaybe<TNode>& node)
{
    Y_ENSURE(node, "Internal error: expected to have response of any type, but got no response.");
}

static void EnsureType(const TNode& node, TNode::EType type)
{
    Y_ENSURE(node.GetType() == type, "Internal error: unexpected response type. "
        << "Expected: " << type << ", actual: " << node.GetType());
}

static void EnsureType(const TMaybe<TNode>& node, TNode::EType type)
{
    Y_ENSURE(node, "Internal error: expected to have response of type " << type << ", but got no response.");
    EnsureType(*node, type);
}

////////////////////////////////////////////////////////////////////

template <typename TReturnType>
class TResponseParserBase
    : public TRawBatchRequest::IResponseItemParser
{
public:
    using TFutureResult = TFuture<TReturnType>;

public:
    TResponseParserBase()
        : Result(NewPromise<TReturnType>())
    { }

    virtual void SetException(std::exception_ptr e) override
    {
        Result.SetException(std::move(e));
    }

    TFuture<TReturnType> GetFuture()
    {
        return Result.GetFuture();
    }

protected:
    TPromise<TReturnType> Result;
};

////////////////////////////////////////////////////////////////////


class TGetResponseParser
    : public TResponseParserBase<TNode>
{
public:
    virtual void SetResponse(TMaybe<TNode> node) override
    {
        EnsureSomething(node);
        Result.SetValue(std::move(*node));
    }
};

////////////////////////////////////////////////////////////////////

class TVoidResponseParser
    : public TResponseParserBase<void>
{
public:
    virtual void SetResponse(TMaybe<TNode> node) override
    {
        EnsureNothing(node);
        Result.SetValue();
    }
};

////////////////////////////////////////////////////////////////////

class TListResponseParser
    : public TResponseParserBase<TNode::TListType>
{
public:
    virtual void SetResponse(TMaybe<TNode> node) override
    {
        EnsureType(node, TNode::List);
        Result.SetValue(std::move(node->AsList()));
    }
};

////////////////////////////////////////////////////////////////////

class TExistsResponseParser
    : public TResponseParserBase<bool>
{
public:
    virtual void SetResponse(TMaybe<TNode> node) override
    {
        EnsureType(node, TNode::Bool);
        Result.SetValue(std::move(node->AsBool()));
    }
};

////////////////////////////////////////////////////////////////////

class TGuidResponseParser
    : public TResponseParserBase<TGUID>
{
public:
    virtual void SetResponse(TMaybe<TNode> node) override
    {
        EnsureType(node, TNode::String);
        Result.SetValue(GetGuid(node->AsString()));
    }
};

////////////////////////////////////////////////////////////////////

class TCanonizeYPathResponseParser
    : public TResponseParserBase<TRichYPath>
{
public:
    explicit TCanonizeYPathResponseParser(const TRichYPath& original)
        : OriginalNode_(PathToNode(original))
    { }

    virtual void SetResponse(TMaybe<TNode> node) override
    {
        EnsureType(node, TNode::String);

        for (const auto& item : OriginalNode_.GetAttributes().AsMap()) {
            node->Attributes()[item.first] = item.second;
        }
        TRichYPath result;
        Deserialize(result, *node);
        result.Path_ = AddPathPrefix(result.Path_);
        Result.SetValue(result);
    }

private:
    TNode OriginalNode_;
};

////////////////////////////////////////////////////////////////////

class TGetOperationResponseParser
    : public TResponseParserBase<TOperationAttributes>
{
public:
    virtual void SetResponse(TMaybe<TNode> node) override
    {
        EnsureType(node, TNode::Map);
        Result.SetValue(ParseOperationAttributes(*node));
    }
};

////////////////////////////////////////////////////////////////////

TRawBatchRequest::TBatchItem::TBatchItem(TNode parameters, ::TIntrusivePtr<IResponseItemParser> responseParser)
    : Parameters(std::move(parameters))
    , ResponseParser(std::move(responseParser))
    , NextTry()
{ }

TRawBatchRequest::TBatchItem::TBatchItem(const TBatchItem& batchItem, TInstant nextTry)
    : Parameters(batchItem.Parameters)
    , ResponseParser(batchItem.ResponseParser)
    , NextTry(nextTry)
{ }

////////////////////////////////////////////////////////////////////

TRawBatchRequest::TRawBatchRequest() = default;

TRawBatchRequest::~TRawBatchRequest() = default;

bool TRawBatchRequest::IsExecuted() const
{
    return Executed_;
}

void TRawBatchRequest::MarkExecuted()
{
    Executed_ = true;
}

template <typename TResponseParser>
typename TResponseParser::TFutureResult TRawBatchRequest::AddRequest(
    const TString& command,
    TNode parameters,
    TMaybe<TNode> input)
{
    return AddRequest(command, parameters, input, MakeIntrusive<TResponseParser>());
}

template <typename TResponseParser>
typename TResponseParser::TFutureResult TRawBatchRequest::AddRequest(
    const TString& command,
    TNode parameters,
    TMaybe<TNode> input,
    TIntrusivePtr<TResponseParser> parser)
{
    Y_ENSURE(!Executed_, "Cannot add request: batch request is already executed");
    TNode request;
    request["command"] = command;
    request["parameters"] = std::move(parameters);
    if (input) {
        request["input"] = std::move(*input);
    }
    BatchItemList_.emplace_back(std::move(request), parser);
    return parser->GetFuture();
}

void TRawBatchRequest::AddRequest(TBatchItem batchItem)
{
    Y_ENSURE(!Executed_, "Cannot add request: batch request is already executed");
    BatchItemList_.push_back(batchItem);
}

TFuture<TNodeId> TRawBatchRequest::Create(
    const TTransactionId& transaction,
    const TYPath& path,
    ENodeType type,
    const TCreateOptions& options)
{
    return AddRequest<TGuidResponseParser>(
        "create",
        SerializeParamsForCreate(transaction, path, type, options),
        Nothing());
}

TFuture<void> TRawBatchRequest::Remove(
    const TTransactionId& transaction,
    const TYPath& path,
    const TRemoveOptions& options)
{
    return AddRequest<TVoidResponseParser>(
        "remove",
        SerializeParamsForRemove(transaction, path, options),
        Nothing());
}

TFuture<bool> TRawBatchRequest::Exists(const TTransactionId& transaction, const TYPath& path)
{
    return AddRequest<TExistsResponseParser>(
        "exists",
        SerializeParamsForExists(transaction, path),
        Nothing());
}

TFuture<TNode> TRawBatchRequest::Get(
    const TTransactionId& transaction,
    const TYPath& path,
    const TGetOptions& options)
{
    return AddRequest<TGetResponseParser>(
        "get",
        SerializeParamsForGet(transaction, path, options),
        Nothing());
}

TFuture<void> TRawBatchRequest::Set(
    const TTransactionId& transaction,
    const TYPath& path,
    const TNode& node,
    const TSetOptions& options)
{
    return AddRequest<TVoidResponseParser>(
        "set",
        SerializeParamsForSet(transaction, path, options),
        node);
}

TFuture<TNode::TListType> TRawBatchRequest::List(
    const TTransactionId& transaction,
    const TYPath& path,
    const TListOptions& options)
{
    return AddRequest<TListResponseParser>(
        "list",
        SerializeParamsForList(transaction, path, options),
        Nothing());
}

TFuture<TNodeId> TRawBatchRequest::Copy(
    const TTransactionId& transaction,
    const TYPath& sourcePath,
    const TYPath& destinationPath,
    const TCopyOptions& options)
{
    return AddRequest<TGuidResponseParser>(
        "copy",
        SerializeParamsForCopy(transaction, sourcePath, destinationPath, options),
        Nothing());
}

TFuture<TNodeId> TRawBatchRequest::Move(
    const TTransactionId& transaction,
    const TYPath& sourcePath,
    const TYPath& destinationPath,
    const TMoveOptions& options)
{
    return AddRequest<TGuidResponseParser>(
        "move",
        SerializeParamsForMove(transaction, sourcePath, destinationPath, options),
        Nothing());
}

TFuture<TNodeId> TRawBatchRequest::Link(
    const TTransactionId& transaction,
    const TYPath& targetPath,
    const TYPath& linkPath,
    const TLinkOptions& options)
{
    return AddRequest<TGuidResponseParser>(
        "link",
        SerializeParamsForLink(transaction, targetPath, linkPath, options),
        Nothing());
}

TFuture<TLockId> TRawBatchRequest::Lock(
    const TTransactionId& transaction,
    const TYPath& path,
    ELockMode mode,
    const TLockOptions& options)
{
    return AddRequest<TGuidResponseParser>(
        "lock",
        SerializeParamsForLock(transaction, path, mode, options),
        Nothing());
}

TFuture<TOperationAttributes> TRawBatchRequest::GetOperation(
    const TOperationId& operationId,
    const TGetOperationOptions& options)
{
    return AddRequest<TGetOperationResponseParser>(
        "get_operation",
        SerializeParamsForGetOperation(operationId, options),
        Nothing());
}

TFuture<TRichYPath> TRawBatchRequest::CanonizeYPath(const TRichYPath& path)
{
    if (path.Path_.find_first_of("<>{}[]") != TString::npos) {
        return AddRequest<TCanonizeYPathResponseParser>(
            "parse_ypath",
            SerializeParamsForParseYPath(path),
            Nothing(),
            MakeIntrusive<TCanonizeYPathResponseParser>(path));
    } else {
        TRichYPath result = path;
        result.Path_ = AddPathPrefix(result.Path_);
        return NThreading::MakeFuture(result);
    }
}

void TRawBatchRequest::FillParameterList(size_t maxSize, TNode* result, TInstant* nextTry) const
{
    Y_VERIFY(result);
    Y_VERIFY(nextTry);

    *nextTry = TInstant();
    maxSize = Min(maxSize, BatchItemList_.size());
    *result = TNode::CreateList();
    for (size_t i = 0; i < maxSize; ++i) {
        LOG_DEBUG("ExecuteBatch preparing: %s", ~RequestInfo(BatchItemList_[i].Parameters));
        result->Add(BatchItemList_[i].Parameters);
        if (BatchItemList_[i].NextTry > *nextTry) {
            *nextTry = BatchItemList_[i].NextTry;
        }
    }
}

void TRawBatchRequest::ParseResponse(
    const TResponseInfo& requestResult,
    const IRetryPolicy& retryPolicy,
    TRawBatchRequest* retryBatch,
    TInstant now)
{
    TNode node = NodeFromYsonString(requestResult.Response);
    return ParseResponse(node, requestResult.RequestId, retryPolicy, retryBatch, now);
}

void TRawBatchRequest::ParseResponse(
    TNode node,
    const TString& requestId,
    const IRetryPolicy& retryPolicy,
    TRawBatchRequest* retryBatch,
    TInstant now)
{
    Y_VERIFY(retryBatch);

    EnsureType(node, TNode::List);
    auto& responseList = node.AsList();
    const auto size = responseList.size();
    Y_ENSURE(size <= BatchItemList_.size(),
        "Size of server response exceeds size of batch request;"
        " size of batch: " << BatchItemList_.size() <<
        " size of server response: " << size << '.');

    for (size_t i = 0; i != size; ++i) {
        try {
            EnsureType(responseList[i], TNode::Map);
            auto& responseNode = responseList[i].AsMap();
            const auto outputIt = responseNode.find("output");
            if (outputIt != responseNode.end()) {
                BatchItemList_[i].ResponseParser->SetResponse(std::move(outputIt->second));
            } else {
                const auto errorIt = responseNode.find("error");
                if (errorIt == responseNode.end()) {
                    BatchItemList_[i].ResponseParser->SetResponse(Nothing());
                } else {
                    TErrorResponse error(400, requestId);
                    error.SetError(TYtError(errorIt->second));
                    if (auto curInterval = retryPolicy.GetRetryInterval(error)) {
                        LOG_INFO(
                            "Batch subrequest (%s) failed, will retry, error: %s",
                            ~RequestInfo(BatchItemList_[i].Parameters),
                            error.what());
                        retryBatch->AddRequest(TBatchItem(BatchItemList_[i], now + *curInterval));
                    } else {
                        LOG_ERROR(
                            "Batch subrequest (%s) failed, error: %s",
                            ~RequestInfo(BatchItemList_[i].Parameters),
                            error.what());
                        BatchItemList_[i].ResponseParser->SetException(std::make_exception_ptr(error));
                    }
                }
            }
        } catch (const yexception& e) {
            // We don't expect other exceptions, so we don't catch (...)
            BatchItemList_[i].ResponseParser->SetException(std::current_exception());
        }
    }
    BatchItemList_.erase(BatchItemList_.begin(), BatchItemList_.begin() + size);
}

void TRawBatchRequest::SetErrorResult(std::exception_ptr e) const
{
    for (const auto& batchItem : BatchItemList_) {
        batchItem.ResponseParser->SetException(e);
    }
}

size_t TRawBatchRequest::BatchSize() const
{
    return BatchItemList_.size();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NYT
