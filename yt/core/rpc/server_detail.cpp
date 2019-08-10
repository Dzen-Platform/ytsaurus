#include "server_detail.h"
#include "private.h"
#include "config.h"
#include "message.h"

#include <yt/core/bus/bus.h>

#include <yt/core/net/address.h>

#include <yt/core/misc/protobuf_helpers.h>

namespace NYT::NRpc {

using namespace NConcurrency;
using namespace NBus;
using namespace NYTree;
using namespace NRpc::NProto;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

TServiceContextBase::TServiceContextBase(
    std::unique_ptr<TRequestHeader> header,
    TSharedRefArray requestMessage,
    NLogging::TLogger logger,
    NLogging::ELogLevel logLevel)
    : RequestHeader_(std::move(header))
    , RequestMessage_(std::move(requestMessage))
    , Logger(std::move(logger))
    , LogLevel_(logLevel)
{
    Initialize();
}

TServiceContextBase::TServiceContextBase(
    TSharedRefArray requestMessage,
    NLogging::TLogger logger,
    NLogging::ELogLevel logLevel)
    : RequestHeader_(new TRequestHeader())
    , RequestMessage_(std::move(requestMessage))
    , Logger(std::move(logger))
    , LogLevel_(logLevel)
{
    YT_VERIFY(ParseRequestHeader(RequestMessage_, RequestHeader_.get()));
    Initialize();
}

void TServiceContextBase::Initialize()
{
    RequestId_ = FromProto<TRequestId>(RequestHeader_->request_id());
    RealmId_ = FromProto<TRealmId>(RequestHeader_->realm_id());
    User_ = RequestHeader_->has_user()
        ? RequestHeader_->user()
        : RootUserName;

    YT_ASSERT(RequestMessage_.Size() >= 2);
    RequestBody_ = RequestMessage_[1];
    RequestAttachments_ = std::vector<TSharedRef>(
        RequestMessage_.Begin() + 2,
        RequestMessage_.End());
}

void TServiceContextBase::Reply(const TError& error)
{
    YT_ASSERT(!Replied_);

    Error_ = error;

    ReplyEpilogue();
}

void TServiceContextBase::Reply(const TSharedRefArray& responseMessage)
{
    YT_ASSERT(!Replied_);
    YT_ASSERT(responseMessage.Size() >= 1);

    // NB: One must parse responseMessage and only use its content since,
    // e.g., responseMessage may contain invalid request id.
    TResponseHeader header;
    YT_VERIFY(ParseResponseHeader(responseMessage, &header));

    if (header.has_error()) {
        Error_ = FromProto<TError>(header.error());
    }
    if (Error_.IsOK()) {
        YT_ASSERT(responseMessage.Size() >= 2);
        ResponseBody_ = responseMessage[1];
        ResponseAttachments_ = std::vector<TSharedRef>(
            responseMessage.Begin() + 2,
            responseMessage.End());
    } else {
        ResponseBody_.Reset();
        ResponseAttachments_.clear();
    }

    ReplyEpilogue();
}

void TServiceContextBase::ReplyEpilogue()
{
    DoReply();

    Replied_.store(true);

    {
        auto responseGuard = Guard(ResponseLock_);

        YT_ASSERT(!ResponseMessage_);
        ResponseMessage_ = BuildResponseMessage();

        if (AsyncResponseMessage_) {
            responseGuard.Release();
            AsyncResponseMessage_.Set(ResponseMessage_);
        }
    }

    if (Logger.IsLevelEnabled(LogLevel_)) {
        LogResponse();
    }
}

void TServiceContextBase::SetComplete()
{ }

TFuture<TSharedRefArray> TServiceContextBase::GetAsyncResponseMessage() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto responseGuard  = Guard(ResponseLock_);

    if (!AsyncResponseMessage_) {
        AsyncResponseMessage_ = NewPromise<TSharedRefArray>();
        if (ResponseMessage_) {
            responseGuard.Release();
            AsyncResponseMessage_.Set(ResponseMessage_);
        }
    }

    return AsyncResponseMessage_;
}

const TSharedRefArray& TServiceContextBase::GetResponseMessage() const
{
    YT_ASSERT(ResponseMessage_);
    return ResponseMessage_;
}

TSharedRefArray TServiceContextBase::BuildResponseMessage()
{
    NProto::TResponseHeader header;
    ToProto(header.mutable_request_id(), RequestId_);
    ToProto(header.mutable_error(), Error_);

    if (RequestHeader_->has_response_format()) {
        header.set_format(RequestHeader_->response_format());
    }

    if (RequestHeader_->has_response_memory_zone()) {
        header.set_memory_zone(RequestHeader_->response_memory_zone());
    }

    // COMPAT(kiselyovp)
    if (RequestHeader_->has_response_codec()) {
        header.set_codec(static_cast<int>(ResponseCodec_));
    }

    auto message = Error_.IsOK()
        ? CreateResponseMessage(
            header,
            ResponseBody_,
            ResponseAttachments_)
        : CreateErrorResponseMessage(header);

    auto responseMessageError = CheckBusMessageLimits(ResponseMessage_);
    if (!responseMessageError.IsOK()) {
        return CreateErrorResponseMessage(responseMessageError);
    }

    return message;
}

