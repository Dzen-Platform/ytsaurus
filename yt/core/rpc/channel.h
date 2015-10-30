#pragma once

#include "public.h"

#include <core/misc/nullable.h>
#include <core/misc/property.h>
#include <core/misc/error.h>

#include <core/actions/future.h>

#include <core/bus/client.h>

#include <core/ytree/public.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

//! Controls the lifetime of a request sent via IChannel::Sent.
struct IClientRequestControl
    : public virtual TIntrinsicRefCounted
{
    //! Cancels the request.
    /*!
     *  An implementation is free to ignore cancelations.
     */
    virtual void Cancel() = 0;
};

DEFINE_REFCOUNTED_TYPE(IClientRequestControl)

////////////////////////////////////////////////////////////////////////////////

//! An interface for exchanging request-response pairs.
/*!
 * \note Thread affinity: any.
 */
struct IChannel
    : public virtual TRefCounted
{
    //! Gets the default timeout.
    virtual TNullable<TDuration> GetDefaultTimeout() const = 0;

    //! Sets the default timeout.
    virtual void SetDefaultTimeout(const TNullable<TDuration>& timeout) = 0;

    //! Returns a textual representation of the channel's endpoint.
    //! Typically used for logging.
    virtual Stroka GetEndpointTextDescription() const = 0;

    //! Returns a YSON representation of the channel's endpoint.
    //! Typically used for constructing error attributes.
    virtual NYTree::TYsonString GetEndpointYsonDescription() const = 0;

    //! Sends a request via the channel.
    /*!
     *  \param request A request to send.
     *  \param responseHandler An object that will handle a response.
     *  \param timeout Request processing timeout.
     *  \returns an object controlling the lifetime of the request;
     *  the latter could be |nullptr| if no control is supported by the implementation in general
     *  or for this particular request.
     */
    virtual IClientRequestControlPtr Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        TNullable<TDuration> timeout,
        bool requestAck) = 0;

    //! Shuts down the channel.
    /*!
     *  It is safe to call this method multiple times.
     *  After the first call the instance is no longer usable.
     */
    virtual TFuture<void> Terminate(const TError& error) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChannel)

////////////////////////////////////////////////////////////////////////////////

//! Provides means for parsing addresses and creating channels.
struct IChannelFactory
    : public virtual TRefCounted
{
    virtual IChannelPtr CreateChannel(const Stroka& address) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChannelFactory)

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
