#include "client.h"
#include "private.h"
#include "dispatcher.h"
#include "message.h"
#include "stream.h"

#include <yt/core/net/local_address.h>

#include <yt/core/misc/cast.h>
#include <yt/core/misc/checksum.h>
#include <yt/core/misc/memory_zone.h>

namespace NYT::NRpc {

using namespace NBus;
using namespace NYTree;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = RpcClientLogger;

static const auto ClientHostAnnotation = TString("client_host");
static const auto RequestIdAnnotation = TString("request_id");

////////////////////////////////////////////////////////////////////////////////

TClientContext::TClientContext(
    TRequestId requestId,
    const NTracing::TTraceContext& traceContext,
    const TString& service,
    const TString& method,
    bool heavy,
    EMemoryZone memoryZone,
    TAttachmentsOutputStreamPtr requestAttachmentsStream,
    TAttachmentsInputStreamPtr responseAttachmentsStream)
    : RequestId_(requestId)
    , TraceContext_(traceContext)
    , Service_(service)
    , Method_(method)
    , Heavy_(heavy)
    , MemoryZone_(memoryZone)
    , RequestAttachmentsStream_(std::move(requestAttachmentsStream))
    , ResponseAttachmentsStream_(std::move(responseAttachmentsStream))
{ }

////////////////////////////////////////////////////////////////////////////////

TClientRequest::TClientRequest(
    IChannelPtr channel,
    const TServiceDescriptor& serviceDescriptor,
    const TMethodDescriptor& methodDescriptor)
    : Channel_(std::move(channel))
    , StreamingEnabled_(methodDescriptor.StreamingEnabled)
{
    Y_ASSERT(Channel_);

    Header_.set_service(serviceDescriptor.ServiceName);
    Header_.set_method(methodDescriptor.MethodName);
    Header_.set_protocol_version_major(serviceDescriptor.ProtocolVersion.Major);
    Header_.set_protocol_version_minor(serviceDescriptor.ProtocolVersion.Minor);

    ToProto(Header_.mutable_request_id(), TRequestId::Create());
}

TClientRequest::TClientRequest(const TClientRequest& other)
    : Attachments_(other.Attachments_)
    , Timeout_(other.Timeout_)
    , RequestAck_(other.RequestAck_)
    , Heavy_(other.Heavy_)
    , RequestCodec_(other.RequestCodec_)
    , ResponseCodec_(other.ResponseCodec_)
    , GenerateAttachmentChecksums_(other.GenerateAttachmentChecksums_)
    , MemoryZone_(other.MemoryZone_)
    , Channel_(other.Channel_)
    , StreamingEnabled_(other.StreamingEnabled_)
    , Header_(other.Header_)
    , SerializedData_(other.SerializedData_)
    , Hash_(other.Hash_)
    , MultiplexingBand_(other.MultiplexingBand_)
    , FirstTimeSerialization_(other.FirstTimeSerialization_)
{ }

TSharedRefArray TClientRequest::Serialize()
{
    if (FirstTimeSerialization_) {
        SetCodecsInHeader();
        SetStreamingParametersInHeader();
    } else {
        if (StreamingEnabled_) {
            THROW_ERROR_EXCEPTION("Retries are not supported for requests with streaming");
        }
        Header_.set_retry(true);
    }
    FirstTimeSerialization_ = false;

    return CreateRequestMessage(
        Header_,
        GetSerializedData());
}

IClientRequestControlPtr TClientRequest::Send(IClientResponseHandlerPtr responseHandler)
{
    TSendOptions options;
    options.Timeout = Timeout_;
    options.RequestAck = RequestAck_;
    options.GenerateAttachmentChecksums = GenerateAttachmentChecksums_;
    options.MemoryZone = MemoryZone_;
    options.MultiplexingBand = MultiplexingBand_;
    auto control = Channel_->Send(
        this,
        std::move(responseHandler),
        options);
    RequestControl_ = control;
    return control;
}

NProto::TRequestHeader& TClientRequest::Header()
{
    return Header_;
}

const NProto::TRequestHeader& TClientRequest::Header() const
{
    return Header_;
}

const TStreamingParameters& TClientRequest::ClientAttachmentsStreamingParameters() const
{
    return ClientAttachmentsStreamingParameters_;
}

TStreamingParameters& TClientRequest::ClientAttachmentsStreamingParameters()
{
    return ClientAttachmentsStreamingParameters_;
}

const TStreamingParameters& TClientRequest::ServerAttachmentsStreamingParameters() const
{
    return ServerAttachmentsStreamingParameters_;
}

TStreamingParameters& TClientRequest::ServerAttachmentsStreamingParameters()
{
    return ServerAttachmentsStreamingParameters_;
}

NConcurrency::IAsyncZeroCopyOutputStreamPtr TClientRequest::GetRequestAttachmentsStream() const
{
    if (!RequestAttachmentsStream_) {
        THROW_ERROR_EXCEPTION(NRpc::EErrorCode::StreamingNotSupported, "Streaming is not supported");
    }
    return RequestAttachmentsStream_;
}

NConcurrency::IAsyncZeroCopyInputStreamPtr TClientRequest::GetResponseAttachmentsStream() const
{
    if (!ResponseAttachmentsStream_) {
        THROW_ERROR_EXCEPTION(NRpc::EErrorCode::StreamingNotSupported, "Streaming is not supported");
    }
    return ResponseAttachmentsStream_;
}

bool TClientRequest::IsHeavy() const
{
    return Heavy_;
}

TRequestId TClientRequest::GetRequestId() const
{
    return FromProto<TRequestId>(Header_.request_id());
}

TRealmId TClientRequest::GetRealmId() const
{
    return FromProto<TRealmId>(Header_.realm_id());
}

const TString& TClientRequest::GetService() const
{
    return Header_.service();
}

const TString& TClientRequest::GetMethod() const
{
    return Header_.method();
}

const TString& TClientRequest::GetUser() const
{
    return Header_.has_user()
        ? Header_.user()
        : RootUserName;
}

void TClientRequest::SetUser(const TString& user)
{
    if (user == RootUserName) {
        Header_.clear_user();
    } else {
        Header_.set_user(user);
    }
}

void TClientRequest::SetUserAgent(const TString& userAgent)
{
    Header_.set_user_agent(userAgent);
}

bool TClientRequest::GetRetry() const
{
    return Header_.retry();
}

void TClientRequest::SetRetry(bool value)
{
    Header_.set_retry(value);
}

TMutationId TClientRequest::GetMutationId() const
{
    return FromProto<TMutationId>(Header_.mutation_id());
}

void TClientRequest::SetMutationId(TMutationId id)
{
    if (id) {
        ToProto(Header_.mutable_mutation_id(), id);
    } else {
        Header_.clear_mutation_id();
    }
}

size_t TClientRequest::GetHash() const
{
    if (!Hash_) {
        size_t hash = 0;
        auto serializedData = GetSerializedData();
        for (auto ref : serializedData) {
            HashCombine(hash, GetChecksum(ref));
        }
        Hash_ = hash;
    }
    return *Hash_;
}

EMultiplexingBand TClientRequest::GetMultiplexingBand() const
{
    return MultiplexingBand_;
}

void TClientRequest::SetMultiplexingBand(EMultiplexingBand band)
{
    MultiplexingBand_ = band;
    Header_.set_tos_level(TDispatcher::Get()->GetTosLevelForBand(band));
}

TClientContextPtr TClientRequest::CreateClientContext()
{
    auto traceContext = NTracing::CreateChildTraceContext();
    if (traceContext.IsEnabled()) {
        SetTraceContext(&Header(), traceContext);
        TraceRequest(traceContext);
    }
    if (traceContext.IsVerbose()) {
        TraceRequest(traceContext);
    }

    if (StreamingEnabled_) {
        RequestAttachmentsStream_ = New<TAttachmentsOutputStream>(
            MemoryZone_,
            RequestCodec_,
            GetInvoker(),
            BIND(&TClientRequest::OnPullRequestAttachmentsStream, MakeWeak(this)),
            ClientAttachmentsStreamingParameters_.WindowSize,
            ClientAttachmentsStreamingParameters_.WriteTimeout);
        ResponseAttachmentsStream_ = New<TAttachmentsInputStream>(
            BIND(&TClientRequest::OnResponseAttachmentsStreamRead, MakeWeak(this)),
            GetInvoker(),
            ClientAttachmentsStreamingParameters_.ReadTimeout);
    }

    return New<TClientContext>(
        GetRequestId(),
        traceContext,
        GetService(),
        GetMethod(),
        Heavy_,
        MemoryZone_,
        RequestAttachmentsStream_,
        ResponseAttachmentsStream_);
}

void TClientRequest::OnPullRequestAttachmentsStream()
{
    auto payload = RequestAttachmentsStream_->TryPull();
    if (!payload) {
        return;
    }

    auto control = RequestControl_.Lock();
    if (!control) {
        RequestAttachmentsStream_->Abort(TError(NRpc::EErrorCode::StreamingNotSupported, "Streaming is not supported"));
        return;
    }

    YT_LOG_DEBUG("Request streaming attachments pulled (RequestId: %v, SequenceNumber: %v, Sizes: %v, Closed: %v)",
        GetRequestId(),
        payload->SequenceNumber,
        MakeFormattableView(payload->Attachments, [] (auto* builder, const auto& attachment) {
            builder->AppendFormat("%v", GetStreamingAttachmentSize(attachment));
        }),
        !payload->Attachments.back());

    control->SendStreamingPayload(*payload).Subscribe(
        BIND(&TClientRequest::OnRequestStreamingPayloadAcked, MakeStrong(this), payload->SequenceNumber));;
}

void TClientRequest::OnRequestStreamingPayloadAcked(int sequenceNumber, const TError& error)
{
    if (error.IsOK()) {
        YT_LOG_DEBUG("Request streaming payload delivery acknowledged (RequestId: %v, SequenceNumber: %v)",
            GetRequestId(),
            sequenceNumber);
    } else {
        YT_LOG_DEBUG(error, "Response streaming payload delivery failed (RequestId: %v, SequenceNumber: %v)",
            GetRequestId(),
            sequenceNumber);
        RequestAttachmentsStream_->Abort(error);
    }
}

void TClientRequest::OnResponseAttachmentsStreamRead()
{
    auto feedback = ResponseAttachmentsStream_->GetFeedback();

    auto control = RequestControl_.Lock();
    if (!control) {
        ResponseAttachmentsStream_->Abort(TError(NRpc::EErrorCode::StreamingNotSupported, "Streaming is not supported"));
        return;
    }

    YT_LOG_DEBUG("Response streaming attachments read (RequestId: %v, ReadPosition: %v)",
        GetRequestId(),
        feedback.ReadPosition);

    control->SendStreamingFeedback(feedback).Subscribe(
        BIND(&TClientRequest::OnResponseStreamingFeedbackAcked, MakeStrong(this)));
}

void TClientRequest::OnResponseStreamingFeedbackAcked(const TError& error)
{
    if (error.IsOK()) {
        YT_LOG_DEBUG("Response streaming feedback delivery acknowledged (RequestId: %v)",
            GetRequestId());
    } else {
        YT_LOG_DEBUG(error, "Response streaming feedback delivery failed (RequestId: %v)",
            GetRequestId());
        ResponseAttachmentsStream_->Abort(error);
    }
}

const IInvokerPtr& TClientRequest::GetInvoker() const
{
    return GetHeavy()
        ? TDispatcher::Get()->GetHeavyInvoker()
        : TDispatcher::Get()->GetLightInvoker();
}

void TClientRequest::TraceRequest(const NTracing::TTraceContext& traceContext)
{
    NTracing::TraceEvent(
        traceContext,
        GetService(),
        GetMethod(),
        NTracing::ClientSendAnnotation);

    NTracing::TraceEvent(
        traceContext,
        RequestIdAnnotation,
        GetRequestId());

    NTracing::TraceEvent(
        traceContext,
        ClientHostAnnotation,
        NNet::GetLocalHostName());
}

void TClientRequest::SetCodecsInHeader()
{
    // COMPAT(kiselyovp): legacy RPC codecs
    if (EnableLegacyRpcCodecs_) {
        return;
    }

    Header_.set_request_codec(static_cast<int>(RequestCodec_));
    Header_.set_response_codec(static_cast<int>(ResponseCodec_));
}

void TClientRequest::SetStreamingParametersInHeader()
{
    if (!StreamingEnabled_) {
        return;
    }

    ToProto(Header_.mutable_server_attachments_streaming_parameters(), ServerAttachmentsStreamingParameters_);
}

const TSharedRefArray& TClientRequest::GetSerializedData() const
{
    if (!SerializedData_) {
        SerializedData_ = SerializeData();
    }
    return SerializedData_;
}

////////////////////////////////////////////////////////////////////////////////

TClientResponse::TClientResponse(TClientContextPtr clientContext)
    : StartTime_(NProfiling::GetInstant())
    , ClientContext_(std::move(clientContext))
{ }

const NProto::TResponseHeader& TClientResponse::Header() const
{
    return Header_;
}

TSharedRefArray TClientResponse::GetResponseMessage() const
{
    Y_ASSERT(ResponseMessage_);
    return ResponseMessage_;
}

size_t TClientResponse::GetTotalSize() const
{
    Y_ASSERT(ResponseMessage_);
    auto result = ResponseMessage_.ByteSize();

    for (const auto& attachment : Attachments_) {
        result += attachment.Size();
    }

    return result;
}

void TClientResponse::HandleError(const TError& error)
{
    auto prevState = State_.exchange(EState::Done);
    if (prevState == EState::Done) {
        // Ignore the error.
        // Most probably this is a late timeout.
        return;
    }

    GetInvoker()->Invoke(
        BIND(&TClientResponse::DoHandleError, MakeStrong(this), error));
}

void TClientResponse::DoHandleError(const TError& error)
{
    Finish(error);
}

void TClientResponse::Finish(const TError& error)
{
    NTracing::TTraceContextGuard guard(ClientContext_->GetTraceContext());

    TraceResponse();

    const auto& requestAttachmentsStream = ClientContext_->GetRequestAttachmentsStream();
    if (requestAttachmentsStream) {
        requestAttachmentsStream->AbortUnlessClosed(error);
    }

    const auto& responseAttachmentsStream = ClientContext_->GetResponseAttachmentsStream();
    if (responseAttachmentsStream) {
        responseAttachmentsStream->AbortUnlessClosed(error);
    }

    SetPromise(error);
}

void TClientResponse::TraceResponse()
{
    NTracing::TraceEvent(
        ClientContext_->GetTraceContext(),
        ClientContext_->GetService(),
        ClientContext_->GetMethod(),
        NTracing::ClientReceiveAnnotation);
}

const IInvokerPtr& TClientResponse::GetInvoker()
{
    return ClientContext_->GetHeavy()
        ? TDispatcher::Get()->GetHeavyInvoker()
        : TDispatcher::Get()->GetLightInvoker();
}

void TClientResponse::Deserialize(TSharedRefArray responseMessage)
{
    Y_ASSERT(responseMessage);
    Y_ASSERT(!ResponseMessage_);

    ResponseMessage_ = std::move(responseMessage);

    Y_ASSERT(ResponseMessage_.Size() >= 2);

    YCHECK(ParseResponseHeader(ResponseMessage_, &Header_));

    // COMPAT(kiselyovp): legacy RPC codecs
    std::optional<NCompression::ECodec> bodyCodecId;
    NCompression::ECodec attachmentCodecId;
    if (Header_.has_codec()) {
        bodyCodecId = attachmentCodecId = CheckedEnumCast<NCompression::ECodec>(Header_.codec());
    } else {
        bodyCodecId = std::nullopt;
        attachmentCodecId = NCompression::ECodec::None;
    }

    DeserializeBody(ResponseMessage_[1], bodyCodecId);

    auto* attachmentCodec = NCompression::GetCodec(attachmentCodecId);

    auto memoryZone = CheckedEnumCast<EMemoryZone>(Header_.memory_zone());

    Attachments_.clear();
    Attachments_.reserve(ResponseMessage_.Size() - 2);
    for (auto attachmentIt = ResponseMessage_.Begin() + 2;
        attachmentIt != ResponseMessage_.End();
        ++attachmentIt)
    {
        TSharedRef decompressedAttachment;
        {
            TMemoryZoneGuard guard(memoryZone);
            if (attachmentCodecId == NCompression::ECodec::None && memoryZone != EMemoryZone::Normal) {
                struct TCopiedAttachmentTag
                { };
                decompressedAttachment = TSharedMutableRef::MakeCopy<TCopiedAttachmentTag>(*attachmentIt);
            } else {
                decompressedAttachment = attachmentCodec->Decompress(*attachmentIt);
            }
        }
        Attachments_.push_back(std::move(decompressedAttachment));
    }
}

void TClientResponse::HandleAcknowledgement()
{
    // NB: Handle without switching to another invoker.
    auto expected = EState::Sent;
    State_.compare_exchange_strong(expected, EState::Ack);
}

void TClientResponse::HandleResponse(TSharedRefArray message)
{
    auto prevState = State_.exchange(EState::Done);
    Y_ASSERT(prevState == EState::Sent || prevState == EState::Ack);

    GetInvoker()->Invoke(
        BIND(&TClientResponse::DoHandleResponse, MakeStrong(this), Passed(std::move(message))));
}

void TClientResponse::DoHandleResponse(TSharedRefArray message)
{
    Deserialize(std::move(message));
    Finish(TError());
}

void TClientResponse::HandleStreamingPayload(const TStreamingPayload& payload)
{
    const auto& stream = ClientContext_->GetResponseAttachmentsStream();
    if (!stream) {
        YT_LOG_DEBUG("Received streaming attachments payload for request with disabled streaming; ignored (RequestId: %v)",
            ClientContext_->GetRequestId());
        return;
    }
    stream->EnqueuePayload(payload);
}

void TClientResponse::HandleStreamingFeedback(const TStreamingFeedback& feedback)
{
    const auto& stream = ClientContext_->GetRequestAttachmentsStream();
    if (!stream) {
        YT_LOG_DEBUG("Received streaming attachments feedback for request with disabled streaming; ignored (RequestId: %v)",
            ClientContext_->GetRequestId());
        return;
    }

    stream->HandleFeedback(feedback);
}

////////////////////////////////////////////////////////////////////////////////

TServiceDescriptor::TServiceDescriptor(const TString& serviceName)
    : ServiceName(serviceName)
{ }

TServiceDescriptor& TServiceDescriptor::SetProtocolVersion(int majorVersion)
{
    auto version = DefaultProtocolVersion;
    version.Major = majorVersion;
    ProtocolVersion = version;
    return *this;
}

TServiceDescriptor& TServiceDescriptor::SetProtocolVersion(TProtocolVersion version)
{
    ProtocolVersion = version;
    return *this;
}

TServiceDescriptor& TServiceDescriptor::SetNamespace(const TString& value)
{
    Namespace = value;
    return *this;
}

TString TServiceDescriptor::GetFullServiceName() const
{
    return Namespace ? Namespace + "." + ServiceName : ServiceName;
}

////////////////////////////////////////////////////////////////////////////////

TMethodDescriptor::TMethodDescriptor(const TString& methodName)
    : MethodName(methodName)
{ }

TMethodDescriptor& TMethodDescriptor::SetMultiplexingBand(EMultiplexingBand value)
{
    MultiplexingBand = value;
    return *this;
}

TMethodDescriptor& TMethodDescriptor::SetStreamingEnabled(bool value)
{
    StreamingEnabled = value;
    return *this;
}

////////////////////////////////////////////////////////////////////////////////

TProxyBase::TProxyBase(
    IChannelPtr channel,
    const TServiceDescriptor& descriptor)
    : Channel_(std::move(channel))
    , ServiceDescriptor_(descriptor)
{
    Y_ASSERT(Channel_);
}

////////////////////////////////////////////////////////////////////////////////

TGenericProxy::TGenericProxy(
    IChannelPtr channel,
    const TServiceDescriptor& descriptor)
    : TProxyBase(std::move(channel), descriptor)
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