bool TServiceContextBase::IsReplied() const
{
    VERIFY_THREAD_AFFINITY_ANY();
    return Replied_.load();
}

void TServiceContextBase::SubscribeCanceled(const TClosure& /*callback*/)
{ }

void TServiceContextBase::UnsubscribeCanceled(const TClosure& /*callback*/)
{ }

bool TServiceContextBase::IsCanceled()
{
    return false;
}

void TServiceContextBase::Cancel()
{ }

const TError& TServiceContextBase::GetError() const
{
    YT_ASSERT(Replied_);

    return Error_;
}

TSharedRef TServiceContextBase::GetRequestBody() const
{
    return RequestBody_;
}

std::vector<TSharedRef>& TServiceContextBase::RequestAttachments()
{
    return RequestAttachments_;
}

IAsyncZeroCopyInputStreamPtr TServiceContextBase::GetRequestAttachmentsStream()
{
    return nullptr;
}

TSharedRef TServiceContextBase::GetResponseBody()
{
    return ResponseBody_;
}

void TServiceContextBase::SetResponseBody(const TSharedRef& responseBody)
{
    YT_ASSERT(!Replied_);

    ResponseBody_ = responseBody;
}

std::vector<TSharedRef>& TServiceContextBase::ResponseAttachments()
{
    return ResponseAttachments_;
}

IAsyncZeroCopyOutputStreamPtr TServiceContextBase::GetResponseAttachmentsStream()
{
    return nullptr;
}

const NProto::TRequestHeader& TServiceContextBase::GetRequestHeader() const
{
    return *RequestHeader_;
}

TSharedRefArray TServiceContextBase::GetRequestMessage() const
{
    return RequestMessage_;
}

TRequestId TServiceContextBase::GetRequestId() const
{
    return RequestId_;
}

TTcpDispatcherStatistics TServiceContextBase::GetBusStatistics() const
{
    return {};
}

const IAttributeDictionary& TServiceContextBase::GetEndpointAttributes() const
{
    return EmptyAttributes();
}

std::optional<TInstant> TServiceContextBase::GetStartTime() const
{
    return RequestHeader_->has_start_time()
        ? std::make_optional(FromProto<TInstant>(RequestHeader_->start_time()))
        : std::nullopt;
}

std::optional<TDuration> TServiceContextBase::GetTimeout() const
{
    return RequestHeader_->has_timeout()
        ? std::make_optional(FromProto<TDuration>(RequestHeader_->timeout()))
        : std::nullopt;
}

bool TServiceContextBase::IsRetry() const
{
    return RequestHeader_->retry();
}

TMutationId TServiceContextBase::GetMutationId() const
{
    return FromProto<TMutationId>(RequestHeader_->mutation_id());
}

const TString& TServiceContextBase::GetService() const
{
    return RequestHeader_->service();
}

const TString& TServiceContextBase::GetMethod() const
{
    return RequestHeader_->method();
}

TRealmId TServiceContextBase::GetRealmId() const
{
    return RealmId_;
}

const TString& TServiceContextBase::GetUser() const
{
    return User_;
}

const TRequestHeader& TServiceContextBase::RequestHeader() const
{
    return *RequestHeader_;
}

TRequestHeader& TServiceContextBase::RequestHeader()
{
    return *RequestHeader_;
}

void TServiceContextBase::SetRawRequestInfo(TString info, bool incremental)
{
    YT_ASSERT(!Replied_);

    if (!Logger.IsLevelEnabled(LogLevel_)) {
        return;
    }

    if (!info.empty()) {
        RequestInfos_.push_back(std::move(info));
    }
    if (!incremental) {
        LogRequest();
    }
}

void TServiceContextBase::SetRawResponseInfo(TString info, bool incremental)
{
    YT_ASSERT(!Replied_);

    if (!Logger.IsLevelEnabled(LogLevel_)) {
        return;
    }

    if (!incremental) {
        ResponseInfos_.clear();
    }
    if (!info.empty()) {
        ResponseInfos_.push_back(std::move(info));
    }
}

const NLogging::TLogger& TServiceContextBase::GetLogger() const
{
    return Logger;
}

NLogging::ELogLevel TServiceContextBase::GetLogLevel() const
{
    return LogLevel_;
}

bool TServiceContextBase::IsPooled() const
{
    return false;
}

