#pragma once

#include "public.h"

#include <core/misc/ref.h>

#include <core/actions/signal.h>

#include <core/bus/public.h>

#include <core/rpc/rpc.pb.h>

#include <core/logging/log.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

//! Represents an RPC request at server-side.
/*!
 *  \note
 *  Implementations are not thread-safe.
 */
struct IServiceContext
    : public virtual TIntrinsicRefCounted
{
    //! Returns the message that contains the request being handled.
    virtual TSharedRefArray GetRequestMessage() const = 0;

    //! Returns the id of the request.
    /*!
     *  These ids are assigned by the client to distinguish between responses.
     *  The server should not rely on their uniqueness.
     *  
     *  #NullRequestId is a possible value.
     */
    virtual TRequestId GetRequestId() const = 0;

    //! Returns the instant when the request was first issued by the client, if known.
    virtual TNullable<TInstant> GetRequestStartTime() const = 0;

    //! Returns the instant when the current retry of request was issued by the client, if known.
    virtual TNullable<TInstant> GetRetryStartTime() const = 0;

    //! Returns the client-specified request timeout, if any.
    virtual TNullable<TDuration> GetTimeout() const = 0;

    //! Returns |true| if this is a duplicate copy of a previously sent (and possibly served) request.
    virtual bool IsRetry() const = 0;

    //! Returns request priority for reordering purposes.
    virtual i64 GetPriority() const = 0;

    //! Returns request service name.
    virtual const Stroka& GetService() const = 0;

    //! Returns request method name.
    virtual const Stroka& GetMethod() const = 0;

    //! Returns request realm id.
    virtual const TRealmId& GetRealmId() const = 0;

    //! Returns the name of the user issuing the request.
    virtual const Stroka& GetUser() const = 0;

    //! Returns |true| if the request if one-way, i.e. replying to it is not possible.
    virtual bool IsOneWay() const = 0;

    //! Returns |true| if the request was already replied.
    virtual bool IsReplied() const = 0;

    //! Signals that the request processing is complete and sends reply to the client.
    virtual void Reply(const TError& error) = 0;

    //! Parses the message and forwards to the client.
    virtual void Reply(TSharedRefArray message) = 0;

    //! Raised when request processing is canceled.
    DECLARE_INTERFACE_SIGNAL(void(), Canceled);

    //! Cancels request processing.
    /*!
     *  Implementations are free to ignore this call.
     */
    virtual void Cancel() = 0;

    //! Returns a future representing the response message.
    /*!
     *  \note
     *  Can only be called before the request handling is started.
     */
    virtual TFuture<TSharedRefArray> GetAsyncResponseMessage() const = 0;

    //! Returns the serialized response message.
    /*!
     *  \note
     *  Can only be called after the context is replied.
     */
    virtual TSharedRefArray GetResponseMessage() const = 0;

    //! Returns the error that was previously set by #Reply.
    /*!
     *  Can only be called after the context is replied.
     */
    virtual const TError& GetError() const = 0;

    //! Returns the request body.
    virtual TSharedRef GetRequestBody() const = 0;

    //! Returns the response body.
    virtual TSharedRef GetResponseBody() = 0;

    //! Sets the response body.
    virtual void SetResponseBody(const TSharedRef& responseBody) = 0;

    //! Returns a vector of request attachments.
    virtual std::vector<TSharedRef>& RequestAttachments() = 0;

    //! Returns a vector of response attachments.
    virtual std::vector<TSharedRef>& ResponseAttachments() = 0;

    //! Returns immutable request header.
    virtual const NProto::TRequestHeader& RequestHeader() const = 0;

    //! Returns mutable request header.
    virtual NProto::TRequestHeader& RequestHeader() = 0;

    //! Sets and immediately logs the request logging info.
    virtual void SetRawRequestInfo(const Stroka& info) = 0;

    //! Sets the response logging info. This info will be logged when the context is replied.
    virtual void SetRawResponseInfo(const Stroka& info) = 0;

    //! Returns the logger for request/response messages.
    virtual const NLogging::TLogger& GetLogger() const = 0;

    //! Returns the logging level for request/response messages.
    virtual NLogging::ELogLevel GetLogLevel() const = 0;


    // Extension methods.

    void SetRequestInfo();
    void SetResponseInfo();

    template <class... TArgs>
    void SetRequestInfo(const char* format, const TArgs&... args);

    template <class... TArgs>
    void SetResponseInfo(const char* format, const TArgs&... args);

    //! Replies with a given message when the latter is set.
    void ReplyFrom(TFuture<TSharedRefArray> asyncMessage);

    //! Replies with a given error when the latter is set.
    void ReplyFrom(TFuture<void> asyncError);

};

DEFINE_REFCOUNTED_TYPE(IServiceContext)

////////////////////////////////////////////////////////////////////////////////

struct TServiceId
{
    TServiceId();
    TServiceId(const Stroka& serviceName, const TRealmId& realmId = NullRealmId);
    TServiceId(const char* serviceName, const TRealmId& realmId = NullRealmId);

    Stroka ServiceName;
    TRealmId RealmId;

};

bool operator == (const TServiceId& lhs, const TServiceId& rhs);
bool operator != (const TServiceId& lhs, const TServiceId& rhs);

Stroka ToString(const TServiceId& serviceId);

////////////////////////////////////////////////////////////////////////////////

//! Represents an abstract service registered within IServer.
/*!
 *  \note
 *  Implementations must be fully thread-safe.
 */
struct IService
    : public virtual TRefCounted
{
    //! Applies a new configuration.
    virtual void Configure(NYTree::INodePtr config) = 0;

    //! Returns the service id.
    virtual TServiceId GetServiceId() const = 0;

    //! Handles incoming request.
    virtual void HandleRequest(
        std::unique_ptr<NProto::TRequestHeader> header,
        TSharedRefArray message,
        NBus::IBusPtr replyBus) = 0;

    //! Handles request cancelation.
    virtual void HandleRequestCancelation(
        const TRequestId& requestId) = 0;
};

DEFINE_REFCOUNTED_TYPE(IService)

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT

//! A hasher for TServiceId.
template <>
struct hash<NYT::NRpc::TServiceId>
{
    inline size_t operator()(const NYT::NRpc::TServiceId& id) const
    {
        return
            THash<Stroka>()(id.ServiceName) * 497 +
            THash<NYT::NRpc::TRealmId>()(id.RealmId);
    }
};

////////////////////////////////////////////////////////////////////////////////

#define SERVICE_INL_H_
#include "service-inl.h"
#undef SERVICE_INL_H_

