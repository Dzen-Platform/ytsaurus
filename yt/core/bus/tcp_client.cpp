#include "stdafx.h"
#include "tcp_client.h"
#include "private.h"
#include "client.h"
#include "bus.h"
#include "config.h"
#include "tcp_connection.h"

#include <core/misc/error.h>
#include <core/misc/address.h>

#include <core/concurrency/thread_affinity.h>

#include <core/rpc/public.h>

#include <core/ytree/convert.h>

#include <errno.h>

#ifndef _win_
    #include <netinet/tcp.h>
    #include <sys/socket.h>
#endif

namespace NYT {
namespace NBus {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = BusLogger;

////////////////////////////////////////////////////////////////////////////////

//! A lightweight proxy controlling the lifetime of client #TTcpConnection.
/*!
 *  When the last strong reference vanishes, it calls IBus::Terminate
 *  for the underlying connection.
 */
class TTcpClientBusProxy
    : public IBus
{
public:
    TTcpClientBusProxy(
        TTcpBusClientConfigPtr config,
        IMessageHandlerPtr handler)
        : Config_(std::move(config))
        , Handler_(std::move(handler))
        , DispatcherThread_(TTcpDispatcher::TImpl::Get()->GetClientThread())
        , Id_(TConnectionId::Create())
    {
        VERIFY_THREAD_AFFINITY_ANY();
        YCHECK(Config_);
        YCHECK(Handler_);
    }

    ~TTcpClientBusProxy()
    {
        VERIFY_THREAD_AFFINITY_ANY();
        if (Connection_) {
            Connection_->Terminate(TError(NRpc::EErrorCode::TransportError, "Bus terminated"));
        }
    }

    void Open()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto interfaceType = Config_->UnixDomainName ? ETcpInterfaceType::Remote : GetInterfaceType(Config_->Address.Get());

        LOG_DEBUG("Connecting to %v (ConnectionId: %v, InterfaceType: %v)",
            Config_->Address,
            Id_,
            interfaceType);

        Connection_ = New<TTcpConnection>(
            Config_,
            DispatcherThread_,
            EConnectionType::Client,
            interfaceType,
            Id_,
            INVALID_SOCKET,
            Config_->UnixDomainName.HasValue() ? Config_->UnixDomainName.Get() : Config_->Address.Get(),
            Config_->UnixDomainName.HasValue(),
            Config_->Priority,
            Handler_);
        DispatcherThread_->AsyncRegister(Connection_);
    }

    virtual Stroka GetEndpointTextDescription() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        return Connection_->GetEndpointTextDescription();
    }

    virtual TYsonString GetEndpointYsonDescription() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        return Connection_->GetEndpointYsonDescription();
    }

    virtual TFuture<void> Send(TSharedRefArray message, EDeliveryTrackingLevel level) override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        return Connection_->Send(std::move(message), level);
    }

    virtual void Terminate(const TError& error) override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        Connection_->Terminate(error);
    }

    virtual void SubscribeTerminated(const TCallback<void(const TError&)>& callback) override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        Connection_->SubscribeTerminated(callback);
    }

    virtual void UnsubscribeTerminated(const TCallback<void(const TError&)>& callback) override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        Connection_->UnsubscribeTerminated(callback);
    }

private:
    TTcpBusClientConfigPtr Config_;
    IMessageHandlerPtr Handler_;
    TTcpDispatcherThreadPtr DispatcherThread_;
    TConnectionId Id_;

    TTcpConnectionPtr Connection_;

    static ETcpInterfaceType GetInterfaceType(const Stroka& address)
    {
        return
            IsLocalServiceAddress(address)
            ? ETcpInterfaceType::Local
            : ETcpInterfaceType::Remote;
    }

};

////////////////////////////////////////////////////////////////////////////////

class TTcpBusClient
    : public IBusClient
{
public:
    explicit TTcpBusClient(TTcpBusClientConfigPtr config)
        : Config_(config)
    { }

    virtual Stroka GetEndpointTextDescription() const override
    {
        return Config_->Address
            ? *Config_->Address
            : "unix://" + *Config_->UnixDomainName;
    }

    virtual TYsonString GetEndpointYsonDescription() const override
    {
        return ConvertToYsonString(GetEndpointTextDescription());
    }

    virtual IBusPtr CreateBus(IMessageHandlerPtr handler) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto proxy = New<TTcpClientBusProxy>(
            Config_,
            std::move(handler));
        proxy->Open();
        return proxy;
    }

private:
    TTcpBusClientConfigPtr Config_;

};

IBusClientPtr CreateTcpBusClient(TTcpBusClientConfigPtr config)
{
    return New<TTcpBusClient>(config);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
