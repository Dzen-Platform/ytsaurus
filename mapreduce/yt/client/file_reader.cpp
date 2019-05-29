#include "file_reader.h"

#include "transaction.h"

#include <mapreduce/yt/common/config.h>
#include <mapreduce/yt/common/helpers.h>
#include <mapreduce/yt/common/retry_lib.h>
#include <mapreduce/yt/common/wait_proxy.h>

#include <mapreduce/yt/interface/logging/log.h>

#include <mapreduce/yt/io/helpers.h>

#include <mapreduce/yt/http/http.h>
#include <mapreduce/yt/http/retry_request.h>

#include <mapreduce/yt/raw_client/raw_requests.h>

namespace NYT {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

static TMaybe<ui64> GetEndOffset(const TFileReaderOptions& options) {
    if (options.Length_) {
        return options.Offset_.GetOrElse(0) + *options.Length_;
    } else {
        return Nothing();
    }
}

////////////////////////////////////////////////////////////////////////////////

TStreamReaderBase::TStreamReaderBase(
    IClientRetryPolicyPtr clientRetryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId)
    : ClientRetryPolicy_(std::move(clientRetryPolicy))
    , Auth_(auth)
    , ReadTransaction_(MakeHolder<TPingableTransaction>(auth, transactionId))
{
}

TStreamReaderBase::~TStreamReaderBase() = default;

TYPath TStreamReaderBase::Snapshot(const TYPath& path)
{
    return NYT::Snapshot(ClientRetryPolicy_, Auth_, ReadTransaction_->GetId(), path);
}

TString TStreamReaderBase::GetActiveRequestId() const
{
    if (Request_) {
        return Request_->GetRequestId();
    } else {
        return "<no-active-request>";
    }
}

size_t TStreamReaderBase::DoRead(void* buf, size_t len)
{
    const int retryCount = TConfig::Get()->ReadRetryCount;
    for (int attempt = 1; attempt <= retryCount; ++attempt) {
        try {
            if (!Input_) {
                Request_ = CreateRequest(Auth_, ReadTransaction_->GetId(), CurrentOffset_);
                Input_ = Request_->GetResponseStream();
            }
            if (len == 0) {
                return 0;
            }
            const size_t read = Input_->Read(buf, len);
            CurrentOffset_ += read;
            return read;
        } catch (TErrorResponse& e) {
            LOG_ERROR("RSP %s - failed: %s (attempt %d of %d)", GetActiveRequestId().data(), e.what(), attempt, retryCount);
            if (!IsRetriable(e) || attempt == retryCount) {
                throw;
            }
            NDetail::TWaitProxy::Get()->Sleep(GetBackoffDuration(e));
        } catch (yexception& e) {
            LOG_ERROR("RSP %s - failed: %s (attempt %d of %d)", GetActiveRequestId().data(), e.what(), attempt, retryCount);
            if (Request_) {
                Request_->InvalidateConnection();
            }
            if (attempt == retryCount) {
                throw;
            }
            NDetail::TWaitProxy::Get()->Sleep(GetBackoffDuration(e));
        }
        Input_ = nullptr;
    }
    Y_UNREACHABLE(); // we should either return or throw from loop above
}

////////////////////////////////////////////////////////////////////////////////

TFileReader::TFileReader(
    const TRichYPath& path,
    IClientRetryPolicyPtr clientRetryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TFileReaderOptions& options)
    : TStreamReaderBase(std::move(clientRetryPolicy), auth, transactionId)
    , FileReaderOptions_(options)
    , Path_(path)
    , StartOffset_(FileReaderOptions_.Offset_.GetOrElse(0))
    , EndOffset_(GetEndOffset(FileReaderOptions_))
{
    Path_.Path_ = TStreamReaderBase::Snapshot(Path_.Path_);
}

THolder<THttpRequest> TFileReader::CreateRequest(const TAuth& auth, const TTransactionId& transactionId, ui64 readBytes)
{
    const ui64 currentOffset = StartOffset_ + readBytes;
    TString proxyName = GetProxyForHeavyRequest(auth);

    THttpHeader header("GET", GetReadFileCommand());
    header.SetToken(auth.Token);
    header.AddTransactionId(transactionId);
    header.SetOutputFormat(TMaybe<TFormat>()); // Binary format

    if (EndOffset_) {
        Y_VERIFY(*EndOffset_ >= currentOffset);
        FileReaderOptions_.Length(*EndOffset_ - currentOffset);
    }
    FileReaderOptions_.Offset(currentOffset);
    header.MergeParameters(FormIORequestParameters(Path_, FileReaderOptions_));

    header.SetResponseCompression(::ToString(TConfig::Get()->AcceptEncoding));

    auto request = MakeHolder<THttpRequest>();
    try {
        request->Connect(proxyName);
        request->StartRequest(header);
        request->FinishRequest();
    } catch (const yexception& ex) {
        LogRequestError(*request, header, ex.what(), "");
        throw;
    }

    LOG_DEBUG("RSP %s - file stream", request->GetRequestId().data());

    return request;
}

////////////////////////////////////////////////////////////////////////////////

TBlobTableReader::TBlobTableReader(
    const TYPath& path,
    const TKey& key,
    IClientRetryPolicyPtr retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TBlobTableReaderOptions& options)
    : TStreamReaderBase(std::move(retryPolicy), auth, transactionId)
    , Key_(key)
    , Options_(options)
{
    Path_ = TStreamReaderBase::Snapshot(path);
}

THolder<THttpRequest> TBlobTableReader::CreateRequest(const TAuth& auth, const TTransactionId& transactionId, ui64 readBytes)
{
    TString proxyName = GetProxyForHeavyRequest(auth);

    THttpHeader header("GET", "read_blob_table");
    header.SetToken(auth.Token);
    header.AddTransactionId(transactionId);
    header.SetOutputFormat(TMaybe<TFormat>()); // Binary format

    const ui64 startPartIndex = readBytes / Options_.PartSize_;
    const ui64 skipBytes = readBytes - Options_.PartSize_ * startPartIndex;
    TNode params = PathToParamNode(TRichYPath(Path_).AddRange(TReadRange().Exact(TReadLimit().Key(Key_))));
    params["start_part_index"] = TNode(startPartIndex);
    params["offset"] = skipBytes;
    if (Options_.PartIndexColumnName_) {
        params["part_index_column_name"] = *Options_.PartIndexColumnName_;
    }
    if (Options_.DataColumnName_) {
        params["data_column_name"] = *Options_.DataColumnName_;
    }
    params["part_size"] = Options_.PartSize_;
    header.MergeParameters(params);
    header.SetResponseCompression(::ToString(TConfig::Get()->AcceptEncoding));

    auto request = MakeHolder<THttpRequest>();
    try {
        request->Connect(proxyName);
        request->StartRequest(header);
        request->FinishRequest();
    } catch (const yexception& ex) {
        LogRequestError(*request, header, ex.what(), "");
        throw;
    }

    LOG_DEBUG("RSP %s - blob table stream", request->GetRequestId().data());
    return request;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NYT
