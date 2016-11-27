#pragma once

#include "public.h"
#include "channel.h"

#include <yt/core/actions/future.h>

#include <yt/core/bus/client.h>

#include <yt/core/compression/public.h>

#include <yt/core/misc/nullable.h>
#include <yt/core/misc/property.h>
#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/rpc/helpers.h>
#include <yt/core/rpc/rpc.pb.h>

#include <yt/core/tracing/trace_context.h>

#include <atomic>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

struct IClientRequest
    : public virtual TRefCounted
{
    virtual TSharedRefArray Serialize() = 0;

    virtual const NProto::TRequestHeader& Header() const = 0;
    virtual NProto::TRequestHeader& Header() = 0;

    virtual bool IsOneWay() const = 0;
    virtual bool IsHeavy() const = 0;

    virtual TRequestId GetRequestId() const = 0;
    virtual TRealmId GetRealmId() const = 0;
    virtual const Stroka& GetService() const = 0;
    virtual const Stroka& GetMethod() const = 0;

    virtual const Stroka& GetUser() const = 0;
    virtual void SetUser(const Stroka& user) = 0;

    virtual bool GetRetry() const = 0;
    virtual void SetRetry(bool value) = 0;
};

DEFINE_REFCOUNTED_TYPE(IClientRequest)

////////////////////////////////////////////////////////////////////////////////

class TClientContext
    : public TIntrinsicRefCounted
{
public:
    DEFINE_BYVAL_RO_PROPERTY(TRequestId, RequestId);
    DEFINE_BYVAL_RO_PROPERTY(NTracing::TTraceContext, TraceContext);
    DEFINE_BYVAL_RO_PROPERTY(Stroka, Service);
    DEFINE_BYVAL_RO_PROPERTY(Stroka, Method);
    DEFINE_BYVAL_RO_PROPERTY(bool, Heavy);

public:
    TClientContext(
        const TRequestId& requestId,
        const NTracing::TTraceContext& traceContext,
        const Stroka& service,
        const Stroka& method,
        bool heavy)
        : RequestId_(requestId)
        , TraceContext_(traceContext)
        , Service_(service)
        , Method_(method)
        , Heavy_(heavy)
    { }
};

DEFINE_REFCOUNTED_TYPE(TClientContext)

////////////////////////////////////////////////////////////////////////////////

class TClientRequest
    : public IClientRequest
{
public:
    DEFINE_BYREF_RW_PROPERTY(std::vector<TSharedRef>, Attachments);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TDuration>, Timeout);
    DEFINE_BYVAL_RW_PROPERTY(bool, RequestAck);
    DEFINE_BYVAL_RW_PROPERTY(bool, Heavy);
    DEFINE_BYVAL_RW_PROPERTY(NCompression::ECodec, Codec);

public:
    virtual TSharedRefArray Serialize() override;

    virtual NProto::TRequestHeader& Header() override;
    virtual const NProto::TRequestHeader& Header() const override;

    virtual bool IsOneWay() const override;

    virtual TRequestId GetRequestId() const override;
    virtual TRealmId GetRealmId() const override;
    virtual const Stroka& GetService() const override;
    virtual const Stroka& GetMethod() const override;

    virtual const Stroka& GetUser() const override;
    virtual void SetUser(const Stroka& user) override;

    virtual bool GetRetry() const override;
    virtual void SetRetry(bool value) override;

protected:
    const IChannelPtr Channel_;

    NProto::TRequestHeader Header_;
    TSharedRef SerializedBody_;
    bool FirstTimeSerialization_ = true;


    TClientRequest(
        IChannelPtr channel,
        const Stroka& service,
        const Stroka& method,
        bool oneWay,
        int protocolVersion);

    virtual bool IsHeavy() const override;

    virtual TSharedRef SerializeBody() = 0;

    TClientContextPtr CreateClientContext();

    IClientRequestControlPtr Send(IClientResponseHandlerPtr responseHandler);

