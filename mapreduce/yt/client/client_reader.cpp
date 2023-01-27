#include "client_reader.h"

#include "transaction.h"
#include "transaction_pinger.h"

#include <mapreduce/yt/common/config.h>
#include <mapreduce/yt/common/helpers.h>
#include <mapreduce/yt/common/retry_lib.h>
#include <mapreduce/yt/common/wait_proxy.h>

#include <mapreduce/yt/interface/logging/yt_log.h>

#include <mapreduce/yt/io/helpers.h>
#include <mapreduce/yt/io/yamr_table_reader.h>

#include <mapreduce/yt/http/helpers.h>
#include <mapreduce/yt/http/requests.h>
#include <mapreduce/yt/http/retry_request.h>

#include <library/cpp/yson/node/serialize.h>

#include <mapreduce/yt/raw_client/raw_requests.h>

#include <util/random/random.h>
#include <util/stream/file.h>
#include <util/stream/str.h>
#include <util/string/builder.h>
#include <util/string/cast.h>

namespace NYT {

using ::ToString;

////////////////////////////////////////////////////////////////////////////////

TClientReader::TClientReader(
    const TRichYPath& path,
    IClientRetryPolicyPtr clientRetryPolicy,
    ITransactionPingerPtr transactionPinger,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TFormat& format,
    const TTableReaderOptions& options,
    bool useFormatFromTableAttributes)
    : Path_(path)
    , ClientRetryPolicy_(std::move(clientRetryPolicy))
    , Auth_(auth)
    , ParentTransactionId_(transactionId)
    , Format_(format)
    , Options_(options)
    , ReadTransaction_(nullptr)
{
    if (options.CreateTransaction_) {
      ReadTransaction_ = MakeHolder<TPingableTransaction>(
          ClientRetryPolicy_,
          Auth_,
          transactionId,
          transactionPinger->GetChildTxPinger(),
          TStartTransactionOptions());
      Path_.Path(Snapshot(
        ClientRetryPolicy_,
        Auth_,
        ReadTransaction_->GetId(),
        path.Path_));
    }

    if (useFormatFromTableAttributes) {
        auto transactionId2 = ReadTransaction_ ? ReadTransaction_->GetId() : ParentTransactionId_;
        auto newFormat = GetTableFormat(ClientRetryPolicy_, Auth_, transactionId2, Path_);
        if (newFormat) {
            Format_->Config = *newFormat;
        }
    }

    TransformYPath();
    CreateRequest();
}

bool TClientReader::Retry(
    const TMaybe<ui32>& rangeIndex,
    const TMaybe<ui64>& rowIndex)
{
    if (CurrentRequestRetryPolicy_) {
        // TODO we should pass actual exception in Retry function
        yexception genericError;
        auto backoff = CurrentRequestRetryPolicy_->OnGenericError(genericError);
        if (!backoff) {
            return false;
        }
    }

    try {
        CreateRequest(rangeIndex, rowIndex);
        return true;
    } catch (const std::exception& ex) {
        YT_LOG_ERROR("Client reader retry failed: %v",
            ex.what());

        return false;
    }
}

void TClientReader::ResetRetries()
{
    CurrentRequestRetryPolicy_ = nullptr;
}

size_t TClientReader::DoRead(void* buf, size_t len)
{
    return Input_->Read(buf, len);
}

void TClientReader::TransformYPath()
{
    for (auto& range : Path_.MutableRangesView()) {
        auto& exact = range.Exact_;
        if (IsTrivial(exact)) {
            continue;
        }

        if (exact.RowIndex_) {
            range.LowerLimit(TReadLimit().RowIndex(*exact.RowIndex_));
            range.UpperLimit(TReadLimit().RowIndex(*exact.RowIndex_ + 1));
            exact.RowIndex_.Clear();

        } else if (exact.Key_) {
            range.LowerLimit(TReadLimit().Key(*exact.Key_));

            auto lastPart = TNode::CreateEntity();
            lastPart.Attributes() = TNode()("type", "max");
            exact.Key_->Parts_.push_back(lastPart);

            range.UpperLimit(TReadLimit().Key(*exact.Key_));
            exact.Key_.Clear();
        }
    }
}

void TClientReader::CreateRequest(const TMaybe<ui32>& rangeIndex, const TMaybe<ui64>& rowIndex)
{
    if (!CurrentRequestRetryPolicy_) {
        CurrentRequestRetryPolicy_ = ClientRetryPolicy_->CreatePolicyForGenericRequest();
    }
    while (true) {
        CurrentRequestRetryPolicy_->NotifyNewAttempt();

        THttpHeader header("GET", GetReadTableCommand());
        header.SetToken(Auth_.Token);
        auto transactionId = (ReadTransaction_ ? ReadTransaction_->GetId() : ParentTransactionId_);
        header.AddTransactionId(transactionId);
        header.AddParameter("control_attributes", TNode()
            ("enable_row_index", true)
            ("enable_range_index", true));
        header.SetOutputFormat(Format_);

        header.SetResponseCompression(ToString(TConfig::Get()->AcceptEncoding));

        if (rowIndex.Defined()) {
            auto& ranges = Path_.MutableRanges();
            if (ranges.Empty()) {
                ranges.ConstructInPlace(TVector{TReadRange()});
            } else {
                if (rangeIndex.GetOrElse(0) >= ranges->size()) {
                    ythrow yexception()
                        << "range index " << rangeIndex.GetOrElse(0)
                        << " is out of range, input range count is " << ranges->size();
                }
                ranges->erase(ranges->begin(), ranges->begin() + rangeIndex.GetOrElse(0));
            }
            ranges->begin()->LowerLimit(TReadLimit().RowIndex(*rowIndex));
        }

        header.MergeParameters(FormIORequestParameters(Path_, Options_));

        auto requestId = CreateGuidAsString();

        try {
            const auto proxyName = GetProxyForHeavyRequest(Auth_);
            Response_ = Auth_.HttpClient->StartRequest(GetFullUrl(proxyName, Auth_, header), requestId, header)->Finish();

            Input_ = Response_->GetResponseStream();

            YT_LOG_DEBUG("RSP %v - table stream", requestId);

            return;
        } catch (const TErrorResponse& e) {
            LogRequestError(
                requestId,
                header,
                e.what(),
                CurrentRequestRetryPolicy_->GetAttemptDescription());

            if (!IsRetriable(e)) {
                throw;
            }
            auto backoff = CurrentRequestRetryPolicy_->OnRetriableError(e);
            if (!backoff) {
                throw;
            }
            NDetail::TWaitProxy::Get()->Sleep(*backoff);
        } catch (const yexception& e) {
            LogRequestError(
                requestId,
                header,
                e.what(),
                CurrentRequestRetryPolicy_->GetAttemptDescription());

            Response_.reset();

            auto backoff = CurrentRequestRetryPolicy_->OnGenericError(e);
            if (!backoff) {
                throw;
            }
            NDetail::TWaitProxy::Get()->Sleep(*backoff);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