NCompression::ECodec TServiceContextBase::GetResponseCodec() const
{
    return ResponseCodec_;
}

void TServiceContextBase::SetResponseCodec(NCompression::ECodec codec)
{
    ResponseCodec_ = codec;
}

////////////////////////////////////////////////////////////////////////////////

TServiceContextWrapper::TServiceContextWrapper(IServiceContextPtr underlyingContext)
    : UnderlyingContext_(std::move(underlyingContext))
{ }

const NProto::TRequestHeader& TServiceContextWrapper::GetRequestHeader() const
{
    return UnderlyingContext_->GetRequestHeader();
}

TTcpDispatcherStatistics TServiceContextWrapper::GetBusStatistics() const
{
    return UnderlyingContext_->GetBusStatistics();
}

const NYTree::IAttributeDictionary& TServiceContextWrapper::GetEndpointAttributes() const
{
    return UnderlyingContext_->GetEndpointAttributes();
}

TSharedRefArray TServiceContextWrapper::GetRequestMessage() const
{
    return UnderlyingContext_->GetRequestMessage();
}

TRequestId TServiceContextWrapper::GetRequestId() const
{
    return UnderlyingContext_->GetRequestId();
}

std::optional<TInstant> TServiceContextWrapper::GetStartTime() const
{
    return UnderlyingContext_->GetStartTime();
}

std::optional<TDuration> TServiceContextWrapper::GetTimeout() const
{
    return UnderlyingContext_->GetTimeout();
}

bool TServiceContextWrapper::IsRetry() const
{
    return UnderlyingContext_->IsRetry();
}

TMutationId TServiceContextWrapper::GetMutationId() const
{
    return UnderlyingContext_->GetMutationId();
}

const TString& TServiceContextWrapper::GetService() const
{
    return UnderlyingContext_->GetService();
}

const TString& TServiceContextWrapper::GetMethod() const
{
    return UnderlyingContext_->GetMethod();
}

TRealmId TServiceContextWrapper::GetRealmId() const
{
    return UnderlyingContext_->GetRealmId();
}

const TString& TServiceContextWrapper::GetUser() const
{
    return UnderlyingContext_->GetUser();
}

bool TServiceContextWrapper::IsReplied() const
{
    return UnderlyingContext_->IsReplied();
}

void TServiceContextWrapper::Reply(const TError& error)
{
    UnderlyingContext_->Reply(error);
}

void TServiceContextWrapper::Reply(const TSharedRefArray& responseMessage)
{
    UnderlyingContext_->Reply(responseMessage);
}

void TServiceContextWrapper::SetComplete()
{
    UnderlyingContext_->SetComplete();
}

void TServiceContextWrapper::SubscribeCanceled(const TClosure& callback)
{
    UnderlyingContext_->SubscribeCanceled(callback);
}

void TServiceContextWrapper::UnsubscribeCanceled(const TClosure& callback)
{
    UnderlyingContext_->UnsubscribeCanceled(callback);
}

bool TServiceContextWrapper::IsCanceled()
{
    return UnderlyingContext_->IsCanceled();
}

void TServiceContextWrapper::Cancel()
{ }

TFuture<TSharedRefArray> TServiceContextWrapper::GetAsyncResponseMessage() const
{
    return UnderlyingContext_->GetAsyncResponseMessage();
}

const TSharedRefArray& TServiceContextWrapper::GetResponseMessage() const
{
    return UnderlyingContext_->GetResponseMessage();
}

const TError& TServiceContextWrapper::GetError() const
{
    return UnderlyingContext_->GetError();
}

TSharedRef TServiceContextWrapper::GetRequestBody() const
{
    return UnderlyingContext_->GetRequestBody();
}

TSharedRef TServiceContextWrapper::GetResponseBody()
{
    return UnderlyingContext_->GetResponseBody();
}

void TServiceContextWrapper::SetResponseBody(const TSharedRef& responseBody)
{
    UnderlyingContext_->SetResponseBody(responseBody);
}

std::vector<TSharedRef>& TServiceContextWrapper::RequestAttachments()
{
    return UnderlyingContext_->RequestAttachments();
}

IAsyncZeroCopyInputStreamPtr TServiceContextWrapper::GetRequestAttachmentsStream()
{
    return UnderlyingContext_->GetRequestAttachmentsStream();
}

std::vector<TSharedRef>& TServiceContextWrapper::ResponseAttachments()
{
    return UnderlyingContext_->ResponseAttachments();
}

const NProto::TRequestHeader& TServiceContextWrapper::RequestHeader() const
{
    return UnderlyingContext_->RequestHeader();
}

IAsyncZeroCopyOutputStreamPtr TServiceContextWrapper::GetResponseAttachmentsStream()
{
    return UnderlyingContext_->GetResponseAttachmentsStream();
}