private:
    void TraceRequest(const NTracing::TTraceContext& traceContext);

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequestMessage, class TResponse>
class TTypedClientRequest
    : public TClientRequest
    , public TRequestMessage
{
public:
    typedef TIntrusivePtr<TTypedClientRequest> TThisPtr;

    TTypedClientRequest(
        IChannelPtr channel,
        const Stroka& path,
        const Stroka& method,
        bool oneWay,
        int protocolVersion)
        : TClientRequest(
            std::move(channel),
            path,
            method,
            oneWay,
            protocolVersion)
    { }

    TFuture<typename TResponse::TResult> Invoke()
    {
        auto context = CreateClientContext();
        auto response = NYT::New<TResponse>(std::move(context));
        auto promise = response->GetPromise();
        auto requestControl = Send(std::move(response));
        if (requestControl) {
            promise.OnCanceled(BIND(&IClientRequestControl::Cancel, std::move(requestControl)));
        }
        return promise.ToFuture();
    }

private:
    virtual TSharedRef SerializeBody() override
    {
        return SerializeToProtoWithEnvelope(*this, Codec_, false);
    }
};

////////////////////////////////////////////////////////////////////////////////

//! Handles the outcome of a single RPC request.
struct IClientResponseHandler
    : public virtual TIntrinsicRefCounted
{
    //! Called when request delivery is acknowledged.
    virtual void HandleAcknowledgement() = 0;

    //! Called if the request is replied with #EErrorCode::OK.
    /*!
     *  \param message A message containing the response.
     */
    virtual void HandleResponse(TSharedRefArray message) = 0;

    //! Called if the request fails.
    /*!
     *  \param error An error that has occurred.
     */
    virtual void HandleError(const TError& error) = 0;
};

DEFINE_REFCOUNTED_TYPE(IClientResponseHandler)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EClientResponseState,
    (Sent)
    (Ack)
    (Done)
);

//! Provides a common base for both one-way and two-way responses.
class TClientResponseBase
    : public IClientResponseHandler
{
public:
    DEFINE_BYVAL_RO_PROPERTY(TInstant, StartTime);

protected:
    using EState = EClientResponseState;

    const TClientContextPtr ClientContext_;

    std::atomic<EState> State_ = {EState::Sent};


    explicit TClientResponseBase(TClientContextPtr clientContext);

    // IClientResponseHandler implementation.
    virtual void HandleError(const TError& error) override;

    void Finish(const TError& error);

    virtual void SetPromise(const TError& error) = 0;

private:
    void TraceResponse();
    void DoHandleError(const TError& error);

};

////////////////////////////////////////////////////////////////////////////////

//! Describes a two-way response.
class TClientResponse
    : public TClientResponseBase
{
public:
    DEFINE_BYREF_RW_PROPERTY(std::vector<TSharedRef>, Attachments);

public:
    TSharedRefArray GetResponseMessage() const;

protected:
    explicit TClientResponse(TClientContextPtr clientContext);

    virtual void DeserializeBody(const TRef& data) = 0;

private:
    TSharedRefArray ResponseMessage_;


    // IClientResponseHandler implementation.
    virtual void HandleAcknowledgement() override;
    virtual void HandleResponse(TSharedRefArray message) override;

    void DoHandleResponse(TSharedRefArray message);
    void Deserialize(TSharedRefArray responseMessage);
};

////////////////////////////////////////////////////////////////////////////////

template <class TResponseMessage>
class TTypedClientResponse
    : public TClientResponse
    , public TResponseMessage
{
public:
    typedef TIntrusivePtr<TTypedClientResponse> TResult;

    explicit TTypedClientResponse(TClientContextPtr clientContext)
        : TClientResponse(std::move(clientContext))
    { }

    TPromise<TResult> GetPromise()
    {
        return Promise_;
    }

private:
    TPromise<TResult> Promise_ = NewPromise<TResult>();


    virtual void SetPromise(const TError& error) override
    {
        if (error.IsOK()) {
            Promise_.Set(this);
        } else {
            Promise_.Set(error);
        }
        Promise_.Reset();
    }

    virtual void DeserializeBody(const TRef& data) override
    {
        DeserializeFromProtoWithEnvelope(this, data);
    }
};

////////////////////////////////////////////////////////////////////////////////

