#include "client.h"
#include "private.h"
#include "dispatcher.h"
#include "message.h"

#include <yt/core/net/local_address.h>

#include <yt/core/misc/checksum.h>

#include <iterator>

namespace NYT {
namespace NRpc {

using namespace NBus;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static const auto ClientHostAnnotation = TString("client_host");
static const auto RequestIdAnnotation = TString("request_id");

////////////////////////////////////////////////////////////////////////////////

TClientRequest::TClientRequest(
    IChannelPtr channel,
    const TString& service,
    const TString& method,
    int protocolVersion)
    : Channel_(std::move(channel))
{
    Y_ASSERT(Channel_);

    Header_.set_service(service);
    Header_.set_method(method);
    ToProto(Header_.mutable_request_id(), TRequestId::Create());
    Header_.set_protocol_version(protocolVersion);
}

TSharedRefArray TClientRequest::Serialize()
{
    if (!FirstTimeSerialization_) {
        Header_.set_retry(true);
    }
    FirstTimeSerialization_ = false;

    return CreateRequestMessage(
        Header_,
        GetSerializedBody(),
        Attachments_);
}

IClientRequestControlPtr TClientRequest::Send(IClientResponseHandlerPtr responseHandler)
{
    TSendOptions options;
    options.Timeout = Timeout_;
    options.RequestAck = RequestAck_;
    options.GenerateAttachmentChecksums = GenerateAttachmentChecksums_;
    options.MultiplexingBand = MultiplexingBand_;
    return Channel_->Send(
        this,
        std::move(responseHandler),
        options);
}

NProto::TRequestHeader& TClientRequest::Header()
{
    return Header_;
}

const NProto::TRequestHeader& TClientRequest::Header() const
{
    return Header_;
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

void TClientRequest::SetMutationId(const TMutationId& id)
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
        HashCombine(hash, GetChecksum(GetSerializedBody()));
        for (const auto& attachment : Attachments_) {
            HashCombine(hash, GetChecksum(attachment));
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

    return New<TClientContext>(
        GetRequestId(),
        traceContext,
        GetService(),
        GetMethod(),
        Heavy_);
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

const TSharedRef& TClientRequest::GetSerializedBody() const
{
    if (!SerializedBody_) {
        SerializedBody_ = SerializeBody();
    }
    return SerializedBody_;
}

////////////////////////////////////////////////////////////////////////////////

TClientResponseBase::TClientResponseBase(TClientContextPtr clientContext)
    : StartTime_(NProfiling::GetInstant())
    , ClientContext_(std::move(clientContext))
{ }

void TClientResponseBase::HandleError(const TError& error)
{
    auto prevState = State_.exchange(EState::Done);
    if (prevState == EState::Done) {
        // Ignore the error.
        // Most probably this is a late timeout.
        return;
    }

    GetInvoker()->Invoke(
        BIND(&TClientResponseBase::DoHandleError, MakeStrong(this), error));
}

void TClientResponseBase::DoHandleError(const TError& error)
{
    Finish(error);
}

void TClientResponseBase::Finish(const TError& error)
{
    NTracing::TTraceContextGuard guard(ClientContext_->GetTraceContext());
    TraceResponse();
    SetPromise(error);
}

void TClientResponseBase::TraceResponse()
{
    NTracing::TraceEvent(
        ClientContext_->GetTraceContext(),
        ClientContext_->GetService(),
        ClientContext_->GetMethod(),
        NTracing::ClientReceiveAnnotation);
}

const IInvokerPtr& TClientResponseBase::GetInvoker()
{
    return ClientContext_->GetHeavy()
        ? TDispatcher::Get()->GetHeavyInvoker()
        : TDispatcher::Get()->GetLightInvoker();
}

////////////////////////////////////////////////////////////////////////////////

TClientResponse::TClientResponse(TClientContextPtr clientContext)
    : TClientResponseBase(std::move(clientContext))
{ }

TSharedRefArray TClientResponse::GetResponseMessage() const
{
    Y_ASSERT(ResponseMessage_);
    return ResponseMessage_;
}

void TClientResponse::Deserialize(TSharedRefArray responseMessage)
{
    Y_ASSERT(responseMessage);
    Y_ASSERT(!ResponseMessage_);

    ResponseMessage_ = std::move(responseMessage);

    Y_ASSERT(ResponseMessage_.Size() >= 2);

    DeserializeBody(ResponseMessage_[1]);

    Attachments_.clear();
    Attachments_.insert(
        Attachments_.begin(),
        ResponseMessage_.Begin() + 2,
        ResponseMessage_.End());
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

////////////////////////////////////////////////////////////////////////////////

TProxyBase::TProxyBase(
    IChannelPtr channel,
    const TServiceDescriptor& descriptor)
    : DefaultRequestAck_(true)
    , Channel_(std::move(channel))
    , Descriptor_(descriptor)
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

} // namespace NRpc
} // namespace NYT
