#include "bus_server.h"
#include "private.h"
#include "server_detail.h"

#include <yt/core/bus/bus.h>
#include <yt/core/bus/server.h>

#include <yt/core/misc/protobuf_helpers.h>

#include <yt/core/rpc/message.h>
#include <yt/core/rpc/proto/rpc.pb.h>

namespace NYT {
namespace NRpc {

using namespace NConcurrency;
using namespace NBus;

////////////////////////////////////////////////////////////////////////////////

class TBusServer
    : public TServerBase
    , public IMessageHandler
{
public:
    explicit TBusServer(IBusServerPtr busServer)
        : TServerBase(RpcServerLogger)
        , BusServer_(std::move(busServer))
    { }

private:
    IBusServerPtr BusServer_;


    virtual void DoStart() override
    {
        BusServer_->Start(this);
        TServerBase::DoStart();
    }

    virtual TFuture<void> DoStop(bool graceful) override
    {
        return TServerBase::DoStop(graceful).Apply(BIND([=, this_ = MakeStrong(this)] (const TError& error) {
            // NB: Stop the bus server anyway.
            auto asyncResult = BusServer_->Stop();
            BusServer_.Reset();
            error.ThrowOnError();
            return asyncResult;
        }));
    }

    virtual void HandleMessage(TSharedRefArray message, IBusPtr replyBus) throw() override
    {
        auto messageType = GetMessageType(message);
        switch (messageType) {
            case EMessageType::Request:
                OnRequestMessage(std::move(message), std::move(replyBus));
                break;

            case EMessageType::RequestCancelation:
                OnRequestCancelationMessage(std::move(message), std::move(replyBus));
                break;

            default:
                // Unable to reply, no request id is known.
                // Let's just drop the message.
                LOG_ERROR("Incoming message has invalid type, ignored (Type: %x)",
                    static_cast<ui32>(messageType));
                break;
        }
    }

    void OnRequestMessage(TSharedRefArray message, IBusPtr replyBus)
    {
        auto header = std::make_unique<NProto::TRequestHeader>();
        if (!ParseRequestHeader(message, header.get())) {
            // Unable to reply, no request id is known.
            // Let's just drop the message.
            LOG_ERROR("Error parsing request header");
            return;
        }

        auto requestId = FromProto<TRequestId>(header->request_id());
        const auto& serviceName = header->service();
        const auto& methodName = header->method();
        auto realmId = FromProto<TRealmId>(header->realm_id());
        auto timeout = header->has_timeout() ? MakeNullable(FromProto<TDuration>(header->timeout())) : Null;
        auto startTime = header->has_start_time() ? MakeNullable(FromProto<TInstant>(header->start_time())) : Null;
        bool isRetry = header->retry();

        if (message.Size() < 2) {
            LOG_ERROR("Too few request parts: expected >= 2, actual %v (RequestId: %v)",
                message.Size(),
                requestId);
            return;
        }

        LOG_DEBUG("Request received (Method: %v:%v, RealmId: %v, RequestId: %v, User: %v, "
            "Timeout: %v, Endpoint: %v, StartTime: %v, Retry: %v)",
            serviceName,
            methodName,
            realmId,
            requestId,
            header->has_user() ? header->user() : RootUserName,
            timeout,
            replyBus->GetEndpointDescription(),
            startTime,
            isRetry);

        if (!Started_) {
            auto error = TError(NRpc::EErrorCode::Unavailable, "Server is not started");
            LOG_DEBUG(error);
            auto response = CreateErrorResponseMessage(requestId, error);
            replyBus->Send(response, TSendOptions(EDeliveryTrackingLevel::None));
            return;
        }

        TServiceId serviceId(serviceName, realmId);
        auto service = FindService(serviceId);
        if (!service) {
            auto error = TError(
                EErrorCode::NoSuchService,
                "Service is not registered")
                 << TErrorAttribute("service", serviceName)
                 << TErrorAttribute("realm_id", realmId);
            LOG_WARNING(error);
            auto response = CreateErrorResponseMessage(requestId, error);
            replyBus->Send(std::move(response), TSendOptions(EDeliveryTrackingLevel::None));
            return;
        }

        service->HandleRequest(
            std::move(header),
            std::move(message),
            std::move(replyBus));
    }

    void OnRequestCancelationMessage(TSharedRefArray message, IBusPtr /*replyBus*/)
    {
        NProto::TRequestCancelationHeader header;
        if (!ParseRequestCancelationHeader(message, &header)) {
            // Unable to reply, no request id is known.
            // Let's just drop the message.
            LOG_ERROR("Error parsing request cancelation header");
            return;
        }

        auto requestId = FromProto<TRequestId>(header.request_id());
        const auto& serviceName = header.service();
        const auto& methodName = header.method();
        auto realmId = FromProto<TRealmId>(header.realm_id());

        TServiceId serviceId(serviceName, realmId);
        auto service = FindService(serviceId);
        if (!service) {
            LOG_DEBUG("Service is not registered (Service: %v, RealmId: %v, RequestId: %v)",
                serviceName,
                realmId,
                requestId);
            return;
        }

        LOG_DEBUG("Request cancelation received (Method: %v:%v, RealmId: %v, RequestId: %v)",
            serviceName,
            methodName,
            realmId,
            requestId);

        service->HandleRequestCancelation(requestId);
    }
};

IServerPtr CreateBusServer(NBus::IBusServerPtr busServer)
{
    return New<TBusServer>(busServer);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