//! Describes a one-way response.
class TOneWayClientResponse
    : public TClientResponseBase
{
public:
    typedef void TResult;

    explicit TOneWayClientResponse(TClientContextPtr clientContext);

    TPromise<TResult> GetPromise();

private:
    TPromise<TResult> Promise_ = NewPromise<TResult>();


    // IClientResponseHandler implementation.
    virtual void HandleAcknowledgement() override;
    virtual void HandleResponse(TSharedRefArray message) override;

    virtual void SetPromise(const TError& error) override;

};

DEFINE_REFCOUNTED_TYPE(TOneWayClientResponse)

////////////////////////////////////////////////////////////////////////////////

static const int DefaultProtocolVersion = 0;
static const int GenericProtocolVersion = -1;

struct TServiceDescriptor
{
    Stroka ServiceName;
    int ProtocolVersion = DefaultProtocolVersion;

    explicit TServiceDescriptor(const Stroka& serviceName)
        : ServiceName(serviceName)
    { }

    TServiceDescriptor& SetProtocolVersion(int value)
    {
        ProtocolVersion = value;
        return *this;
    }
};

#define DEFINE_RPC_PROXY(type, descriptor) \
    static const ::NYT::NRpc::TServiceDescriptor& GetDescriptor() \
    { \
        static const ::NYT::NRpc::TServiceDescriptor result = (descriptor); \
        return result; \
    } \
    \
    explicit type(::NYT::NRpc::IChannelPtr channel) \
        : ::NYT::NRpc::TProxyBase(std::move(channel), GetDescriptor()) \
    { }

#define RPC_PROXY_DESC(name) \
    NYT::NRpc::TServiceDescriptor(#name)

#define DEFINE_RPC_PROXY_METHOD(ns, method) \
    typedef ::NYT::NRpc::TTypedClientResponse<ns::TRsp##method> TRsp##method; \
    typedef ::NYT::NRpc::TTypedClientRequest<ns::TReq##method, TRsp##method> TReq##method; \
    typedef ::NYT::TIntrusivePtr<TRsp##method> TRsp##method##Ptr; \
    typedef ::NYT::TIntrusivePtr<TReq##method> TReq##method##Ptr; \
    typedef ::NYT::TErrorOr<TRsp##method##Ptr> TErrorOrRsp##method##Ptr; \
    \
    TReq##method##Ptr method() \
    { \
        static Stroka MethodName(#method); \
        return CreateRequest<TReq##method>(MethodName, false); \
    }

////////////////////////////////////////////////////////////////////////////////

#define DEFINE_ONE_WAY_RPC_PROXY_METHOD(ns, method) \
    typedef ::NYT::NRpc::TTypedClientRequest<ns::TReq##method, ::NYT::NRpc::TOneWayClientResponse> TReq##method; \
    typedef ::NYT::TIntrusivePtr<TReq##method> TReq##method##Ptr; \
    \
    TReq##method##Ptr method() \
    { \
        static Stroka MethodName(#method); \
        return CreateRequest<TReq##method>(MethodName, true); \
    }

////////////////////////////////////////////////////////////////////////////////

class TProxyBase
{
public:
    DEFINE_RPC_PROXY_METHOD(NProto, Discover);

    DEFINE_BYVAL_RW_PROPERTY(TNullable<TDuration>, DefaultTimeout);
    DEFINE_BYVAL_RW_PROPERTY(bool, DefaultRequestAck);

protected:
    const IChannelPtr Channel_;
    const TServiceDescriptor Descriptor_;

    TProxyBase(
        IChannelPtr channel,
        const TServiceDescriptor& descriptor);

    template <class T>
    TIntrusivePtr<T> CreateRequest(const Stroka& methodName, bool oneWay)
    {
        auto request = New<T>(
            Channel_,
            Descriptor_.ServiceName,
            methodName,
            oneWay,
            Descriptor_.ProtocolVersion);
        request->SetTimeout(DefaultTimeout_);
        request->SetRequestAck(DefaultRequestAck_);
        return request;
    }
};

////////////////////////////////////////////////////////////////////////////////

class TGenericProxy
    : public TProxyBase
{
public:
    TGenericProxy(
        IChannelPtr channel,
        const TServiceDescriptor& descriptor);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