NProto::TRequestHeader& TServiceContextWrapper::RequestHeader()
{
    return UnderlyingContext_->RequestHeader();
}

void TServiceContextWrapper::SetRawRequestInfo(TString info, bool incremental)
{
    UnderlyingContext_->SetRawRequestInfo(std::move(info), incremental);
}

void TServiceContextWrapper::SetRawResponseInfo(TString info, bool incremental)
{
    UnderlyingContext_->SetRawResponseInfo(std::move(info), incremental);
}

const NLogging::TLogger& TServiceContextWrapper::GetLogger() const
{
    return UnderlyingContext_->GetLogger();
}

NLogging::ELogLevel TServiceContextWrapper::GetLogLevel() const
{
    return UnderlyingContext_->GetLogLevel();
}

bool TServiceContextWrapper::IsPooled() const
{
    return UnderlyingContext_->IsPooled();
}

NCompression::ECodec TServiceContextWrapper::GetResponseCodec() const
{
    return UnderlyingContext_->GetResponseCodec();
}

void TServiceContextWrapper::SetResponseCodec(NCompression::ECodec codec)
{
    UnderlyingContext_->SetResponseCodec(codec);
}

////////////////////////////////////////////////////////////////////////////////

void TServerBase::RegisterService(IServicePtr service)
{
    YT_VERIFY(service);

    auto serviceId = service->GetServiceId();

    {
        TWriterGuard guard(ServicesLock_);
        YT_VERIFY(ServiceMap_.insert(std::make_pair(serviceId, service)).second);
        if (Config_) {
            auto it = Config_->Services.find(serviceId.ServiceName);
            if (it != Config_->Services.end()) {
                service->Configure(it->second);
            }
        }
        DoRegisterService(service);
    }

    YT_LOG_INFO("RPC service registered (ServiceName: %v, RealmId: %v)",
        serviceId.ServiceName,
        serviceId.RealmId);
}

bool TServerBase::UnregisterService(IServicePtr service)
{
    YT_VERIFY(service);

    auto serviceId = service->GetServiceId();

    {
        TWriterGuard guard(ServicesLock_);
        auto it = ServiceMap_.find(serviceId);
        if (it == ServiceMap_.end() || it->second != service) {
            return false;
        }
        ServiceMap_.erase(it);
        DoUnregisterService(service);
    }

    YT_LOG_INFO("RPC service unregistered (ServiceName: %v, RealmId: %v)",
        serviceId.ServiceName,
        serviceId.RealmId);
    return true;
}

IServicePtr TServerBase::FindService(const TServiceId& serviceId)
{
    TReaderGuard guard(ServicesLock_);
    auto it = ServiceMap_.find(serviceId);
    return it == ServiceMap_.end() ? nullptr : it->second;
}

void TServerBase::Configure(TServerConfigPtr config)
{
    TWriterGuard guard(ServicesLock_);

    // Future services will be configured appropriately.
    Config_ = config;

    // Apply configuration to all existing services.
    for (const auto& pair : config->Services) {
        const auto& serviceName = pair.first;
        const auto& serviceConfig = pair.second;
        auto services = DoFindServices(serviceName);
        for (auto service : services) {
            service->Configure(serviceConfig);
        }
    }
}

void TServerBase::Start()
{
    YT_VERIFY(!Started_);

    DoStart();

    YT_LOG_INFO("RPC server started");
}

TFuture<void> TServerBase::Stop(bool graceful)
{
    if (!Started_) {
        return VoidFuture;
    }

    YT_LOG_INFO("Stopping RPC server (Graceful: %v)",
        graceful);

    return DoStop(graceful);
}

TServerBase::TServerBase(const NLogging::TLogger& logger)
    : Logger(logger)
{ }

void TServerBase::DoStart()
{
    Started_ = true;
}

TFuture<void> TServerBase::DoStop(bool graceful)
{
    Started_ = false;

    std::vector<TFuture<void>> asyncResults;

    if (graceful) {
        std::vector<IServicePtr> services;
        {
            TReaderGuard guard(ServicesLock_);
            for (const auto& pair : ServiceMap_) {
                services.push_back(pair.second);
            }
        }

        for (const auto& service : services) {
            asyncResults.push_back(service->Stop());
        }
    }

    return Combine(asyncResults).Apply(BIND([=, this_ = MakeStrong(this)] () {
        YT_LOG_INFO("RPC server stopped");
    }));
}

void TServerBase::DoRegisterService(const IServicePtr& service)
{ }

void TServerBase::DoUnregisterService(const IServicePtr& service)
{ }

std::vector<IServicePtr> TServerBase::DoFindServices(const TString& serviceName)
{
    std::vector<IServicePtr> result;
    for (const auto& pair : ServiceMap_) {
        if (pair.first.ServiceName == serviceName) {
            result.push_back(pair.second);
        }
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
